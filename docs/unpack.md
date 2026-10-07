**Languages**: [English](unpack.md) | [简体中文](zh-CN/unpack.md) | [繁體中文](zh-TW/unpack.md) | [日本語](ja/unpack.md) | [한국어](ko/unpack.md) | [Français](fr/unpack.md) | [Deutsch](de/unpack.md) | [Español](es/unpack.md) | [Italiano](it/unpack.md) | [Русский](ru/unpack.md) | [العربية](ar/unpack.md)

# Unpacking packed executables

`neverd unpack` recovers the program that a packed executable rebuilds in its own address space. It runs the input as a bounded guest process, observes where the stub hands control to the code it produced, and writes that image as a new file of the same container. It does not devirtualize: functions a protector virtualized stay virtualized. Build with `NEVERD_ENABLE_CPU_EMULATION=ON`.

## Supported inputs

The container selects how a file is validated and rebuilt, the instruction set selects how a transfer is judged, and both select the guest process profile. An input outside this table is rejected by name before anything executes.

| Container (`format`) | Instruction set | Guest profile | Entry evidence |
| --- | --- | --- | --- |
| PE32+ (`pe64`) | x86-64 | [`windows-pe64-v1`](process-emulation.md) | runtime observation |
| PE32+ (`pe64`) | ARM64 | [`windows-pe64-v1`](process-emulation.md) | runtime observation |

## Use

```bash
neverd unpack packed.exe -o unpacked.exe
neverd unpack packed.exe -o unpacked.exe \
  --options='{"backend":"unicorn","instruction_limit":400000000,"transfer":2}'
```

The command prints one JSON report. Exit code 0 means the image was written, 3 means the bounded run ended before an entry was accepted (`outcome` is `no_entry` and nothing is written), and 1 is an invalid input, option or setup failure. The report names the `format`, the `architecture` and the `profile` that ran. The C entry point is `neverd_unpack_json`; Python exposes `Session.unpack`. Options are the [process options](process-emulation.md) plus `transfer`. Defaults differ where a stub needs more room: 100000000 instructions, 600 seconds and 512 MiB, and `windows.defer_unmodeled` is on.

## How the entry is established

Generation zero is the image as the guest loader mapped it. A transfer starts execution of code newer than the code that was running. `transfers` records the RVA, `generation`, stack equality (`stack_balanced`) and whether execution belongs to the main program's entry invocation (`program_invocation`). Before that invocation starts, stack equality uses the first initializer's stack.

1. The default entry requires both `stack_balanced` and `program_invocation`: the stub has returned the stack of the main entry invocation. `entry_source` is `transfer`.
2. OS-owned TLS and DLL callbacks are reported but cannot become the default entry, even with a balanced stack. Deeper guest calls also continue. No callback is skipped and no stub signature predicts an entry.
3. `transfer` explicitly selects a listed transfer by position, including initialization callbacks or staged transfers.

No entry is predicted from the shape of compiler startup code. A run that stops first reports `no_entry` with the process `stop_reason`.

## The rebuilt image

Sections keep their RVAs and hold the observed memory, including effects of initializers that already ran; each section has its observed page access. A final `.neverd` section holds a new import directory over existing export cells. Additional cells needed for observed export calls and address loads live in that new section, so original zero-filled storage is preserved. `imports` lists every cell with its `origin`: `static` cells were bound from the input directory, `runtime` cells were stored by guest code or added for a repaired call. The image is fixed at its observed base: relocations of generated content were not observed, so the relocation directory is removed and `IMAGE_FILE_RELOCS_STRIPPED` is set.

Existing runtime IAT candidates must form a contiguous export-pointer array for one provider with an intact zero terminator inside the original section. A neighboring provider's terminator cannot validate another array.

A replaced TLS directory is recovered from the loader's allocation identity, a complete terminated callback list and observed calls into generated code. Multiple matching records fail explicitly. This rule uses PE metadata and execution witnesses, without protector-specific bytes or names. Generated callback execution takes precedence over a completed loader initializer when selecting the directory.

`materialized_tls_callbacks` counts main-image TLS callbacks whose OS-owned process-attach calls returned before the captured program invocation. Their memory effects are already in the snapshot. Adapters in the new `.neverd` section bypass only that repeated process-attach call and tail-call the original callback for other reasons. The directory, callback table and adapters use new storage; original bytes stay intact. The section is executable when it contains adapters and writable only when new IAT cells require it. Guest-internal callbacks without this completion evidence keep their original behavior. On process attach, the first adapter also restores captured main-thread TLS bytes when they differ from the template. Missing or mismatched live TLS evidence fails explicitly. The original template remains intact for future threads; other notification reasons never restore the captured block.

Deferred loading permits executable callback and entry targets that earlier initializers materialize in zero-filled memory. Callback arrays and TLS allocation metadata still require validated backing; ordinary strict loading retains its file-backing checks. The OS model supplies invocation provenance and notifies observers when it prepares an invocation or restores a suspended caller. Transfer watches are rearmed at those boundaries, including when a callback and the generated entry share a page.

`import_repair` covers two additional bounded runs after entry capture: export-call discovery, then helper-state observation. Each run has its own process limits. Instruction/event counts are their saturating sum; `stop_reason` and the diagnostic describe the last run, and observed calls count discovery only. If discovery finds no continuations, observation is skipped. Conflicting export identities cannot rewrite a site. Results cover reached paths; `unpacked` does not certify all imports or successful program execution.

Import binding order can change export addresses. Repair retains the identity from the proving run and observes exports resolved after entry; an address from another run cannot authorize a rewrite.

Discovery observes export dispatch, including the unmodeled export that stops execution, rather than relying on modeled-call logs. An unreadable ABI return location cannot seed a helper proof. Repairing a pure call to an opaque export preserves the explicit unsupported-service stop.

Observation examines at most 256 candidate starts in the 256 bytes preceding actual export-call continuations. A direct `CALL rel32`, optionally preceded by a GPR PUSH or POP, identifies a boundary to observe; bytes after CALL are not a signature. A witnessed six- to eight-byte call window can become `call [rip+IAT]` only when API entry has exactly one pushed return address, unchanged other registers (including flags and SIMD), unchanged mappings and persistent RAM, and no earlier OS call. Leading NOPs retain the exact API return address. A seven- or eight-byte window can instead become `mov r64, [rip+IAT]` when it returns a known export in exactly one GPR with a balanced stack and the same preservation requirements. The result register comes from state comparison and may be any GPR except RSP, including R8-R15; an eight-byte load leaves a trailing NOP. Only callee scratch below the caller stack pointer is excluded; caller-owned stack bytes are compared. A later impure, unresolved or incomplete invocation invalidates earlier evidence. Executed starts are required; a preceding byte cannot authorize a REX prefix, and overlapping starts sharing a return are rejected. `observed_loads` and `repaired_loads` count address loads separately from calls.

The original section table, import directory layout and relocation table are not reconstructed; a packer does not restore them in memory.

## Identification

The compatibility fields `packer.kind` and `packer.evidence` report `unidentified` and an empty array. No protector registry, pack header parser or stub signature affects recovery. The legacy identification API validates the container and returns the same empty identity.

## Limits

Executables only; DLLs are not run. Checked execution admits one instruction at a time, on the order of 10^5 per second. x64 Unicorn, KVM and WHP support `direct-user-x64-v1`, bounded by time and events without an instruction count. Execution is tracked per 4 KiB page: a rewrite within an already executed page of the running generation is not reported as a transfer. Initialization may modify image data or create external process state before the entry; that state is not generally portable into a fresh process, and rerunning TLS callbacks may have additional effects. Unmodeled APIs stop execution explicitly. Import repair covers validated six- to eight-byte x64 call windows and seven- or eight-byte address loads; unreachable sites and other protected call forms remain unresolved. Virtualized code stays virtualized.

## Verification

`NeverDUnpackTests` checks container validation, safe import-cell allocation, conflicting witnesses and TLS metadata refusal. `NeverDUnpackExecutionTests` unpacks checked-in UPX fixtures (NRV2B, NRV2D, NRV2E, LZMA and a C runtime program) using the same generic path on Unicorn, KVM and WHP. It compares sections with the independent linked image observed after its own initializers and requires identical recovered files across available backends. `UnpackGeneratedTests.cpp` packs independent x86-64 and ARM64 programs inside the test and covers transfers, staged loading and import repair, including checked execution and x64 Unicorn/KVM/WHP direct execution. Native CI requires the matching KVM/WHP direct cases rather than accepting skips. `NeverDUnpackPublicTests` covers the C ABI and CLI. `unittests/unpack/fixtures/Makefile` regenerates the UPX fixtures.
