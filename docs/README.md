**Languages**: [English](README.md) | [简体中文](zh-CN/README.md) | [繁體中文](zh-TW/README.md) | [日本語](ja/README.md) | [한국어](ko/README.md) | [Français](fr/README.md) | [Deutsch](de/README.md) | [Español](es/README.md) | [Italiano](it/README.md) | [Русский](ru/README.md) | [العربية](ar/README.md)

[← NeverD project](../README.md)

# NeverD Documentation

Project overview and build/CLI notes live in the repository README. Contributor-facing design and test references are indexed here.

**Mobile support (experimental CLI):** `neverd mobile` supports [Android](android.md) APK, DEX, and smali inputs for Java recovery, and [iOS](ios.md) IPA, `.app`, and Mach-O inputs for native C and supported Objective-C/Swift source recovery. JSON reports describe recovery results and coverage. Start with the [mobile overview](mobile.md), then use the platform guides for commands and limitations.

English guides live directly in `docs/`. Translations are grouped in language
directories: `ar/`, `de/`, `es/`, `fr/`, `it/`, `ja/`, `ko/`, `ru/`, `zh-CN/`,
and `zh-TW/`. Each language directory contains a `README.md` documentation index,
a `project.md` project overview, topic guides, `CONTRIBUTING.md`, `ATTRIBUTION.md`,
and `roadmap.md`. Shared images remain in `assets/`.

CPU execution separates ISA admission, guest memory, backend transport and guest OS policy. `NEVERD_ENABLE_CPU_EMULATION` enables the x64/ARM64 CPU layer; `NEVERD_ENABLE_DRIVER_EMULATION` adds the bounded x64 Windows WDM/KMDF environment. The `linux-elf64-v1` profile runs supported Linux ELF processes. See [CPU execution](cpu-execution.md), [Guest process emulation](process-emulation.md) and [Windows driver emulation](driver-emulation.md).

`driver-strict` / `checked-x64-v1` supports KVM on matching Linux x64 hosts and WHP on matching Windows x64 hosts; `auto` selects that native transport, and cross-ISA execution selects Unicorn. Explicit Unicorn and the original V1 API retain the portable software profile. Native execution checks canonical addresses and instruction effects before entry; unavailable hardware fails without fallback. Unsupported instructions and OS behavior remain explicit errors. Native Windows x64 CI with Unicorn disabled passes all 329 required checks: 101 CPU checks, 224 driver outcomes from 26 built-in images, 46 WDK images and 40 scenario cases at both preferred and relocated bases, plus four SEH boundary checks ([`b2ca3cff`](https://github.com/NeverSight/NeverD/actions/runs/36973625293)). Native ARM64 runtime evidence is still pending, and this does not establish arbitrary-driver or Android/Darwin compatibility.

`checked-aarch64-v1` and `checked-user-aarch64-v1` provide bounded ARM64 FP32/FP64, fixed-width SIMD and complete FPCR/FPSR/vector state. Matching Linux ARM64 hosts use KVM, matching Windows ARM64 hosts use WHP, and cross-ISA execution uses Unicorn. Native ARM64 runtime evidence remains pending; Windows driver loading remains x64.

| Document | Description |
|----------|-------------|
| [README (English)](../README.md) | Overview, quick start, build, SDK, CLI |
| [Contributing](../CONTRIBUTING.md) | Development setup, build profiles, workflow, style, and PR expectations |
| [Architecture](architecture.md) | IR routes, component boundaries, strict lifting, support depth, and where to edit |
| [Testing](testing.md) | Test suites, generated fixtures, Unicorn roundtrips, and incremental commands |
| [Desktop workbench](gui.md) | Qt Quick views, worker separation, localization, annotations and MCP connections |
| [Desktop qualification](gui-qualification.md) | Supported workflows, verification evidence and platform release requirements |
| [Interpreter source recovery](interpreter-recovery.md) | Experimental x64 interpreter specialization, control contexts, source routes, local evidence, and explicit refusal boundaries; nested loop proof proposals; explicit discovery budgets and versioned C API; exact native-to-LLVM proof API |
| [Windows exception reconstruction](windows-exception-reconstruction.md) | SEH/C++ unwind support matrix, IR contract, native patch rules, and PE validation |
| [CPU execution](cpu-execution.md) | Validated configuration, capability queries, backend availability and typed CPU outcomes, independent of guest OS |
| [Bitvector proof backends](solver.md) | Optional Z3 proofs, fail-closed synthesis, independent solver tests and query export |
| [Guest process emulation](process-emulation.md) | Explicit Linux ELF64 process profile, startup stack, system calls, bounded output, CLI/C/Python and current limits |
| [Windows driver emulation](driver-emulation.md) | Bounded x64 WDM/KMDF lifecycle, requests, hardware scenarios, SEH, PnP subsets, backend selection and limits |
| [Memory-safety audit & hunt](memory-safety.md) | Heap-lifetime and copy-overflow analysis: identity contract per format, sink/source catalog, verdicts, budgets, and JSON schema |
| [Native plugins](plugins.md) | Pure-C descriptor ABI, callbacks and events, build/link workflow, discovery, and compatibility rules |
| [Python plugins](python-plugins.md) | Typed authoring SDK, embedded host, lifecycle, loading, safety, tests, and publishing |
| [Mobile overview](mobile.md) | Android and iOS support, input/output formats, quick start, and links to platform guides |
| [Android Java recovery](android.md) | Supports APK (including multidex), DEX, and smali → Java; CLI options, JSON reports, verification, and recovery limits |
| [iOS source recovery](ios.md) | Supports IPA/.app/Mach-O → native C and supported Objective-C/Swift sources; artifact selection, JSON coverage reports, verification, and recovery limits |
| [EVM decompilation](evm.md) | EVM inputs, hardforks, staged IR, C/LLVM host ABI, Solidity reconstruction, and limitations |
| [Solana SBF decompilation](sbf.md) | SBF v0-v4 ELF rules, staged IR, syscalls, C/Rust/LLVM backends, and host contracts |
| [Roadmap](roadmap.md) | Status: native formats, EVM, and Solana SBF implemented |
| Localized documentation | Use the language links above to open each language's index and project overview |

Native x64 and ARM64 startup probes validate bounded complete-state execution under an exclusive memory lease. XSAVE packets and ISA-aware page-table caches have one authoritative owner; native ARM64 workload evidence remains incomplete.

Native x64 `FOP/FIP/FDP` follow host save/restore rules: AMD may clear inactive x87 exception metadata. Startup probes validate these fields with a pending unmasked exception.
