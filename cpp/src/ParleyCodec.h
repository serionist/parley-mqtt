#ifndef PARLEY_CODEC_H
#define PARLEY_CODEC_H

#include "detail/json.h"

namespace parley {

// The codec customization point: how a typed payload maps to/from an ArduinoJson value (design.md —
// "ArduinoJson codec; consumer-supplied encode/decode over ArduinoJson"). The default delegates to
// ArduinoJson's own conversion, which covers scalars, strings, and any type with an ArduinoJson
// `convertToJson` / `convertFromJson` overload. Consumers specialize `ParleyCodec<TheirType>` for
// structured payloads.
//
// A codec never sees an absent payload: the shell substitutes the type's default value when `p` is
// omitted/null (protocol.md §4 — "omitted/null means no payload" ≡ the empty value).
template <class T>
struct ParleyCodec {
  // Writes `value` into `dst`. Returns false on encode failure (e.g. fixed-arena overflow).
  static bool encode(const T& value, ArduinoJson::JsonVariant dst) { return dst.set(value); }

  // Reads `src` into `out`. Returns false when `src` does not represent a T (→ $decodeError on the
  // commander result path; silent drop on the request/progress paths).
  static bool decode(ArduinoJson::JsonVariantConst src, T& out) {
    if (!src.template is<T>()) return false;
    out = src.template as<T>();
    return true;
  }
};

}  // namespace parley

#endif  // PARLEY_CODEC_H
