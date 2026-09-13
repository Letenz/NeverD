# Acceptance

## Root-Cause Acceptance Checklist

Do not call the issue root-fixed until all applicable items are true:

- [ ] The exact failure reproduces on the pre-fix revision.
- [ ] A focused test fails for the predicted semantic reason.
- [ ] All equivalent decision sites have been enumerated.
- [ ] One authoritative policy replaces the divergent implementations.
- [ ] Ordering and negative counterexamples pass.
- [ ] Relevant architectures and formats were tested or explicitly ruled out
      with evidence.
- [ ] The original fixture passes through every affected stage.
- [ ] Safety diagnostics remain enabled.
- [ ] Relevant broad regression suites pass with skips identified.
- [ ] Broad runs completed in a verified build profile; interrupted runs were
      not counted.
- [ ] LLVM IR/object validation used the repository-matched toolchain or any
      host-version mismatch was stated explicitly.
- [ ] The final explanation distinguishes regression, latent defect, and
      incomplete prior fix.

## Anti-Patterns

Reject fixes that:

- special-case a function name, symbol, address, section name, or fixture;
- reorder PHIs so the desired value happens to appear first;
- choose the widest value without checking entry definedness;
- weaken or remove the ambiguity verifier;
- add a stale-address fallback;
- patch only the reported file format when the decision is shared;
- duplicate the same selector in another caller; or
- add broad refactoring unrelated to the proven invariant.

## Delivery

Keep one issue and its regression coverage in one focused commit when the user
has requested that workflow. Push, close issues, or mutate external state only
when authorized. A public issue comment should state the root cause, shared fix,
and validation scope without exposing private internal documentation.

The final handoff should answer four questions directly:

1. Was the report a real bug?
2. Was it a new regression, a latent defect, or an incomplete earlier fix?
3. Which sibling architectures, formats, and paths were checked?
4. What prevents the same semantic rule from diverging again?
