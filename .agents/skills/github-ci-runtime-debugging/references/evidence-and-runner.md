# Evidence And Runner

## Step 7: Preserve evidence

Always retain:

- `ctest.log`;
- `build-ci/Testing/Temporary/LastTest.log` and `LastTestsFailed.log` when present;
- the verbose CTest command and exact GoogleTest filter;
- compiler, linker, CMake, OS, architecture, CPU-count, and LLVM-mode details;
- debugger backtrace or sanitizer report;
- output hashes and the command used to produce them.

Lift and CLI fixtures preserve failure directories named like `nd_test_*`/`nd_e2e_*` in the host
temporary directory. Inspect their `_stdout.txt` and `_stderr.txt`.

Semantic roundtrip work directories are deleted by `WorkGuard` on every exit path, including
assertions and skips. If those intermediates are essential, make an explicitly temporary diagnostic
change that preserves or uploads the per-test directory; do not assume the current fixture retained
it.

For Linux core dumps on a disposable debug runner:

```bash
ulimit -c unlimited
echo "$PWD/core.%e.%p" | sudo tee /proc/sys/kernel/core_pattern
```

Run the smallest reproducer, then symbolize with the matching executable:

```bash
gdb -batch \
  -ex 'thread apply all bt full' \
  -ex 'info registers' \
  build-ci/bin/<NeverDTestBinary> <core-file>
```

Core dumps may contain sensitive process data. Upload only what is needed and only to an
appropriately protected workflow run.

Keep sanitizer builds separate from `build-ci`. The documented SBF ASan/UBSan profile in
`docs/testing.md` uses prebuilt LLVM, requires `-fno-rtti`, and has a stated integration-test
packaging boundary; do not generalize it blindly to the integrated-LLVM Release shard.

## Step 8: Use a real runner only when needed

If local native reproduction is unavailable, or Docker/QEMU passes while the real runner fails, add
a minimal temporary `workflow_dispatch` workflow only with user authorization.

The debug workflow should:

1. hard-code one affected runner leg;
2. copy checkout, dependency, compiler, CMake, and LLVM-mode details from current `ci.yml`;
3. build only the owning target where possible;
4. repeat one exact test before adding parallel stress;
5. capture backtraces/logs even when the test step fails;
6. upload minimal evidence with `if: always()` using the repository's currently pinned
   `actions/upload-artifact` revision;
7. use minimal permissions and a bounded `timeout-minutes`.

Because the normal CI has no native artifact, a debug workflow must build the reproducer itself or
debug a newly instrumented run. Do not claim it is using the original failed run's exact binary.

Remove the throwaway workflow and diagnostic-only code after the investigation.
