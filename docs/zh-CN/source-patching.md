**Languages**: [English](../source-patching.md) | [简体中文](source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: e4a0692201d6aed196a2d3754627545fcc84707b2bb16eb47b48da1e8d1ebb14 -->

[← Documentation Index](README.md)

# 源码与原生对象 patch

NeverD 使用指定编译器编译替换函数，再安装到明确的原函数入口。编译器负责 C/C++ 语言语义；NeverD 负责地址绑定、重定位、存储、展开记录和发布。二进制修复与提升后的重写继续使用各自管线，不需要重建程序其余部分。

<!-- i18n-section: compiler -->
## 选择编译器

使用 `--compiler=clang`、`--compiler=gcc` 或 `--compiler=neverc`。Clang 和普通模式的 NeverC 可以生成 LLVM IR；`object` 制品保留编译器生成的机器指令。CLI 对 GCC 默认选择对象；Python SDK 使用 GCC 时须明确指定 `artifact="object"`。选定的编译器不会静默回退。

使用 `--compiler-arg` 或 SDK 的 `compiler_args` 传入包含路径、宏定义及受支持的源码选项。禁止覆盖目标或输出模式，也禁止在任何参数位置传入响应文件标记（`@file`），包括单独选项的值。普通的 `-I` 路径和 `-D` 宏定义仍然受支持。

示例地址须替换为已确认的原函数入口。替换声明必须匹配原机器 ABI，包括调用约定和结构体布局；地址本身不能证明声明正确。C++ 需要 `--language=c++` 和正确的 Clang++/G++ 路径。建议使用 `extern "C"` 入口，否则传入精确的修饰名。允许额外辅助函数。`--entry` 与 `--func` 共同明确选择替换目标；旧入口 `--from-c` 也会遵守指定地址。

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
## 已验证配置与限制

| 编译器 | 制品 | 已验证的操作系统运行配置 |
|---|---|---|
| Clang 22.1.6 | LLVM IR | ELF x64 C/C++；Mach-O x64/ARM64 C/C++；PE x64 C/C++ FH3 |
| Clang 22.1.6 | 原生对象 | ELF x64 C/C++ |
| GCC/G++ 13.3.0 | 原生对象 | ELF x64 C/C++，需要兼容的 Itanium 运行库 |
| NeverC 3389.1.5 普通模式 | LLVM IR | ELF x64 C；Mach-O x64/ARM64 C |
| NeverC 3389.1.5 普通模式 | 原生对象 | ELF x64 C |

这些是经过验证的配置，不代表所有编译器版本和选项都兼容。尚未接入 `dyncode` 或 NeverC C++。GCC 不能生成 LLVM IR。原生 COFF/Mach-O 对象尚不支持；对象安装要求 ELF x64。未知重定位、TLS、构造函数、LTO、common/IFUNC 符号以及无法解析的导入均明确失败。JSON 凭据记录编译器身份、参数、绑定和安装地址；原生对象另记录 SHA-256。

独立 CPU 矩阵覆盖四种架构（`x86`、`x64`、`ARM32`、`ARM64`）和三种格式（`ELF`、`PE`、`Mach-O`），检查源码安装或明确拒绝。隔离的 CPU 执行不能证明操作系统加载器、C++ 展开器或任意 ABI 正确；这些结论应依据上表的运行配置。

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `REFUSED` | `CPU` |
| `PE` | `REFUSED` | `CPU` | `REFUSED` | `REFUSED` |
| `Mach-O` | `REFUSED` | `CPU` | `REFUSED` | `CPU` |

`CPU` 表示源码安装与隔离函数执行；`REFUSED` 表示明确缺少安装契约。ARM32 ELF 需要 EHABI 注册；32 位 Mach-O 需要经验证的加载器、前端及展开契约；PE 源码安装目前要求 x64。

CPU 矩阵使用独立的算术、分支与辅助函数调用。其结果不能证明 ELF x86/ARM64 数据引用或 ARM64 C++ 导入正确；额外运行探测因尚未支持的重定位契约而明确拒绝了安装。

<!-- i18n-section: references -->
## 原程序引用与存储

绑定对应外部声明：`function` 要求已确认的原函数或导入桩；`data` 要求可写存储；`const` 要求 IR 中为 const 声明。对象数据绑定必须提供非零范围。范围不能跨越存储边界，也不能赋予 RELRO 写权限。不能回调被替换函数的原始函数体：此配置尚无保存原序言的回调跳板。

新代码保持 RX，新可变状态保持 RW。只复用已有导入，并区分 GOT/IAT 槽与可调用桩。Mach-O 需要空余加载命令空间，例如 `-Wl,-headerpad,0x400`；PE 需要空余节头空间。空间不足会失败。ELF `.data.rel.ro` 和原 CET/IBT 保护需要额外契约，目前明确拒绝。

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

Mach-O 展开表增长还需要相邻的空余文件与虚拟空间；仅预留头部空间并不能满足这一要求。测试样本分别处理这两种空间限制。

<!-- i18n-section: exceptions -->
## C++ 异常

ELF 使用编译器的 FDE/LSDA；Mach-O 使用紧凑展开/DWARF。PE x64 支持编译器的 FH3 表，要求原程序已有兼容的 MSVC 运行库和类型身份；type-info 数据可能需要显式绑定。FH4、自定义 personality、SEH 源码和启用的 CFG/EHCont/XFG 模式会被拒绝，原保护标志保持不变。

运行用例覆盖正常返回、内部捕获、新异常被原调用者捕获、原被调函数的异常穿过替换函数、恰好一次 RAII 清理、重新抛出、noexcept 终止及未替换函数。原调用点必须允许异常传播；patch 不能恢复原编译器因函数不抛异常而删除的捕获路径。

<!-- i18n-section: publication -->
## SDK 与发布

追加式 C API 为 `neverd_patch_source`，选项结构为 `neverd_source_patch_options_v1`；`neverd_patch_source_report` 返回凭据。对象输入须提供字节长度。旧 API `neverd_patch_from_c` 继续可用。

输入先保存快照，候选文件暂存于目标旁，重新加载并核对每个请求入口后才独占发布。不覆盖现有文件或符号链接别名。缺少任一映射会使整个请求失败。拒绝 `--mode=inplace`。会话 IR 变换不能用于原生对象。Mach-O 严格验证签名并重新签名；不会静默剥离 PE 签名。

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

<!-- i18n-section: verification -->
## 构建与验证

通过 `NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` 指定兼容的源码编译器（已验证 Clang 22.1.6）。集成构建只修正构建目录中的 LLVM 副本，不改子模块或共享预编译缓存。r3/r4 预编译版的 x64 PLT 问题可能导致 IR patch 被拒绝；原生对象避开该 MC 路径。`NEVERD_TEST_REQUIRE_FIXED_LLVM=1` 要求编译器生成精确且已修正的 PLT 引用。

`NeverDSourcePatchTests` 包含原生 C/API/对象/异常用例；`NeverDSourcePatchMatrixTests` 使用 Unicorn 检查所有目标组合。配置 `NEVERD_TEST_WINDOWS_CRT_LIB`、`NEVERD_TEST_WINDOWS_UCRT_LIB` 和 `NEVERD_TEST_WINDOWS_SDK_LIB` 后，`SourcePatchPE` 执行七种 C++ 场景。非 Windows 宿主还需要 `NEVERD_TEST_WINDOWS_RUNNER`。`NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` 使缺少运行输入成为失败。Windows 专项工作流要求原生执行且拒绝跳过用例。

开发 Mac 上的 Linux 证据通过 Docker 和显式 Linux 加载器取得；PE 证据使用 Wine 11。Mach-O ARM64 在 Mac 本机运行。Windows 原生 CI 结果须单独查看：配置了工作流不等于运行通过。历史数量、基线失败和模拟环境限制见[详细契约与验证记录](../source-patching-details.md)。

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

LLVM 构建副本还修正了 Mach-O x64 的 GOT 数据引用：Clang 的类型捕获表以四字节重定位字段末尾为基准。r3/r4 预编译版会偏移四字节；发布前检查会拒绝未修正的结果。开发 Mac 的 x64 Mach-O 用例通过 Rosetta 2 执行，ARM64 用例直接执行。

Windows x64 原生验收 [96cf7cd59](https://github.com/NeverSight/NeverD/actions/runs/38200205814) 的 15 个 PE/对象/trampoline 专项用例和 48 个目标矩阵用例全部通过，无跳过。FH3 用例执行了七种 C++ 运行场景。该轮使用 Clang 22.1.6 及原生 MSVC、Windows SDK 输入。
