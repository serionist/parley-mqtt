# Parley interop test peer — JS/TS, mqtt.js 5.15.2, run via tsx 4.23.1 (Parley core from source).
# Build context: repo root.
FROM node:24-bookworm-slim
WORKDIR /app
COPY js/ ./js/
COPY docs/conformance/interop/scenarios.json /app/scenarios.json
WORKDIR /app/js/testpeer
RUN npm install --no-audit --no-fund
ENTRYPOINT ["node", "--import", "tsx", "/app/js/testpeer/peer.ts"]
