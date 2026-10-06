**语言**：[English](../cpu-execution.md) | [简体中文](cpu-execution.md) | [繁體中文](../zh-TW/cpu-execution.md) | [日本語](../ja/cpu-execution.md) | [한국어](../ko/cpu-execution.md) | [Français](../fr/cpu-execution.md) | [Deutsch](../de/cpu-execution.md) | [Español](../es/cpu-execution.md) | [Italiano](../it/cpu-execution.md) | [Русский](../ru/cpu-execution.md) | [العربية](../ar/cpu-execution.md)

[← 文档索引](README.md)

# CPU 执行与能力查询

CPU 执行独立于来宾 OS、映像加载器和调用约定。启用 `NEVERD_ENABLE_CPU_EMULATION` 可单独构建；`NEVERD_ENABLE_DRIVER_EMULATION` 也会包含 Windows 驱动环境。[架构指南](architecture.md)说明所有权、后端选择和当前平台限制。

`NEVERD_ENABLE_SEMANTIC_TESTS` 默认为 `ON`，控制 `unittests/semantic` 中的测试组及其聚合运行目标。构建不依赖 Unicorn 的原生 CPU 测试时，保留 `BUILD_TESTING=ON`，同时设置 `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` 和 `NEVERD_EMULATION_BACKEND_UNICORN=OFF`。原生 KVM/WHP 测试仍可构建，包括具有对应 SDK 头文件的 Windows ARM64/MSVC 配置。在 Windows ARM64 上启用 Unicorn 仍需 ARM64 LLVM-MinGW 工具链。这项构建解耦不等于 ARM64 原生运行验证。

## 配置

公共 [`ExecutionConfiguration`](../../include/neverd/emulation/ExecutionConfiguration.h) 同时供 CPU 工厂和能力报告使用。分配 CPU 或附加地址空间前会先验证要求。省略值使用契约固定配置；显式指定的不支持值会失败。

| JSON 字段 | 默认值 | 含义 |
|---|---|---|
| `backend` | `auto` | `auto`、`unicorn`、`kvm` 或 `whp` |
| `contract` | `software-cpu-v1` | 带版本的执行语义 |
| `architecture` | `x86_64` | `x86_64` 或 `aarch64` |
| `privilege` | 契约配置 | `flat`、`supervisor` 或 `user`；必须符合所选契约 |
| `virtual_address_bits` | 契约配置 | checked 配置为 48 位；flat 配置为 64 位直接映射命名空间 |
| `page_size` | 4096 | 来宾映射粒度；其他值会被拒绝 |
| `required_features` | `[]` | [`ExecutionConfiguration.def`](../../include/neverd/emulation/ExecutionConfiguration.def) 中的必需功能名 |

`driver-strict` 支持 x64；`software-cpu-v1` 支持 x64 与 ARM64。`checked-x64-v1` 与 `checked-aarch64-v1` 要求对应架构并以 supervisor 特权运行。`checked-user-x64-v1` 与 `checked-user-aarch64-v1` 分别在 CPL3 和 EL0 执行与契约对应的有界指令集，具备 MMU 隔离和显式服务请求退出。支持 Unicorn 以及与主机匹配的 KVM/WHP；`auto` 遵循现有主机选择。flat 配置不承诺架构级 user/supervisor MMU 隔离。checked ARM64 与 x64 配置支持下文列出的受限 FP/SIMD 指令族。supervisor x64 另支持受限 MMIO 事务和预备读取的字符串传输；user 配置拒绝设备映射。端口 I/O 仍不支持。只有 user 配置会报告 `service_traps`。

用户态执行要求**每个**映射页同时具有 `UserAccessible` 和相应的 `Read`、`Write` 或 `Execute` 权限。已有映射默认仅 supervisor 可访问；即使别名共享物理字节，其权限仍彼此独立。仅有 `UserAccessible` 不会授权访问。可信宿主操作和 supervisor CPU 使用 RWX。例如：

```cpp
Configuration.Contract = ExecutionContract::CheckedUserX64;
Configuration.Privilege = ExecutionPrivilege::User;
auto CPU = llvm::cantFail(createExecutionBackend(Configuration, Space)).CPU;
llvm::cantFail(CPU->map(Code, 4096, Read | Write | Execute | UserAccessible));
```

特权由契约固定。恢复上下文或绑定地址空间不会改变特权，写 x64 段选择子也不能提权。可恢复的数据访问故障会保留原指令和寄存器，直到所属模型处理。`canAccess` 严格检查请求的权限；查询 user 可见性时需包含 `UserAccessible`。页表是 CPU 私有投影，不开放可修改的来宾页表或特权切换 API；ARM64 的 user 页在 EL1 也不可执行。

未知字段、null 字段值、无效名称或数值宽度、重复功能及不支持的组合都会失败。输入上限为 64 KiB；旧 CPU 工厂和驱动 C 选项保持兼容。

<a id="parallel-cpus-and-mmio"></a>

## 并行 CPU 与设备原子事务

KVM、WHP 和 Unicorn 的 checked x64/ARM64 可请求 `ExecutionFeature::ParallelCPUs`（`parallel_cpus`）。独立 CPU 可在不同宿主线程上共享物理 RAM；已证明不写 RAM 的原生指令可重叠执行，待处理写入阻止新指令进入，等待读取结束后再发布。指令效果采用顺序一致性，支持等待取消和回滚。所有运行结束前，映射修改与宿主写入仍被禁止。同一 CPU 对象只有 `stop()` 支持跨线程调用。默认执行和 OS 调度仍为协作式，弱内存模型探索属于独立契约。

并行 WHP CPU 使用独立分区和私有传输 RAM，每步同步代码与全部声明的操作数，仅暂存声明的输出字节；观察器始终读取共享 RAM 的权威状态。KVM 与 checked Unicorn 直接共享 backing。此能力不扩展到 HVF 或 Unicorn `Software` 契约。

`MMIOAtomics`（`mmio_atomics`）要求设备明确提供 `GuestMMIOCallbacks::PrepareAtomic`。checked supervisor x64 支持已接纳的交换、比较交换、整数更新和修改型位操作；ARM64 支持包含 `CASP` 的 LSE。操作数须自然对齐，宽度限于 1/2/4/8/16 字节。x64 在私有临时页执行原指令，保留精确寄存器与 FLAGS；ARM64 复用统一 LSE 语义。准备阶段无设备副作用，提交验证生命周期和版本并只生效一次。提交前停止或失败保留 CPU/设备状态，成功提交优先于同时到来的停止。禁止退化成普通 Read+Write。内核寄存器库仍使用声明的 1/2/4 字节宽度，并拒绝同值写入、电源变化或所有者销毁后留下的旧预览。ARM64 普通设备访问、设备独占监视器、用户态 MMIO 和任意真实硬件不在此能力内。

## 不执行工作负载的能力查询

```bash
neverd cpu-capabilities
neverd cpu-capabilities \
  --configuration='{"contract":"checked-aarch64-v1","architecture":"aarch64"}' \
  --probe-host
```

schema 版本为 1。报告分别给出补全配置前的 `requested_configuration`、规范化后的 `configuration`、静态语义 `capabilities`、适配器构建与 ABI 兼容性的 `build`，以及仅在请求 `--probe-host` 时提供的 `host`（否则为 null）。主机探测会在私有 RAM 上初始化临时 CPU，只证明该初始化流程成功，不能证明任意工作负载兼容性。可用性可能变化；不可用后端不会被静默替换。合法报告即使说明后端不可用，CLI 仍返回 0；配置或查询无效时返回 1。

## SDK 与 C++ 边界

[`neverd_cpu_capabilities_json`](../../include/neverd/sdk/NeverDCAPICPU.h) 接收现有 session、可选配置 JSON，以及取值为 0 或 1 的 `ProbeHost`，不需要加载映像。用 `neverd_free_string` 释放结果；返回 NULL 表示错误，可通过 `neverd_last_error` 查询。禁用 CPU 的构建仍导出同一函数并明确报告功能已禁用。Python 插件使用 `session.cpu_capabilities(...)`。C++ 可分别使用 `executionCapabilities`、`resolveExecutionConfiguration`、`queryExecutionBackendBuild` 和 `probeExecutionBackend`；`createExecutionBackend` 将 CPU 附加到已有地址空间，或创建私有 RAM 和默认空间。

## CPU 结果与预算

`CPU.runUntilExit(PC, TimeoutMicroseconds)` 返回带类型的 [`ExecutionExit`](../../include/neverd/emulation/ExecutionExit.h)。开始执行前的配置错误返回 `llvm::Error`；执行开始后会明确报告停止、deadline、服务请求、可恢复故障、来宾故障／trap、不支持的操作、设备／后端故障或无法解释的引擎停止。CPU、设备或后端故障优先于同时发生的 stop/deadline，但会保留独立事实和故障详情。timeout 必须为正数，且能表示为时长和绝对 deadline；否则会在修改 CPU 前失败。每次运行都必须使用有限预算；0 既不表示无限，也不是有效的立即超时。控制是协作式的，不保证硬实时上限。结果不会消费可恢复故障；OS 所属模型必须先取走故障并安装已验证的异常转移，再恢复执行。旧 `run`、`fault`、`timedOut` API 仍可用；只覆盖 `run` 的外部 CPU 实现会拒绝新的类型化边界。

## 服务请求

checked user x64 只拦截精确的无前缀 `SYSCALL` 编码；checked user ARM64 拦截 `SVC #imm16`。`SYSENTER`、`INT`、`HVC`、`BRK` 等机制仍不支持。先运行指令观察器；若它未停止或产生故障，CPU 会在执行服务指令或进入后端**之前**返回 `ExecutionExitKind::ServiceRequest`，并携带指令类型、原始 `PC`、顺序 `NextPC` 和 SVC immediate。寄存器、flags、栈和特权均不改变：x64 尚未发生 SYSCALL 对 RCX/R11 的改写，ARM64 也尚未进入异常向量。SVC immediate 不是通用服务编号。

请求会保持 pending，并阻止后续执行、CPU 修改、地址空间绑定及上下文捕获／恢复；CPU 停止时由 OS 所属模型通过 `takeServiceRequest()` 恰好消费一次。该模型负责解码 OS ABI、处理服务，并显式选择结果寄存器和下一 PC／异常转移。不支持的服务必须在该边界明确失败；重试原始 PC 会产生新请求，不会隐式变成 NOP 或成功返回。服务事件优先于同时发生的 stop/deadline，但来宾或后端故障优先级更高。CPU 停止、软件 HLT、deadline 或 trap 均不能证明工作负载成功。单独的 [Linux 进程配置](process-emulation.md)使用自己的 OS 模型，不证明 Windows、Android 或 Darwin 可运行。Windows 与 ARM64 原生执行仍需要实际运行验证。

## x64 扩展与原生 CPU 状态

关于屏蔽异常的 x64 指令说明描述可移植基线。KVM/WHP 为 `driver-strict`、`checked-x64-v1` 和 `checked-user-x64-v1` 增加 `precise_simd_exceptions`：原生启动探针验证精确 `#XM` 及两种重试后，才允许未屏蔽的 MXCSR 写入、`LDMXCSR` 和 Windows `CONTEXT` 恢复。`ExecutionProfiles.def` 统一负责选择，`supportsSIMDExceptions` 提供已解析的实例能力。checked Unicorn 仍要求屏蔽；本次不扩展 ARM64 或 HVF 的异常能力。

原生 x64 KVM/WHP 通过 `X64SIMDException` 将实际 `#XM` 故障交给驱动 C SEH。硬件故障的 `CONTEXT` 保存 XMM0–15 和 MXCSR。过滤器、异常展开的 finally 回调及选定处理器以 MXCSR `0x1f80`、清除 DF 的状态执行。负过滤器可修改 XMM 及顶层 `CONTEXT.MxCsr`（按客户 CPU 掩码截断）后重试原指令；`FltSave.MxCsr` 不控制内核恢复。模型 API 抛出仍保留整数/控制记录；x87/AVX 上下文修改继续明确拒绝。

checked x64 允许受限的传统 SSE/SSE2 移动和逻辑指令、`MOVLHPS`/`MOVHLPS`，以及带屏蔽的标量 `CVTTSS2SI`/`CVTTSD2SI`/`SUBSS`/`SUBSD`。MXCSR 保留粘滞状态、舍入和 FTZ；可移植执行仍拒绝未屏蔽异常。KVM/WHP 同步全部 16 个 XMM 寄存器及 MXCSR；未列出的编码和操作数组合仍拒绝。

checked x64 还支持带屏蔽的传统 `ADD`、`SUB`、`MUL`、`DIV`、`SQRT`、`MIN` 和 `MAX` 的 `SS`、`SD`、`PS`、`PD` 形式。`X64SSEInstructions.def` 统一定义操作数宽度、对齐和准入规则。`MaskedSSEArithmeticMatchesIndependentHostExecution` 使用独立的本机 CPU 参照验证寄存器与 RAM 形式，覆盖四种舍入模式、FTZ、有符号零、次正规输入和 NaN；`SSEMemoryObserverStopsBeforeResultAndStatusChanges` 验证停止请求发生在效果提交之前。x87 和 AVX 仍未开放。

在可移植软件配置中，Unicorn 会对传统 `MINSS/MINSD/MINPS/MINPD` 和 `MAXSS/MAXSD/MAXPS/MAXPD` 选中的值应用 DAZ。选中的次正规输入变为有符号零；NaN 载荷和已有 MXCSR 状态保持不变。`test_x86_sse_minmax_daz` 检查寄存器、RAM 和寄存器别名形式，覆盖 DAZ 开关、全部舍入模式、FTZ 和粘滞状态。

KVM/WHP 通过私有 `FXSAVE64` 执行探测 `MXCSR_MASK`，并用有符号次正规数的算术验证所宣告的 DAZ 能力。checked Unicorn 提供软件掩码。`supportedControlBits` 返回 CPU 的不可变掩码；FX/XSAVE、快照和 Windows CONTEXT 使用同一能力。checked `LDMXCSR/STMXCSR` 精确访问 m32，检查完整 RAM 范围，并在故障或观察回调取消时保留状态。加载保留位触发 #GP；可移植执行仍拒绝未屏蔽 SIMD 异常。`X64MXCSRTests.cpp` 检查控制值与重试；`X64DAZData` 用原始本机指令对照全部 28 种已准入 SSE 算术形式，覆盖 DAZ、舍入和 FTZ。这不扩展 HVF 的 DAZ 支持。

原生 x64 启动验证共用一个 `5 s` 预算，涵盖传输层冷启动准备及全部探测步骤；客体执行预算独立。初始化中断会报告指令阶段、`stop_requested` 和 `deadline_reached`，保留中断类型及底层传输诊断，不重试或接受未完成验证的 CPU。

`PAUSE`（`F3 90`）通过共享 x64 机器边界在 KVM/WHP 和受限 Unicorn 上执行，包括原生 `driver-strict`。`X64PauseTests.cpp` 验证完整状态保留、执行前停止、上下文恢复、非法 `LOCK` 拒绝，以及自旋循环的超时和恢复。这条处理器提示指令不负责客户线程调度，也不保证具体延迟。

`PUSHFQ`（`9C`）和 16 位 `PUSHF`（`66 9C`）通过原生 KVM/WHP 及 checked Unicorn 执行，包括 `driver-strict`。共享 ISA 层在 RAM 事务提交前从压栈结果中移除内部单步 TF。隐式栈寻址使用完整 RSP，前缀顺序决定操作数宽度。完整范围权限检查、观察器停止和故障重试保持原子性。`X64PushFlagsTests.cpp` 覆盖全部 256 种允许的标志组合、九种编码、跨页别名、用户权限、中止及上下文恢复。

`POPFQ`（`9D`）与 16 位 `POPF`（`66 9D`）由共享 x64 ISA 层恢复标志，适用于 KVM、WHP、checked Unicorn 及 `driver-strict`。在配置固定 IOPL 为零的条件下，CPL0 可修改 IF；CPL3 保持 IF 和 IOPL。保留位及 VM/VIF/VIP 被忽略，RF 清零。有效修改客体 TF、NT、AC、ID 或 CPL0 IOPL 仍不支持，会在发布状态前明确失败。完整栈读取先于 FLAGS/RSP/RIP 的原子更新；操作数使用完整 RSP，有效前缀顺序决定宽度。栈可以只读或与可执行内存互为别名。共享层完成指令，避免清除传输层的内部 TF。`X64PopFlagsTests.cpp` 包含独立的原生 CPL3 指令对照，检查每个输入位、故障、观察器及后续原生执行。

checked x64 接纳 `CLC/STC/CMC` 和 `LAHF/SAHF`。进位指令由传输层执行；AH 转换由 KVM、WHP 和 checked Unicorn 共用一处 ISA 实现，包括原生 `driver-strict`。LAHF 将五个状态标志及固定位写入 AH；SAHF 只修改 CF/PF/AF/ZF/SF。OF/IF/DF 及未选择的寄存器保持不变。被忽略的前缀，包括所有 REX 值，仍使用隐含 AH。`X64StatusFlagsTests.cpp` 检查主机原始指令、完整状态、取消及继续执行。固定版本的 Unicorn 译码器也为可移植配置保留 REX 下的隐含 AH，并在状态改变前拒绝这五条指令的 LOCK 形式。

checked x64 通过 KVM、WHP 和 Unicorn 执行 `SHLD/SHRD`，支持 16/32/64 位目标及 imm8 或 CL 计数。计数采用架构掩码；16 位形式掩码后大于 16 的未定义计数在产生效果前拒绝。RAM 目标按准确宽度检查读写权限，并复用现有事务：结果观察器在发布前运行，取消会回滚 CPU 与内存。LOCK 和设备操作数仍不支持。

`X64ScalarShiftInstructions.def` 统一定义 `SHL/SHR/SAR`、`ROL/ROR` 和 `RCL/RCR` 的 8/16/32/64 位目的操作数效果。寄存器和 RAM 形式支持隐含的 1、imm8 或 CL 计数，包括掩码后为零的计数及 SAL 别名。RAM 写入经过权限检查、结果观察回调和共享事务；停止或回调错误会恢复 CPU 与内存。KVM、WHP 和 Unicorn 检查模式共用此边界。LOCK 与 MMIO 形式仍不受支持。

`X64LoopInstructions.def` 通过处理器传输执行 `LOOP/LOOPE/LOOPNE`。地址宽度选择 RCX 或零扩展的 ECX，FLAGS 保持不变。目标宽度遵循 CPU 模型：Intel 在长模式忽略 `66H`，AMD 保留其 16 位覆盖，REX.W 优先。原生 KVM/WHP 使用宿主处理器，Unicorn 使用默认 Intel Haswell 模型。共享准入规则在产生效果前拒绝 LOCK 和 REP，包括解码元数据省略的前缀。

`X64BranchModel` 让相对 `JMP/Jcc` 的解码与执行一致。原生 KVM/WHP 在有时间上限的初始化中，通过不跳转的 `66H` 分支和 REX.W 优先级探针确定规则；Unicorn 检查模式使用其 Intel 模型。不可变结果同时决定指令观察器和 Windows 驱动策略的解码模式。Intel 在长模式下保留 rel32 近分支和完整目标宽度；AMD 遵守其 16 位覆盖规则。指令字节不完整时，在观察器或 CPU 执行前失败。

`X64StackInstructions.def` 在 KVM、WHP 和 checked Unicorn（含 `driver-strict`）中支持通用寄存器、普通 RAM 的 16/64 位 `PUSH/POP` 及立即数 PUSH。PUSH 先读取源操作数，再减小 RSP；POP 先增加 RSP，再计算使用 RSP/ESP 的目标地址。地址宽度截断只作用于显式操作数。完整范围权限检查、有序观察回调和单次 RAM 事务确保故障、取消或回调错误时 CPU 与内存不被部分更新。LOCK 和设备操作数仍不支持。

KVM、WHP 和 checked Unicorn 也支持 16/64 位 `LEAVE`。它始终通过完整 RBP 读取保存的栈帧，包括带 `67H` 的情况；16 位形式保留 RBP 未选中的位。故障或读取取消时保留原始 RSP 和 CPU 上下文。LOCK、REP 和设备栈帧仍不支持。

KVM、WHP 和 checked Unicorn 支持 16/64 位 `ENTER`，分配量按无符号 16 位解释，嵌套级别取模 32。访问使用完整 RSP/RBP，并遵循有效前缀顺序。访客故障会保留已完成的栈写入，RSP、RBP 和 PC 则保持入口值。最终栈检查覆盖整个操作数宽度的写权限，但不写入数据。LOCK、REP、APX 前缀及设备栈帧仍不支持。

`X64PackedIntegerInstructions.def` 准入 45 条 legacy SSE2 packed integer 指令，涵盖回绕／饱和加减、比较、乘法、平均值、极值、字节差、打包及解包。XMM 和对齐的 128 位 RAM 源操作数在 KVM、WHP、Unicorn 上共用现有 checked 路径。FLAGS 与 MXCSR 保持不变；故障或观察器取消保留状态。MMX、VEX/EVEX 和设备操作数仍不支持。

`X64PackedShiftInstructions.def` 准入十种 legacy SSE2 打包移位。元素移位接受 imm8 或 XMM／对齐的 m128 计数，字节移位仅接受 imm8。变量计数使用无符号低 64 位，不按标量移位规则掩码；高 64 位不参与计算。即使计数为零或超出位宽，内存操作数仍须完整读取 16 字节。FLAGS 和 MXCSR 保持不变；MMX、VEX/EVEX 和设备操作数仍被排除。

`X64VectorOperands.def` 统一定义传统 SSE 搬运、运算、移位、转换和掩码的完整操作数规则。`MOVMSKPS`、`MOVMSKPD` 和 `PMOVMSKB` 从 XMM 提取符号位写入 r32/r64，并清零目标其余位。KVM、WHP 与 checked Unicorn 共用准入规则，保留 FLAGS、MXCSR 和源寄存器；掩码的内存操作数、MMX 和 VEX/EVEX 形式仍不支持。

`X64ShuffleInstructions.def` 新增 `PSHUFD`、`PSHUFHW`、`PSHUFLW`、`SHUFPS` 和 `SHUFPD`。`X64VectorOperands.def` 要求完整的三个操作数：XMM 目标、XMM 或对齐的 m128 源，以及 imm8。原始指令按位选择通道，保留 FLAGS 和 MXCSR；内存形式检查全部 16 字节，对齐故障先于数据观察。KVM、WHP 与 checked Unicorn 共用这些规则。 同一清单还允许恰有两个操作数的 `UNPCKLPS`、`UNPCKHPS`、`UNPCKLPD` 和 `UNPCKHPD`，交错原始目标与源的位模式。硬件可以只读取所选的 64 位；checked RAM 检查对齐的 m128 操作数。

`MOVLPS`、`MOVHPS`、`MOVLPD` 和 `MOVHPD` 精确传输八字节 RAM，无对齐要求。`X64VectorInstructions.def` 声明存储使用的半部；`X64VectorOperands.def` 限定 XMM/m64 操作数对。加载保留另一个 64 位半部，高半部存储观察者接收高半部数据。KVM、WHP 和 checked Unicorn 共享全范围权限检查与 RAM 回滚。仅寄存器形式的 `MOVHLPS`/`MOVLHPS` 保留各自语义。

`CVTSI2SS` 与 `CVTSI2SD` 按 MXCSR 舍入规则转换有符号 32/64 位整数，并保留精度状态。共享的 `IntegerSource` 规则仅允许 XMM 目的及 r32/r64 或 m32/m64 源。传统指令保留目的的高 96/64 位；内存检查采用整数宽度。KVM、WHP 和 checked Unicorn 执行原始指令。MMX 和 VEX/EVEX 仍不支持。

`CVTSS2SI` 与 `CVTSD2SI` 通过共享 `IntegerResult` 规则，按 MXCSR 舍入模式生成有符号 32/64 位整数；`CVTTSS2SI` 与 `CVTTSD2SI` 始终向零截断。屏蔽异常时，NaN 或越界转换返回整数不定值并置无效状态；有效但不精确的结果置精度状态。既有粘滞位、FLAGS 和 XMM 源保持不变。r32 结果清除通用寄存器高半部。RAM 读取采用浮点源宽度，与目的宽度无关；FTZ 不丢弃次正规输入。KVM、WHP 和 checked Unicorn 共用这些规则。

`COMISS`、`COMISD`、`UCOMISS` 和 `UCOMISD` 通过共享 `Source` 规则比较标量 XMM 或 m32/m64 操作数。它们设置 CF/PF/ZF、清除 OF/SF/AF，保留其他 FLAGS 和源通道。COMIS 对任意 NaN 置无效状态，UCOMIS 仅对 signaling NaN 置该状态；NaN 处理优先于次正规状态。MXCSR 粘滞位保留，舍入和 FTZ 不影响比较。KVM、WHP 和 checked Unicorn 共用精确内存检查。固定版本的 Unicorn 比较函数复用其现有次正规输入分类逻辑。

`CMPSS`、`CMPSD`、`CMPPS` 和 `CMPPD` 在 KVM、WHP 和 checked Unicorn 上执行八种传统比较条件。共享 `Source` 规则接纳解码后的条件别名，保留控制值仍不支持。标量形式保留高位通道并读取 m32/m64，向量形式要求对齐的 m128。FLAGS 和已有 MXCSR 状态保留，无效及次正规状态按有效通道累积。Capstone 统一负责指令族 ID 和 SSE 条件，取代仅在 lifter 内修正身份的逻辑；Unicorn 在各比较函数内部分类次正规输入。

`CVTSS2SD`、`CVTSD2SS`、`CVTPS2PD` 和 `CVTPD2PS` 通过共享 `Source` 规则转换传统 SSE 精度。标量结果保留目标高 64/96 位；打包扩宽读取 m64 并写入两个双精度数，打包缩窄读取对齐的 m128、写入两个单精度数并清零高 64 位。KVM、WHP 和 checked Unicorn 执行原始指令，保留 FLAGS，并按舍入与 FTZ 控制累积已屏蔽异常的 MXCSR 状态。Unicorn 在转换函数中逐个分类有效次正规输入。VEX/EVEX 仍不支持。

`CVTDQ2PS` 和 `CVTDQ2PD` 通过共享 `Source` 规则转换打包的有符号 32 位整数。单精度读取对齐的 m128 并使用 MXCSR 舍入；双精度读取可未对齐的 m64，结果精确。目标 XMM 全部位被替换，FLAGS 和已有 MXCSR 状态保留，非精确单精度结果累积精度状态。KVM、WHP 和 checked Unicorn 执行原始指令。Unicorn 在选择八字节读取前识别两种打包扩宽转换。MMX 和 VEX/EVEX 仍不支持。

`CVTPS2DQ` 和 `CVTPD2DQ` 使用 MXCSR 舍入；`CVTTPS2DQ` 和 `CVTTPD2DQ` 向零截断。共享 `Source` 规则要求对齐的 m128 或 XMM 输入。NaN 或越界通道产生 signed32 indefinite 并设置无效状态；其他有效但非精确通道独立累积精度状态。单精度输入产生四个整数，双精度输入产生两个并清零目标高 64 位。FLAGS 和已有 MXCSR 状态保留；FTZ 不丢弃次正规输入。KVM、WHP 和 checked Unicorn 执行原始指令。MMX 和 VEX/EVEX 仍不支持。

`X64AlignmentTests.cpp` 验证已准入 aligned SSE 指令的未对齐操作数在数据观察器、权限检查或设备回调之前报告可恢复或终止性的 `#GP(0)`。故障保留完整公开 x64 寄存器上下文、PC 和 RAM；地址宽度回绕先于 FS/GS 基址相加，修复地址后重试原指令。直接 KVM/WHP 机器测试独立验证硬件边界。Windows ring3 已派发明确分类的 `operand_alignment` 故障；其他原因的 `#GP` 仍不支持。

线程指针包括 x64 FS/GS 基址以及 ARM64 `TPIDR_EL0` 的精确 `MRS`/`MSR` 编码。原生传输和 CPU 快照独立于内存保存这些状态，但不会创建 OS 线程或分配 TLS 块。supervisor x64 支持对齐的 1/2/4 字节标量 MMIO 事务，以及每个重启边界一个 MOVS 元素。设备读取需先提供无副作用的预览，再至多提交一次。user 配置拒绝设备映射；RMW、宽 MMIO 和端口 I/O 仍不支持。

KVM/WHP 会取消正在运行的原生入口，并在释放执行资源前确认取消。KVM 使用专用执行线程和临时解除屏蔽的 realtime 信号；入口执行期间所选信号不得被忽略。不会修改调用方的 signal mask 或 handler。若来宾进度不确定，取消会成为终止性后端错误；不保证硬性墙钟期限。

## x64 原生同步异常

checked x64 的 `DIV`/`IDIV` 使用处理器产生的结果和 `#DE`。KVM 通过私有 supervisor IDT/IST 接收异常，WHP 使用明确的异常拦截位图；异常保留原始上下文和可用的错误码，与后端传输错误分开。OS 模型必须先消费可恢复事件，再安装明确的继续执行上下文。Windows 驱动将零除及商溢出映射为 `STATUS_INTEGER_DIVIDE_BY_ZERO`，并执行实际 SEH filter、`__finally` 和重试。`NeverDX64ExceptionTests` 可在禁用 Unicorn 时构建；原始 WDK 用例由 `DriverWDMCPUException` 验证。缺少的 ARM64 主机覆盖会明确跳过。

## 分阶段提交 RAM 效果

`RAMTransaction` 在物理执行租约内，只保存一条指令明确声明的写入范围的物理并集。结果观察器运行前恢复原始 RAM；取消、后端传输错误和观察器异常不会发布部分 RAM 或寄存器。CPU 异常在 RAM 回滚后保留架构异常状态。ARM64 的单次和成对写入共用该内存权威层。x64 支持 8/16/32/64 位 `XCHG`、`XADD`、`CMPXCHG`，LOCK 或隐式锁定形式要求自然对齐。`NeverDRAMTransactionTests` 将结果与独立宿主 CPU 对照，并验证回滚、别名和权限；不可用的平台明确跳过。设备原子事务使用独立提供方；CPU 快照不会撤销已提交的 RAM。

`CMPXCHG8B` 和 `CMPXCHG16B` 在 KVM、WHP 和 checked Unicorn 的驱动及用户模式中执行原始指令。比较成功或失败都需要读写权限，故障按写访问分类。`CMPXCHG16B` 在访问内存前检查 16 字节对齐，不满足时报告 `#GP(0)`。两次结果观察属于同一 RAM 事务；任一次停止或抛出异常，都不会发布寄存器或内存变化。未加锁的 `CMPXCHG8B` 可以跨页，加锁操作仍要求自然对齐。`X64WideAtomicTests.cpp` 对照宿主机原始执行结果和直接原生故障，并检查别名、前缀、寻址、修复重试和取消。原创 Windows 驱动及 ring3 PE 样例覆盖两种宽度，WDK 样例还执行 `_InterlockedCompareExchange128`。CPU 模型必须支持 `CMPXCHG16B`。

## 完整 x87 状态

`NeverDEmulationArch` 独立负责 ISA、页表及 FP 状态布局，原生与 Unicorn 传输共用该层。x64 上下文保存 x87 控制、状态、TOP、物理标签、操作码、指令／数据指针和八个 80 位寄存器。`FP0`–`FP7` 使用 `RegisterValue`，标量访问拒绝截断；`FPTag` 是物理非空位图。`NeverDX64FPTests` 覆盖全部 TOP、精确运算的宿主 FXSAVE/FXRSTOR 对照及上下文恢复。这不新增 checked x87 指令，也不证明全部舍入语义；缺少原生主机时明确跳过。

`driver-strict` 支持匹配的 Linux x64 主机上的 KVM 和 Windows x64 主机上的 WHP；`auto` 选择对应原生传输，跨 ISA 执行选择 Unicorn。显式 Unicorn 和原有 V1 API 保留可移植软件配置。原生执行在进入 CPU 前检查规范地址和指令效果；硬件不可用时明确失败且不回退。未支持的指令及 OS 行为仍明确报错。Windows x64 原生 CI 在关闭 Unicorn 的配置下通过全部 359 项必跑检查：131 项 CPU 检查、26 个内置映像与 46 个 WDK 映像及 40 个场景组合在首选和重定位地址产生的 224 项驱动结果，以及 4 项 SEH 边界检查 ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). 原生 ARM64 的实机证据仍待补充，这不表示兼容任意驱动或 Android/Darwin 环境。

使用 `executionCapabilities(Contract, ISA, Backend)` 查询所选后端的能力配置。`NativeLegacyX64` 描述原生 x64 驱动执行；`NeverDNativeDriverTests` 验证原有驱动集，也可在关闭 Unicorn 的构建中运行。

Checked ARM64 使用统一的完整状态提交边界。`Registers.def` 定义 39 个标量字段及 32 个 128 位向量寄存器；`captureAArch64State` 暂存全部读取、应用声明位宽及 NZCV 规范化，最后一次提交。Unicorn、KVM、WHP 和 HVF 传递相同清单，包括 TPIDR_EL0、TPIDRRO_EL0、TPIDR_EL1、FPCR 和 FPSR。原生适配器通过 CPACR_EL1 开启 FP/SIMD 访问。任一标量或向量读取失败、进入取消，都会保留完整调用方状态。

ARM64 KVM/WHP/HVF 初始化执行私有 `AArch64MachineProbe.def` 程序：NOP、向正无穷舍入的 FP32 加法和双通道 SIMD 加法。每步比较全部 39 个标量字段与 32 个向量，包括 TLS、NZCV、目标寄存器高位清零及保留和累积的 FPCR/FPSR 状态。自检只使用特权级监控存储，共享一个总截止时间。自检仅证明有界初始化。Linux ARM64 KVM 和 Windows ARM64 WHP 的工作负载验证仍待完成；macOS 原生结果记录于 [HVF 指南](macos-hvf.md)。 程序还包括密钥关闭时的 A/B 返回地址签名和认证，以及非防护页上的四种 BTI 指令。

自检还执行两次 `MRS CTR_EL0`，以及 `DC CVAU`、`DSB ISH`、`IC IVAU` 和 `ISB`，核对缓存几何信息稳定及完整状态。Checked EL0/EL1 接纳原始指令、所有具名基线 DSB 选项及 ISB SY。CTR 来自选定虚拟 CPU，不同传输可以不同。缓存目标必须在当前权限下指向可读普通 RAM，允许非对齐地址和别名；其他目标明确报未支持。维护操作不产生数据读写观察事件。投影保证指令执行的一致性，不模拟私有缓存内容或并行硬件 SMP。`NeverDAArch64CacheTests` 检查完整状态、只读页尾、拒绝项、停止、上下文、预算，以及通过跨页 RW/RX 别名更新 guest 代码；不可用的 KVM/WHP 主机明确跳过。

x64 KVM/WHP/HVF 原生初始化在私有 supervisor 页面执行 `X64MachineProbe.def`。一个时限覆盖 NOP、向正无穷舍入的 FP32 加法、双通道 SIMD 加法、FS/GS 加载及 CS/SS/CR8 读取；每一步比较完整的标量、XMM、物理 x87 和控制状态。x64 与 ARM64 自检都必须取得物理内存的独占执行租约。`MemoryProjection` 统一保存缓存身份（ISA、地址空间、映射代次、权限及监控变体）和各 ISA 已提交的页表根历史。构建器在改写私有字节前使缓存失效；失败的重建不能复用部分写入的页表，调用者也不能传入过期页表根。自检仅证明有界初始化。Linux ARM64 KVM 和 Windows ARM64 WHP 的工作负载验证仍待完成；macOS 原生结果记录于 [HVF 指南](macos-hvf.md)。

共享 XSAVE 解码器区分标准格式与压缩格式的 SSE 初始状态。XSTATE_BV[1] 清零时，两种格式都初始化 XMM 寄存器；标准格式仍读取并校验 MXCSR，压缩格式才初始化 MXCSR。`X64XsaveCases.def` 提供独立的数据布局和原创主机 XRSTOR 程序。`X64XsaveTests.cpp` 检查拒绝状态的原子性，并以真实主机执行对照两种格式，同时保留调用方 FP/SSE 状态。主机架构或所需指令功能不可用时，对照测试明确跳过。

`X64FPState.def` 声明压缩 AVX、AVX-512、CET_U/CET_S 和 AMX 传输布局，包括分量的 64 字节对齐。存在的扩展数据必须符合架构的全零初始状态；缺席分量的数据与对齐填充不定义状态。偏移由布局位决定，未知布局、非初始数据或错误长度会在发布前失败。`CompactedOffsetsFollowLayoutRatherThanPresentBits`、`WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`、`InitialCETComponentsDoNotHideFPState` 和 `InitialWideComponentsDoNotHideFPState` 覆盖 872 字节及 10752 字节 WHP 数据包。这项传输支持不准入上述扩展指令。

`WhpXsaveRegisters.def` 使用具名 x87/SSE 控制寄存器补充完整 XSAVE 数据包。最后操作码及指令/数据地址会显式写入并从主机回读；可补齐数据包中的零值字段，但非零元数据冲突或共有控制字段不一致时，会在发布状态前失败。`NamedMetadataRestoresOmittedPacketFields` 验证字段缺失场景，并保留完整 FP 数据。

原生 `FOP/FIP/FDP` 遵循宿主 x87 保存、恢复规则。没有未屏蔽的待处理异常时，AMD 可能清零这些字段；快照保留实际观测值。`X64MachineProbe.def` 与精确 NOP/上下文测试使用一致的待处理异常状态，确保每个字段有效并逐项比较，不屏蔽差异。宿主进程 FXRSTOR64/FXSAVE64 参考程序覆盖两种状态；后端不会用输入元数据替代宿主结果。

共享的 `encodeX64XsaveState` / `decodeX64XsaveState` 编解码层拥有标准及压缩 FP/SSE 数据包、物理 TOP 轮转、缺失组件的初始状态和原子校验。WHP 使用完整 XSAVE API，优先选择 `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState`，旧 XSAVE API 作为兼容路径。单独的旧 x87 寄存器接口不能替代完整数据包。非初始扩展组件、畸形头部、非法控制位和截断捕获明确失败。WHP 映射错误保留 HRESULT、GPA 和大小以便诊断。

`CheckedX64Instructions.def` 通过既有 CPU 后端准入 8/16/32/64 位无符号 `MUL` 和 `CBW/CWDE/CDQE/CWD/CDQ/CQO`。`NeverDX64IntegerTests` 使用独立的 `X64IntegerCases.def` 编码和预期值，在两种特权级验证部分寄存器保留、32 位零扩展、乘积高低两部分、已定义的 CF/OF 结果及符号扩展不改变标志位。普通 RAM 乘法保留完整访问范围的权限检查和读观察回调；故障或观察回调中止会保留隐式输出寄存器及 PC。设备操作数仍不支持。这些用例也在 checked Unicorn 上运行；不可用的原生后端明确跳过。

`X64BitInstructions.def` 支持 16/32/64 位寄存器及普通 RAM 的 `BT/BTS/BTR/BTC`。寄存器位索引按操作数宽度解释为有符号数并选中完整数据字；立即数索引限制在基址的数据字内。地址宽度截断先于 FS/GS 基址相加。CF 与写入值由处理器提供；`RAMTransaction` 在观察回调接受前保留私有执行结果。完整范围权限检查覆盖独立页面分配和别名。停止、回调失败或页面权限不足均保留原始 CPU 和 RAM。LOCK 要求自然对齐；修改型 MMIO 操作要求明确的预备原子事务提供方。`X64BitStringTests.cpp` 使用独立编码与 x64 本机实际执行对照，检查负索引、宽度截断、跨页访问、取消及非法 LOCK 形式。参见 [Intel 指令参考](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)。

`X64StringInstructions.def` 统一管理普通 RAM 上 8/16/32/64 位的 `MOVS/STOS/LODS`；`CLD/STD` 只改变方向标志。每个 REP 元素在观察回调前验证整个操作数，并在一个可恢复边界提交。后续故障保留此前完成的元素；取消或回调异常不改变当前元素。FS/GS 仅作用于源地址，且在地址宽度截断之后相加。AL/AX 加载保留高位，EAX 加载零扩展。32 位地址模式的零次 REP 要求计数高位为零，MOVS/STOS 还要求参与的地址寄存器高位为零，否则不同真实 CPU 实现会产生不同结果。MOVS/STOS/LODS 的 REPNE 形式及 STOS/LODS 设备操作数仍不支持。`X64StringTransferTests.cpp` 用独立的主机指令对照宽度、方向、重叠和零次数，并分别检查权限、别名、回绕、故障和恢复。原创 WDK 资源驱动通过 `driver_resource_strings.def` 执行四种宽度的 STOS/LODS。

`X64StringInstructions.def` 还统一管理普通 RAM 上 8/16/32/64 位的 `CMPS/SCAS` 及 `REPE/REPNE`。每个元素在观察回调前验证全部读取操作数，更新六个算术标志，并在首次满足终止条件时退出。数据故障恢复本次连续 REP 执行开始时的标志，同时保留已完成的指针和计数更新；公开接口恢复执行时，以已发布的 CPU 状态重新开始。停止和观察回调异常不改变当前元素，提前终止也不会读取下一个元素。FS/GS 仅影响 CMPS 源地址；SCAS 保留累加器和未使用的源寄存器。设备操作数及有歧义的 32 位零次数高位状态仍不支持。`X64StringComparisonTests.cpp` 用独立主机指令对照标志、方向、别名、回绕、权限和恢复，并通过 Linux x64 信号测试读取真实故障时的寄存器。原创 WDK 资源驱动通过 `driver_resource_strings.def` 执行四种宽度的两类条件重复形式。参见 [Intel 指令参考](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)。 Linux 原生验证覆盖首元素执行前后发生的故障，并区分 Intel 恢复入口 flags 与 Hyper-V 下 AMD EPYC 7763 保留最后一次比较 flags 的行为（[原生观测](https://github.com/NeverSight/NeverD/actions/runs/37202522130)）；未知 CPU 厂商会明确失败。所有后端的 checked 来宾仍统一恢复入口 flags。

默认协作模式：`WhpResourceCache.h` 将逻辑 CPU 状态与 WHP 分区分离。运行时保留一个活动原生分区：同一 CPU 连续单步复用它；切换 CPU 时先销毁旧分区，再重建映射、虚拟处理器并恢复完整状态。逻辑 CPU 保留独立的 `MemoryProjection` 视图和权威 RAM。获取租约遵守取消信号和当前截止时间；销毁非活动 CPU 不会销毁其他 CPU 的分区。x64 保留宿主默认 XSAVE 特性组合，并通过 `WHvGetPartitionProperty` 验证实际分区，不通过清除依赖特性强制缩减掩码。CPU 协作式切换不提供并行硬件 SMP。

`CheckedBackend` 为每个 CPU 保留一块取指缓冲区和一条 `cs_disasm_iter` 指令记录。每一步重新读取有执行权限的字节并解码；代码写入、别名变更或恢复运行后不会复用旧的解码结果。执行租约在接触复用存储前拒绝递归执行。这样可消除逐指令的缓冲区和记录分配，同时保留指令观察、系统服务拦截和精确故障处理。 固定版本的 Unicorn 单步路径在读取后继指令前结束，包含间接翻译查找路径，并且不会把内部代码写入重试计为已完成指令。

`WhpX64Partition.h` 将 x64 WHP 寄存器复用状态归属到实际分区。固定数据包采用 `WhpX64Registers.def` 和 `X64HostRegisters.def`；每个成功单步仍完整读取通用、控制、段寄存器和 FP/SSE 状态。只有完全确认的调试退出才能省略未变化的输入，比较忽略保留位和联合体填充。变化的 CR3、CPL、TLS、通用或 FP 输入会重新安装；部分失败、取消及异常使复用失效。分区重建从全量安装开始。这减少重复传输，不扩大指令准入，也不宣称端到端提速。

WHP 将 `WhpXsaveRegisters.def` 中的 x87/SSE 元数据与普通寄存器放入同一次 `WHvGetVirtualProcessorRegisters` 调用。停止的 vCPU 始终由同一个分区租约保护。发布前仍须完整捕获 XSAVE 并核对全部元数据一致性；每步减少一次宿主 API 调用，不据此宣称吞吐提升。

`CheckedAArch64Instructions.def` 和 `AArch64InstructionEffects` 在 EL0/EL1 接纳有界的基础 FP32/FP64 算术、比较、移动和定宽 SIMD 运算。FPCR 支持四种舍入模式、FZ 和 DN；FPSR 保留累积状态及 QC。未支持的控制位和状态位在修改前拒绝。FP16 算术、SVE/SME、未屏蔽异常、可选扩展及未列出的形式明确失败。这些 CPU 能力不代表已经支持 Windows ARM64 驱动加载或新增 OS 环境。

`AArch64InstructionEffects` 负责标量及 FP/SIMD 的单次、成对 RAM 访问范围，单个操作数最大 128 位。共享地址空间在进入 CPU 前验证每一页；`RAMTransaction` 只提交完整声明的物理写入。128 位写观察器在生效前按顺序收到两个 64 位字。停止和故障保留 RAM、向量和地址写回。Xn/Vn 的编号重叠合法；发生地址回绕的成对访问被拒绝。`NeverDAArch64MemoryTests` 使用独立的 `AArch64CrossPageCases.def` 与 `AArch64VectorMemoryCases.def` 编码。

KVM x64/ARM64 通过 `KvmRunControl` 在同一专用 vCPU 工作线程上准备状态、进入 `KVM_RUN` 和读取状态。`EINTR` 重试只准备一次；取消进入或读取失败不能发布状态。`KvmAArch64Machine.cpp` 在该线程上执行地址转换维护及完整标量、向量传递，并共用一次单步期限。调用线程只在确认完成后提交；ISA 解码、RAM 事务、OS 策略和观察器仍属于调用线程。ARM64 原生运行仍缺少实机证据。

KVM 根据 `X64HostRegisters.def` 和 `X64FPState.def` 将通用寄存器及完整 FP/SSE 状态与上次确认完成的调试退出状态比较，只重新安装变化的输入。宿主写入和上下文恢复也参与比较；异常、取消及失败会使复用失效。每条指令仍启用单步并读取真实的通用及 FP 状态。

x64 KVM 查询 `KVM_CAP_SYNC_REGS`，分别启用主机支持的 `KVM_SYNC_X86_REGS` 和 `KVM_SYNC_X86_SREGS` 捕获集合。已确认完成的 `KVM_RUN` 通过共享区返回真实寄存器；连续成功单步可省去 `KVM_GET_REGS` 和 `KVM_GET_SREGS`。未支持的集合或可选查询失败保留 ioctl 路径。变化的输入仍在 `KVM_SET_GUEST_DEBUG` 前安装，共享脏位保持清零。异常、取消及捕获失败使复用失效。完整 FP/SSE 捕获仍必需，未变化的 FP 输入免去重复 XSAVE 编码。这减少传输调用，但不代表已证明端到端提速。[KVM API](https://docs.kernel.org/virt/kvm/api.html#kvm-cap-sync-regs)。

硬件执行本身不保证更低的端到端耗时。当前原生执行逐条进行指令准入、观察、状态传输和 VM 退出。比较相同原始镜像与场景时，应使用一致的指令和事件预算，同时报告结果一致性与耗时；测量 CLI 延迟时应包含启动和加载。

checked Unicorn 使用 `MachineRunControl`：ARM64 维护、来宾执行和完整状态回读共用一次单步额度。`UC_HOOK_CODE` 在指令入口检查借用的停止令牌和期限；同步引擎调用返回前解除 hook 借用，而机器单步保留控制直到发布状态。Unicorn 与 WHP 暂存完整 CPU 状态，并在成功步骤发布前检查同一个控制条件。WHP 在准备前只创建一次额度。已确认的 x64 CPU 异常优先于回读期间到来的停止请求。回读取消时，checked RAM 事务丢弃推测写入；非受限软件契约不变。 `MachineInterruptedError` 区分已确认取消与主机或回读失败。共享 checked CPU 返回 `Stopped` 或 `Deadline`，保留 CPU/RAM 并允许重试；真实故障即使伴随停止请求也仍是 `BackendFailure`。

`RunDeadline::invoke` 在 WHP 入口已停止或过期时拒绝调用宿主，取消期间保留真实宿主结果，并在释放借用的停止标记前确认中断回调结束。KVM 和 WHP 在持有执行租约的调用线程上验证完整捕获的私有状态，然后分类同时到达的停止或超时。真实宿主错误、捕获失败以及经过认证的 x64 CPU 异常保持更高优先级。普通成功状态在取消检查结束前保持私有；已确认的中断丢弃推测性的 CPU/RAM 效果并允许重试。准备、原生执行和捕获共用一次单步宽限。这些控制提供协作式取消，不保证硬性墙钟时限。

## macOS HVF

`hvf` · Hypervisor.framework · Apple Silicon → ARM64 · Intel Mac → x86-64.

[配置、签名与硬件验证](macos-hvf.md)

checked ARM64 支持 8/16/32/64 位 `LDXR/STXR`、32/64 位寄存器对 `LDXP/STXP`、相应的 acquire/release 形式及 `CLREX`。KVM、WHP 和 checked Unicorn 共用 ISA 层的独占监视器，以 16 字节物理范围记录保留状态，传输层单步退出不会破坏循环进展。已提交的写入即使没有改变字节，也会使保留状态失效；别名和保留视图写入遵循同一规则。停止保留尚未发布的状态，快照不能撤销期间发生的写入。checked 独占指令采用 FEAT_LSE2 对齐规则：操作数可在同一个 16 字节对齐块内未对齐访问，跨越该块才产生 `alignment` 故障。同宽条件存储按保留的物理粒度匹配；即使监视器已失效，也先检查对齐和权限，再决定条件存储是否成功。独占指令仍只支持普通 RAM。 ARM64 Unicorn 的 `Software` 契约也使用此监视器，包括软件和 checked CPU 共用物理 RAM 的情况。 Unicorn 的 `Software` 配置保留引擎的自然对齐模型。


同一 ISA 层支持 FEAT_LSE `CAS/CASP`、`SWP`、`LDADD/LDCLR/LDEOR/LDSET` 及有符号/无符号 min/max，包括字节、半字、字、双字和 acquire/release 形式。checked 配置采用上述对齐策略，Unicorn `Software` 保留自然对齐。比较按操作数宽度执行，返回的旧值零扩展。CAS 比较失败仍要求写权限，并选择 Arm 允许的旧值回写行为，使物理保留状态失效。回调看到提交前的 CPU/RAM 状态；取消和同步故障不会发布部分原子操作结果。并行 CPU 与 MMIO 原子事务遵循并发提交契约。`MRS/MSR NZCV` 按架构规则传输四个条件标志，正确处理保留位和零寄存器。 先检查读权限，再检查写权限：不可读操作数报告读故障，只读操作数报告写故障，与原始 Windows ARM64 观测一致。
