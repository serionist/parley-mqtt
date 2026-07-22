#ifndef PARLEY_TESTPEER_JSON_UTIL_H
#define PARLEY_TESTPEER_JSON_UTIL_H

#include <string.h>

#include "Parley.h"

// Type-discriminating deep JSON equality, matching the frozen cross-language JsonComparer semantics
// (identical to cpp/test/json_compare.cpp): objects by member iteration + count (null-value vs
// absent-key distinct); arrays element-wise in order; numbers by value with int==float equality and
// 3.14 exact (double compare, ARDUINOJSON_USE_DOUBLE); bool != number (distinct kinds); strings by
// UTF-8 content. ArduinoJson's variant operator== coerces types, so this hand-rolled comparator is
// required.
inline bool peerJsonEqual(ArduinoJson::JsonVariantConst a, ArduinoJson::JsonVariantConst b);

namespace peer_json_detail {

enum Kind { kNull, kBool, kNum, kStr, kArr, kObj };

inline Kind kindOf(ArduinoJson::JsonVariantConst v) {
  if (v.isNull()) return kNull;
  if (v.is<bool>()) return kBool;  // checked before number: bool != number
  if (v.is<const char*>()) return kStr;
  if (v.is<ArduinoJson::JsonArrayConst>()) return kArr;
  if (v.is<ArduinoJson::JsonObjectConst>()) return kObj;
  return kNum;
}

}  // namespace peer_json_detail

inline bool peerJsonEqual(ArduinoJson::JsonVariantConst a, ArduinoJson::JsonVariantConst b) {
  using namespace peer_json_detail;
  Kind ka = kindOf(a);
  Kind kb = kindOf(b);
  if (ka != kb) return false;
  switch (ka) {
    case kNull:
      return true;
    case kBool:
      return a.as<bool>() == b.as<bool>();
    case kNum:
      return a.as<double>() == b.as<double>();
    case kStr:
      return strcmp(a.as<const char*>(), b.as<const char*>()) == 0;
    case kArr: {
      ArduinoJson::JsonArrayConst aa = a.as<ArduinoJson::JsonArrayConst>();
      ArduinoJson::JsonArrayConst bb = b.as<ArduinoJson::JsonArrayConst>();
      if (aa.size() != bb.size()) return false;
      ArduinoJson::JsonArrayConst::iterator ia = aa.begin();
      ArduinoJson::JsonArrayConst::iterator ib = bb.begin();
      for (; ia != aa.end() && ib != bb.end(); ++ia, ++ib) {
        if (!peerJsonEqual(*ia, *ib)) return false;
      }
      return true;
    }
    case kObj: {
      ArduinoJson::JsonObjectConst aa = a.as<ArduinoJson::JsonObjectConst>();
      ArduinoJson::JsonObjectConst bb = b.as<ArduinoJson::JsonObjectConst>();
      if (aa.size() != bb.size()) return false;
      for (ArduinoJson::JsonPairConst kv : aa) {
        bool found = false;
        for (ArduinoJson::JsonPairConst kb2 : bb) {
          if (strcmp(kv.key().c_str(), kb2.key().c_str()) == 0) {
            if (!peerJsonEqual(kv.value(), kb2.value())) return false;
            found = true;
            break;
          }
        }
        if (!found) return false;
      }
      return true;
    }
    default:
      return false;
  }
}

// Whether `o` contains `key` (distinguishes an absent key from a present null-valued key).
inline bool objHasKey(ArduinoJson::JsonObjectConst o, const char* key) {
  for (ArduinoJson::JsonPairConst kv : o) {
    if (strcmp(kv.key().c_str(), key) == 0) return true;
  }
  return false;
}

#endif  // PARLEY_TESTPEER_JSON_UTIL_H
