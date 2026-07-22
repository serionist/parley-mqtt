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
