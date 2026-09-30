**语言**：[English](../solver.md) | [简体中文](solver.md) | [繁體中文](../zh-TW/solver.md) | [日本語](../ja/solver.md) | [한국어](../ko/solver.md) | [Français](../fr/solver.md) | [Deutsch](../de/solver.md) | [Español](../es/solver.md) | [Italiano](../it/solver.md) | [Русский](../ru/solver.md) | [العربية](../ar/solver.md)

[← 文档索引](README.md)

# Bitvector 证明后端

NeverD 默认使用内置 bitvector solver。精确 MBA 推导独立于通用 solver。表达式合成只有在证明等价后才接受候选；反例或未决查询都会保留原表达式。

<!-- i18n-section: z3-build -->

## 可选 Z3 构建

启用 Z3 后，CMake 通过 `FetchContent` 下载固定的 4.13.3 源码版本，并与 NeverD 一起构建静态库；不需要系统安装 Z3：

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_Z3=ON -DNEVERD_BUILD_SOLVER_BENCH=ON
cmake --build build-release --target neverd NeverDSolverTests \
  NeverDSymbolicTests neverd-solver-bench --parallel 4
```

默认 provider 为 `NEVERD_Z3_PROVIDER=FETCH`。首次配置会下载源码，后续构建复用 build 目录的 `_deps`。不会构建 Z3 CLI、测试、示例、文档或语言绑定；Z3 Python 生成器使用 NeverD 已依赖的 Python interpreter。使用已安装库时选择 `-DNEVERD_Z3_PROVIDER=SYSTEM`，也可指定 `-DZ3_ROOT=/path/to/prefix`。缺少开发文件会失败，不会静默切换 provider。离线构建可用 `-DFETCHCONTENT_SOURCE_DIR_NEVERD_Z3=/path/to/z3` 指定本地源码。

默认 `NEVERD_ENABLE_Z3=OFF` 时，NeverD 不下载、不搜索也不链接 Z3。运行时显式请求不可用后端会失败，不会回退。

<!-- i18n-section: synthesis -->

## 证明门控的表达式合成

```sh
build-release/bin/neverd simplify --synthesize --solver=z3 \
  --solver-timeout-ms=1000 --json '(x >> 4) + ((x >> 2) >> 2)'
```

默认值为 `--solver=builtin`。选择 solver 必须同时使用 `--synthesize`；普通 MBA 简化器不会调用 Z3。Z3 timeout 按每次检查计时，不包括表达式翻译。取消是协作式的，不是硬性墙钟期限。0 使用默认 1000 ms；`--exhaustive` 移除该限制。SAT 冲突、传播和 watch 访问限制仅适用于内置后端，与 Z3 同用会被拒绝。Z3 检查会增加 proof-query 计数；内置 SAT 工作计数保持为 0。

C API 在 `neverd_synthesize_options` 中追加 `solver_backend` 和 `solver_timeout_ms`。有大小边界的读取器仍为旧调用方选择内置后端。`neverd_solver_backend_available()` 报告构建能力；Python 也可通过 `synthesize_expression(..., solver='z3', solver_timeout_ms=1000)` 选择。当前仅覆盖表达式合成；concolic 执行、安全分析及既有 IR 优化默认仍沿用原 solver 策略。内部用户可通过语义简化器的 proof callback 提供 Z3 verifier。

```python
from neverd_plugin import synthesize_expression

result = synthesize_expression(
    '(x >> 4) + ((x >> 2) >> 2)', solver='z3', solver_timeout_ms=1000
)
```

<!-- i18n-section: checks -->

## 独立检查与查询导出

启用 Z3 时，`NeverDSolverTests` 包含独立表达式求值和跨后端检查。参考表达式会直接构造，避免 NeverD 表达式 builder 的错误把测试两侧一起简化。禁用 Z3 的构建会明确跳过 oracle 用例，但仍测试后端不可用契约。

```sh
build-release/bin/NeverDSolverTests --gtest_brief=1
build-release/bin/neverd-solver-bench \
  tools/neverd-bench/solver-corpus.txt --width=32 --repeat=5 \
  --backend=both --timeout-ms=1000 --max-conflicts=10000 \
  --dump-dir=/tmp/neverd-queries > /tmp/neverd-solver-results.json
```

每条非注释行格式为 `original ; candidate`；没有分号时，以 MBA 简化器的结果作为候选。工具报告判定、模型重放及耗时；计时包括 session 创建、翻译和求解，不包括解析、MBA 简化、导出和销毁。每次重复使用全新 solver。SAT 模型必须在表达式求值器中重现差异。相互矛盾的确定判定、无效查询或模型会使运行失败；`unknown` 会记录，但不构成等价证明。

SMT-LIB 导出包含原 DAG、永久断言和最后一次查询的假设，可用 `z3 query-N.smt2` 重放。应记录资源限制和 solver 版本。两个后端的预算单位不同，因此比较的是有界工作负载，不是相同工作量。Bitvector 证明采用表达式语言的 total fixed-width 语义。机器异常、内存效果和 LLVM poison 仍由提升／翻译边界负责，表达式证明并不认证这些行为。
