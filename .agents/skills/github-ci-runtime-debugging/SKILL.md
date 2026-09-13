---
name: github-ci-runtime-debugging
description: Diagnose NeverD runtime tests that fail only or intermittently on GitHub Actions. Use for runner-specific crashes, signals, timeouts, or semantic mismatches; not ordinary local failures or workflow configuration errors.
---

# NeverD GitHub CI runtime debugging

Reproduce the failure at the smallest faithful boundary, identify the failing runtime layer, and validate the cause on the affected native runner. Read current repository configuration instead of relying on the snapshots in this skill.

## Start here

Inspect the failing run, its exact SHA, workflow, matrix leg, step, test case, owning binary, exit status, and last known green result. Read only the repository files that define that boundary. For the main native test workflow these commonly include `.github/workflows/ci.yml`, the failing fixture, and the relevant section of `docs/testing.md`; other workflows have different contracts.

A read-only diagnosis does not authorize reruns, debug commits, temporary workflows, or artifact uploads. Prepare the smallest useful external experiment before asking for any required authorization.

## Topic routing

- [Triage](references/triage.md) for NeverD CI facts, workflow-stage classification, nested runtime layers, signals, and skip interpretation.
- [Reproduction](references/reproduction.md) for faithful Release builds, CTest/GoogleTest commands, concurrency isolation, repetition, and platform matching.
- [Evidence and runner escalation](references/evidence-and-runner.md) for logs, cores, sanitizer artifacts, and bounded temporary workflow design.
- [Fix and validation](references/fix-and-validation.md) for regression boundaries, native flake evidence, broader CI checks, and common false conclusions.

Use `systematic-debugging` for ordinary local failures. Use `neverd-semantic-invariant-debugging` when evidence points to a shared semantic decision rather than a runner-specific runtime condition.

## Completion

For diagnosis, report the failing layer and evidence without implementing a fix unless requested. For a requested fix, continue through the smallest regression, affected native reproduction, and proportionate CI validation; state any platform or optional-tool coverage that remains unavailable.
