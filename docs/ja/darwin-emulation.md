**言語**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 21781b6920e7f63dec6efbcc4bc83f255533a7ad1069780a0bc03dc62951cb3f -->

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

`darwin_files` は3つのプロファイルに閉じた読み取り専用ファイル一覧を提供します。必須の `files` は正規の絶対ゲスト `path` と16進数 `bytes_hex` を持ち、任意の `stdin_hex` は有限入力です。入力省略は未知で非ゼロ読み取りを停止し、空文字列は EOF です。一覧未指定の open は停止し、明示的な空一覧の未存在絶対パスは ENOENT を返します。ホストのファイルや入力は参照しません。

追加サービスは `open`、`read`、`pread`、`lseek`、`close`、`dup`、`dup2`、`fcntl` です。read/write/open/close/fcntl/pread の nocancel 入口も同じ実装を使います。O_RDONLY/O_CLOEXEC と F_DUPFD、F_DUPFD_CLOEXEC、F_GETFD、F_SETFD、F_GETFL を扱います。独立 open は別の位置、dup は共有位置と個別の close-on-exec フラグを持ち、pread は位置を変えません。0/1/2 の close・置換も後続 I/O に反映し、出力の複製は元の捕捉先と予算を保持します。

上限は256ファイル、パス/NUL/内容/入力の合計16 MiB、1024バイト未満のパス、255バイト以下の成分です。排他的上限 `descriptor_limit` は3–4096、既定256、JSON は64 KiBです。不正設定はロード前に拒否します。INT_MAX を超える read は FD 検査前に EINVAL、EOF は宛先に触れず、不正宛先は EFAULT です。部分的に書き込み可能な範囲はコピーや位置変更の前に停止します。SET/CUR/END の失敗は位置を保持します。書き込み、旧 stat、疎ファイル seek、その他 fcntl は未対応です。ファイルを祖先にすると ENOTDIR です。同じオブジェクトでネイティブ macOS と比較し、C/CLI/Python は5つのゲスト組合せを検証します。iOS 実機の証拠ではありません。

2026-10-05 の Release 検証は381項目中177成功、204スキップ、失敗なしで、ARM64 HVF 必須51/51を実行しました。ネイティブ macOS 7プログラム、公開 C/CLI・レポート35項目、Python の5ゲスト組合せ、検証スクリプト66項目も成功しました。件数は重複します。新しいファイルサービスの Intel HVF/KVM/WHP ネイティブ証拠はありません。Intel HVF は未検証で Actions を停止中です。iOS SDK と実機比較はありません。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## ディレクトリと相対パス

任意の `directories` は正規の絶対 `path` と任意の完全な `metadata` を持ち、空ディレクトリを指定できます。ルートと祖先は暗黙に存在し、メタデータで未存在パスは作れません。mode は `0x4000` と権限、size は [0, INT64_MAX] の明示的観測です。`working_directory` は既存の正規ディレクトリを指定し、省略時の CWD は未知です。ホストから継承しません。指定パスは最大256（祖先メタデータを含む）、パス/NUL/内容/入力/CWD は計16 MiBです。

`openat` (463)、`openat_nocancel` (464)、`chdir` (12)、`fchdir` (13)、`fstatat64` (470) は同じ解決器を使います。相対パスはディレクトリ FD または `AT_FDCWD=-2`、絶対パスは FD を無視します。重複スラッシュ、`.`、`..`、末尾スラッシュも祖先を検査し、`/file/..` は ENOTDIR、`/missing/..` は ENOENT です。失敗や元の FD の close・再利用・置換は CWD を変えません。`F_GETPATH=50` は複製 FD にも正規パスと NUL を返し、その後を書き換えません。

ディレクトリ read/pread はゼロ長でも EISDIR、負の pread オフセットは先に EINVAL です。SET/CUR は位置を共有し、END には明示 size が必要です。mmap は EINVAL。fstatat64 は 0、`AT_SYMLINK_NOFOLLOW=0x20`、`AT_SYMLINK_NOFOLLOW_ANY=0x800`、`AT_FDONLY=0x400`（パスを無視）に対応。不正ビットは EINVAL、`AT_REALDEV=0x200` は未対応です。ストリームの識別は未知、権限はアクセス制御モデルではなく、書き込みは残作業です。同じ `directories` をネイティブと5ゲストで比較し、stat は実ファイルとディレクトリを照合します。Intel HVF Actions は停止中です。

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

同じ `directory-entries` がネイティブ macOS と記録・dup/巻戻し・小分け読出し・EOF・コピー順序を比較します。別テストで SDK 配置と実記録の全バイト、長い名前を比較します。固定スナップショット値は巻戻しでも変わらず、APFS の動的世代は再現しません。旧 `getdirentries` (196)、書込み、他のネイティブバックエンド、iOS実機は未検証です。

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

`stat64` (338)、`fstat64` (339)、`lstat64` (340) は ARM64/x64 で同じ144バイト LP64 レコードを返します。open とパス解決を共有し、FD の複製と close を反映します。FD やカーソルは変更せず、rdev、パディング、予約領域はゼロです。メタデータは呼び出し側の固定観測値で、read は時刻を更新せず、mode はアクセス許可を変えません。未指定メタデータ、ストリーム、シンボリックリンク、旧 stat、拡張セキュリティは未対応です。パス/FD エラーを出力ポインターより先に処理し、部分的に書ける出力は変更前に停止します。ネイティブ試験は実ファイルの全バイトと SDK 配置を比較し、同じ独自プログラムで3呼び出しを検証します。

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

独立したワークロード検証は ARM64 69 件または x64 46 件をすべて要求し、各プラットフォームの `LC_MAIN` と `LC_UNIXTHREAD` を含みます。必須項目の欠落、スキップ、`ld64.lld` の不足はいずれも失敗です。

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
