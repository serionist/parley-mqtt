# Parley — build map (CLAUDE.md)

**Parley is a small cross-platform library for request/response RPC over MQTT** (C# · C++/ESP32 ·
JS/TS), so a .NET service, an embedded firmware, and a JS edge script can call each other over the MQTT
broker they already share. Purpose + functionalities: **[`README.md`](README.md)** (the brief).

**Status: idea/brief only.** Right now this repo is just the brief + this working-method setup. Nothing
is implemented. The next step is to write the protocol spec, then build the three reference libraries.

## Standing instructions

- **Plan first.** Start a working session with the `plan-first` skill: a short plan → the user's explicit
  approval before editing files. Non-trivial work runs the chain **plan-first → `review-plan` →
  implement → `review-code`**. Trivial dictated one-liners may skip.
- **Spec is the contract.** Once the wire protocol is written (`docs/protocol.md`), it is normative — code
  serves it; changing wire behavior edits the spec in the same change. Until then, `README.md` is the
  brief the design must honor.
- **One shape, three languages.** Keep the C#, C++, and JS/TS public APIs deliberately parallel — a
  divergence is a design smell to justify, not a free choice. Keep protocol logic in each language's core;
  transport adapters and generated stubs stay thin.
- **Context7 before coding against a library** (MQTTnet, mqtt.js, PubSubClient, ArduinoJson, the build
  toolchains) — APIs drift.
- **Latest-stable, pinned** for every dependency and toolchain (`update-deps` skill).
- **No secrets, ever.** Parley never holds broker credentials or TLS material — the consumer supplies
  those to the transport at runtime.
- **Public OSS discipline.** No consumer-specific detail (hostnames, private topics, business logic) —
  only the standalone library and generic docs. Examples use placeholder namespaces + endpoint ids.
- **Prefer section edits over whole-file rewrites.** The diff is the review.

## Layout (as it grows)
```
README.md         the brief — purpose + main functionalities
docs/             protocol spec + design + schema/codegen (to be written)
csharp/ js/ cpp/  the three reference libraries (to be built)
.claude/          CONVENTIONS.md + skills (plan-first · review-plan · review-code · update-deps)
```

## Skills
`plan-first` · `review-plan` · `review-code` · `update-deps` — the plan→review→implement→review loop and
the dependency-currency pass, carried over from the parent project and adapted for a library. See
[`.claude/CONVENTIONS.md`](.claude/CONVENTIONS.md).
