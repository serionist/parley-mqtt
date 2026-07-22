# Parley interop test peer — C++ desktop, libmosquitto (Debian bookworm, apt-version-pinned), ArduinoJson
# 7.4.3 (vendored, pinned). The frozen protocol core is compiled straight from cpp/src. Build context:
# repo root. Base image digest-pinned (tag kept for readability) and libmosquitto pinned to an exact apt
# version for reproducible builds — resolved 2026-07.
FROM debian:bookworm-slim@sha256:7b140f374b289a7c2befc338f42ebe6441b7ea838a042bbd5acbfca6ec875818 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake libmosquitto-dev=2.0.11-1.2+deb12u2 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY cpp/src/ ./cpp/src/
COPY cpp/test/vendor/ ./cpp/test/vendor/
COPY cpp/testpeer/ ./cpp/testpeer/
RUN cmake -S cpp/testpeer -B /build -DCMAKE_BUILD_TYPE=Release && cmake --build /build -j

FROM debian:bookworm-slim@sha256:7b140f374b289a7c2befc338f42ebe6441b7ea838a042bbd5acbfca6ec875818
RUN apt-get update && apt-get install -y --no-install-recommends libmosquitto1=2.0.11-1.2+deb12u2 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /build/parley_testpeer /app/parley_testpeer
COPY docs/conformance/interop/scenarios.json /app/scenarios.json
COPY docs/conformance/interop/deep-scenarios.json /app/deep-scenarios.json
ENTRYPOINT ["/app/parley_testpeer"]
