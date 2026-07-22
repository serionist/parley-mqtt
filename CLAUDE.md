# Parley — build map (CLAUDE.md)

**Parley is a small cross-platform library for request/response RPC over MQTT** (C# · Python ·
C++/ESP32 · JS/TS), so a .NET service, a Python service, an embedded firmware, and a JS edge script can
call each other over the MQTT broker they already share. Purpose + functionalities:
**[`README.md`](README.md)** (the brief).

**Status: v1 complete.** The wire protocol (`docs/protocol.md`), design + decisions docs, the
41 conformance vectors, all four libraries — C# (66 tests), TS (78), Python (102), C++ (1407
assertions incl. ASan/UBSan) — each deep-reviewed CLEAN against the same vector net, AND the
docker e2e matrix (`e2e/`): 16 commander×handler pairs × 9 scenarios + stale-detect +
broker-restart disruptions + a QoS-1 compatibility pass, ALL GREEN ×2. Recorded gap: QoS-1
redelivery-dedup over the wire (needs persistent sessions; future work).

**Publishing is wired for v1.0.0 (lockstep):** npm `parley-mqtt` · PyPI `parley-mqtt` (module
stays `parley`) · NuGet `Parley.Mqtt` (assembly stays `Parley`) · PlatformIO `ParleyMqtt`, from
`github.com/serionist/parley` via a tag-triggered GitHub Actions release workflow using OIDC
trusted publishing (runbook: [`docs/publishing.md`](docs/publishing.md)). The first push + the
first `v1.0.0` tag are user-gated (one-time registry setup required). Next candidates: PR-CI for
day-to-day development, reference adapter packages, the Arduino Library Manager mirror.

## Standing instructions

- **Plan first.** Start a working session with the `plan-first` skill: a short plan → the user's explicit
  approval before editing files. Non-trivial work runs the chain **plan-first → `review-plan` →
  implement → `review-code`**. Trivial dictated one-liners may skip.
- **Spec is the contract.** The wire protocol (`docs/protocol.md`) is normative — code serves it;
  changing wire behavior edits the spec (and the conformance vectors) in the same change. `README.md`
  is the non-normative brief.
- **One shape, four languages.** Keep the C#, Python, C++, and JS/TS public APIs deliberately parallel —
  a divergence is a design smell to justify, not a free choice. Keep protocol logic in each language's
  core; transport adapters stay thin.
- **Context7 before coding against a library** (MQTTnet, paho-mqtt, mqtt.js, PubSubClient, ArduinoJson,
  the build toolchains) — APIs drift.
- **Latest-stable, pinned** for every dependency and toolchain (`update-deps` skill).
- **No secrets, ever.** Parley never holds broker credentials or TLS material — the consumer supplies
  those to the transport at runtime.
- **Public OSS discipline.** No consumer-specific detail (hostnames, private topics, business logic) —
  only the standalone library and generic docs. Examples use placeholder namespaces + endpoint ids.
- **Prefer section edits over whole-file rewrites.** The diff is the review.

## Layout (as it grows)
```
README.md         the OSS front door — pitch · how it works · per-language usage
LICENSE           MIT © 2026 The Parley Authors
CHANGELOG.md      release notes (lockstep across all four packages)
docs/             protocol.md (normative) · design.md · testing.md · decisions.md · publishing.md · conformance/
csharp/ python/   the four reference libraries (each with a packaged README + publish manifest)
js/ cpp/
examples/         one worked example per language (the same backup command) + illustrative adapters
scripts/          bump-version.sh — lockstep version bump across the five manifest fields
.github/          release workflow (tag v* → gates → OIDC publish) + dependabot
.claude/          CONVENTIONS.md + skills (plan-first · review-plan · review-code · update-deps)
```

## Skills
`plan-first` · `review-plan` · `review-code` · `update-deps` — the plan→review→implement→review loop and
the dependency-currency pass, carried over from the parent project and adapted for a library. See
[`.claude/CONVENTIONS.md`](.claude/CONVENTIONS.md).
