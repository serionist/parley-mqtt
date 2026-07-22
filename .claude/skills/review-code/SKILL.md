---
name: review-code
description: One code-review cycle over a diff, against the protocol spec and the conventions. One background reviewer finds AND adversarially refutes its own findings; survivors converge via a propose→apply→ask loop until CLEAN and the gates are green. Interactive for the loop.
---

# review-code

The last step before commit. Reviews a change against the ground truth, fixes what's clear, asks what's
ambiguous, and converges to `CLEAN`.

## Instructions
1. **Resolve scope** from the argument: empty → the current branch diff vs the default branch (whole repo
   if no diff); a path → that; `all` → whole repo. Confirm with `git diff --stat`. Exclude generated files
   (list them as skipped on purpose). Trivial diffs (typos/formatting) are covered by the gates — say so
   and skip. Don't read the diff or ground-truth docs into the main context — that's the reviewer's job.
2. **Run the review** — launch **one background `Agent`** (`general-purpose`) that must:
   a. Read the ground truth: `docs/protocol.md` (the wire contract, once written; else `README.md`), the
      decisions log, `.claude/CONVENTIONS.md`, `CLAUDE.md` — then the in-scope files. Read-only.
   b. Apply every lens below and adversarially **refute each finding** (nit dressed as a bug? already
      handled? contradicted by the source? uncertain → default-refute). Keep refuted findings with reasons.
   c. Return: confirmed findings (title, severity blocker/major/minor/nit, `file:line`, evidence,
      recommended fix) + refuted findings + a one-line verdict.

### Lenses (cross-platform library)
- **protocol conformance** — does the code match the wire spec (envelopes, topics, ack/progress/result,
  heartbeat, timeouts, retransmit/dedup)? Would it stay wire-compatible with the other languages?
- **correctness** — real bugs: edge/null/boundary, races, cancellation, error handling.
- **cross-language consistency** — the public API/behavior stays parallel with the other two languages.
- **API design & conventions** — idiomatic per language, minimal surface, matches `.claude/CONVENTIONS.md`
  (layering, no secrets, no consumer specifics).
- **simplicity** — no needless abstraction/duplication/premature generality.
- **tests** — fake-transport unit coverage + the relevant conformance vectors; real assertions, denial/
  failure paths, not just happy path.
- **docs/spec freshness** — if the wire/API surface moved, the spec + conformance vectors moved in the
  same change.
- **resource-safety** — buffers (esp. C++ explicit-length publish), allocation, connection lifecycle.

3. **Write the review file** to a scratch path; findings get stable ids + status.
4. **Propose → apply → ask.** Auto-apply the clear fixes via section-edits (updating the spec/vectors in
   the same change when the surface moves). **Ask the user** only the genuinely ambiguous ones (needs
   intent). Record answers verbatim; never guess intent.
5. **Re-review the deltas** (fixes can add findings) until the exit condition.

## Exit — `CLEAN` requires
every question answered + resolved; every blocker/major VERIFIED or user-approved-WONTFIX; the last
re-review added no new blocker/major; **gates green** (each touched language builds warnings-clean, its
formatter verifies no-changes, its tests pass); and if the wire/API surface moved, the spec + conformance
vectors were updated in the same change.

**Manual stop:** the user can end the loop any time ("this is fine"); remaining findings are recorded as
accepted/deferred (never silently dropped). A non-interactive run STOPS after writing the review file +
questions. Deep tier (per-lens fan-out) can be added later; the standard single-reviewer tier works
end-to-end.
