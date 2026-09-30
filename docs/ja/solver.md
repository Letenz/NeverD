**言語**: [English](../solver.md) | [简体中文](../zh-CN/solver.md) | [繁體中文](../zh-TW/solver.md) | [日本語](solver.md) | [한국어](../ko/solver.md) | [Français](../fr/solver.md) | [Deutsch](../de/solver.md) | [Español](../es/solver.md) | [Italiano](../it/solver.md) | [Русский](../ru/solver.md) | [العربية](../ar/solver.md)

[← ドキュメント索引](README.md)

# Bitvector 証明バックエンド

NeverD は既定で組み込み bitvector solver を使います。厳密な MBA 導出は汎用 solver に依存しません。式合成では等価性を証明した候補だけを採用し、反例や未確定の問い合わせでは元の式を保持します。

## オプションの Z3 ビルド

Z3 を有効にすると、CMake は `FetchContent` で固定された 4.13.3 のソース revision を取得し、NeverD と静的ライブラリをビルドします。システムへの Z3 インストールは不要です。

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_Z3=ON -DNEVERD_BUILD_SOLVER_BENCH=ON
cmake --build build-release --target neverd NeverDSolverTests \
  NeverDSymbolicTests neverd-solver-bench --parallel 4
```

既定の `NEVERD_Z3_PROVIDER=FETCH` は初回 configure でソースを取得し、以降は build の `_deps` を再利用します。Z3 の CLI、テスト、例、ドキュメント、language binding はビルドしません。Z3 の Python generator は NeverD が既に必要とする Python を使います。インストール済みライブラリを使う場合は `-DNEVERD_Z3_PROVIDER=SYSTEM`、必要なら `-DZ3_ROOT=...` を指定します。開発ファイルがなければ失敗し、provider を暗黙に切り替えません。オフラインでは `-DFETCHCONTENT_SOURCE_DIR_NEVERD_Z3=...` にローカル checkout を指定できます。

既定の `NEVERD_ENABLE_Z3=OFF` では Z3 を取得・検索・リンクしません。利用不能 backend を明示的に要求すると、fallback せず失敗します。

## 証明ゲート付き式合成

```sh
build-release/bin/neverd simplify --synthesize --solver=z3 \
  --solver-timeout-ms=1000 --json '(x >> 4) + ((x >> 2) >> 2)'
```

既定は `--solver=builtin` です。solver 選択には `--synthesize` が必要で、通常の MBA simplifier は Z3 を呼びません。Z3 timeout は式翻訳を除く各検査に適用されます。キャンセルは協調的であり、厳密な wall-clock deadline ではありません。0 は既定の 1000 ms を選び、`--exhaustive` は制限を外します。SAT conflict/propagation/watch visit 制限は組み込み backend 専用で、Z3 使用時には拒否されます。Z3 検査は proof-query counter を増やし、組み込み SAT work counter は 0 のままです。

C API は `neverd_synthesize_options` に `solver_backend` と `solver_timeout_ms` を追加します。サイズ境界付き reader は古い caller のため組み込み backend を維持します。`neverd_solver_backend_available()` は build capability を返し、Python でも `synthesize_expression(..., solver='z3', solver_timeout_ms=1000)` を選べます。現時点で対象となるのは式合成だけです。concolic 実行、安全性解析、既存 IR 最適化の既定 solver policy は変更されません。内部利用者は意味論 simplifier の proof callback に Z3 verifier を渡せます。

## 独立検査と query export

Z3 有効時の `NeverDSolverTests` は独立した式評価と backend 間検査を含みます。参照式を直接構築するため、NeverD の式 builder の bug がテスト両辺を同時に簡約することはありません。Z3 無効 build は oracle ケースを明示的に skip しつつ、backend unavailable 契約を検査します。

```sh
build-release/bin/NeverDSolverTests --gtest_brief=1
build-release/bin/neverd-solver-bench tools/neverd-bench/solver-corpus.txt \
  --width=32 --repeat=5 --backend=both --timeout-ms=1000 \
  --max-conflicts=10000 --dump-dir=/tmp/neverd-queries
```

コメント以外の各行は `original ; candidate` です。セミコロンがなければ MBA simplifier の結果を候補として使います。ツールは判定、model replay、時間を報告します（session 構築・翻訳・solver 実行を含み、parse、MBA 簡約、export、teardown は除外）。各反復で新しい solver を使います。SAT model は式 evaluator 上で差分を再現する必要があります。相反する確定判定、不正 query/model は実行失敗です。`unknown` は記録されますが、等価性の証明ではありません。

SMT-LIB export には元の DAG、恒久 assertion、最後の query assumptions が含まれ、`z3 query-N.smt2` で再実行できます。resource limit と solver version を記録してください。backend の budget 単位は異なるため、同一作業量ではなく上限付き workload の比較です。bitvector proof は式言語の total fixed-width semantics を使います。machine exception、memory effect、LLVM poison は lifting/translation boundary の責任であり、式の証明だけでは保証しません。
