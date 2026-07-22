---
name: review-plan
description: Reviews a drafted implementation plan BEFORE any code, against the brief/protocol spec and the conventions. One background reviewer finds AND adversarially refutes its own findings; survivors converge the plan via a propose→apply→ask loop until APPROVED. The plan-time analogue of review-code. Interactive for the loop.
---

# review-plan

Stress-tests a drafted plan while fixing it is still a one-line edit. Runs BEFORE implementation, inside
plan mode; the apply-target is the **plan file**.

## Instructions
1. **Resolve the plan** (an explicit path, else the in-flight plan-mode scratch file). Confirm the path.
   Don't read the plan or the ground-truth docs into the main context — that's the reviewer's job.
2. **Run the review** — launch **one background `Agent`** (`general-purpose`) that must:
   a. Read the ground truth: `README.md` (the brief), `docs/protocol.md` + the decisions log if they
      exist, and `.claude/CONVENTIONS.md` — then the plan. Read-only.
   b. Apply every lens below as a checklist and adversarially **refute each finding** (default-refute when
      uncertain; the dominant plan false-positive is "the plan already covers this two sections down").
      Keep refuted findings in the output with the reason.
   c. Return: confirmed findings (title, severity blocker/major/minor/nit, plan section, spec anchor,
      evidence, recommended plan edit) + refuted findings + a verdict `BLOCKED | WARN | APPROVED`.

### Lenses (tune to a cross-platform library)
- **spec/decision-conflict** (always on) — does the plan contradict the brief or a locked decision?
- **omissions / completeness** (always on) — missing steps, tests, or spec/vector updates.
- **wire-contract impact** — does it change the protocol? Is the spec + conformance-vector update
  scheduled in the same change?
- **cross-language consistency** — a change in one language owes a matching change (or a stated reason) in
  the others; do the three APIs stay parallel?
- **layering** — protocol logic stays in the core; adapters/generated code stay thin.
- **API design** — is the public surface minimal, idiomatic per language, and consistent across them?
- **sequencing** — foundations before surface; nothing depends on a later step.
- **testability** — is each step provable by a fake-transport test or a conformance vector?
- **simplicity / YAGNI** — no premature generality.
- **pre-mortem** — the most likely way this plan produces a wrong or divergent implementation.

3. **Write the review file** to a scratch path; findings get stable ids + status.
4. **Propose → apply → ask.** Auto-apply the clear, non-design-changing fixes to the plan (missing test
   line, missing spec/vector task, ordering, tightened acceptance check). **Ask the user** for anything
   that changes the plan's design or scope — the plan is what they approve; never silently redesign it.
5. **Re-review the deltas** (a fix can open a gap) until the exit condition.

## Exit — `APPROVED` requires
every question answered + applied; every blocker/major VERIFIED or user-approved-WONTFIX; the last
re-review added no new blocker/major; and the plan schedules the spec + conformance-vector updates in the
same change if the wire/API surface moves, ending with the `review-code` checkpoint.

*(No "gates green" exit item — a plan has nothing to build yet; the brief/spec is the ground truth.)*

On `APPROVED`, hand back to `plan-first`. A non-interactive run STOPS after writing the review file +
questions; never auto-resolve a design/scope question. Deep tier (a per-lens fan-out) can be added later;
the standard single-reviewer tier above works end-to-end.
