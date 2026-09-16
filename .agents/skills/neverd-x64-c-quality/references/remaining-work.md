# Remaining x64 C-quality work

This is the backlog, not a claim of completion. Close items with a dump plus
a regression, then delete the row.

## Proven open

| Gap | Surface | Notes |
|---|---|---|
| Named frame slot + param-home copy-forward | HighC | `NeverDHighCStoreForwardingTests` (5 cases) fail: seed store skipped, later loads see `var_m*` or everything collapses to `arg0`. `DeadVars` stays set after `CopyForward.erase`, so some slots (`var_8`) can be assigned without a declaration. |
| LLVMC PHI | LLVMC | `/* phi: R9D_3_031 */` only. Uses of that name are uninitialized or dead. Obfuscated joins need real predecessor assignments. |
| LLVMC EH is a wrap | LLVMC | Whole-function `__try` + goto, not nested `__try`/`__except` regions. HighC structured regions are the readable target. |
| `--llvm` shard opt | pipeline | `seh_probe-msvc-x86_64-fh3-no-gs-o0.exe` decompile `--llvm` reported `LLVM shard N optimization failed: input-invalid` while still emitting C. |
| Flag / popcount noise | lift + LLVMC | Corpus dump still materializes PF/AF/OF, `__builtin_popcount`, and `*(T*)0 =` clobbers. Junk on obfuscated x64 will be worse until DCE owns it in LLVM, not the C printer. |
| Extra Win64 params on LLVM route | MedLLVM / LLVMC | HighC compacted `probe_plain_seh` to `int32_t arg0`. LLVMC still showed `arg0..arg7`. |
| Import as data pointer | LLVMC | `*(uint64_t*)_nd_codeptr_*` instead of `RaiseException(...)`. HighC resolves IAT names. |
| Wrapping casts | HighC | `return (int32_t)(uint32_t)((uint32_t)var + 1)` is required by sanitizer tests. Do not strip. |
| Source names | both | No PDB → `var_m18` / `arg0` / `g_1400050E0` / `sub_1400024E0`, not `Result` / `Value` / `ProbeSink` / `probe_filter`. |
| x86 outlined except | HighC | Handler body can sit outside the function range; epilogue may read an adjacent slot instead of the try Result. |

## Closed (do not regress)

| Decision | Test / helper |
|---|---|
| `g_<hex>` unnamed globals, not `dword_` | `makeSyntheticGlobalName`; `HighCPointerAddresses.NamesWritableImageDataStore`; `LLVMCPointerAddresses.NamesWritableNdDataGlobal` |
| `extern` on image objects | HighC `writeImageObjects`; LLVMC `writeGlobals` for `__nd_data_*` |
| Readonly image scalar → immediate | `HighCPointerAddresses.FoldsReadonlyImageIntegerLoad` |
| HighC assigns are declared (`t22_1`, `v36_0`, Phi) | `HighCPointerAddresses.DeclaresAssignedTempsAndUnusedCallResults` |
| LLVMC assigns are declared (including unused non-calls) | `LLVMCPointerAddresses.DeclaresAssignedTempsAndUnusedCallResults` |
| Win64 `rcx,rdx,r8,r9` | HighC param recovery; dump `probe_plain_seh` as one `int32_t` |
| x86 `call [IAT]` is an import | HighC `RaiseException` |
| Unused params compacted only if unused ≥ 4 | `emittedParamIndices` |
| Alloca load/store as named locals + `&` | `COFFExceptionIR.LLVMCAllocaLoadStoreUsesNamedLocalsAndAddressOf` |
| CatchSwitch renders `__except` syntax | `COFFExceptionIR.LLVMCCatchSwitchRendersExceptSyntax` |

## Next x64 exe pass

Prefer LLVMC for obfuscated guests. Sequence:

1. Make `--llvm` complete without shard `input-invalid` on the EH corpus.
2. Declare-and-assign PHI, or rewrite PHI into block-end copies.
3. Kill flag/popcount/`*(T*)0` in LLVM (or mark them analysis-only) before pretty-print.
4. Resolve IAT/import calls to names in LLVMC the same way HighC does.
5. Then pretty-print: empty-if invert, copy-forward, wrapping-cast display (without dropping sanitizer semantics).

Private PE/PDB fixtures are not the contract. Re-dump corpus `probe_plain_seh` after each of those layers.