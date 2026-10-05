**言語**: [English](../cpu-execution.md) | [简体中文](../zh-CN/cpu-execution.md) | [繁體中文](../zh-TW/cpu-execution.md) | [日本語](cpu-execution.md) | [한국어](../ko/cpu-execution.md) | [Français](../fr/cpu-execution.md) | [Deutsch](../de/cpu-execution.md) | [Español](../es/cpu-execution.md) | [Italiano](../it/cpu-execution.md) | [Русский](../ru/cpu-execution.md) | [العربية](../ar/cpu-execution.md)

[← ドキュメント索引](README.md)

# CPU 実行と機能照会

CPU 実行はゲスト OS、イメージローダー、呼び出し規約から独立しています。`NEVERD_ENABLE_CPU_EMULATION` で単独ビルドでき、`NEVERD_ENABLE_DRIVER_EMULATION` は Windows ドライバーモデルも含めます。[アーキテクチャガイド](architecture.md)では所有境界、バックエンド選択、プラットフォーム上の制限を説明します。

`NEVERD_ENABLE_SEMANTIC_TESTS` の既定値は `ON` で、`unittests/semantic` のテスト群と集約実行ターゲットを制御します。Unicorn を使わずにネイティブ CPU テストを構築するには、`BUILD_TESTING=ON` を維持し、`NEVERD_ENABLE_SEMANTIC_TESTS=OFF` と `NEVERD_EMULATION_BACKEND_UNICORN=OFF` を指定します。適切な SDK ヘッダーを備えた Windows ARM64/MSVC を含め、KVM/WHP のネイティブテストは引き続き構築できます。Windows ARM64 で Unicorn を有効にする場合は ARM64 LLVM-MinGW ツールチェーンが必要です。このビルド分離は ARM64 の実機実行を検証するものではありません。

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

`driver-strict` は x64、`software-cpu-v1` は x64 と ARM64 を受け付けます。`checked-x64-v1` と `checked-aarch64-v1` は指定 ISA と supervisor 権限を要求します。`checked-user-x64-v1` と `checked-user-aarch64-v1` は契約に対応する限定命令群をそれぞれ CPL3/EL0 で実行し、MMU 分離と明示的なサービス要求 exit を提供します。Unicorn とホストに一致する KVM/WHP をサポートし、`auto` は既存のホスト選択に従います。flat プロファイルにユーザー／supervisor の MMU 分離保証はありません。checked ARM64 と x64 は、以下に示す限定的な FP/SIMD 命令群を許可します。supervisor x64 は限定 MMIO と prepared-read の文字列転送を追加し、user profile は device mapping を拒否します。checked 全体で port I/O と並列 CPU 要件は引き続き拒否されます。`service_traps` を通知するのは user プロファイルだけです。

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

スキーマは version 1 です。レポートは、既定値補完前の `requested_configuration`、正規化済み `configuration`、静的意味論の `capabilities`、adapter build/ABI の `build`、および `--probe-host` 指定時のみ返る `host`（未指定なら null）を分離します。host probe は専用 RAM 上で一時 CPU を初期化するだけで、任意のワークロードの互換性を証明しません。利用可能性は変化し、未対応 backend への暗黙 fallback はありません。CLI は backend 不在でも有効なレポートなら 0、無効な設定・照会なら 1 を返します。

## SDK と C++ の境界

[`neverd_cpu_capabilities_json`](../../include/neverd/sdk/NeverDCAPICPU.h) は既存 session、任意の設定 JSON、0/1 の `ProbeHost` を受け取ります。ロード済み image は不要です。結果は `neverd_free_string` で解放し、NULL の場合は `neverd_last_error` を確認します。CPU 無効ビルドも同じ関数を公開し、無効状態を明示します。Python plugin は `session.cpu_capabilities(...)` を使います。C++ では `executionCapabilities`、`resolveExecutionConfiguration`、`queryExecutionBackendBuild`、`probeExecutionBackend` を個別に利用でき、`createExecutionBackend` は既存空間に CPU を接続するか専用 RAM と既定空間を作ります。

## CPU の結果と予算

`CPU.runUntilExit(PC, TimeoutMicroseconds)` は型付き [`ExecutionExit`](../../include/neverd/emulation/ExecutionExit.h) を返します。実行開始前の設定エラーは `llvm::Error`、開始後は stop/deadline/service request/回復可能 fault/guest fault・trap/未対応操作/device・backend failure/説明できない engine stop を明示します。CPU/device/backend fault は同時 stop/deadline より優先され、独立した事実と詳細は保持されます。timeout は正で、duration と絶対 deadline の両方に表現可能でなければなりません。無効なら CPU 状態を変更する前に失敗します。各呼出しには有限予算が必要で、0 は無制限でも有効な即時 timeout でもありません。制御は協調的で、厳密な実時間期限ではありません。戻り値は回復可能 fault を消費しません。OS owner が取得し、再開前に検証済み例外遷移を設定します。既存の `run`、`fault`、`timedOut` は維持されますが、`run` だけを override する外部 CPU 実装は新しい型付き境界を拒否します。

## サービス要求

checked user x64 は正確な prefix なしの `SYSCALL` encoding のみを intercept し、checked user ARM64 は `SVC #imm16` を intercept します。`SYSENTER`、`INT`、`HVC`、`BRK` などは未対応です。命令 observer を先に実行し、stop/fault がなければ backend に入る前、命令実行前に `ExecutionExitKind::ServiceRequest` を返します。結果には種類、元の `PC`、順次 `NextPC`、SVC immediate が含まれます。レジスター、flags、stack、権限は変わりません。x64 の RCX/R11 clobber も、ARM64 の例外 vector への移行も発生していません。SVC immediate は汎用 service 番号ではありません。

request は pending のまま実行、CPU 変更、空間 binding、context capture/restore をブロックします。停止中に OS owner が `takeServiceRequest()` で一度だけ消費します。owner が OS ABI を解釈し、service を処理して、結果レジスターと次の PC/例外遷移を明示的に選択します。未対応 service はその owner で失敗し、元の PC を再実行すれば新しい request が発生します。暗黙の NOP や成功結果はありません。service event は同時 stop/deadline より優先されますが、guest/backend failure はさらに優先です。CPU stop、software HLT、deadline、trap は workload 成功を意味しません。[Linux process profile](process-emulation.md) は独立した OS model を使い、Windows/Android/Darwin の動作を証明しません。Windows と ARM64 の native 実行は実機での検証が必要です。

## x64 拡張と native CPU state

例外をマスクした x64 命令の説明は移植可能な基準です。KVM/WHP は `driver-strict`、`checked-x64-v1`、`checked-user-x64-v1` に `precise_simd_exceptions` を追加します。ネイティブ起動検証が正確な `#XM` と両方の再試行を確認してから、非マスク MXCSR 書き込み、`LDMXCSR`、Windows `CONTEXT` 復元を許可します。選択は `ExecutionProfiles.def` が管理し、`supportsSIMDExceptions` が実際のインスタンス能力を示します。checked Unicorn はマスクを要求し、ARM64 と HVF の例外能力は変更しません。

ネイティブ x64 KVM/WHP は実際の `#XM` を `X64SIMDException` 経由でドライバーの C SEH に渡します。ハードウェア例外の `CONTEXT` は XMM0–15 と MXCSR を保持します。フィルター、例外アンワインド中の finally、選択されたハンドラーは MXCSR `0x1f80` と DF クリアで実行されます。負のフィルターは XMM と最上位の `CONTEXT.MxCsr` を変更して元の命令を再試行できます。後者にはゲスト CPU のマスクを適用し、`FltSave.MxCsr` はカーネル復元に使用しません。モデル API の例外は整数/制御レコードを維持し、x87/AVX コンテキスト変更は引き続き拒否します。

checked x64 は限定された legacy SSE/SSE2 move/logical、`MOVLHPS`/`MOVHLPS`、mask 付き scalar `CVTTSS2SI`/`CVTTSD2SI`/`SUBSS`/`SUBSD` を許可します。MXCSR は sticky status、rounding、FTZ を保持し、移植可能な実行では unmasked exception を拒否します。KVM/WHP は16個すべての XMM register と MXCSR を同期します。列挙されていない encoding/operand は許可されません。

checked x64 は mask 付き legacy `ADD`、`SUB`、`MUL`、`DIV`、`SQRT`、`MIN`、`MAX` の `SS`、`SD`、`PS`、`PD` 形式も許可します。`X64SSEInstructions.def` が operand 幅、alignment、admission を一元管理します。`MaskedSSEArithmeticMatchesIndependentHostExecution` は独立した host CPU oracle で register/RAM 形式、4 種の rounding、FTZ、signed zero、subnormal、NaN を検証し、`SSEMemoryObserverStopsBeforeResultAndStatusChanges` は効果反映前の停止を検証します。x87、AVX は許可しません。

可搬ソフトウェアプロファイルでは、Unicorn は従来の `MINSS/MINSD/MINPS/MINPD` と `MAXSS/MAXSD/MAXPS/MAXPD` が選択した値に DAZ を適用します。選択された非正規化入力は符号付きゼロになり、NaN ペイロードと既存の MXCSR 状態は保持されます。`test_x86_sse_minmax_daz` はレジスタ、RAM、同一レジスタの形式について、DAZ の有効・無効、全丸めモード、FTZ、累積状態を検証します。

KVM/WHP は非公開の `FXSAVE64` 実行で `MXCSR_MASK` を調べ、符号付き非正規化数の演算で DAZ 能力を検証します。checked Unicorn はソフトウェアのマスクを提供します。`supportedControlBits` は CPU 固有の不変マスクを返し、FX/XSAVE、スナップショット、Windows CONTEXT が同じ能力を使用します。checked `LDMXCSR/STMXCSR` は m32 全体の RAM 権限を確認し、障害や観測コールバックの中止時には状態を保持します。予約ビットのロードは #GP、移植可能な実行では非マスク SIMD 例外を拒否します。`X64MXCSRTests.cpp` は制御と再試行を検証し、`X64DAZData` は許可された全 28 種の SSE 演算を実機の元命令と比較し、DAZ、丸め、FTZ を確認します。HVF の DAZ 対応は拡張しません。

`PAUSE`（`F3 90`）は、ネイティブの `driver-strict` を含む KVM/WHP と checked Unicorn で、共通の x64 マシン境界を通して実行されます。`X64PauseTests.cpp` は全状態の保持、実行前の停止、コンテキストの復元、不正な `LOCK` の拒否、スピンループの期限切れと再開を検証します。このプロセッサヒントはゲストスレッドをスケジュールせず、特定の遅延も保証しません。

`X64PackedIntegerInstructions.def` は、桁あふれを切り捨てる加減算と飽和加減算、比較、乗算、平均、最小・最大、バイト差、パックとアンパックを含む 45 個の legacy SSE2 packed integer 命令を許可します。XMM と整列した 128 ビット RAM の入力は KVM、WHP、Unicorn の既存 checked 経路を共有します。FLAGS と MXCSR は変化せず、障害や監視コールバックによるキャンセル時は状態を保持します。MMX、VEX/EVEX、デバイスオペランドは対象外です。

`X64PackedShiftInstructions.def` は十種類の legacy SSE2 パックシフトを受け入れます。要素シフトの回数は imm8 または XMM／整列済み m128、バイトシフトは imm8 のみです。可変回数は符号なし下位 64 ビットを使い、スカラーシフトのマスクを適用せず、上位 64 ビットを無視します。ゼロや範囲外の回数でもメモリから 16 バイト全体を読み取ります。FLAGS と MXCSR は不変で、MMX、VEX/EVEX、デバイスオペランドは対象外です。

`X64VectorOperands.def` が従来の SSE 転送、演算、シフト、変換、マスクの完全なオペランド規則を定義します。`MOVMSKPS`、`MOVMSKPD`、`PMOVMSKB` は XMM の符号ビットを r32/r64 に抽出し、宛先の残りのビットをゼロにします。KVM、WHP、checked Unicorn はこの受け入れ規則を共有し、FLAGS、MXCSR、ソースレジスタを保持します。マスクのメモリオペランド、MMX、VEX/EVEX 形式は未対応です。

`X64ShuffleInstructions.def` は `PSHUFD`、`PSHUFHW`、`PSHUFLW`、`SHUFPS`、`SHUFPD` を追加します。`X64VectorOperands.def` は XMM 宛先、XMM または整列済み m128 ソース、imm8 の完全な3オペランド形式を要求します。元の命令は FLAGS と MXCSR を保持し、ビット列としてレーンを選択します。メモリ形式は16バイト全体を検証し、整列違反はデータ観察より先に発生します。KVM、WHP、checked Unicorn はこの規則を共有します。 同じ一覧は2オペランド形式の `UNPCKLPS`、`UNPCKHPS`、`UNPCKLPD`、`UNPCKHPD` も受け入れ、元の宛先とソースのビット列を交互に配置します。ハードウェアは選択された64ビットだけを取得できます。checked RAM は整列済み m128 オペランドを検証します。

`MOVLPS`、`MOVHPS`、`MOVLPD`、`MOVHPD` は、アラインメントを要求せず RAM の正確に 8 バイトを転送します。`X64VectorInstructions.def` がストアする半分を宣言し、`X64VectorOperands.def` が XMM/m64 の組を要求します。ロードは残りの 64 ビットを保持し、上位半分のストア監視には上位データを渡します。KVM、WHP、checked Unicorn は全範囲の権限検査と RAM ロールバックを共有します。レジスタ専用の `MOVHLPS`/`MOVLHPS` は固有の意味を維持します。

`CVTSI2SS` と `CVTSI2SD` は MXCSR の丸め規則で符号付き 32/64 ビット整数を変換し、精度状態を保持します。共有の `IntegerSource` 規則は XMM 出力と r32/r64 または m32/m64 入力のみを許可します。従来形式は出力の上位 96/64 ビットを保持し、メモリ検査には整数幅を使います。KVM、WHP、checked Unicorn は元の命令を実行します。MMX、VEX/EVEX は対象外です。

`CVTSS2SI` と `CVTSD2SI` は共有の `IntegerResult` 規則で MXCSR の丸めに従い符号付き 32/64 ビット整数を生成し、`CVTTSS2SI` と `CVTTSD2SI` は常にゼロ方向へ切り捨てます。例外をマスクした NaN・範囲外変換は整数不定値を返して無効状態を設定し、有効な不正確結果は精度状態を設定します。既存の累積状態、FLAGS、XMM 入力は保持されます。r32 出力は汎用レジスタの上位半分をゼロにします。RAM 読み取り幅は出力幅に関係なく浮動小数点入力幅で決まり、FTZ は非正規化入力を破棄しません。KVM、WHP、checked Unicorn に共通の規則です。

`COMISS`、`COMISD`、`UCOMISS`、`UCOMISD` は共有 `Source` 規則でスカラー XMM または m32/m64 を比較します。CF/PF/ZF を設定し OF/SF/AF を消去し、他の FLAGS と入力レーンを保持します。COMIS は全 NaN、UCOMIS は signaling NaN のみで無効状態を設定し、NaN 処理は非正規化状態より優先します。MXCSR の累積ビットは保持され、丸めと FTZ は比較に影響しません。KVM、WHP、checked Unicorn は正確なメモリ検査を共有します。固定版 Unicorn の比較関数は既存の非正規化入力分類を再利用します。

`CMPSS`、`CMPSD`、`CMPPS`、`CMPPD` は KVM、WHP、checked Unicorn で従来の8条件を実行します。共有 `Source` 規則はデコード済み別名を受け入れ、予約値は非対応です。スカラー形式は上位レーンを保持して m32/m64 を読み、パック形式は整列した m128 を要求します。FLAGS と既存 MXCSR 状態を保持し、有効レーンごとに無効・非正規化状態を累積します。Capstone が命令族 ID と SSE 条件を管理し、lifter 内だけの ID 修正を置き換えます。Unicorn は各比較関数内で非正規化入力を分類します。

`CVTSS2SD`、`CVTSD2SS`、`CVTPS2PD`、`CVTPD2PS` は共有 `Source` 規則で従来の SSE 精度を変換します。スカラー結果は宛先の上位 64/96 ビットを保持します。パック拡張は m64 から倍精度2要素へ、縮小は整列した m128 から単精度2要素へ変換し上位64ビットをゼロにします。KVM、WHP、checked Unicorn の元の命令実行は FLAGS を保持し、丸めと FTZ に従ってマスクされた MXCSR 状態を累積します。Unicorn は変換関数で有効な非正規化入力を個別に分類します。VEX/EVEX は対象外です。

`CVTDQ2PS` と `CVTDQ2PD` は共有 `Source` 規則で符号付き32ビット整数をパック変換します。単精度は整列した m128 と MXCSR の丸めを使用し、倍精度は非整列も可能な m64 を読み正確に変換します。宛先 XMM 全体を置換し、FLAGS と既存の MXCSR 状態を保持し、不正確な単精度結果は精度フラグを蓄積します。KVM、WHP、checked Unicorn は元の命令を実行します。Unicorn は両パック拡張を識別して8バイト読取を選択します。MMX、VEX/EVEX は対象外です。

`CVTPS2DQ` と `CVTPD2DQ` は MXCSR の丸めを使用し、`CVTTPS2DQ` と `CVTTPD2DQ` はゼロ方向へ切り捨てます。共有 `Source` 規則は整列した m128 または XMM 入力を要求します。NaN や範囲外のレーンは signed32 indefinite と無効フラグを生成し、他の有効な不正確レーンは独立して精度フラグを加えます。単精度入力は整数4個、倍精度入力は2個となり上位64ビットをクリアします。FLAGS と既存の MXCSR 状態を保持し、FTZ は非正規化入力を破棄しません。KVM、WHP、checked Unicorn は元の命令を実行します。MMX、VEX/EVEX は対象外です。

`X64AlignmentTests.cpp` は、許可された aligned SSE 命令の非整列オペランドがデータ監視、権限検査、デバイスコールバックより前に回復可能または終端の `#GP(0)` を報告することを検証します。障害時は公開 x64 レジスタ全体、PC、RAM を保持します。アドレス幅の折り返し後に FS/GS ベースを加算し、アドレス修復後は元の命令を再試行します。直接の KVM/WHP マシンテストがハードウェア境界を独立に検証します。Windows ring3 は分類済みの `operand_alignment` 障害を配送します。他の原因の `#GP` は未対応です。

thread pointer は x64 FS/GS base と ARM64 `TPIDR_EL0` を正確な `MRS`/`MSR` encoding で扱います。native transport と CPU snapshot は memory とは独立してこの状態を保持しますが、OS thread や TLS block を作るものではありません。supervisor x64 は1/2/4 byte の aligned scalar MMIO と restart boundary ごとに1要素の MOVS を許可します。device read には effect のない prepared preview と最大一度の commit が必要です。user profile は device mapping を拒否し、RMW、wide MMIO、port I/O も未対応です。

KVM/WHP は active native entry を cancel し、実行資源を解放する前に acknowledgement を待ちます。KVM は専用 execution thread と一時的に unblock する realtime signal を使います。entry 中、選択した signal は ignored であってはなりません。caller の signal mask/handler は変更しません。guest の進行が不確かな中断は terminal failure であり、厳密な wall-clock deadline は保証しません。

## x64 のネイティブ同期例外

checked x64 の `DIV`/`IDIV` は実際のプロセッサ結果と `#DE` を使用します。KVM は非公開の supervisor IDT/IST、WHP は明示的な例外ビットマップを使用し、元のコンテキストと利用可能なエラーコードを転送エラーと区別します。OS は回復可能なイベントを消費してから継続コンテキストを設定します。Windows ドライバはゼロ除算と商のオーバーフローを `STATUS_INTEGER_DIVIDE_BY_ZERO` に変換し、実際の SEH filter、`__finally`、再試行を実行します。`NeverDX64ExceptionTests` は Unicorn 無効でも構築でき、`DriverWDMCPUException` は元の WDK 用例を検証します。利用できない ARM64 ホストは明示的にスキップします。

## 段階的な RAM 効果

`RAMTransaction` は物理実行リースの下で、命令が宣言した書き込み範囲の物理的な和集合だけを保持します。結果観測器を呼ぶ前に元の RAM を復元し、取消し、転送エラー、観測器の例外では部分的な RAM やレジスタを公開しません。CPU 例外では RAM を戻した後もアーキテクチャの例外状態を保持します。ARM64 の単一・ペアストアも同じ管理層を使います。x64 は 8/16/32/64 ビットの `XCHG`、`XADD`、`CMPXCHG` を実行し、LOCK または暗黙のロックを持つ形式には自然整列を要求します。`NeverDRAMTransactionTests` はホスト CPU との結果比較、復元、エイリアス、権限を検証し、利用できないプラットフォームを明示的にスキップします。デバイスと並列 SMP は対象外で、CPU スナップショットは確定済み RAM を戻しません。

## 完全な x87 状態

`NeverDEmulationArch` は ISA、ページテーブル、FP 状態の配置を所有し、ネイティブと Unicorn の転送が共有します。x64 コンテキストは x87 制御、状態、TOP、物理タグ、オペコード、命令／データポインター、8 個の 80 ビットレジスターを保持します。`FP0`–`FP7` は `RegisterValue` を使い、スカラーアクセスによる切り捨ては拒否します。`FPTag` は物理レジスターの非空ビットマップです。`NeverDX64FPTests` は全 TOP、正確な演算のホスト FXSAVE/FXRSTOR 比較と復元を検証します。checked x87 命令や全丸め意味論の証明を追加するものではなく、利用できないネイティブホストは明示的にスキップします。

`driver-strict` は一致する Linux x64 host の KVM と Windows x64 host の WHP を許可します。`auto` は対応する native transport を選び、cross-ISA は Unicorn を選びます。明示的な Unicorn と従来の V1 API は portable software profile を保持します。native 実行は entry 前に canonical address と instruction effect を検証し、hardware 不可用時は fallback なしで失敗します。未対応 instruction/OS behavior は明示的な error です。Windows x64 のネイティブ CI は Unicorn を無効にして必須の 359 検査すべてに合格します。内訳は CPU 検査 131 件、組み込みイメージ 26 個・WDK イメージ 46 個・シナリオケース 40 件を優先アドレスと再配置先で実行したドライバー結果 224 件、および SEH 境界検査 4 件です ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). native ARM64 の実機証拠は未取得で、任意 driver や Android/Darwin の互換性を保証しません。

選択したバックエンドの機能は `executionCapabilities(Contract, ISA, Backend)` で照会します。`NativeLegacyX64` はネイティブ x64 ドライバー実行を表し、`NeverDNativeDriverTests` は既存のドライバー群を検証します。このテストは Unicorn を無効にしたビルドでも実行できます。

Checked ARM64 の完全な状態は一つの境界で確定します。`Registers.def` が39個のスカラー項目と32個の128ビットベクトルを定義し、`captureAArch64State` が全読み取り、ビット幅、NZCV 正規化を検証して一度だけ公開します。Unicorn、KVM、WHP、HVF は TPIDR_EL0、TPIDRRO_EL0、TPIDR_EL1、FPCR、FPSR を含む同じ状態を転送します。native adapter は CPACR_EL1 で FP/SIMD を有効化します。読み取り失敗や entry の取消では呼び出し側の全状態を保持します。

ARM64 KVM/WHP/HVF の初期化は専用の `AArch64MachineProbe.def` を実行します。NOP、正の無限大へ丸める FP32 加算、2レーンの SIMD 加算です。各ステップで39個のスカラー値と32個のベクトルを比較し、TLS、NZCV、結果の上位ビット消去、FPCR/FPSR の保持と累積状態を確認します。監視用メモリは supervisor 専用で、全体の期限は共通です。この検査が証明するのは限定された初期化のみです。Linux ARM64 KVM と Windows ARM64 WHP のワークロード検証は未完了です。macOS のネイティブ検証結果は [HVF ガイド](macos-hvf.md) に記録されています。 このプログラムには、鍵を無効にした A/B リターンアドレス署名・認証と、保護されていないページでの4種類の BTI 命令も含まれます。

プローブは `MRS CTR_EL0` を2回実行し、`DC CVAU`、`DSB ISH`、`IC IVAU`、`ISB` とともにキャッシュ構成の安定性と完全な状態を検証します。Checked EL0/EL1 は元の命令、名前付きの基本 DSB オプション、ISB SY を許可します。CTR は選択した仮想 CPU に由来し、転送方式によって異なる場合があります。対象は現在の権限で読み取り可能な通常 RAM に限り、非整列アドレスと別名も許可します。それ以外は未対応として拒否し、保守操作によるデータ読み書きの観測イベントは生成しません。投影は命令実行の整合性を保ちますが、プライベートキャッシュの内容や並列ハードウェア SMP はモデル化しません。`NeverDAArch64CacheTests` は完全な状態、読み取り専用ページ末尾、拒否、停止、コンテキスト、予算、ページ境界をまたぐ RW/RX 別名による guest コード更新を検証します。利用できない KVM/WHP ホストは明示的にスキップします。

x64 KVM/WHP/HVF のネイティブ初期化は、非公開の supervisor ページで `X64MachineProbe.def` を実行します。単一の期限内で NOP、正の無限大方向に丸める FP32 加算、2 レーンの SIMD 加算、FS/GS ロード、CS/SS/CR8 読み出しを行い、各ステップで全スカラー、XMM、物理 x87、制御状態を比較します。x64 と ARM64 の検査には物理メモリの排他的実行リースが必要です。`MemoryProjection` がキャッシュ識別子（ISA、アドレス空間、マッピング世代、権限、モニター構成）と ISA ごとの確定済みページテーブルルート履歴を所有します。非公開バイトを書き換える前にキャッシュを無効化するため、再構築失敗時の不完全なテーブルや呼び出し側の古いルートを再利用しません。この検査が証明するのは限定された初期化のみです。Linux ARM64 KVM と Windows ARM64 WHP のワークロード検証は未完了です。macOS のネイティブ検証結果は [HVF ガイド](macos-hvf.md) に記録されています。

共有 XSAVE デコーダーは標準形式と圧縮形式の SSE 初期状態を区別します。XSTATE_BV[1] が 0 の場合、どちらも XMM を初期化しますが、標準形式は MXCSR を読み取り検証し、圧縮形式は MXCSR を初期化します。`X64XsaveCases.def` は独立したデータ配置と独自のホスト XRSTOR プログラムを提供します。`X64XsaveTests.cpp` は拒否時の状態の原子性を検証し、呼び出し元の FP/SSE 状態を保存しながら、両形式を実ホストの実行結果と比較します。ホストのアーキテクチャーや必要な命令機能が利用できなければ明示的にスキップします。

`X64FPState.def` は圧縮 AVX、AVX-512、CET_U/CET_S、AMX の転送配置と成分の 64 バイト境界を宣言します。存在する拡張成分は全ゼロの初期状態に限り、欠落成分のデータと境界調整領域は状態を定義しません。配置ビットがオフセットを決め、未知の配置、非初期値、不正な長さは公開前に失敗します。`CompactedOffsetsFollowLayoutRatherThanPresentBits`、`WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`、`InitialCETComponentsDoNotHideFPState`、`InitialWideComponentsDoNotHideFPState` は 872 バイトと 10752 バイトの WHP パケットを検証します。これらの拡張命令の実行を許可するものではありません。

`WhpXsaveRegisters.def` は完全な XSAVE パケットを名前付き x87/SSE 制御レジスターで補完します。最終オペコードと命令・データポインターを明示的に書き込み、ホストから取得します。ゼロのパケット項目は補完できますが、非ゼロのメタデータ衝突や共通制御値の不一致は状態公開前に失敗します。`NamedMetadataRestoresOmittedPacketFields` は FP ペイロードを保持したまま欠落項目を検証します。

ネイティブの `FOP/FIP/FDP` はホストの x87 保存・復元規則に従います。マスクされていない保留例外がなければ AMD はこれらをゼロにでき、スナップショットは観測値を保持します。`X64MachineProbe.def` と厳密な NOP/コンテキストテストは整合する保留例外を設定し、全フィールドを有効な状態で差分を隠さず比較します。ホストプロセスの FXRSTOR64/FXSAVE64 参照は両状態を検証し、バックエンドはホストの結果を入力メタデータで置き換えません。

共通の `encodeX64XsaveState` / `decodeX64XsaveState` が標準・圧縮 FP/SSE パケット、物理 TOP の回転、欠落成分の初期状態、アトミックな検証を所有します。WHP は完全な XSAVE API を使い、`WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState` を優先し、旧 XSAVE API を互換経路とします。旧式の個別 x87 レジスター転送は完全なパケットを代替できません。非初期状態の拡張成分、不正なヘッダー、制御値、切り詰められた取得結果は明示的に失敗します。WHP マッピングエラーは診断用に HRESULT、GPA、サイズを保持します。

`CheckedX64Instructions.def` は既存の CPU バックエンドで 8/16/32/64 ビットの符号なし `MUL` と `CBW/CWDE/CDQE/CWD/CDQ/CQO` を許可します。`NeverDX64IntegerTests` は独立した `X64IntegerCases.def` の命令列と期待値を使い、両特権レベルで部分レジスターの保持、32 ビットのゼロ拡張、積の上位・下位、定義された CF/OF、符号拡張によるフラグの不変性を検証します。通常 RAM の乗算はアクセス範囲全体の権限検査と読み取り観測を維持し、障害や観測コールバックによる停止では暗黙の出力レジスターと PC を保持します。デバイスオペランドは未対応です。checked Unicorn でも実行し、利用できないネイティブバックエンドは明示的にスキップします。

`X64BitInstructions.def` は 16/32/64 ビットのレジスタと通常 RAM の `BT/BTS/BTR/BTC` を許可します。レジスタのビット索引はオペランド幅の符号付き値としてワード全体を選択し、即値は基底ワード内に限定されます。アドレス幅による切り詰めは FS/GS 基底の加算より前に行います。CF と書き込み値はプロセッサが生成し、`RAMTransaction` は観測コールバックの承認まで結果を非公開に保ちます。全範囲の権限検査は独立したページ割り当てとエイリアスを対象とし、停止、コールバック失敗、アクセス拒否では元の CPU と RAM を保持します。LOCK は自然整列されたメモリ変更形式に限られ、MMIO とハードウェア並列 SMP は未対応です。`X64BitStringTests.cpp` は独立した符号化を x64 ホストの実行と比較し、負の索引、幅の切り詰め、ページ境界、キャンセル、不正な LOCK 形式を検証します。[Intel 命令リファレンス](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)を参照してください。

`X64StringInstructions.def` は通常 RAM の 8/16/32/64 ビット `MOVS/STOS/LODS` を管理し、`CLD/STD` は他のフラグを変えずに方向を制御します。REP は各要素の全範囲を観測前に検証し、再開可能な境界で確定します。後続の障害でも完了済み要素は残り、停止やコールバック例外は現在の要素を変更しません。FS/GS はアドレス幅の切り詰め後にソースだけへ加算します。AL/AX のロードは上位ビットを保持し、EAX はゼロ拡張します。32 ビットアドレスのゼロ回 REP はカウントの上位ビットがゼロである必要があり、MOVS/STOS では使用するアドレスレジスタも同様です。それ以外は実 CPU ごとに結果が異なります。MOVS/STOS/LODS の REPNE 形式と STOS/LODS のデバイス操作数は未対応です。`X64StringTransferTests.cpp` は独立したホスト命令で幅、方向、重なり、ゼロ回を照合し、権限、エイリアス、折り返し、障害、再開も検証します。独自の WDK リソースドライバは `driver_resource_strings.def` を使い STOS/LODS の全四幅を実行します。

`X64StringInstructions.def` は通常 RAM 上の 8/16/32/64 ビット `CMPS/SCAS` と `REPE/REPNE` も管理します。各要素は観測前に読み取り範囲全体を検証し、六つの算術フラグを更新して最初の終了条件で停止します。データ障害では、この連続した REP の開始時のフラグを復元し、完了済みのポインタとカウント更新は保持します。公開 API からの再開は公開済み CPU 状態を出発点とします。停止や観測例外は現在の要素を変更せず、早期終了後は次の要素を読みません。FS/GS は CMPS のソースだけに作用し、SCAS は累算器と未使用のソースレジスタを保持します。デバイス操作数と曖昧な 32 ビットゼロ回実行時の上位ビットは対象外です。`X64StringComparisonTests.cpp` は独立したホスト命令とフラグ、方向、エイリアス、折り返し、権限、再開を照合し、Linux x64 シグナルで実際の障害時レジスタも検証します。独自 WDK リソースドライバは `driver_resource_strings.def` で全四幅の両条件反復を実行します。[Intel 命令リファレンス](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)を参照してください。 Linux のネイティブ検証は最初の要素の実行前後の障害を対象とし、Intel の開始時 flags 復元と Hyper-V 上の AMD EPYC 7763 で観測された直前の比較 flags の保持を区別します（[ネイティブ観測](https://github.com/NeverSight/NeverD/actions/runs/37202522130)）。未知の CPU ベンダーは明示的に失敗します。checked ゲストは全バックエンドで開始時 flags を復元します。

`WhpResourceCache.h` は論理 CPU の状態と WHP パーティションを分離します。ランタイムは一つのネイティブパーティションを保持し、同じ CPU の連続ステップでは再利用します。CPU 切り替え時は古いパーティションを破棄してから、マッピングと仮想プロセッサを再構築し、完全な状態を復元します。各論理 CPU は独立した `MemoryProjection` ビューと正本の RAM を保持します。リース取得はキャンセルと現在の期限を守り、非アクティブ CPU の破棄は別の CPU のパーティションを破棄しません。x64 はホスト既定の XSAVE 機能群を保持し、`WHvGetPartitionProperty` で実効設定を検証します。依存機能を消してマスクを縮小しません。協調的な CPU 切り替えは並列ハードウェア SMP を提供しません。

`CheckedBackend` は CPU ごとに命令フェッチ用バッファと `cs_disasm_iter` の命令レコードを保持します。各ステップで実行権限のあるバイトを読み直して再デコードし、コード書き込み、エイリアス変更、再開後に古いデコード結果を使いません。実行リースは再利用領域に触れる前に再帰実行を拒否します。命令ごとの領域確保をなくしつつ、命令観測、システムサービスの捕捉、正確な障害処理を維持します。 固定版 Unicorn の単一ステップは間接変換検索も含めて後続命令のフェッチ前に終了し、内部のコード書き込み再試行を完了済み命令として数えません。

`WhpX64Partition.h` は実際の分割区画ごとに x64 WHP のレジスタ再利用を管理します。固定パケットは `WhpX64Registers.def` と `X64HostRegisters.def` を使用し、成功した各ステップで汎用・制御・セグメントレジスタと完全な FP/SSE 状態を取得します。完了確認済みのデバッグ終了だけが未変更入力の省略を許可し、比較では予約ビットと共用体のパディングを無視します。CR3、CPL、TLS、汎用または FP 入力の変更は再設定され、部分失敗、キャンセル、例外は再利用を無効化します。区画再作成時は全状態を設定します。命令の許可範囲を広げず、処理全体の高速化も主張しません。

WHP は `WhpXsaveRegisters.def` の x87/SSE メタデータを通常のレジスタと同じ `WHvGetVirtualProcessorRegisters` 呼び出しで取得します。停止中の vCPU は同じ区画リースで保護されます。公開前の完全な XSAVE 取得とすべてのメタデータ整合性検査は維持します。ステップごとのホスト API 呼び出しを一つ削減しますが、スループット向上の測定結果を示すものではありません。

`CheckedAArch64Instructions.def` と `AArch64InstructionEffects` は EL0/EL1 で範囲を限定した基本 FP32/FP64 演算、比較、転送、固定幅 SIMD を許可します。FPCR は4種類の丸め、FZ、DN に対応し、FPSR は累積状態と QC を保持します。未対応の制御・状態ビットは変更前に拒否します。FP16 演算、SVE/SME、非マスク例外、追加拡張、未列挙の形式は明示的なエラーです。Windows ARM64 ドライバーのロードや新しい OS 環境は追加しません。

`AArch64InstructionEffects` が最大128ビットの scalar/FP/SIMD 単一・ペア RAM 範囲を所有します。共有 address space は CPU entry 前に全ページを検証し、`RAMTransaction` は宣言された完全な物理書き込みのみを確定します。128ビット書き込みは実行前に二つの64ビット値として順序付きで観測されます。停止・fault は RAM、vector、writeback を保持します。Xn/Vn の番号重複は有効で、pair 範囲のアドレス wrap は拒否します。`NeverDAArch64MemoryTests` は独立した `AArch64CrossPageCases.def` と `AArch64VectorMemoryCases.def` を使用します。

KVM x64/ARM64 は `KvmRunControl` により同じ専用 vCPU worker で状態準備、`KVM_RUN`、状態取得を実行します。`EINTR` の再試行でも準備は一度で、取消や取得失敗は状態を公開しません。`KvmAArch64Machine.cpp` の変換維持と全スカラー・ベクトル転送も一つの step deadline を共有します。呼び出し側は完了確認後に確定し、ISA decode、RAM transaction、OS policy、observer は呼び出し側に残ります。native ARM64 の実機証拠は未取得です。

KVM は `X64HostRegisters.def` と `X64FPState.def` に従い、汎用レジスタと完全な FP/SSE 状態を直前に完了確認したデバッグ終了状態と比較し、変更された入力を再設定します。ホスト側の書き込みとコンテキスト復元も比較対象です。例外、キャンセル、失敗は再利用を無効にします。単一ステップの設定と実際の汎用・FP 状態の読み取りは各命令で行います。

x64 KVM は `KVM_CAP_SYNC_REGS` を照会し、対応する `KVM_SYNC_X86_REGS` と `KVM_SYNC_X86_SREGS` を個別に使用します。完了確認済みの `KVM_RUN` は共有領域に実際のレジスタを返すため、連続して成功するステップでは `KVM_GET_REGS` と `KVM_GET_SREGS` を省略できます。非対応の集合や任意の照会の失敗では ioctl 経路を維持します。変更された入力は引き続き `KVM_SET_GUEST_DEBUG` より前に設定し、共有 dirty ビットはゼロのままです。例外、キャンセル、取得失敗は再利用を無効にします。完全な FP/SSE 取得は必須ですが、変更のない FP 入力の XSAVE 再エンコードは省略します。転送呼び出しの削減であり、処理全体の高速化を実証したものではありません。[KVM API](https://docs.kernel.org/virt/kvm/api.html#kvm-cap-sync-regs)。

ハードウェア実行だけでは、処理全体の待ち時間が短くなるとは限りません。現在のネイティブ実行は命令ごとに許可判定、観測、状態転送と VM 終了を行います。同じ元のイメージとシナリオを同じ命令・イベント予算で比較し、時間と結果の一致を併記してください。CLI の待ち時間には起動とロードも含めます。

checked Unicorn は `MachineRunControl` を使い、ARM64 の保守、ゲスト実行、完全な状態読み出しを一つのステップ時間枠で処理します。`UC_HOOK_CODE` は命令入口で借用した停止トークンと期限を確認します。同期エンジン呼び出しは戻る前に hook の借用を解除しますが、マシンステップは状態公開まで制御を保持します。Unicorn と WHP は完全な CPU 状態を一時保存し、成功したステップの公開直前に同じ制御を確認します。WHP は準備前に時間枠を一度だけ作ります。確認済みの x64 CPU 例外は読み出し中の停止要求に優先します。読み出しが中止されると checked RAM トランザクションは投機的な書き込みを破棄し、非制限ソフトウェア契約は変わりません。 `MachineInterruptedError` は確認済みの中止をホストや状態読み出しの失敗と区別します。共通 checked CPU は `Stopped` または `Deadline` を返し、CPU/RAM を保持して再試行を許可します。同時に停止要求があっても実際の失敗は `BackendFailure` のままです。

`RunDeadline::invoke` は WHP の停止済み・期限切れの実行をホスト呼び出し前に拒否し、キャンセル中も実際のホスト結果を保持し、借用した停止トークンを解放する前に割り込みコールバックの完了を確認します。KVM と WHP は、完全に取得した非公開状態を実行リースの所有スレッドで検証してから、同時に到着した停止や期限を分類します。実際のホスト・取得エラーと認証済み x64 CPU 例外が優先されます。通常の成功状態はキャンセル確認が終わるまで公開せず、確認済みの中断では投機的な CPU/RAM 効果を破棄して再試行を許可します。準備、ネイティブ実行、状態取得には単一のステップ猶予を使います。協調キャンセルを提供しますが、厳密な実時間上限は保証しません。

## macOS HVF

`hvf` · Hypervisor.framework · Apple Silicon → ARM64 · Intel Mac → x86-64.

[Setup, signing and native hardware validation (English)](../macos-hvf.md)
