**语言**: [English](../macos-hvf.md) | [简体中文](macos-hvf.md) | [繁體中文](../zh-TW/macos-hvf.md) | [日本語](../ja/macos-hvf.md) | [한국어](../ko/macos-hvf.md) | [Français](../fr/macos-hvf.md) | [Deutsch](../de/macos-hvf.md) | [Español](../es/macos-hvf.md) | [Italiano](../it/macos-hvf.md) | [Русский](../ru/macos-hvf.md) | [العربية](../ar/macos-hvf.md)

<!-- i18n-source: a3178a18ee18f1b147cc6950c932b37a4bccb59a14815068345b1a8ec8edb51f -->

[← 文档索引](README.md)

# macOS 原生 CPU 后端（HVF）

NeverD 使用 Apple 的 Hypervisor.framework，后端名为 `hvf`，对应 Linux 的 KVM 和 Windows 的 WHP。按本机架构执行：Apple Silicon 使用 ARM64，Intel Mac 使用 x86-64。对支持原生执行的契约，`auto` 在主客体架构匹配时选择 HVF；跨架构以及 `software-cpu-v1` 契约继续使用 Unicorn。Rosetta 下的翻译进程会明确拒绝初始化，应改用原生 arm64 构建。

启用 `NEVERD_ENABLE_CPU_EMULATION=ON` 或驱动模拟后，`NEVERD_EMULATION_BACKEND_HVF` 默认开启。关闭该选项仍保留 `hvf` 配置名称，但能力查询报告构建未启用。原生后端初始化失败时不会静默回退。传输层要求 macOS 11 及硬件虚拟化支持；其他依赖仍可能要求更新的系统。

框架链接及 hypervisor 签名仅适用于 macOS 构建目标（`CMAKE_SYSTEM_NAME=Darwin`）。iOS 等 Apple 移动平台目标不会被附加该框架依赖或权限。NeverD 本身在架构匹配的 Mac 上运行时，iOS 来宾配置仍可使用 HVF。

真正调用框架的**进程可执行文件**必须具有 `com.apple.security.hypervisor` entitlement。CMake 在链接后为 CLI、worker 和模拟测试签名；默认使用 ad-hoc，可通过 `NEVERD_HVF_SIGN_IDENTITY` 指定已有签名身份。打包脚本在修改 Mach-O 依赖后重新签名 worker，并检查最终权限没有丢失。嵌入式 SDK 使用方应签署自己的宿主程序；NeverD 不会改签已安装的 Python 解释器。

独立构建 worker 或桌面程序时，导入的引擎库不提供其构建选项，因此 worker 默认仍会附加该 entitlement。若明确不需要 HVF，可设 `NEVERD_WORKER_SIGN_HVF=OFF`。

能力查询区分构建支持与实际初始化；使用 `cpu-capabilities --configuration=JSON --probe-host`
检查当前进程的硬件与签名条件。

实现复用现有 checked 契约、页表、寄存器模型和 RAM 事务。一个专用线程拥有进程内的 VM/vCPU，各逻辑 CPU 串行使用它；切换前退休旧映射，销毁时先解绑再释放 backing。Apple Silicon 的宿主映射按 16 KiB 对齐，来宾的权限和内存预算仍以 4 KiB 为单位。ARM64 关闭软件单步后，在一次原生进入中执行完整的固定 TLB/I-cache 维护序列。专用 HVC #1 必须通过返回 PC、syndrome、PSTATE 和 ESR_EL1 校验，才会单步执行准入的客体指令。`PSTATE.D` 不能屏蔽送往 EL2 的调试异常。完整传输标量、TLS 和 FP/SIMD。Intel 使用 VMCS、MTF、XSAVE 和处理器异常退出。RIP/RFLAGS 每次均直接通过 VMCS 传输，也覆盖取消后重建 vCPU 的入口。Intel 的 CR0/CR4 同时遵守框架可写掩码和硬件必置位，用读取影子保持来宾可见状态。CR8 使用 VMX 访问退出及架构层的读取完成逻辑，因为宿主 TPR 接口可能与来宾实际状态不一致；只处理经过硬件认证的 CR8 读取，其他控制寄存器访问明确失败，checked 指令准入范围不变。每次创建或取消后重建 Intel vCPU 都初始化独立的托管 `IA32_KERNEL_GS_BASE` 上下文，来宾 MSR 访问仍陷出；未支持的 MSR/SWAPGS 指令不会进入硬件。两种架构都必须通过现有完整状态启动探针。

ARM64 通过 `RunDeadline` 确认原生中断已结束；Intel 在 owner 线程内使用有限期限的 `hv_vcpu_run_until` 轮询取消。确认退出后才允许下一个任务进入；被取消的原生入口会重建 vCPU，防止旧中断影响后续任务。排队期间也检查停止令牌和期限。真正的宿主或状态读取错误优先保留，取消的普通 CPU/RAM 状态不提交。

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

门禁要求 HVF 用例实际通过，不能用全部跳过代替成功；支持关闭 Unicorn 后独立运行。测试包含跨线程调用、多个 CPU 的同地址隔离、权限与跨页访存、完整状态、启动探针对其他 CPU 的影响、部分映射失败回滚、排队取消、两种架构的原生死循环中断及重试。中断用例必须观察到实际原生返回，进入前取消不能算通过。当前 transport 门禁要求 ARM64 15 项、Intel 12 项；完整门禁分别要求 23 项和 20 项，Intel 包含 CR8 全部目标寄存器及权限回归。Intel 的交叉编译只能证明编译通过，仍需 Intel 真机执行门禁。

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

以下为优化前的历史结果；2026-10-04 的当前测量见文末。

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

`sample-active-child=true` 可在观测到原生子进程登记五秒后保存一次封存的执行中快照：对已核验身份的原生子进程采样一秒调用栈、截取最多 1 MiB 的当前日志尾部，并记录宿主状态。默认值为 `false`。为遵守 artifact 数量限制，启用采样的每个作业最多选择 166 个方法；采样命令限时二十秒，报告上限 1 MiB。身份核验和采集失败都会记录，包括命令返回 0 却没有调用栈报告的情况。执行中快照从独立的不可变目录上传；上传失败会取消原生子进程并使 action 失败。采样会改变调度，因此标记为带采样的部分证据，不重置原始方法计时，也不能替代完整验收。

完整工作流也支持 `recovery-repetitions=1000`，用于集中调查中断与恢复问题；默认仍为 `100`。选择 1000 次时，重复测试步骤的总预算从三分钟变为十分钟。每次原生测试保留原始期限、断言和遇错停止行为，运行标题会标明加长的重复设置。这些重复测试不能替代完整 CPU 或 Darwin 验收。

单独的 [Intel 恢复诊断工作流](../../.github/workflows/hvf-intel-recovery.yml) 只构建 `NeverDHvfTests`，在一个进程内使用原始 `HvfExecutor.Native*` 筛选器，选择 `repetitions=100` 或 `1000`。它要求精确的 `source-ref`、原生 Intel VM/vCPU 探测、Release、启用 HVF 并禁用 Unicorn。控制器、被测源码和官方 artifact 上传器分别检出。

Action 在执行前上传计划。执行期间保存初始进程标记，每增加至少 25 个完整轮次保存进度；存在尚未保存的新日志且进度停滞时，额外保存至多一次现场。上传期间合并进度，进度 artifact 最多 42 份，每份日志副本最多 1 MiB，只上传封存目录。上传不会暂停各轮执行或重置计时器，但仍会影响宿主调度，因此属于带观测的部分证据。

成功要求从第 1 轮到指定次数连续，每轮准确匹配原生测试的 RUN/OK/PASSED，无失败或跳过、退出码为零且确认进程已回收。重复执行时被覆盖的 XML 不能证明这些条件。原生执行总预算为三分钟或十分钟，控制器额外留出 30 秒清理，Action 上限为 20 分钟。上传失败会取消执行，取消操作会回收原生进程组和上传器。宿主仍可达时上传最终证据。不完整或截断的快照不能满足完整 CPU 或 Darwin 验收。

恢复工作流仍默认使用 `runner=hosted-intel`。`runner=self-hosted` 沿用 `[self-hosted, macOS, X64, hvf]` 标签，可用于 Intel 宿主对照。两种方式都要求原生 x86-64 和 VM/vCPU 探测通过。托管镜像安装 Ninja；自托管宿主须预先提供 `cmake`、`ninja`、`python3`、`clang` 和 `codesign`。源码隔离、重复次数限制、证据核验和失败处理保持一致。此入口需要已有可用的匹配 runner，不会创建硬件。

2026-10-03，干净源码 `e4a8169e69eb668ed3795efe4bd5f42cf4f287c2` 在本机原生 ARM64 Release 配置下通过验证：启用 HVF、禁用 Unicorn，使用 `NEVERD_LLVM_PREBUILT=ON`。完整 CPU 配置核对了 20 个目标、520 个方法和 7,125 个结果：882 项通过、6,243 项跳过、零失败，16 项必需原生用例全部通过。独立 Darwin 配置核对了 32 个方法和 286 个结果：65 项通过、221 项跳过、零失败，39 项必需原生用例全部通过。原始单进程恢复循环还连续完成 1,000 次重复，随后 12 项原生传输测试全部通过。原始日志、XML、进程状态和对应源码定义均已独立核验。CPU 与 Darwin 的总数有重叠，不能相加。这些结果不代表 Intel 完整验收通过，也不代表已经完成 iOS SDK 构建。

使用源码 `bd284894c60427cf4e6a60e661a1fa0df8a070f5` 的[独立 Intel 恢复诊断运行](https://github.com/NeverSight/NeverD/actions/runs/37159724276)于 2026-10-03 23:41:30 UTC 结束，GitHub 注释明确报告托管宿主失联。计划和十一份进度 artifact 得以保留，下载后均核对了服务端 SHA-256。最后保存的快照证明完整完成 252 轮并开始第 253 轮，不能据此定位最终故障。没有最终结果或进程回收记录，完整作业日志接口返回 404，因此请求的 1,000 轮仍未验证。仓库当时仍无自托管 runner。这些是保存下来的失败证据，不代表稳定性已经修复或 Intel 已通过完整验收。

可使用[个人仓库工作流](https://github.com/gmh5225/test_mac_intel)在托管 Intel runner 上验证，无需本地 Intel 真机。它分别固定 NeverD 诊断代码和被测源码，并独立记录工作流版本。排队时间与运行稳定性需要分别判断。恢复诊断现在会在报告失败前保存每个上传子进程的 PID、父进程、可执行文件、退出码、信号和终止原因。Action 失败后，收集器最多等待 20 秒，仅复制与该子进程 PID、父进程、进程名和执行时间匹配的 macOS IPS 崩溃报告；报告缺失会明确记录。上传失败仍会终止原生运行，不能据此断定 Hypervisor 故障或测试通过。

2026-10-04，个人仓库首次 [macOS 26](https://github.com/gmh5225/test_mac_intel/actions/runs/37175472452) 和 [macOS 15](https://github.com/gmh5225/test_mac_intel/actions/runs/37175511460) 作业分别在创建后 8 秒和 5 秒启动。两者均在上传子进程异常退出后失败；控制器取消并回收了原生子进程，分别留下 3 轮和 105 轮完整记录。最终证据包已保存并独立核验。另一次[纯上传对照](https://github.com/gmh5225/test_mac_intel/actions/runs/37177383621) 的 16 次上传全部通过，17 份产物均核对了服务端摘要，并标明 `native_execution=false`。这些证据区分了上传失败与原生断言失败，尚不能定位根因，也不能算作请求的 1,000 轮通过。此前组织仓库的对照也在 5 秒后启动，因此这些样本不能证明换仓库改善了排队速度。

后续 [macOS 26](https://github.com/gmh5225/test_mac_intel/actions/runs/37176652027) 与 [macOS 15](https://github.com/gmh5225/test_mac_intel/actions/runs/37176990174) 两次运行均于 2026-10-04 失败结束，GitHub 明确报告托管 runner 失联。分别保存的 10 份和 16 份产物均已核验。最后保留的原始日志证明分别连续完成 175 轮和 326 轮，随后各开始一轮；不能据此定位最终故障。两次均缺少原生最终结果及进程回收记录，完整作业日志接口均返回 HTTP 404。没有发出人工取消请求。Intel 的 1,000 轮门禁仍未验证通过。[保留的原始日志片段与运行、摘要清单](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-04)可在 Actions 产物保留期结束后继续查阅。

## ARM64 维护路径优化（2026-10-04）

五条 TLB/I-cache 维护操作现在在同一个不可变私有代码段中连续执行，以专用 HVC #1 退出。传输层严格核对完整 syndrome、返回 PC、PSTATE 和 ESR_EL1，随后只单步执行一条准入的客体指令。仅在维护期间关闭软件单步；所有屏障、完整状态捕获和共用取消期限均保留。不使用 ERET，因为异常返回会使 ESR_EL1 在架构上成为 UNKNOWN。 [Arm](https://documentation-service.arm.com/static/649ae5b238511951cb799288).

实机为 M4 Max、macOS 15.6.1，使用 Release、Apple Clang 17 和预编译 LLVM。独立计数中，相同的 42044 条客体指令及 56 条启动探针指令，原生进入从 252600 次降至 84200 次，即每条指令从六次降至两次。计数运行的耗时不纳入性能测量。三项故障/恢复测试在同一进程各连续通过 1000 轮；取消测试要求原生写入标记，并验证改写客体代码后成功重试。

干净集成版本 [389bebfdd](https://github.com/NeverSight/NeverD/commit/389bebfdda31a0db19facc7ab8ca5461a8c8c1bc) 的完整 CPU 清单：2546 通过、4710 跳过、零失败，23 项必需原生用例全部通过。独立 Darwin 清单：130 通过、156 跳过、零失败，39 项必需原生用例全部通过。两者覆盖重叠，不能相加；不代表 iOS SDK 或设备上的独立验证。

15 组进程配对交替运行，每个负载预热一次；测量时本任务未运行编译或测试。共享宿主负载为 26.7–33.0，软件对比为 28.8–32.6。表中为中位数［最小–最大］毫秒；加速比为逐对耗时比的中位数，95% 百分位 bootstrap 区间采用 10000 次重采样、种子 20261004。明显长尾和仅 15 组样本限制了结论的适用范围。

`4b54908b9` → `056090929` / Release / Apple Clang 17 / LLVM 23 prebuilt / Unicorn `df88be772`.

| 负载 | 优化前 ms［最小–最大］ | 优化后 ms［最小–最大］ | 配对加速比 | 95% 区间 | 更快的配对 |
| --- | --- | --- | --- | --- | --- |
| `initialization` | 1.292 [0.804–6.793] | 1.118 [0.797–29.077] | 0.998× | 0.809–1.154 | 7/15 |
| `integer` | 125.426 [92.700–587.385] | 70.229 [54.067–895.051] | 1.595× | 1.373–1.884 | 13/15 |
| `branch` | 251.909 [168.279–974.846] | 155.678 [106.833–1566.522] | 1.495× | 1.055–1.687 | 12/15 |
| `memory` | 231.574 [140.137–1511.815] | 143.496 [96.508–1209.777] | 1.535× | 1.276–1.984 | 13/15 |
| `tls_call` | 347.850 [203.188–2536.731] | 212.467 [136.304–1441.830] | 1.552× | 1.428–2.912 | 14/15 |
| `two_cpu_switch` | 34.629 [25.019–416.105] | 26.684 [18.926–60.159] | 1.389× | 1.283–1.515 | 13/15 |

### Unicorn / 优化后 HVF（大于 1 表示 HVF 更快）

| 负载 | 配对加速比 | 95% 区间 |
| --- | --- | --- |
| `initialization` | 0.232× | 0.195–0.325 |
| `integer` | 1.878× | 1.437–2.896 |
| `branch` | 2.681× | 1.824–3.317 |
| `memory` | 2.967× | 2.224–3.215 |
| `tls_call` | 2.339× | 0.977–3.257 |
| `two_cpu_switch` | 0.831× | 0.541–1.612 |

这些是共享宿主上的 checked ARM64 工作负载实测，不是通用性能排名。Intel runner 失联与进程崩溃仍待单独定位。受限的 macOS/iOS CPU 环境并未因此变成完整 Apple OS 或设备模拟。

[复现方法: `neverd-cpu-bench`, `benchmark_cpu.py`](../testing.md#reproduce-checked-arm64-cpu-measurements).

## Intel 隔离实验结果（2026-10-04）

有限 owner 期限候选 `909672ca6` 仍属实验方案。原始 1000 轮恢复测试在 macOS 15 和 26 上均失联；最后保存的日志分别证明 277/278、250/251 轮已完成/已开始。同一候选的普通指令测试没有主动取消操作，在 macOS 15 上也失联（576/577）。三次均有 GitHub 失联注记，均缺少最终原生结果和进程回收记录；保存的日志不能定位最终故障。[原始证据](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-04-boundaries)。

独立且未修改的 `hvf-edge-cases` 程序 `f150b38` 在两个镜像上完成 real-mode guest 和 100000 次随机中断调用尝试，耗时约 362、398 秒。两次均正常退出并回收子进程；产物摘要、源码哈希、进度标记和 guest 完成输出均已独立核验。先前 300 秒尝试达到观察器期限后被回收，没有 runner 失联。上游程序忽略随机中断返回值，调用次数不代表逐次成功交付。该对照不构成 NeverD 验收或性能比较。[源码、日志与审计](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-04-upstream)。

这些结果缩小了调查范围，但尚未证明生产修复。保留 VM 和 Executor 线程仅用于诊断对照，尚未成为接受的生命周期变更。Intel 仍须在同一个干净候选上通过原始 1000 轮恢复、完整 CPU 清单和独立 Darwin 门禁。

两项生命周期对照现已在两套 Intel 镜像上均通过 1000/1000 轮：仅重建 vCPU（37195529270、37195554929），以及保留同一 owner、重建 VM 和 vCPU（37196504453、37196535787）。后一项每次运行均包含 8000 个有序原生事件及第 1001 代的最终销毁；24/27 个产物摘要、原生与控制器正常退出、子进程回收均已核验。这缩小了与完整 Executor 周转的比较范围，但尚未定位原因或证明生产修复。下一项诊断将保留 VM、更换 vCPU 和 owner 线程；原始恢复及完整 CPU/Darwin 验收仍须通过。

线程更换实验未完成 1000 轮：macOS 15 上传器在 V8 字符串解析中触发 SIGTRAP（37198629082），macOS 26 上传器在 V8 作用域查找中触发 SIGSEGV（37198630903）。控制器随后取消并回收原生进程；保存的日志分别显示 24/25、467/468 轮完成/开始，没有原生断言或最终原生结果。两台 runner 均保持在线并提供匹配的崩溃报告。这属于观察器触发的中断，既不是已验证的原生通过，也不是已确认的 runner 失联。原因仍未知；先用纯上传对照比较，再决定后端变更。

两组纯上传对照（37199672430、37199674303）及原始失败快照重放（37200549588、37200551385）均在无 VM、无 guest 的条件下完成各 16/16 次上传。每次运行的 17 个产物摘要、零退出码、无信号或取消，以及与固定载荷提交 `e02e8c6` 逐字节一致均已独立核验。四组对照的 Node 24.19.0、V8 13.6.233.17-node.51 和执行文件 SHA256 相同；Intel UUID 与此前崩溃报告一致，但此前未记录执行文件摘要。这些对照没有复现上传内容单独导致的崩溃；与原生执行并发时的影响、崩溃原因和 Intel 验收仍未解决。 [已核验证据](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-04-upload-controls).

有限期限恢复对照保留整个 Executor/VM/owner 会话，在 macOS 15 上通过 1000/1000 轮（37203540596）。macOS 26（37203542459）完成 772 轮，第 773 轮因调用方的 50 ms 期限在入场阶段耗尽、原生回调尚未开始而触发断言。原生进程在 `--gtest_break_on_failure` 下以 SIGTRAP 退出并被回收，runner 保持在线；43/34 个产物摘要均已核验。这是原生测试断言，与上传器崩溃或 runner 失联不同，也不能单独归因于 VM 生命周期。测试修正沿用 2 秒协作式入场期限，在 owner 完成准备后开始原生 50 ms 计时，并受外层期限约束。全部中断模式、真实返回断言、重试检查及独立的 600 秒控制器期限均保留。后续控制器还会在 guest 执行前封存实际 Node 文件的 SHA256、原生 Mach-O UUID 和 Node/V8 版本；这证明采集时的磁盘文件身份，不证明进程内存完整性，也不能补齐旧崩溃缺少的摘要。

固定 owner 线程候选 `faad8299b` 在两套镜像上也未通过原始 fresh-Executor recovery1000 门禁：GitHub 分别确认 macOS 15（37202068724）和 macOS 26（37202070988）失联。15/12 个已核验产物分别保留 302/303、225/226 轮完成/开始的证据，均缺少最终原生结果和进程回收记录。保持 owner 线程存活不足以解决这两次运行的问题。候选已于 2026-10-04 13:16:49 UTC 通过 [PR #444](https://github.com/NeverSight/NeverD/pull/444) 合入 `dev`。合并不代表运行时修复已验证或完整 CPU/Darwin 验收通过；保存的日志前缀不能定位故障点。

修正后的测试 `7dd7342ec` 随后在 macOS 15（37204841332）和 macOS 26（37204843517）上均连续通过 1000/1000 轮会话复用恢复。每次运行的 43 个产物、原生与控制器零退出码、进程回收及前后一致的 Node 运行时记录均已独立核验。这验证了该诊断中的入场预算修正，尚未解决 fresh-Executor 失联或完成 Intel 全面验收。

修正后的恢复测试仅在每轮重建 vCPU（被测源码 `7dd7342ec`、控制器 `31afddad3`、工作流 `10bf50753`），已在 macOS 15 完成 1000 轮并通过（[37215096822](https://github.com/gmh5225/test_mac_intel/actions/runs/37215096822)，43 个已核验产物）。原生进程和控制器最终退出码均为 0，子进程已回收；生命周期证据为 1 个 VM、1001 代 vCPU 边界，总计/执行中的 vCPU 数仍未知。macOS 26（[37215098793](https://github.com/gmh5225/test_mac_intel/actions/runs/37215098793)，19 个产物）的 progress-016 上传器收到 SIGSEGV；IPS 与 PID 32519、父进程 29104、采集时间和 Node UUID 匹配。原生日志证明完成 403 轮、开始第 404 轮，未见断言失败。控制器随后取消并回收原生进程（SIGKILL），没有最终原生结果。这属于未完成的原生证据，不能判为 runner 失联或通过。执行前记录的 Node 24.19.0 SHA-256 为 `1052eb9c7d6c60a79b968e09f75af55a73462b0f6dff0964336d63b5e13eb63c`；它证明磁盘文件身份，不证明进程内存未改变。这些固定源码的结果不能为后续合入 dev 的版本背书。

每轮重建 VM/vCPU 的恢复测试最终也在两套镜像确认失联：[macOS 15 / 37213675739](https://github.com/gmh5225/test_mac_intel/actions/runs/37213675739)、[macOS 26 / 37213681083](https://github.com/gmh5225/test_mac_intel/actions/runs/37213681083)。两者使用源码 `7dd7342ec`、控制器 `4c702d35a`、工作流 `fad0eadf2`。23/26 个已核验产物保存连续日志，分别证明 502/503、575/576 轮完成/开始，均没有最终原生结果和进程回收记录。GitHub 仅确认通信中断，未确认原因。仅执行普通指令时的 VM 重建通过结论不能扩大到此恢复负载；依赖这两组通过的整个会话重置实验继续保持未启动。

上传器 `jitless` 对照（[37217523688](https://github.com/gmh5225/test_mac_intel/actions/runs/37217523688) / [37217525863](https://github.com/gmh5225/test_mac_intel/actions/runs/37217525863)，macOS 15/26）均在启动恢复测试前失败：官方上传器的 HTTP 解析器依赖 WebAssembly，而 `--jitless` 使其不可用，两个计划上传进程均以 1 退出且没有信号。每次有 2 个已核验产物，plan 从最终证据中恢复，不能算独立计划上传成功。这些失败没有测试原生恢复，但不否认工作流此前已运行 HVF 能力探测。替代的可选模式 `js-interpreter` 仅给控制器的 plan/progress 上传子进程传入 `--no-turbofan --no-maglev --no-sparkplug`，保留 WebAssembly 及其他代码生成；父进程、原生负载和截止时间不变，provenance/final 上传仍使用默认模式。本地真实 HTTP 请求与无凭据官方上传器检查通过，仅证明观察工具兼容性，不是 HVF 稳定性修复。

`js-interpreter` 模式的 vCPU 重建恢复对照使用源码 `7dd7342ec`、控制器 `caeb594ad`、工作流 `e6d054c45`。macOS 26（[37218631679](https://github.com/gmh5225/test_mac_intel/actions/runs/37218631679)）通过 1000/1000：43 个产物、全部 41 次 plan/progress 上传调用、原生/控制器退出码 0、子进程回收和最终第 1001 代边界退役均已独立核验。macOS 15（[37218629672](https://github.com/gmh5225/test_mac_intel/actions/runs/37218629672)）的 progress-018 上传器仍收到 SIGSEGV；21 个产物保留原生 460 轮完成/461 轮开始，随后控制器取消并以 SIGKILL 回收原生进程。IPS 的 PID 62812、父进程 59666、时间和 Node UUID 均匹配，顶部栈帧位于 V8 并发堆标记，非法地址为 `0x80000000`。禁用三个 JavaScript 编译层的参数确实生效，两镜像记录的 Node 文件 SHA 相同，说明该模式未消除上传器崩溃。栈帧不能确定根因，macOS 15 原生结果仍未知，完整 CPU/Darwin 验收和缓存候选的双镜像门槛均未满足。默认长期缓存还会违反既有临时探测与最后客户端释放后归还 VM 的约定，因此未实施。
