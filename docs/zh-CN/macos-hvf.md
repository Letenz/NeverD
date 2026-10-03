**语言**: [English](../macos-hvf.md) | [简体中文](macos-hvf.md) | [繁體中文](../zh-TW/macos-hvf.md) | [日本語](../ja/macos-hvf.md) | [한국어](../ko/macos-hvf.md) | [Français](../fr/macos-hvf.md) | [Deutsch](../de/macos-hvf.md) | [Español](../es/macos-hvf.md) | [Italiano](../it/macos-hvf.md) | [Русский](../ru/macos-hvf.md) | [العربية](../ar/macos-hvf.md)

<!-- i18n-source: 3d2e1d7ba9709cac62c969fd06f4cfe7d5312c872f9339afb0161bc9643c4e33 -->

[← 文档索引](README.md)

# macOS 原生 CPU 后端（HVF）

NeverD 使用 Apple 的 Hypervisor.framework，后端名为 `hvf`，对应 Linux 的 KVM 和 Windows 的 WHP。按本机架构执行：Apple Silicon 使用 ARM64，Intel Mac 使用 x86-64。对支持原生执行的契约，`auto` 在主客体架构匹配时选择 HVF；跨架构以及 `software-cpu-v1` 契约继续使用 Unicorn。Rosetta 下的翻译进程会明确拒绝初始化，应改用原生 arm64 构建。

启用 `NEVERD_ENABLE_CPU_EMULATION=ON` 或驱动模拟后，`NEVERD_EMULATION_BACKEND_HVF` 默认开启。关闭该选项仍保留 `hvf` 配置名称，但能力查询报告构建未启用。原生后端初始化失败时不会静默回退。传输层要求 macOS 11 及硬件虚拟化支持；其他依赖仍可能要求更新的系统。

框架链接及 hypervisor 签名仅适用于 macOS 构建目标（`CMAKE_SYSTEM_NAME=Darwin`）。iOS 等 Apple 移动平台目标不会被附加该框架依赖或权限。NeverD 本身在架构匹配的 Mac 上运行时，iOS 来宾配置仍可使用 HVF。

真正调用框架的**进程可执行文件**必须具有 `com.apple.security.hypervisor` entitlement。CMake 在链接后为 CLI、worker 和模拟测试签名；默认使用 ad-hoc，可通过 `NEVERD_HVF_SIGN_IDENTITY` 指定已有签名身份。打包脚本在修改 Mach-O 依赖后重新签名 worker，并检查最终权限没有丢失。嵌入式 SDK 使用方应签署自己的宿主程序；NeverD 不会改签已安装的 Python 解释器。

独立构建 worker 或桌面程序时，导入的引擎库不提供其构建选项，因此 worker 默认仍会附加该 entitlement。若明确不需要 HVF，可设 `NEVERD_WORKER_SIGN_HVF=OFF`。

能力查询区分构建支持与实际初始化；使用 `cpu-capabilities --configuration=JSON --probe-host`
检查当前进程的硬件与签名条件。

实现复用现有 checked 契约、页表、寄存器模型和 RAM 事务。一个专用线程拥有进程内的 VM/vCPU，各逻辑 CPU 串行使用它；切换前退休旧映射，销毁时先解绑再释放 backing。Apple Silicon 的宿主映射按 16 KiB 对齐，来宾的权限和内存预算仍以 4 KiB 为单位。ARM64 在每条来宾指令前单步执行 TLB/I-cache 维护，并完整传输标量、TLS 和 FP/SIMD。Intel 使用 VMCS、MTF、XSAVE 和处理器异常退出。RIP/RFLAGS 每次均直接通过 VMCS 传输，也覆盖取消后重建 vCPU 的入口。Intel 的 CR0/CR4 同时遵守框架可写掩码和硬件必置位，用读取影子保持来宾可见状态。CR8 使用 VMX 访问退出及架构层的读取完成逻辑，因为宿主 TPR 接口可能与来宾实际状态不一致；只处理经过硬件认证的 CR8 读取，其他控制寄存器访问明确失败，checked 指令准入范围不变。每次创建或取消后重建 Intel vCPU 都初始化独立的托管 `IA32_KERNEL_GS_BASE` 上下文，来宾 MSR 访问仍陷出；未支持的 MSR/SWAPGS 指令不会进入硬件。两种架构都必须通过现有完整状态启动探针。

取消会确认原生中断已结束，再允许下一个任务进入；被取消的原生入口会重建 vCPU，防止旧中断影响后续任务。排队期间也检查停止令牌和期限。真正的宿主或状态读取错误优先保留，取消的普通 CPU/RAM 状态不提交。

实际硬件门禁及完整构建命令见[英文实现说明](../macos-hvf.md)。核心命令为：

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

门禁要求 HVF 用例实际通过，不能用全部跳过代替成功；支持关闭 Unicorn 后独立运行。测试包含跨线程调用、多个 CPU 的同地址隔离、权限与跨页访存、完整状态、启动探针对其他 CPU 的影响、部分映射失败回滚、排队取消、两种架构的原生死循环中断及重试。中断用例必须观察到实际原生返回，进入前取消不能算通过。当前 transport 门禁要求 ARM64 12 项、Intel 10 项；完整门禁分别要求 16 项和 14 项，Intel 包含 CR8 全部目标寄存器及权限回归。Intel 的交叉编译只能证明编译通过，仍需 Intel 真机执行门禁。

手动工作流默认使用带 `hvf` 标签的自托管宿主，也可选择 `hosted-intel` 尝试
GitHub 的 `macos-15-intel`。两种方式都先编译、签名并运行 `scripts/probe_hvf_host.c`，
实际创建和销毁 VM/vCPU，成功后才准备 LLVM。这项可用性探针不执行来宾指令。
宿主拒绝 HVF 就在此处失败，不能用 runner 名称推断硬件可用。
CPU 门禁通过后，还必须执行本机架构的全部 Darwin 工作负载。

GitHub 的[宿主策略](https://docs.github.com/en/actions/concepts/runners/github-hosted-runners)
把 runner 内的嵌套虚拟化列为实验性用途，不保证稳定性、性能或兼容性。
因此需保留专用原生 Mac 的持续验收路径；这条平台限制不能直接说明某次停滞的根因。

工作流会先构建只依赖 LLVM Support 和解码器的 `NeverDHvfTests`，验证原生指令执行，
再构建完整进程测试的依赖。`validation=transport` 可单独运行这一诊断；默认 `full`
仍要求完整 CPU 和 Darwin 两道门禁。`validation=darwin` 只构建进程测试所需目标，
可独立要求本机架构的全部 Darwin 工作负载通过。小范围检查复用完整清单中的 HVF 必需项，并在
摘要中标记 `hvf_transport_only=true`，不能当作完整 CPU/进程验收。
本地脚本对应参数为 `--require-hvf --hvf-transport-only`。
`validation=probe` 只检查 VM/vCPU 可用性，无需 LLVM，不能作为指令执行或
NeverD 传输层正确性的证据。

硬件虚拟化不保证在逐指令 checked 执行中更快；初始化、完整状态传输和缓存维护都有成本，应与相同 checked 契约的 Unicorn 比较。该后端不增加新的跨架构模拟方案或来宾 OS 模型。

## 本次验证记录（2026-10-02 至 2026-10-03）

最新已完成证据如下，表中范围有重叠，不能相加：

| 范围 | 干净源码 | 结果 |
| --- | --- | --- |
| ARM64 完整 CPU 清单，20 个目标 | `defc93928` | 6,842 项：849 通过、0 失败、5,993 跳过；16/16 原生必需项 |
| ARM64 全部 Darwin 工作负载 | `defc93928` | 286 项：65 通过、0 失败、221 跳过；39/39 原生必需项 |
| Intel HVF 全部 Darwin 工作负载 | `8dcc74c59` | 286 项：52 通过、0 失败、234 跳过；26/26 原生必需项 |
| Intel HVF 完整浮点目标 | `3e01cda5c` | 71 项：35 通过、0 失败、36 跳过；12 个 HVF 原生项全部通过 |
| Intel HVF 异常/状态目标中的原生子集 | `908a830e6` | 253 项全通过：120 项裸异常、128 项公开 CPU 除法、5 项状态切换 |

ARM64 使用 Apple M4 Max、macOS 15.6.1、SDK 15.5、Release，关闭 Unicorn。
完整 CPU 与独立 Darwin 证据保留在
`build-hvf-native/hvf-launcher-clean-{full,darwin}-evidence/`。
跳过项来自异架构、其他后端或不可用的软件执行路径，必需原生项均实际运行。
Intel 的同一组 20 个目标有 6,840 项注册；数量差异仅在 transport 目标：
ARM64 为 12 项，Intel 为 10 项。

[Intel Darwin 任务](https://github.com/NeverSight/NeverD/actions/runs/37106013999)
已完成全部 32 个方法，包括原始程序与宿主内核的对照。产物 `11267489438` 的
SHA-256 为 `cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`，
与 GitHub 元数据一致；每个原始 XML 身份和子进程状态均已独立核对。
证据在 `build-hvf/verification/hvf-intel-darwin-accepted/`。
同轮还通过 10 项 transport、100 次中断恢复及独立 CR8 状态/权限检查。
宿主为四个逻辑 CPU 的 macOS x86-64、Darwin 24.6.0。
这些结果证明有限 macOS 和 iOS Simulator 契约通过，不代表 iOS device 内核实测。

完整 Intel CPU 清单仍未验收。[先前任务](https://github.com/NeverSight/NeverD/actions/runs/37106679688)
在 `76a922088ceccb1e5fdaccbde7f0b3a680a4e3b8` 上完成了 20 个目标构建、10 项 transport、
100 次恢复和 CR8 检查，前置产物已下载并核对 SHA-256。该任务于 2026-10-03 08:35 UTC
结束，GitHub 记录运行器失联；没有 CPU 产物，作业日志接口返回 404。仓库当时没有
self-hosted runner。需要完整运行结果或可接入的原生 Intel Mac 才能关闭这项验收缺口。
早前停滞或取消且缺少完整证据的任务不计为通过，也不能据此推断来宾故障。
一轮[早前任务](https://github.com/NeverSight/NeverD/actions/runs/37097301977)
明确报告 runner 失联，其根因未确定。CPU/Darwin 工作流步骤分别有 30/15 分钟上限。

[裸异常专项](https://github.com/NeverSight/NeverD/actions/runs/37094333371)与
[除法专项](https://github.com/NeverSight/NeverD/actions/runs/37094335126)的产物证明上表 253 个原生项通过，
覆盖十种异常、两种权限、恢复、映射变化、私有入口完整性、终止异常与观察者停止。
两轮共享的 5 个状态切换检查只计一次。
[浮点专项](https://github.com/NeverSight/NeverD/actions/runs/37101437752)
完整核对了 71 项注册，包含物理浮点状态、每种 TOP、逻辑 CPU 切换和实际探针。
宿主标准 XRSTOR 通过，该 runner 不支持紧凑 XRSTOR。这些专项不能代替完整 CPU 门禁。

### 验收证据的完整性

托管 Intel 的完整及 Darwin 验收使用 `--execution-methods`：CTest 提供全部注册命令和身份，
每个 GoogleTest 方法以独立进程执行全部参数，保留标志、环境和工作目录。
未知 CTest 属性直接失败；支持直接命令及普通原生 `GoogleTest/LaunchTest.cmake` 包装，
拒绝自定义执行器、额外参数或预设输出路径。真实 Intel 的 6,840 项清单已完整解析并核对。

方法总时限最多 120 秒，超时后有界回收进程组；方法内不再分别执行 CTest 的逐参数时限。
保留原始 XML、日志、退出状态和精确名称映射。首次超时或 XML 不完整及时返回部分失败，
完整的断言失败结果仍允许收集后续方法。漏跑、重复、未执行或跳过原生必需项均失败。
摘要明确记录 `execution=gtest-methods` 和串行执行；自托管继续使用 CTest 逐用例进程及时限。
采集器、门禁、CI 配置、清单与结果审计共 124 项检查通过。先前的指令探针已清理；下述独立方法诊断仍用于调查 Intel 执行器失联。

### 修复与集成验证

三个 Intel 实际问题均已复现并修复。
[逐个 MSR 对照](https://github.com/NeverSight/NeverD/actions/runs/37078094659)
确认缺少托管 `IA32_KERNEL_GS_BASE` 上下文；另外 11 个 MSR 不恢复执行。
VM 指令错误 12 也出现在成功执行中，未将其当作根因。
[CR8 对照](https://github.com/NeverSight/NeverD/actions/runs/37082402190)
确认来宾优先级 0、1、3、15 与宿主 TPR 读回不一致，现由架构层完成经认证的 CR8 退出。
取消后重建 vCPU 的测试发现 RFLAGS 为零，改为直接通过 VMCS 传递 RIP/RFLAGS 后，
三种中断模式及重试均通过。这些回归保留在原生 transport/状态清单中。

ARM64 裸中断测试原先交替覆盖同一地址，可能执行缓存中的旧 `HVC`；改用独立固定程序后，
8 路并发重复 1,000 次通过，生产缓存维护仍保留。
干净源码 `9319c880d` 也通过 transport 12 项及 100 次中断恢复，无跳过。

构建与公开接口已验证：

- 同时关闭测试与 Unicorn 后，签名 CLI 能初始化 HVF 并执行 ARM64 ELF。
  关闭 HVF 后报告 `build_disabled`，不链接框架。
- 实际 capability probe 覆盖自动选择、异架构拒绝及软件契约。
  移除进程 entitlement 后报告 `device_access`，必需原生门禁失败而非跳过。
- Qt 6.11.1 包内 186 个 Mach-O 的依赖/签名检查通过，worker 保留 entitlement；
  Cocoa 启动和 EVM 加载通过。签名探针加载包内引擎，与 CLI 的三个 ARM64 Darwin
  profile 共 18 份报告一致。该包的依赖要求 macOS 15.0。
- 独立 worker 的 8 项检查（含 3 项真实引擎集成）和 Qt/IPC/MCP 的 19 项检查通过。
  `e078b129c` 的[GUI 工作流](https://github.com/NeverSight/NeverD/actions/runs/37053518872)
  在 macOS、Windows、Ubuntu 全部通过。
- Linux KVM 与 Windows WHP 关闭 Unicorn 后分别通过 51 项 Darwin 检查，零失败，
  各 26 个 x64 必需工作负载均执行。来源与产物见 [Darwin 验证记录](darwin-emulation.md)。

### 性能与尚未完成的范围

同机微基准使用 `checked-aarch64-v1`，运行两条初始化指令及 1,000 次 `ADD/SUBS/B.NE`，
共 3,002 条指令。创建 CPU 与启动探针不计时；预热后交替运行两个后端，各取 7 次中位数，
并核对最终寄存器与 PC。Unicorn 为 **73.9 ms**，HVF 为 **95.1 ms**，耗时约多 **29%**。
这是短整数循环，不能代表其他负载；当前没有证明性能提升。

随后补充以下测量。宿主共享负载约 30，HVF 耗时波动明显；这些受干扰数据不能与上一轮
绝对耗时直接比较。每项预热后取 7 次中位数；入口数由测试程序计数，未修改后端。

| 场景 | 来宾指令数 | Unicorn 中位 ms | HVF 中位 ms | HVF 最小–最大 ms | HVF 入口数 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 后端创建与启动探针 | — | 0.729 | 9.832 | 4.510–13.966 | 18 |
| 整数循环 | 3,002 | 160.160 | 2,327.342 | 1,650.271–4,907.726 | 18,012 |
| 条件分支 | 5,503 | 153.081 | 203.819 | 184.760–7,934.218 | 33,018 |
| 内存读写 | 5,002 | 141.085 | 173.748 | 160.369–7,851.044 | 30,012 |
| TLS 与函数调用 | 7,003 | 345.838 | 5,995.233 | 5,091.529–10,846.128 | 42,018 |
| 两个 CPU 交替执行 | 512 | 23.098 | 301.252 | 44.290–549.546 | 3,072 |
| Linux 进程 CLI（含初始化） | 685 | 141.420 | 971.306 | 65.619–1,431.597 | 未计数 |

普通 ARM64 指令需要 5 次维护入口加 1 次来宾入口。CPU 场景核对寄存器、PC、RAM 和执行数；
Linux 进程在正常退出、内存故障、未知服务和预算停止时的完整报告均一致。
空闲宿主性能测试及 Intel 完整 CPU 验收仍未完成；仓库尚未配置专用自托管 runner。
有限 Darwin 环境未实现 dyld、Mach IPC 或 Apple 应用框架，具体见[契约说明](darwin-emulation.md)。

## Intel 完整清单分片验收

早前完整运行 `37106679688` 于 2026-10-03 08:35 UTC 结束，GitHub 记录运行器失联。运行 `37116327329` 的四个作业均失联，未产生 CPU XML。这些现象不能定位某条来宾指令。托管 Intel 的 `full` 使用四个作业，最多两个并发；每个作业连续执行四批，共十六片，编号为 `job + 4 × batch`，保留原来每个作业的测试集合。每批仍先构建并检查完整二十个目标的 CTest 清单。`--hvf-shard INDEX/COUNT` 保留整个方法及全部参数，执行属性不同也不拆散。

CPU 执行前，`scripts/prepare_hvf_batches.py` 保存完整清单、各片所选清单和方法计划，由工作流独立上传这些诊断。一个本地 composite action 连续执行四批，每批结束立即上传原始 XML、身份映射、进程状态和必要环境变量白名单。外层统一的 30 分钟期限覆盖四批执行与上传；某批失败后不再执行后续批次。失败或超时后，只要运行器仍可通信，另有独立的两分钟诊断上传；宿主失联时只能依靠此前已上传的证据。计划及未完成的诊断包不能算作通过的分片。

独立 Linux 作业运行 `scripts/audit_hvf_shards.py`，从检出的源码重新推导目标和必需原生项。工作流仅下载本次尝试的 CPU 产物；审计要求同一干净提交的全部十六片、正确 macOS 宿主 ISA、规范化执行契约一致，且分片互斥、并集恰好等于完整清单。所有子进程必须成功退出，全部必需原生项必须通过；漏片、过滤器变更、摘要不符、XML 不完整或必需原生项跳过均失败。每个原生作业仍执行 transport、恢复、CR8 和独立 Darwin 门禁；自托管保持未分片 CTest。分批保存本身不代表 Intel 已验收。重试须重新运行全部原生作业，不混用之前尝试的产物。

[运行 `37123148209`](https://github.com/NeverSight/NeverD/actions/runs/37123148209) 在干净源码 `f5f29a484` 上产生的首份已核验 CPU 批次为第 `1/16` 片：476 个注册结果、31 个方法进程，82 通过、0 失败、394 跳过；该片唯一的必需原生项通过。产物 `11274755752` 已核验 SHA-256，原始 XML、子进程退出状态、清单和方法计划均与执行前计划核对一致。这只是 Intel 的部分证据，不代表完整 CPU 验收通过。

## 最新本机原生验证

干净源码 `4ce0b8247`，2026-10-03 UTC：ARM64 完整清单以 503 个方法进程执行通过，原始 XML、执行契约和子进程回收状态均已独立核对，独立 Darwin 验证也通过。两行覆盖重叠，不可相加。

| 范围 | 注册 | 通过 | 失败 | 跳过 | 必需原生项 |
| --- | ---: | ---: | ---: | ---: | ---: |
| CPU，完整清单 | 7,003 | 867 | 0 | 6,136 | 16/16 |
| Darwin | 286 | 65 | 0 | 221 | 39/39 |

`build-hvf-native/hvf-current-4ce0-full-evidence/summary.json` · `build-hvf-native/hvf-current-4ce0-darwin-evidence/summary.json`

## 独立 Intel 诊断

手动触发的 [Intel 诊断工作流](../../.github/workflows/hvf-intel-diagnostic.yml) 分别检出控制器与被测源码。`source-ref` 必须是完整提交 SHA；`shards` 选择原十六分片中的片号。`first-method` 从零计数，`method-count=0` 选择剩余方法；只有 `method-count=1` 时，`case-index` 才能选取一个原始参数。先发现全部二十个目标的完整清单，再进行选择；保留原 Release 构建、命令、参数与原生必需检查。

`intel-image` 与完整工作流一致，默认选择 `macos-15-intel`，也可选择 `macos-26-intel` 做受控对照。运行标题标明所选镜像，可用性检查仍要求原生 x86-64 宿主。镜像切换同时涉及操作系统、SDK 和工具链，不能据此单独归因于内核变化。

在总计 180 分钟的作业内，编译拥有独立的 120 分钟预算：首轮 macOS 26 完整构建耗时 76 分钟。原生方法执行仍限制为 120 秒，诊断 Action 仍限制为 30 分钟。

每个方法开始前，Action 上传不可变执行计划及主机快照；结束后保存原始 XML、进程回收、控制器状态及第二份快照。快照记录内存、交换空间、负载、磁盘，以及进程编号、状态、CPU、RSS 和可执行文件名，不包含进程参数或环境；采集错误也会保留。执行或上传失败即停止后续方法。每个方法仍有 120 秒执行上限；主机失联可能阻止清理和最终上传，此时只能使用已上传的证据。这些局部诊断不能替代完整 CPU 或独立 Darwin 验收。最后一个开始标记只能定位执行边界，不能直接确定故障指令或根因。

完整的 `Native macOS HVF` 工作流也接受可选的 `source-ref`，默认使用工作流所在提交，指定时必须提供完整 SHA。原生作业和汇总审计都会检出并核验同一份源码；即使控制器版本不同，审计也按被测源码提交核对证据。

选择 `hosted-intel` 时，完整工作流支持 `intel-image=macos-15-intel`（默认）或 `macos-26-intel`，两者均在[官方 runner 镜像列表](https://github.com/actions/runner-images)中。可保持同一 `source-ref`，明确比较宿主环境；镜像同时改变系统、SDK 和工具链。VM/vCPU、原生传输、CR8、完整 CPU 和 Darwin 的要求不变。选择镜像本身不能证明稳定性或运行时修复。

`sample-active-child=true` 可在方法运行五秒后保存一次封存的执行中快照：对已核验身份的原生子进程采样一秒调用栈、截取最多 1 MiB 的当前日志尾部，并记录宿主状态。默认值为 `false`。为遵守 artifact 数量限制，启用采样的每个作业最多选择 166 个方法；采样命令限时五秒，报告上限 1 MiB。身份核验和采集失败都会记录。执行中快照从独立的不可变目录上传；上传失败会取消原生子进程并使 action 失败。采样会改变调度，因此标记为带采样的部分证据，不重置原始方法计时，也不能替代完整验收。
