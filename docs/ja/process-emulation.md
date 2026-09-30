**言語**: [English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← ドキュメント索引](README.md)

# ゲストプロセスのエミュレーション

`neverd emulate` は明示したゲスト OS プロファイルでイメージを実行します。CPU transport、イメージ解析、プロセス入口、OS サービスはそれぞれ別の責任範囲です。`NEVERD_ENABLE_CPU_EMULATION=ON` で有効にします。ドライバーエミュレーションにも含まれます。

最初の `linux-elf64-v1` profile は、x64/AArch64 の ELF `ET_EXEC` と自己再配置する static PIE `ET_DYN` を CPL3/EL0 で実行します。実際の ELF segment をロードし、初期 stack を構築し、命令 quantum ごとに再開し、明示的な Linux system call request を処理します。これは独立したプロセスモデルであり、完全な Linux distribution や任意の libc binary の実行保証ではありません。dynamic linking、signal、thread、filesystem、未対応 service は明示的に失敗します。x64 profile は限定された SSE/SSE2 形式を一部許可し、AArch64 は integer-only です。Windows、Android、Darwin などの kernel workload は別途です。

## CLI と SDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

対応する Linux host では KVM、Windows host では WHP を選びます。その他の host/guest ISA 組み合わせは Unicorn を使用します。選択 backend が利用不可なら error となり、暗黙 fallback はありません。Windows 上で実行しても ELF は Linux process model を使用します。命令範囲と制限は[CPU 実行](cpu-execution.md)を参照してください。

CLI は JSON report を一つ出力します。exit code は guest status が 0 なら 0、その他なら 2、未完了（fault/limit 含む）なら 3、setup/API error なら 1 です。実際の guest status は `exit_status` にあります。追加 C entry [`neverd_emulate_process_json`](../../include/neverd/sdk/NeverDCAPIProcess.h) は session、空でない input path、明示 profile、任意の options JSON を受け取ります。結果を `neverd_free_string` で解放します。NULL は setup error で、`neverd_last_error` に詳細があります。guest fault や resource stop も report を返します。session の分析用ロード済み image は不要で、変更もしません。

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

## オプションと結果

options は最大 64 KiB の JSON object です。未知/null field、不正な型、文字列内 NUL、正でない limit は拒否します。

| Option | 既定値 | 契約 |
|---|---|---|
| `backend` | `auto` | `auto`、`unicorn`、`kvm`、`whp` |
| `arguments` | 入力ファイル名 | argv[0] を含む完全な argv。空なら既定値 |
| `environment` | `[]` | 明示的 guest string。host 環境は継承しない |
| `instruction_limit` | 100000 | 共有される許可済み命令試行数 |
| `event_limit` | 10000 | system-call event 数。OS service 前に課金 |
| `timeout_microseconds` | 5000000 | process setup 後に始まる単調 deadline |
| `memory_limit` | 67108864 | 物理／map 済みメモリ予算 |
| `stack_size` | 1048576 | 予算内の page-aligned stack |
| `output_limit` | 1048576 | stdout/stderr 合計の捕捉 byte 数 |
| `instruction_quantum` | 1024 | runtime に戻るまでの admission 間隔 |

`schema_version` は 1 です。report には profile、architecture、選択 backend と理由、`stop_reason`、nullable `exit_status`、診断、入口/現在 PC、counter、service record、最後の型付き CPU exit が含まれます。address、syscall number、引数 register、raw return bit は `0x` なしの hex string です。`stdout_hex`/`stderr_hex` は NUL と不正 UTF-8 を保持します。syscall 結果 null は model が戻り値を定義しないこと（exit や未対応 request など）を示し、成功値 0 とは異なります。

## Linux profile の意味

OS policy は既存 ELF loader のデコード済み program header を使用します。ABI tag、segment alignment、map 済み PHDR table、user address 範囲を検証します。mapping plan は allocation 前に範囲、権限、重なり、budget を確認し、完全に準備できた private address space だけを公開します。file page の prefix/tail を保持し、BSS を zero 化し、segment 権限を守り、stack guard gap を予約します。ページが重なる layout や矛盾 header は推測せず拒否します。

初期 stack には aligned argc/argv/envp/auxv、PHDR/PHENT/PHNUM、entry、page size、identity value が入ります。model PID/TID/UID/GID は 1000 です。再現可能な実行のため `AT_RANDOM` は入力 SHA-256 の先頭16 byte です。暗号学的 entropy ではなくモデルの決定的 policy です。HWCAP/HWCAP2 は 0、vDSO はありません。

実装済み syscall は `write`、`exit`、`exit_group`、`getpid`、`gettid` で、番号は [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) と [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h) で別です。戻る x64 SYSCALL は RCX/R11 clobber、RAX、次 PC を反映します。ARM64 は番号に x8、結果に x0 を使います。未知の呼び出しは `unsupported_service` で停止し、host syscall は実行しません。

descriptor 1 と 2 は仮想 byte sink です。`write` は読取可能な user page を検証し、後続 page にアクセスできなければ読取済み prefix を返し、1 byte も読めなければ guest `EFAULT` を返します。不正 descriptor は `EBADF`。有効 descriptor への length 0 write は pointer を参照しません。Linux pipe atomicity や file object はモデル化しません。出力上限を超える write は公開前に停止します。

## 検証

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# shared-library/CLI build の場合:
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

両 ISA の original ELF entry assembly/C をコンパイルし、data/BSS、startup metadata、syscall error、binary output、permission fault、partial write、未対応 service、quantum 間の予算保持を検証します。利用不可 backend は明示的に skip します。公開 suite は C ABI/CLI と report/exit code の一致を検査します。cross compile や Unicorn ARM64 は native ARM64 KVM/WHP の証拠ではありません。

## Static PIE、TLS、検証

Static PIE は少なくとも `0x40000000` の決定的 load bias を使い、大きな `PT_LOAD` alignment に応じて増加します。各 segment、entry PC、`AT_PHDR`/`AT_ENTRY` に同じ bias を使い、元の program header 値は変更せず、interpreter がないため `AT_BASE` は0です。mapping source は元の file bytes であり、analysis pointer fixup は混入しません。guest startup が relocation と初期化を実行します。loader は section header に依存せず、元ファイルの範囲検証済み record から `PT_DYNAMIC` を decode します。存在する場合、table は readable/terminated で最大4096 entries。`PT_INTERP`、外部 dependency/filter/audit tag は拒否し、dynamic linker、symbol resolver、constructor runner は提供しません。

Static TLS template `PT_TLS` は loader fact として検証されます。template は1つ、file/memory extent は bounded、alignment は congruent、初期 byte は readable でなければなりません。guest startup が各 TLS block を allocate/init し thread pointer を設定します。Linux model は libc 固有の TCB/DTV を作りません。これにより freestanding program の compiler-generated local-exec TLS を許可します。dynamic TLS と OS thread は別の範囲です。

x64 の `arch_prctl` は `ARCH_SET_FS`、`ARCH_GET_FS`、`ARCH_SET_GS`、`ARCH_GET_GS` をサポートします。Set は未mapの user-range base も受け入れますが、後の dereference では権限を検査します。kernel-range base は guest `EPERM`、無効な Get destination は CPU fault なしで guest `EFAULT`。その他の操作は明示的に失敗します。ARM64 startup は `MSR` で `TPIDR_EL0` を設定します。`MRS`、FS/GS memory access、context restore は quantum/backend entry 間で thread pointer を維持します。thread scheduler は実装しません。

PIE/TLS fixture は独立した aligned block、TLS BSS、relocated auxv と guest relocation 前の zero RELA slot を検証します。x64 では `arch_prctl` error 後も前の base が保たれることを確認します。上記 build/CTest に `NeverDThreadPointerTests` を加えます。利用不能 backend は明示的に skip します。
