#ifndef PARLEY_TEST_VECTOR_RUNNER_H
#define PARLEY_TEST_VECTOR_RUNNER_H

#include <string>
#include <vector>

// Replays one protocol.md §10 conformance vector against the Parley core over the fake adapter and a
// virtual clock (tick before each step, §10 timer rule). Reports via doctest CHECK macros; every
// message names the vector + step. Supports both roles, all directives, exact-set expect matching, raw
// payload injection, scripted call ids, outbox-empty-after, and per-call outcome checks.
void runVector(const std::string& path);

// Lists docs/conformance/*.json (host filesystem, PARLEY_CONFORMANCE_DIR), sorted.
std::vector<std::string> discoverVectors();

// Structural validation of a vector document's TEXT with NO doctest side effects (TQ2). Returns an
// empty string when the shape is well-formed (parseable JSON object; role is "commander"/"handler";
// steps is an array whose every `at` is an integer and non-decreasing); otherwise a short reason.
// runVector() REQUIREs this before replaying; tests assert it directly on synthetic malformed input.
std::string vectorStructureError(const std::string& text);

#endif  // PARLEY_TEST_VECTOR_RUNNER_H
