---
name: plan-first
description: The session-start planning front door. Use at the START of a working session, or whenever the user gives a new task, BEFORE editing files. Produce a short session plan and get explicit approval. No feature code on an unapproved plan. Interactive only.
---

# plan-first

Every working session starts here. **Before editing any file**, emit a short **session plan** and get the
user's **explicit approval**. Trivial, unambiguous one-liners the user dictated verbatim (a typo, a
one-word doc fix) may skip straight to the edit; **anything touching code, the protocol/wire contract, a
public API, or more than one file goes through the plan.**

Read the brief (`README.md`) and, once it exists, the protocol spec (`docs/protocol.md`) + the decisions
log first. Ground the plan in them.

## The session plan — state these, concisely
1. **Goal** — one sentence; in-scope vs explicitly out-of-scope.
2. **Governing spec/decisions** — which part of the protocol spec / which locked decisions govern this.
   If it **contradicts a locked decision → STOP** and surface it now.
3. **Touch map** — which files/languages change. Respect the "one shape, three languages" rule and the
   fat-core/thin-edge layering.
4. **Wire/API impact** — does this change the wire protocol, the public API of any language, or the
   schema/codegen? If yes, the spec + conformance vectors update in the same change.
5. **Cross-language consistency** — if it lands in one language, note the matching change owed to the
   others (or why not).
6. **Test plan** — which fake-transport unit tests + conformance vectors prove it.
7. **Review checkpoint** — the last step before commit is `review-code`.
8. **Risks / open questions** — resolve with the user **before** coding (no guessing).

**Before `AWAITING APPROVAL` on a non-trivial plan, run `review-plan`** — it stress-tests the drafted
plan against the brief/spec and converges it to `APPROVED`. A `BLOCKED` verdict stops the session at
planning — fix the plan, don't code. End with an explicit **`AWAITING APPROVAL`** line; only implement
after the user approves.

## The chain
```
plan-first ──draft──▶ review-plan ──APPROVED──▶ IMPLEMENT ──diff──▶ review-code ──CLEAN──▶ CLOSE
```

## Interactivity
Needs the user's plan approval → **interactive sessions only**.
