**语言**：[English](../emulation.md) | [简体中文](emulation.md) | [繁體中文](../zh-TW/emulation.md) | [日本語](../ja/emulation.md) | [한국어](../ko/emulation.md) | [Français](../fr/emulation.md) | [Deutsch](../de/emulation.md) | [Español](../es/emulation.md) | [Italiano](../it/emulation.md) | [Русский](../ru/emulation.md) | [العربية](../ar/emulation.md)

<!-- i18n-source: a3e64122b77a690dd856d02f5b2af53973d3bf1affd973bea5f9735aa9dd6722 -->

[← 文档索引](README.md)

# CPU 执行与来宾环境

<!-- i18n-section: backends -->

## CPU 后端与工作负载

CPU 执行分离 ISA 准入、来宾内存、后端传输与来宾 OS 策略。`NEVERD_ENABLE_CPU_EMULATION` 启用 x64/ARM64 CPU 层；`NEVERD_ENABLE_DRIVER_EMULATION` 添加有界 x64 Windows WDM/KMDF 环境。`linux-elf64-v1` 配置运行受支持的 Linux ELF 进程。参见[CPU 执行](cpu-execution.md)、[来宾进程模拟](process-emulation.md)及[Windows 驱动模拟](driver-emulation.md)。

对于受支持的原生契约，`auto` 在来宾 ISA 与宿主一致时选择 Linux 的 KVM、Windows 的 WHP 或 [macOS 的 HVF](macos-hvf.md)。跨 ISA 使用 Unicorn；`software-cpu-v1` 和原有 V1 API 保留软件执行。显式选择的后端不可用时直接失败，不自动回退。原生执行在进入 CPU 前检查指令准入、地址和效果。宿主虚拟化不决定来宾 OS：[Darwin 配置](darwin-emulation.md) 单独建模 macOS、iOS 和 iOS Simulator。HVF 需要 `com.apple.security.hypervisor` 权限。

`driver-strict` / `checked-x64-v1` 覆盖有界 x64 执行；Windows 驱动加载仍限 x64。`checked-aarch64-v1` 和 `checked-user-aarch64-v1` 包含有界 ARM64 FP32/FP64、定宽 SIMD 及完整 FPCR/FPSR/向量状态。Mac 指南已记录 ARM64 HVF 原生验收；ARM64 KVM/WHP 工作负载验证与 Intel HVF 完整验收仍待完成。CPU 执行受支持不代表兼容任意驱动或应用。

<!-- i18n-section: windows-processes -->

## Windows 进程与模块

`windows-pe64-v1` 支持有界 Windows x64/ARM64 控制台进程，包括 PEB/TEB、静态和动态 TLS、`DllMain`、具名 Win32 API 和显式无环 DLL 图。客户模块支持按名称／序号导入代码及数据、DIR64 重定位、转发导出和真实加载器链表身份。`LoadLibraryA`／`LoadLibraryW`、`FreeLibrary` 和 `GetProcAddress` 使用配置的模块目录。CRT／GUI、ARM64 基于栈帧的用户态 SEH、线程和通用 Windows 应用兼容性仍待完成；原生 ARM64 KVM/WHP 证据仍缺失。

`WindowsSystemModules` 为两种 ISA 构造有界的 `ntdll.dll`、`kernelbase.dll` 和 `kernel32.dll` PE64 模型映像。ASCII `GetModuleHandleA` / `GetModuleHandleW`、`LoadLibraryA` / `LoadLibraryW` 与 `GetProcAddress` 共用其映射基址；PEB/LDR 和 `MEM_IMAGE` 描述同一批映像。静态导入、按名称查询和客户 DLL 转发使用相同 API 跳板与导出解析器。提供方固定驻留，不执行客户初始化回调，普通客户 DLL 全部卸载后不会阻止入口返回。头部或导出元数据改变会停止查询。未知系统导出名称和非零系统序号查询明确停止；已建模名称的大小写不匹配和空名称返回错误 127，空指针查询返回 87。生成的字节和地址属于模型策略，不复刻特定 Windows DLL 布局、原生序号或跨提供方别名。`WindowsSystemTests.cpp` 对照原始 x64/ARM64 EXE 与原生 Windows，并独立观察八次初始线程返回。

<!-- i18n-section: environment-memory -->

## 环境变量与内存

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` 共用 PEB 进程参数中的实时客户环境块。名称限 ASCII 且忽略大小写，值为 UTF-16。修改前校验输入、容量及可写内存。快照不受后续修改影响，释放时回收客户内存。模型的环境块上限为 64 KiB；字符串与展开操作有明确边界并检查工作负载截止时间。未知指针归属、格式错误的环境块、ANSI 代码页及展开缓冲区重叠仍不支持。`WindowsEnvironmentTests.cpp` 在可用后端比较原创 x64/ARM64 样例，CI 必须执行独立的原生 Windows 对照。

`WindowsProcessHeap` 统一管理进程堆的分配、`HeapReAlloc`、释放和尺寸查询。调整大小保留原有有效数据；`HEAP_ZERO_MEMORY` 清零新增字节，`HEAP_REALLOC_IN_PLACE_ONLY` 禁止搬迁。重分配失败时保留旧块，返回 NULL 并设置 `ERROR_NOT_ENOUGH_MEMORY`（8），与原生观测一致。独立页内存使收缩和释放能归还容量，分阶段扩容及有界复制检查工作负载截止时间。自定义堆、异常生成标志、未知归属以及不可访问的复制或清零范围均明确停止。`WindowsHeapTests.cpp` 覆盖两种 ISA、强制搬迁、预算复用和失败原子性；CI 也在原生 Windows 上运行同一原创 EXE。

Windows 虚拟内存新增 `VirtualAlloc`、`VirtualFree`、`VirtualProtect`、`VirtualQuery` 及当前进程的 `FlushInstructionCache`。OS 层管理预留区域，`AddressSpace` 统一管理已提交页面、权限和物理存储。测试覆盖动态代码改写、访问故障和内存额度回收。

`WriteProcessMemory` 对不超过 4 KiB 的当前进程写入，遵循 x64/ARM64 原生实测的已提交页面语义。它保留各区域的权限、已复制前缀、字节数和 LastError，包括 `ERROR_NOACCESS`、`ERROR_PARTIAL_COPY` 以及 RX 前缀写入后返回成功的情况。`WindowsMemoryWriteTests.cpp` 检查全部 25 种权限组合；`check_windows_memory_write.py` 在原生 Windows CI 上验证同一份原创可执行文件。未提交的目标区域仍明确不受支持。

<!-- i18n-section: vectored-exceptions -->

## 向量异常与继续执行

`WindowsProcessExceptions` 在同一 CPU 和进程预算内实现 `AddVectoredExceptionHandler`、`RemoveVectoredExceptionHandler` 和 `RaiseException`。有序处理器可注册或移除处理器、触发嵌套异常、调用已建模 API、加载 DLL 以及退出进程。x64/ARM64 数据访问异常和 x64 整数除法异常可在校验客户对 `CONTEXT` 的修改后恢复；通用寄存器、SIMD 和受支持的浮点状态会保留。软件异常经模型提供方中的真实返回指令继续执行。模型限制为最多保留 128 个注册项、嵌套 16 层。非法处置值、被修改的异常指针、不支持的上下文字段和超限均明确失败。ARM64 基于栈帧的 SEH／展开、调试器派发及执行／保护页异常仍不支持。`WindowsExceptionTests.cpp` 将原创 EXE／DLL 场景与原生 Windows 对照；原生 ARM64 KVM/WHP 证据仍待补齐。 软件异常记录带有 `EXCEPTION_SOFTWARE_ORIGINATE`（`0x80`），与调用者传入的不可继续标志分别处理；原始 Windows 可执行文件精确核对软件异常和硬件异常的标志值。

`AddVectoredContinueHandler` 和 `RemoveVectoredContinueHandler` 管理独立的有序列表，与异常处理器共用最多保留 128 个注册项的限制。向量异常处理器接受继续执行后，继续处理器读取同一份可修改的异常记录和 `CONTEXT`；最终上下文校验在这些回调完成后进行，包含嵌套异常与 DLL 通知。两类处理器的句柄不可交叉移除。`WindowsContinuationTests.cpp` 将顺序、提前结束派发、增删、上下文修复、嵌套派发、加载器回调及进程退出的原创 EXE 场景与原生 Windows 对照。已测 Windows x64 向量处理路径允许在设置 `EXCEPTION_NONCONTINUABLE` 时继续执行；这不代表基于栈帧的 SEH 行为。原生 ARM64 执行仍未验证。

<!-- i18n-section: caller-context -->

## 调用者上下文

`RtlCaptureContext` 已通过 `kernel32.dll` 和 `ntdll.dll` 支持 x64、ARM64。共享的 `WindowsProcessContext` 与 `IntegerABI` 保存调用者 PC/SP，不修改 CPU 状态或 LastError。原生 Windows 观察确认 x64 标志为 `0x10000f`，未涉及的 home／调试／向量存储保持原样，x87 地址字段保留传统的低 32 位；ARM64 从 LR 保存 PC，并清零记录中的 X0/LR。寄存器、SIMD 与浮点控制来自客体；x64 选择子和 MXCSR 能力掩码遵循配置的客体 CPU。无效、未对齐或部分不可访问的目标记录在写入前明确失败。`WindowsContextTests.cpp` 覆盖静态导入、提供者查询、VEH 回调、跨页输出及失败原子性。`scripts/check_windows_context.py` 在原生 Windows x64、ARM64 上运行原创程序，并单独验证非空 x87 状态。这些 ARM64 API 观察不代表原生 KVM/WHP 执行验证。上下文恢复、栈回溯和动态函数表仍需继续实现。 `WindowsProcessServices.def` 声明精确的模块限制：模型在 `kernelbase.dll` 中查询此符号时返回 `ERROR_PROC_NOT_FOUND`（127），与原生观察一致，不凭空增加导出。 [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

<!-- i18n-section: structured-exceptions -->

## 结构化异常处理

`WindowsProcessSEH` 使用 `os/windows/exception/` 中共享的 `X64SEH`（`NeverDEmulationWindowsException`，无需启用驱动环境），处理 x64 `__C_specific_handler` 和 UNWIND_INFO V1。VEH 搜索结束后支持过滤器、finally 回调、非局部处理器跳转、嵌套／冲突展开以及重定位 EXE/DLL 栈帧，保留非易失 GPR/XMM 状态。过滤器选择继续执行时，VCH 使用同一份 `CONTEXT`。`WindowsSEHTests.cpp` 将 23 个原创场景与原生 Windows 对照；KVM/WHP/Unicorn 共用这些语义。派发在进程预算内重新校验映像代次、头部、展开／作用域字节、语言处理器代码区域及 IAT 绑定。元数据被修改或保留的映像被卸载时明确失败。ARM64 栈式 SEH、C++ EH、动态函数表、通用 RtlUnwind/NtContinue、跨加载器／VEH／VCH 回调边界展开仍不支持。

当记录包含 `EXCEPTION_NONCONTINUABLE` 而 x64 过滤器返回 `EXCEPTION_CONTINUE_EXECUTION` 时，系统使用新上下文派发 `STATUS_NONCONTINUABLE_EXCEPTION`（`0xc0000025`，标志 `0x81`，关联记录指针为空）。先重新运行 VEH，再从保留的逻辑栈重新搜索，在相同深度与执行预算内保留 finally 顺序和 EXE/DLL 栈帧身份。23 个原生场景包括 21 个成功执行和两个终止场景：即使恢复原始 `CONTEXT`，VEH/VCH 接受继续这个二次异常后，它仍未处理。模型将此结果报告为运行时失败。软件异常地址等于保存的 PC；内部派发器地址和寄存器布局由模型定义。 [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

<!-- i18n-section: processor-state -->

## 指令与处理器状态

受检 x64 现支持普通 RAM 上的 `MOVS/STOS/LODS` 与 `CLD/STD`，并逐元素验证恢复、取消和跨页访问。依赖 CPU 型号的零次数高位行为与 STOS/LODS 设备操作数仍不在契约内。

受检 x64 还支持普通 RAM 上的 `CMPS/SCAS` 与 `REPE/REPNE`，涵盖算术标志、提前终止、逐元素停止和故障恢复；设备比较仍不支持。

x64 与 ARM64 原生启动自检在独占内存租约下验证有界的完整状态执行。XSAVE 数据包和包含 ISA 身份的页表缓存由唯一权威层管理。

原生 x64 的 `FOP/FIP/FDP` 遵循宿主保存、恢复规则：AMD 可能清零未生效的 x87 异常元数据。启动自检通过未屏蔽的待处理异常验证这些字段。
