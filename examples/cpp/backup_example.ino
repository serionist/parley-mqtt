// Parley example (C++ / ESP32-Arduino) — a `backup` command over MQTT.
// ===========================================================================
//
// Two roles, one command type:
//
//   • the HANDLER runs on the device that owns the storage (say, an ESP32 with
//     an SD card). It receives a backup request, streams progress while it
//     works, and returns how many bytes it wrote — or a typed error (the disk
//     filled up, the target was unreachable).
//
//   • the COMMANDER runs wherever you start a backup. It calls the command,
//     watches progress, can cancel, and gets one typed answer back.
//
// A microcontroller is normally ONE role, so pick it here and flash that build:
#define PARLEY_ROLE_HANDLER 1 // set to 0 to build the commander role instead
//
// Parley on C++ is statically allocated, uses no exceptions/RTTI, and is
// TICK-DRIVEN: you pump it from loop() with tick(millis()) and feed it inbound
// bytes with deliver(). It never opens the connection — you hand it a live one
// through a small adapter (below).
//
//   request   { "target": string }        — what to back up
//   progress  { "percent": number }       — advisory, streamed while working
//   result    { "bytesWritten": number }  — the single terminal success value
//   errors    "diskFull" | "targetUnreachable"  — your own typed error codes

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

#include <string.h>

#include "Parley.h" // add the Parley cpp/src tree to your include path

using namespace parley;

// ---------------------------------------------------------------------------
// 1. The message shapes for this command, plus their JSON codecs.
// ---------------------------------------------------------------------------
// On a constrained target you own the memory, so payloads are fixed-size
// structs. A ParleyCodec<T> specialization says how each maps to/from JSON;
// Parley binds them with the four template params <Req, Prog, Res, Err>.

struct BackupRequest {
  char target[64];
  BackupRequest() { target[0] = '\0'; }
};

struct BackupProgress {
  int percent;
  BackupProgress() : percent(0) {}
};

struct BackupResult {
  long bytesWritten;
  BackupResult() : bytesWritten(0) {}
};

// A custom error code is just a string on the wire; this struct carries it.
// (A custom code must not start with '$' — that prefix is Parley's builtins.)
struct BackupError {
  char code[24];
  BackupError() { code[0] = '\0'; }
};

static void copyStr(char* dst, size_t cap, const char* src) {
  size_t i = 0;
  if (src != nullptr) {
    for (; src[i] != '\0' && i + 1 < cap; ++i) dst[i] = src[i];
  }
  dst[i] = '\0';
}

// Named error values.
static BackupError backupError(const char* code) {
  BackupError e;
  copyStr(e.code, sizeof(e.code), code);
  return e;
}

namespace parley {

template <>
struct ParleyCodec<BackupRequest> {
  static bool encode(const BackupRequest& v, ArduinoJson::JsonVariant dst) {
    dst.to<ArduinoJson::JsonObject>()["target"] = v.target;
    return true;
  }
  static bool decode(ArduinoJson::JsonVariantConst src, BackupRequest& out) {
    if (!src.is<ArduinoJson::JsonObjectConst>()) return false;
    const char* t = src["target"].as<const char*>();
    if (t == nullptr) return false;
    copyStr(out.target, sizeof(out.target), t);
    return true;
  }
};

template <>
struct ParleyCodec<BackupProgress> {
  static bool encode(const BackupProgress& v, ArduinoJson::JsonVariant dst) {
    dst.to<ArduinoJson::JsonObject>()["percent"] = v.percent;
    return true;
  }
  static bool decode(ArduinoJson::JsonVariantConst src, BackupProgress& out) {
    if (!src.is<ArduinoJson::JsonObjectConst>() || src["percent"].isNull()) return false;
    out.percent = src["percent"].as<int>();
    return true;
  }
};

template <>
struct ParleyCodec<BackupResult> {
  static bool encode(const BackupResult& v, ArduinoJson::JsonVariant dst) {
    dst.to<ArduinoJson::JsonObject>()["bytesWritten"] = v.bytesWritten;
    return true;
  }
  static bool decode(ArduinoJson::JsonVariantConst src, BackupResult& out) {
    if (!src.is<ArduinoJson::JsonObjectConst>() || src["bytesWritten"].isNull()) return false;
    out.bytesWritten = src["bytesWritten"].as<long>();
    return true;
  }
};

template <>
struct ParleyCodec<BackupError> {
  static bool encode(const BackupError& v, ArduinoJson::JsonVariant dst) { return dst.set(v.code); }
  static bool decode(ArduinoJson::JsonVariantConst src, BackupError& out) {
    const char* s = src.as<const char*>();
    if (s == nullptr) return false;
    copyStr(out.code, sizeof(out.code), s);
    return true;
  }
};

}  // namespace parley

// The four topic strings — literals in flash. Parley imposes no naming scheme;
// your broker's ACLs apply to them directly. Both roles must agree on the set.
static const ParleyTopics kBackupTopics("home/backup/req",  // commander → handler
                                        "home/backup/hb",   // handler → commander: heartbeats
                                        "home/backup/prg",  // handler → commander: progress
                                        "home/backup/res"); // handler → commander: the result

// ===========================================================================
// 2. An ILLUSTRATIVE transport adapter over PubSubClient.
// ===========================================================================
// ⚠️  This adapter is hand-written for the example. It has NOT been shipped as a
//     library or run through CI — it shows the shape you would implement over
//     your own client. A verified, reusable adapter arrives with the e2e suite.
//
// The C++ adapter is outbound-only: it publishes/subscribes, and the SKETCH
// feeds inbound bytes back in via deliver() from the PubSubClient callback
// (wired in setup() below). Publishes use an explicit length — payloads are
// UTF-8 JSON, not C strings.
class PubSubAdapter : public ParleyConnectionAdapter {
 public:
  explicit PubSubAdapter(PubSubClient& client) : client_(client) {}

  void publish(const char* topic, const uint8_t* payload, size_t length) override {
    client_.publish(topic, payload, static_cast<unsigned int>(length), /*retained=*/false);
  }

  void subscribe(const char* topic) override { client_.subscribe(topic); }

  void unsubscribe(const char* topic) override { client_.unsubscribe(topic); }

 private:
  PubSubClient& client_;
};

// ===========================================================================
// 3. The HANDLER — the side that performs the backup.
// ===========================================================================
// handleCommand runs on the dispatch path and must return quickly (heavy work
// would delay other commands' heartbeats). So we STASH the execution and do the
// real copying incrementally from loop(), in pump(). The execution stays valid
// for the command's lifetime, and a stale-slot guard makes late calls no-ops.
typedef ParleyCommandHandler<BackupRequest, BackupProgress, BackupResult, BackupError, /*MaxCommands=*/2>
    BackupHandlerBase;

class BackupHandler : public BackupHandlerBase {
 public:
  typedef ParleyExecution<BackupRequest, BackupProgress, BackupResult, BackupError> Exec;

  BackupHandler(ParleyConnectionAdapter& adapter, const ParleyTopics& topics,
                const ParleyConfig& config = ParleyConfig())
      : BackupHandlerBase(adapter, topics, config) {}

  // Advance any in-progress backup by one chunk. Call this from loop().
  void pump() {
    if (exec_ == nullptr) return;

    // Cooperative cancellation: a wire-level `cnl` sets this flag. Complete as
    // cancelled and let go of the execution.
    if (exec_->isCancelRequested()) {
      exec_->cancel(); // → the caller sees $cancelled
      exec_ = nullptr;
      return;
    }

    long wrote = copyOneChunk(exec_->request().target, chunk_);
    if (wrote < 0) {
      exec_->fail(backupError("diskFull")); // a typed domain failure
      exec_ = nullptr;
      return;
    }
    bytesWritten_ += wrote;

    // Advisory progress. Each call publishes one `prg`; delivery is best-effort
    // (never acknowledged). Meanwhile Parley heartbeats this command for you, so
    // the caller can always tell "slow" from "dead".
    BackupProgress p;
    p.percent = chunk_ * 10;
    exec_->reportProgress(p);

    if (++chunk_ > 10) {
      BackupResult result;
      result.bytesWritten = bytesWritten_;
      exec_->succeed(result); // the single terminal success value
      exec_ = nullptr;
    }
  }

 protected:
  // MaxConcurrentExecutions defaults to 1, so at most one backup runs at a time;
  // a single stashed pointer is enough for this example.
  void handleCommand(Exec& execution) override {
    exec_ = &execution;
    chunk_ = 1;
    bytesWritten_ = 0;
  }

 private:
  // Stand-in for real I/O (e.g. reading the SD card). Returns bytes written for
  // this chunk, or -1 to simulate a full disk.
  static long copyOneChunk(const char* target, int chunk) {
    (void)target;
    return 1024L * chunk; // pretend each chunk is a little bigger
  }

  Exec* exec_ = nullptr;
  int chunk_ = 1;
  long bytesWritten_ = 0;
};

// ===========================================================================
// 4. The COMMANDER — the side that requests a backup.
// ===========================================================================
typedef ParleyCommander<BackupRequest, BackupProgress, BackupResult, BackupError, /*MaxInFlight=*/2>
    BackupCommander;

// One outstanding call's state, handed to the callbacks through void* userData.
struct BackupCall {
  bool done;
  ParleyResult<BackupResult, BackupError> result;
  BackupCall() : done(false) {}
};

// Progress callback — a plain function pointer + userData (no std::function).
static void onBackupProgress(void* userData, const BackupProgress& progress) {
  (void)userData;
  Serial.printf("backup %d%% done\n", progress.percent);
}

// Completion callback — fires exactly once with the single terminal outcome.
static void onBackupComplete(void* userData, const ParleyResult<BackupResult, BackupError>& result) {
  BackupCall* call = static_cast<BackupCall*>(userData);
  call->result = result;
  call->done = true;

  // Every outcome — success, your errors, and all six builtins — is a value.
  if (result.isSuccess()) {
    Serial.printf("backup complete: %ld bytes written\n", result.value().bytesWritten);
    return;
  }

  const ParleyError<BackupError>& error = result.error();
  if (error.isBuiltin()) {
    // A builtin: something about delivery/liveness, not your domain.
    switch (error.builtin()) {
      case ParleyBuiltinError::NoAck:
        Serial.println("no handler answered — is the device online?");
        break;
      case ParleyBuiltinError::Busy:
        Serial.println("the device is at capacity — retry shortly");
        break;
      case ParleyBuiltinError::ExecutionStale:
        Serial.println("the device went silent mid-backup");
        break;
      case ParleyBuiltinError::ExecutionTimeout:
        Serial.println("the backup ran past its time cap");
        break;
      case ParleyBuiltinError::Cancelled:
        Serial.println("the backup was cancelled");
        break;
      case ParleyBuiltinError::DecodeError:
        Serial.println("the device sent a result we could not read");
        break;
      case ParleyBuiltinError::None:
        break;
    }
  } else {
    // A custom code — decoded into the typed BackupError.
    if (strcmp(error.custom().code, "diskFull") == 0) {
      Serial.printf("out of space%s%s\n", error.hasMessage() ? ": " : "",
                    error.hasMessage() ? error.message() : "");
    } else if (strcmp(error.custom().code, "targetUnreachable") == 0) {
      Serial.println("the backup target could not be reached");
    }
  }
}

// ===========================================================================
// 5. Wiring it up.
// ===========================================================================
static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);
static PubSubAdapter adapter(mqtt);

#if PARLEY_ROLE_HANDLER
static BackupHandler handler(adapter, kBackupTopics);
#else
static BackupCommander commander(adapter, kBackupTopics);
static BackupCall call;                     // the single outstanding call
static ParleyCallHandle callHandle;         // its handle — for cancel()
static bool started = false;
#endif

// PubSubClient hands every inbound message to ONE global callback. We forward it
// straight into Parley's deliver() (which routes by topic + id internally).
static void onMqttMessage(char* topic, uint8_t* payload, unsigned int length) {
#if PARLEY_ROLE_HANDLER
  handler.deliver(topic, payload, length);
#else
  commander.deliver(topic, payload, length);
#endif
}

static void ensureConnected() {
  if (mqtt.connected()) return;
  // YOU own the connection: server, credentials, and TLS all live here, never
  // in Parley. (WiFi is assumed already joined.)
  mqtt.setServer("192.0.2.10", 1883);
  mqtt.setBufferSize(512); // headroom over Parley's MaxPayload (default 256)
  mqtt.setCallback(onMqttMessage);
  if (mqtt.connect("parley-backup-device" /*, user, pass */)) {
    // On every (re)connect, tell Parley — it re-subscribes its topics, which is
    // how a dropped connection self-heals.
#if PARLEY_ROLE_HANDLER
    handler.connected();
#else
    commander.connected();
#endif
  }
}

void setup() {
  Serial.begin(115200);
  // ... join WiFi here ...
  ensureConnected();
#if PARLEY_ROLE_HANDLER
  Serial.println("backup handler ready");
#endif
}

void loop() {
  ensureConnected();
  mqtt.loop(); // drains inbound MQTT → onMqttMessage → deliver()

  uint32_t now = millis();

#if PARLEY_ROLE_HANDLER
  handler.tick(now); // drives heartbeats, result retries, timeouts
  handler.pump();    // advances an in-progress backup one chunk
#else
  commander.tick(now); // drives request retries, sign-of-life + timeout timers

  // Start one backup, once.
  if (!started && mqtt.connected()) {
    started = true;
    BackupRequest req;
    copyStr(req.target, sizeof(req.target), "photos");

    ParleyCallOptions options;
    options.executionTimeout = 5UL * 60 * 1000; // cap at 5 minutes once alive

    callHandle = commander.execute(req, options, onBackupProgress, onBackupComplete, &call);
    if (!callHandle.valid()) {
      // Synchronous local failure: the in-flight pool was full or the request
      // payload exceeded MaxPayload. Never a `$` wire code.
      Serial.println("could not start backup (pool full or payload too large)");
    }
    // To cancel later: commander.cancel(callHandle);  // sends a real `cnl`
  }
#endif
}

// Teardown is automatic: when a commander/handler goes out of scope its
// destructor unsubscribes, cancels every timer, and frees all state. (Globals
// like these live for the program's lifetime; a shorter-lived instance is torn
// down at the closing brace of its scope.)
