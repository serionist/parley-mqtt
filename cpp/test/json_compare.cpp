#include "json_compare.h"

#include <string.h>

using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonObjectConst;
using ArduinoJson::JsonPairConst;
using ArduinoJson::JsonVariantConst;

namespace {

enum Kind { kNull, kBool, kNum, kStr, kArr, kObj };

Kind kindOf(JsonVariantConst v) {
  if (v.isNull()) return kNull;
  if (v.is<bool>()) return kBool;            // checked before number: bool != number
  if (v.is<const char*>()) return kStr;
  if (v.is<JsonArrayConst>()) return kArr;
  if (v.is<JsonObjectConst>()) return kObj;
  return kNum;  // any remaining scalar is numeric
}

bool objectsEqual(JsonObjectConst a, JsonObjectConst b) {
  if (a.size() != b.size()) return false;  // null-value vs absent-key: counts must match
  for (JsonPairConst kv : a) {
    bool found = false;
    for (JsonPairConst kb : b) {
      if (strcmp(kv.key().c_str(), kb.key().c_str()) == 0) {
        if (!jsonEqual(kv.value(), kb.value())) return false;
        found = true;
        break;
      }
    }
    if (!found) return false;  // key present in a but not b
  }
  return true;
}

bool arraysEqual(JsonArrayConst a, JsonArrayConst b) {
  if (a.size() != b.size()) return false;
  JsonArrayConst::iterator ia = a.begin();
  JsonArrayConst::iterator ib = b.begin();
  for (; ia != a.end() && ib != b.end(); ++ia, ++ib) {
    if (!jsonEqual(*ia, *ib)) return false;
  }
  return true;
}

}  // namespace

bool jsonEqual(JsonVariantConst a, JsonVariantConst b) {
  Kind ka = kindOf(a);
  Kind kb = kindOf(b);
  if (ka != kb) return false;
  switch (ka) {
    case kNull:
      return true;
    case kBool:
      return a.as<bool>() == b.as<bool>();
    case kNum:
      return a.as<double>() == b.as<double>();  // int==float equal; 3.14 exact (USE_DOUBLE)
    case kStr:
      return strcmp(a.as<const char*>(), b.as<const char*>()) == 0;
    case kArr:
      return arraysEqual(a.as<JsonArrayConst>(), b.as<JsonArrayConst>());
    case kObj:
      return objectsEqual(a.as<JsonObjectConst>(), b.as<JsonObjectConst>());
    default:
      return false;
  }
}
