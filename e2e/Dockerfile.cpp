# Parley interop test peer — C++ desktop, libmosquitto (Debian bookworm 2.0.11), ArduinoJson 7.4.3
# (vendored, pinned). The frozen protocol core is compiled straight from cpp/src. Build context: repo root.
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake libmosquitto-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY cpp/src/ ./cpp/src/
COPY cpp/test/vendor/ ./cpp/test/vendor/
COPY cpp/testpeer/ ./cpp/testpeer/
RUN cmake -S cpp/testpeer -B /build -DCMAKE_BUILD_TYPE=Release && cmake --build /build -j

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libmosquitto1 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /build/parley_testpeer /app/parley_testpeer
COPY docs/conformance/interop/scenarios.json /app/scenarios.json
ENTRYPOINT ["/app/parley_testpeer"]
