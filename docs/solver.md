**Languages**: [English](solver.md) | [简体中文](zh-CN/solver.md) | [繁體中文](zh-TW/solver.md) | [日本語](ja/solver.md) | [한국어](ko/solver.md) | [Français](fr/solver.md) | [Deutsch](de/solver.md) | [Español](es/solver.md) | [Italiano](it/solver.md) | [Русский](ru/solver.md) | [العربية](ar/solver.md)

# Bitvector proof backends

NeverD uses its built-in bitvector solver by default. Exact MBA derivation
remains independent of a general solver. Expression synthesis accepts a
candidate only after an equivalence proof; a counterexample or inconclusive
query retains the original expression.

<!-- i18n-section: builtin-comparisons -->

## Built-in comparison circuits

Comparisons wider than eight bits compare high halves first and use the low
halves when the high halves are equal. Small chunks use subtraction carries.
The shared encoder can reuse high-prefix gates when a partial-register update
changes only low bits. Signed comparisons still invert both sign bits. The
expression semantics and existing resource limits are unchanged; exhaustion
still returns `Unknown`.

`BitBlaster.WidePredicatesAgreeWithTheEvaluator` checks signed and unsigned
predicates against the expression evaluator at selected widths from 8 to 256,
including odd widths, boundary neighbours and significant bits above 64. It
also excludes incorrect output values. Partial-counter tests check full queries,
counterexample models and gate exhaustion. Native cached-comparison regressions
prove both comparison/update orders with all register and flag observations,
and reject a changed original loop body.

<!-- i18n-section: z3-build -->

## Optional Z3 build

Enabling Z3 downloads the pinned 4.13.3 source revision through CMake
`FetchContent` and builds a static library with NeverD. No system Z3 install
is required:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_Z3=ON -DNEVERD_BUILD_SOLVER_BENCH=ON
cmake --build build-release --target neverd NeverDSolverTests \
  NeverDSymbolicTests neverd-solver-bench --parallel 4
```

`NEVERD_Z3_PROVIDER=FETCH` is the default. The first configure downloads the
source; later builds reuse it under the build directory's `_deps`. Z3's CLI,
tests, examples, documentation and language bindings are not built. Its Python
source generators use NeverD's existing Python interpreter dependency.

For an installed library, select `-DNEVERD_Z3_PROVIDER=SYSTEM` and optionally
`-DZ3_ROOT=/path/to/prefix`. This mode fails if development files are missing;
it does not silently switch providers. Offline builds can supply a local source
checkout with `-DFETCHCONTENT_SOURCE_DIR_NEVERD_Z3=/path/to/z3`.

With `NEVERD_ENABLE_Z3=OFF` (the default), NeverD neither downloads, searches for,
nor links Z3. An explicit runtime request for an unavailable backend fails
without falling back.

<!-- i18n-section: synthesis -->

## Proof-gated expression synthesis

```sh
build-release/bin/neverd simplify --synthesize --solver=z3 \
  --solver-timeout-ms=1000 --json '(x >> 4) + ((x >> 2) >> 2)'
```

`--solver=builtin` is the default. Solver selection requires `--synthesize`;
the ordinary MBA simplifier does not invoke Z3. The Z3 timeout applies to each
check, excluding expression translation. Cancellation is cooperative, so this
is not a hard wall-clock deadline. Zero selects the 1000 ms default;
`--exhaustive` removes that limit. SAT conflict, propagation, and watch-visit
limits apply only to the built-in backend and are rejected with Z3. Z3 checks
increment the proof-query counter; the built-in SAT work counters remain zero.

The C API appends `solver_backend` and `solver_timeout_ms` to
`neverd_synthesize_options`. Size-bounded readers retain the built-in backend
for old callers. `neverd_solver_backend_available()` reports build capability.
Python exposes the same choice:

```python
from neverd_plugin import synthesize_expression

result = synthesize_expression(
    '(x >> 4) + ((x >> 2) >> 2)', solver='z3', solver_timeout_ms=1000
)
```

This selection currently covers expression synthesis. Concolic execution,
safety analysis, and existing IR optimization defaults retain their existing
solver policies. Internal users can supply the Z3 verifier through the
semantic simplifier's proof callback.

<!-- i18n-section: checks -->

## Independent checks and query export

`NeverDSolverTests` includes independent Z3 expression evaluation and
cross-backend checks when Z3 is enabled. These construct reference expressions
directly, so a bug in NeverD's expression builders cannot simplify away both
sides of the test. Disabled builds explicitly skip those oracle cases and
still test the unavailable-backend contract.

```sh
build-release/bin/NeverDSolverTests --gtest_brief=1
build-release/bin/neverd-solver-bench \
  tools/neverd-bench/solver-corpus.txt --width=32 --repeat=5 \
  --backend=both --timeout-ms=1000 --max-conflicts=10000 \
  --dump-dir=/tmp/neverd-queries > /tmp/neverd-solver-results.json
```

Each non-comment input line is `original ; candidate`. A line without a
semicolon uses the MBA simplifier's result as the candidate. The tool reports
verdicts, model replay, and timings including session construction, translation,
and solving, but excluding parsing, MBA simplification, export, and teardown. Each
repetition uses a fresh solver. SAT models must reproduce the difference in the
expression evaluator. Opposing decisive verdicts, invalid queries, or invalid
models fail the run; `unknown` is recorded and is not an equivalence proof.

Exported SMT-LIB contains the original DAG, permanent assertions and the last
query's assumptions. It can be replayed with `z3 query-N.smt2`. Resource limits
and solver versions should be recorded alongside performance results; the two
backends use different budget units, so these are bounded workload comparisons,
not identical-work comparisons.

Bitvector proofs use the expression language's total fixed-width semantics.
Machine exceptions, memory effects, and LLVM poison remain the responsibility
of the lifting and translation boundaries; an expression proof does not certify
those boundaries.
