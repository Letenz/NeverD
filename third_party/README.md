# Third-party sources

The repositories below are Git submodules of the NeverSight forks. Compatibility
fixes belong on each fork's default branch. NeverD changes belong on `dev`.

| Submodule | Current default branch | NeverD use |
| --- | --- | --- |
| `highway` | `master` | Bounded byte scanning |
| `simdutf` | `master` | UTF-8 validation |
| `simdjson` | `master` | Transport JSON admission |
| `oneTBB` | `master` | Optional task scheduling; no allocator proxy |
| `mimalloc` | `main3` | Default allocator for supported executables |

Configure with CMake, then run `python scripts/build.py BUILD_DIR`. Every invocation
resolves the forks' live default branches and updates these five submodules.
The script preserves uncommitted and unpublished divergent work by stopping.
Submit local fixes to the fork before using the mainline build entry point.
The recorded gitlinks provide checkout revisions; they do not pin mainline builds.

Compilation uses immutable exports under
`BUILD_DIR/third_party/<library>/<commit>/src`. Separate builds can therefore use
different commits without modifying sources under an active compiler. The script
serializes submodule updates and each build directory separately.
`forks.json` identifies the five repositories managed by this build policy;
`.gitmodules` remains the Git submodule configuration. Capstone, Unicorn, LLVM and
the other existing submodules retain their current revision policy.

`neverd-dependency-inputs.json` records configuration inputs and current licenses
come from those sources. `neverd-build-receipt.json` records the requested targets,
configuration and build result. An input list alone is not proof of binary or
test compatibility. See [build instructions](../CONTRIBUTING.md) and
[testing](../docs/testing.md#acceleration-libraries).
