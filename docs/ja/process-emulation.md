**言語**: [English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← ドキュメント索引](README.md)

# ゲストプロセスのエミュレーション

`neverd emulate` は明示したゲスト OS プロファイルでイメージを実行します。CPU transport、イメージ解析、プロセス入口、OS サービスはそれぞれ別の責任範囲です。`NEVERD_ENABLE_CPU_EMULATION=ON` で有効にします。ドライバーエミュレーションにも含まれます。

最初の `linux-elf64-v1` profile は、x64/AArch64 の ELF `ET_EXEC` と自己再配置する static PIE `ET_DYN` を CPL3/EL0 で実行します。実際の ELF segment をロードし、初期 stack を構築し、命令 quantum ごとに再開し、明示的な Linux system call request を処理します。これは独立したプロセスモデルであり、完全な Linux distribution や任意の libc binary の実行保証ではありません。dynamic linking、signal、thread、filesystem、未対応 service は明示的に失敗します。

<!-- i18n-section: cli-sdk -->

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

<!-- i18n-section: options-results -->

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
| `linux_priority` | 未設定 | タスクごとの明示的な nice 値と Linux の生の優先度サービスに対する呼び出し権限 |
| `linux_kernel` | 未設定 | リリース済み GKI ブランチ、または観測したゲストカーネルインターフェースの欠如を明示 |

`schema_version` は 1 です。report には profile、architecture、選択 backend と理由、`stop_reason`、nullable `exit_status`、診断、入口/現在 PC、counter、service record、最後の型付き CPU exit が含まれます。address、syscall number、引数 register、raw return bit は `0x` なしの hex string です。`stdout_hex`/`stderr_hex` は NUL と不正 UTF-8 を保持します。syscall 結果 null は model が戻り値を定義しないこと（exit や未対応 request など）を示し、成功値 0 とは異なります。

<!-- i18n-section: linux-semantics -->

## Linux profile の意味

`writev` は x64/ARM64 と Android Bionic で同じ出力先を共有します。出力前に最大 1024 個のゲスト `iovec` を読み込み、負の長さを `EINVAL` で拒否し、ユーザーアドレス範囲を検査して Linux のページ境界に合わせた転送上限を適用します。不正な記述子はベクトル参照前に `EBADF`、読めないメタデータは出力なしで `EFAULT` を返します。後続データの障害ではコピー済みの接頭部を保持します。出力予算は公開前に両ストリームとベクトル全体に適用します。`write` と `writev` は記述子の下位 32 ビットを使い、ベクトル数も Linux の 32 ビット取り込みに従います。Bionic のみが負の生エラーを `-1` と `errno` に変換します。`LinuxOutputNativeTests` は独自の十例をホスト Linux で通常ファイルに出力し、モデルの x64/ARM64 テストは予算も検査します。[Linux ベクトル取り込み契約](https://github.com/torvalds/linux/blob/v6.12/lib/iov_iter.c)を参照してください。

OS policy は既存 ELF loader のデコード済み program header を使用します。ABI tag、segment alignment、map 済み PHDR table、user address 範囲を検証します。mapping plan は allocation 前に範囲、権限、重なり、budget を確認し、完全に準備できた private address space だけを公開します。file page の prefix/tail を保持し、BSS を zero 化し、segment 権限を守り、stack guard gap を予約します。ページが重なる layout や矛盾 header は推測せず拒否します。

Static PIE は少なくとも `0x40000000` の決定的 load bias を使い、大きな `PT_LOAD` alignment に応じて増加します。各 segment、entry PC、`AT_PHDR`/`AT_ENTRY` に同じ bias を使い、元の program header 値は変更せず、interpreter がないため `AT_BASE` は0です。mapping source は元の file bytes であり、analysis pointer fixup は混入しません。guest startup が relocation と初期化を実行します。loader は section header に依存せず、元ファイルの範囲検証済み record から `PT_DYNAMIC` を decode します。存在する場合、table は readable/terminated で最大4096 entries。`PT_INTERP`、外部 dependency/filter/audit tag は拒否し、dynamic linker、symbol resolver、constructor runner は提供しません。

初期 stack には aligned argc/argv/envp/auxv、PHDR/PHENT/PHNUM、entry、page size、identity value が入ります。model PID/TID/UID/GID は 1000 です。再現可能な実行のため `AT_RANDOM` は入力 SHA-256 の先頭16 byte です。暗号学的 entropy ではなくモデルの決定的 policy です。HWCAP/HWCAP2 は 0、vDSO はありません。

実装済み syscall は `write`、`exit`、`exit_group`、`getpid`、`gettid`, `mmap`, `mprotect`, `munmap`, `brk` で、番号は [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) と [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h) で別です。戻る x64 SYSCALL は RCX/R11 clobber、RAX、次 PC を反映します。ARM64 は番号に x8、結果に x0 を使います。未知の呼び出しは `unsupported_service` で停止し、host syscall は実行しません。

Static TLS template `PT_TLS` は loader fact として検証されます。template は1つ、file/memory extent は bounded、alignment は congruent、初期 byte は readable でなければなりません。guest startup が各 TLS block を allocate/init し thread pointer を設定します。Linux model は libc 固有の TCB/DTV を作りません。これにより freestanding program の compiler-generated local-exec TLS を許可します。dynamic TLS と OS thread は別の範囲です。

x64 の `arch_prctl` は `ARCH_SET_FS`、`ARCH_GET_FS`、`ARCH_SET_GS`、`ARCH_GET_GS` をサポートします。Set は未mapの user-range base も受け入れますが、後の dereference では権限を検査します。kernel-range base は guest `EPERM`、無効な Get destination は CPU fault なしで guest `EFAULT`。その他の操作は明示的に失敗します。ARM64 startup は `MSR` で `TPIDR_EL0` を設定します。`MRS`、FS/GS memory access、context restore は quantum/backend entry 間で thread pointer を維持します。thread scheduler は実装しません。

descriptor 1 と 2 は仮想 byte sink です。`write` は読取可能な user page を検証し、後続 page にアクセスできなければ読取済み prefix を返し、1 byte も読めなければ guest `EFAULT` を返します。不正 descriptor は `EBADF`。長さ 0 の書き込みでもユーザーアドレス範囲を検査しますが、ページのマッピングやデータ読み取りは不要です。Linux pipe atomicity や file object はモデル化しません。出力上限を超える write は公開前に停止します。

匿名メモリサービスは、イメージやスタックと同じプロセスアドレス空間と物理メモリ予算を使います。`mmap` が受け付けるのは `MAP_PRIVATE | MAP_ANONYMOUS` と、通常の `PROT_NONE`、`PROT_READ`、`PROT_READ | PROT_WRITE`、`PROT_READ | PROT_EXEC`、読み取り可能な RWX です。空いているページ境界のヒントを優先し、それ以外は `0x100000000`、次いで最小ユーザーアドレスから空きを探し、スタックガードを確保します。これは決定的配置であり Linux ASLR ではありません。新しいページは独立所有されゼロ初期化されるため、部分的な解除でも固定されていないページを回収できます。CPU 投影や保持中の backing view は、退役した割り当てを自身の寿命まで保持する場合があります。

長さはページ単位に切り上げます。`munmap` は穴や重複解除を許容し、`mprotect` は穴までのマッピングを変更してから `ENOMEM` を返します。`PROT_NONE` は割り当てと内容を保持しつつゲストアクセスを禁止します。生の `brk` は成功時に要求したバイト境界、失敗時に旧境界を返し、libc のゼロ／負一の規約とは異なります。初期 break はページ境界に揃えたイメージ終端です。拡張は他のマッピングと予算に従い、縮小は残る部分ページの内容を保持します。対象範囲の規則とエラー優先順位は Linux の[マッピング](https://github.com/torvalds/linux/blob/v6.8/mm/mmap.c)および[保護](https://github.com/torvalds/linux/blob/v6.8/mm/mprotect.c)に従います。

ファイル／共有／固定マッピング、下方拡張、巨大ページ、メモリ固定、保護キー、実行専用／書き込み専用方針、その他のフラグは明示的に未対応です。効果の公開や戻り値の生成前に停止します。対応範囲内の通常の範囲・長さ・整列エラーはゲストエラーを返して実行を続けます。ゲストポインタやマッピング要求をホスト OS に転送することはありません。

任意の `linux_kernel` 入力は、明確に観測したカーネルインターフェースの欠如を記録します。例えば `pidfd_open` 実装がない fixture は次を使用します。

```json
{"linux_kernel":{"unavailable_syscalls":["pidfd_open"]}}
```

選択した生の呼出しは、カーネル入口がない場合と同様に引数検査前に -ENOSYS を返し、記述子を作らずゲストメモリも変更しません。Bionic の `syscall` wrapper は通常の -1/errno 変換を保ちます。入力の省略と空リストでは、このインターフェースは従来の未対応境界に留まり、他の未知の呼出しを ENOSYS に変換しません。現在は `pidfd_open` のみを受理し、未知の名前、重複、誤った型を拒否します。入力からカーネル版、ホストの可用性、動作する pidfd 実装を推定しません。[カーネルの欠落呼出し実装](https://github.com/torvalds/linux/blob/master/kernel/sys_ni.c)を参照してください。

明示した `linux_kernel.gki` は、5.10～6.18 の公開済み Android common カーネル系列を選びます。実装範囲は、生存するモデルプロセスと明示したゲストタスクに対する `pidfd_open`、および版別のベクトル取り込みです。`linux_files` と記述子表を共有し、Bionic と生のトラップは所有権とエラー順序を共有します。GKI と `pidfd_open` 不在観測は同時に指定できません。任意の `linux_kernel.tasks` 配列は、他の生存タスクの固定された閉じた一覧を宣言します。項目例は `{ "id": 2000, "group_leader": true }` です。配列省略時は他の対象の検索が未対応、空配列では現在のグループリーダーだけが既知です。宣言した一覧外の PID は ESRCH になります。一覧と優先度観測は整合が必要で、協調型 Android ゲストスレッドとは併用できません。Android API レベルはカーネルを選びません。8 個のソース版、記述子動作、テストと残る範囲は[公開済み GKI 契約](../android-gki-kernels.md)を参照してください。

任意の `linux_priority` 入力は、呼び出し元と同じ UID を持つテスト用タスクの nice 状態を宣言します。Linux ELF64 と Android の生の `setpriority` / `getpriority` はこの状態を共有し、ホストの優先度は変更しません。

```json
{"linux_priority":{"tasks":[{"id":1000,"nice":0}],
                   "cap_sys_nice":false,"rlimit_nice":0}}
```

タスク ID は重複しない正の符号付き 32 ビット値で、初期 nice は -20..19、`rlimit_nice` は 0..40 です。`cap_sys_nice` と `rlimit_nice` の既定値は false と 0 ですが、タスク状態は常に明示します。入力やタスクが未宣言の場合、新規スレッドの状態が未宣言の場合、および PRIO_PGRP / PRIO_USER は未対応として停止し、継承や所有者を推測しません。PRIO_PROCESS の who=0 は現在のゲストタスク、それ以外は指定タスクを選びます。不正な選択値は生の -EINVAL を返します。設定要求は符号付き 32 ビットの nice を -20..19 に制限します。nice 値を小さくするには CAP_SYS_NICE または十分な RLIMIT_NICE が必要で、拒否時は状態を変えず生の -EACCES を返します。生の取得値は `20 - nice`、つまりカーネルの 40..1 符号化であり、libc が変換した結果ではありません。

[Linux setpriority/getpriority](https://man7.org/linux/man-pages/man2/setpriority.2.html).

`linux_signals` はプロセス全体の初期シグナル動作を指定します。未指定の項目は不明であり、`SIG_DFL` を意味しません。明示的な空リストでは、以前の動作を照会せずに新しい動作を登録できます。5 フィールドはすべて必須です。JSON の正確な整数範囲を超える符号なし 64 ビット値は十進文字列で指定します。

```json
{"linux_signals":{"actions":[
  {"signal":11,"handler":0,"flags":0,"restorer":0,"mask":0}
]}}
```

シグナル番号は 1–64 です。`rt_sigaction` と Bionic は状態を共有し、構造体の配置とエラー順序はそれぞれの ABI に従います。保留中のシグナル、配送、ハンドラー実行、スレッド別マスクは未実装で、ホストのハンドラーも使いません。[完全な契約](../process-emulation.md#linux-profile-semantics)を参照してください。

<a id="windows-pe64-profile"></a>

<!-- i18n-section: linux-clocks -->

## 明示的なゲストクロック

省略可能な `linux_time` は、Linux システムコールと Android Bionic に固定のクロック入力を与えます。ホスト時刻の読み取り、命令実行に伴う時刻の進行、既定時刻の推測は行いません。

```json
{"linux_time":{"clocks":[
  {"id":0,"seconds":"4294967297","nanoseconds":987654321},
  {"id":1,"seconds":123,"nanoseconds":456789}],
  "timezone":{"minutes_west":-60,"dst_time":0}}}
```

静的クロック ID 0–9 と 11 に対応します。各クロックは独立しており、省略した値は未知のままです。重複または未知の ID は拒否します。秒は符号付き 64 ビット、ナノ秒は `[0, 1000000000)` です。JSON 整数は `±9007199254740991` 以内に制限し、十進文字列では全 64 ビット範囲を保持します。タイムゾーンのフィールドは符号付き 32 ビットです。C++ では `ProcessOptions::LinuxTime` を使い、他の OS プロファイルでは拒否します。

`clock_gettime`、`gettimeofday`、x64 の `time` が入力を共有します。入力の欠落、動的クロック、未モデル化の部分書き込みでは明示的に停止し、完了済みの書き込みは保持します。時刻調整、スリープ、実機クロックは未対応です。書き込み順、エラー、ポインタの扱いは[クロック契約の詳細](../process-emulation.md#explicit-guest-clocks)を参照してください。

<a id="explicit-memory-files"></a>

<!-- i18n-section: linux-files -->

## 明示的なメモリファイル

`linux_files` は Linux ELF64 と Android に閉じた読み取り専用ファイル一覧を提供します。必須の `files` は空でもよく、各要素は正規の絶対パス `path` とバイナリの `bytes_hex` が必須です。ホストファイルや暗黙の `/proc` 内容は読みません。未指定なら停止し、一覧にないパスは `ENOENT` を返します。

```json
{"linux_files":{"files":[
  {"path":"/fixture/data","bytes_hex":"00ff410a805a"}],
  "descriptor_limit":256}}
```

各 open は独立した位置を持ち、Bionic、`syscall`、生のトラップと guest スレッドは記述子表を共有します。close は最小の空き番号を再利用可能にします。読み取り専用の `open/openat`、`read/close`、通常の `lseek`、`O_CLOEXEC` とアーキテクチャ固有の `O_LARGEFILE` に対応します。errno は Bionic が変換します。読み取り障害ではコピー済みの部分を保持します。stdin の内容、相対パス、ディレクトリ、書き込み、リンクは未対応です。

C++: `ProcessOptions::LinuxFiles`. `descriptor_limit`: 3–4096 (256); `files` ≤ 256; `path` < 4096 bytes; component ≤ 255 bytes; data + paths + NUL ≤ 16 MiB; JSON ≤ 64 KiB. [Contract](../process-emulation.md#explicit-memory-files).

長さ 0 の読み取りはユーザーアドレス範囲の上限から開始できます。元のアドレス範囲を検証した後、ファイル位置と元の要求長の和が `INT64_MAX` を超える場合は、EOF でも `EINVAL` を返し、カーソルを変更しません。

各要素には完全な `metadata` を指定できます。C++ の `LinuxFileOptions::Metadata` は既存のファイルパスをキーとします。`fstat`/`fstat64` と syscall は固定観測値を共有し、ホストの属性を読まず、size を内容長から推測せず、位置も変更しません。通常ファイルと全必須フィールドのみを受け入れ、全幅整数は十進文字列で指定します。x64/AArch64 は 144/128 バイトを書き込み、rdev と padding はゼロです。無効な記述子は `EBADF`、全域が書き込み不可なら `EFAULT`。観測値の欠落、未知の標準ストリーム、部分的に書き込み可能な出力は、元のバイトを保って停止します。フィールドと範囲はリンク先の契約を参照してください。

```json
{"linux_files":{"files":[{"path":"/fixture/virtual","bytes_hex":"616263",
  "metadata":{"device":1,"inode":"18446744073709551615","mode":33060,
    "link_count":1,"uid":1000,"gid":1000,"size":0,"block_size":4096,"blocks":0,
    "access_time":{"seconds":0,"nanoseconds":0},
    "modification_time":{"seconds":0,"nanoseconds":0},
    "change_time":{"seconds":0,"nanoseconds":0}}}]}}
```

<!-- i18n-section: windows-pe64 -->

## Windows PE64 プロファイル

`windows-pe64-v1` は PEB/TEB、静的・動的 TLS、`DllMain`、名前付き Win32 API、明示的な非循環 DLL グラフを持つ有界 Windows x64/ARM64 コンソールプロセスに対応します。ゲストモジュールは名前／序数によるコード・データのインポート、DIR64 再配置、転送エクスポートと実際のローダーリスト識別子を扱います。`LoadLibraryA` / `LoadLibraryW`、`FreeLibrary`、`GetProcAddress` は設定済みモジュールカタログを使用します。CRT/GUI、ARM64 のフレームベースのユーザー SEH、スレッド、一般的な Windows アプリ互換性は未完成で、ネイティブ ARM64 KVM/WHP の証拠も未取得です。

Windows 仮想メモリに `VirtualAlloc`、`VirtualFree`、`VirtualProtect`、`VirtualQuery` と現在のプロセスの `FlushInstructionCache` を追加しました。OS 層が予約領域を所有し、コミット済みページ、権限、物理記憶域は `AddressSpace` が一元管理します。動的コードの書き換え、アクセス違反、メモリ予算の再利用をテストします。

プライベート割り当ては `MEM_RESERVE`、`MEM_COMMIT`、`MEM_DECOMMIT`、`MEM_RELEASE`、`MEM_TOP_DOWN` に対応し、予約は 64 KiB 境界、ページは 4 KiB です。予約だけではゲスト RAM を消費しません。再コミットは内容を保持して権限を更新し、デコミットは各ページの記憶域を返します。範囲全体の検証と割り当ての準備により、通常の失敗で部分変更を残しません。クエリは 48 バイトの x64/ARM64 メモリ情報を返し、同一割り当て内で前方に結合します。初期イメージ、環境、ヒープ領域、API 入口、スタック境界も配置に含め、スタックの識別を TEB と一致させます。成功した `VirtualProtect` が旧権限の出力先を読み取り専用にした場合、新しい権限は適用されたまま、出力内容は変わらず、呼び出しは成功を返します。 未コミットページを含む範囲の保護変更は `ERROR_INVALID_ADDRESS` を返し、旧権限の出力に `PAGE_NOACCESS` を書き込みますが、ページ権限は変更しません。

対応する保護は `PAGE_NOACCESS`、`PAGE_READONLY`、`PAGE_READWRITE`、`PAGE_EXECUTE_READ`、`PAGE_EXECUTE_READWRITE` です。ガードページ、実行専用、コピーオンライト、キャッシュ修飾子、大きなページ、reset/write-watch/プレースホルダー、モデル所有の実行時マッピングの変更は明示的に未対応です。デコミットと解放はプライベート仮想割り当てだけが対象です。

[VirtualAlloc](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc), [VirtualFree](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualfree), [VirtualProtect](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect), [VirtualQuery](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualquery), [MEMORY_BASIC_INFORMATION](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-memory_basic_information).

```bash
neverd emulate guest.exe --profile=windows-pe64-v1 \
  --options='{"backend":"auto","arguments":["guest.exe","argument"],"environment":["MODE=test"]}'
```

単一スレッドの PE32+ EXE は推奨ベースを維持し、任意の入口と静的 TLS を持つ明示的な DLL を受け入れます。`WindowsProcessOptions::Modules` または JSON `windows.modules` の `name` と `path` で最大 64 個のゲスト基本名とホスト入力を指定します。ホスト DLL の探索・実行はありません。ASCII 名は大文字小文字を区別せず、重複とシステム API 提供元の上書きを拒否し、到達可能なファイルだけを読みます。名前／序数の関数・データを実際のエクスポートに結び付けます。欠番、未定義、循環、bound/delay import、未対応の load configuration/CFG は失敗します。可動 DLL の衝突には DIR64 を適用し、固定衝突やリンク用メタデータへの再配置書込みは 対象イメージを公開する前に拒否します。

`readPEProgramExports` が元のエクスポートと読み取り範囲、`WindowsProcessModules` が依存グラフとプロセス共通の提供元／名前 API ゲートを所有します。`VirtualMemory` は全イメージを事前予約し、`AddressSpace` がページと権限を管理します。PEB/LDR は実イメージのみを示し、初期化リストはローダーへの登録順です。登録順と依存関係に基づく attach 呼び出し順を別々に保持します。`GetModuleHandleW` は NULL または ASCII 基本名に対応し、大文字小文字を無視し、拡張子なしでは `.dll` を追加します。パス、非 ASCII、末尾の点は未対応です。未検出はエラー 126、成功時は LastError を保持します。API モデルはインストール済み DLL ではありません。

入力総バイト数と全イメージ範囲はそれぞれ `memory_limit` に制限され、実行環境のマッピングも後者に含みます。準備は 65,536 レコード、64 MiB のメタデータ読み取り、名前長、共通期限で制限します。ホスト I/O の硬い時間保証はありません。独自 EXE→DLL→DLL は再配置、序数、共有データ、API ポインター、`MEM_IMAGE`、リスト、EXE TLS attach/detach を検証します。`NeverDWindowsProcessTests` はネイティブ Windows 対照、`NeverDPEProgramExportsTests` は不正メタデータと予算、`NeverDProcessPublicTests` は C ABI/CLI の一致を検証します。利用不能なバックエンドは明示的にスキップします。

`WindowsProcessLifetime` は同じ CPU と実行予算で依存順に DLL TLS コールバック、`DllMain`、続いて EXE TLS と入口を実行します。各モジュールに独立した TLS インデックスと整列済み領域を割り当て、再配置・リンク済みイメージから共有 64 KiB 領域へコピーします。TLS の予約引数はゼロ、起動／プロセス終了時の `DllMain` は不透明な非 NULL 値です。明示的なプロセス終了は初期化完了 DLL をローダーリストの逆順に切り離し、その後 EXE TLS を呼びます。EXE 初期化前でも同様です。起動時の `DllMain(FALSE)` は detach 通知なしで `0xc0000142` 終了します。障害や予算切れは後処理を捏造しません。ゲスト DLL がある PE 入口の return は未対応のスレッド終了を必要とするため明示停止します。非ゼロの `SizeOfZeroFill` は未対応ですが、実際の TLS テンプレート内のゼロ初期化バイトは対応します。 入口なし DLL は TLS attach を受けますが、プロセス detach 通知は受けません。

`WindowsProcessExports` は静的インポートと `GetProcAddress` で名前／序数の解決を共有し、コード、データ、別名、連鎖転送を扱います。 実際に参照する起動時の転送だけがカタログのモジュールと初期化依存関係を追加し、未使用の転送はファイルを読みません。 名前は大小文字を区別し、名前がなければ NULL／エラー 127、直接照会で序数がなければ穴を含め NULL／エラー 182、照会引数が NULL ならエラー 87、成功時は LastError を保持します。 未知のモジュールハンドルは未対応です。 有界 API 登録から提供元／名前ごとの入口を一度だけ確保します。 各イメージの現在の PE ヘッダーとエクスポートメタデータを検査し、変更や読み取り不能を拒否します。 連鎖は最大 64 項で、準備段階の残りのメタデータ予算と実行期限を共有します。 穴への転送は対象イメージのベースを返し LastError を保持します。 序数ゼロへの転送はエラー 87 です。 ベースはデータアドレスであり、イメージヘッダーの実行権限は与えません。 実行時の転送は設定カタログのモジュールをロードし、初期化完了後に照会結果を返せます。実行中のエクスポート表変更は未対応です。

`WindowsProcessLoader` は `windows.modules` の ASCII DLL ベース名をロードし、明示参照、共有依存関係、起動モジュールの保持を管理します。転送の反復照会で余分な参照は増えません。再ロードでは同じカタログ枠に新しい常駐世代を割り当てます。TLS と `DllMain` は同じ CPU 上で中断 API のスタックフレームより下に実行し、レジスター復元はゲストのメモリー書き込みを保ち、現在の戻り先を使用します。動的 attach/detach の予約ポインターはゼロです。明示的ロード中の attach 失敗はクリーンアップ後にエラー 1114 を返し、成功済みの独立した入れ子ロードは保持します。アンロードはイメージと TLS を解放し、再ロードは元の内容から始まります。モデル外のローダーリストや TLS ポインター変更は明示的に拒否します。ファイル、イメージ、メタデータ予算は失敗や再ロードでも累積します。システム提供元のモジュールハンドルはマップ済み PE のベースです。ファイルシステム検索、非 ASCII パス、`LoadLibraryEx` フラグ、循環インポート、初期化／アンロード中の同一モジュールへの再入状態変更は未対応です。

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` は PEB プロセスパラメーター内の実際のゲスト環境ブロックを共有します。名前は大文字小文字を区別しない ASCII、値は UTF-16 です。変更前に入力、容量、書き込み権限を検証します。スナップショットは後続の変更から独立し、解放時にゲストメモリを回収します。モデルのブロック上限は 64 KiB で、文字列と展開処理には境界と実行期限の検査があります。不明なポインター所有権、不正なブロック、ANSI コードページ、展開バッファーの重複は未対応です。`WindowsEnvironmentTests.cpp` は利用可能なバックエンドで独自の x64/ARM64 フィクスチャを比較し、CI では独立したネイティブ Windows オラクルを必須とします。

`WindowsProcessHeap` はプロセスヒープの割り当て、[`HeapReAlloc`](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heaprealloc)、解放、サイズ照会を一元管理します。サイズ変更は保持範囲のデータを維持し、`HEAP_ZERO_MEMORY` は追加領域をゼロ化、`HEAP_REALLOC_IN_PLACE_ONLY` は移動を禁止します。再割り当て失敗時は旧ブロックを保持し、NULL と `ERROR_NOT_ENOUGH_MEMORY`（8）を返すネイティブの観測結果に一致します。独立したページの縮小・解放で容量を返却し、段階的な拡張と有界コピーで実行期限を確認します。独自ヒープ、例外生成フラグ、不明な所有権、アクセス不能なコピー・ゼロ化範囲は明示的に停止します。`WindowsHeapTests.cpp` は両 ISA、強制移動、予算再利用、失敗時の原子性を検証し、CI は同じ独自 EXE をネイティブ Windows でも実行します。

`WindowsSystemModules` は両 ISA 向けに `ntdll.dll`、`kernelbase.dll`、`kernel32.dll` の有界な PE64 モデルイメージを構築します。ASCII の `GetModuleHandleA` / [`GetModuleHandleW`](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulehandlew)、`LoadLibraryA` / `LoadLibraryW`、[`GetProcAddress`](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress) はそのマップ済みベースを共有し、PEB/LDR と `MEM_IMAGE` も同じイメージを示します。静的インポート、名前検索、ゲスト DLL の転送は同じ API ゲートとエクスポート解決器を使います。提供元は常駐し、ゲスト初期化コールバックを持たず、通常のゲスト DLL をすべて解放すればエントリから復帰できます。ヘッダーやエクスポートメタデータの変更で検索を停止します。未対応のシステムエクスポート名と非ゼロ序数は明示的に停止し、対応名の大小文字違いと空名はエラー 127、NULL 検索は 87 を返します。生成バイトとアドレスはモデル方針であり、Windows DLL の版別配置、実際の序数、提供元間の別名は再構築しません。`WindowsSystemTests.cpp` は独自 x64/ARM64 EXE をネイティブ Windows と比較し、初期スレッドの復帰を独立して 8 回観測します。

`WindowsProcessExceptions` は同じ CPU とプロセス予算で `AddVectoredExceptionHandler`、`RemoveVectoredExceptionHandler`、`RaiseException` を実装します。順序付きハンドラーは登録・削除、入れ子の例外、モデル化 API、DLL 読み込み、プロセス終了を扱えます。x64/ARM64 のデータアクセス違反と x64 の整数除算例外は、ゲストが変更した `CONTEXT` の検証後に再開できます。汎用レジスター、SIMD、対応する FP 状態を保持し、ソフトウェア例外はモデル提供元内の実際の return 命令から再開します。保持する登録は 128 件、入れ子は 16 フレームまでです。不正な処置、例外ポインターの変更、未対応フィールド、上限超過は明示的に失敗します。ARM64 のフレームベースの SEH／アンワインド、デバッガー配送、実行／ガードページ例外は未対応です。`WindowsExceptionTests.cpp` は独自 EXE／DLL をネイティブ Windows と比較します。ARM64 KVM/WHP の実機証拠は未取得です。 [AddVectoredExceptionHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-addvectoredexceptionhandler), [RemoveVectoredExceptionHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-removevectoredexceptionhandler), [RaiseException](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-raiseexception), [CONTEXT x64](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-context), [ARM64_NT_CONTEXT](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-arm64_nt_context). ソフトウェア例外レコードには `EXCEPTION_SOFTWARE_ORIGINATE`（`0x80`）が付き、呼び出し元の継続不可フラグとは個別に扱います。元の Windows 実行ファイルでソフトウェア例外とハードウェア例外のフラグ値を厳密に照合します。 [EXCEPTION_RECORD](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_record).

`WindowsProcessContext` は各ディスパッチフレームの発生元を保持します。対応する x64 データアクセス／除算フォルトでは `CONTEXT.EFlags` に RF（`0x10000`）を示し、アクセス違反コードを含むソフトウェアの `RaiseException` では現在のコンテキストを保持します。発生元は VEH/VCH と SEH の検索／巻き戻しを通じて維持されます。有効な継続では RF を除いた論理 CPU フラグを復元し、ゲストによる RF の変更は状態公開前に拒否します。この限定プロファイルは命令ブレークポイントやゲストによる RF 制御を扱いません。`WindowsExceptionTests.cpp` は保存記録、復元、拒否時の CPU／RAM 不変性を確認します。

Windows ring3 は独立したネイティブ観測に従い、checked x64 の `operand_alignment` 障害をパラメーター `[read, UINT64_MAX]` の `STATUS_ACCESS_VIOLATION` に変換します。ストア命令も同じです。原因は CPU 層が提供し、Windows はベクトル 13 から推測したり再デコードしたりしません。`WindowsAlignmentProcessTests.cpp` は元の PE 命令で 72 障害シナリオと 9 アドレス修復再試行（`72 + 9`）を実行し、PC、RF、XMM、RAM を確認します。未分類または不整合な障害は拒否します。プロセスとドライバーの障害レポートは nullable な `cause` と 16 進の `error_code` を保持し、欠落とゼロを区別します。この配送は checked x64 ユーザープロファイルに適用されます。

`AddVectoredContinueHandler` と `RemoveVectoredContinueHandler` は独立した順序付きリストを管理し、例外ハンドラーと保持登録数 128 の上限を共有します。ベクター例外ハンドラーが実行再開を受け入れると、継続ハンドラーは同じ変更可能な例外レコードと `CONTEXT` を参照します。入れ子の例外や DLL 通知を含め、最終コンテキスト検証は継続コールバックの終了後に行います。異なる種類のハンドラーのハンドルは削除できません。`WindowsContinuationTests.cpp` は独自 EXE の順序、早期終了、登録変更、コンテキスト修復、入れ子の配送、ローダーコールバック、プロセス終了をネイティブ Windows と比較します。検証済みの Windows x64 ベクター処理経路は `EXCEPTION_NONCONTINUABLE` が設定されていても実行再開を許可しますが、フレームベースの SEH の動作を証明するものではありません。ネイティブ ARM64 実行は未検証です。 [AddVectoredContinueHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-addvectoredcontinuehandler), [RemoveVectoredContinueHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-removevectoredcontinuehandler).

`RtlCaptureContext` は x64 と ARM64 の `kernel32.dll`、`ntdll.dll` で利用できます。共通の `WindowsProcessContext` と `IntegerABI` が CPU 状態や LastError を変更せず、呼び出し元の PC/SP を保存します。ネイティブ Windows の観測で、x64 のフラグ `0x10000f`、未使用の home／デバッグ／ベクトル領域の保持、従来の 32 ビット x87 アドレス欄を確認しました。ARM64 は LR を PC に保存し、記録内の X0/LR をゼロにします。レジスタ、SIMD、浮動小数点制御はゲストから取得し、x64 セレクタと MXCSR 能力マスクは設定されたゲスト CPU に従います。無効、未整列、または一部アクセス不能な出力レコードは書き込み前に失敗します。`WindowsContextTests.cpp` は直接インポート、提供元検索、VEH コールバック、ページ境界をまたぐ出力、失敗時の原子性を検証します。`scripts/check_windows_context.py` は独自実行ファイルを Windows x64／ARM64 で実行し、非空の x87 状態を別途検証します。この ARM64 API の観測はネイティブ KVM/WHP 実行の証拠ではありません。コンテキスト復元、スタック走査、動的関数テーブルは引き続き別の実装課題です。 `WindowsProcessServices.def` は正確なモジュール制約を宣言します。`kernelbase.dll` での検索はネイティブ観測と一致する `ERROR_PROC_NOT_FOUND`（127）を返し、存在しないエクスポートを追加しません。 [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

`WindowsProcessSEH` は `os/windows/exception/` の共有 `X64SEH`（ドライバー環境なしでも利用可能な `NeverDEmulationWindowsException`）で x64 `__C_specific_handler` と UNWIND_INFO V1 を処理します。VEH 検索後のフィルター、finally、非局所的なハンドラーへの転送、入れ子／衝突アンワインド、再配置された EXE/DLL フレームに対応し、非揮発 GPR/XMM を保持します。フィルターによる継続では同じ `CONTEXT` で VCH を実行します。`WindowsSEHTests.cpp` は独自の 23 シナリオをネイティブ Windows と比較し、KVM/WHP/Unicorn は同じ意味論を使います。プロセス予算内でイメージ世代、ヘッダー、アンワインド／スコープのバイト列、言語ハンドラーのコード領域、IAT を再検証します。メタデータ変更や保持中のイメージのアンロードは明示的なエラーです。ARM64 のフレーム SEH、C++ EH、動的関数テーブル、汎用 RtlUnwind/NtContinue、ローダー／VEH／VCH コールバック境界を越えるアンワインドは未対応です。

`EXCEPTION_NONCONTINUABLE` に対して x64 フィルターが `EXCEPTION_CONTINUE_EXECUTION` を返すと、新しいコンテキストで `STATUS_NONCONTINUABLE_EXCEPTION`（`0xc0000025`、フラグ `0x81`、関連レコードは null）を配信します。VEH を再実行してから保持した論理スタックを再検索し、同じ深度・実行予算で finally の順序と EXE/DLL フレームの同一性を保ちます。23 のネイティブシナリオは 21 の正常実行と二つの終了を含みます。元の `CONTEXT` を復元しても、VEH/VCH がこの二次例外の継続を受け入れると未処理のまま終了し、モデルは実行時失敗を報告します。ソフトウェア例外のアドレスは保存 PC と一致し、内部ディスパッチャーのアドレスとレジスター配置はモデルの方針です。 [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

動的アンロードのコールバック前にモジュールを初期化リストから外しますが、マッピング、名前検索、ロード／メモリリストへの所属はコールバック中も維持します。入口の復帰を調べるネイティブ比較では、システムワーカースレッドとは独立して初期スレッドを観測します。

`WindowsDynamicTests.cpp` は元の x64/ARM64 DLL と EXE を独立したネイティブ Windows 観測と比較し、参照数、共有依存、入れ子ロード、attach 失敗時の清掃、転送照会、プロセス終了、入口なし DLL、再ロード時の TLS 初期化を確認します。追加回帰はローダー改変と無効なコードポインターを拒否し、累積準備予算と中断 API の未完了結果を確認します。Windows CI は原生オラクルと WHP ケースを必須にします。クロスコンパイルと Unicorn ARM64 はネイティブ ARM64 実行の証拠ではありません。

`GetProcAddress` の転送チェーンでライブラリが欠けるとエラー 127 を返し、明示的な `LoadLibrary` でカタログにないモジュールを指定すると 126 を返します。ネイティブ比較と各利用可能バックエンドは宣言済みの全 41 シナリオを検証します。Windows では DLL の各バリアントについて、全 DLL をアンロードした後の入口復帰を 16 回確認します。 `GetProcAddress` の転送先の初期化に失敗した場合も、クリーンアップ後に 127 を返します。プロセス終了の detach コールバックは終了を呼び出した側のスタック内容を保持します。

`WindowsExportTests.cpp` は元の x64/ARM64 DLL と EXE で、転送コード／データ／序数呼び出し、別名、初期化中の照会、再配置、大小文字を区別する欠落、LastError、循環・非常駐ターゲット、不正ポインター、成功した照会後のメタデータ変更を検証します。同じ EXE をネイティブ Windows の独立オラクルで実行し、ネイティブ CI は WHP ケースを必須とします。C ABI／CLI は完全なレポートを比較します。ARM64 ネイティブ実機の証拠は未取得です。 エクスポート表の有無を変えた EXE で、両依存グラフ、PEB リスト順、detach 順、名前／序数／NULL のエラーコードを確認します。

`WindowsLifetimeTests.cpp` は固定トレースを独立したネイティブ Windows プロセスと KVM/WHP/Unicorn で照合します。通常終了、入口 return、両 DLL の初期化失敗、4 箇所の早期終了、入口なし DLL を含みます。回呼障害、共通予算、再配置 TLS フィールド、TLS 総容量も検証します。ネイティブ入口 return のプローブは初期スレッドのハンドルを保持し、終了コードと正確なスレッド／プロセス通知列を 64 回検証します。残る子スレッドは観測後に終了させ、プロセス終了値を入口の戻り値として扱いません。

```json
{"windows":{"modules":[{"name":"middle.dll","path":"inputs/middle.dll"},{"name":"leaf.dll","path":"inputs/leaf.dll"}]}}
```

x64 GS と ARM64 x18 は TEB を指し、スタック範囲、自身、PID/TID、PEB、プロセスパラメーター、LastError、TLS を提供します。 UTF-8 を厳密に UTF-16 へ変換し、argv は Microsoft CRT 規則で引用します。 環境名は ASCII、大小文字を無視した重複は拒否し、値は Unicode 可、整列した環境は二重 NUL で終端します。 ホスト環境やファイルシステムは継承しません。 静的 TLS はテンプレートとゼロ BSS、32 ビット索引を設定し、動的 TLS は別の TEB スロットを使います。 起動・終了は変更後の回呼表を順に読み、期限と予算を共有します。 通常のプロセス終了は終了回呼を実行します。 入口からの return は常駐ゲスト DLL がない場合だけに対応し、プロセス終了処理中の二度目の `ExitProcess` は未対応です。

正確な API は `WindowsProcessServices.def` にあります。`ExitProcess`、`RtlExitUserProcess`、標準出力ハンドルと同期 `WriteFile`、LastError、プロセス・スレッド ID と疑似ハンドル、`GetCommandLineW` / `HeapReAlloc`、ヒープ確保・解放・サイズ、動的 TLS、`LoadLibraryA` / `LoadLibraryW` / `FreeLibrary` / `GetModuleHandleA` / `GetModuleHandleW` / `GetProcAddress` を扱います。`kernel32.dll`、`kernelbase.dll`、`ntdll.dll` の正確な名前だけを解決します。直接 syscall や偽の回呼ゲートでは API を選べません。ヒープの所有権と回収、バイナリー出力、API エラーと非同期 I/O・ユーザー例外の未対応を区別します。別名ポインターでも完了数の初期ゼロ化と実際の戻り先変更を反映します。

`windows.native_calls` はモジュール・関数名、宣言されたスカラー引数、nullable な結果を記録し、NT syscall 番号を捏造しません。`NeverDWindowsProcessTests` は実 PE、コンパイラー TLS、回呼変更、ヒープ、別名、不正メタデータ、権限、予算を検証し、`NeverDProcessPublicTests` は CLI/C ABI を検証します。Windows CI は同じ EXE を直接実行して独立比較し、WHP テストも必須です。ネイティブ ARM64 の実行証拠には対応マシンが必要です。

空でない入力バッファが読み取り不可の場合、`WriteFile` は `ERROR_INVALID_USER_BUFFER`（1784）を返し、書き込みバイト数をゼロにして、バイトを出力しません。

`windows.defer_unmodeled` は、モデルが実装していないローダー上の事実を含むイメージを読み込み、実行がそのいずれかに依存した場合にのみ停止します。API インベントリにないエクスポートとカタログにないモジュールは不透明なエントリに束縛されます。各識別子は 1 つのアドレスに解決され、それを実行すると `module!export` を名指しして `unsupported_service` で停止します。モデルが解釈しないディレクトリは解釈されないままとなり、ファイルに裏付けのないメタデータは読み込み時に読まれず、そのようなイメージを通るフレームベースの例外ディスパッチは停止します。`observeProcess` は `ProcessObserver` を追加します。これはプロセスの開始時と各実行ウォッチで停止したプロセスを読み取りますが、ゲストの状態を変えることはできず、これが実行を終了させた場合は `observer` が報告されます。[アンパック](unpack.md)はこの 2 つの上に構築されています。

[PE/COFF](https://learn.microsoft.com/windows/win32/debug/pe-format), [ARM64 ABI](https://learn.microsoft.com/cpp/build/arm64-windows-abi-conventions), [WriteFile](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-writefile), [TLS](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue), [Wine 10.0 loader](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/ntdll/loader.c). [GetProcAddress](https://learn.microsoft.com/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress).

<!-- i18n-section: verification -->

## 検証

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# shared-library/CLI build の場合:
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

テストは両 ISA の独立した ELF エントリアセンブリと C をコンパイルし、data/BSS、起動情報、システムコールエラー、バイナリ出力、権限障害、部分書き込み、未対応サービス、実行量をまたぐ予算を確認します。TLS は独立整列ブロック、ゼロ化 BSS、スレッドポインタと切り替え後の保持を検証し、x64 は `arch_prctl` エラー後も旧ベースが残ることを確認します。利用できないバックエンドはスキップを明示します。公開テストは共有 C ABI と CLI の報告／終了コードを照合します。静的 PIE は自前のデータ／関数ポインタ再配置前に auxv とゼロの RELA スロットを確認します。マッピングテストは分析用バイトを選んだ場合の fixup 保持、動的表はセクションなし・不正・依存入力を検証します。匿名メモリは割り当て、保護、穴、再マッピング、ヒープ伸縮、処理可能なエラーを両 ISA で検証し、実際のゲスト書き込みで通常／部分保護後の障害を確認します。x64 は RW/RX 切り替えで同一アドレスのコードを更新し両版を呼び出します。同じ ELF の Linux ネイティブ実行を独立した結果／障害の参照とします。メモリ単体テストは予算枯渇、回収、RAM を保持しない正本マッピングのスナップショットを検証します。クロスコンパイルや Unicorn ARM64 はネイティブ ARM64 KVM/WHP の証拠ではありません。
