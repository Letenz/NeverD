**语言**：[English](../process-emulation.md) | [简体中文](process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← 文档索引](README.md)

# 来宾进程模拟

`neverd emulate` 在明确指定的来宾 OS 配置下执行映像。CPU 传输、映像解析、进程入口和 OS 服务分别由不同层负责。启用 `NEVERD_ENABLE_CPU_EMULATION=ON`；驱动模拟也会启用它。

首个配置 `linux-elf64-v1` 在 CPL3 或 EL0 运行 x64/AArch64 ELF `ET_EXEC` 与可自重定位的静态 PIE `ET_DYN`。它加载真实 ELF 分段，构造初始栈，按指令量恢复执行，并处理显式 Linux 系统调用请求。这是独立进程模型，不是完整 Linux 发行版，也不承诺运行任意 libc 二进制。动态链接、信号、线程、文件系统和不支持的服务都会明确失败。x64 配置允许少量受限 SSE/SSE2 形式；AArch64 仍为整数指令配置。Windows、Android、Darwin 和其他内核工作负载属于独立工作。

## CLI 与 SDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

匹配的 Linux 主机选择 KVM，匹配的 Windows 主机选择 WHP；其他主机／来宾 ISA 组合使用 Unicorn。所选后端不可用时会报错，不会静默回退。即使在 Windows 上运行，ELF 仍使用 Linux 进程模型。CPU 指令范围与限制见[CPU 执行](cpu-execution.md)。

CLI 输出一份 JSON 报告。来宾退出状态为 0 时 CLI 返回 0，其他来宾状态返回 2，执行不完整（包括故障和资源限制）返回 3，设置/API 错误返回 1。实际来宾状态位于 `exit_status`。新增 C 入口 [`neverd_emulate_process_json`](../../include/neverd/sdk/NeverDCAPIProcess.h) 接收 session、非空输入路径、明确配置和可选 options JSON。使用 `neverd_free_string` 释放结果；NULL 表示设置失败，可通过 `neverd_last_error` 查询。来宾故障或资源停止会返回报告。session 中已加载的分析映像既非必需，也不会被修改。

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

## 选项与结果

选项为不超过 64 KiB 的 JSON object。未知或 null 字段、类型无效、字符串内含 NUL、非正数限制都会被拒绝。

| 选项 | 默认值 | 约定 |
|---|---|---|
| `backend` | `auto` | `auto`、`unicorn`、`kvm` 或 `whp` |
| `arguments` | 输入文件名 | 完整 argv，包含 argv[0]；空值使用默认值 |
| `environment` | `[]` | 显式来宾字符串；不继承宿主环境 |
| `instruction_limit` | 100000 | 共享的已接纳指令尝试次数 |
| `event_limit` | 10000 | system-call 事件数，在 OS 服务处理前计费 |
| `timeout_microseconds` | 5000000 | 进程设置后开始的单调 deadline |
| `memory_limit` | 67108864 | 物理／映射内存预算 |
| `stack_size` | 1048576 | 预算内按页对齐的栈 |
| `output_limit` | 1048576 | 捕获的 stdout/stderr 总字节数 |
| `instruction_quantum` | 1024 | 让出给 runtime 前的接纳间隔 |

`schema_version` 为 1。结果含 profile、架构、所选后端与原因、`stop_reason`、可空的 `exit_status`、诊断、入口／当前 PC、计数器、服务记录和最后一个类型化 CPU 退出。地址、syscall 编号、参数寄存器及原始返回位均为**不带** `0x` 的十六进制字符串；`stdout_hex`／`stderr_hex` 保留 NUL 和无效 UTF-8。syscall 结果为 null 表示没有建模返回值（例如退出或不支持的请求），不表示成功返回 0。

## Linux 配置语义

OS 策略复用现有 ELF 加载器解析出的 program headers。它验证 ABI 标签、segment 对齐、已映射的 program-header 表及用户地址边界。通用映射计划会在分配前检查范围、权限、重叠和预算，只发布完全准备好的私有地址空间。保留文件页前缀／尾部字节，将 BSS 清零，遵循 segment 权限并为栈保留 guard gaps。页重叠布局和矛盾 header 会被拒绝，不会猜测。

初始栈含对齐的 argc/argv/envp/auxv、PHDR/PHENT/PHNUM、entry、页大小和身份值。模型 PID/TID/UID/GID 均为 1000。为保证可复现，`AT_RANDOM` 是输入 SHA-256 的前 16 字节；这是确定性的模型策略，不是加密熵。HWCAP/HWCAP2 均为 0，没有 vDSO。

已实现的调用为 `write`、`exit`、`exit_group`、`getpid`、`gettid`，其编号分别遵循 [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) 与 [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h)。x64 SYSCALL 返回时会应用 RCX/R11 clobber，同时设置 RAX 与下一 PC；ARM64 使用 x8 作为编号、x0 作为结果。未知调用会以 `unsupported_service` 停止，不会执行宿主 syscall。

文件描述符 1 和 2 是虚拟字节 sink。`write` 验证可读 user pages；后续页不可读时返回已读前缀，没有任何字节可读时返回来宾 `EFAULT`。无效描述符返回 `EBADF`；对有效描述符写入零字节不会访问指针。这不模拟 Linux pipe 原子性或文件对象。若输出超过限制，会在发布写入前停止。

## 验证

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# 在构建共享库／CLI 时：
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

测试为两种 ISA 编译原始 ELF 入口 assembly 和 C，检查 data/BSS、启动元数据、syscall 错误、二进制输出、权限故障、部分写入、不支持的服务及跨执行量预算保持。不可用后端会明确标记为跳过。公开套件通过 C ABI/CLI 并核对报告与退出码。交叉编译和 Unicorn ARM64 不构成原生 ARM64 KVM/WHP 证据。

## 静态 PIE、TLS 与验证

静态 PIE 使用不低于 `0x40000000` 的确定性 load bias，并按较大的 `PT_LOAD` 对齐要求递增。所有映射段、入口 PC、`AT_PHDR`/`AT_ENTRY` 共用该 bias；原始 program header 值不变，没有 interpreter 时 `AT_BASE` 为零。映射来源明确为原始文件字节，不使用分析阶段的 pointer fixup；来宾启动代码必须自行完成 relocation 和初始化。Loader 从有界的原始文件记录中解码 `PT_DYNAMIC`，不依赖 section header。若存在，该表必须可读、正确终止且最多 4096 项。拒绝 `PT_INTERP` 和外部 dependency/filter/audit 标签；不会提供 dynamic linker、符号解析器或 constructor runner。

静态 `PT_TLS` 模板作为 loader 提供的事实进行验证：仅一个模板，文件／内存范围有界、对齐一致且初始字节可读。来宾启动时分配并初始化 TLS 块，安装 thread pointer；Linux 模型不会虚构 libc 专用 TCB/DTV。这样支持 freestanding 程序中的编译器生成 local-exec TLS。Dynamic TLS 和 OS 线程仍属独立工作。

x64 的 `arch_prctl` 支持 `ARCH_SET_FS`、`ARCH_GET_FS`、`ARCH_SET_GS` 和 `ARCH_GET_GS`。Set 可接受尚未映射的 user-range 基址，后续解引用仍检查权限。kernel-range 基址返回来宾 `EPERM`；无效 Get 目标返回来宾 `EFAULT`，不会触发 CPU fault。其他操作明确失败。ARM64 启动用 `MSR` 安装 `TPIDR_EL0`；`MRS`、FS/GS 内存访问和上下文恢复会跨执行量与后端入口保留 thread pointer。这本身不实现线程调度器。

PIE/TLS fixture 检查独立对齐的 TLS 块、TLS BSS、重定位后的 auxv，以及来宾自行 relocation 前原始为零的 RELA 槽。x64 测试验证 `arch_prctl` 错误不会丢失先前基址。请在上述 build/CTest 命令中加入 `NeverDThreadPointerTests`；不可用后端仍明确跳过。
