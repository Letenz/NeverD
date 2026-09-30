**语言**：[English](../cpu-execution.md) | [简体中文](cpu-execution.md) | [繁體中文](../zh-TW/cpu-execution.md) | [日本語](../ja/cpu-execution.md) | [한국어](../ko/cpu-execution.md) | [Français](../fr/cpu-execution.md) | [Deutsch](../de/cpu-execution.md) | [Español](../es/cpu-execution.md) | [Italiano](../it/cpu-execution.md) | [Русский](../ru/cpu-execution.md) | [العربية](../ar/cpu-execution.md)

[← 文档索引](README.md)

# CPU 执行与能力查询

CPU 执行独立于来宾 OS、映像加载器和调用约定。启用 `NEVERD_ENABLE_CPU_EMULATION` 可单独构建；`NEVERD_ENABLE_DRIVER_EMULATION` 也会包含 Windows 驱动环境。[架构指南](architecture.md)说明所有权、后端选择和当前平台限制。

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

`driver-strict` 支持 x64；`software-cpu-v1` 支持 x64 与 ARM64。`checked-x64-v1` 与 `checked-aarch64-v1` 要求对应架构并以 supervisor 特权运行。`checked-user-x64-v1` 与 `checked-user-aarch64-v1` 分别在 CPL3 和 EL0 执行同一有界标量指令集，具备 MMU 隔离和显式服务请求退出。支持 Unicorn 以及与主机匹配的 KVM/WHP；`auto` 遵循现有主机选择。flat 配置不承诺架构级 user/supervisor MMU 隔离。所有 checked 配置均拒绝 FP/SIMD、MMIO、端口 I/O 和并行 CPU 要求。只有 user 配置会报告 `service_traps`。

用户态执行要求**每个**映射页同时具有 `UserAccessible` 和相应的 `Read`、`Write` 或 `Execute` 权限。已有映射默认仅 supervisor 可访问；即使别名共享物理字节，其权限仍彼此独立。仅有 `UserAccessible` 不会授权访问。可信宿主操作和 supervisor CPU 使用 RWX。例如：

```cpp
Configuration.Contract = ExecutionContract::CheckedUserX64;
Configuration.Privilege = ExecutionPrivilege::User;
auto CPU = llvm::cantFail(createExecutionBackend(Configuration, Space)).CPU;
llvm::cantFail(CPU->map(Code, 4096, Read | Write | Execute | UserAccessible));
```

特权由契约固定。恢复上下文或绑定地址空间不会改变特权，写 x64 段选择子也不能提权。可恢复的数据访问故障会保留原指令和寄存器，直到所属模型处理。`canAccess` 严格检查请求的权限；查询 user 可见性时需包含 `UserAccessible`。页表是 CPU 私有投影，不开放可修改的来宾页表或特权切换 API；ARM64 的 user 页在 EL1 也不可执行。

未知字段、null 字段值、无效名称或数值宽度、重复功能及不支持的组合都会失败。输入上限为 64 KiB；旧 CPU 工厂和驱动 C 选项保持兼容。

## 不执行工作负载的能力查询

```bash
neverd cpu-capabilities
neverd cpu-capabilities \
  --configuration='{"contract":"checked-aarch64-v1","architecture":"aarch64"}' \
  --probe-host
```

schema 版本为 1。报告分别给出补全配置前的 `requested_configuration`、规范化后的 `configuration`、静态语义 `capabilities`、适配器构建与 ABI 兼容性的 `build`，以及仅在请求 `--probe-host` 时提供的 `host`（否则为 null）。主机探测会在私有 RAM 上初始化临时 CPU，只证明可初始化，不证明某个工作负载可运行或具有原生 ARM64 执行能力。可用性可能变化；不可用后端不会被静默替换。合法报告即使说明后端不可用，CLI 仍返回 0；配置或查询无效时返回 1。

## SDK 与 C++ 边界

[`neverd_cpu_capabilities_json`](../../include/neverd/sdk/NeverDCAPICPU.h) 接收现有 session、可选配置 JSON，以及取值为 0 或 1 的 `ProbeHost`，不需要加载映像。用 `neverd_free_string` 释放结果；返回 NULL 表示错误，可通过 `neverd_last_error` 查询。禁用 CPU 的构建仍导出同一函数并明确报告功能已禁用。Python 插件使用 `session.cpu_capabilities(...)`。C++ 可分别使用 `executionCapabilities`、`resolveExecutionConfiguration`、`queryExecutionBackendBuild` 和 `probeExecutionBackend`；`createExecutionBackend` 将 CPU 附加到已有地址空间，或创建私有 RAM 和默认空间。

## CPU 结果与预算

`CPU.runUntilExit(PC, TimeoutMicroseconds)` 返回带类型的 [`ExecutionExit`](../../include/neverd/emulation/ExecutionExit.h)。开始执行前的配置错误返回 `llvm::Error`；执行开始后会明确报告停止、deadline、服务请求、可恢复故障、来宾故障／trap、不支持的操作、设备／后端故障或无法解释的引擎停止。CPU、设备或后端故障优先于同时发生的 stop/deadline，但会保留独立事实和故障详情。timeout 必须为正数，且能表示为时长和绝对 deadline；否则会在修改 CPU 前失败。每次运行都必须使用有限预算；0 既不表示无限，也不是有效的立即超时。控制是协作式的，不保证硬实时上限。结果不会消费可恢复故障；OS 所属模型必须先取走故障并安装已验证的异常转移，再恢复执行。旧 `run`、`fault`、`timedOut` API 仍可用；只覆盖 `run` 的外部 CPU 实现会拒绝新的类型化边界。

## 服务请求

checked user x64 只拦截精确的无前缀 `SYSCALL` 编码；checked user ARM64 拦截 `SVC #imm16`。`SYSENTER`、`INT`、`HVC`、`BRK` 等机制仍不支持。先运行指令观察器；若它未停止或产生故障，CPU 会在执行服务指令或进入后端**之前**返回 `ExecutionExitKind::ServiceRequest`，并携带指令类型、原始 `PC`、顺序 `NextPC` 和 SVC immediate。寄存器、flags、栈和特权均不改变：x64 尚未发生 SYSCALL 对 RCX/R11 的改写，ARM64 也尚未进入异常向量。SVC immediate 不是通用服务编号。

请求会保持 pending，并阻止后续执行、CPU 修改、地址空间绑定及上下文捕获／恢复；CPU 停止时由 OS 所属模型通过 `takeServiceRequest()` 恰好消费一次。该模型负责解码 OS ABI、处理服务，并显式选择结果寄存器和下一 PC／异常转移。不支持的服务必须在该边界明确失败；重试原始 PC 会产生新请求，不会隐式变成 NOP 或成功返回。服务事件优先于同时发生的 stop/deadline，但来宾或后端故障优先级更高。CPU 停止、软件 HLT、deadline 或 trap 均不能证明工作负载成功。单独的 [Linux 进程配置](process-emulation.md)使用自己的 OS 模型，不证明 Windows、Android 或 Darwin 可运行。Windows 与 ARM64 原生执行仍需要实际运行验证。
