# Parley interop test peer — JS/TS, mqtt.js 5.15.2, run via tsx 4.23.1 (Parley core from source).
# Build context: repo root. Base image digest-pinned (tag kept for readability) — resolved 2026-07.
FROM node:24-bookworm-slim@sha256:6f7b03f7c2c8e2e784dcf9295400527b9b1270fd37b7e9a7285cf83b6951452d
WORKDIR /app
COPY js/ ./js/
COPY docs/conformance/interop/scenarios.json /app/scenarios.json
COPY docs/conformance/interop/deep-scenarios.json /app/deep-scenarios.json
WORKDIR /app/js/testpeer
RUN npm install --no-audit --no-fund
ENTRYPOINT ["node", "--import", "tsx", "/app/js/testpeer/peer.ts"]
