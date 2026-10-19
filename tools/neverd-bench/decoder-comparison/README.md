# x86 decoder comparison

This standalone experiment uses public Capstone, Zydis, XED and iced APIs.
It is not part of NeverD's product build and does not add product dependencies.
Use the NeverSight Capstone fork for both the baseline and candidate.
Build every library for the same host, optimization level and runtime. A host
running ARM64 code can decode x86 bytes; its timings are not x64-host timings.

## Build

Supply local static libraries. Record full Git SHAs, dirty diffs, compiler
versions, flags and CPU model in a JSON metadata file; do not compare release
and debug builds or reuse timings after an input changes. The four decoder
sources are [Capstone](https://github.com/NeverSight/capstone),
[Zydis](https://github.com/zyantific/zydis),
[XED](https://github.com/intelxed/xed) and [iced](https://github.com/icedland/iced).
Keep their source and license notices with any redistributed benchmark binary.

Build Capstone and Zydis with CMake Release. Disable unused Capstone
architectures in both baseline and candidate equally, and initialize Zydis's
Zycore submodule. Build XED with its mbuild tool, `--opt=3 --no-encoder install`.
Use matching C/C++ compilers and avoid enabling LTO or host-specific flags in
only one library.

Build `iced.rs` as a Rust static library with a temporary Cargo manifest:

```toml
[package]
name = "iced_compare"
version = "0.1.0"
edition = "2021"
[lib]
crate-type = ["staticlib"]
path = "/absolute/path/to/decoder-comparison/iced.rs"
[dependencies]
iced-x86 = { path = "/absolute/path/to/iced/src/rust/iced-x86", default-features = false, features = ["std", "decoder", "intel", "instr_info"] }
[profile.release]
opt-level = 3
panic = "abort"
```

Run `cargo build --release --manifest-path <manifest>`, preserving Cargo.lock
and the Rust toolchain version in the experiment record. Use forward slashes
in Windows TOML paths. Configure this directory separately with:

```sh
cmake -S tools/neverd-bench/decoder-comparison -B benchmark-results/decoder-base \
  -DCMAKE_BUILD_TYPE=Release \
  -DCAPSTONE_SOURCE=<capstone-root> -DCAPSTONE_LIBRARY=<static-library> \
  -DZYDIS_SOURCE=<zydis-root> -DZYDIS_LIBRARY=<static-library> \
  -DZYCORE_LIBRARY=<static-library> -DXED_KIT=<xed-install-root> \
  -DXED_LIBRARY=<static-library> -DICED_LIBRARY=<rust-static-library>
cmake --build benchmark-results/decoder-base --config Release
```

Repeat in a separate build directory with the candidate Capstone archive;
keep every other input identical. For MSVC, use matching MSVC-ABI libraries
and Rust's `*-pc-windows-msvc` target, not GNU libraries. This tool provides
Windows process CPU accounting and binary snapshot output; native Windows
execution still needs validation on the selected toolchain and host.

## Measure

Create a corpus manifest containing objects such as
`{"name":"scalar64","path":"inputs/scalar64.text","bits":64}`.
Paths are relative to the manifest. Use a mix of independently compiled code,
SIMD code, 16/32/64-bit modes and real executable text sections. Do not select
only samples where the candidate wins. Text sections can contain data, so
acceptance differences must remain visible.

```sh
python tools/neverd-bench/decoder-comparison/measure.py \
  --baseline <baseline-decoder-compare> --candidate <candidate-decoder-compare> \
  --manifest <corpus.json> --metadata <build-inputs.json> \
  --out benchmark-results/decoder-comparison --samples 7
```

The runner warms every case, rotates execution order, calibrates to at least
100 ms of baseline CPU work, hashes inputs/binaries, and records raw CPU/wall
times. Keep other builds and tests idle. Report paired ratios and dispersion;
do not infer small gains from noisy wall-time medians. CPU time uses process
accounting, including `GetProcessTimes` on Windows. No speed threshold is a
correctness test.

| Mode | Capstone | Zydis | XED | iced |
| --- | --- | --- | --- | --- |
| scan | iter, text/detail off | DecodeInstruction | decode | decode_out |
| format | iter, text on/detail off | DecodeFull + Intel formatter | decode + Intel formatter | decode_out + Intel formatter |
| detail | text/detail on, regs_access, operands/flags/groups | full operands/actions/CPU flags/category + formatter | operand, register, memory, flags/category queries + formatter | InstructionInfoFactory, operands/registers/memory/flags/flow + formatter |

These APIs do different amounts of work and expose different metadata. Equal
instruction boundaries do not prove equivalent semantics or identical work.
XED still decodes operand information in scan mode. Every backend advances
one byte after rejection. Setup is once per batch: Capstone/Zydis handle setup
is inside the measured function; iced state and XED tables are initialized
outside it. Rust crosses the C ABI once per batch, not per instruction.

The timing checksum consumes results and checks baseline/candidate stability;
it is **not** a semantic equivalence proof. Compare public fields separately
using `decoder-snapshot bits att text detail every_offset input`. Each boolean
argument is 0 or 1. Redirect baseline and candidate output into separate
binary files and compare bytes; counts are printed on stderr. This covers
text, IDs, instruction extent, operands, access, registers, flags, groups,
encoding offsets and x86 decorations without comparing struct padding.
Use every-offset probes for malformed/truncated data and run the fork's x86
unit/YAML tests as well. An unchanged snapshot can preserve a pre-existing
bug; it does not replace ISA-based correctness tests.

Keep raw reports and corpora in ignored build or benchmark-results directories.
Do not equate decoder microbenchmarks with NeverD's full analysis latency,
and do not rank different-acceptance samples as equivalent-work results.
