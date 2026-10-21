**语言**: [English](../../CONTRIBUTING.md) | [简体中文](CONTRIBUTING.md) | [繁體中文](../zh-TW/CONTRIBUTING.md) | [日本語](../ja/CONTRIBUTING.md) | [한국어](../ko/CONTRIBUTING.md) | [Français](../fr/CONTRIBUTING.md) | [Deutsch](../de/CONTRIBUTING.md) | [Español](../es/CONTRIBUTING.md) | [Italiano](../it/CONTRIBUTING.md) | [Русский](../ru/CONTRIBUTING.md) | [العربية](../ar/CONTRIBUTING.md)

# 为 NeverD 做贡献

NeverD 是一个语义优先的二进制分析项目。有价值的贡献应当目标集中、
让不支持的行为明确失败，并包含能够证明变更契约的最小测试。

引入现有代码时，须保留版权、许可证和来源声明，包括借助 AI/LLM 工具复制或改编的代码。
请遵循[署名与引用指南](ATTRIBUTION.md)，注明复用内容的原始来源和版本，
并在提交前检查其许可证。第三方声明应按其原有许可证保留。

开始修改前，请阅读[架构指南](architecture.md)。测试套件的选择请参考
[测试指南](testing.md)，产品规划请参考
[路线图](roadmap.md)。

## 前置条件

- 支持递归子模块的 Git
- CMake 3.20 或更高版本
- Ninja
- 支持 C++20 的编译器
- 用于完整跨目标 fixture 集的 Clang 和 LLD（`ld.lld` 与 `lld-link`）

递归子模块提供 NeverD 的 LLVM fork、Capstone fork、Unicorn 和签名数据。
验证变更时，请勿将它们替换为任意的系统版本。

## 克隆并初始化

开发成果合入 `dev`，它也是仓库的默认分支。克隆时获取全部子模块：

```bash
git clone --branch dev --recurse-submodules \
  https://github.com/NeverSight/NeverD.git
cd NeverD
```

对于已有的克隆，请在首次构建前，以及任何修改子模块记录版本的提交之后，
同步并初始化子模块：

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

## 选择构建配置

| 配置 | 用途 | 重要行为 |
|------|------|----------|
| Release | 常规开发、完整测试、解码/提升基准测试 | 已优化；吞吐量具有代表性 |
| RelWithDebInfo | 分析或调试已优化的热点路径 | 已优化并包含调试符号 |
| Debug | 断言、源码级单步调试、局部正确性工作 | 未优化；解码基准会明显变慢 |

除非任务明确需要 Debug 行为，否则使用 Release：

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel
```

默认情况下，构建会将 `third_party/llvm-project` 作为集成依赖一并编译。
首次构建通常需要 30–60 分钟，之后的构建是增量的。`CMakePresets.json`
还定义了 `release`、`relwithdebinfo` 和 `debug` 配置/构建预设；上面使用显式
构建目录，是为了让测试开关清晰可见。

进行源码级调试时，请使用独立目录，不要重新配置 Release 构建树：

```bash
cmake -S . -B build-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
cmake --build build-debug --parallel
```

绝不要使用 Debug 构建报告解码或提升吞吐量。基准测试使用 Release；需要
调试符号进行性能分析时使用 RelWithDebInfo。

### macOS 上的预编译 LLVM

Apple Silicon 贡献者可以避免在本地编译 LLVM fork：

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
cmake --build build-release --parallel
```

CMake 会下载仓库配置的发布包，校验 SHA-256，并在后续构建中复用已解压的
用户缓存。预编译通道仅支持 macOS arm64；Intel Mac 和通用二进制构建必须
使用默认的本地 LLVM 构建。`NEVERD_LLVM_PREBUILT_TAG`、镜像 URL、缓存目录
和显式校验和等高级覆盖项记录在 `cmake/NeverDLLVMPrebuilt.cmake` 中。

## 分支与拉取请求流程

从最新的 `dev` 开始，并创建目标集中的主题分支：

```bash
git switch dev
git pull --ff-only origin dev
git switch -c docs/contributor-guide
```

拉取请求应面向 `dev`，不要假定某个发布分支。保持提交便于审查：每次提交只做
一件连贯的事，不包含生成的构建输出和无关格式化；除非提案本身需要，否则不要
修改子模块版本。

## 代码风格

C 和 C++ 遵循 [LLVM Coding Standards](https://llvm.org/docs/CodingStandards.html)，
以 [`.clang-format`](../../.clang-format) 作为仓库的格式权威，
[`.editorconfig`](../../.editorconfig) 负责编码、换行和空白约定。
C++ 语言版本以 NeverD 的 `CMakeLists.txt` 为准，不直接沿用上游风格文档的版本。
只格式化你修改的文件：

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

CI 的格式化工具版本固定在
[`scripts/check_clang_format.py`](../../scripts/check_clang_format.py) 中；
复现 CI 结果时使用相同版本。格式检查不会判断头文件归属，也不会识别策略裸数字。

不要为一个局部修复执行全仓库格式化。遵循周边文件的命名和拆分方式，将平台特定
行为保留在对应的 loader、lifter 或 backend 边界内，并避免通过纯 C SDK 暴露
内部 C++ 类型。

### 源文件与头文件布局

- 新建 C++ 源文件和头文件使用 LLVM 风格的“文件名/用途”横幅、`NeverD Decompiler`
  和 Doxygen `\file` 说明，可参照 `lib/debug/PDBLoader.cpp`。保留已有版权和许可声明。
- 自包含头文件使用 `.h`，显式包含所需依赖；实现文件首先包含对应的接口头文件。
  其余 include 由 `.clang-format` 排序：同目录私有头、`neverd/` 公共头、
  引号形式的 LLVM/Capstone 头，最后是尖括号形式的头。不得依赖偶然的包含顺序
  或传递包含来补足依赖。
- 新头文件统一使用由路径推导的 `NEVERD_..._H` include guard。例如
  `neverd/sdk/Limits.h` 对应 `NEVERD_SDK_LIMITS_H`；私有和工具头需包含足够的
  目录信息，避免重名。末尾使用带 guard 名的 `#endif` 注释。明确整理旧头文件
  时采用同一布局，不顺带改动无关头文件。
- 共享库接口放在 `include/neverd/<component>/`，实现私有头放在所属的 `lib/`
  或 `tools/` 源文件旁。放进 include 目录不等于成为稳定的公共 C SDK API。
  保持依赖无环，核心模型和 reader 不依赖 session 状态或传输适配层；模块算法由
  所属目录负责，CLI、SDK、worker 不另行复制语义判断。参见
  [架构文档](architecture.md)。
- 使用所属模块的命名空间和有意义的名称。新 C++ 类型和字段使用 `UpperCamelCase`，
  函数使用 `lowerCamelCase`，命名策略常量沿用 NeverD 的 `kUpperCamelCase`。
  扩展既有接口时保持周边命名方式，并保留公共 C/Python 合同和既有 STL 风格接口的
  拼写。头文件不得加入命名空间级的 `using namespace` 指令。
- 有意多次展开的表沿用 `.def`/`.inc` 约定，并说明宏合同；它们不是独立 `.h`
  接口，不添加会阻止重复展开的 guard。生成头文件应修改其生成器。

独立策略头文件的完整模板见[英文规范](../../CONTRIBUTING.md#file-and-header-layout)。

### Limits、默认值与配置归属

每个策略值必须在所属模块的 limits/configuration 头文件中有唯一权威定义，
即使当前只有一个调用点。范围包括输入/输出上限、递归和图遍历边界、工作量和
迭代预算、启发式阈值、缓存保留、分页/批次、重试和超时。规则同时适用于命名
`constexpr`、宏、裸数字比较、`min`/`clamp` 参数、构造/字段初始化和默认参数。

新增文件前先选择已有归属：

| 策略归属 | 定义位置 |
| --- | --- |
| 共享 core/pipeline | [`include/neverd/Limits.h`](../../include/neverd/Limits.h) |
| SDK | [`include/neverd/sdk/Limits.h`](../../include/neverd/sdk/Limits.h) |
| Web | [`include/neverd/web/Limits.h`](../../include/neverd/web/Limits.h) 或已有专门的 limits/configuration 定义 |
| 其他库模块 | 已有策略头或定义表；没有时在所属层新增职责明确的 `Limits.h` |
| CLI、worker、GUI 自有策略 | 工具内的 `Limits.h` 或 `Config.h`；core 不得反向依赖工具头 |
| 调用方可选设置 | 所属的 `Config.h`、`*Options.h` 或 `*Configuration.h` 模型 |

策略头保持轻量，按用途分组。已有 `*Limits.def` 或配置表负责某组值时复用它，
并通过自包含头暴露 C++ 视图。不要在 `Limits.h`、`Constants.h`、`Config.h` 中
分别定义同一策略。共享 core 策略不放进 SDK/工具头，也不将无关模块的策略全部
塞进根 `Limits.h`。

C++ 标量策略常量使用带明确类型的 `inline constexpr`，保留所属模块既有命名空间，
例如 `neverd::limits`、`neverd::sdk`。名称表达用途和单位，例如 `kMaxSourceBytes`、
`kDefaultPageRows`、`kRetryDelayMilliseconds`；注明零值/哨兵语义和特殊上限的依据。
默认值和硬上限含义不同时分别命名。Options 模型可直接拥有字段默认值，调用方应
使用该模型、默认值工厂或同一命名常量；有关联的默认值和上限应从共同定义推导，
不要重复填写裸数字。

适配层、持久化校验、CLI/GUI/worker 和 Python/MCP 绑定复用对应的权威定义；
schema 上限和包含限值的诊断信息也从同一值派生。
跨语言合同值从既有定义表或生成绑定派生；不适合生成时加入漂移检查，并保持纯 C
头文件的兼容性。前端预览/分页限制可以有意比引擎上限更严格，但需独立命名并明确
归属。不能只因数值相同就合并两个策略。

二进制布局、ABI/协议/指令常量、整数可表示范围，以及绑定具体证明算法的计费项，
保留在其语义模型中。状态初始化、索引运算、分配提示和独立测试输入不自动视为
策略。测试用于验证边界时，不得将独立的预期值改成它要验证的实现常量。

审查时同时检查定义和裸数字使用，包括跨行保护、循环、宏、默认值、计时器和绑定；
逐一核实新增或改动策略的归属和含义，只搜索 `Max*` 不够。集中定义时保留类型、
数值、比较方向、哨兵含义及预算耗尽/错误行为。策略调整与整理分开处理，并按
[测试指南](testing.md)运行受影响检查。

### 文档与本机产物

Markdown 应简洁且可从源码核实。仓库内部文件使用相对链接；当 CLI 行为、公共
API、支持声明、构建选项或测试命令发生变化时，在同一个拉取请求中更新文档。

## 运行测试

通过聚合目标运行所有已注册测试：

```bash
cmake --build build-release --target check-neverd
```

开发期间使用最小的相关目标或 CTest 标签：

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

[测试指南](testing.md)记录了全部便捷目标、仅标签的变换套件、单测试
正则表达式、fixture 编译和 Unicorn 往返。如果某个目标因缺少交叉编译器或
链接器而跳过，请报告这一限制；不要把未执行的路径描述为通过。

## 拉取请求检查清单

请求审查前：

- 按维护者偏好的流程 rebase 或 merge 最新 `dev`，并有意处理子模块变更。
- 以 Release 构建受影响目标；如需其他配置，请说明原因。
- 运行精确的回归测试和实际可行的最广相关套件；在 PR 描述中列出准确命令和
  所有跳过项。
- 保持严格提升：不支持的指令不得静默变为猜测操作或 `NOP`。
- 行为变更应增加语义覆盖，而不只是文本 IR 快照。
- 差异中不得包含无关清理、生成文件或本地构建产物。
- 检查[头文件与策略归属规范](#代码风格)，包括裸数字保护/默认值以及绑定漂移；
  仅通过格式化检查还不够。
- 当行为、支持范围、选项、命令或测试所有权变化时，更新公共文档和贡献者文档。

对不应以公开拉取请求开始的安全敏感报告，请遵循
[SECURITY.md](../../SECURITY.md)。
