# Changelog

All notable changes to Parley are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). All four reference libraries share one
lockstep version each release.

## [1.0.1] — 2026-07-23

Documentation, package metadata and CI only — no library code changes; wire behavior is
byte-identical to 1.0.0, upgrading is optional.

### Changed

- **Repositioning of the public-facing text.** The root README now leads with a badge row, the
  one-line pitch, the install table and four differentiator bullets stated as capability: three
  distinct liveness outcomes (`$noAck` / `$executionStale` / `$executionTimeout`), reliability the
  protocol earns itself at QoS 0, cancellation delivered to the running handler, and the same
  protocol down to a statically-allocated microcontroller. Progress streaming, the six built-in
  error codes and "no code generation" are now supporting text rather than headlines.
- **The at-most-once claim is now qualified wherever it appears** — *at most once per handler
  process lifetime, for as long as the handler retains dedup/tombstone state for that command id* —
  with a deep link to [`docs/protocol.md` §9](docs/protocol.md#9-reliability-properties-and-residual-caveats)
  at the point of claim, in all five READMEs (the four packaged ones use the absolute URL, because
  registries resolve relative links against their own domains). **This is a wording change only.**
  The guarantee, and §9 itself, are unchanged since 1.0.0: the qualification was always in the
  spec, and is now repeated everywhere the claim is made.
- **New [`docs/comparison.md`](docs/comparison.md)** — a dated survey of request/response over
  MQTT, sourced from each project's own public documentation, and the only place in the repository
  that carries a comparison table. It records explicitly that shared cross-language protocol test
  cases and compatibility kits exist elsewhere in this space too.
- **New `When not to use Parley` and `Related projects`** sections in the root README.
- **The C++ example moved into the package**: `examples/cpp/backup_example.ino` →
  `cpp/examples/Backup/Backup.ino`, so PlatformIO and the Arduino IDE can render it.

### Added

- **`cpp/README.md` and `cpp/LICENSE`**, both now shipped in the PlatformIO tarball — the registry
  page previously had no README and the package carried no license file.
- **`.github/workflows/ci.yml`** — a deterministic gate on every push to `main` and every pull
  request: the four language suites plus `gate-pack`, which asserts the NuGet package carries its
  README, non-empty tags and a symbol package; the npm tarball carries `dist` + README + LICENSE;
  the Python build succeeds with every trove classifier verified, `twine check --strict` clean and
  `py.typed` in the wheel; the PlatformIO tarball carries the README and an example; and the five
  manifest versions agree. This is the workflow the README's CI badge reports.
- **`.github/workflows/e2e.yml`** — the docker interop matrix, on pushes to `main` and nightly,
  kept separate from the CI badge because it is timing-sensitive and depends on external images.
  `release.yml` continues to run it as a hard pre-publish gate.
- **Registry metadata**: `mqtt-rpc`, `request-reply` and `remote-procedure-call` keywords across all
  four registries, per-registry extras (`typescript`, `dotnet`/`aot`, `asyncio`,
  `arduino`/`embedded`), expanded package descriptions, seven further verified PyPI trove
  classifiers and a PyPI `Changelog` URL, a PlatformIO `homepage`, and `includes=Parley.h` for the
  Arduino Library Manager.

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

[1.0.1]: https://github.com/serionist/parley-mqtt/releases/tag/v1.0.1
[1.0.0]: https://github.com/serionist/parley-mqtt/releases/tag/v1.0.0
