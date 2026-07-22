#include "commander_core.h"

#include <string.h>

namespace parley {
namespace detail {

using ArduinoJson::JsonDocument;
using ArduinoJson::JsonVariantConst;

CommanderCore::CommanderCore()
    : adapter_(nullptr), idgen_(nullptr), host_(nullptr), slots_(nullptr), slotCount_(0),
      maxPayload_(0), scratch_(nullptr), parseArenaBuf_(nullptr), parseArenaCap_(0),
      buildArenaBuf_(nullptr), buildArenaCap_(0), now_(0) {}

void CommanderCore::init(ParleyConnectionAdapter* adapter, const ParleyTopics& topics,
                         const ParleyConfig& config, ParleyIdGenerator* idgen, CommanderHost* host,
                         CommanderSlot* slots, int slotCount, uint8_t* reqCaches, size_t maxPayload,
                         uint8_t* scratch, char (*tombIds)[kIdCapacity], uint32_t* tombDue,
                         int tombCap, uint8_t* parseArena, size_t parseArenaCap, uint8_t* buildArena,
                         size_t buildArenaCap) {
  adapter_ = adapter;
  topics_ = topics;
  config_ = config;
  idgen_ = idgen;
  host_ = host;
  slots_ = slots;
  slotCount_ = slotCount;
  maxPayload_ = maxPayload;
  scratch_ = scratch;
  parseArenaBuf_ = parseArena;
  parseArenaCap_ = parseArenaCap;
  buildArenaBuf_ = buildArena;
  buildArenaCap_ = buildArenaCap;
  now_ = 0;
  for (int i = 0; i < slotCount_; ++i) {
    slots_[i].inUse = false;
    slots_[i].gen = 0;
    slots_[i].reqBuf = reqCaches + static_cast<size_t>(i) * maxPayload;
    slots_[i].reqCap = static_cast<uint16_t>(maxPayload);
  }
  tombstones_.init(tombIds, tombDue, tombCap);
  subscribeAll();
}

void CommanderCore::subscribeAll() {
  // Commander subscribes to the three handler→commander topics (protocol.md §3).
  adapter_->subscribe(topics_.heartbeat);
  adapter_->subscribe(topics_.progress);
  adapter_->subscribe(topics_.result);
}

void CommanderCore::connected() { subscribeAll(); }

int CommanderCore::reserveCall(const ParleyCallOptions& opts) {
  int slot = -1;
  for (int i = 0; i < slotCount_; ++i) {
    if (!slots_[i].inUse) {
      slot = i;
      break;
    }
  }
  if (slot < 0) return -1;  // F7: pool exhausted → synchronous local failure

  CommanderSlot& s = slots_[slot];
  idgen_->next(s.id, kIdCapacity);
  if (!wire::isValidId(s.id)) return -1;

  s.inUse = true;
  ++s.gen;
  s.state = kSending;
  s.cancelling = false;
  s.reqLen = 0;
  s.requestRetry.disarm();
  s.signOfLife.disarm();
  s.executionCap.disarm();
  s.cancelRetry.disarm();

  s.requestRetryInterval =
      opts.requestRetryInterval ? opts.requestRetryInterval : config_.requestRetryInterval;
  s.noAckTimeout = opts.noAckTimeout ? opts.noAckTimeout : config_.noAckTimeout;
  s.staleTimeout = opts.staleTimeout ? opts.staleTimeout : config_.staleTimeout;
  s.cancelRetryInterval =
      opts.cancelRetryInterval ? opts.cancelRetryInterval : config_.cancelRetryInterval;
  s.tombstoneTtl = opts.tombstoneTtl ? opts.tombstoneTtl : config_.tombstoneTtl;
  // executionTimeout: 0 = inherit config (which may itself be kNoTimeout); else the per-call value
  // (possibly kNoTimeout to disable). Never fed as start + kNoTimeout arithmetic (plan F8).
  s.executionTimeout = opts.executionTimeout ? opts.executionTimeout : config_.executionTimeout;
  return slot;
}

ParleyCallHandle CommanderCore::sendCall(int slot, wire::PayloadWriter writer, void* writerCtx) {
  CommanderSlot& s = slots_[slot];
  FixedArena barena(buildArenaBuf_, buildArenaCap_);
  size_t len = wire::buildRequest(s.reqBuf, s.reqCap, barena, s.id, writer, writerCtx);
  if (len == 0) {
    // Oversize / encode failure → synchronous local failure (plan §3): release the slot, no publish.
    freeSlot(slot);
    return ParleyCallHandle::invalid();
  }
  s.reqLen = static_cast<uint16_t>(len);

  // CR1 (publish comes last): schedule the request-retry (C1) and sign-of-life (C3) timers BEFORE the
  // initial publish, so a synchronous loopback reply finds real timers to cancel/reschedule.
  s.requestRetry.arm(now_, s.requestRetryInterval);
  s.signOfLife.arm(now_, s.noAckTimeout);
  publish(topics_.request, s.reqBuf, s.reqLen);
  return ParleyCallHandle(slot, s.gen);
}

void CommanderCore::cancel(ParleyCallHandle handle) {
  if (!handle.valid() || handle.slot >= slotCount_) return;
  CommanderSlot& s = slots_[handle.slot];
  if (!s.inUse || s.gen != handle.gen) return;
  requestCancel(handle.slot);
}

void CommanderCore::requestCancel(int slot) {
  CommanderSlot& s = slots_[slot];
  if (s.cancelling) return;
  s.cancelling = true;

  // C7: a command the caller no longer wants must not keep being offered for admission.
  if (s.state == kSending) s.requestRetry.disarm();

  // CR1: schedule the cancel-retry timer BEFORE publishing cnl (a synchronous $cancelled reply can
  // tear the command down from inside publish; the timer must exist to be disarmed).
  s.cancelRetry.arm(now_, s.cancelRetryInterval);
  publishCnl(s.id);
}

void CommanderCore::deliver(const char* topic, const uint8_t* payload, size_t len) {
  if (len > maxPayload_) return;  // F2: inbound oversize is unparseable → silently dropped (§4)

  wire::MsgType expected;
  if (topic == nullptr) return;
  if (strcmp(topic, topics_.heartbeat) == 0) {
    expected = wire::MsgType::Hb;
  } else if (strcmp(topic, topics_.progress) == 0) {
    expected = wire::MsgType::Prg;
  } else if (strcmp(topic, topics_.result) == 0) {
    expected = wire::MsgType::Res;
  } else {
    return;  // not a commander inbound topic
  }

  FixedArena parena(parseArenaBuf_, parseArenaCap_);
  JsonDocument doc(&parena);
  wire::Parsed p;
  if (!wire::parse(doc, parena, payload, len, p)) return;
  if (p.v != 1) return;                    // §4: unrecognized version
  if (p.t != expected) return;             // §4: unknown type / type on wrong topic
  if (!wire::isValidId(p.id)) return;      // §4.2

  switch (expected) {
    case wire::MsgType::Hb:
      onHeartbeat(p.id);
      break;
    case wire::MsgType::Prg:
      onProgressMsg(p.id, p.p, p.hasP);
      break;
    case wire::MsgType::Res:
      onResult(p);
      break;
    default:
      break;
  }
}

int CommanderCore::findInflight(const char* id) const {
  for (int i = 0; i < slotCount_; ++i) {
    if (slots_[i].inUse && strcmp(slots_[i].id, id) == 0) return i;
  }
  return -1;
}

void CommanderCore::onHeartbeat(const char* id) {
  int slot = findInflight(id);
  if (slot >= 0) onSignOfLife(slot);
}

void CommanderCore::onProgressMsg(const char* id, JsonVariantConst p, bool hasP) {
  int slot = findInflight(id);
  if (slot < 0) return;
  onSignOfLife(slot);
  host_->onProgress(slot, p, hasP);  // C8: advisory; shell drops undecodable payloads silently
}

void CommanderCore::onResult(const wire::Parsed& p) {
  if (!p.hasOk) return;                       // §4: ok is required on res
  bool ok = p.ok;
  if (!ok && p.e == nullptr) return;          // §4: e required when ok:false
  bool isBusy = !ok && p.e != nullptr && strcmp(p.e, builtin_codes::kBusy) == 0;

  int slot = findInflight(p.id);
  if (slot >= 0) {
    CommanderSlot& s = slots_[slot];
    if (s.state == kLive && isBusy) return;   // C4 exception: ignore a stale $busy in LIVE
    if (ok) {
      terminateSuccess(slot, p.p, p.hasP);
    } else {
      terminateError(slot, p.e, p.m, isBusy);
    }
    return;
  }

  // C5: a res for a tombstoned id re-emits rak (unless $busy) and discards the content.
  if (tombstones_.contains(p.id)) {
    if (!isBusy) publishRak(p.id);
    return;
  }
  // C6: neither in-flight nor tombstoned — belongs to another commander instance. Ignore.
}

void CommanderCore::onSignOfLife(int slot) {
  CommanderSlot& s = slots_[slot];
  if (s.state == kSending) {
    // C2: first sign of life — enter LIVE, stop req retries, start the execution cap.
    s.state = kLive;
    s.requestRetry.disarm();
    if (s.executionTimeout != kNoTimeout) {
      s.executionCap.arm(now_, s.executionTimeout);  // F8: explicit skip when disabled
    }
  }
  // C2/C3: the sliding sign-of-life timer restarts on every sign of life.
  s.signOfLife.arm(now_, s.staleTimeout);
}

void CommanderCore::terminateSuccess(int slot, JsonVariantConst p, bool hasP) {
  CommanderSlot& s = slots_[slot];
  char id[kIdCapacity];
  copyIdInto(id, kIdCapacity, s.id);
  uint32_t due = now_ + s.tombstoneTtl;

  // CR1 (publish comes last): finalize state (free slot + tombstone) BEFORE publishing rak, so a
  // synchronous loopback that bounces the rak back as another res finds the id tombstoned (re-rak +
  // discard) instead of re-entering an in-flight slot and completing the call twice.
  freeSlot(slot);
  tombstones_.add(id, due);
  publishRak(id);  // C4: the result was delivered → rak

  // C4: decode after acking. A payload that fails to decode is $decodeError (rak already sent).
  bool decoded = host_->onSuccess(slot, p, hasP);
  if (!decoded) host_->onError(slot, builtin_codes::kDecodeError, nullptr);
}

void CommanderCore::terminateError(int slot, const char* code, const char* m, bool isBusy) {
  CommanderSlot& s = slots_[slot];
  char id[kIdCapacity];
  copyIdInto(id, kIdCapacity, s.id);
  uint32_t due = now_ + s.tombstoneTtl;

  // CR1 (publish comes last): finalize state before the rak publish (see terminateSuccess).
  freeSlot(slot);
  tombstones_.add(id, due);
  // C4: rak every terminal error res except $busy (the handler holds no state for a refused id).
  if (!isBusy) publishRak(id);
  host_->onError(slot, code, m);  // code/m point into the live parse doc; the shell copies them
}

void CommanderCore::terminateBuiltin(int slot, const char* wireCode) {
  CommanderSlot& s = slots_[slot];
  char id[kIdCapacity];
  copyIdInto(id, kIdCapacity, s.id);
  uint32_t due = now_ + s.tombstoneTtl;
  freeSlot(slot);
  tombstones_.add(id, due);
  host_->onError(slot, wireCode, nullptr);  // commander-local terminal — no rak
}

void CommanderCore::freeSlot(int slot) {
  CommanderSlot& s = slots_[slot];
  s.requestRetry.disarm();
  s.signOfLife.disarm();
  s.executionCap.disarm();
  s.cancelRetry.disarm();
  s.inUse = false;
}

void CommanderCore::publishRak(const char* id) {
  FixedArena barena(buildArenaBuf_, buildArenaCap_);
  size_t len = wire::buildResultAck(scratch_, maxPayload_, barena, id);
  if (len > 0) publish(topics_.request, scratch_, len);
}

void CommanderCore::publishCnl(const char* id) {
  FixedArena barena(buildArenaBuf_, buildArenaCap_);
  size_t len = wire::buildCancel(scratch_, maxPayload_, barena, id);
  if (len > 0) publish(topics_.request, scratch_, len);
}

void CommanderCore::publish(const char* topic, const uint8_t* bytes, size_t len) {
  adapter_->publish(topic, bytes, len);
}

void CommanderCore::tick(uint32_t now) {
  now_ = now;
  while (fireEarliestDue(now)) {
  }
  tombstones_.expire(now);
}

bool CommanderCore::fireEarliestDue(uint32_t now) {
  int bestSlot = -1;
  int bestKind = -1;
  uint32_t bestDue = 0;

  for (int i = 0; i < slotCount_; ++i) {
    CommanderSlot& s = slots_[i];
    if (!s.inUse) continue;
    const Timer* timers[4] = {&s.requestRetry, &s.signOfLife, &s.executionCap, &s.cancelRetry};
    for (int k = 0; k < 4; ++k) {
      if (timers[k]->dueAt(now) && (bestSlot < 0 || timeBefore(timers[k]->due, bestDue))) {
        bestSlot = i;
        bestKind = k;
        bestDue = timers[k]->due;
      }
    }
  }
  if (bestSlot < 0) return false;

  CommanderSlot& s = slots_[bestSlot];
  switch (bestKind) {
    case 0:  // C1 request retry (armed only while SENDING, not cancelling)
      s.requestRetry.arm(now, s.requestRetryInterval);
      publish(topics_.request, s.reqBuf, s.reqLen);
      break;
    case 1:  // C3 sign-of-life expiry
      terminateBuiltin(bestSlot, s.state == kSending ? builtin_codes::kNoAck
                                                     : builtin_codes::kExecutionStale);
      break;
    case 2:  // C3 execution cap expiry
      terminateBuiltin(bestSlot, builtin_codes::kExecutionTimeout);
      break;
    case 3:  // C7 cancel retry
      s.cancelRetry.arm(now, s.cancelRetryInterval);
      publishCnl(s.id);
      break;
    default:
      break;
  }
  return true;
}

void CommanderCore::disposeAll() {
  adapter_->unsubscribe(topics_.heartbeat);
  adapter_->unsubscribe(topics_.progress);
  adapter_->unsubscribe(topics_.result);
  for (int i = 0; i < slotCount_; ++i) {
    if (slots_[i].inUse) {
      freeSlot(i);
      host_->onError(i, builtin_codes::kCancelled, nullptr);
    }
  }
  tombstones_.clear();
}

int CommanderCore::inflightCount() const {
  int n = 0;
  for (int i = 0; i < slotCount_; ++i) {
    if (slots_[i].inUse) ++n;
  }
  return n;
}

int CommanderCore::activeTimerCount() const {
  int n = 0;
  for (int i = 0; i < slotCount_; ++i) {
    const CommanderSlot& s = slots_[i];
    if (!s.inUse) continue;
    if (s.requestRetry.active) ++n;
    if (s.signOfLife.active) ++n;
    if (s.executionCap.active) ++n;
    if (s.cancelRetry.active) ++n;
  }
  n += tombstones_.count();  // each tombstone carries a TTL deadline
  return n;
}

}  // namespace detail
}  // namespace parley
