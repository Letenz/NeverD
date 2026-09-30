**言語**: [English](../cpu-execution.md) | [简体中文](../zh-CN/cpu-execution.md) | [繁體中文](../zh-TW/cpu-execution.md) | [日本語](cpu-execution.md) | [한국어](../ko/cpu-execution.md) | [Français](../fr/cpu-execution.md) | [Deutsch](../de/cpu-execution.md) | [Español](../es/cpu-execution.md) | [Italiano](../it/cpu-execution.md) | [Русский](../ru/cpu-execution.md) | [العربية](../ar/cpu-execution.md)

[← ドキュメント索引](README.md)

# CPU 実行と機能照会

CPU 実行はゲスト OS、イメージローダー、呼び出し規約から独立しています。`NEVERD_ENABLE_CPU_EMULATION` で単独ビルドでき、`NEVERD_ENABLE_DRIVER_EMULATION` は Windows ドライバーモデルも含めます。[アーキテクチャガイド](architecture.md)では所有境界、バックエンド選択、プラットフォーム上の制限を説明します。

## 構成

公開 [`ExecutionConfiguration`](../../include/neverd/emulation/ExecutionConfiguration.h) は CPU ファクトリーと機能レポートで共有されます。CPU の割り当てやアドレス空間への接続より前に要件を検証します。省略値は契約固有の固定プロファイルを使い、明示された未対応値は失敗します。

| JSON フィールド | 既定値 | 意味 |
|---|---|---|
| `backend` | `auto` | `auto`、`unicorn`、`kvm`、`whp` |
| `contract` | `software-cpu-v1` | バージョン付き実行セマンティクス |
| `architecture` | `x86_64` | `x86_64` または `aarch64` |
| `privilege` | 契約プロファイル | `flat`、`supervisor`、`user`。契約と一致する必要があります |
| `virtual_address_bits` | 契約プロファイル | checked は48ビット、flat は64ビット直接マッピング空間 |
| `page_size` | 4096 | ゲストマッピング粒度。ほかの値は拒否 |
| `required_features` | `[]` | [`ExecutionConfiguration.def`](../../include/neverd/emulation/ExecutionConfiguration.def) の必須機能名 |

`driver-strict` は x64、`software-cpu-v1` は x64 と ARM64 を受け付けます。`checked-x64-v1` と `checked-aarch64-v1` は指定 ISA と supervisor 権限を要求します。`checked-user-x64-v1` と `checked-user-aarch64-v1` は同じ限定されたスカラー命令群をそれぞれ CPL3/EL0 で実行し、MMU 分離と明示的なサービス要求 exit を提供します。Unicorn とホストに一致する KVM/WHP をサポートし、`auto` は既存のホスト選択に従います。flat プロファイルにユーザー／supervisor の MMU 分離保証はありません。すべての checked プロファイルは FP/SIMD、MMIO、ポート I/O、並列 CPU 要件を拒否します。`service_traps` を通知するのは user プロファイルだけです。

user 実行には、マップされた**各ページ**で `UserAccessible` と適切な `Read`、`Write` または `Execute` 権限が必要です。既存マッピングは既定で supervisor 用です。同じ物理バイトを共有する alias でも権限は独立し、`UserAccessible` だけではアクセスを許可しません。信頼されたホスト操作と supervisor CPU は RWX を使います。例:

```cpp
Configuration.Contract = ExecutionContract::CheckedUserX64;
Configuration.Privilege = ExecutionPrivilege::User;
auto CPU = llvm::cantFail(createExecutionBackend(Configuration, Space)).CPU;
llvm::cantFail(CPU->map(Code, 4096, Read | Write | Execute | UserAccessible));
```

権限は契約で固定されます。コンテキスト復元やアドレス空間の切替では変化せず、x64 セグメントセレクターでも昇格できません。回復可能なデータアクセス fault は、所有者が処理するまで元の命令とレジスターを保持します。`canAccess` は要求された権限だけを正確に検査し、user 可視性の照会には `UserAccessible` を含めます。ページテーブルは CPU 内部の投影であり、変更可能なゲストページテーブルや権限切替 API は公開しません。ARM64 では user ページは EL1 でも実行不可です。

未知/null フィールド、無効な名前や数値幅、必須機能の重複、未対応の組み合わせは失敗します。入力上限は64 KiBです。従来の CPU factory とドライバー C オプションは互換性を維持します。

## ワークロードを実行せずに照会

```bash
neverd cpu-capabilities
neverd cpu-capabilities \
  --configuration='{"contract":"checked-aarch64-v1","architecture":"aarch64"}' \
  --probe-host
```

スキーマは version 1 です。レポートは、既定値補完前の `requested_configuration`、正規化済み `configuration`、静的意味論の `capabilities`、adapter build/ABI の `build`、および `--probe-host` 指定時のみ返る `host`（未指定なら null）を分離します。host probe は専用 RAM 上で一時 CPU を初期化するだけで、ワークロード適合性や native ARM64 実行を証明しません。利用可能性は変化し、未対応 backend への暗黙 fallback はありません。CLI は backend 不在でも有効なレポートなら 0、無効な設定・照会なら 1 を返します。

## SDK と C++ の境界

[`neverd_cpu_capabilities_json`](../../include/neverd/sdk/NeverDCAPICPU.h) は既存 session、任意の設定 JSON、0/1 の `ProbeHost` を受け取ります。ロード済み image は不要です。結果は `neverd_free_string` で解放し、NULL の場合は `neverd_last_error` を確認します。CPU 無効ビルドも同じ関数を公開し、無効状態を明示します。Python plugin は `session.cpu_capabilities(...)` を使います。C++ では `executionCapabilities`、`resolveExecutionConfiguration`、`queryExecutionBackendBuild`、`probeExecutionBackend` を個別に利用でき、`createExecutionBackend` は既存空間に CPU を接続するか専用 RAM と既定空間を作ります。

## CPU の結果と予算

`CPU.runUntilExit(PC, TimeoutMicroseconds)` は型付き [`ExecutionExit`](../../include/neverd/emulation/ExecutionExit.h) を返します。実行開始前の設定エラーは `llvm::Error`、開始後は stop/deadline/service request/回復可能 fault/guest fault・trap/未対応操作/device・backend failure/説明できない engine stop を明示します。CPU/device/backend fault は同時 stop/deadline より優先され、独立した事実と詳細は保持されます。timeout は正で、duration と絶対 deadline の両方に表現可能でなければなりません。無効なら CPU 状態を変更する前に失敗します。各呼出しには有限予算が必要で、0 は無制限でも有効な即時 timeout でもありません。制御は協調的で、厳密な実時間期限ではありません。戻り値は回復可能 fault を消費しません。OS owner が取得し、再開前に検証済み例外遷移を設定します。既存の `run`、`fault`、`timedOut` は維持されますが、`run` だけを override する外部 CPU 実装は新しい型付き境界を拒否します。

## サービス要求

checked user x64 は正確な prefix なしの `SYSCALL` encoding のみを intercept し、checked user ARM64 は `SVC #imm16` を intercept します。`SYSENTER`、`INT`、`HVC`、`BRK` などは未対応です。命令 observer を先に実行し、stop/fault がなければ backend に入る前、命令実行前に `ExecutionExitKind::ServiceRequest` を返します。結果には種類、元の `PC`、順次 `NextPC`、SVC immediate が含まれます。レジスター、flags、stack、権限は変わりません。x64 の RCX/R11 clobber も、ARM64 の例外 vector への移行も発生していません。SVC immediate は汎用 service 番号ではありません。

request は pending のまま実行、CPU 変更、空間 binding、context capture/restore をブロックします。停止中に OS owner が `takeServiceRequest()` で一度だけ消費します。owner が OS ABI を解釈し、service を処理して、結果レジスターと次の PC/例外遷移を明示的に選択します。未対応 service はその owner で失敗し、元の PC を再実行すれば新しい request が発生します。暗黙の NOP や成功結果はありません。service event は同時 stop/deadline より優先されますが、guest/backend failure はさらに優先です。CPU stop、software HLT、deadline、trap は workload 成功を意味しません。[Linux process profile](process-emulation.md) は独立した OS model を使い、Windows/Android/Darwin の動作を証明しません。Windows と ARM64 の native 実行は実機での検証が必要です。
