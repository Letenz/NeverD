**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: 817e1308a48a9a751b9ef618b8094d4082fed99234c2b7381269f524196f5c80 -->

[← Documentation Index](README.md)

# ソースとネイティブオブジェクトのパッチ

NeverD は指定したコンパイラで置換関数をコンパイルし、明示した元の関数入口にインストールします。C/C++ の言語意味論はコンパイラが担当し、アドレスの束縛、再配置、記憶領域、アンワインド情報、公開は NeverD が担当します。バイナリ修復とリフト後の書き換えはそれぞれのパイプラインを使い、プログラムの残りは再ビルドしません。

<!-- i18n-section: compiler -->
## コンパイラの選択

`--compiler=clang`、`--compiler=gcc`、`--compiler=neverc` を選択します。Clang と通常モードの NeverC は LLVM IR を生成できます。`object` は生成された機械命令を保持します。CLI は GCC に対してオブジェクトを既定で選び、Python SDK では `artifact="object"` の指定が必要です。他のコンパイラへの暗黙のフォールバックはありません。

例のアドレスは確認済みの元関数入口に置き換えてください。置換関数の宣言は呼出規約と構造体配置を含む元の機械 ABI と一致する必要があり、アドレスだけでは証明できません。C++ は `--language=c++` と適切な Clang++/G++ を使います。入口は `extern "C"` にするか、正確な修飾名を指定してください。補助関数は追加できます。`--entry` と `--func` で置換対象を明示し、従来の `--from-c` も指定アドレスを尊重します。

```sh
# Clang C -> LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.clang

# NeverC ordinary C -> native object
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC -> native object (CLI default for GCC)
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/gcc \
  --patch-report -o app.gcc

# Independently compiled ELF x64 object
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

<!-- i18n-section: profiles -->
## 検証済みプロファイルと制限

| コンパイラ | 成果物 | OS 上で検証した実行プロファイル |
|---|---|---|
| Clang 22.1.6 | LLVM IR | ELF x64 C/C++、Mach-O x64/ARM64 C/C++、PE x64 C/C++ FH3 |
| Clang 22.1.6 | ネイティブオブジェクト | ELF x64 C/C++ |
| GCC/G++ 13.3.0 | ネイティブオブジェクト | 互換性のある Itanium ランタイムを使う ELF x64 C/C++ |
| NeverC 3389.1.5 通常モード | LLVM IR | ELF x64 C、Mach-O x64/ARM64 C |
| NeverC 3389.1.5 通常モード | ネイティブオブジェクト | ELF x64 C |

この表は検証した設定を示し、すべてのコンパイラ版やオプションの互換性を保証しません。`dyncode` と NeverC C++ は未対応です。GCC は LLVM IR を生成できません。ネイティブ COFF/Mach-O オブジェクトは未対応で、オブジェクトのインストールには ELF x64 が必要です。未知の再配置、TLS、コンストラクタ、LTO、common/IFUNC シンボル、未解決のインポートは明確に失敗します。JSON 記録にはコンパイラ、引数、束縛、インストール先を含み、ネイティブオブジェクトには SHA-256 も記録します。

独立した CPU マトリクスは 4 アーキテクチャ（`x86`、`x64`、`ARM32`、`ARM64`）と 3 形式（`ELF`、`PE`、`Mach-O`）のソースインストールまたは明示的な拒否を確認します。隔離した CPU 実行だけでは OS ローダー、C++ アンワインダー、任意の ABI を検証できません。それらには上表の OS 実行プロファイルを参照してください。

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `REFUSED` | `CPU` |
| `PE` | `REFUSED` | `CPU` | `REFUSED` | `REFUSED` |
| `Mach-O` | `REFUSED` | `CPU` | `REFUSED` | `CPU` |

`CPU` はソースのインストールと隔離した関数実行、`REFUSED` はインストール契約不足による明示的拒否を示します。ARM32 ELF には EHABI 登録、32 ビット Mach-O には検証済みのローダー・フロントエンド・アンワインド契約が必要です。PE のソースインストールは現在 x64 に限られます。

CPU マトリクスは独立した算術、分岐、補助関数呼出を使います。その結果は ELF x86/ARM64 のデータ参照や ARM64 C++ のインポートを検証しません。追加の実行検証では、未対応の再配置契約によりインストールが明示的に拒否されました。

<!-- i18n-section: references -->
## 元の参照と記憶領域

束縛は外部宣言に対応します。`function` には確認済みの元関数またはインポートスタブ、`data` には書込み可能領域、`const` には const の IR 宣言が必要です。オブジェクトのデータ束縛には非ゼロの範囲を指定します。領域境界を越えたり RELRO への書込み権限を与えたりできません。置換対象の元本体を呼び戻すための、プロローグを保存するトランポリンはありません。

新しいコードは RX、可変状態は RW に置きます。既存のインポートだけを再利用し、GOT/IAT スロットと呼出可能なスタブを区別します。Mach-O は `-Wl,-headerpad,0x400` などのロードコマンド余白、PE はセクションヘッダー余白が必要です。不足すると失敗します。ELF `.data.rel.ro` と元の CET/IBT 保護には追加の契約が必要なため、現在は拒否します。

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

Mach-O のアンワインド表の拡張には、隣接するファイル領域と仮想領域にも余白が必要です。ヘッダー余白だけでは予約できません。検証用入力は両方の要件を個別に満たします。

<!-- i18n-section: exceptions -->
## C++ 例外

ELF はコンパイラの FDE/LSDA、Mach-O は compact unwind/DWARF を使います。PE x64 はコンパイラの FH3 テーブルを受け入れ、元プログラムに互換性のある MSVC ランタイムと型識別が必要です。type-info データは明示的な束縛を要する場合があります。FH4、独自 personality、SEH ソース、有効な CFG/EHCont/XFG は拒否し、元の保護フラグを保持します。

実行ケースは通常の戻り、内部 catch、新しい例外を元の呼出元で捕捉、元の呼出先から置換関数を通る例外、RAII の一度だけの解放、再送出、noexcept 終了、未変更の関数を検証します。元の呼出箇所は既に例外伝播を許容している必要があります。元コンパイラが非送出関数の周囲から削除した catch はパッチで復元できません。

<!-- i18n-section: publication -->
## SDK と公開

追加型 C API は `neverd_patch_source` と `neverd_source_patch_options_v1` です。`neverd_patch_source_report` は記録を返します。オブジェクト入力ではバイト長を渡します。従来の `neverd_patch_from_c` も使用できます。

入力をスナップショットし、出力先の隣に候補を作り、再ロードしてすべての入口を確認してから排他的に公開します。既存ファイルやシンボリックリンクの別名は上書きしません。一つでも対応が欠ければ全体が失敗します。`--mode=inplace` は拒否します。セッションの IR 変換はネイティブオブジェクトには適用できません。Mach-O は署名を厳密に検証して再署名し、PE の署名を黙って削除しません。

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

<!-- i18n-section: verification -->
## ビルドと検証

`NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` で互換性のあるコンパイラを指定します（Clang 22.1.6 を検証済み）。統合ビルドはビルドディレクトリ内の LLVM コピーだけを修正し、サブモジュールや共有プリビルドキャッシュは変更しません。r3/r4 の x64 PLT 不具合により IR パッチが拒否される場合があります。ネイティブオブジェクトはこの MC 経路を通りません。`NEVERD_TEST_REQUIRE_FIXED_LLVM=1` は修正済みの正確な PLT 生成を必須にします。

`NeverDSourcePatchTests` はネイティブ C/API/オブジェクト/例外を、`NeverDSourcePatchMatrixTests` は Unicorn ですべての組合せを検証します。`NEVERD_TEST_WINDOWS_CRT_LIB`、`NEVERD_TEST_WINDOWS_UCRT_LIB`、`NEVERD_TEST_WINDOWS_SDK_LIB` を設定すると `SourcePatchPE` が 7 つの C++ シナリオを実行します。Windows 以外では `NEVERD_TEST_WINDOWS_RUNNER` も必要です。`NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` は入力不足を失敗とします。専用 Windows ワークフローはネイティブ実行を要求し、スキップを許容しません。

開発 Mac の Linux 検証は Docker と明示的な Linux ローダー、PE 検証は Wine 11 を使います。Mach-O ARM64 は Mac 本体で実行します。Windows ネイティブ CI の結果は別途確認が必要です。ジョブの設定は成功した実行の証拠にはなりません。履歴、ベースラインの失敗、エミュレーターの制限は[詳細契約と検証記録](../source-patching-details.md)を参照してください。

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=ON \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDSourcePatchMatrixTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDSourcePatchMatrixTests)$' \
  --output-on-failure --parallel 4
```

LLVM のビルド用コピーは Mach-O x64 の GOT データ参照も修正します。Clang の型付き catch 表は 4 バイト再配置フィールドの末尾を基準にします。r3/r4 は 4 バイトずれるため、公開前検証で未修正の結果を拒否します。開発 Mac の x64 Mach-O ケースは Rosetta 2、ARM64 ケースは直接実行します。
