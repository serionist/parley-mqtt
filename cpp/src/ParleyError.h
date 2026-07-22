#ifndef PARLEY_ERROR_H
#define PARLEY_ERROR_H

#include <string.h>

#include "detail/limits.h"

namespace parley {

// The six builtin error codes (protocol.md §5). None is the sentinel for "not a builtin" (a custom
// consumer code).
enum class ParleyBuiltinError {
  None = 0,
  NoAck,             // $noAck        — commander-local
  ExecutionStale,    // $executionStale — commander-local
  ExecutionTimeout,  // $executionTimeout — commander-local
  Cancelled,         // $cancelled    — wire (res)
  Busy,              // $busy         — wire (res)
  DecodeError,       // $decodeError  — commander-local
};

// Wire strings for the builtin codes.
namespace builtin_codes {
static const char* const kNoAck = "$noAck";
static const char* const kExecutionStale = "$executionStale";
static const char* const kExecutionTimeout = "$executionTimeout";
static const char* const kCancelled = "$cancelled";
static const char* const kBusy = "$busy";
static const char* const kDecodeError = "$decodeError";
}  // namespace builtin_codes

inline const char* builtinToWire(ParleyBuiltinError b) {
  switch (b) {
    case ParleyBuiltinError::NoAck: return builtin_codes::kNoAck;
    case ParleyBuiltinError::ExecutionStale: return builtin_codes::kExecutionStale;
    case ParleyBuiltinError::ExecutionTimeout: return builtin_codes::kExecutionTimeout;
    case ParleyBuiltinError::Cancelled: return builtin_codes::kCancelled;
    case ParleyBuiltinError::Busy: return builtin_codes::kBusy;
    case ParleyBuiltinError::DecodeError: return builtin_codes::kDecodeError;
    case ParleyBuiltinError::None: default: return "";
  }
}

// Maps a wire code string onto a builtin, or None for a custom code.
inline ParleyBuiltinError builtinFromWire(const char* code) {
  if (code == nullptr) return ParleyBuiltinError::None;
  if (strcmp(code, builtin_codes::kNoAck) == 0) return ParleyBuiltinError::NoAck;
  if (strcmp(code, builtin_codes::kExecutionStale) == 0) return ParleyBuiltinError::ExecutionStale;
  if (strcmp(code, builtin_codes::kExecutionTimeout) == 0) return ParleyBuiltinError::ExecutionTimeout;
  if (strcmp(code, builtin_codes::kCancelled) == 0) return ParleyBuiltinError::Cancelled;
  if (strcmp(code, builtin_codes::kBusy) == 0) return ParleyBuiltinError::Busy;
  if (strcmp(code, builtin_codes::kDecodeError) == 0) return ParleyBuiltinError::DecodeError;
  return ParleyBuiltinError::None;
}

// A terminal error surfaced to the commander consumer. Always carries the raw wire `code` (a fixed
// buffer) and the builtin classification; for a custom code it also carries the decoded typed error.
// The optional `m` detail is surfaced in a fixed buffer (truncation accepted).
template <class Err>
class ParleyError {
 public:
  ParleyError() : builtin_(ParleyBuiltinError::None), hasMessage_(false), custom_() {
    code_[0] = '\0';
    message_[0] = '\0';
  }

  const char* code() const { return code_; }
  bool isBuiltin() const { return builtin_ != ParleyBuiltinError::None; }
  ParleyBuiltinError builtin() const { return builtin_; }
  bool hasMessage() const { return hasMessage_; }
  const char* message() const { return message_; }
  // Meaningful only when !isBuiltin(); default-constructed otherwise.
  const Err& custom() const { return custom_; }

  // --- construction (used by the commander shell) ---
  static ParleyError builtinError(ParleyBuiltinError b) {
    ParleyError e;
    e.builtin_ = b;
    copyInto(e.code_, kCodeCapacity, builtinToWire(b));
    return e;
  }
  static ParleyError customError(const char* wireCode, const Err& value, const char* msg, bool hasMsg) {
    ParleyError e;
    e.builtin_ = ParleyBuiltinError::None;
    copyInto(e.code_, kCodeCapacity, wireCode);
    e.custom_ = value;
    if (hasMsg && msg != nullptr) {
      copyInto(e.message_, kMessageCapacity, msg);
      e.hasMessage_ = true;
    }
    return e;
  }
  void setMessage(const char* msg, bool hasMsg) {
    if (hasMsg && msg != nullptr) {
      copyInto(message_, kMessageCapacity, msg);
      hasMessage_ = true;
    }
  }

 private:
  static void copyInto(char* dst, size_t cap, const char* src) {
    if (src == nullptr) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < cap; ++i) dst[i] = src[i];
    dst[i] = '\0';
  }

  char code_[kCodeCapacity];
  ParleyBuiltinError builtin_;
  char message_[kMessageCapacity];
  bool hasMessage_;
  Err custom_;
};

// The terminal outcome of a command, returned to the commander consumer as a value (never thrown).
template <class Res, class Err>
class ParleyResult {
 public:
  ParleyResult() : success_(false), value_(), error_() {}

  bool isSuccess() const { return success_; }
  bool isError() const { return !success_; }
  const Res& value() const { return value_; }
  const ParleyError<Err>& error() const { return error_; }

  static ParleyResult success(const Res& value) {
    ParleyResult r;
    r.success_ = true;
    r.value_ = value;
    return r;
  }
  static ParleyResult failure(const ParleyError<Err>& error) {
    ParleyResult r;
    r.success_ = false;
    r.error_ = error;
    return r;
  }

 private:
  bool success_;
  Res value_;
  ParleyError<Err> error_;
};

}  // namespace parley

#endif  // PARLEY_ERROR_H
