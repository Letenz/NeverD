# NeverD agent guide

Keep repository-wide instructions small. Read the documentation that matches the task:

- Use `docs/architecture.md` when changing component boundaries or shared pipeline ownership.
- Use `docs/testing.md` to choose build profiles, targets, labels, and semantic test coverage.
- Use `CONTRIBUTING.md` for public API, dependency, attribution, formatting, and pull-request conventions.
- Use `docs/roadmap.md` only when prioritizing or scoping planned product work.

NeverD is semantics-first. Unsupported instructions and malformed inputs must fail clearly; do not silently guess semantics or replace unsupported behavior with a `NOP`. Put a semantic decision in one authoritative layer and keep loaders, architectures, and emitters consistent with it.

The local build and test fixtures are disposable and have no production access. Run the smallest check that can disprove the change, fix failures caused by the requested work, and rerun affected checks without asking after each step. Broaden testing when the change crosses a shared semantic boundary, affects an architecture or binary format, or is being prepared for review; report unavailable tools and skipped coverage as limitations.

For requested implementation work, continue through the implementation, focused verification, and fixes for failures caused by the change. Stop earlier only when the user asked for diagnosis or review, or when completion requires a product decision, credentials, external mutation, or other authority that is not already present.

Preserve unrelated working-tree changes and local diagnostic artifacts. Do not reformat untouched files or change submodule revisions unless the task requires it.
