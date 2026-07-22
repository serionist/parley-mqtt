#ifndef PARLEY_EXECUTION_H
#define PARLEY_EXECUTION_H

#include <stddef.h>
#include <string.h>

#include "ParleyCodec.h"
#include "detail/fixed_arena.h"
#include "detail/handler_core.h"
#include "detail/json.h"
#include "detail/limits.h"

namespace parley {

// The handler-side execution context handed to the consumer's handleCommand (design.md C++ sketch).
// Thin typed forwarder over the non-templated HandlerCore: request() reads the decoded request; the
// completion/progress calls route back into the core. An instance is owned by the handler shell per
// slot and stays valid for the command's lifetime, so a long-running handler may stash `&exec` and
// keep working across tick()/loop() turns.
template <class Req, class Prog, class Res, class Err>
class ParleyExecution {
 public:
  ParleyExecution() : core_(nullptr), slot_(-1), gen_(0), request_(nullptr) {}

  // Bound by the handler shell at dispatch time. `gen` is the slot's admission generation; it is
  // forwarded into every core op so a call made through a stale execution (its slot has since been
  // tombstoned and reused) no-ops instead of acting on the new occupant (MS1/HC1).
  void bind(detail::HandlerCore* core, int slot, uint32_t gen, const Req* request) {
    core_ = core;
    slot_ = slot;
    gen_ = gen;
    request_ = request;
  }

  const Req& request() const { return *request_; }

  // Emit one advisory progress update (protocol.md C8). An unencodable/oversize payload is dropped.
  void reportProgress(const Prog& progress) {
    core_->emitProgress(slot_, gen_, &progressWriter, const_cast<Prog*>(&progress));
  }

  // Complete successfully (H4/H5). An unencodable/oversize result aborts the command (handler fault).
  void succeed(const Res& result) {
    core_->completeSuccess(slot_, gen_, &resultWriter, const_cast<Res*>(&result));
  }

  // Complete with a consumer (custom) error code. A code that maps to a non-string, to a $-reserved
  // string (§5), or to a string longer than the wire code buffer (kCodeCapacity-1), is a handler fault
  // → the command aborts (never a masqueraded or silently-truncated wire code — AB2).
  void fail(const Err& error) {
    uint8_t arenaBuf[128];
    detail::FixedArena arena(arenaBuf, sizeof(arenaBuf));
    ArduinoJson::JsonDocument doc(&arena);
    ArduinoJson::JsonVariant v = doc.to<ArduinoJson::JsonVariant>();
    if (!ParleyCodec<Err>::encode(error, v) || !v.is<const char*>()) {
      core_->abort(slot_, gen_);
      return;
    }
    const char* code = v.as<const char*>();
    if (code == nullptr || code[0] == '$') {  // $-prefix reserved for the protocol
      core_->abort(slot_, gen_);
      return;
    }
    if (strlen(code) >= kCodeCapacity) {  // AB2: too long to carry without truncation → fail-fast
      core_->abort(slot_, gen_);
      return;
    }
    core_->completeCustomError(slot_, gen_, code, nullptr);
  }

  // Complete as cancelled (H7) — emits an error res with e:"$cancelled". This is the builtin-cancel
  // path; it never routes through fail(Err).
  void cancel() { core_->completeCancelled(slot_, gen_); }

  // Poll the cancellation flag (H7 EXECUTING arm). A cnl for this id sets it; the handler decides what
  // outcome to produce.
  bool isCancelRequested() const { return core_->isCancelRequested(slot_, gen_); }

  const char* commandId() const { return core_->slotId(slot_, gen_); }

 private:
  static bool progressWriter(void* ctx, ArduinoJson::JsonVariant dst) {
    return ParleyCodec<Prog>::encode(*static_cast<const Prog*>(ctx), dst);
  }
  static bool resultWriter(void* ctx, ArduinoJson::JsonVariant dst) {
    return ParleyCodec<Res>::encode(*static_cast<const Res*>(ctx), dst);
  }

  detail::HandlerCore* core_;
  int slot_;
  uint32_t gen_;
  const Req* request_;
};

}  // namespace parley

#endif  // PARLEY_EXECUTION_H
