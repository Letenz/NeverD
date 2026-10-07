# 公開済み Android GKI カーネルの契約

NeverD は他の Linux 系統に先立ち、公開済み Android GKI 5.10–6.18 の各ブランチを優先します。プロセス要求で明示的に選択します。

```json
{"linux_kernel":{"gki":"android17-6.18"},"linux_files":{"files":[],"descriptor_limit":16}}
```

Android ネイティブプロファイルの API 28 契約は Bionic のインポートを表し、カーネル版を選びません。GKI 選択は実装済みの `pidfd_open`、ベクトル出力、符号化されたプロセス CPU クロックだけを制御します。完全なカーネルの認証や起動、デバイス・名前空間・資格情報・プロセス一覧の推測は行いません。未対応サービスは明示的に停止します。[公式 GKI リリース方針](https://source.android.com/docs/core/architecture/kernel/gki-releases)を参照してください。

## 固定したソース版

`LinuxGKIKernels.def` は次の正式な `r1` タグを採用し、2026-10-07 に確認しました。固定コミットの対象は `kernel/pid.c`、`include/uapi/linux/pidfd.h`、`arch/arm64/configs/gki_defconfig`、`kernel/fork.c`、`lib/iov_iter.c`、`fs/read_write.c` です。フラグ値は UAPI と呼び出し検証に基づき、Android API レベルやホストカーネルから推測しません。 CPU クロックの固定ソースは後掲の表を参照してください。

| 要求ブランチ | 正式公開タグ | 固定ソースコミット | 許可フラグ | Iovec インポート |
| --- | --- | --- | --- | --- |
| `android12-5.10` | `android12-5.10-2026-07_r1` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/pid.c) | `PIDFD_NONBLOCK` (`0x800`) | [全メタデータを先にコピー](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/lib/iov_iter.c) |
| `android13-5.10` | `android13-5.10-2026-07_r1` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/pid.c) | `PIDFD_NONBLOCK` | [全メタデータを先にコピー](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/lib/iov_iter.c) |
| `android13-5.15` | `android13-5.15-2026-09_r1` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/pid.c) | `PIDFD_NONBLOCK` | [全メタデータを先にコピー](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/lib/iov_iter.c) |
| `android14-5.15` | `android14-5.15-2026-07_r1` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/pid.c) | `PIDFD_NONBLOCK` | [全メタデータを先にコピー](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/lib/iov_iter.c) |
| `android14-6.1` | `android14-6.1-2026-09_r1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/pid.c) | `PIDFD_NONBLOCK` | [全メタデータを先にコピー](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/lib/iov_iter.c) |
| `android15-6.6` | `android15-6.6-2026-07_r1` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/pid.c) | `PIDFD_NONBLOCK` | [単一バッファ経路](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/lib/iov_iter.c) |
| `android16-6.12` | `android16-6.12-2026-09_r1` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/pid.c) | `PIDFD_NONBLOCK` と `PIDFD_THREAD` (`0x80`) | [単一バッファ経路](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/lib/iov_iter.c) |
| `android17-6.18` | `android17-6.18-2026-09_r1` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/pid.c) | `PIDFD_NONBLOCK` と `PIDFD_THREAD` | [単一バッファ経路](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/lib/iov_iter.c) |

これらの固定版が契約を定義します。新しい公開版やバックポートにはソース確認と回帰検証が必要です。GKI 選択と `pidfd_open` 不在の明示的観測は矛盾するため、ロード前に拒否します。

## 実装済みプロセス記述子の範囲

x64/AArch64 の生トラップと Bionic `syscall` は `LinuxServices` とワークロード所有の記述子表を共有します。PID とフラグは下位 32 ビットを使用し、不明なフラグや符号付きで非正の PID は割り当て前に `EINVAL` となります。任意の `tasks` 配列は、ほかの生存ゲストタスクを示す固定の閉じた一覧です。

```json
{"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true},{"id":3000,"group_leader":false}]},"linux_files":{"files":[],"descriptor_limit":16}}
```

実行中のグループリーダー PID 1000 は空配列でも暗黙に含まれます。一覧省略時は外部対象の検索を未対応のまま保持し、明示一覧にない有効な正 PID は割り当て前に `ESRCH` を返します。`PIDFD_THREAD` のない生存非リーダーは固定 5.10–6.12 で `EINVAL`、[6.18 の `pidfd_prepare`](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/fork.c) で `ENOENT` です。許可されたフラグがあれば 6.12/6.18 は宣言済み非リーダーを開けます。フラグ検証は対象検索より先です。

各項目には 1..2147483647 の整数 `id` と真偽値 `group_leader` が必要で、上限は 4096 項目です。重複、追加フィールド、PID 1000 の非リーダー宣言、GKI 未選択の一覧は拒否します。優先度の観測対象は一覧内タスクまたは現在のプロセスに限定します。Android 協調スレッド（`thread_limit > 1`）との併用は不可です。生成、回収、資格情報、名前空間変換などには独自の寿命管理が必要です。

`linux_files` の宣言が必須で、通常ファイルと pidfd は所有権・上限を共有します。最小の空き番号を割り当て、枯渇は `EMFILE`、`close` は解放、二重クローズは `EBADF` です。閉じた標準ストリームの番号は再利用できます。ホストの pidfd、ファイルシステム、プロセス検索は利用しません。

有効な pidfd の `read`/`write` はデータアクセス前に `EINVAL`、`lseek` は基準位置を検証後に `ESPIPE` を返します。`writev` はメタデータとユーザー範囲を先に検証するため、未実装書き込みの `EINVAL` より先に `EFAULT` が返り得ます。データの読み出し・出力取得はしません。stdout/stderr も同じ版別インポーターを使います。[VFS の順序](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/fs/read_write.c)を参照してください。

5.10/5.15/6.1 は iovec 全体をコピーしてから長さを検証するため、先の負の長さと後続のアクセス不能メタデータは `EFAULT` です。単一ベクトルも上限適用前に元の範囲を検証します。6.6/6.12/6.18 は逐次検証し、同じ場合は `EINVAL` です。単一バッファは先に制限し、複数ではすべての元の範囲を検証します。根拠は `copy_iovec_from_user`、`__import_iovec`、`import_ubuf` です。GKI 未指定時は既存の単一バッファ方針を保持し、版を推測しません。

Bionic は生の負エラーを `-1` とスレッド局所 `errno` に変換し、成功時は `errno` を維持します。`fstat` 用メタデータはありません。ポーリング、終了通知、pidfd 経由のシグナル、`pidfd_getfd`、`fcntl`、pidfs ioctl、未観測タスク検索は未対応です。pidfd からスケジューリングや寿命は推測しません。

## 実装済みプロセス CPU クロックの範囲

GKI を明示的に選ぶと、`clock_gettime` は PROF、VIRT、SCHED の負の符号化プロセス CPU クロック ID を受け付け、引数の下位 32 ビットを符号付きで解釈します。PID と種類が `linux_time` の明示的な標本を特定します。現在のプロセスは暗黙に存在し、他のプロセスは標本を与える前に閉じたタスク一覧で存続中のグループリーダーとして宣言する必要があります。

```json
{"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true}]},"linux_time":{"advance_on_idle":true,"clocks":[{"id":1,"seconds":10,"nanoseconds":0},{"id":2,"seconds":3,"nanoseconds":4},{"id":-16006,"seconds":7,"nanoseconds":9}]}}
```

`-16006` は PID 2000 の SCHED です。PROF と VIRT は独立した観測です。現在の SCHED ID 2、-6（PID ゼロ）、-8006（PID 1000）は同じ標本を共有し、PROF の別名は -8／-8008、VIRT は -7／-8007 です。値が同じでも重複する別名は拒否します。CPU 秒は非負、ナノ秒は正規化が必要です。アイドル進行は壁時計 ID 0、1、7 だけを進め、プロセス CPU 標本は固定します。命令実行から CPU 使用量は推定しません。

現在のタスク自身の TID はそのプロセスグループも指定します。外部一覧を持たない Android 協調スレッドでも同じです。閉じた一覧にない外部 PID と存続中の非リーダーは、出力先に触れる前に `EINVAL` になります。一覧省略時の外部検索と、既知グループの標本欠落は出力前に未対応として停止します。無効な種類は `EINVAL`、有効な標本のユーザーコピーは `EFAULT` を返す場合があります。raw トラップは負のエラーを保持し、Bionic だけが errno を更新して -1 を返します。

対象と種類の規則は、各固定リリースの `pid_for_clock`、`posix_cpu_clock_get`、クロック分派と ID 定義に基づきます。

| 要求ブランチ | プロセス CPU クロックのソース |
| --- | --- |
| `android12-5.10` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/time/posix-cpu-timers.c) |
| `android13-5.10` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/time/posix-cpu-timers.c) |
| `android13-5.15` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/time/posix-cpu-timers.c) |
| `android14-5.15` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/time/posix-cpu-timers.c) |
| `android14-6.1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/time/posix-cpu-timers.c) |
| `android15-6.6` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/time/posix-cpu-timers.c) |
| `android16-6.12` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/time/posix-cpu-timers.c) |
| `android17-6.18` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/time/posix-cpu-timers.c) |

FD クロックの識別と CPU 分派は固定の [6.18 分派器](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/time/posix-timers.c)と[クロック ID 定義](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/include/linux/posix-timers_types.h)にも基づきます。FD クロックと符号化されたスレッド別 CPU クロックは未対応です。一覧は固定のゲスト観測であり、権限、名前空間、プロセス寿命、CPU 計量の拡張には個別の契約が必要です。

## 検証と今後の対象

`LinuxPIDFDTests.cpp` は独立した x64/AArch64 O0/O2 ELF 呼び出し側を全八ブランチと利用可能なバックエンドで実行し、フラグ、表の共有、上限・再利用、エラー順序、メタデータ障害、長さ制限と元範囲、一覧の省略・閉包、非リーダー、FD 枯渇前の対象検索を検証します。`AndroidSyscallTests.cpp` は通常・Android packed・RELR の六種の O0/O2 構成で raw/Bionic の所有権・検索・errno を再検証します。ソースとモデル実行はこの部分契約の証拠であり、すべての固定 GKI イメージを起動するネイティブ検証は未実施です。ほかの Linux への拡張もサービスごとに版・構成・観測証拠を保持します。

CPU クロックのテストは識別、出力順序、独立した種類、明示的標本の検証、壁時計のアイドル進行との分離を確認します。`AndroidTimeTests.cpp` は名前付き／raw 出力とカナリアを、協調 syscall は現在の非リーダー TID の別名を確認します。
