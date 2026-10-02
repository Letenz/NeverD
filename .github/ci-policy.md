# CI scheduling

NeverD and NeverC share the NeverSight organization runner pool. Standard
GitHub-hosted runner minutes are free for these public repositories, but the
Free plan has a shared limit of 20 concurrent jobs, including at most five
macOS jobs. Workflow cancellation does not limit the jobs inside a matrix.

## Builds and auxiliary validation

- Main CI keeps the newest run of each workflow/PR or workflow/ref. Linux,
  Windows, and macOS coverage remains enabled.
- Markdown-only pushes and pull requests skip full CI and mobile builds.
  The short LLVM Style workflow runs the documentation translation tests and
  documentation checker, including for Markdown-only changes.
- The mobile host matrix runs one job at a time; the desktop GUI and Python SDK
  matrices run at most two jobs at a time. Every existing matrix entry remains
  enabled. Source changes still exercise the full mobile regression matrix.
- EVM Upstream Audit and Prebuilt LLVM Audit run on changes to their relevant
  source, pins, scripts, or workflow definitions. Their existing scheduled and
  manual triggers continue checking upstream drift independently of pushes.
- Release qualification and publication workflows keep their existing policies.

## Optional GitHub scanning

CodeQL default setup and Code Quality analysis are disabled in this repository's
GitHub settings. These automatically generated workflows are controlled outside
`.github/workflows`; editing CI concurrency alone cannot disable them. Repository
administrators can re-enable them from Settings when scanning is needed.
