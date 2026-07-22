#include <string>
#include <vector>

#include "doctest.h"
#include "vector_runner.h"

// Layer-2 conformance (docs/testing.md): replays every docs/conformance/*.json vector against the
// protocol core, both roles. Discovered from the host filesystem so a new vector is picked up with no
// code change; each file becomes its own doctest subcase (one test per file, protocol.md §10).
TEST_CASE("conformance vectors (protocol.md §10)") {
  std::vector<std::string> vectors = discoverVectors();
  CHECK_MESSAGE(vectors.size() == 41,
                "expected 41 conformance vectors, found " << vectors.size());
  for (size_t i = 0; i < vectors.size(); ++i) {
    const std::string& path = vectors[i];
    std::string base = path.substr(path.find_last_of('/') + 1);
    DOCTEST_SUBCASE(base.c_str()) { runVector(path); }
  }
}

// TQ2: the runner's structural guard rejects malformed vectors instead of hitting UB (missing role →
// std::string(nullptr)) or vacuously passing (missing steps) or ticking backward (decreasing at).
// Synthetic bad inputs are fed as inline strings — no files are added to docs/conformance.
TEST_CASE("TQ2: vectorStructureError rejects malformed vectors without UB") {
  // Well-formed minimal vectors pass.
  CHECK(vectorStructureError("{\"role\":\"commander\",\"steps\":[{\"at\":0,\"expect\":[]}]}") == "");
  CHECK(vectorStructureError("{\"role\":\"handler\",\"steps\":[]}") == "");

  // Missing role — the exact UB (std::string(nullptr)) the old runner would hit.
  CHECK(vectorStructureError("{\"steps\":[]}") != "");
  // Unknown role.
  CHECK(vectorStructureError("{\"role\":\"referee\",\"steps\":[]}") != "");
  // Missing steps (would vacuously "pass" with no assertions).
  CHECK(vectorStructureError("{\"role\":\"commander\"}") != "");
  // Non-integer at.
  CHECK(vectorStructureError("{\"role\":\"commander\",\"steps\":[{\"at\":\"soon\"}]}") != "");
  // Decreasing at (a silent backward tick).
  CHECK(vectorStructureError(
            "{\"role\":\"handler\",\"steps\":[{\"at\":10,\"expect\":[]},{\"at\":5,\"expect\":[]}]}") !=
        "");
  // Equal (non-decreasing) at is allowed — same-`at` step pairs are ubiquitous in the real vectors.
  CHECK(vectorStructureError(
            "{\"role\":\"handler\",\"steps\":[{\"at\":10,\"expect\":[]},{\"at\":10,\"expect\":[]}]}") ==
        "");

  // Parse errors and non-object roots.
  CHECK(vectorStructureError("not json {{{") == "parse error");
  CHECK(vectorStructureError("[1,2,3]") != "");
}
