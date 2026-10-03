**言語**: [English](../emulation.md) | [简体中文](../zh-CN/emulation.md) | [繁體中文](../zh-TW/emulation.md) | [日本語](emulation.md) | [한국어](../ko/emulation.md) | [Français](../fr/emulation.md) | [Deutsch](../de/emulation.md) | [Español](../es/emulation.md) | [Italiano](../it/emulation.md) | [Русский](../ru/emulation.md) | [العربية](../ar/emulation.md)

<!-- i18n-source: 958937aacc6b214e71730502c8f4d249d0c6417721a28a32c65de1e4fa927d63 -->

[← ドキュメント索引](README.md)

# CPU 実行とゲスト環境

<!-- i18n-section: backends -->

## CPU バックエンドとワークロード

CPU 実行は ISA 検証、ゲストメモリー、バックエンド転送、ゲスト OS 方針を分離します。`NEVERD_ENABLE_CPU_EMULATION` は x64/ARM64 CPU 層を有効にし、`NEVERD_ENABLE_DRIVER_EMULATION` は範囲を限定した x64 Windows WDM/KMDF 環境を追加します。`linux-elf64-v1` は対応する Linux ELF プロセスを実行します。[CPU 実行](cpu-execution.md)、[ゲストプロセスのエミュレーション](process-emulation.md)、[Windows ドライバーエミュレーション](driver-emulation.md)を参照してください。

対応するネイティブ契約では、ゲストとホストの ISA が一致する場合、`auto` は Linux の KVM、Windows の WHP、[macOS の HVF](macos-hvf.md) を選びます。異なる ISA では Unicorn を使い、`software-cpu-v1` と従来の V1 API はソフトウェア実行を維持します。明示したバックエンドが利用できない場合はフォールバックせず失敗します。ネイティブ実行は入場前に命令、アドレス、効果を検査します。ホスト仮想化はゲスト OS を決めず、[Darwin プロファイル](darwin-emulation.md) が macOS、iOS、iOS Simulator を個別にモデル化します。HVF には `com.apple.security.hypervisor` エンタイトルメントが必要です。

`driver-strict` / `checked-x64-v1` は限定された x64 実行を対象とし、Windows ドライバーのロードも x64 に限定されます。`checked-aarch64-v1` と `checked-user-aarch64-v1` は限定された ARM64 FP32/FP64、固定幅 SIMD、完全な FPCR/FPSR/ベクトル状態を含みます。ARM64 HVF のネイティブ受け入れ結果は Mac ガイドに記録済みです。ARM64 KVM/WHP のワークロード検証と Intel HVF の完全な受け入れは未完了です。CPU 対応は任意のドライバーやアプリの互換性を意味しません。

<!-- i18n-section: windows-processes -->

## Windows プロセスとモジュール

`windows-pe64-v1` は PEB/TEB、静的・動的 TLS、`DllMain`、名前付き Win32 API、明示的な非循環 DLL グラフを持つ有界 Windows x64/ARM64 コンソールプロセスに対応します。ゲストモジュールは名前／序数によるコード・データのインポート、DIR64 再配置、転送エクスポートと実際のローダーリスト識別子を扱います。`LoadLibraryA` / `LoadLibraryW`、`FreeLibrary`、`GetProcAddress` は設定済みモジュールカタログを使用します。CRT/GUI、ARM64 のフレームベースのユーザー SEH、スレッド、一般的な Windows アプリ互換性は未完成で、ネイティブ ARM64 KVM/WHP の証拠も未取得です。

`WindowsSystemModules` は両 ISA 向けに `ntdll.dll`、`kernelbase.dll`、`kernel32.dll` の有界な PE64 モデルイメージを構築します。ASCII の `GetModuleHandleA` / `GetModuleHandleW`、`LoadLibraryA` / `LoadLibraryW`、`GetProcAddress` はそのマップ済みベースを共有し、PEB/LDR と `MEM_IMAGE` も同じイメージを示します。静的インポート、名前検索、ゲスト DLL の転送は同じ API ゲートとエクスポート解決器を使います。提供元は常駐し、ゲスト初期化コールバックを持たず、通常のゲスト DLL をすべて解放すればエントリから復帰できます。ヘッダーやエクスポートメタデータの変更で検索を停止します。未対応のシステムエクスポート名と非ゼロ序数は明示的に停止し、対応名の大小文字違いと空名はエラー 127、NULL 検索は 87 を返します。生成バイトとアドレスはモデル方針であり、Windows DLL の版別配置、実際の序数、提供元間の別名は再構築しません。`WindowsSystemTests.cpp` は独自 x64/ARM64 EXE をネイティブ Windows と比較し、初期スレッドの復帰を独立して 8 回観測します。

<!-- i18n-section: environment-memory -->

## 環境変数とメモリ

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` は PEB プロセスパラメーター内の実際のゲスト環境ブロックを共有します。名前は大文字小文字を区別しない ASCII、値は UTF-16 です。変更前に入力、容量、書き込み権限を検証します。スナップショットは後続の変更から独立し、解放時にゲストメモリを回収します。モデルのブロック上限は 64 KiB で、文字列と展開処理には境界と実行期限の検査があります。不明なポインター所有権、不正なブロック、ANSI コードページ、展開バッファーの重複は未対応です。`WindowsEnvironmentTests.cpp` は利用可能なバックエンドで独自の x64/ARM64 フィクスチャを比較し、CI では独立したネイティブ Windows オラクルを必須とします。

`WindowsProcessHeap` はプロセスヒープの割り当て、`HeapReAlloc`、解放、サイズ照会を一元管理します。サイズ変更は保持範囲のデータを維持し、`HEAP_ZERO_MEMORY` は追加領域をゼロ化、`HEAP_REALLOC_IN_PLACE_ONLY` は移動を禁止します。再割り当て失敗時は旧ブロックを保持し、NULL と `ERROR_NOT_ENOUGH_MEMORY`（8）を返すネイティブの観測結果に一致します。独立したページの縮小・解放で容量を返却し、段階的な拡張と有界コピーで実行期限を確認します。独自ヒープ、例外生成フラグ、不明な所有権、アクセス不能なコピー・ゼロ化範囲は明示的に停止します。`WindowsHeapTests.cpp` は両 ISA、強制移動、予算再利用、失敗時の原子性を検証し、CI は同じ独自 EXE をネイティブ Windows でも実行します。

Windows 仮想メモリに `VirtualAlloc`、`VirtualFree`、`VirtualProtect`、`VirtualQuery` と現在のプロセスの `FlushInstructionCache` を追加しました。OS 層が予約領域を所有し、コミット済みページ、権限、物理記憶域は `AddressSpace` が一元管理します。動的コードの書き換え、アクセス違反、メモリ予算の再利用をテストします。

<!-- i18n-section: vectored-exceptions -->

## ベクター例外と継続処理

`WindowsProcessExceptions` は同じ CPU とプロセス予算で `AddVectoredExceptionHandler`、`RemoveVectoredExceptionHandler`、`RaiseException` を実装します。順序付きハンドラーは登録・削除、入れ子の例外、モデル化 API、DLL 読み込み、プロセス終了を扱えます。x64/ARM64 のデータアクセス違反と x64 の整数除算例外は、ゲストが変更した `CONTEXT` の検証後に再開できます。汎用レジスター、SIMD、対応する FP 状態を保持し、ソフトウェア例外はモデル提供元内の実際の return 命令から再開します。保持する登録は 128 件、入れ子は 16 フレームまでです。不正な処置、例外ポインターの変更、未対応フィールド、上限超過は明示的に失敗します。ARM64 のフレームベースの SEH／アンワインド、デバッガー配送、実行／ガードページ例外は未対応です。`WindowsExceptionTests.cpp` は独自 EXE／DLL をネイティブ Windows と比較します。ARM64 KVM/WHP の実機証拠は未取得です。 ソフトウェア例外レコードには `EXCEPTION_SOFTWARE_ORIGINATE`（`0x80`）が付き、呼び出し元の継続不可フラグとは個別に扱います。元の Windows 実行ファイルでソフトウェア例外とハードウェア例外のフラグ値を厳密に照合します。

`AddVectoredContinueHandler` と `RemoveVectoredContinueHandler` は独立した順序付きリストを管理し、例外ハンドラーと保持登録数 128 の上限を共有します。ベクター例外ハンドラーが実行再開を受け入れると、継続ハンドラーは同じ変更可能な例外レコードと `CONTEXT` を参照します。入れ子の例外や DLL 通知を含め、最終コンテキスト検証は継続コールバックの終了後に行います。異なる種類のハンドラーのハンドルは削除できません。`WindowsContinuationTests.cpp` は独自 EXE の順序、早期終了、登録変更、コンテキスト修復、入れ子の配送、ローダーコールバック、プロセス終了をネイティブ Windows と比較します。検証済みの Windows x64 ベクター処理経路は `EXCEPTION_NONCONTINUABLE` が設定されていても実行再開を許可しますが、フレームベースの SEH の動作を証明するものではありません。ネイティブ ARM64 実行は未検証です。

<!-- i18n-section: caller-context -->

## 呼び出し元のコンテキスト

`RtlCaptureContext` は x64 と ARM64 の `kernel32.dll`、`ntdll.dll` で利用できます。共通の `WindowsProcessContext` と `IntegerABI` が CPU 状態や LastError を変更せず、呼び出し元の PC/SP を保存します。ネイティブ Windows の観測で、x64 のフラグ `0x10000f`、未使用の home／デバッグ／ベクトル領域の保持、従来の 32 ビット x87 アドレス欄を確認しました。ARM64 は LR を PC に保存し、記録内の X0/LR をゼロにします。レジスタ、SIMD、浮動小数点制御はゲストから取得し、x64 セレクタと MXCSR 能力マスクは設定されたゲスト CPU に従います。無効、未整列、または一部アクセス不能な出力レコードは書き込み前に失敗します。`WindowsContextTests.cpp` は直接インポート、提供元検索、VEH コールバック、ページ境界をまたぐ出力、失敗時の原子性を検証します。`scripts/check_windows_context.py` は独自実行ファイルを Windows x64／ARM64 で実行し、非空の x87 状態を別途検証します。この ARM64 API の観測はネイティブ KVM/WHP 実行の証拠ではありません。コンテキスト復元、スタック走査、動的関数テーブルは引き続き別の実装課題です。 `WindowsProcessServices.def` は正確なモジュール制約を宣言します。`kernelbase.dll` での検索はネイティブ観測と一致する `ERROR_PROC_NOT_FOUND`（127）を返し、存在しないエクスポートを追加しません。 [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

<!-- i18n-section: structured-exceptions -->

## 構造化例外処理

`WindowsProcessSEH` は `os/windows/exception/` の共有 `X64SEH`（ドライバー環境なしでも利用可能な `NeverDEmulationWindowsException`）で x64 `__C_specific_handler` と UNWIND_INFO V1 を処理します。VEH 検索後のフィルター、finally、非局所的なハンドラーへの転送、入れ子／衝突アンワインド、再配置された EXE/DLL フレームに対応し、非揮発 GPR/XMM を保持します。フィルターによる継続では同じ `CONTEXT` で VCH を実行します。`WindowsSEHTests.cpp` は独自の 23 シナリオをネイティブ Windows と比較し、KVM/WHP/Unicorn は同じ意味論を使います。プロセス予算内でイメージ世代、ヘッダー、アンワインド／スコープのバイト列、言語ハンドラーのコード領域、IAT を再検証します。メタデータ変更や保持中のイメージのアンロードは明示的なエラーです。ARM64 のフレーム SEH、C++ EH、動的関数テーブル、汎用 RtlUnwind/NtContinue、ローダー／VEH／VCH コールバック境界を越えるアンワインドは未対応です。

`EXCEPTION_NONCONTINUABLE` に対して x64 フィルターが `EXCEPTION_CONTINUE_EXECUTION` を返すと、新しいコンテキストで `STATUS_NONCONTINUABLE_EXCEPTION`（`0xc0000025`、フラグ `0x81`、関連レコードは null）を配信します。VEH を再実行してから保持した論理スタックを再検索し、同じ深度・実行予算で finally の順序と EXE/DLL フレームの同一性を保ちます。23 のネイティブシナリオは 21 の正常実行と二つの終了を含みます。元の `CONTEXT` を復元しても、VEH/VCH がこの二次例外の継続を受け入れると未処理のまま終了し、モデルは実行時失敗を報告します。ソフトウェア例外のアドレスは保存 PC と一致し、内部ディスパッチャーのアドレスとレジスター配置はモデルの方針です。 [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

<!-- i18n-section: processor-state -->

## 命令とプロセッサ状態

検証付き x64 は通常 RAM の `MOVS/STOS/LODS` と `CLD/STD` に対応し、要素ごとの再開、停止、ページ境界を検証します。CPU 固有のゼロ回実行時の上位ビットと STOS/LODS デバイス操作数は契約対象外です。

検証付き x64 は通常 RAM の `CMPS/SCAS` と `REPE/REPNE` にも対応し、算術フラグ、早期終了、要素単位の停止、障害復旧を扱います。デバイス比較は未対応です。

x64 と ARM64 のネイティブ起動検査は、排他的メモリリース下で限定された完全状態の実行を検証します。

ネイティブ x64 の `FOP/FIP/FDP` はホストの保存・復元規則に従い、AMD は非アクティブな x87 例外メタデータをゼロにできます。起動プローブはマスクされていない保留例外でこれらを検証します。
