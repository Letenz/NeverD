**言語**: [English](../windows-exception-reconstruction.md) | [简体中文](../zh-CN/windows-exception-reconstruction.md) | [繁體中文](../zh-TW/windows-exception-reconstruction.md) | [日本語](windows-exception-reconstruction.md) | [한국어](../ko/windows-exception-reconstruction.md) | [Français](../fr/windows-exception-reconstruction.md) | [Deutsch](../de/windows-exception-reconstruction.md) | [Español](../es/windows-exception-reconstruction.md) | [Italiano](../it/windows-exception-reconstruction.md) | [Русский](../ru/windows-exception-reconstruction.md) | [العربية](../ar/windows-exception-reconstruction.md)

# Windows 例外再構築

[← ドキュメント索引](README.md)

NeverD は Windows のテーブルベース例外情報を、ロード、lift、逆コンパイル、
バイナリ書き換えの全工程で保持します。例外 metadata は関数の実行契約の一部です。
生成コード、runtime-function record、言語テーブル、guard table の整合性を証明できない
書き換えは拒否されます。

この文書では三つのサポートレベルを区別します。

- **解析**：ネイティブ表現を検査済みの正規化 record に decode し、IR pipeline に公開。
- **逆コンパイル**：reducible な protected region を明示的な HighIR 例外 node にする。
  その他は handler や state transition を失わない決定的なネイティブ注釈として保持。
- **ネイティブ再構築**：patch mode が LLVM に完全な代替例外契約を生成させ、最終 PE に
  インストール可能。

解析サポートはネイティブ再構築サポートを意味しません。

## サポート表

| ネイティブ形式 | Lift／解析 | 高水準出力 | Patch mode |
|----------------|------------|------------|------------|
| x64 unwind v1/v2 | 完全に検査された unwind record、operation、chain、handler data、provenance | frame/unwind 要約と、該当する場合は構造化言語 region | 完全な primary record をサポート。生成した `.pdata`/`.xdata` が置換対象 closure を更新 |
| x64 unwind v3/APX | 専用 v3 payload、epilog、operation accounting | 明示的 v3 注釈 | 解析のみ。対象関数の変更を拒否 |
| ARM32/ARM64 packed unwind | function range、packed field、primary/fragment identity | frame/unwind 要約 | 完全な非言語 primary record かつ独立 addressable fragment がない場合のみ |
| ARM32/ARM64 unpacked unwind | 検査済み xdata header/code extent、handler association、fragment | frame/unwind 要約 | 完全な非言語 primary record かつ独立 addressable fragment がない場合のみ |
| `__C_specific_handler` | scope range、filter、finally target、handler、continuation target | reducible region は `__try`/`__except`/`__finally`、それ以外は注釈 | 完全で表現可能な scope graph のネイティブ x64 再構築 |
| `__CxxFrameHandler3` | unwind/try map、catch、catch-object/frame offset、continuation、IP-to-state map | reducible state interval を明示的 C++ HighIR と C 互換型注釈に変換 | 後述する狭い verifier-clean subset のネイティブ x64 再構築 |
| `__CxxFrameHandler4` | action kind と object offset を含む bounded variable-length decode | FH4 provenance を持つ共通 HighIR graph | 解析のみ。対象関数の変更を拒否 |
| `__GSHandlerCheck_SEH/EH/EH4` | wrapped personality と検査済み GS cookie provenance | base language graph と wrapper 注釈 | 解析のみ。downgrade せず対象関数の変更を拒否 |
| x86 registration-chain EH | 検証済み SEH チェーンと C++ グラフ | callback の識別を保持する HighIR 領域 | 証明済み部分集合の PE32 再構築。詳細な契約を参照 |

スカラー参照の `catch` 内にある `try` は、生存中の外側の例外オブジェクトと専用スタックの復元を保持できます。共有 callback と一般的なオブジェクト寿命は、まだネイティブ再構築の対象外です。 [PE32 C++](../windows-exception-reconstruction.md).

Malformed record を完全な通常 record として扱うことはありません。partial decode は調査に
使えますが、ネイティブ metadata 生成を許可しません。ARM xdata header から bounded な
executable fragment range を証明できても後続 unwind body が壊れている場合、range は
disassembly に残りますが record は malformed となり、patchable function にはなりません。

## 正規化モデル

`ExceptionInfo` は `BinaryImage` が所有します。各 `ExceptionFunction` は次を持ちます。

- 検査済み half-open code range。
- primary、chained、fragment の identity。
- native unwind encoding と正確な runtime/unwind provenance。
- 正規化 unwind operation/epilog。未解釈 operation の opaque operand bytes も保持。
- 正確な personality identity と handler data。
- 任意の SEH scope、C++ state map、GS cookie data。
- `Complete`、`Partial`、`Malformed` status と決定的 diagnostic。

loader はこのモデルから raw file pointer を公開しません。native RVA は診断と patch
replacement 用に保持し、IR consumer は検証済み VA/range だけを使用します。

image-wide index は chained/fragment record の overlap を許し、address を覆う最も具体的な
function を返します。破損 directory、range、pointer、count、state transition、compressed
integer、chain cycle、decode-budget exhaustion は該当 parse status を低下させます。

language-table limit は個別テーブルと関数全体の正規化 graph の両方に適用します。同じ
handler map を多数の try-map entry が再利用しても総予算を超えません。同じ `FuncInfo` と
personality を共有する FH3 record は bounded function group として decode され、親の
IP-to-state map は自身の catch funclet を参照できますが、無関係な runtime function の
address は受け入れません。

狭い整数引数は、証明済みの ECX/EDX の 8、16、32 ビット幅を保持し、物理スタック枠は
32 ビットのままです。符号拡張とゼロ拡張は元の命令に従います。FS 公開前のロードと
ローカル保存は ESP、EBP、登録フィールドを変更してはいけません。

生成済み PE32 C++ エントリは、完全なソース証明に成功すれば再ロード、再リフト、再構築できます。
loader は実際の FS:[0] 書き込み、整列フレーム、SafeSEH ポインターの役割をバイト列から検証します。
インストーラーは以前生成した実行可能セクション内の認証済みエントリも変更できますが、仮想範囲と
ファイル上の格納範囲が一意で、解析時のバイト列と一致する必要があります。cdecl/stdcall/thiscall/fastcall
の検体は Microsoft x86 CRT で二世代の再構築、両 patch モード、強制再配置を検証します。
各世代で LowIR と LLVM を新たに証明し、古い証明記録を許可として再利用しません。
生成された ESI cleanup relay も、ローカルフレームの基点と保存した入口 EBP が認証済みの
親と一致すれば二度目の再構築に対応します。キャッシュしたバイト列の証明は呼び出しごとに
親フレームへ結び直し、LowIR がオブジェクトの借用と保存フレームの寿命を証明します。
実際の MSVC 値／参照サンプルで、二世代、両 patch モード、強制再配置にわたりデストラクタの
順序と参照書き戻しを検証します。一般的なオブジェクト寿命と型変換は未対応です。

PE32 の自明にコピーできる例外オブジェクトは、値・参照による捕捉、直接 throw、
再 throw にも対応します。不変の ThrowInfo は型を一つだけ含み、アドレス調整、コピー・
転送コールバックを持たない必要があります。全バイトの初期化と私有
フレームポインタの非流出を証明します。O0/O1 の 8・12 バイト構造体でフィールド更新、
catch 内分岐、両 CLI モード、Microsoft CRT の再配置を検証します。PHI コピーは元の
CFG 辺とコールバックに所属し、独立検証は追加呼び出し、別名メモリ、遷移先変更を拒否します。
RTTI はメンバー配置を示さないため、C++ 構文検査にはサンプルの実際の型宣言を使い、
出力はネイティブオフセットと未知の集約型の throw 呼び出しを保持します。

例外オブジェクトのデストラクタには独立した LowIR 証明が必要です。戻り可能な
リーフ関数であり、私有スタックを持ち、ECX 経由のアクセスがオブジェクト内に収まる
必要があります。コードとメモリ効果も保存する呼び出し契約に含めます。O0/O1 テストは
破棄の回数・順序・オブジェクト内容を、再 throw と二世代の再構築で検査します。

非自明な実行時コピーコンストラクタも、独立した LowIR 証明により対応します。
ECX のコピー先とスタック引数のコピー元を別のアドレス領域として追跡し、コピー元を
読み取り専用に保ち、境界内アクセス、全戻り経路でのコピー先全体の初期化、`RET 4`
を要求します。ポインタの流出、CRT 呼び出し元アドレスの観測、未証明の呼び出しを拒否します。
O0/O1 テストは各実行の四回のコピーと十二回の破棄を二世代で検査します。不明な集約型は
元の throw 呼び出しと初期化を保持し、RTTI から `throw Type()` を推測しません。
一般的な構築呼び出しグラフ、型変換、仮想基底のコピー、一般的な破棄呼び出しは未対応です。

## IR 契約

例外 metadata は通常 CFG の意味を変えずに全表現へ渡されます。

- LowIR は protected-range endpoint、state transition、filter、handler、cleanup action、
  continuation target で block を分割。
- exceptional successor/predecessor は通常のものと分離し、dominator/structuring pass が
  runtime dispatch edge を machine branch と誤認しないようにする。
- MedIR は正規化 function descriptor と安定した exceptional edge を保持。
- HighIR は別々の `SEHTry`/`CxxTry` statement を使用。clause descriptor は native
  target VA、type descriptor、adjective、catch-object/parent-frame offset、cleanup
  action kind/object offset、state、continuation VA を保持。

HighIR structurer は interval-conservative です。完全な protected range に全 address が
含まれる連続 statement slice だけを動かし、nested region は内側から処理します。crossing
region、partial graph、address-less ambiguous boundary、out-of-line funclet は元の control
flow に残り、function の unstructured-EH count を増やします。

C backend は reducible な single-clause SEH region に MSVC SEH syntax を出力します。
HighC は C backend なので C++ catch/cleanup state は決定的な C-compatible comment とし、
compilable C++ を装いません。out-of-line native funclet は正確な address を保持します。

## LLVM metadata schema

emitted function に関連する解析済み例外 function は、native WinEH lowering 非対象でも
lossless LLVM metadata を受け取ります。

- function attachment：`neverd.windows.eh`
- native-lowering marker：`neverd.windows.eh.native`
- module table：`neverd.windows.eh.functions`
- current schema version：`3`

fixed function record は parse status、encoding、code range、native runtime/unwind RVA、
runtime-record kind/chain provenance、packed-unwind word、frame description、canonical/
resolved personality name、handler data、native unwind bytes、operation（native slot count
を含む）/epilog、SEH scope、C++ header/map、GS data、diagnostic、regeneration flag を保持。
patch validation は正確な schema version と loaded image との完全な range match を要求し、
例外契約を持つ auto-named lifted function は attachment を省略できません。

native x64 SEH lowering は LLVM WinEH を使用し、scope graph 全体が表現可能な場合だけ
verifier-clean `invoke`/funclet control flow を生成します。native FH3 lowering の条件は：

- x64 COFF、unwind v1/v2、complete metadata、有効な synchronous FH3 state graph。
- `noexcept`、asynchronous、separated-funclet、GS-wrapper、FH4、unknown flag semantics なし。
- protected interval は nested または disjoint で、crossing しない。
- destructor/unwind action、catch-object construction、parent-frame dependency なし。
- handler は lifted function 内の predecessor-free、call-free な通常 block。
- unwind し得る protected operation はすべて LLVM `invoke` で表現。

一条件でも偽なら LLVM は解析可能なまま lossless metadata を保持しますが、patch planning
は native language-table replacement を拒否します。PE entry point、TLS callback、CRT
callback root は preservation boundary であり、通常の ABI rewrite candidate ではありません。

## Patch transaction

サポートする rewrite は一つの PE transaction として処理します。

1. loaded exception graph と LLVM metadata attachment に対して各 touched function を検証。
2. section identity/alignment/allocation、code/data trait、semantic symbol-index reference を
   保持して replacement code を compile。local Windows personality は codegen 前に
   externalize し、emitted xdata を実証済み original executable handler に binding。
3. untouched runtime-function entry を保持し、各 touched primary function が置換する
   native closure（関連 chained record を含む）を削除。
4. generated code/xdata を relocate、generated/retained pdata を merge、begin RVA で sort、
   overlap を拒否。同じ personality class の generated runtime-function record が全
   redirected language-EH entry を覆うことを証明し、単一 replacement exception directory
   を install。
5. input CFG instrumentation mode を保持し、`.gfids` semantic reference と redirected entry
   を Guard CF table に merge。`.gehcont` reference を generated executable VA に解決して
   Guard EH continuation table に merge し、guard flag を保ったまま load-config を更新。
   unresolved helper は transaction を中止。CFW、return-flow guard、retpoline、XFG は
   異なる codegen contract が必要なため analysis-only。
6. disk へ書く前に完成 byte image を再 parse。

LLVM fork extension は汎用のままです。final-image writer が section trait と semantic
symbol-index reference を保持し、PE/MSVC decode、policy、directory merge、load-config
update、final validation は NeverD に残ります。

original Guard CF/EH continuation entry は original entry trampoline が有効な indirect
target のため保持されます。generated target は emitted code 内を指し、結果 table は
厳密な RVA sort を満たす必要があります。

## 最終 image validation

次の全条件を満たさない patched PE は拒否します。

- LLVM が bytes を COFF object として受理し、PE machine/class/section table、optional
  header directory bounds、image base/extent が一致。
- section raw/virtual extent が範囲内で、section range に overlap がない。
- exception directory が file-backed で image 内。
- runtime-function entry が sorted、nonempty、non-overlapping、fully executable。
- x64 unwind RVA/header/code array/version/flag/handler target が有効で、chain は bounded/acyclic。
- final import/export/COFF symbol を memory 内で再構築し、既知 SEH/FH3 personality と
  scope/state table を完成 bytes から再 parse。
- ARM runtime entry/xdata の version/range が有効かつ対応済み。
- guard flag が table を示す場合 load-config に Guard CF/EH continuation field が存在。
- guard pointer/count/stride が PE image と file の両方の範囲内で、entry は strict-sorted
  executable target。

失敗すると patch operation は中止され、best-effort image は出力しません。

## 対象検証

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

guarded x64 fixture は `/guard:cf` と `/guard:ehcont` で cross-assemble/link されます。統合
test は SEH scope/guard table、structured HighC、patched image の reload、table count/order/
executable target を検証します。

別の linked x64 FH3 fixture は同じ transaction で supported C++ closure を検査し、original
fixed table、HighC state annotation、personality binding、再構築 try/catch graph、reload 後
IP-to-state map を検証します。parser 変更時は共通モデルを使う ARM format case も実行します。

## ネイティブサポートの拡張

新しいネイティブ再構築対応には、同じ変更内で次が必要です。

- complete bounded parser と normalized-model invariant。
- HighIR/LLVM metadata round-trip coverage。
- 新たに受理する全 graph shape の verifier-clean native IR。
- 必要な emitted-section/semantic-reference retention。
- 正確な architecture/personality/version の linked PE fixture。
- exception-directory/load-config/final-image structural validation。
- 最も近い unsupported shape の explicit rejection test。

decode できるだけで allow-list を広げてはいけません。最終 linked image で runtime exception
behavior が保存されることが受理条件です。

選択された handler の cleanup は、親フレーム内の同じ capture または別の有効な capture に新しいスカラー値でジャンプできます（0 は 1 に正規化）。各宛先と値をフレーム効果と組にし、実際の宛先の保存レジスタ、ESP、unwind 境界を復元します。中断中の入れ子 finally 呼び出しも終了します。MedIR は置換を正確な callback 呼び出し位置と capture に結び付け、非循環の転送グラフですべての経路の完結を要求します。残りの cleanup 全体で両バッファの有効性を検証し、重複書き込み、失効したバッファ、未証明の循環、フレームアドレスの流出を拒否します。

中断中の finally 内の RaiseException が呼ぶ検索 filter も、同じ capture や別の有効な capture へジャンプし、別の分岐では戻ることができます。戻りとジャンプのフレーム効果は分離され、ジャンプは filter と中断中の入れ子 finally をすべて終了し、固有の宛先と値を維持します。

finally コールバックと検索フィルターの有限ループは、レジスターのカウンター、親フレームのセル、専用スタックを含む完全なフレームを実行段階ごとに保持します。完全状態の再帰、終了の証明不足、予算超過は拒否します。PHI コピーのブロックは正確なソース辺と同じコールバックに属する必要があり、インストーラーは復元したスカラー領域の escape インデックスと、すべての別名が専用の非アトミックアクセスだけに使われることを検査します。

検証済みの固定フレーム SEH3/EH4 finally は SavedESP を復元し、親関数に属することが
確認された継続先へ直接分岐できます。生成処理は非公開 SjLj コンテキストで生成コードの
スタックと FS:[0] を復元し、現在の論理レジスタと callback のフレーム書き込みを保持して、
正確なソース scope 境界へ進みます。インストーラは別名、保存、転送、分岐を独立検証し、
ソースのバッファによるコンテキストの置換を拒否します。実行時の脱出が CRT の一時的な
アンワインドノードを迂回する場合、次の呼び出しや例外を起こし得る操作の前にソース側で
生存中の registration を再接続するか、return 時に除去する必要があります。チェーン復元を
証明できない場合や、対象が検証済みの親 CFG に属さない場合は再構築を拒否します。

継続先は finally からしか到達できない場合も扱えます。ただし、非循環の末尾にある
すべてのデコード済み経路が、filter や finally の入口を横切らず、既知の親コードへ
合流する必要があります。この物理的な所属証明は実行時 handler のメモリアクセス権限を
与えません。SavedESP の転送と到達状態は別途検証します。既知の親コードに接続しない
return や循環から、親への所属を導くことはできません。


通常の finally 呼び出しは、親フレームの非公開スロットを介して
EAX/ECX/EDX/EBX/ESI/EDI を明示的に受け渡し、物理的な near return 時の値も反映します。
ランタイム入口は固有のレジスタ契約を維持し、以前の通常呼び出しの入力を流用しません。
検証済みの通常 finally 呼び出しだけが参照する scope も、ソースの try level が
有効化しなくても保持します。

実際の MSVC x86 `/Od` と `/O2` の `nonlocal_finally_return` は `/GS-` と `/GS`、最適化前後のソース LLVM、両パッチ方式、二つの固定ベースで検証されます。外側の SEH 観測器は通常・例外戻り、スタック保護とチェーン復元を確認します。GS では呼び出し元のフレームを観察しないスカラーヘルパーを証明し、元の例外コード正規化ラッパーを保持します。

`/O2` の親関数への直接 return には、隣接する物理 call、保存 ESP の復元、元のチェーン解除、完全な親エピローグの検証が必要です。LowIR がソースの効果を証明し、MedIR が残存する操作を結び付け、COFF インストーラーが契約を独立に再検証します。未使用の合成レジスタ受け渡しは親の物理引数を増やさず、参照される値の依存関係は維持します。検証済みの MSVC の非局所 `goto` 形式には、以下の完全展開契約を使用します。

検証済みの `_local_unwind2` と `_local_unwind4` 呼び出しは、正確なソース操作、認証済みインポートスロット、実際の親フレーム引数、符号なしの展開境界、復帰時の状態対を保持します。EH4 では cookie アドレスとフレーム内 cookie の初期化も証明します。インポート領域の書き換え、条件付き呼び出し、ソース識別情報の不一致があれば証明を取り消します。MedIR はスタック上の引数を正確に二つまたは三つ復元し、偶然生存しているレジスタを引数にしません。ネイティブ置換は、物理親フレームへのブリッジ、SavedESP の復元、ソース操作を独立に検証した後、EH3 `-1` または EH4 `-2` までの完全展開をサポートします。実際の MSVC `nonlocal_finally_goto` は `/Od`、`/O2`、`/GS-`、`/GS`、最適化前後の LLVM、両方のパッチモード、二つの固定ロードベースで実行検証します。部分的な展開境界と有効な外側 scope 内への復帰には、引き続き追加のネイティブ契約が必要です。
