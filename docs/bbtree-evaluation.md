# bbtree evaluation

The reviewed repository is
[XiangpengHao/bbtree at 8adae97a](https://github.com/XiangpengHao/bbtree/tree/8adae97a0ed97e5c98fa2790312eb0aec9719846).
Its README's Rust container comparisons are not measurements of NeverD.

| Idea | Fit for NeverD |
| --- | --- |
| Dense keys, independent comparisons and rank counting | Applicable to the immutable address vectors in `ExecutableCodeOwnerIndex`. No new index allocation is needed. |
| Inline string prefixes | Potentially useful for large symbol-name indexes, but prefixes require full-key tie checks. No measured NeverD win yet. |
| Interleaved batch traversal | Worth evaluating where callers already have independent query batches. Current single-address ownership queries do not provide that interface. |
| Mutable B-tree replacement | The Rust API is not a C++ container replacement. An FFI boundary, ownership changes and extra dependency are not justified by the current evidence. |
| Read-only mapped index files | Requires a persisted-format and lifetime contract. It is not a transparent replacement for publicly mutable `BinaryImage` metadata. |

The relevant source discussions are the
[cost model](https://github.com/XiangpengHao/bbtree/blob/8adae97a0ed97e5c98fa2790312eb0aec9719846/design/06-cost-model.md),
[inline-key design](https://github.com/XiangpengHao/bbtree/blob/8adae97a0ed97e5c98fa2790312eb0aec9719846/design/04-inline-key.md), and
[batch API](https://github.com/XiangpengHao/bbtree/blob/8adae97a0ed97e5c98fa2790312eb0aec9719846/design/12-batch-api.md).
The implementation is original C++ using NeverD's existing Highway dependency;
no bbtree source or Rust runtime is incorporated. bbtree is dual MIT/Apache-2.0
licensed. No LLVM fork or dependency revision change is required.

## Cost model and implementation

`neverd::sorted::lowerBound64` keeps small arrays on the inline standard-library
path. For larger arrays it narrows a sorted interval, then counts unsigned keys
below the query in a bounded, contiguous tail using CPU SIMD. The fixed tail
replaces dependent probes with independent comparisons. Runtime dispatch and
the extra comparisons still cost time, so a vector instruction alone is not
evidence of an improvement.

Clang narrowing retains the pivot on the right with a query-independent interval
length. Other compilers retain standard bisection so ordered inputs can benefit
from predictable branches. A complete tail is extended right or shifted left at EOF; sorted order
preserves the absolute rank. All loads stay within the original array. Scalable
vectors use a scalar remainder when their lane count does not divide the tail.
Duplicates, empty input, unaligned arrays and the complete unsigned address
range retain standard lower-bound semantics.

The first variable-tail prototype regressed random queries. Disassembly showed
key-dependent branches. Clang's
[`__builtin_unpredictable`](https://clang.llvm.org/docs/LanguageExtensions.html#builtin-unpredictable)
must appear directly in the conditional expression for this implementation to
retain conditional moves; assigning the hinted value to a boolean temporary
did not change the generated x86 code. A fixed tail then removed the variable
loop/remainder overhead. Rejected prototypes are not production paths.

The policy values live in `support/Limits.h`; Highway owns ISA feature dispatch.
Clang admits the SIMD path from 64 keys; other compilers use a conservative 4096
key threshold to keep medium ordered queries on the scalar path. x86 targets
with fewer than four 64-bit SIMD lanes retain standard lower bound: SSE2,
SSSE3 and SSE4 experiments did not show consistent gains across query orders.
There are no prefetches, retained caches, locks or per-query allocations. The
foundation target is independent of LLVM and the loader, avoiding a dependency
cycle through `NeverDSupport`.

## Measurement scope

The benchmark validates every result against a standard-library reference and
reports wall and process CPU time. Parsing, reference computation, allocation
and index construction are untimed. Inputs include synthetic arrays of 16, 256
and 4096 keys, plus 44,183 defined dynamic function addresses from the LLVM 23
ELF shared library (SHA-256
`7d53c9d91d53ab56c3cef98a64b74acad14a7f72acffe5e0903fed2bf95061e8`).
Queries alternate exact addresses and address-plus-one probes, in seeded random
and sorted order. These are synthetic query traces over real addresses, not a
recorded decompilation trace.

Local measurements use an i9-13900H, a Release build and CPU 0 affinity, with
alternating process order. No own build, test or profiler overlaps timing.
Other users' builds may contend; hardware performance counters are unavailable
under the host's existing policy. No kernel settings were changed. Small
control cases help expose scheduling and frequency noise. These component
measurements do not establish whole-CLI/GUI or IDA/radare2 superiority.

GPU offload has no demonstrated benefit for this single-query path. A future
experiment would need a resident batch workload and must include transfer,
submission and synchronization cost. This change uses CPU SIMD and adds no GPU
runtime dependency.

See [acceleration validation](testing.md#acceleration-libraries) for the
standalone tests, optional benchmark target and native/scalar configurations.

The local input generation is reproducible with Python's `random.Random` seed
20261025. Extract `nm -D --defined-only -n LLVM_LIBRARY` into `llvm-symbols.txt`,
then run:

```python
from pathlib import Path
import random

out = Path("search-inputs")
out.mkdir(exist_ok=True)
real = sorted({int(p[0], 16)
               for line in Path("llvm-symbols.txt").read_text().splitlines()
               if len(p := line.split()) >= 3 and p[1] in "TtWwiI"})
def write(name, values):
    (out / name).write_text("".join(f"{value:x}\n" for value in values))
for name, keys in [("small", [(1 << 63) + i * 32 for i in range(16)]),
                   ("medium", [(1 << 63) + i * 32 for i in range(256)]),
                   ("large", [(1 << 63) + i * 32 for i in range(4096)]),
                   ("llvm", real)]:
    rng = random.Random(20261025)
    queries = [keys[rng.randrange(len(keys))] + (i % 2)
               for i in range(1048576)]
    write(name + "-keys.hex", keys)
    write(name + "-mixed-queries.hex", queries)
    write(name + "-ordered-queries.hex", sorted(queries))
```

## Measured results

The complete [evidence record](evidence/bbtree-search-2026-10-25.json) contains input/source/binary hashes, dependency revisions, all samples and test inventories. Negative changes mean less CPU time.

| Keys / query order | Clang lower bound | GCC lower bound | Clang actual owner call |
| --- | ---: | ---: | ---: |
| small-mixed | +8.8% | +3.5% | +5.5% |
| small-ordered | +3.3% | +6.9% | -1.9% |
| medium-mixed | -5.3% | +5.1% | -4.3% |
| medium-ordered | -8.5% | +26.4% | -3.4% |
| large-mixed | -10.7% | -28.1% | -3.2% |
| large-ordered | -3.8% | +5.7% | -1.8% |
| llvm-mixed | -22.4% | -15.2% | -20.4% |
| llvm-ordered | +2.3% | -2.9% | +1.7% |

The real `BinaryImage::hasFunctionSymbolAt` call over the 44,183-address random trace changed from 55.41 to 44.10 ns/query (process CPU medians). This is the primary integration evidence. Tiny inputs retain scalar search, but admission checks still have a cost; ordered controls are not uniformly faster. The full record includes wall times, which were noisier under external contention.

Local checks passed: Clang 22, GCC 22, ASan/UBSan 19, scalar/system allocator 19, installed acceleration 22, installed scheduler 2, and code-owner integration 10 cases, all without failures or skips. ARM64 emulation passed all 3 sorted-search cases for NEON and for 384-bit SVE/SVE2. Native CI covers six ISA/OS combinations with eight compiler configurations; native results are not claimed here.

The benchmark uses `GetProcessTimes` on Windows because [Microsoft documents that CRT `clock()` measures wall time](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/clock). On Unix it uses the process CPU clock exposed by `std::clock()`. The empty-array contract follows [C++20 pointer arithmetic](https://timsong-cpp.github.io/cppwp/n4861/expr.add) and is covered by explicit null-input and sanitizer checks.
