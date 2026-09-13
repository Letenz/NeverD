# Reproduction

## Step 3: Reproduce the exact CI configuration

Start from the failing SHA and initialize every submodule:

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

Use a fresh build directory. For a faithful Linux/macOS reproduction:

```bash
PYTHON=python3
PYTHON_EXECUTABLE="$("$PYTHON" -c 'import sys; print(sys.executable)')"
cmake -S . -B build-ci -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DBUILD_TESTING=ON \
  -DNEVERD_BUILD_PLUGINS=ON \
  -DNEVERD_ENABLE_PYTHON_PLUGINS=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON \
  -DPython3_EXECUTABLE="$PYTHON_EXECUTABLE" \
  -DNEVERD_LLVM_PREBUILT=OFF
cmake --build build-ci --config Release --parallel <matrix-parallel>
```

On Windows, run from an MSVC x64 developer environment and use `cl` for both C and C++. The current
runner configures, builds, and tests through Bash after enabling MSVC; preserve that shell boundary
when reproducing quoting or child-process failures.

`-DNEVERD_LLVM_PREBUILT=ON` is a valid fast-triage variant on the currently published host set, but
label the result as non-faithful when the failed CI run used `OFF`. Never reuse one build directory
while switching LLVM mode or build type.

Reproduce only the owning target first:

```bash
cmake --build build-ci --target <NeverDTestBinary> --parallel <matrix-parallel>
ctest --test-dir build-ci --build-config Release \
  -R '^<exact-ctest-name>$' \
  --output-on-failure --parallel 1
```

For repetition in fresh CTest child processes:

```bash
ctest --test-dir build-ci --build-config Release \
  -R '^<exact-ctest-name>$' \
  --repeat until-fail:100 \
  --output-on-failure --parallel 1
```

For direct GoogleTest debugging, copy the exact command shown by `ctest -N -V`:

```bash
build-ci/bin/<NeverDTestBinary> \
  --gtest_filter='<Suite.Case>' \
  --gtest_repeat=100 \
  --gtest_break_on_failure
```

Use `.exe` on Windows when needed.

## Step 4: Separate the two concurrency dimensions

First isolate the exact case, then restore enough neighboring cases to make CTest launch concurrent
processes. `--parallel N` has no effect when the selection contains only one case.

| Run | Selection | CTest `--parallel` | `NEVERD_THREADS` | What it isolates |
|---|---|---:|---:|---|
| A | exact case | 1 | 1 | Logic/platform baseline |
| B | exact case | 1 | default | NeverD internal pipeline concurrency |
| C | owning label or representative case set | matrix value | 1 | Multi-process/tool/resource pressure |
| D | original CI profile | matrix value | default | Exact CI stress |

Example:

```bash
NEVERD_THREADS=1 ctest --test-dir build-ci --build-config Release \
  -R '^<exact-name>$' --repeat until-fail:100 --output-on-failure --parallel 1
```

Interpretation:

- A fails identically: prioritize deterministic logic, ABI, codegen, or platform behavior.
- A passes and B fails: investigate NeverD's internal parallel phases and shared state.
- B passes and C fails: investigate process/FD/memory pressure, temp names, child tools, and CTest
  concurrency.
- Only D fails: inspect multiplicative thread/process pressure and cross-process resources first.

`scripts/run_semantic_parallel.sh` is useful for large semantic stress and checks for killed shards,
but it is hard-coded to `build/`, not `build-ci/`. Do not use it as proof of CI fidelity without
first reconciling that build configuration. It also calls `make`, so a Ninja-configured `build/`
will fail there. `ND_NO_BUILD=1` can silently reuse a stale binary.

## Step 5: Test determinism and resource sensitivity

For a flaky failure:

1. Repeat the exact case enough times to estimate frequency.
2. Compare the failing case and phase across runs.
3. Record whether the process printed its final GoogleTest summary.
4. Compare A/B/C/D from the concurrency grid.
5. Check runner memory, process count, disk space, and timeout before changing code.

After excluding a workflow-level cancellation, different cases dying on different runs, exit 137,
missing final summaries, or repeated
`link failed after retries (transient infra)` strongly suggests resource/process pressure. The
semantic fixture already uses fixed test inputs and retry logic; do not call such failures
"random-input bugs" without evidence.

For suspected nondeterministic code generation, emit the same Release output repeatedly and hash the
behaviorally relevant bytes. Exclude debug sections, paths, timestamps, UUIDs/build IDs, and other
known metadata before concluding that code generation differs. Then compare the first differing
section or IR stage, not only the whole-file hash.

When a stable reproduction exists, change one variable at a time: optimization level, LLVM mode,
`NEVERD_THREADS`, CTest parallelism, host architecture, or tool version.

## Step 6: Match the failing platform

### macOS arm64

An Apple Silicon development host matches the current macOS CI architecture. Match the CI compiler,
Release mode, LLVM mode, and parallelism. Debug a direct test command with LLDB:

```bash
lldb -- build-ci/bin/<NeverDTestBinary> --gtest_filter='<Suite.Case>'
```

Inside LLDB, use `run` and `thread backtrace all`. A Debug-only success does not clear a
Release-only bug; use a separate RelWithDebInfo directory if symbols are needed.

### Linux x64 from Apple Silicon

Use an amd64 Ubuntu 24.04 container for first-pass environment matching:

Initialize the host checkout recursively before mounting it read-only; the container cannot repair
missing LLVM, Capstone, Unicorn, signatures, or corpus submodules.

```bash
docker build --platform linux/amd64 -t neverd-ci-repro - <<'EOF'
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y \
    build-essential clang cmake gdb lld ninja-build \
    python3 python3-dev git ca-certificates
EOF

docker run --rm --platform linux/amd64 --cpuset-cpus=0-3 \
  -v "$PWD":/src:ro \
  -v neverd-amd64-build:/build \
  -v neverd-amd64-llvm-cache:/root/.cache/neverd-llvm \
  neverd-ci-repro \
  bash -lc 'PYTHON_EXECUTABLE="$(python3 -c "import sys; print(sys.executable)")"; \
    cmake -S /src -B /build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DBUILD_TESTING=ON \
    -DNEVERD_BUILD_PLUGINS=ON -DNEVERD_ENABLE_PYTHON_PLUGINS=ON \
    -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON \
    -DPython3_EXECUTABLE="$PYTHON_EXECUTABLE" \
    -DNEVERD_LLVM_PREBUILT=ON && cmake --build /build --parallel 4'
```

This intentionally uses prebuilt LLVM for fast triage. Switch to `OFF` only when integrated LLVM is
part of the hypothesis and the much slower emulated build is justified.

QEMU can hide real scheduling, CPU-feature, alignment, and timing bugs. A pass under emulation does
not clear a native Linux x64 failure.

### Windows x64

Use a native MSVC x64 environment for final reproduction. Preserve the workflow's `cl` + Ninja
combination. Wine or cross-linking may test file generation but cannot clear a native Windows
runtime or command-quoting failure. Investigate `NeverDTestProcessTests` when paths, quoting, or
child exit codes differ only on Windows.

Use Visual Studio or WinDbg for an access violation. Verify debugger/dump-tool availability on a
runner before writing a workflow around it.
