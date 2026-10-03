**言語**: [English](../macos-hvf.md) | [简体中文](../zh-CN/macos-hvf.md) | [繁體中文](../zh-TW/macos-hvf.md) | [日本語](macos-hvf.md) | [한국어](../ko/macos-hvf.md) | [Français](../fr/macos-hvf.md) | [Deutsch](../de/macos-hvf.md) | [Español](../es/macos-hvf.md) | [Italiano](../it/macos-hvf.md) | [Русский](../ru/macos-hvf.md) | [العربية](../ar/macos-hvf.md)

<!-- i18n-source: 3d2e1d7ba9709cac62c969fd06f4cfe7d5312c872f9339afb0161bc9643c4e33 -->

[← ドキュメント一覧](README.md)

# macOS のネイティブ CPU 実行（HVF）

NeverD は macOS の KVM/WHP 相当として [Hypervisor.framework](https://developer.apple.com/documentation/hypervisor) を使用します。`--backend hvf` は明示的な選択です。`auto` はネイティブ実行可能な契約でホストとゲストの ISA が一致するときに HVF を選びます。Apple Silicon では ARM64、Intel Mac では x86-64 を実行します。`software-cpu-v1` と異なる ISA 間の自動選択は Unicorn を使用します。Rosetta で変換された実行ファイルは拒否します。

macOS 11 以降とハードウェア仮想化が必要です。他の依存関係が要求する最低 OS バージョンは別です。選択したネイティブバックエンドが利用できない場合、理由を報告し、黙って別のバックエンドに切り替えません。[Virtualization.framework](https://developer.apple.com/documentation/virtualization) は VM 全体向けの API です。NeverD には Hypervisor.framework の vCPU、レジスタ、メモリマッピング、例外制御が必要です。

## ビルドと署名

`NEVERD_ENABLE_CPU_EMULATION=ON` またはドライバエミュレーションを有効にします。`NEVERD_EMULATION_BACKEND_HVF` は既定で `ON` で、macOS のみでフレームワークをリンクします。`OFF` でも `hvf` という名前は認識されますが、機能 API は `build_disabled` を返します。

フレームワークのリンクと hypervisor 署名は macOS ビルドターゲット（`CMAKE_SYSTEM_NAME=Darwin`）に限定します。iOS などの Apple モバイルターゲットには、このフレームワーク依存関係や権限を追加しません。NeverD 自体を ISA が一致する Mac で実行する場合、iOS ゲストプロファイルも HVF を利用できます。

**プロセスの実行ファイル**に [`com.apple.security.hypervisor`](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.hypervisor) が必要です。`libneverd.dylib` だけの署名では足りません。CMake は `resources/macos/neverd-hypervisor.entitlements` で CLI、worker、テストを署名します。`NEVERD_HVF_SIGN_IDENTITY` の既定値はアドホック署名の `-` で、既存の署名 ID も指定できます。パッケージ処理は Mach-O 依存関係の修復後に権限を再適用し、検証します。

組み込みアプリケーションは自分の実行ファイルを署名します。NeverD はインストール済み Python を再署名しません。`cpu-capabilities --configuration=JSON --probe-host` で実際のプロセスを確認できます。独立した worker も既定で署名します。`NEVERD_WORKER_SIGN_HVF=OFF` は HVF が不要な場合のみ使用します。

## 所有権と実行

`backends/hvf/HvfExecutor` はプロセスにつき一つの VM と一つの vCPU を所有し、専用スレッドで作成、使用、破棄します。論理 CPU は実行器を共有し、ネイティブへの進入を直列化します。CPU の切り替えでは以前の所有者の二つの物理領域を先に解除します。非アクティブな CPU の破棄は別の CPU のマッピングに影響しません。RAM 解放前に同期的に関連付けを解除し、部分登録の失敗はロールバックします。解除に失敗した場合はメモリ解放前に VM を終了し、回復不能な破棄エラーではプロセスを停止します。

ホスト側のマッピングはホストのページサイズに従い、Apple Silicon では 16 KiB を含みます。アーキテクチャのページテーブルとゲスト CPU の予算は 4 KiB のままです。命令の許可判定、権限、CPU 状態、メモリトランザクション、OS サービスはそれぞれの層が所有します。ネイティブ worker はゲストのオブザーバーを呼ばず、呼び出し元のコアメモリロックも取得しません。

ARM64 は固定された五つの TLB/I-cache 保守命令を単一ステップ実行し、その後に許可済みゲスト命令を実行します。`PSTATE.D` は EL2 に送られるデバッグ例外をマスクできません。スカラー、TLS、FP/SIMD の全状態を取得します。Intel は VMCS 制御を調整し、monitor trap、TLB 無効化、完全な XSAVE パケットを使います。RIP/RFLAGS は vCPU 再作成後も VMCS で直接転送します。CR0/CR4 はフレームワークのマスクとハードウェアの固定ビットに従います。認証された CR8 読み出し終了は ISA 層が完了し、他の制御レジスタアクセスは失敗します。各 vCPU は専用の管理対象 `IA32_KERNEL_GS_BASE` を初期化します。ゲスト MSR アクセスは捕捉され、未対応の MSR/SWAPGS は許可されません。

待ち行列も元の停止トークンと期限を守ります。準備、保守、進入、状態取得で一つの予算を共有します。`RunDeadline` は割り込みの確認完了を待って返ります。キャンセル後は vCPU を再作成し、遅れて届く割り込みを分離します。無関係な Intel ホスト割り込みは同じキャンセル世代で再試行します。状態取得エラーと認証済み例外は同時の停止より優先し、通常のキャンセル状態は公開しません。協調的キャンセルであり、ハードリアルタイムの保証ではありません。

## 検証手順

Release と CMake、Ninja、Python 3、Clang、`ld.lld`、`lld-link`、`ld64.lld`、`codesign` を使用します。LLVM リンカは ELF、PE、Mach-O のテスト入力を作成します。必須のネイティブ入力がなければ検証は失敗します。

```sh
cmake -S . -B build-hvf -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_BUILD_SHARED=OFF -DNEVERD_ENABLE_PYTHON_PLUGINS=OFF \
  -DNEVERD_ENABLE_CPU_EMULATION=ON \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=OFF \
  -DNEVERD_EMULATION_BACKEND_UNICORN=OFF
python3 scripts/run_native_cpu_ci.py --build build-hvf \
  --evidence build-hvf/native-evidence --require-hvf
```

Apple Silicon では `-DNEVERD_LLVM_PREBUILT=ON` を追加できます。Intel は固定された LLVM リビジョンをソースからビルドします。[HVF ワークフロー](../../.github/workflows/hvf.yml) は `self-hosted, macOS, ARM64/X64, hvf` および `macos-15-intel` の `hosted-intel` に対応します。最初に VM/vCPU を実際に作成、破棄します。`validation=probe` は命令実行の証拠ではなく、`transport` は転送層のみ、`darwin` は対応する全 Darwin ワークロード、`full` は CPU と Darwin の両方の完全な検証を要求します。

転送層の必須項目は ARM64 が 12、Intel が 10、完全な CPU 検証はそれぞれ 16、14 です。全状態、特権、権限、ページ境界、エイリアス、CPU 切り替え、ロールバック、キャンセル、再試行を確認します。Intel は大きなビルドの前に CR8 を確認します。成果物に登録一覧、ソースリビジョン、ホスト、結果、各試行を残します。[GitHub](https://docs.github.com/en/actions/concepts/runners/github-hosted-runners) はネストした仮想化を実験的と位置付けており、再現可能な受け入れには専用のネイティブ Mac が適しています。

ホスト型 Intel の `--execution-methods` は、完全な CTest 一覧から全パラメーター、フラグ、環境、作業ディレクトリを維持して GoogleTest メソッドを直列実行します。未知のプロパティは拒否します。メソッド全体の期限は最大 120 秒で、各パラメーターに個別の期限は適用しません。その後、期限を設けてプロセスグループを回収します。元の XML、名前対応、終了状態を保存します。タイムアウトや不完全な XML は部分失敗となり、必須ネイティブ項目の欠落やスキップは合格になりません。自己管理 runner は引き続き CTest のケースごとのプロセスと期限を使います。

## 検証結果と制限

2026-10-03 の記録です。範囲が重複するため各行を合算しないでください。

| 範囲 | ソース | 合格 | 失敗 | スキップ | 必須ネイティブ項目 |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 CPU 全 20 ターゲット | `defc93928` | 849 | 0 | 5,993 | 16/16 |
| ARM64 Darwin | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel Darwin | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| Intel FP ターゲット全体 | `3e01cda5c` | 35 | 0 | 36 | HVF 12 件 |

ARM64 は 6,842 件を照合しました。同じ 20 ターゲットの Intel は 6,840 件です。Intel の例外、除算、状態遷移のネイティブ 253 件も合格しました。[Intel Darwin 実行](https://github.com/NeverSight/NeverD/actions/runs/37106013999) は 286 件の識別子と 32 プロセスを照合し、転送層十件、復旧 100 回、CR8 も通過しました。収集器と監査の 124 件が合格しています。CLI、SDK、worker、186 個の Mach-O 署名を統合検証しました。このパッケージの依存関係は macOS 15.0 を要求します。

Intel CPU 全体の受け入れは未完了です。[以前の実行](https://github.com/NeverSight/NeverD/actions/runs/37106679688) は 2026-10-03 08:35 UTC に終了し、GitHub はランナーとの通信喪失を記録しました。CPU 成果物は残りませんでした。ビルドと事前確認は完全な結果の代わりにはなりません。macOS カーネルとの比較は iOS 実機カーネルの証拠ではありません。

同じ契約の小さな ARM64 ベンチマークは Unicorn 73.9 ms、HVF 95.1 ms で、所要時間は約 29 % 長く、速度向上は未証明です。通常の一命令に六回のネイティブ進入が必要です。高負荷ホストでの測定から安定した性能は主張できません。[詳細な証拠](../macos-hvf.md#implementation-validation-2026-10-02-to-2026-10-03)と[限定された Darwin 契約](darwin-emulation.md)を参照してください。

## Intel の完全なインベントリーを分割して検証

以前の完全実行 `37106679688` は 2026-10-03 08:35 UTC に終了し、GitHub はランナーとの通信喪失を記録しました。実行 `37116327329` の全四ジョブも通信を失い、CPU XML は残りませんでした。これらの事実だけでは障害を起こしたゲスト命令を特定できません。ホステッド Intel の `full` は四ジョブ、同時実行は最大二ジョブです。各ジョブで四バッチを順次実行し、合計十六シャードを `job + 4 × batch` で番号付けします。元の各ジョブのテスト集合を維持します。各バッチは二十ターゲットの完全な CTest インベントリーを構築・検査してから実行します。`--hvf-shard INDEX/COUNT` は実行属性が異なってもメソッド全体と全パラメーターを同じシャードに保ちます。

CPU 実行前に `scripts/prepare_hvf_batches.py` が完全・選択済みインベントリーとメソッド計画を保存し、ワークフローが診断資料として別途アップロードします。ローカル composite action は四バッチを順次実行し、各バッチ終了直後に元の XML、識別子対応、プロセス状態、必要な環境変数の許可リストをアップロードします。外側の 30 分期限は四バッチと全アップロードをまとめて制限します。一バッチが失敗すると後続の実行は止まります。失敗やタイムアウト後もランナーと通信できれば、別枠の二分で診断資料をアップロードします。通信喪失時はそれ以前に保存できた証拠だけが残ります。計画や未完了の診断資料は合格シャードには数えません。

別の Linux ジョブの `scripts/audit_hvf_shards.py` が、チェックアウトしたソースからターゲットと必須ネイティブ項目を再導出します。ワークフローは同じ試行の CPU 成果物だけをダウンロードし、監査は同じクリーンなコミットの全十六シャード、正しい macOS ホスト ISA、一致する正規化済み実行契約を要求します。シャードは重複せず、和集合が完全な一覧と一致し、全子プロセスの正常終了と必須項目の合格が必要です。欠落、フィルター変更、要約の不一致、不完全な XML、必須項目のスキップは失敗です。各ネイティブジョブは転送、回復、CR8、独立 Darwin 検証も維持します。セルフホステッドでは分割しない CTest を使います。分割保存自体は Intel の合格証拠ではありません。再試行では全ネイティブジョブを再実行し、過去の試行の成果物は混在させません。

[実行 `37123148209`](https://github.com/NeverSight/NeverD/actions/runs/37123148209)、クリーンなソース `f5f29a484` の最初の検証済み CPU バッチはシャード `1/16` です。31 メソッドプロセスで登録結果 476 件を照合し、82 件合格、0 件失敗、394 件スキップ、このシャードの必須ネイティブ一件も合格しました。成果物 `11274755752` の SHA-256 を確認し、元の XML、子プロセス終了状態、インベントリー、メソッド計画を実行前の計画と照合しました。これは Intel の部分的な証拠であり、CPU 全体の受け入れ完了を意味しません。

## 最新のローカルネイティブ検証

2026-10-03 UTC、クリーンなソース `4ce0b8247`。ARM64 の完全な一覧を 503 個のメソッドプロセスで実行し、すべての元の XML、実行契約、子プロセスの終了を独立に照合しました。独立した Darwin 検証も合格しました。行は重複するため合算しません。

| 範囲 | 登録 | 合格 | 失敗 | スキップ | 必須ネイティブ |
| --- | ---: | ---: | ---: | ---: | ---: |
| CPU、完全な一覧 | 7,003 | 867 | 0 | 6,136 | 16/16 |
| Darwin | 286 | 65 | 0 | 221 | 39/39 |

`build-hvf-native/hvf-current-4ce0-full-evidence/summary.json` · `build-hvf-native/hvf-current-4ce0-darwin-evidence/summary.json`

## 独立した Intel 診断

手動の [Intel 診断ワークフロー](../../.github/workflows/hvf-intel-diagnostic.yml) は、制御側と検証対象のソースを別々にチェックアウトします。`source-ref` は完全なコミット SHA、`shards` は元の十六分割の番号です。`first-method` はゼロ始まりで、`method-count=0` は残りすべてを選択します。`case-index` で元のパラメーターを一つ選べるのは `method-count=1` の場合だけです。選択前に全二十ターゲットの完全な一覧を取得し、元の Release ビルド、コマンド、パラメーター、必須ネイティブ検査を維持します。

`intel-image` は完全なワークフローと同様に、既定の `macos-15-intel` または比較用の `macos-26-intel` を選択します。実行タイトルに選択したイメージが表示され、可用性検査は引き続きネイティブ x86-64 ホストを要求します。イメージの変更には OS、SDK、ツールチェーンが含まれるため、カーネルだけの変更を切り分けるものではありません。

ジョブ全体の上限 180 分のうち、コンパイルには独立した 120 分の枠を設けています。macOS 26 での最初の完全ビルドは 76 分かかりました。ネイティブメソッドの実行上限は 120 秒、診断 Action の上限は 30 分のままです。

各メソッドの実行前に、不変の実行計画とホストのスナップショットをアップロードします。実行後は元の XML、プロセス回収、制御側の状態、二つ目のスナップショットを保存します。メモリ、スワップ、負荷、ディスク、プロセス ID・状態・CPU・RSS・実行ファイル名を記録し、引数や環境変数は含めません。収集エラーも残します。実行またはアップロードの失敗で後続メソッドを停止します。各メソッドの実行上限は引き続き 120 秒ですが、ホストとの通信が失われると回収や最終アップロードはできない場合があります。その場合、既にアップロードした証拠だけが残ります。部分診断は完全な CPU 検証や独立した Darwin 検証を満たしません。最後の開始マーカーは実行の境界を示すだけで、障害命令や根本原因を特定するものではありません。

完全検証用の `Native macOS HVF` ワークフローも任意の `source-ref` を受け付けます。省略時はワークフローのコミットを使い、指定時は完全な SHA が必要です。ネイティブジョブと集約監査は同じソースをチェックアウトして確認します。制御側のリビジョンが異なっても、監査は検証対象のソースコミットに対して証拠を照合します。

`hosted-intel` では、完全検証ワークフローの `intel-image` に `macos-15-intel`（既定）または `macos-26-intel` を指定できます。どちらも[公式 runner イメージ一覧](https://github.com/actions/runner-images)に掲載されています。同じ `source-ref` でホスト環境を比較できますが、イメージ変更には OS、SDK、ツールの変更も含まれます。VM/vCPU、ネイティブ転送、CR8、完全な CPU 検証、Darwin の要件は変わりません。イメージの選択だけでは、安定性や実行時の修正を証明できません。

`sample-active-child=true` を指定すると、メソッドの実行開始から5秒後に、実行中のスナップショットを一度だけ封存します。対象は身元を確認したネイティブ子プロセスの1秒間のスタックサンプル、最大1 MiBの現在のログ末尾、ホスト状態です。既定値は `false` です。artifact数の上限を守るため、採取を有効にしたジョブは最大166メソッドに制限します。採取コマンドの期限は5秒、レポートの上限は1 MiBです。プロセス確認や採取の失敗も記録します。独立した変更不能のディレクトリからアップロードし、失敗時はネイティブ子プロセスをキャンセルしてactionを失敗させます。採取はスケジューリングに影響するため、計測を伴う部分的な証拠として扱います。元のメソッドのタイマーをリセットせず、完全な受け入れ検証を代替しません。
