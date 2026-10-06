**言語**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 34d34e187ac46720dfc5b061a28605b13c368d0ab705c3d1a43465497ac1b4fe -->

[← ドキュメント一覧](README.md)

# macOS と iOS のゲストプロセス環境

`lib/emulation/os/darwin/` は、ホスト CPU 転送層とは別に、制限を明示した独立 Mach-O プロセスをモデル化します。`NEVERD_ENABLE_CPU_EMULATION` を有効にします。Windows ドライバエミュレーションは不要です。`macos/` と `ios/` が明示的なプラットフォームプロファイルを定義します。

| プロファイル | Mach-O プラットフォーム | ゲスト ISA | OS ページ |
| --- | --- | --- | --- |
| `macos-macho64-v1` | macOS | x86-64、基本 ARM64 | x64 4 KiB、ARM64 16 KiB |
| `ios-macho64-v1` | iOS デバイス | 基本 ARM64 | 16 KiB |
| `ios-simulator-macho64-v1` | iOS Simulator | x86-64、基本 ARM64 | x64 4 KiB、ARM64 16 KiB |

デバイス用バイナリはシミュレーター用イメージではなく、ホストからゲストのプラットフォームを推測しません。macOS で ISA が一致すれば [HVF](macos-hvf.md)、異なる場合の `auto` は Unicorn を使います。CPU のマッピング単位は 4 KiB のままです。[C、Python、CLI API](process-emulation.md) はオプション、制限、レポートを共有します。

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

## イメージと起動

`MachOExecutionImage` は解析による再配置パッチを適用せず元のバイト列を保持します。プラットフォームとエントリが一意な thin little-endian `MH_EXECUTE` のみ許可します。ユニバーサルイメージは必要なスライスを明示的に取り出してください。

メタデータと末尾バイトを含むファイル全体が、解析やコピーの前に `memory_limit` 内に収まる必要があります。通常ファイルから上限付きのプライベートスナップショットを読み、NUL を含むパス、短い読み取り、サイズ変更を拒否します。生きたファイルマッピングを保持しません。ファイルとゲストメモリは同じ値の別々の上限です。ホストのファイル I/O に厳密な実時間保証はありません。

セグメントは現在と最大の権限、ゼロ埋めを保持します。`__PAGEZERO` は広大な実メモリを割り当てずアドレスを予約します。ファイル/VM 範囲、OS ページ整列、丸め後の重複、ヘッダーの所属、実行可能エントリ、予算を確認します。ヘッダーセグメントは読み取りと実行が可能である必要があります。ガードページと専用の復帰ゲートは予約されたままです。最後のファイルページはページ境界または EOF まで元のバイトを保ち、後続の完全な VM ページをゼロ化します。[XNU ローダー](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c)に基づきます。

`LC_MAIN` は `argc`、`argv`、`envp`、apple ベクターを四つの整数引数で受け取り、戻り値の下位八ビットが終了ステータスになります。`/usr/lib/dyld` はインポートのないこのエントリ引き渡しに限り許可し、ホストの dyld は実行しません。非ゼロの `stacksize` は拒否します。予算を決めるのは呼び出し元の `stack_size` です。

`LC_UNIXTHREAD` は PC だけが設定された完全なネイティブ 64 ビット汎用レジスタレコードを一つだけ許可します。初期スタックは argc、終端付き argv/envp、`executable_path=<input filename>` を含む終端付き apple ベクターで構成します。独自 SP/フラグ、他のレジスタ、追加 flavor、矛盾するエントリを拒否し、ホスト環境や Linux 補助ベクターを継承しません。[dyld の設計](https://github.com/apple-oss-distributions/dyld/blob/main/doc/dyld4.md)を参照してください。

外部 dylib、インポート、rebases/chained fixups、コンストラクター/デストラクター、TLS セクション、arm64e/PAC、未対応 CPU サブタイプ、暗号化、未モデル化ロードコマンドは実行前に失敗します。fixup のない PIE は優先アドレスを使い、ASLR ではありません。署名 blob はメタデータであり、AMFI や entitlement ポリシーを実装しません。

## Darwin サービス

BSD 呼出しの ARM64 は X16、X0–X5 と `svc #0x80`、x64 は BSD クラス `0x02000000`、RAX、RDI/RSI/RDX/R10/R8/R9 を使います。成功は carry を消し、エラーは carry と正の errno を返します。ARM64 は X1 を消し、x64 は成功時に RDX を消してエラー時には保持します。SYSCALL が変更するレジスタは明示されています。レポートの `result` と `error=true` は BSD エラーを表します。戻らない要求や未対応要求には両フィールドがありません。XNU の [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c)、[x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c) の規則に基づき、Apple の実装コードは取り込んでいません。

サービスは `exit`、`write`、`getpid`、`getppid`、`getuid`、`geteuid`、`getgid`、`getegid`、`mmap`、`mprotect`、`munmap` です。PID/UID/GID は 1000、PPID は 1 に固定します。記述子 1、2 は NUL や非 UTF8 を含むバイトを捕捉し、閉じた記述子や読み取り専用記述子は EBADF です。部分コピー済みのバイトは保持しますが、後続の障害は EFAULT のままです。`INT_MAX` を超える長さは、記述子、ポインター、予算の確認前に EINVAL になります。[XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c)に基づきます。

メモリサービスは `flags=0x1002`、記述子 -1、オフセットゼロのプライベート匿名データマッピングに対応します。長さと非固定ヒントは OS ページに切り上げ、占有済みヒントでは上位アドレスを検索してから既定配置へ戻ります。従来の生 mmap は長さゼロで割り当てなしのゼロを返します。`MAP_UNIX03` に対応し、長さゼロは EINVAL です。Unmap/protect は整列済みアドレスを要求し、NONE/READ/WRITE に対応、WRITE は READ を含みます。物理所有は OS ページ単位なので部分解除で予算を解放し、新しいページはゼロになります。空白や最大権限境界を越える protect が失敗しても、範囲全体の元の権限を保持します。[XNU VM サービス](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c)を参照してください。

共有/固定/JIT マッピング、実行可能匿名メモリ、その他の Mach trap、間接システムコール、スレッド、シグナル、ホストファイル/ネットワーク、dyld、Objective-C/Swift runtime、Foundation/UIKit は対象外で、明示的に停止します。完全な Apple OS や iOS Simulator アプリケーションではありません。

## 検証

独自の C テスト入力を Clang と `ld64.lld` で生成し、Apple SDK や専有バイナリは使いません。五つのプラットフォーム/ISA、不正な Mach-O、4/16 KiB ページ、予算上限時の部分解放を検証します。`NeverDProcessPublicTests` が C API/CLI を比較し、`NEVERD_TEST_LIBNEVERD` と `NEVERD_TEST_DARWIN_FIXTURES` で Python SDK の同じ五つの組み合わせを有効にします。

## 明示的なファイル入力と記述子

`darwin_files` は3つのプロファイルに既定で読み取り専用の閉じたファイル一覧を提供します。必須の `files` は正規の絶対ゲスト `path` と16進数 `bytes_hex` を持ち、任意の `stdin_hex` は有限入力です。入力省略は未知で非ゼロ読み取りを停止し、空文字列は EOF です。一覧未指定の open は停止し、明示的な空一覧の未存在絶対パスは ENOENT を返します。ホストのファイルや入力は参照しません。

追加サービスは `open`、`read`、`pread`、`lseek`、`close`、`dup`、`dup2`、`fcntl` です。read/write/open/close/fcntl/pread の nocancel 入口も同じ実装を使います。O_RDONLY/O_CLOEXEC と F_DUPFD、F_DUPFD_CLOEXEC、F_GETFD、F_SETFD、F_GETFL を扱います。独立 open は別の位置、dup は共有位置と個別の close-on-exec フラグを持ち、pread は位置を変えません。0/1/2 の close・置換も後続 I/O に反映し、出力の複製は元の捕捉先と予算を保持します。

上限は256ファイル、パス/NUL/内容/入力の合計16 MiB、1024バイト未満のパス、255バイト以下の成分です。排他的上限 `descriptor_limit` は3–4096、既定256、JSON は64 KiBです。不正設定はロード前に拒否します。INT_MAX を超える read は FD 検査前に EINVAL、EOF は宛先に触れず、不正宛先は EFAULT です。部分的に書き込み可能な範囲はコピーや位置変更の前に停止します。SET/CUR/END の失敗は位置を保持します。旧 stat とその他 fcntl は未対応です。ファイルを祖先にすると ENOTDIR です。同じオブジェクトでネイティブ macOS と比較し、C/CLI/Python は5つのゲスト組合せを検証します。iOS 実機の証拠ではありません。

2026-10-05 の Release 検証は381項目中177成功、204スキップ、失敗なしで、ARM64 HVF 必須51/51を実行しました。ネイティブ macOS 7プログラム、公開 C/CLI・レポート35項目、Python の5ゲスト組合せ、検証スクリプト66項目も成功しました。件数は重複します。新しいファイルサービスの Intel HVF/KVM/WHP ネイティブ証拠はありません。Intel HVF は未検証で Actions を停止中です。iOS SDK と実機比較はありません。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## 既存ファイルの変更

厳密な真偽値 `"writable":true`、または C++ の `DarwinFileOptions::WritableFiles` でプロセス内変更を明示します。省略・false は読み取り専用で、未知の許可は停止します。ホストや入力オプションは変更しません。write(4/397)、pwrite(154/415)、truncate(200)、ftruncate(201)、O_TRUNC は同じ内容ノードを使います。open の位置は独立、dup は位置と状態を共有し、最後の close 後も内容を保持します。拡張はゼロ埋め、切り詰めは位置を保持し、O_RDONLY|O_TRUNC も切り詰めます。

F_SETFL は O_APPEND のみを変更し、アクセスモード、close-on-exec、FWASWRITTEN を保持します。実際に非ゼロバイトを転送すると F_GETFL に 0x10000 が現れ、pwrite と出力捕捉も対象です。pwrite は append を無視して位置を保持します。INT_MAX 長さ検査は FD より先、pwrite の -1 はさらに先に EINVAL。INT64_MAX はゼロ書き込みより先に EFBIG となり、長さの制限後に追加位置を選びます。

成功した ftruncate は同サイズでも呼び出し元の open 記述と dup に FWASWRITTEN を設定します。O_TRUNC は O_RDONLY を含め新しい記述だけに設定し、パスの truncate は既存記述を変えません。

部分的に読める入力は効果の前に停止します。全体 EFAULT は内容を保持しますが、非空 append は位置を EOF に移します。転送失敗は内容・位置を確定しません。`mutation_policy` 未指定では、非ゼロ書き込み、切り詰め、非ゼロ全体 EFAULT は完全な stat 観測を無効化し、以後の stat は出力前に停止します。ゼロ書き込みは保持します。16 MiB はパス/NUL、入力、ディレクトリ記録、CWD、現在の内容と書き込みパス参照の合計論理予算です。縮小で backing を置き換えて容量を解放し、初期入力と有界の置換バッファは別に存在します。既知 inode 別名と immutable/append-only フラグは拒否します。

DarwinMemory の全マッピング区間を unmap するまで変更を拒否します。PROT_NONE と close 済み FD も含み、失敗・旧式ゼロ長マップはリースを残しません。新しいマップは現在の内容を使います。O_WRONLY の READ/WRITE mmap は EACCES、PROT_NONE は成功し後から mprotect で読み書きを許可できます。

元の通常/nocancel プログラムをネイティブと比較し、4K/16K 単体テストと C/CLI/Python の5構成を検証します。権限強制・ディレクトリ削除・親をまたぐ改名・ハードリンク、実ファイルシステムのメタデータ更新、マップ整合性、EOF SIGBUS、完全な環境と iOS 実機は未完了です。Intel HVF は未検証、Actions は停止中です。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233","writable":true}]}}
```

[XNU write](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU vnode](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c).

## 明示的な可変メタデータ

`writable: true` と完全な metadata に加え、ファイルに mutation_policy を設定できます。C++ は `DarwinFileOptions::MutationPolicies` を使います。これは明示的な仮想疎割り当て契約であり、APFS の推測やホスト時計の参照ではありません。省略時は変更後メタデータが未知のままです。

allocation_unit、mutation_time、seconds/nanoseconds は必須で、整数は既存の無損失規則を使います。単位は512バイト〜16 MiBの2の累乗で、block_size や VM ページとは独立です。通常権限（set-id/sticky なし）、flags=0、link_count=1、および初期密割り当て blocks=ceil(size/allocation_unit)*(allocation_unit/512) を要求します。ゼロ値から穴を推測しません。ポリシーのパス参照も16 MiB論理予算に含み、割り当て台帳から ENOSPC は推測しません。

書き込みは触れる全単位を割り当て、穴へのゼロ書き込みも対象です。truncate の拡張はゼロのみ追加し、縮小は切り上げ EOF より先の単位を破棄して末尾の部分単位を保持します。再拡張は破棄した割り当てを復元しません。非ゼロ成功書き込みと全成功 truncate（同サイズ、空 O_TRUNC も）は size/blocks と固定 mtime/ctime を更新します。他の値と初期入力は不変、read は atime を進めません。パス stat、別 open、dup、再 open は同じノードを参照します。

ゼロ書き込み、予算/マップ拒否、部分入力拒否、バックエンド失敗は既知状態を保持します。非ゼロ全体 EFAULT は未知状態にし、後の成功でも復元しません。stat 出力失敗はノードを変えません。virtual-file-metadata は5構成と C/CLI/Python で144バイト全体を検証します。割り当てはポリシーテストで、APFS 等価性の証拠ではありません。ネイティブは別にフラグ・位置・エラー順序を検証します。名前空間、実 FS 整合性、Mach、動的ランタイムは未完了です。

```json
{"mutation_policy":{"allocation_unit":4096,"mutation_time":{"seconds":-7,"nanoseconds":123456789}}}
```


## 疎ファイルの位置指定

通常ファイルに mutation_policy があり割り当てが既知なら、lseek は SEEK_HOLE=3 と SEEK_DATA=4 を受け付け、stat と同じ台帳を参照します。初回変更前はゼロ値も含む密割り当てです。指定種別の単位内なら入力位置、なければ次の一致単位の先頭を返し、終端の穴は EOF です。負値は EINVAL、EOF 以降（空ファイル含む）または後続データなしは ENXIO=6。失敗は位置を保持し、成功はその open 記述と dup のみ変更します。別 open は独立、再 open は現在の割り当てを見ます。メタデータ・フラグ・内容は不変、whence 上位ビットは無視します。

ポリシーなし、ディレクトリ、全体 EFAULT 後の未知割り当ては未対応です。ゼロや拒否された変更から割り当てを推測しません。独自 sparse-file-seek はネイティブ/ゲストでエラー、書いたバイト、EOF、記述の寿命を比較し、前方の FS 固有区間位置は仮定しません。virtual-file-metadata は厳密なポリシー配置を別途検証し、C/CLI/Python は5構成を確認します。

[XNU lseek](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## 通常ファイル名の削除

ディレクトリの `mutable:true`（C++ `MutableDirectories`）は直下の名前変更を明示的に許可し、内容の `writable` とは独立です。権限がなければ未対応として停止。既知の非ゼロ flags、親の特殊アクセス権、子の link_count≠1、親または子の既知の別名を拒否します。stat とスナップショットの inode を共用し、明示的に異なるデバイスは区別。パスは既存の予算に計上されます。

`unlink(10)` / `unlinkat(472)` は既存の通常名を削除。通常ファイル削除の下位32ビットは0または `0x800` のみ対応し、未知ビットはパス/FDより先に EINVAL、AT_REMOVEDIR は後述の制限付き削除、DATALESS と SYSTEM_DISCARDED は未対応です。共通パス解決を使い、不在 ENOENT、ファイル末尾スラッシュ ENOTDIR、通常ディレクトリ EPERM、スラッシュのみのルート EISDIR、末尾が `.`/`..` のルート EBUSY。ネイティブで末尾 `.`/`..` も確認。

古い FD/dup/独立 open はデータ・カーソル・フラグを維持し、F_GETPATH は捕捉した旧パスを返します。新規 open は失敗し、暗黙の親と CWD は残ります。書込み権限はオブジェクトに属し、最後の記述子とマップ範囲の解放後だけ close/dup2/次の変更で現在のバイト予算を回収。初期パス費用は残り、権限強制・親をまたぐ改名・ハードリンクは未完了です。初期ディレクトリの削除には後述の明示許可が必要です。

親の stat/readdir/SEEK_END は旧/新 FD とパス全体で未知となり、コピー/カーソル変更前に停止。read/pread は EISDIR、SET/CUR/F_GETPATH/fchdir/相対解決は継続。既知の変更ポリシーでは nlink=0、ctime=固定時刻のみ変更し、後の書込みでも nlink=1 に戻りません。ポリシーなし/EFAULT 後はメタデータ不明。失敗は状態を保持。元の `unlinked-file` はネイティブの名前/FD規則を比較し、時刻と親の失効は明示的モデル規則です。

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"00"}],"directories":[{"path":"/work","mutable":true}]}}
```

[XNU unlink / unlinkat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [F_GETPATH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c).

## 通常ファイルの作成

O_CREAT=0x200 は明示的な mutable 親の直下に空ファイルを作成します。通常/nocancel open・openat は共通です。新規オブジェクトは書込み可能、既存は WritableFiles に従います。読取り専用 FD でも作成できますが書込みはできません。作成ポリシーがなければ stat64 と疎領域の検索は未知のままです。同名の旧 metadata/mutation_policy は常に継承しません。

O_CREAT と O_EXCL=0x800 の併用は既存ファイル・ディレクトリに切詰め前の EEXIST。O_EXCL 単独は無効です。既存ディレクトリの読取り専用 O_CREAT は成功します。無効アクセスモード→FD 空き→O_CREAT|O_DIRECTORY の EINVAL→パスの順です。作成できるのは元パスの最後の欠落要素だけで、欠落祖先や末尾 `/`・`//`・`/.`・`/..` は ENOENT。新規 O_CREAT|O_TRUNC は FWASWRITTEN を設定せず、既存切詰めは設定します。

実際の挿入だけが親の観測を無効化。同名の新旧データ・FD・メタデータ・マップ寿命は独立です。256 項は初期非ファイル項と生存ファイルを数え、新しい正規パス/NUL とデータは 16 MiB に課金。削除後、最後の FD/マップ解放で動的費用を回収し、初期費用は保持します。予算超過や1024バイト以上の正規パスは明示的に停止し、ENOSPC やネイティブのパスエラーを捏造しません。失敗時は名前/FD を公開しません。created-file のネイティブ/5構成、4K/16K 境界試験が対象。権限強制・親をまたぐ改名・リンク・ディレクトリ変更は残っています。

[XNU open](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

## 明示的な作成メタデータとプロセス umask

任意の `darwin_files.umask`（C++ `InitialUmask`）は初期マスクを八進数 0..07777 で指定し、作成権限とは独立です。`umask(60)` は旧マスクを返し、入力の下位 07777 ビットを保存します。ゲストメモリも空き FD も不要です。省略は未知で、ホスト値や既定値を推測しません。一度だけ初期化し、将来の作成だけに影響し、呼出し元の入力を変更しません。下例の十進数 18 は八進数 0022 です。

任意の `darwin_files.creation_policy`（C++ `CreationPolicy`）は新規オブジェクトの完全なメタデータを指定します。厳密なオブジェクトは `first_inode`、`block_size`、`generation`、`creation_time`、`mutation_policy` の5項目だけで、時刻と変更ポリシーには既存の形式を使います。明示 umask、1つ以上の mutable 親、全対象親の完全な metadata が必要です。block_size は 1..INT32_MAX、generation は uint32、割当単位は 512..16 MiB の2の累乗でブロックサイズ/VMページとは独立、ナノ秒は [0,1000000000) です。first_inode は非ゼロ uint64 で、別デバイスを含む全 stat/スナップショット inode より大きくします。JSON の正確な整数範囲を超える値は十進文字列を使います。

成功した新規挿入だけが全体共通の inode 列を進めます。UINT64_MAX を使うと永久に枯渇し、close/unlink/名前再利用/umask/後の検索でも戻りません。排他、FD、パス、項目数、バイト予算の拒否は名前・FD・採番を確定せず、既存 O_CREAT も消費しません。新 stat64 は直接の親の device/GID、固定ゲスト実効 UID 1000、mode `S_IFREG | (mode & 0777 & ~umask)`、nlink=1、size/blocks/flags=0 を使います。ブロックサイズ、generation、4つの初期固定時刻はポリシー由来です。親の完全な stat/列挙が失効しても、不変の device/GID だけは使え、完全な記録は復元しません。

新ノードは独自のメタデータ/割当状態を持ち、旧同名オブジェクトを継承しません。write/truncate/unlink は変更ポリシーを共有し、inode/mode/birthtime と削除後 nlink=0 を保持します。全範囲 EFAULT 後は永久に未知、既存ノードには遡及適用しません。`created-file-metadata` は5構成でネイティブの権限、マスク返値、実効 UID、親デバイス/グループ、寿命を比較し、`virtual-created-metadata` は144バイト全体を別に照合します。ネイティブの4時刻は一致するとは限りません。固定時刻/疎割当は仮想規則で、権限強制、資格情報切替、ACL、ネイティブ APFS の動作は対象外です。

```json
{"darwin_files":{"files":[],"umask":18}}
```

[XNU creation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU umask](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c). [XNU rename / renameat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## 同一親ディレクトリ内の通常ファイル改名

`rename(128)`、`renameat(465)`、`renameatx_np(488)` は、同じ明示的に変更可能な直近の親内で通常ファイルを改名・置換します。同名の無操作にも許可が必要で、許可後は観測を保持します。下位32ビットの flags は `RENAME_EXCL=0x4` と `RENAME_NOFOLLOW_ANY=0x10`、および両者の組合せを受け付けます。未知ビットと EXCL+SWAP はパス読取り前に EINVAL、SECLUDE と SWAP は未対応です。ここの無操作と EISDIR の規則は EXCL なしの場合に適用され、EXCL は後述の契約に従います。共通解析器はソース優先、FD、元のスラッシュ・ドットの検査順序を保持します。ディレクトリソースは即座に未対応。通常ファイルのターゲット末尾ドット/二重ドットは解決成功後、マウント・許可検査より先に EINVAL となり、入れ子や別親も対象です。存在しない/非ディレクトリの祖先のエラーが優先します。許可された親内の通常ディレクトリターゲットは EISDIR です。

独立 open・dup・既存 FD の `F_GETPATH` はソースの新しい名前に追従します。置換された物体は最後の名前、データ、カーソル、フラグ、マップ寿命を保持し、書込み権限やメタデータをソースへ渡しません。既知ポリシーは各 ctime と置換先の nlink=0 だけを変更し、識別子・所有権・作成時刻・割当を維持します。ポリシーなし/全体 EFAULT 後は完全なメタデータ不明のまま。実際の改名だけが親の stat/列挙を無効にします。

新規 inode・項目・空き FD は不要。新パス/NUL がソースの動的費用を置き換え、初期費用は残ります。古い FD/マップのない置換先だけが容量回収に使え、回収は一度だけです。部分 unmap では全物体の費用が残ります。1024バイト以上のパスや16 MiB超過は変更前に停止。親をまたぐ移動、既知 device の矛盾、ディレクトリ移動、swap/exclusive/seclude、権限強制は未対応です。同じ stat device は同じマウントを証明せず、EXDEV を推測しません。`renamed-file` はネイティブ/ゲストの識別・パス・置換・マップを比較し、時刻と予算は仮想規則です。

## ディレクトリと相対パス

任意の `directories` は正規の絶対 `path` と任意の完全な `metadata` を持ち、空ディレクトリを指定できます。ルートと祖先は暗黙に存在し、メタデータで未存在パスは作れません。mode は `0x4000` と権限、size は [0, INT64_MAX] の明示的観測です。`working_directory` は既存の正規ディレクトリを指定し、省略時の CWD は未知です。ホストから継承しません。指定パスは最大256（祖先メタデータを含む）、パス/NUL/内容/入力/CWD は計16 MiBです。

`openat` (463)、`openat_nocancel` (464)、`chdir` (12)、`fchdir` (13)、`fstatat64` (470) は同じ解決器を使います。相対パスはディレクトリ FD または `AT_FDCWD=-2`、絶対パスは FD を無視します。重複スラッシュ、`.`、`..`、末尾スラッシュも祖先を検査し、`/file/..` は ENOTDIR、`/missing/..` は ENOENT です。失敗や元の FD の close・再利用・置換は CWD を変えません。`F_GETPATH=50` は複製 FD にも正規パスと NUL を返し、その後を書き換えません。

ディレクトリ read/pread はゼロ長でも EISDIR、負の pread オフセットは先に EINVAL です。SET/CUR は位置を共有し、END には明示 size が必要です。mmap は EINVAL。fstatat64 は 0、`AT_SYMLINK_NOFOLLOW=0x20`、`AT_SYMLINK_NOFOLLOW_ANY=0x800`、`AT_FDONLY=0x400`（パスを無視）に対応。不正ビットは EINVAL、`AT_REALDEV=0x200` は未対応です。ストリームの識別は未知、権限はアクセス制御モデルではなく、ディレクトリ作成と制限付き削除は後述の明示許可を使います。同じ `directories` をネイティブと5ゲストで比較し、stat は実ファイルとディレクトリを照合します。Intel HVF Actions は停止中です。

[XNU VFS](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/fcntl.h).

ディレクトリ検証（2026-10-05、Release）：登録467件、成功227、スキップ240、失敗0、必須ARM64 HVFは60/60実行。ネイティブmacOS 10プログラム、公開C/CLI/レポート37件（スキップなし）、Python 5ゲスト、検証スクリプト66件が成功しました。件数は重複します。証拠：`build-hvf-arm64/darwin-directory-verified-evidence/`。他のネイティブ転送とiOS実機は未検証です。

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## 明示的なディレクトリスナップショット

`getdirentries64` (344) は既存の `directories` 項目の任意の不変 `contents` を列挙します。C++ は `DarwinFileOptions::DirectoryContents` を使用します。`entries` は `.`、`..` と全直接子項目を明示順序で含む完全な一覧です。空ディレクトリでも未指定は未知です。パスや stat 情報を作成せず、ホストを参照しません。

各項目は `name`、非ゼロ `inode`、`type`（0 未知、4 ディレクトリ、8 通常ファイル）、`next_offset`、`seek_offset` が必須です。型はパスと、同じ解決先の inode は他の一覧・メタデータと一致します。`next_offset` はディレクトリ内で一意の非ゼロ値で INT64_MAX 以下、昇順は不要です。ゼロは巻き戻しです。`seek_offset` は独立した符号なし64ビット d_seekoff 観測値で、ゼロの重複も可能です。整数は stat と同じ損失のない十進文字列規則です。

`contents.minimum_buffer_size` は EOF を含むペイロード最小値（1–128 MiB）として必須です。項目の任意の `minimum_buffer_size`（既定0）は、その位置から始まる読み出しをさらに制約します。例は APFS で最初のドット2項目に64バイト、EOF に1バイトを要した観測です。他の位置でも完全な1記録が必要です。LP64 記録長は `roundUp(25 + nameBytes, 8)`、8バイト整列です。全一覧合計4096項目まで、記録バイトも16 MiB予算に算入します。メタデータ/一覧だけを持つ祖先パスは重複なしで256パス制限に算入し、JSON は64 KiBまでです。

独立 open は独立位置、dup は共有位置です。ゼロか宣言済みの値だけで再開し、未知の位置は停止します。収まる完全記録を順に返します。要求長 >=1024 は元の末尾4バイトに EOF（末尾1、それ以外0）を予約し、記録部分だけを128 MiBに制限します。末尾アドレスは元の符号なし演算とラップを保持します。データ、位置更新、読出し前位置、フラグの順で処理し、後段 EFAULT は先行効果を保持します。EOF は空データをコピーしません。部分的に書込可能な個別コピーはその前で未対応停止し、以前の効果は保持します。

同じ `directory-entries` がネイティブ macOS と記録・dup/巻戻し・小分け読出し・EOF・コピー順序を比較します。別テストで SDK 配置と実記録の全バイト、長い名前を比較します。固定スナップショット値は巻戻しでも変わらず、APFS の動的世代は再現しません。旧 `getdirentries` (196)、変更後の列挙、他のネイティブバックエンド、iOS実機は未検証です。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"directories":[{"path":"/empty"},{"path":"/","contents":{
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":73,"type":8,"next_offset":99,"seek_offset":0}]}}]}}
```

[XNU getdirentries64](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [dirent ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [extended flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent_private.h).

列挙検証（2026-10-05、Release）：Darwin 498件中246成功、利用不能バックエンド252件スキップ、失敗0。ARM64 HVF 必須63/63件を実行。ネイティブmacOS 11プログラム、C/CLI/レポート40件（スキップなし）、Python 5構成で各8ファイルワークロード、ランナー66件が成功しました。計数は重複します。証跡：`build-hvf-arm64/darwin-dirents-merged-evidence/`。Intel HVF Actions は停止中で、他のネイティブバックエンドとiOS実機は未検証です。

## プライベートファイルマッピング

`mmap` は通常のカタログファイルの `MAP_PRIVATE` に対応します。`flags=0x2`、または `MAP_UNIX03` を加えた `0x40002` を指定し、オフセットは OS ページに整列させます。要求長が短くてもページ内の元のファイルバイトを保持し、EOF 最終ページの残りはゼロです。書き込みは現在のマッピングだけを変え、元のファイル、他のマッピング、固定メタデータ、共有カーソルを変えません。close や記述子再利用後も有効です。読み取り専用や PROT_NONE の初期内容を保持し、`mprotect` で書き込みを許可できます。

ファイル終端の算術オーバーフロー、UNIX03 の長さゼロや未整列オフセットは FD 検索前に EINVAL、無効な FD は予算検査前に EBADF です。従来の長さゼロも FD を検査してから割り当てなしでゼロを返します。従来の未整列オフセット、ストリーム、空ファイルのページ、EOF を完全に超えるページは割り当て前に未対応として停止します。macOS は EOF 外のマッピングを作成できますがアクセスで SIGBUS を発生させるため、モデルは読み取り可能なゼロページやシグナル配信を捏造しません。共有、固定、実行可能、JIT マッピングは未対応です。

`DarwinFiles` が記述子とバイトを解決し、`DarwinMemory` が配置、権限、予算、ロールバックを管理します。入力は `darwin_files` のみです。同じ `file-mapping` プログラムがネイティブとゲストでコピー、close、カーソル、エラー、匿名ページ再利用を検査します。独立したネイティブ比較は非ゼロオフセット、ページ全体と SIGBUS 境界を検証します。

[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c)

### プライベートマッピングの検証（2026-10-05）

Release Darwin は438件の一意な登録を照合し、210件成功、228件スキップ、失敗ゼロでした。ARM64 HVF 必須57/57件を実行し、Unicorn は5種類のゲストを検証しました。ネイティブmacOSの9プログラム、非ゼロオフセットの全ページ比較、隔離子プロセスのSIGBUS確認が成功しました。公開API/レポート36件はスキップなし、Pythonの5組合せ（`file-mapping` を含む）、検証スクリプト66件、来歴回帰38件も成功しました。件数は重複します。証拠：`build-hvf-arm64/darwin-mmap-verified-evidence/`。Intel HVF/KVM/WHPやiOS実機の追加証拠はなく、Intel HVF Actionsは停止したままです。

## 明示的なファイルメタデータ

ファイル項目には `metadata` を追加できます。指定する場合、下の全フィールドが必須です。10進文字列は整数の全幅を保持し、JSON 数値は ±(2^53−1) 内の正確な整数に制限されます。device は符号付き32ビット、mode/link_count は符号なし16ビット、inode は符号なし64ビット、uid/gid/flags/generation は符号なし32ビットです。size はファイルのバイト数と一致し、blocks は符号付き64ビット上限以下、block_size は非負の符号付き32ビットです。時刻は符号付き64ビット秒と 0–999999999 ナノ秒を使います。

`stat64` (338)、`fstat64` (339)、`lstat64` (340) は ARM64/x64 で同じ144バイト LP64 レコードを返します。open とパス解決を共有し、FD の複製と close を反映します。FD やカーソルは変更せず、rdev、パディング、予約領域はゼロです。入力は初期メタデータを与え、変更は任意のポリシーに従います。read は時刻を更新せず、mode はアクセス許可を変えません。未指定メタデータ、ストリーム、シンボリックリンク、旧 stat、拡張セキュリティは未対応です。パス/FD エラーを出力ポインターより先に処理し、部分的に書ける出力は変更前に停止します。ネイティブ試験は実ファイルの全バイトと SDK 配置を比較し、同じ独自プログラムで3呼び出しを検証します。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","metadata":{
  "device":1,"inode":"18364758544493064720","mode":33188,"link_count":1,
  "uid":1000,"gid":1000,"size":10,"block_size":4096,"blocks":8,
  "flags":0,"generation":0,
  "access_time":{"seconds":-1,"nanoseconds":1},
  "modification_time":{"seconds":2,"nanoseconds":3},
  "change_time":{"seconds":4,"nanoseconds":5},
  "birth_time":{"seconds":6,"nanoseconds":7}
}}]}}
```

[XNU stat.h](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/sys/stat.h)

### メタデータ検証と残作業（2026-10-05）

stat64 追加後の Release 検証は409件：成功193、スキップ216、失敗0。必須 ARM64 HVF は54/54件実行し、Unicorn は5つのゲスト組合せを検証しました。SDK 配置と実レコード全体の比較、ネイティブ8プログラム、公開 API/レポート36件（スキップなし）、Python の5組合せ、検証スクリプト66件も成功しました。件数には重複があります。ネイティブ試験の出力をケース別に分け、短い出力に以前の末尾が残る問題を修正しました。Intel HVF/KVM/WHP と iOS 実機の追加機能の証拠はありません。

次は共有マッピングと EOF フォールト、有界書き込み（EOF ページ、close 後の寿命、エラー順序を検証）、明示的な時刻/システム情報、必要な Mach/スレッド、Mach-O 依存関係・再配置/バインド・初期化/TLS の順です。Objective-C/Swift と Foundation/UIKit は実行可能なネイティブ例で進めます。iOS 実機には SDK と端末が必要です。Intel HVF は未検証で Actions を停止したままです。



```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

独立したワークロード検証は ARM64 108 件または x64 72 件をすべて要求し、各プラットフォームの `LC_MAIN` と `LC_UNIXTHREAD` を含みます。必須項目の欠落、スキップ、`ld64.lld` の不足はいずれも失敗です。

```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

Linux は `kvm`、Windows は `whp` を使用します。[Darwin ワークフロー](../../.github/workflows/darwin-native.yml) は Unicorn なしで両 x64 転送層を検証し、個別再実行も可能です。[カーネル参照](../../.github/workflows/darwin-kernel-reference.yml)は NeverD/LLVM なしで両 macOS ISA 上のプログラムを直接実行します。`DarwinNativeCases.def` がモード、終了状態、期待バイトを管理します。実際の dyld エントリ用に libSystem をリンクするのはホスト参照だけです。ISA 不一致、Rosetta、タイムアウト、結果の差異は失敗し、iOS 実機カーネルの証拠とは区別します。

## 証拠と残る範囲

2026-10-03 の結果です。重複する行は合算しません。

| 転送層 | ソース | 合格 | 失敗 | スキップ | ネイティブワークロード |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 HVF | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel HVF | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| x64 KVM | `36e11ca8a` | 51 | 0 | 235 | 26/26 |
| x64 WHP | `36e11ca8a` | 51 | 0 | 235 | 26/26 |

[Intel 実行](https://github.com/NeverSight/NeverD/actions/runs/37106013999)は 286 件の CTest 識別子と 32 プロセスを元の XML と照合しました。234 件のスキップは、無効な Unicorn 65 件、ARM64 ゲスト 39 件、他のホストプラットフォーム 130 件です。成果物 `11267489438` の SHA-256 は `cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef` と検証済みです。[KVM/WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703) も独立に照合しました。[カーネル参照](https://github.com/NeverSight/NeverD/actions/runs/37064795867)は各 ISA で 4/4 プログラムが合格し、終了状態 37、完全一致の出力、空の stderr を確認しました。

Unicorn ありの C API/CLI は合格 138、スキップ 156、失敗ゼロです。Python は五つの組み合わせをカバーします。パッケージのエンジンは ARM64 CLI の 18 レポートと一致し、186 個の Mach-O 署名を確認しました。HVF/Unicorn OFF では 38 合格、231 スキップで、Hypervisor.framework をリンクしません。これらは統合証拠であり、追加のネイティブ実行数ではありません。Intel CPU 全体は未検証です。[HVF](macos-hvf.md)と[詳細記録](../darwin-emulation.md#hosted-native-verification-2026-10-03)を参照してください。

## 明示的な時刻の観測値

`ProcessOptions::DarwinTime` / `darwin_time` は、すべての Darwin profile で raw `gettimeofday` (116) に固定の観測値を渡します。第3出力 `mach_absolute_time` も含みます。`time_of_day`、`timezone`、`mach_absolute_time` はそれぞれ省略可能で、省略は不明、明示的なゼロは値です。空のオブジェクトは既定の時計を作りません。ホスト時計の参照、タイムゾーンの推測、時間の進行、絶対 tick の換算は行いません。

指定するレコードには全メンバーが必要です。`seconds` は符号なし32ビット、`microseconds` は [0, 999999]、`minutes_west` / `dst_time` は符号付き32ビット、絶対 tick は符号なし64ビットです。JSON は共通の無損失整数規則に従い、安全な整数範囲外では10進文字列を使います。不明なフィールド、範囲外の値、Darwin 以外の profile はイメージ読込前に拒否されます。

LP64 `timeval` は16バイトで、オフセット0がゼロ拡張した秒、8が32ビットのマイクロ秒、12の4バイトはゼロです。タイムゾーンは符号付き32ビット値2個、tick は8バイトです。暦時刻と絶対時刻は最初に同時採取するため、要求した両観測値がコピーやポインタ検査より先に必要です。その後 timeval、timezone、absolute ticks の順でコピーします。タイムゾーンの欠落や後続の EFAULT は、それ以前の書込みを保持します。重複アドレスも同順序です。個々の出力が一部しか書込み可能でない場合、そのコピー前に未対応として停止し、以前のコピーは残します。全ポインタが null なら設定不要で成功し、個別問い合わせは要求した値だけを必要とします。

独自の `time` ワークロードでネイティブ動作を確認し、`time-values` は5種類のゲストの C/CLI/Python で指定した32バイトを出力します。別の SDK オラクルは1回の raw ネイティブ呼出しで3出力を採取し、全バイトを比較します。時計進行、換算、commpage カウンタ、タイマー、Mach 時計オブジェクト/IPC は対象外です。dyld、スレッド、Objective-C/Swift、Foundation/UIKit は引き続き必要です。Intel HVF Actions は停止中で、ネイティブ Intel と実機 iOS の検証は追加していません。

```json
{"darwin_time":{"time_of_day":{"seconds":4045620583,"microseconds":654321},"timezone":{"minutes_west":-480,"dst_time":-1},"mach_absolute_time":"18364758544493064720"}}
```

[XNU gettimeofday](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_time.c), [time ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/time.h).

時刻検証（2026-10-06、Release）：Darwin 登録538件、成功274件、利用不能バックエンドのスキップ264件、失敗0件。必須 ARM64 HVF は66/66件を実行しました。独自のネイティブ macOS 12プログラムと単一サンプルの SDK バイト比較も成功。公開 C/CLI/レポートは43/43件、スキップなし。Python は時刻の完全なバイト列と既存8ファイルモードを含む5ゲスト構成で成功。ランナー66件、翻訳、機能一覧、書式検査も成功。件数は重複します。証拠：`build-hvf-arm64/darwin-time-verified-evidence/`、`darwin-time-native-first/`、`darwin-time-public.xml`。

## Mach 時刻と戻り規約

`darwin_time.timebase` の `numerator` と `denominator` は非ゼロの符号なし32ビット値で、約分・換算せず保持します。`mach_timebase_info_trap` の番号89は ARM64 X16=-89、x64 RAX=0x01000059 です。分子・分母をリトルエンディアン8バイトで書き、ゼロを返します。完全に無効な出力先でもゼロを返しますが、一部だけ書ける出力はコピー前に停止し、転送自体のエラーは伝播します。未設定の timebase は null を含むポインタ検査より先に停止します。

ARM64 X16=-3 と X16=-4 は `mach_absolute_time` と `mach_continuous_time` の符号なし64ビット全体を返します。それぞれ自身の値だけが必要で、明示的なゼロも有効です。x64 の該当ネイティブ表項目は EXC_SYSCALL を起こすため未対応です。時刻進行、commpage、タイマー、Mach 時計オブジェクト/IPC は未実装です。

分派は番号の下位32ビットを使い、報告は元の64ビットを保持します。ARM64 の負数は Mach、x64 の Mach は0x01000000、BSD は0x02000000です。BSD 3/4 は read/write のままで、未知番号や別形式のクラスは停止します。解決済みの対応表が戻り規約を決めます。Mach はフラグと X1/RDX を保持し、BSD は既存の carry 規則に従います。x64 の RCX/R11 更新は残ります。Mach 報告は `result` を持ち、入力 carry にかかわらず `error` を省略します。

`mach-time` は ARM64 ネイティブでフラグ、次結果、上位番号ビット、不正ポインタ、BSD への遷移を比較します。`mach-timebase-values` は5ゲスト構成、`mach-clock-values` は ARM64 で正確なバイト列を検証し、SDK は配置と採取した比率を照合します。Intel HVF Actions は停止中で、x64 ソフトウェア・構文検査はネイティブ Intel や実機 iOS の検証を意味しません。

```json
{"darwin_time":{"timebase":{"numerator":125,"denominator":3},"mach_absolute_time":"18364758544493064720","mach_continuous_time":"18446744073709551615"}}
```

[XNU clock traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/clock.c), [ARM64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [ARM64 special traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/sleh.c), [x64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/x86_64/idt64.s).

Mach 検証（2026-10-06、Release）：Darwin 569件、成功293、利用不能バックエンド276件スキップ、失敗0。必須 ARM64 HVF 69/69を実行し、最終実行でネイティブ13ワークロードと2つの時刻 SDK 比較が成功しました。公開 C/CLI/report は100/100、スキップなし。Python は5ゲストを検証しました。公開比較はプラットフォーム・ワークロード別で、明示的な10秒の実行予算を使用します。製品既定値と期限回帰は変更しません。計数は重複します。

初回ネイティブ起動は既存の5秒制限を超え、独立計測で6.056秒、再利用時0.010秒でした。同じバイナリの再試行は元の制限で13件成功し、失敗記録も保持しています。ホスト負荷下のタイムアウト後、個別の逐次検証は成功しました。証跡：`build-hvf-arm64/darwin-mach-time-final-evidence/`、`darwin-mach-time-native-recheck/existing-binary-recheck.json`、`darwin-mach-time-public-accepted.xml`。コミット前の作業ツリーです。その時点では ARM64 MRS/MSR NZCV が未対応だったため、整数命令でフラグを観測していました。以下の変更でこの CPU 不足を解消します。

## ARM64 条件フラグレジスタ

共通の checked ARM64 契約は EL0/EL1 で正確な `MRS Xt, NZCV` と `MSR NZCV, Xt` の符号化を許可します。読み出すのはビット31–28のみで、書き込みも入力のその4ビットだけを使い、他は無視します。`XZR` への読み出しは破棄され、`XZR` からの書き込みは SP を読まず4フラグをクリアします。各バックエンドは元の命令を実行します。ホストのレジスタ設定検証と FPCR/FPSR の制限は変更せず、近隣の未登録システムレジスタは未対応のままです。

`NeverDAArch64NZCVTests` は全フラグ組合せをホスト命令と比較し、全スカラ/ベクトル状態、メモリ、レジスタ境界、監視停止/失敗、コンテキスト復元後の再試行、共有命令予算を検証します。ARM64 `mach-time` は SVC の前後で実際の MSR/MRS を使い、Mach のフラグ保持と BSD への遷移を確認します。ネイティブ HVF 必須項目には両権限の6メソッドとホスト比較を含めます。ARM64 KVM/WHP と実機 iOS は未検証です。書込み可能ファイル、システム情報、進行する時計、Mach IPC/スレッド、dyld/ランタイム/フレームワークと実機受入れは引き続き環境整備の対象です。

[Arm NZCV (DDI0601, 2025-06)](https://developer.arm.com/documentation/ddi0601/2025-06/AArch64-Registers/NZCV--Condition-Flags).


変更可能ファイルの検証（2026-10-06）：Release Darwin は610項目中322成功、利用不能バックエンド288スキップ、失敗0。ARM64 HVF 必須72/72を実行。最後に追加した EFAULT メタデータ検査を含む集中テストは114項目中102成功、12スキップ。ネイティブ15プログラムと公開 C/CLI/レポート111項目も成功しました。件数は重複します。最初のネイティブ実行で FWASWRITTEN の欠落を検出し、修正後に成功、失敗証拠は保持しています。期限は不変で、GitHub 全体 CI と iOS 実機は別途検証が必要です。Intel Actions は停止中です。

`build-hvf-arm64/writable-darwin-evidence/` · `writable-native-final/` · `writable-focused-final.xml` · `writable-public.xml`

Python の初回全体テストで ARM64 の3ディレクトリ例がタイムアウトしました。同じ引数の診断で新規書き込み10/10は成功、iOS の1例は実時間5.005秒・CPU1.263秒で停止。5秒制限を変えない個別再検査は3例とも成功（2.43–3.17秒、10,941命令、出力65）。16論理CPUで負荷54–70はスケジューリング圧力を示しますが、安定した遅延の保証ではなく初回失敗も保持します。

最後の未変更 Python 統合メソッドは5構成すべて成功し、合計41.118秒でした。各プロセスの5秒制限は不変で、前の失敗・診断記録は別に保持します。


メタデータ検証（2026-10-06）：Release単体148件は124成功/24未対応バックエンドskip。全Darwin645件は343成功/300skip/既存ARM64 HVFディレクトリ2件timeout。同条件・元の5秒上限で20件を再検証し8成功/12skip、対象は3.818/3.949秒でした。必須HVF75件すべてに成功観測がありますが初回失敗は保存します。公開C/CLI/レポート117/117（Darwin73）、Python5構成27.359秒、ネイティブ15/15、runner66/66成功。割り当てはAPFS証拠ではありません。期限変更なし、全CI・Intel・iOS実機・完全な環境は未完了です。

`build-hvf-arm64/mutation-metadata-validation-summary.json`; `mutation-metadata-darwin-evidence/`; `mutation-metadata-directory-recheck/`; `mutation-metadata-focused.xml`; `mutation-metadata-public.xml`.

疎ファイル位置検証（2026-10-06）：Release Darwin 671 件中 359 成功、利用不可バックエンド 312 スキップ、失敗ゼロ。必須 ARM64 HVF 78 件をすべて実行し、Unicorn は五つのゲストを検証。対象テストは 147 件中 123 成功、24 スキップ。ネイティブ 16、公開 C/CLI/report 122（Darwin 比較 78）、Python 五構成（12.344 秒）、runner 66 が成功。件数は重複し、期限は変更せず、過去の失敗も保存。証拠：`build-hvf-arm64/sparse-seek-validation-summary.json`。割り当て規則は明示的な仮想ポリシーであり、APFS 同等性ではありません。完全な CI と iOS 実機検証は別途必要で、Intel HVF Actions は停止中です。

unlink 検証（2026-10-06）：Release Darwin 708 件中 384 成功、利用不可324スキップ、失敗ゼロ。必須 ARM64 HVF 81 件すべて実行。対象156件は137成功/19スキップ、ネイティブ17/17、C/CLI/report128/128（Darwin83）、Python五構成16.268秒、runner66/66成功。独立設計・実装レビューに残る阻害事項なし。件数重複、期限不変、再試行不要。証拠：`build-hvf-arm64/unlink-validation-summary.json`。親の失効と固定時刻はモデル規則であり、完全なファイルシステム/ランタイムやiOS実機の検証ではありません。Intel HVF Actions は停止中、完全CIは別途必要です。

### 作成の検証、2026-10-06

Release Darwin は748登録、412成功、336バックエンド不在のスキップ、失敗なし。必須ARM64 HVF 84件を実行。重点162件は150成功/12スキップ。C/CLI/レポート133/133（Darwin88）、Python5構成9.982秒、ネイティブ18/18、runner66/66成功。初回ARM64は試験用ポインタ表の再配置を正しく拒否し、インラインバイトへ変更して成功。ローダーを緩和せず、初回失敗とバイナリを保存。runner期待数は27から28へ更新。独立レビューに阻害事項なし、親観測を保持する容量失敗も検証。件数は重複、時間制限は不変。完全なCIと実機iOSは別途、Intel HVF Actionsは停止中。

`build-hvf-arm64/create-validation-summary.json`, `create-darwin-evidence/`, `create-focused.xml`, `create-public.xml`, `create-native-final/`, `create-initial-evidence/`.

### 作成メタデータ検証、2026-10-06

Release Darwin は登録 787 件：成功 439、利用不可バックエンドのスキップ 348、失敗 0。必須 ARM64 HVF 87 件をすべて実行。対象 151 件は成功 139、スキップ 12。公開 C/CLI/レポートは 145/145（Darwin 入力比較 98）、未変更の Python メソッドは5構成を 12.211 秒で成功。ネイティブ 19/19、検証スクリプト 66/66 成功。独立レビューに阻害事項なし。別親の device/GID と全体 inode、最初の書込み前 unlink、FD/入力が使えない umask を追加検証。件数は重複し、期限不変、失敗再試行は不要。固定作成/変更時刻と割当は仮想ポリシーです。完全 GitHub CI と iOS 実機は別途必要、Intel HVF Actions は停止中です。

`build-hvf-arm64/creation-metadata-validation-summary.json`, `creation-metadata-darwin-evidence/`, `creation-metadata-focused.xml`, `creation-metadata-public.xml`, `creation-metadata-native/`.

### 改名の検証、2026-10-06

Release Darwin は835件：成功474、利用不可スキップ360、既存 macOS ARM64 HVF 仮想メタデータ1件が5.087秒で期限超過。同じ引数・5秒制限の20件再確認は成功8、スキップ12、該当項目0.113秒。両実行で必須 ARM64 HVF 90件の成功観測を網羅しますが、完全ゲートの失敗記録は維持します。改名対象は42/54成功、12スキップ。C/CLI/レポート150/150（Darwin比較103）、未変更Pythonの5構成18.478秒、ネイティブ20/20、スクリプト66/66。独立レビューで入れ子のドット分類を修正し、4K/16K回帰は修正前失敗・後成功。以前の読取り専用 ftruncate テスト期待値を EINVAL に訂正。失敗とプローブ各版を保持し、件数は重複、期限は不変。完全GitHub CI・iOS実機は別途、Intel HVF Actionsは停止中。

`build-hvf-arm64/rename-validation-summary.json`, `rename-focused-final.xml`, `rename-darwin-final-evidence/`, `rename-metadata-recheck.xml`, `rename-public.xml`, `rename-native-final/`, `rename-review-initial-evidence/`.

## 明示的なシステム観測値

`ProcessOptions::DarwinSystem` / `darwin_system` は、すべての Darwin profile の `sysctl(202)` と raw `sysctlbyname(274)` に固定観測値を渡します。各フィールドは省略可能で、未指定の値や一覧外のキーは未対応として停止します。ホストへの問い合わせや版・機種の推測はありません。厳密な JSON と C++ 検証は、読み込み前に不正値と Darwin 以外の profile を拒否します。

`os_revision` は符号付き32ビット、`cpu_count` は1～INT32_MAX、`memory_size` は符号なし64ビットをすべて保持します。他は最大1023バイトで内部 NUL のない文字列です。明示的な空文字列も有効で、出力には終端 NUL を含みます。CPU 数やメモリ量の報告はスケジューリングや割当予算を変えません。

| JSON フィールド | sysctl 名 | MIB |
| --- | --- | --- |
| `os_type` | `kern.ostype` | `1,1` |
| `os_release` | `kern.osrelease` | `1,2` |
| `os_revision` | `kern.osrevision` | `1,3` |
| `kernel_version` | `kern.version` | `1,4` |
| `os_version` | `kern.osversion` | `1,65` |
| `machine` | `hw.machine` | `6,1` |
| `model` | `hw.model` | `6,2` |
| `cpu_count` | `hw.ncpu` | `6,3` |
| `memory_size` | `hw.memsize` | `6,24` |

`hw.pagesize` は既存のゲストメモリ方針を使用し、通常8バイト、非 null 出力の容量がちょうど4なら4バイトです。旧 MIB `[6,7]` と `hw.pagesize_compat` は常に4バイトです。`hw.pagesize` の動的数値 OID は未対応です。`hw.memsize` も容量4では、64ビット値が符号付き32ビット値の符号拡張と一致する場合だけ縮小します。それ以外は ERANGE34 で出力と長さを保持します。

MIB 数は下位32ビットで2～12、名前長は64ビット全体で1024未満です。指定された全バイトを検査してから最初の NUL を解釈し、末尾の点を一つ除きます。空の名前は ENOENT、部分的に読める入力は未対応です。非 null `oldlenp` は副作用前に8バイト全体の読み書きが必要です。不正な長さポインタのネイティブ試験は期限内に戻らなかったため、明示的な未対応範囲とします。null `oldlenp` は容量0、null `oldp` はサイズのみの問い合わせです。短い領域では ENOMEM12、データは不変で長さ0です。データ EFAULT は元の長さを保持します。入力と容量の取得、データ、最後の長さという順序で、別名と後続の転送失敗時の既存コピーを保持します。

`newp` と `newlen` が両方非ゼロの場合だけ書き込みです。選択されたノードは、モデルの固定非 root 身元に対して観測値・出力検査前に EPERM1 を返します。ネイティブでは特権書き込み可能な `kern.osversion` も含みます。新しい長さ0ならポインタを無視します。不明なキー、他のツリー、動的 OID に ENOENT を推測しません。

独自の `system-info` はネイティブ macOS とゲスト ABI を検査し、`virtual-system` は C++、C/CLI、Python で設定済みバイト列を比較します。別の SDK 検査はホストの九つの観測値を明示的なテスト入力として名前・数値出力を照合します。iOS 実機や Intel HVF の検証ではありません。

```json
{"darwin_system":{"os_type":"Darwin","os_release":"24.test","os_revision":0,"kernel_version":"Virtual kernel","os_version":"V42","machine":"virtual64","model":"VirtualModel","cpu_count":4,"memory_size":"17179869184"}}
```

[XNU sysctl](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_newsysctl.c), [XNU hardware MIB](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mib.c), [Apple sysctl(3)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/sysctl.3.html).

### システム問い合わせの検証、2026-10-06

Release Darwin は881登録、509成功、利用不可372スキップ、失敗なし。必須 ARM64 HVF 93件をすべて実行。重点49件は37成功・12スキップ。C/CLI/レポート163/163（Darwin 比較113）、未変更 Python の5構成15.302秒、独自ネイティブ21/21、検証スクリプト66/66が成功しました。独立レビューに阻害事項はなく、追加のエラー優先順位と SDK 捕捉比較も成功。新しい SDK 検査は StringExtras ヘッダー不足で一度コンパイルに失敗し、追加後に成功しました。元のログ・ソースと、未対応境界に置いた不正長さポインタのネイティブ記録を保持します。検証後は二つのファイル先頭コメントだけを整理して再ビルド成功。件数は重複、期限は不変、実行失敗の再検査は不要でした。完全 GitHub CI と iOS 実機は別途、Intel HVF Actions は停止中です。

`build-hvf-arm64/sysctl-validation-summary.json`, `sysctl-darwin-final-evidence/`, `sysctl-focused-final.xml`, `sysctl-public.xml`, `sysctl-native-final/`, `sysctl-sdk-build-failure/`, `sysctl-initial-probe-evidence/`.

## ベクトルファイル入出力と出力捕捉

`readv`/`writev`、`preadv`/`pwritev` と nocancel 入口は、スカラーのファイル・出力捕捉実装を共有します。新しい設定やホストアクセスはありません。LP64 iovec は8バイトのアドレスと8バイトの長さです。iovcnt の符号付き下位32ビットは1–1024で、記述子検索より前に配列全体をコピーします。出力との別名でも要求は変化しません。部分的に読める配列は未対応です。

アクセス権とストリームの位置指定可否を先に検査し、その後、各長さと合計を INT64_MAX 以下に制限します。通常ファイル・ディレクトリの合計はさらに INT_MAX 以下です。有限 stdin は残りの入力に短縮し、捕捉は出力予算に従います。pwritev の負の位置は配列を読む前に拒否し、preadv の位置検査は記述子と長さの後です。空の項目はアドレスを無視しますが、記述子・種類・位置の検査は残ります。EOF 後の項目には触れません。位置指定ではカーソルを変えず、pwritev は追記を無視します。通常の追記は元のカーソルで要求全体を一度だけ短縮してから EOF を選びます。

後続の完全に無効な項目は EFAULT を返し、完了済みのバイト、通常カーソルの進行、非ゼロバイトを書いた場合の FWASWRITTEN を保持します。承認済みの非ゼロ書き込みがデータバッファの EFAULT を返す場合、完全なメタデータを無効化します。引数エラー、モデルの受付拒否、バックエンド障害では保持します。部分的に書ける読み取り先は UnsupportedService で停止し、現在の項目はコピーせず、前のコピーは残ります。部分的に読めるファイル書き込み元は全ファイル効果の前に未対応で停止します。権限・マッピングリース・合計保存予算を先に検査し、バックエンドの事前検査・読み取り失敗ではファイルや捕捉を公開しません。

捕捉は stdout/stderr 共通予算を先に検査します。ユーザーアドレス上限をまたぐ項目はコピーせず、先行項目だけ保持します。その他の部分入力は確認済み接頭部を保持して EFAULT です。スカラー範囲エラーは従来どおり予算より優先します。複製・転送記述子は元の出力先を保持します。独自 `vectored-io` がネイティブ macOS、5つのゲスト構成、公開 C/CLI/Python で8入口を検証します。キャンセル、パイプ、スレッド、実機 iOS の受け入れは含みません。

`readv`: 120/411; `writev`: 121/412; `preadv`: 540/542; `pwritev`: 541/543.

[XNU vector calls](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU iovec lengths](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_subr.c), [XNU vnode I/O](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

### ベクトル I/O 検証、2026-10-06

Release Darwin は937登録、553成功、未提供バックエンド384スキップ、失敗なし。必須 ARM64 HVF 96件をすべて実行しました。対象検査は45/57成功、12スキップ。公開 C/CLI/report は168/168成功、うち Darwin 入力比較118件。Python は5構成を20.397秒で検証、ネイティブ22/22、検証スクリプト66/66成功。独立レビューで疎な位置指定書き込みの障害テストを追加し、カーソル、実際の EOF、メタデータ拒否、残存容量を検証しました。初回ビルドは旧テストの削除済み内部問い合わせ参照で失敗し、実出力の検証に変更しました。新イベント断言の optional<bool> 誤用で正常な8ゲスト実行が失敗扱いになりましたが、修正後すべて成功。両失敗のソースとログを保存しています。件数は重複し、制限時間は不変。完全な GitHub CI と実機 iOS は別検証、Intel HVF Actions は停止継続です。

`build-hvf-arm64/vector-validation-summary.json`, `vector-darwin-final-evidence/`, `vector-focused-final.xml`, `vector-public.xml`, `vector-native-initial/`, `vector-initial-build-failure/`, `vector-initial-assertion-evidence/`.

## ファイルの存在確認

`access(33)` と `faccessat(466)` は現在の仮想カタログを照会し、記述子の割り当てや内容・カーソル・フラグ・メタデータの変更を行いません。F_OK は既存の探索契約で名前の存在を確認します。メタデータはカタログのアクセス権を付与・撤回せず、祖先ディレクトリの検索権限、ACL、MAC のネイティブ検証ではありません。stat 観測が不明でも照会可能です。古い FD やマッピングが残っても削除名は ENOENT となり、作成・名前再利用・改名は現在の名前空間に従います。

モードは下位32ビットです。R/W/X はビット0–2、拡張権限は9–21を使用します。`(mode & 0x003ffe07) == 0` が存在確認で、符号を含む他のビットは EINVAL にせず無視します。権限要求は探索成功後に UnsupportedService となり、許可を示すようなメタデータや変更許可から推測しません。既知のパス・記述子エラーが先です。

Faccessat は下位フラグ AT_EACCESS(0x10)、AT_SYMLINK_NOFOLLOW(0x20)、AT_SYMLINK_NOFOLLOW_ANY(0x800) の任意の組合せを受け入れます。それ以外は、カタログ未設定でもパス・FD の前に EINVAL です。現在のカタログはシンボリックリンクを含まず、実/実効 ID は固定です。絶対パスは dirfd を無視し、相対パスは設定済み CWD/ディレクトリ FD に従います。最初の NUL までコピーしてから相対 FD を検査し、文字列の欠落バイトは EFAULT です。空の相対パスでも未知 FD は EBADF、通常ファイル FD は ENOTDIR、それ以外は ENOENT。未設定カタログや未知のストリーム種別は未対応です。

独自 `file-access` はネイティブ macOS と5ゲスト構成、C++/C/CLI/Python で両入口、無視ビット、フラグと順序を比較します。NOFOLLOW_ANY は相対ディレクトリ FD を使い、ホスト `/tmp`、`/var` のリンクに左右されません。直接テストは名前の変更、混合権限ビット、FD 枯渇、メタデータ非依存とメモリエラーを確認します。

[XNU access and faccessat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU access mode bits](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/unistd.h).

### ファイル存在確認の検証、2026-10-06

Release Darwin は971登録、575成功、未提供バックエンド396スキップ、失敗なし。必須 ARM64 HVF 99件を実行しました。対象35件は23成功・12スキップで直接14件を含みます。公開 C/CLI/report は173/173成功、うち Darwin 入力比較123件。Python は5構成を16.235秒で検証、ネイティブ23/23、検証スクリプト66/66成功。独立した設計・実装レビューに阻害要因なし。ホスト /tmp のリンクによる初期 NOFOLLOW_ANY 結果と正規パス比較を保存し、共通ワークロードは相対ディレクトリ FD を使います。件数は重複、制限時間は不変、実行失敗の再検査は不要でした。完全な GitHub CI と実機 iOS は別検証、Intel HVF Actions は停止継続です。

`build-hvf-arm64/access-validation-summary.json`, `access-darwin-final-evidence/`, `access-focused-final.xml`, `access-public.xml`, `access-native-initial/`, `access-evidence/`.

## ディレクトリの作成と削除

`mkdir(136)` と `mkdirat(475)` は明示的に変更可能な直接の親内に作成します。新しいディレクトリは名前空間変更権限を継承し、初期ディレクトリには個別の許可が必要です。既知の親デバイス/GID だけを継承し、完全な stat、サイズ、割当、時刻、列挙位置は推測しません。再利用したファイル名の古い観測は遮蔽します。`creation_policy` は通常ファイル専用で、配下のファイルは継承した親識別と既存の全体 inode 列を使用し、mkdir はその inode を消費しません。権限強制とネイティブなディレクトリメタデータは対象外です。

共通の要素別探索で、mkdir は末尾スラッシュだけが続く欠落名を作成できます。欠落祖先後の点/二点は ENOENT、ファイル祖先は ENOTDIR、既存名は EEXIST。相対 FD/CWD、絶対パスの FD 無視、文字列障害優先を維持します。空き FD は不要で、探索・許可・容量・転送の拒否は名前や親観測を変更しません。

`rmdir(137)` と AT_REMOVEDIR(0x80) 付き `unlinkat(472)` はこのプロセスが作った空ディレクトリを削除し、AT_SYMLINK_NOFOLLOW_ANY(0x800) も併用可能です。未知の下位32ビットは入力前に EINVAL、DATALESS と SYSTEM_DISCARDED は未対応。既知のパス/型/ルートエラーを保持し、removable 許可のない初期ディレクトリ削除は引き続き UnsupportedService。許可された対象の末尾点は EINVAL、リンク中のディレクトリからの二点・非空対象は ENOTEMPTY。ディレクトリ FD、dup、CWD は元のオブジェクトを保持し、削除を妨げません。unlink 済み通常ファイルの FD/マッピングは名前ではなく、親削除・再利用後も内容、inode、最終 F_GETPATH が残ることをネイティブ対照で確認します。

新規ディレクトリの正規パス+NUL と1項目を共通16 MiB/256項目予算に計上し、削除後に参照がなくなるとそれだけ返却します。孤立ファイルとマッピングは保持します。成功時だけ直接の親の stat/列挙観測を無効化し、新規ディレクトリの完全な観測は常に未知です。独自 `directory-mutations` はネイティブ macOS、5ゲスト、C++/C/CLI/Python で入れ子作成・改名・unlink・削除・孤立物体の再利用を比較します。

[XNU mkdir/rmdir](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU directory creation lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_lookup.c).

### ディレクトリ変更の検証、2026-10-06

Release Darwin: 1,017登録、609成功、未提供バックエンド408スキップ、失敗なし。必須 ARM64 HVF 102件を実行。対象58成功・12スキップ、新規4K/16K直接26件を含みます。公開 C/CLI/report178/178（Darwin入力128）、Python5構成17.255秒、ネイティブ24/24、検証スクリプト66/66。独立レビューは予算、名前再利用、親の同一性、ファイルリース、ロールバックを確認。最初の成功後の追加ネイティブ検査で、スラッシュのみのルート削除は EISDIR、末尾点/二点は EBUSY と判明し、共通判定と共通テストを修正しました。以前の結果とソース/実行ファイルを保存。件数重複、期限不変、Intel HVF Actions停止継続。全GitHub CIと実機iOSは別検証。

`build-hvf-arm64/directory-mutation-validation-summary.json`, `directory-mutation-darwin-final-evidence/`, `directory-mutation-focused-final.xml`, `directory-mutation-public-final.xml`, `directory-mutation-native-final/`, `directory-mutation-before-root-fix/`.

## 保持されたディレクトリ識別

削除後も FD/CWD は元の親チェーンを保持し、名前再利用で別のオブジェクトへ接続しません。点の open は独立カーソル、dup は共有カーソル、二点は元の親を使います。削除済みの通常子名は ENOENT。LOOKUP は保持された削除済み親をたどれますが、作成・削除・改名検索は ENOENT。改名の末尾点/二点はその要素より先に EINVAL、以前の祖先エラーが優先します。F_GETPATH は最後のパスを保持し、完全な stat/列挙は不明です。新規ディレクトリのパス+NUL と一項分は FD/CWD/旧子参照がなくなるまで課金し、close/dup2/CWD変更/変更受付が到達不能チェーンを回収します。初期入力とファイルリースは別です。独自 `deleted-directories` はネイティブ macOS と5構成で保持中削除、親と名前の再利用、検索意図、CWDだけの保持を比較します。

### ディレクトリ寿命の検証、2026-10-06

Release Darwin1,051登録、631成功、未提供420スキップ、失敗なし；必須 ARM64 HVF105件実行。対象98成功・12スキップ、初期直接64/64（新規14件と保持中削除更新）。C/CLI/report183/183、Darwin133件；Python5構成18.691秒、ネイティブ25/25、スクリプト66/66。追加ネイティブ改名検査で末尾点の順序を修正し、初期ソース/結果/スナップショットを保持。主エージェントが証拠を照合し、最終独立レビューは利用不可でした。件数重複、期限不変、完全CIと実機iOSは別、Intel HVF Actions停止継続。

`build-hvf-arm64/directory-lifetime-validation-summary.json`, `directory-lifetime-darwin-final-evidence/`, `directory-lifetime-focused-final.xml`, `directory-lifetime-public.xml`, `directory-lifetime-native/`, `directory-lifetime-before-rename-fix/`.

## 明示的に許可した初期ディレクトリの削除

ディレクトリ項目の厳密な真偽値 `"removable": true`（C++ は `DarwinFileOptions::RemovableDirectories`）は、名前空間上の識別対象が一つで、マウントではない通常のディレクトリを宣言します。ルート以外の明示的な初期 `directories` 項目と、変更を明示許可した直接の親が必要です。既知の特殊モード/フラグ、スナップショットを含む inode 別名、親子の既知デバイス番号の矛盾は拒否します。番号の一致だけではマウント不在の証明になりません。省略/false は未対応のまま、他の JSON 型は無効です。一般的な権限やマウントのモデルではありません。

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/empty","removable":true}],"working_directory":"/empty"}}
```

削除には現在の名前空間が空であることが必要です。初期の暗黙的な子ディレクトリは、最後の元ファイルを unlink しても残ります。成功すると対象と直接の親の完全な stat/列挙は不明になりますが、旧 FD/dup/CWD は元のオブジェクトと親チェーンを保持します。不変入力から削除名を復活させません。同名の新しいファイル/ディレクトリは別の識別対象で、旧メタデータやスナップショットを継承せず、呼出元の入力も変更しません。

各 removable 参照のパスと NUL は初期 16 MiB 費用に含まれます。初期項目、パス、参照、スナップショットの費用と256項目上限の初期枠は、削除や最後の close でも返却しません。新しいオブジェクトには独自の動的費用を適用します。独自の `initial-directory-removal` はネイティブ試験の既存空ディレクトリを保持中に削除し、同名ファイル、次いでディレクトリを作成して CWD 単独保持を確認し、空ディレクトリを復元します。同じプログラムを C++/C/CLI/Python の5構成で実行します。

### 初期ディレクトリ削除の検証、2026-10-06

最終 Release ソースで Darwin 登録 1,089 件を照合し、657 件成功、バックエンド利用不可によるスキップ 432 件、失敗 0 件となった。必須 ARM64 HVF 108 件はすべて実行した。重点検証は 27/39 件成功、12 件利用不可スキップで、追加のスナップショットのみの別名検証も成功した。公開 C/CLI/レポートは 191/191、Python は 76.276 秒で 5 組合せ、独自ネイティブ負荷は 26/26、証拠ランナーは 66/66 成功。初回の inode/スナップショットが矛盾したテスト入力は修正し、失敗記録を保存した。

先行する完全検証 2 回では既存のファイル/rename ケースに 1 件と 3 件のタイムアウトがあり、診断付き実行でも実時間 5.008 秒、プロセス CPU 時間 0.171 秒のファイルタイムアウトを再現した。同一メソッドと旧プログラムの比較は成功したが、遅延原因は未解明であり、最終成功はタイムアウトの安定性を証明しない。一時診断を除去し、フィクスチャのハッシュを復元し、元のゲスト 5 秒制限を維持した。主担当によるソース/証拠監査を完了し、独立レビューは利用不可だった。件数は重複する。完全 GitHub CI、実機 iOS、停止中の Intel HVF Actions はこのローカル受入範囲外である。

`build-hvf-arm64/initial-directory-validation-summary.json`, `initial-directory-darwin-restored-evidence/`, `initial-directory-public.xml`, `initial-directory-native-evidence/`, `initial-directory-timeout-probe/`.

## 通常ファイルの排他的な改名

ソースとターゲットの検索後、RENAME_EXCL は別の既存ファイルまたはディレクトリに EEXIST を返し、マウントや名前空間変更の検査に先行します。末尾ドット/二重ドットの EINVAL など先のパスエラーは優先されます。ターゲットがなければ既存の有界改名トランザクションを使用し、保持された記述、カーソル、フラグ、マッピングリースと設定済みメタデータ遷移を保ちます。同一オブジェクトへの排他的改名は、ファイルシステムの大文字小文字区別に依存するため明示的に未対応です。正確なカタログキーからこの性質を推定しません。大小文字の同一視、ディレクトリソース、SECLUDE と SWAP は範囲外です。既存の独自 `renamed-file` は拒否時のメタデータ保持と EXCL|NOFOLLOW_ANY の成功をネイティブ macOS と C++/C/CLI/Python で比較します。

検証、2026-10-06（Release）：Darwin 登録 1,097 件、成功 665 件、利用不可スキップ 432 件、失敗なし。必須 ARM64 HVF 108 件すべて実行。重点検証は成功 44 件、利用不可 12 件で、新規直接検証 8 件を含む。公開 C/CLI/レポート 191/191、Python 5 組合せ 19.241 秒、独立した生呼出しプローブ 26 件成功。最初のネイティブ全体実行は既存 return がタイムアウトし、renamed-file を含む他の 25 件は成功。同じ未変更バイナリの return 再確認 3 回は 0.014–0.034 秒、その後全 26 件が元の 5 秒制限で成功した。初回失敗は保存し原因は未解明で、以前の最終 HVF 成功を含め遅延安定性の証明ではない。主担当監査済み、独立レビュー利用不可。件数は重複する。実機 iOS、完全 GitHub CI、停止中の Intel HVF Actions はローカル受入範囲外。

`build-hvf-arm64/exclusive-rename-validation-summary.json`, `exclusive-rename-darwin-evidence/`, `exclusive-rename-public.xml`, `exclusive-rename-native-evidence/`, `exclusive-rename-native-rechecked-summary.json`, `exclusive-rename-probe/`.
