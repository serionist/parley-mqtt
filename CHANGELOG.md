# Changelog

All notable changes to Parley are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). All four reference libraries share one
lockstep version each release.

## [1.0.0] — 2026-07-22

First stable release. The Parley v1 wire protocol is frozen at `v:1`; the public APIs of all four
reference libraries are semver-stable from here (a breaking change means a new major version).

### Added

- **Four reference libraries** implementing one wire protocol
  ([`docs/protocol.md`](docs/protocol.md)): C# (`net10.0` + `netstandard2.0`), TypeScript/JS
  (ESM + CJS + `.d.ts`), Python (asyncio, `py.typed`), and C++ (ESP32/Arduino, statically
  allocated, no exceptions/RTTI).
- **Request/response over MQTT**: request retry until sign-of-life, per-execution heartbeats,
  advisory progress streaming, acknowledged-and-retried results, wire-level cancellation, and six
  built-in error codes — at-most-once execution correct even at QoS 0.
- **Shared conformance suite**: 41 language-neutral vectors replayed by every library over a
  virtual clock; a dockerized e2e matrix of 16 commander×handler language pairs against a real
  broker, including stale-detect and broker-restart disruption scenarios and a QoS-1 compatibility
  pass.
- **Packaging**: published to npm (`parley-mqtt`), PyPI (`parley-mqtt`, module `parley`), NuGet
  (`Parley.Mqtt`, assembly `Parley`), and the PlatformIO Registry (`ParleyMqtt`), released in
  lockstep from a tag-triggered GitHub Actions workflow using OIDC trusted publishing. C# packages
  ship SourceLink and a symbol package.

### Known gaps

- QoS-1 redelivery-deduplication over a real broker is not yet exercised end-to-end (needs
  persistent sessions and induced disconnects); the deduplication logic itself is vector-verified
  in all four libraries.
- Arduino Library Manager listing is deferred to a follow-up single-library mirror repo (the
  Library Manager requires the library at a repo root); PlatformIO ships now.

[1.0.0]: https://github.com/serionist/parley-mqtt/releases/tag/v1.0.0
