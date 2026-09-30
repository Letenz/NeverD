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

`driver-strict` は x64、`software-cpu-v1` は x64 と ARM64 を受け付けます。`checked-x64-v1` と `checked-aarch64-v1` は指定 ISA と supervisor 権限を要求します。`checked-user-x64-v1` と `checked-user-aarch64-v1` は契約に対応する限定命令群をそれぞれ CPL3/EL0 で実行し、MMU 分離と明示的なサービス要求 exit を提供します。Unicorn とホストに一致する KVM/WHP をサポートし、`auto` は既存のホスト選択に従います。flat プロファイルにユーザー／supervisor の MMU 分離保証はありません。checked ARM64 は FP/SIMD を拒否しますが、x64 は以下の限定的な命令群を許可します。supervisor x64 は限定 MMIO と prepared-read の文字列転送を追加し、user profile は device mapping を拒否します。checked 全体で port I/O と並列 CPU 要件は引き続き拒否されます。`service_traps` を通知するのは user プロファイルだけです。

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

## x64 拡張と native CPU state

checked x64 は限定された legacy SSE/SSE2 move/logical、`MOVLHPS`/`MOVHLPS`、mask 付き scalar `CVTTSS2SI`/`CVTTSD2SI`/`SUBSS`/`SUBSD` を許可します。MXCSR は sticky status、rounding、FTZ を保持し、DAZ と unmasked exception は拒否します。checked ARM64 は引き続き FP/SIMD を拒否します。KVM/WHP は16個すべての XMM register と MXCSR を同期します。列挙されていない encoding/operand は許可されません。

checked x64 は mask 付き legacy `ADD`、`SUB`、`MUL`、`DIV`、`SQRT`、`MIN`、`MAX` の `SS`、`SD`、`PS`、`PD` 形式も許可します。`X64SSEInstructions.def` が operand 幅、alignment、admission を一元管理します。`MaskedSSEArithmeticMatchesIndependentHostExecution` は独立した host CPU oracle で register/RAM 形式、4 種の rounding、FTZ、signed zero、subnormal、NaN を検証し、`SSEMemoryObserverStopsBeforeResultAndStatusChanges` は効果反映前の停止を検証します。DAZ、unmasked exception、x87、AVX は許可しません。

thread pointer は x64 FS/GS base と ARM64 `TPIDR_EL0` を正確な `MRS`/`MSR` encoding で扱います。native transport と CPU snapshot は memory とは独立してこの状態を保持しますが、OS thread や TLS block を作るものではありません。supervisor x64 は1/2/4 byte の aligned scalar MMIO と restart boundary ごとに1要素の MOVS を許可します。device read には effect のない prepared preview と最大一度の commit が必要です。user profile は device mapping を拒否し、RMW、wide MMIO、port I/O も未対応です。

KVM/WHP は active native entry を cancel し、実行資源を解放する前に acknowledgement を待ちます。KVM は専用 execution thread と一時的に unblock する realtime signal を使います。entry 中、選択した signal は ignored であってはなりません。caller の signal mask/handler は変更しません。guest の進行が不確かな中断は terminal failure であり、厳密な wall-clock deadline は保証しません。

## x64 のネイティブ同期例外

checked x64 の `DIV`/`IDIV` は実際のプロセッサ結果と `#DE` を使用します。KVM は非公開の supervisor IDT/IST、WHP は明示的な例外ビットマップを使用し、元のコンテキストと利用可能なエラーコードを転送エラーと区別します。OS は回復可能なイベントを消費してから継続コンテキストを設定します。Windows ドライバはゼロ除算と商のオーバーフローを `STATUS_INTEGER_DIVIDE_BY_ZERO` に変換し、実際の SEH filter、`__finally`、再試行を実行します。`NeverDX64ExceptionTests` は Unicorn 無効でも構築でき、`DriverWDMCPUException` は元の WDK 用例を検証します。利用できない WHP/ARM64 ホストは明示的にスキップします。

## 段階的な RAM 効果

`RAMTransaction` は物理実行リースの下で、命令が宣言した書き込み範囲の物理的な和集合だけを保持します。結果観測器を呼ぶ前に元の RAM を復元し、取消し、転送エラー、観測器の例外では部分的な RAM やレジスタを公開しません。CPU 例外では RAM を戻した後もアーキテクチャの例外状態を保持します。ARM64 の単一・ペアストアも同じ管理層を使います。x64 は 8/16/32/64 ビットの `XCHG`、`XADD`、`CMPXCHG` を実行し、LOCK または暗黙のロックを持つ形式には自然整列を要求します。`NeverDRAMTransactionTests` はホスト CPU との結果比較、復元、エイリアス、権限を検証し、利用できないプラットフォームを明示的にスキップします。デバイスと並列 SMP は対象外で、CPU スナップショットは確定済み RAM を戻しません。

## 完全な x87 状態

`NeverDEmulationArch` は ISA、ページテーブル、FP 状態の配置を所有し、ネイティブと Unicorn の転送が共有します。x64 コンテキストは x87 制御、状態、TOP、物理タグ、オペコード、命令／データポインター、8 個の 80 ビットレジスターを保持します。`FP0`–`FP7` は `RegisterValue` を使い、スカラーアクセスによる切り捨ては拒否します。`FPTag` は物理レジスターの非空ビットマップです。`NeverDX64FPTests` は全 TOP、正確な演算のホスト FXSAVE/FXRSTOR 比較と復元を検証します。checked x87 命令や全丸め意味論の証明を追加するものではなく、利用できないネイティブホストは明示的にスキップします。

`driver-strict` は一致する Linux x64 host の KVM と Windows x64 host の WHP を許可します。`auto` は対応する native transport を選び、cross-ISA は Unicorn を選びます。明示的な Unicorn と従来の V1 API は portable software profile を保持します。native 実行は entry 前に canonical address と instruction effect を検証し、hardware 不可用時は fallback なしで失敗します。未対応 instruction/OS behavior は明示的な error です。native ARM64/WHP の実機証拠は未取得で、任意 driver や Android/Darwin の互換性を保証しません。

選択したバックエンドの機能は `executionCapabilities(Contract, ISA, Backend)` で照会します。`NativeLegacyX64` はネイティブ x64 ドライバー実行を表し、`NeverDNativeDriverTests` は既存のドライバー群を検証します。このテストは Unicorn を無効にしたビルドでも実行できます。
