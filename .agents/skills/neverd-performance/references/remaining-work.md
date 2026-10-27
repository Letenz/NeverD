# Performance: remaining work

Basis: `origin/dev` 159588db82 (2026-10-27). Each item names its entry
condition. Do not start an item until that condition is met with a profile or
same-base A/B; see `docs/development-principles.md` § Speculative defenses.

## Retained decisions to re-measure

| Decision | Where | Revisit when |
| --- | --- | --- |
| GCC keeps rank-based membership | `include/neverd/support/Limits.h:77-83` | GCC caller measurements show direct membership no longer slows small scalar loops |
| Non-Clang compilers enter SIMD search at 4096 keys | `include/neverd/support/Limits.h:72-75` | A GCC or MSVC A/B shows medium ordered queries gain |
| x86 targets with fewer than four u64 lanes stay scalar | `include/neverd/support/Limits.h:84-85` | SSE2-SSE4 gain consistently across query orders |

## Candidates from `docs/bbtree-evaluation.md`

- **Inline string prefixes** for large symbol-name indexes. Entry: name-index
  lookups appear in a profile. Prefix matches still require a full-key tie
  check.
- **Interleaved batch traversal.** Entry: a caller that already holds
  independent query batches shows lookup cost, for example the batched runtime
  entry-point name lookups (`a89f54a3d3`).
- **Read-only mapped index files.** Entry: a persisted-format and lifetime
  contract. This cannot transparently replace mutable `BinaryImage` metadata.

## Unexplored; profile first

- **Integer-keyed `std::map`/`std::set`** that is built once and then only
  queried. Move it to a sorted vector with `neverd::sorted::*`, or to
  `DenseMap` for exact keys. Entry: the container appears in a profile. Keep
  iteration order wherever output depends on it.
- **Interval-shaped address data.** Evaluate `llvm::IntervalMap` (no uses
  yet). Entry: interval lookups or coalescing appear in a profile.
- **Sorting.** VQSort (Highway contrib, not built) and `tbb::parallel_sort`
  (vendored, unused) are available. Entry: sorting is a significant share of a
  profile. Output must not change, including tie order. A parallel sort needs
  a size threshold in limits.
- **Build.** ThinLTO and PGO are unevaluated. Entry: a whole-request A/B on
  Linux, macOS and Windows that also reports build time and binary size. Keep
  the generic ISA baseline; runtime dispatch covers SIMD.
- **JSON.** Transport keeps the nlohmann DOM, and simdjson On-Demand is unused.
  Entry: JSON parsing or DOM copying is significant in a worker or MCP request
  profile.
- **`std::regex`** in `tools/neverd/mobile/*` and
  `tools/neverd-worker/CodeEdits.cpp`. Entry: a regex runs on a per-item path
  in a profile.
- **Phase-scoped arenas** beyond the jump-table resolver. Entry: allocation is
  significant in a profile with mimalloc enabled.
- **A regression guard.** Extend the opt-in benchmark targets and the bounded
  `neverd decompile ... --max-func` run (see `SKILL.md` § Measuring) into a
  pinned-corpus run. Entry: a runner with stable hardware counters. Until
  then, the same-base A/B is the guard.

## Rejected

`docs/bbtree-evaluation.md` gives the reasons:

- The bbtree Rust container, and any FFI boundary to it.
- Wholesale replacement of LLVM containers.
- GPU offload for single queries.
- Software prefetch, retained caches or per-query allocation in sorted search.
- SSE2-SSE4 SIMD for sorted search.
- The variable-tail search prototype: it regressed random queries.
