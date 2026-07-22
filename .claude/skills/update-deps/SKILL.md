---
name: update-deps
description: Bump every dependency and toolchain to its newest STABLE release and verify nothing broke. Use when the user says "update dependencies", "bump everything", "are we on the latest", or before a release. Enforces the latest-stable-pinned convention. Interactive (consults Context7 for breaking changes).
---

# update-deps

The deliberate, **verified** pass that keeps the repo on the newest stable of everything, pinned to
concrete versions (never floating ranges / `:latest` / prereleases without a recorded waiver). The rule is
*pinned*, not *stale* — this skill is how the pins stay current without giving up reproducibility.

Run it in a `plan-first` session like any change; it ends in `review-code` because it touches the build.

## 1. Enumerate every dependency source
- **NuGet (C#):** central pins (`csharp/Directory.Packages.props` or the `.csproj`) —
  `dotnet list csharp/Parley.slnx package --outdated` and `--vulnerable`.
- **.NET SDK:** `csharp/global.json` (`sdk.version`) vs the newest released feature band.
- **npm (JS/TS):** `js/package.json` (+ lockfile) — `cd js && npm outdated`. Include `devDependencies`
  (TypeScript, the bundler, the test runner).
- **PlatformIO / C++:** `cpp/library.json` dependencies (ArduinoJson, PubSubClient) and any
  `platformio.ini` platform/framework pins.
- **CI actions (when added):** `uses:` pins in `.github/workflows/*.yml`.

## 2. Resolve the newest STABLE
- The `outdated` reports give NuGet/npm; **exclude prereleases** (no `-preview`/`-rc`/`-beta` except a
  recorded waiver). C++ libs: check the registry / GitHub releases for the latest stable tag.
- **Pull current docs via Context7** for any non-trivial jump — note breaking changes BEFORE bumping.

## 3. Bump the pins
- Section-edit each source to the **exact** newest-stable version. Never a range or `:latest`.

## 4. Verify (non-negotiable) — roll back any single bump that fails a gate, and record it
- **C#:** `dotnet restore && dotnet build csharp/Parley.slnx` (warnings-as-errors)
  `&& dotnet format csharp/Parley.slnx --verify-no-changes && dotnet test csharp/Parley.slnx`.
- **JS/TS:** `cd js && npm ci && npm run typecheck && npm test && npm run build`.
- **C++:** `cd cpp && pio test -e native` (+ an ESP32 build if the toolchain is present).
- If the conformance vectors exist, they must still pass in every language (the interop guarantee).

## 5. Report + record
- Emit a table: **dep · old → new · breaking? · gate result**. Note anything held back and why.
- Record any waiver (a deliberately-pinned prerelease) with its closure condition. Then `review-code` →
  commit (when the user asks).

## Notes
- Interactive only — Context7 + the build smoke aren't headless-friendly.
- A major-version jump (a runtime/framework migration) is **not** a routine bump — stop and plan it as its
  own change; this skill is for staying current within stable lines.
