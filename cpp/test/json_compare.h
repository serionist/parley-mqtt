#ifndef PARLEY_TEST_JSON_COMPARE_H
#define PARLEY_TEST_JSON_COMPARE_H

#include "detail/json.h"

// Type-discriminating deep JSON equality, matching the frozen C# JsonComparer semantics (plan F3):
//   - objects compare by member iteration + count (null-value vs absent-key distinct);
//   - arrays element-wise in order;
//   - numbers by value with int==float equality (1 == 1.0) and 3.14 exact (double compare);
//   - bool != number (distinct kinds);
//   - strings by content (UTF-8 bytes).
// ArduinoJson's variant operator== coerces types, so this hand-rolled comparator is required.
bool jsonEqual(ArduinoJson::JsonVariantConst a, ArduinoJson::JsonVariantConst b);

#endif  // PARLEY_TEST_JSON_COMPARE_H
