#include "handler_core.h"

#include <string.h>

namespace parley {
namespace detail {

using ArduinoJson::JsonDocument;
using ArduinoJson::JsonVariantConst;

HandlerCore::HandlerCore()
    : adapter_(nullptr), host_(nullptr), slots_(nullptr), slotCount_(0), maxPayload_(0),
      scratch_(nullptr), parseArenaBuf_(nullptr), parseArenaCap_(0), buildArenaBuf_(nullptr),
      buildArenaCap_(0), now_(0), nextSeq_(0), executingCount_(0), dispatching_(false) {}

void HandlerCore::init(ParleyConnectionAdapter* adapter, const ParleyTopics& topics,
                       const ParleyConfig& config, HandlerHost* host, HandlerSlot* slots,
                       int slotCount, uint8_t* resCaches, size_t maxPayload, uint8_t* scratch,
                       char (*tombIds)[kIdCapacity], uint32_t* tombDue, int tombCap,
                       uint8_t* parseArena, size_t parseArenaCap, uint8_t* buildArena,
                       size_t buildArenaCap) {
  adapter_ = adapter;
  topics_ = topics;
  config_ = config;
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
  nextSeq_ = 0;
  executingCount_ = 0;
  dispatching_ = false;
  for (int i = 0; i < slotCount_; ++i) {
    slots_[i].inUse = false;
    slots_[i].resBuf = resCaches + static_cast<size_t>(i) * maxPayload;
    slots_[i].resCap = static_cast<uint16_t>(maxPayload);
  }
  tombstones_.init(tombIds, tombDue, tombCap);
  subscribeAll();
}

void HandlerCore::subscribeAll() { adapter_->subscribe(topics_.request); }

void HandlerCore::connected() { subscribeAll(); }

void HandlerCore::deliver(const char* topic, const uint8_t* payload, size_t len) {
  if (len > maxPayload_) return;  // F2: inbound oversize → silently dropped (§4)
  if (topic == nullptr || strcmp(topic, topics_.request) != 0) return;

  FixedArena parena(parseArenaBuf_, parseArenaCap_);
  JsonDocument doc(&parena);
  wire::Parsed p;
  if (!wire::parse(doc, parena, payload, len, p)) return;
  if (p.v != 1) return;
  if (!wire::isValidId(p.id)) return;

  switch (p.t) {
    case wire::MsgType::Req:
      onRequest(p.id, p.p, p.hasP);
      break;
    case wire::MsgType::Rak:
      onResultAck(p.id);
      break;
    case wire::MsgType::Cnl:
      onCancel(p.id);
      break;
    default:
      // Unknown type, or a handler→commander type on the request topic — ignore (§4).
      break;
  }
}

int HandlerCore::findCommand(const char* id) const {
  for (int i = 0; i < slotCount_; ++i) {
    if (slots_[i].inUse && strcmp(slots_[i].id, id) == 0) return i;
  }
  return -1;
}

int HandlerCore::reserveSlot() {
  for (int i = 0; i < slotCount_; ++i) {
    if (!slots_[i].inUse) {
      // MS1/HC1: bump the admission generation on every reserve, mirroring the commander's per-reserve
      // ++gen. The stamp handed to the execution at dispatch goes stale the instant this slot is reused.
      ++slots_[i].gen;
      return i;
    }
  }
  return -1;
}

bool HandlerCore::execIsLive(int slot, uint32_t gen) const {
  if (slot < 0 || slot >= slotCount_) return false;
  const HandlerSlot& s = slots_[slot];
  return s.inUse && s.gen == gen && s.state == kExecuting;
}

void HandlerCore::onRequest(const char* id, JsonVariantConst p, bool hasP) {
  // H1: dedup FIRST — the capacity check applies only to unknown ids.
  int existing = findCommand(id);
  if (existing >= 0) {
    HandlerSlot& s = slots_[existing];
    if (s.state == kQueued || s.state == kExecuting) {
      publishHeartbeat(existing);  // H1: immediate hb resynchronizes the commander cheaply
    } else if (s.state == kResultCached) {
      republishCachedResult(existing);  // H1: re-publish the cached res
    }
    return;
  }

  // H1: a duplicate of a tombstoned id is ignored.
  if (tombstones_.contains(id)) return;

  // H2/H8: unknown id — admit if a slot is free, otherwise stateless $busy.
  if (commandCount() >= slotCount_) {
    publishStateless(id, builtin_codes::kBusy);
    return;
  }

  int slot = reserveSlot();
  HandlerSlot& s = slots_[slot];
  s.inUse = true;
  s.state = kQueued;
  s.cancelRequested = false;
  s.resLen = 0;
  s.seq = nextSeq_++;
  s.heartbeat.disarm();
  s.resultRetry.disarm();
  s.resultRetryTimeout.disarm();
  copyIdInto(s.id, kIdCapacity, id);

  if (!host_->decodeRequest(slot, p, hasP)) {
    // §4 robustness: a req whose payload does not decode is silently ignored — no admission.
    s.inUse = false;
    return;
  }

  // H2: the first heartbeat is the accept-ack, published immediately.
  publishHeartbeat(slot);
  tryDispatch();
}

void HandlerCore::onResultAck(const char* id) {
  // H6: a rak for an id not in RESULT-CACHED is ignored.
  int slot = findCommand(id);
  if (slot >= 0 && slots_[slot].state == kResultCached) moveToTombstone(slot);
}

void HandlerCore::onCancel(const char* id) {
  int slot = findCommand(id);
  if (slot >= 0) {
    HandlerSlot& s = slots_[slot];
    switch (s.state) {
      case kExecuting:
        // H7: trigger the cancellation signal; the outcome is whatever the handler produces.
        s.cancelRequested = true;
        break;
      case kQueued:
        // H7: dequeue and produce a cached, retried $cancelled result. Internal worker — this slot is
        // known-live and has no outstanding execution (never dispatched), so no gen guard applies.
        finishCancelled(slot);
        break;
      case kResultCached:
        // H7: cancel lost the race — re-publish the cached result.
        republishCachedResult(slot);
        break;
      default:
        break;
    }
    return;
  }
  // H7: unknown or tombstoned id — stateless $cancelled.
  publishStateless(id, builtin_codes::kCancelled);
}

void HandlerCore::tryDispatch() {
  if (dispatching_) return;
  dispatching_ = true;
  // H4: dequeue FIFO into at most maxConcurrentExecutions slots.
  while (executingCount_ < config_.maxConcurrentExecutions) {
    int slot = pickQueued();
    if (slot < 0) break;
    slots_[slot].state = kExecuting;
    ++executingCount_;
    host_->dispatch(slot, slots_[slot].gen);
  }
  dispatching_ = false;
}

int HandlerCore::pickQueued() const {
  int best = -1;
  uint32_t bestSeq = 0;
  for (int i = 0; i < slotCount_; ++i) {
    const HandlerSlot& s = slots_[i];
    if (s.inUse && s.state == kQueued) {
      if (best < 0 || timeBefore(s.seq, bestSeq)) {
        best = i;
        bestSeq = s.seq;
      }
    }
  }
  return best;
}

void HandlerCore::completeSuccess(int slot, uint32_t gen, wire::PayloadWriter writer, void* ctx) {
  if (!execIsLive(slot, gen)) return;  // MS1/HC1: stale/duplicate call → no-op (see header)
  HandlerSlot& s = slots_[slot];
  FixedArena barena(buildArenaBuf_, buildArenaCap_);
  size_t len = wire::buildSuccessResult(s.resBuf, s.resCap, barena, s.id, writer, ctx);
  if (len == 0) {
    abortSlot(slot);  // outcome-encode failure ≡ handler fault → abort (plan §3)
    return;
  }
  completeWithResult(slot, len, s.state == kExecuting);
}

void HandlerCore::completeCustomError(int slot, uint32_t gen, const char* code, const char* m) {
  if (!execIsLive(slot, gen)) return;  // MS1/HC1
  HandlerSlot& s = slots_[slot];
  FixedArena barena(buildArenaBuf_, buildArenaCap_);
  size_t len = wire::buildCustomErrorResult(s.resBuf, s.resCap, barena, s.id, code, m);
  if (len == 0) {
    abortSlot(slot);
    return;
  }
  completeWithResult(slot, len, s.state == kExecuting);
}

void HandlerCore::completeCancelled(int slot, uint32_t gen) {
  if (!execIsLive(slot, gen)) return;  // MS1/HC1 — the H7 EXECUTING arm (exec.cancel())
  finishCancelled(slot);
}

void HandlerCore::finishCancelled(int slot) {
  HandlerSlot& s = slots_[slot];
  FixedArena barena(buildArenaBuf_, buildArenaCap_);
  size_t len = wire::buildBuiltinErrorResult(s.resBuf, s.resCap, barena, s.id, builtin_codes::kCancelled);
  if (len == 0) {
    abortSlot(slot);
    return;
  }
  completeWithResult(slot, len, s.state == kExecuting);
}

void HandlerCore::completeWithResult(int slot, size_t resLen, bool wasExecuting) {
  HandlerSlot& s = slots_[slot];
  // H3: heartbeats run until the command's first res is published.
  s.heartbeat.disarm();
  s.resLen = static_cast<uint16_t>(resLen);
  s.state = kResultCached;

  // H5: retry the result until rak or resultRetryTimeout (from this first publication).
  // CR1 (publish comes last): schedule BOTH result timers BEFORE the res publish, so a synchronous rak
  // delivered from inside publish finds real timers to dispose.
  s.resultRetry.arm(now_, config_.resultRetryInterval);
  s.resultRetryTimeout.arm(now_, config_.resultRetryTimeout);
  publish(topics_.result, s.resBuf, s.resLen);

  if (wasExecuting) {
    --executingCount_;
    tryDispatch();
  }
}

void HandlerCore::abort(int slot, uint32_t gen) {
  if (!execIsLive(slot, gen)) return;  // MS1/HC1 — the ParleyExecution::fail() reject paths
  abortSlot(slot);
}

void HandlerCore::abortSlot(int slot) {
  HandlerSlot& s = slots_[slot];
  s.heartbeat.disarm();
  bool wasExecuting = (s.state == kExecuting);
  releaseSlot(slot);
  tombstones_.add(s.id, now_ + config_.tombstoneTtl);
  if (wasExecuting) {
    --executingCount_;
    tryDispatch();
  }
}

void HandlerCore::moveToTombstone(int slot) {
  HandlerSlot& s = slots_[slot];
  s.resultRetry.disarm();
  s.resultRetryTimeout.disarm();
  s.heartbeat.disarm();
  releaseSlot(slot);  // H8: the admission slot is freed at the tombstone transition
  tombstones_.add(s.id, now_ + config_.tombstoneTtl);
}

void HandlerCore::publishHeartbeat(int slot) {
  HandlerSlot& s = slots_[slot];
  // H3: one cadence per id, restarted by every hb publication. (Re)schedule BEFORE publishing.
  s.heartbeat.arm(now_, config_.heartbeatInterval);
  FixedArena barena(buildArenaBuf_, buildArenaCap_);
  size_t len = wire::buildHeartbeat(scratch_, maxPayload_, barena, s.id);
  if (len > 0) publish(topics_.heartbeat, scratch_, len);
}

void HandlerCore::republishCachedResult(int slot) {
  HandlerSlot& s = slots_[slot];
  publish(topics_.result, s.resBuf, s.resLen);
}

void HandlerCore::publishStateless(const char* id, const char* code) {
  FixedArena barena(buildArenaBuf_, buildArenaCap_);
  size_t len = wire::buildBuiltinErrorResult(scratch_, maxPayload_, barena, id, code);
  if (len > 0) publish(topics_.result, scratch_, len);
}

void HandlerCore::releaseSlot(int slot) {
  HandlerSlot& s = slots_[slot];
  s.heartbeat.disarm();
  s.resultRetry.disarm();
  s.resultRetryTimeout.disarm();
  // MS1/HC1: clear the EXECUTING marker as the slot is freed. Without this a freed slot keeps
  // state==kExecuting, so a duplicate exec op reading state (before the gen guard existed) would
  // re-decrement executingCount_ into the negatives. The gen guard is the primary defense; this is
  // the belt-and-suspenders that keeps a freed slot from ever reading as a live execution.
  s.state = kQueued;
  s.inUse = false;
}

void HandlerCore::emitProgress(int slot, uint32_t gen, wire::PayloadWriter writer, void* ctx) {
  if (!execIsLive(slot, gen)) return;  // MS1/HC1: stale progress → dropped (never for another command)
  HandlerSlot& s = slots_[slot];
  FixedArena barena(buildArenaBuf_, buildArenaCap_);
  size_t len = wire::buildProgress(scratch_, maxPayload_, barena, s.id, writer, ctx);
  if (len > 0) publish(topics_.progress, scratch_, len);
  // An unencodable/oversize progress payload is dropped (C8: advisory) — never aborts the command.
}

bool HandlerCore::isCancelRequested(int slot, uint32_t gen) const {
  if (!execIsLive(slot, gen)) return false;  // MS1/HC1: a stale poll never reads a reused slot's flag
  return slots_[slot].cancelRequested;
}

const char* HandlerCore::slotId(int slot, uint32_t gen) const {
  if (!execIsLive(slot, gen)) return "";  // MS1/HC1: a stale handle never borrows a reused slot's id
  return slots_[slot].id;
}

void HandlerCore::publish(const char* topic, const uint8_t* bytes, size_t len) {
  // ARENA invariant: the handler only ever publishes on topics it never parses (heartbeat / progress /
  // result), and only ever parses the one topic it never publishes (request). A synchronous-loopback
  // adapter may re-enter deliver() from inside this call and reset the parse arena — but because that
  // arena is disjoint from any borrowed pointer live on this outbound path, the re-entrancy can never
  // invalidate state the caller is still holding. Do not add a publish on the request topic here.
  adapter_->publish(topic, bytes, len);
}

void HandlerCore::tick(uint32_t now) {
  now_ = now;
  while (fireEarliestDue(now)) {
  }
  tombstones_.expire(now);
}

bool HandlerCore::fireEarliestDue(uint32_t now) {
  int bestSlot = -1;
  int bestKind = -1;
  uint32_t bestDue = 0;
  for (int i = 0; i < slotCount_; ++i) {
    HandlerSlot& s = slots_[i];
    if (!s.inUse) continue;
    const Timer* timers[3] = {&s.heartbeat, &s.resultRetry, &s.resultRetryTimeout};
    for (int k = 0; k < 3; ++k) {
      if (timers[k]->dueAt(now) && (bestSlot < 0 || timeBefore(timers[k]->due, bestDue))) {
        bestSlot = i;
        bestKind = k;
        bestDue = timers[k]->due;
      }
    }
  }
  if (bestSlot < 0) return false;

  switch (bestKind) {
    case 0:  // H3 heartbeat cadence
      publishHeartbeat(bestSlot);
      break;
    case 1:  // H5 result retry
      slots_[bestSlot].resultRetry.arm(now, config_.resultRetryInterval);
      republishCachedResult(bestSlot);
      break;
    case 2:  // H5 result-retry give-up
      moveToTombstone(bestSlot);
      break;
    default:
      break;
  }
  return true;
}

void HandlerCore::disposeAll() {
  adapter_->unsubscribe(topics_.request);
  for (int i = 0; i < slotCount_; ++i) {
    if (slots_[i].inUse) {
      slots_[i].cancelRequested = true;
      releaseSlot(i);
    }
  }
  executingCount_ = 0;
  tombstones_.clear();
}

int HandlerCore::commandCount() const {
  int n = 0;
  for (int i = 0; i < slotCount_; ++i) {
    if (slots_[i].inUse) ++n;
  }
  return n;
}

int HandlerCore::queueCount() const {
  int n = 0;
  for (int i = 0; i < slotCount_; ++i) {
    if (slots_[i].inUse && slots_[i].state == kQueued) ++n;
  }
  return n;
}

int HandlerCore::activeTimerCount() const {
  int n = 0;
  for (int i = 0; i < slotCount_; ++i) {
    const HandlerSlot& s = slots_[i];
    if (!s.inUse) continue;
    if (s.heartbeat.active) ++n;
    if (s.resultRetry.active) ++n;
    if (s.resultRetryTimeout.active) ++n;
  }
  n += tombstones_.count();
  return n;
}

}  // namespace detail
}  // namespace parley
