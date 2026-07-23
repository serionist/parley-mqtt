# Publishing Parley

How the four Parley packages get to their registries, and the one-time setup each registry needs.

Parley ships **one lockstep version** to four ecosystems from a single tag:

| Ecosystem | Package | Auth model | Repo secret needed |
|---|---|---|---|
| npm | `parley-mqtt` | OIDC trusted publishing (after a one-time token bootstrap) | `NPM_TOKEN` — **temporary**, deleted after bootstrap |
| PyPI | `parley-mqtt` (module `parley`) | OIDC trusted publishing (pending publisher) | none |
| NuGet | `Parley.Mqtt` (assembly `Parley`) | OIDC trusted publishing (owner-scoped policy) | `NUGET_USER` — your nuget.org **username** |
| PlatformIO | `ParleyMqtt` | Personal auth token (no OIDC available) | `PLATFORMIO_AUTH_TOKEN` |

The release workflow ([`.github/workflows/release.yml`](../.github/workflows/release.yml)) is
triggered by pushing a `vX.Y.Z` tag. It verifies the tag against all five manifest version fields,
runs the full gates (four language suites + the docker e2e matrix), then publishes each package and
creates the GitHub Release. Publishing is **keyless wherever the registry supports OIDC** — the two
token cases below are the documented exceptions (npm's first publish, and PlatformIO, which has no
OIDC). The no-secrets convention governs *broker* credentials, not CI publish identity.

> All publishing runs **inside CI**. Do not `npm publish` / `twine upload` from a laptop: provenance
> and OIDC trusted publishing depend on the GitHub Actions OIDC token, which only exists in the
> workflow. A local publish cannot produce provenance and cannot use trusted publishing.

---

## One-time setup

Do all of this **once**, before the first release. Steps that must land before you push the first
tag are marked **(pre-tag)**.

### 0. The GitHub repository (pre-tag)

1. Create a **public** repo `github.com/serionist/parley-mqtt`. (The remote `origin` is already set in
   this working copy — `git remote -v` to confirm.)
2. Push `main`. **The first push publishes the source publicly** — make sure you intend that.
3. Suggested branch protection on `main`: require a pull request before merging, require the status
   checks green, and restrict who can push tags (tags are what trigger a release). Consider a
   protected GitHub **Environment** named `release` with required reviewers if you want a manual
   approval gate on the publish jobs — then add `environment: release` to each publish job and set
   the matching **Environment** field in the npm/PyPI/NuGet configs below (all are left blank by
   default).

### 1. PyPI — pending publisher (pre-tag)

PyPI can authorize a publisher **before the project exists**; the project is created on first
publish. No secret is ever stored.

1. Log in to <https://pypi.org> → your account → **Publishing** → **Add a pending publisher**
   (GitHub Actions).
2. Fill in exactly:
   - **PyPI Project Name:** `parley-mqtt`
   - **Owner:** `serionist`
   - **Repository name:** `parley-mqtt`
   - **Workflow name:** `release.yml`
   - **Environment name:** *(leave blank, unless you set up the `release` environment above)*
3. Save.

> **Squat caveat:** a pending publisher does **not** reserve the name. If someone registers
> `parley-mqtt` on PyPI before your first publish, the pending publisher is **invalidated**. Create
> the pending publisher and cut the first release promptly.

### 2. NuGet — trusted publishing policy (pre-tag)

NuGet's policy is **owner-scoped** (it applies to all packages owned by you), so the package does not
need to exist first.

1. Log in to <https://www.nuget.org> → your username → **Trusted Publishing** → add a policy:
   - **Repository Owner:** `serionist`
   - **Repository:** `parley-mqtt`
   - **Workflow File:** `release.yml`  *(file name only — no `.github/workflows/` prefix)*
   - **Environment:** *(leave blank, unless using the `release` environment)*
2. Add a repo secret **`NUGET_USER`** = your nuget.org account **username** (the profile name, **not**
   your email address). The `NuGet/login` action needs it to select the account.

> **Gradual rollout:** if you don't see **Trusted Publishing** in your nuget.org account yet, it
> hasn't reached you. Fallback (documented, not preferred): create a nuget.org **API key** scoped to
> push `Parley.Mqtt`, store it as a repo secret `NUGET_API_KEY`, and in the `publish-nuget` job drop
> the `NuGet/login` step, remove `id-token: write`, and push with `--api-key "$NUGET_API_KEY"`.
>
> Because the repo is **public**, the policy activates permanently on the first successful publish
> (the 7-day "pending activation" window only applies to private repos).

### 3. PlatformIO — account token (pre-tag)

PlatformIO has no OIDC; it uses a personal auth token.

1. Create/log in to a PlatformIO account and generate a **Personal Authentication Token**
   (`pio account token`, or the web dashboard).
2. Add it as the repo secret **`PLATFORMIO_AUTH_TOKEN`**. `pio pkg publish` reads it from the
   environment; the workflow packs `cpp/` (only `src/` + the two manifests, per the `export` filter
   in `library.json`).

### 4. npm — the token bootstrap, then trusted publishing

npm has **no pending-publisher mechanism**: a brand-new package name **cannot** be published
tokenlessly the first time, and you cannot pre-configure a Trusted Publisher for a package that does
not exist yet. So the first release is a one-time, CI-run token bootstrap; every release after it is
keyless. The workflow's npm job is **dual-path** and needs no change between the two.

**Bootstrap (first release only):**

1. On <https://www.npmjs.com> create a **Granular Access Token** (Account → Access Tokens →
   Generate → *Granular*): **Read and write** packages, a **short** expiry, and — because the
   package doesn't exist yet — "All packages" (or your user scope). Enable the *bypass 2FA for
   automation* option so CI can publish unattended.
2. Add it as the repo secret **`NPM_TOKEN`**.
3. Push the `v1.0.0` tag. The `publish-npm` job publishes with the token **and** `--provenance`
   (provenance works with a token because the job also has `id-token: write`). This creates
   `parley-mqtt@1.0.0` on npm, with provenance.
4. On npmjs, open the now-existing package → **Settings → Trusted Publishers** → add a GitHub Actions
   publisher:
   - **Organization or user:** `serionist`
   - **Repository:** `parley-mqtt`
   - **Workflow filename:** `release.yml`
   - **Environment:** *(blank, unless using `release`)*
5. **Delete the `NPM_TOKEN` repo secret and revoke the token on npmjs.**

**Every release after the bootstrap** is tokenless: with `NPM_TOKEN` absent, the workflow takes
its separate OIDC step, which configures **no auth at all** — npm detects the CI OIDC context and
performs the trusted-publishing exchange itself. (The two paths are distinct workflow steps on
purpose: an `.npmrc` `_authToken` line with an *empty* env var would silently suppress the OIDC
exchange — actions/setup-node#1551 — so the tokenless path must carry no auth config whatsoever.
Trusted publishing needs **npm ≥ 11.5.1 / Node ≥ 22.14**; the job runs `npm install -g npm@latest`
before publishing to guarantee it.)

### Repo secrets summary

| Secret | When | Notes |
|---|---|---|
| `NPM_TOKEN` | bootstrap only | delete + revoke after configuring the npm Trusted Publisher |
| `NUGET_USER` | permanent | nuget.org **username**, not email |
| `PLATFORMIO_AUTH_TOKEN` | permanent | PlatformIO personal auth token |
| `NUGET_API_KEY` | fallback only | only if NuGet trusted publishing isn't enabled for you yet |

PyPI needs **no** secret. `GITHUB_TOKEN` is provided automatically to the GitHub Release job.

---

## Cutting a release

1. **Update the changelog.** Add a `## [X.Y.Z] — DATE` section to
   [`CHANGELOG.md`](../CHANGELOG.md) (the GitHub Release notes are pulled from it).
2. **Bump the version** in lockstep across all five manifests:
   ```bash
   scripts/bump-version.sh X.Y.Z     # then `scripts/bump-version.sh --check` prints/verifies them
   ```
3. **Land it on `main`** through a pull request (branch protection will not let you push straight to
   `main`). The release workflow refuses to publish a tag whose commit is not an ancestor of `main`.
4. **Tag the merged commit and push the tag:**
   ```bash
   git checkout main && git pull
   git tag vX.Y.Z          # must match the version in the manifests
   git push origin vX.Y.Z
   ```
5. The workflow runs: **verify → gates (four suites + e2e) → publish (npm · PyPI · NuGet ·
   PlatformIO) → GitHub Release.** If any gate is red, nothing publishes.
6. **Confirm** the four packages show the new version and that the GitHub Release was created.

If a gate fails after a tag is pushed, delete the tag (`git push origin :refs/tags/vX.Y.Z`), fix the
problem on `main`, and re-tag. Registries reject re-publishing an already-published version, so never
reuse a version number for a changed artifact.
