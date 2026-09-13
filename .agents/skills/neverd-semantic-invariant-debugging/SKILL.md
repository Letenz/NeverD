---
name: neverd-semantic-invariant-debugging
description: Diagnose NeverD lifting bugs caused by duplicated or divergent semantic decisions across pipeline surfaces. Use for cross-architecture, cross-format, order-dependent, verifier, PHI, ABI, or provenance inconsistencies; not isolated feature work.
---

# NeverD semantic-invariant debugging

Find the semantic question the pipeline answers inconsistently, give it one authoritative implementation, and verify the invariant across relevant callers. A fixture-only patch is insufficient when the same policy is duplicated across traversal, ABI recovery, loaders, architectures, or emitters.

## Topic routing

Read only what the current investigation needs:

- [Investigation and authoritative fix](references/investigation-and-fix.md) for reproducing, locating every policy owner, writing contract tests, and choosing the fix layer. It also contains patterns learned from previous NeverD failures; consult only the matching subsection.
- [Sibling audit and validation](references/sibling-audit-and-validation.md) for cross-architecture, cross-format, build-profile, LLVM-toolchain, and change-classification checks.
- [Acceptance and delivery](references/acceptance.md) when reviewing completeness or preparing the final handoff.
- [Re-materialized table bases](references/rematerialized-table-base.md) only for immutable table-base recovery and related pointer-provenance bugs.

Use current code, tests, and documentation as authority. Do not assume a previous fix pattern applies merely because symptoms look similar. Prefer the smallest failing semantic boundary, then broaden only to sibling surfaces that share the same decision.

Use `github-ci-runtime-debugging` when the defining evidence is runner-specific or intermittent CI runtime behavior. Use `systematic-debugging` for an isolated bug with no duplicated semantic owner.

## Completion

For diagnosis, provide the inconsistent question, its current owners, and the evidence. For a requested fix, centralize the policy, add meaningful regression coverage, run the narrow test first, and broaden validation in proportion to the affected semantic surfaces. Report skipped architectures, formats, or tools explicitly.
