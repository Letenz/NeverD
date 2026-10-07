**语言**：[English](../process-emulation.md) | [简体中文](process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← 文档索引](README.md)

# 来宾进程模拟

`neverd emulate` 在明确指定的来宾 OS 配置下执行映像。CPU 传输、映像解析、进程入口和 OS 服务分别由不同层负责。启用 `NEVERD_ENABLE_CPU_EMULATION=ON`；驱动模拟也会启用它。

macOS 和 iOS 也提供明确的 Mach-O profile，覆盖 macOS、iOS 设备与 iOS Simulator。
启动、Darwin ABI、匿名内存和支持范围见 [macOS/iOS 进程环境](darwin-emulation.md)。
macOS 宿主的同架构硬件加速使用 [HVF](macos-hvf.md)。

首个配置 `linux-elf64-v1` 在 CPL3 或 EL0 运行 x64/AArch64 ELF `ET_EXEC` 与可自重定位的静态 PIE `ET_DYN`。它加载真实 ELF 分段，构造初始栈，按指令量恢复执行，并处理显式 Linux 系统调用请求。这是独立进程模型，不是完整 Linux 发行版，也不承诺运行任意 libc 二进制。动态链接、信号、线程、文件系统和不支持的服务都会明确失败。

<!-- i18n-section: cli-sdk -->

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

<!-- i18n-section: options-results -->

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
| `linux_kernel` | 缺省 | 显式 GKI 分支、固定来宾任务清单或内核接口缺失观察值 |
| `linux_priority` | 缺省 | 显式任务 nice 值及原始 Linux 优先级服务所需的调用者权限 |

`schema_version` 为 1。结果含 profile、架构、所选后端与原因、`stop_reason`、可空的 `exit_status`、诊断、入口／当前 PC、计数器、服务记录和最后一个类型化 CPU 退出。地址、syscall 编号、参数寄存器及原始返回位均为**不带** `0x` 的十六进制字符串；`stdout_hex`／`stderr_hex` 保留 NUL 和无效 UTF-8。syscall 结果为 null 表示没有建模返回值（例如退出或不支持的请求），不表示成功返回 0。

<!-- i18n-section: linux-semantics -->

## Linux 配置语义

`writev` 在 x64/ARM64 及 Android Bionic 中共用上述输出对象。它先导入最多 1024 个来宾 `iovec`，负长度返回 `EINVAL`，校验用户地址范围，再应用 Linux 按页对齐的传输上限。无效描述符先于向量访问返回 `EBADF`；不可读的描述符表返回 `EFAULT`，不产生输出。后续数据页故障保留已复制的前缀。输出预算在发布前覆盖整个向量及两个输出流。`write` 和 `writev` 使用描述符的低 32 位；向量数量也遵循 Linux 的 32 位导入规则。仅 Bionic 将原始负错误码转换成 `-1` 和 `errno`。`LinuxOutputNativeTests` 在本机 Linux 上以普通文件重定向运行十个原创用例；模拟的 x64/ARM64 用例还验证预算。参见 [Linux 向量导入契约](https://github.com/torvalds/linux/blob/v6.12/lib/iov_iter.c)。

OS 策略复用现有 ELF 加载器解析出的 program headers。它验证 ABI 标签、segment 对齐、已映射的 program-header 表及用户地址边界。通用映射计划会在分配前检查范围、权限、重叠和预算，只发布完全准备好的私有地址空间。保留文件页前缀／尾部字节，将 BSS 清零，遵循 segment 权限并为栈保留 guard gaps。页重叠布局和矛盾 header 会被拒绝，不会猜测。

静态 PIE 使用不低于 `0x40000000` 的确定性 load bias，并按较大的 `PT_LOAD` 对齐要求递增。所有映射段、入口 PC、`AT_PHDR`/`AT_ENTRY` 共用该 bias；原始 program header 值不变，没有 interpreter 时 `AT_BASE` 为零。映射来源明确为原始文件字节，不使用分析阶段的 pointer fixup；来宾启动代码必须自行完成 relocation 和初始化。Loader 从有界的原始文件记录中解码 `PT_DYNAMIC`，不依赖 section header。若存在，该表必须可读、正确终止且最多 4096 项。拒绝 `PT_INTERP` 和外部 dependency/filter/audit 标签；不会提供 dynamic linker、符号解析器或 constructor runner。

初始栈含对齐的 argc/argv/envp/auxv、PHDR/PHENT/PHNUM、entry、页大小和身份值。模型 PID/TID/UID/GID 均为 1000。为保证可复现，`AT_RANDOM` 是输入 SHA-256 的前 16 字节；这是确定性的模型策略，不是加密熵。HWCAP/HWCAP2 均为 0，没有 vDSO。

已实现的调用为 `write`、`exit`、`exit_group`、`getpid`、`gettid`, `mmap`, `mprotect`, `munmap`, `brk`，其编号分别遵循 [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) 与 [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h)。x64 SYSCALL 返回时会应用 RCX/R11 clobber，同时设置 RAX 与下一 PC；ARM64 使用 x8 作为编号、x0 作为结果。未知调用会以 `unsupported_service` 停止，不会执行宿主 syscall。

静态 `PT_TLS` 模板作为 loader 提供的事实进行验证：仅一个模板，文件／内存范围有界、对齐一致且初始字节可读。来宾启动时分配并初始化 TLS 块，安装 thread pointer；Linux 模型不会虚构 libc 专用 TCB/DTV。这样支持 freestanding 程序中的编译器生成 local-exec TLS。Dynamic TLS 和 OS 线程仍属独立工作。

x64 的 `arch_prctl` 支持 `ARCH_SET_FS`、`ARCH_GET_FS`、`ARCH_SET_GS` 和 `ARCH_GET_GS`。Set 可接受尚未映射的 user-range 基址，后续解引用仍检查权限。kernel-range 基址返回来宾 `EPERM`；无效 Get 目标返回来宾 `EFAULT`，不会触发 CPU fault。其他操作明确失败。ARM64 启动用 `MSR` 安装 `TPIDR_EL0`；`MRS`、FS/GS 内存访问和上下文恢复会跨执行量与后端入口保留 thread pointer。这本身不实现线程调度器。

文件描述符 1 和 2 是虚拟字节 sink。`write` 验证可读 user pages；后续页不可读时返回已读前缀，没有任何字节可读时返回来宾 `EFAULT`。无效描述符返回 `EBADF`；零字节写入仍检查用户地址范围，但不要求页面已映射，也不读取数据。这不模拟 Linux pipe 原子性或文件对象。若输出超过限制，会在发布写入前停止。

匿名内存服务与映像、栈共用进程地址空间和物理内存预算。`mmap` 仅接受 `MAP_PRIVATE | MAP_ANONYMOUS`，权限为普通 `PROT_NONE`、`PROT_READ`、`PROT_READ | PROT_WRITE`、`PROT_READ | PROT_EXEC` 或可读的 RWX。空闲且页对齐的提示地址会被采用；否则从 `0x100000000` 起、再从最低用户地址起查找空隙，并保留栈保护区。这是确定性布局，不模拟 Linux ASLR。新页独立分配并清零；部分解除映射能回收未被固定的页。CPU 投影或仍持有的 backing view 可以将已退役分配的生命周期延长到自身释放时。

长度向上取整到页。`munmap` 允许空洞和重复移除；`mprotect` 遇到空洞前会修改已映射前缀，然后返回 `ENOMEM`。`PROT_NONE` 保留分配和字节，但禁止来宾访问。原始 `brk` 成功时返回请求的字节边界，失败时返回旧边界，不采用 libc 包装器的零／负一约定。初始 break 是页对齐的映像末尾。增长受其他映射与内存预算限制；收缩保留剩余部分页中的字节。受支持子集的规则与错误优先级遵循 Linux 的[映射](https://github.com/torvalds/linux/blob/v6.8/mm/mmap.c)和[保护](https://github.com/torvalds/linux/blob/v6.8/mm/mprotect.c)服务。

文件、共享、固定映射，向下增长、大页、内存锁定、保护键、仅执行／仅写策略及其他标志都属于明确不支持的服务：在发布效果或构造返回值前停止。已支持子集内的一般范围、长度和对齐错误会返回来宾错误，并允许继续执行。任何内存服务都不会向宿主 OS 转发来宾指针或映射请求。

可选的 `linux_kernel` 记录经观察确认缺失的内核接口。例如，测试环境缺少 `pidfd_open` 实现时使用：

```json
{"linux_kernel":{"unavailable_syscalls":["pidfd_open"]}}
```

指定的原始调用在验证参数前返回 -ENOSYS，不创建描述符，也不修改客户内存；这与缺失的内核入口一致。Bionic 的 `syscall` 包装器仍执行通常的 -1／errno 转换。缺少输入或提供空列表时，该接口仍明确不支持；其他未知调用不会自动转换为 ENOSYS。目前仅接纳 `pidfd_open`，未知名称、重复项和类型错误均被拒绝。此输入不推断内核版本、宿主可用性或可工作的 pidfd 实现。参见[内核缺失调用实现](https://github.com/torvalds/linux/blob/master/kernel/sys_ni.c)。

可选的 `linux_priority` 为与调用者 UID 相同的测试任务声明 nice 状态。Linux ELF64 与 Android 原生工作负载的原始 `setpriority` 和 `getpriority` 共用此状态，不修改宿主优先级。

```json
{"linux_priority":{"tasks":[{"id":1000,"nice":0}],
                   "cap_sys_nice":false,"rlimit_nice":0}}
```

任务 ID 必须为互不重复的正有符号 32 位整数，初始 nice 值范围为 -20 至 19，`rlimit_nice` 范围为 0 至 40。`cap_sys_nice` 和 `rlimit_nice` 默认为 false 和零；任务状态必须显式声明。缺少输入、未列出的任务或 PRIO_PGRP／PRIO_USER 选择会以不支持的服务停止；这也包括未声明 nice 状态的新建线程。模型不猜测继承规则或其他任务的归属。PRIO_PROCESS 的 who 为零时选择当前客户任务，否则选择指定任务。

无效选择器返回原始 -EINVAL。设置请求将有符号 32 位 nice 参数限制到 -20..19。降低 nice 值需要 CAP_SYS_NICE 或足够的 RLIMIT_NICE；拒绝时返回原始 -EACCES，状态保持不变。原始查询返回 `20 - nice`，保留内核的 40..1 编码，并非 libc 转换后的 `getpriority` 结果。参见 [Linux 优先级接口](https://man7.org/linux/man-pages/man2/setpriority.2.html)。

`linux_signals` 显式提供进程级初始信号处置。缺失项表示未知，不代表 `SIG_DFL`；显式空列表允许在不查询旧值的情况下安装新处置。五个字段均必填，四个动作字段为无符号 64 位值，超出 JSON 精确整数范围时使用十进制字符串。

```json
{"linux_signals":{"actions":[
  {"signal":11,"handler":0,"flags":0,"restorer":0,"mask":0}
]}}
```

信号编号为 1–64。`rt_sigaction` 与 Bionic 共用状态，结构布局和错误顺序按各自接口处理。不实现待处理信号、投递、处理函数调用或线程信号掩码，也不使用宿主处理函数。参见[完整契约](../process-emulation.md#linux-profile-semantics)。

<a id="windows-pe64-profile"></a>

`linux_kernel.gki` 显式选择 5.10–6.18 已发布分支，控制 `pidfd_open` 与版本化向量导入；Android API 级别不选择内核。`linux_kernel.tasks` 是可选的固定存活任务清单，条目如 `{ "id": 2000, "group_leader": true }`；省略时外部目标仍不支持，空数组仅含当前组首领，清单外 PID 返回 ESRCH。任务与优先级观察值须一致，且不能组合协作式 Android 线程。描述符与 `linux_files` 共用表；GKI 与显式缺失 pidfd 观察值不能并存。参见[完整契约与限制](android-gki-kernels.md)。

该 GKI 子集还支持已观察的进程 CPU 时钟，以及零超时 pidfd `ppoll`。后者要求显式零 timespec、空临时掩码和 `linux_files` 描述符限额；仅按序写 `revents`，不回写超时或推进墙钟。已关闭描述符返回 POLLNVAL，已观察的存活 pidfd 无就绪状态；阻塞等待和其他类型的就绪状态仍不支持。

<!-- i18n-section: linux-clocks -->

## 显式客户机时钟

可选的 `linux_time` 为 Linux 系统调用与 Android Bionic 提供固定时钟输入。模型不读取宿主时钟，不随指令执行推进时间，也不猜测默认时间。

```json
{"linux_time":{"clocks":[
  {"id":0,"seconds":"4294967297","nanoseconds":987654321},
  {"id":1,"seconds":123,"nanoseconds":456789}],
  "timezone":{"minutes_west":-60,"dst_time":0}}}
```

时钟输入包括静态 ID 0–9、11，以及显式 GKI 支持的负数编码进程 CPU 时钟。PROF、VIRT、SCHED 独立，当前进程别名共用样本；重复身份、未知 ID 或超过 16384 个观察值均拒绝。外部进程须在固定清单中声明为存活组首领。秒数为有符号 64 位，CPU 时钟须非负，纳秒范围为 `[0, 1000000000)`；JSON 整数限于 `±9007199254740991`，十进制字符串保留完整范围，时区为有符号 32 位。C++ 使用 `ProcessOptions::LinuxTime`；其他 OS 配置拒绝此选项。

`advance_on_idle: true` 显式启用 x64／ARM64 的相对 `nanosleep`，包括 Android 命名及 variadic 包装。墙钟 ID 0、1、7 随虚拟空闲时间推进，进程 CPU 样本可同时存在且保持固定；其他时钟不接受此策略。已阻塞线程保留原服务，其他可运行线程先执行；全部阻塞时推进至最早期限，单线程直接推进。指令执行不增加时间或 CPU 用量。

`clock_gettime` 取 ID 的低 32 位有符号值，在目标复制前检查类别、任务及观察值；非法类别／外部目标可返回 `EINVAL`，有效样本的目标复制可返回 `EFAULT`。缺少观察值和未支持的动态／FD／编码线程时钟明确停止。原始陷阱保留负错误，Bionic 单独转换 errno。已有 `gettimeofday` 与 `time` 的有序写入及部分故障规则仍适用；参见[已发布 GKI 时钟契约](android-gki-kernels.md)与[完整时钟规则](../process-emulation.md#explicit-guest-clocks)。

<a id="explicit-memory-files"></a>

<!-- i18n-section: linux-files -->

## 显式内存文件

`linux_files` 为 Linux ELF64 与 Android 提供封闭的只读文件目录；C++ 使用 `ProcessOptions::LinuxFiles`。`files` 必填且可为空，每项必填规范绝对路径 `path` 和二进制 `bytes_hex`。不读取宿主文件，也不推断进程命令行或 `/proc` 内容。缺失目录选项时文件服务仍明确停止；目录中未列出的路径返回 `ENOENT`。

```json
{"linux_files":{"files":[
  {"path":"/fixture/data","bytes_hex":"00ff410a805a"}],
  "descriptor_limit":256}}
```

每次打开建立独立游标，所有 guest 线程、Bionic、`syscall` 与原始陷阱共用描述符表；关闭释放最低可用编号。0–2 初始保留 stdin/stdout/stderr，stdin 没有默认内容。支持只读 `open/openat`、`read/close` 和普通 `lseek`，可选 `O_CLOEXEC` 及架构对应的 `O_LARGEFILE`。Bionic 单独处理 errno。读取故障保留已复制前缀，EOF 不探测目标映射；相对路径、目录、写入、链接和未知形式仍不支持。

`descriptor_limit`: 3–4096 (256). `files`: ≤ 256. `path`: < 4096 bytes; component ≤ 255 bytes. Total bytes + paths + NUL ≤ 16 MiB; JSON ≤ 64 KiB. [Complete contract / 完整约定](../process-emulation.md#explicit-memory-files).

零长度读取的地址可以等于用户地址范围的末端。先验证原始地址范围，再检查文件位置加原始长度是否超过 `INT64_MAX`；超过时即使已到 EOF 也返回 `EINVAL`，游标保持不变。

每项还可提供完整的 `metadata`；C++ 用 `LinuxFileOptions::Metadata` 按已有文件路径保存。`fstat`、`fstat64` 和 syscall 共用这些固定观察值，不推断宿主身份或按内容长度生成 size，也不改变游标。元数据必须包含设备、inode、模式、链接数、UID/GID、大小、块大小、块数和三组时间；仅接受普通文件，时间为有符号 64 位秒及规范纳秒。超出 JSON 精确整数范围的值使用十进制字符串。x64/AArch64 分别写入 144/128 字节，rdev 和填充清零；无效描述符为 `EBADF`，完全不可写缓冲区为 `EFAULT`。缺少元数据、未知标准流或部分可写缓冲区明确停止，保留原字节。完整字段与范围见上方约定。

```json
{"linux_files":{"files":[{"path":"/fixture/virtual","bytes_hex":"616263",
  "metadata":{"device":1,"inode":"18446744073709551615","mode":33060,
    "link_count":1,"uid":1000,"gid":1000,"size":0,"block_size":4096,"blocks":0,
    "access_time":{"seconds":0,"nanoseconds":0},
    "modification_time":{"seconds":0,"nanoseconds":0},
    "change_time":{"seconds":0,"nanoseconds":0}}}]}}
```

<!-- i18n-section: windows-pe64 -->

## Windows PE64 配置

`windows-pe64-v1` 支持有界 Windows x64/ARM64 控制台进程，包括 PEB/TEB、静态和动态 TLS、`DllMain`、具名 Win32 API 和显式无环 DLL 图。客户模块支持按名称／序号导入代码及数据、DIR64 重定位、转发导出和真实加载器链表身份。`LoadLibraryA`／`LoadLibraryW`、`FreeLibrary` 和 `GetProcAddress` 使用配置的模块目录。CRT／GUI、ARM64 基于栈帧的用户态 SEH、线程和通用 Windows 应用兼容性仍待完成；原生 ARM64 KVM/WHP 证据仍缺失。

`WindowsProcessTime.cpp` 管理基于主机时钟的 `GetSystemTimeAsFileTime`、`GetTickCount`、`QueryPerformanceCounter`、`QueryPerformanceFrequency` 与 `ZwDelayExecution`。FILETIME 以 1601 年为起点，单位为 100 ns；性能计数器使用单调时钟并报告 10 MHz 频率，毫秒 tick 计数按 32 位回绕。支持不超过 500 ms 的非警觉相对延迟和零间隔；警觉等待、正数绝对时间和更长延迟明确停止，不缩短等待或报告成功。这些服务保持 LastError，各 CPU 后端使用同一模型。 `ZwDelayExecution` 仅读取 `BOOLEAN` 的低 8 位，参数寄存器的未使用高位不会改变等待策略。

[Windows x64 ABI](https://learn.microsoft.com/cpp/build/x64-calling-convention), [QueryPerformanceFrequency](https://learn.microsoft.com/windows/win32/api/profileapi/nf-profileapi-queryperformancefrequency), [FILETIME](https://learn.microsoft.com/windows/win32/api/minwinbase/ns-minwinbase-filetime).

Windows 虚拟内存新增 `VirtualAlloc`、`VirtualFree`、`VirtualProtect`、`VirtualQuery` 及当前进程的 `FlushInstructionCache`。OS 层管理预留区域，`AddressSpace` 统一管理已提交页面、权限和物理存储。测试覆盖动态代码改写、访问故障和内存额度回收。

私有分配支持 `MEM_RESERVE`、`MEM_COMMIT`、`MEM_DECOMMIT`、`MEM_RELEASE` 和 `MEM_TOP_DOWN`，预留按 64 KiB 对齐，页面大小为 4 KiB。仅预留不消耗来宾 RAM。重复提交保留数据并更新权限，解除提交归还独立页面的存储。完整范围校验和分阶段分配避免普通分配或权限失败留下部分修改。查询返回 48 字节的 x64/ARM64 内存信息结构，并仅在同一次分配内向后合并。初始映像、环境、堆区域、API 入口和栈边界均参与地址分配；栈的分配标识与 TEB 一致。如果成功的 `VirtualProtect` 将旧权限的输出地址改成只读，新权限仍生效，输出内容保持不变，调用仍返回成功。 对未完整提交范围的权限修改失败时，返回 `ERROR_INVALID_ADDRESS`，旧权限输出写为 `PAGE_NOACCESS`，各页权限保持不变。

支持的权限为 `PAGE_NOACCESS`、`PAGE_READONLY`、`PAGE_READWRITE`、`PAGE_EXECUTE_READ` 和 `PAGE_EXECUTE_READWRITE`。保护页、仅执行及写时复制策略、缓存修饰符、大页、reset/write-watch/占位区域及修改模型拥有的运行时映射仍明确拒绝。仅私有虚拟分配可解除提交或释放。

[VirtualAlloc](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc), [VirtualFree](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualfree), [VirtualProtect](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect), [VirtualQuery](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualquery), [MEMORY_BASIC_INFORMATION](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-memory_basic_information).

```bash
neverd emulate guest.exe --profile=windows-pe64-v1 \
  --options='{"backend":"auto","arguments":["guest.exe","argument"],"environment":["MODE=test"]}'
```

单线程配置将 PE32+ EXE 保持在首选基址，接收可带入口及静态 TLS 的显式 DLL。`WindowsProcessOptions::Modules` 或 JSON `windows.modules` 通过 `name`、`path` 提供最多 64 个客户模块基本名及主机输入路径，不搜索或执行主机 DLL。ASCII 名称不区分大小写；重复名称和覆盖系统 API 提供方均拒绝，只读取可达文件。按名称／序号导入的函数和数据绑定实际映射导出；导出空洞、缺失符号、循环、绑定／延迟导入和未支持的加载配置／CFG 明确失败。可重定位 DLL 遇到地址冲突时应用 DIR64；固定地址冲突和写入链接元数据的重定位在发布受影响映像前失败。

`readPEProgramExports` 拥有原始导出身份及有界元数据读取范围；`WindowsProcessModules` 拥有客户模块图和进程统一的精确提供方／名称 API 跳板。`VirtualMemory` 在映射前登记所有映像，`AddressSpace` 管理页面及权限。PEB/LDR 仅列出真实映像，初始化链表保留加载器登记顺序，并与按依赖计算的挂接调用顺序分别维护。`GetModuleHandleW` 接受 NULL 或 ASCII 基本名，不区分大小写，无扩展名时补 `.dll`；路径、非 ASCII 查询及末尾点规则仍不支持。名称缺失返回错误 126，成功保持 LastError。API 模型不等同于已安装的系统 DLL。

输入文件总字节数和映像总范围各自受 `memory_limit` 限制，运行时映射也计入映像预算。准备阶段共享 65,536 条记录、64 MiB 元数据读取、名称长度和整个任务的截止时间限制；阻塞式主机 I/O 不保证硬实时。原创 EXE→DLL→DLL 样例检查重定位指针、序号调用、共享数据、API 指针身份、`MEM_IMAGE`、加载器链表及 EXE TLS 挂接／分离。`NeverDWindowsProcessTests` 包含这些检查和直接原生 Windows 对照；`NeverDPEProgramExportsTests` 验证畸形元数据及资源计费，`NeverDProcessPublicTests` 验证 C ABI/CLI 模块目录一致性。不可用后端明确跳过。

`WindowsProcessLifetime` 在同一个 CPU 和执行预算下，按依赖顺序执行 DLL TLS 回调及 `DllMain`，随后执行 EXE TLS 和入口。每个模块都有独立 TLS 索引及对齐的数据块，从完成重定位和导入绑定的映像复制，共享 64 KiB 空间。TLS 保留参数为零，启动／进程退出的 `DllMain` 接收不透明非空值。显式进程退出按加载器链表的逆序分离已完成初始化的 DLL，再执行 EXE TLS 退出回调，即使 EXE 初始化尚未运行。启动 `DllMain(FALSE)` 以 `0xc0000142` 退出，不发送分离通知。故障和预算耗尽不伪造清理。带客户 DLL 的 PE 入口返回涉及尚未支持的线程终止，明确停止。非零 `SizeOfZeroFill` 仍不支持；实际 TLS 模板中的零初始化字节受支持。 无入口 DLL 接收 TLS 挂接通知，但不接收进程分离通知。

`WindowsProcessExports` 为静态导入和 `GetProcAddress` 共用名称／序号解析，覆盖代码、数据、别名及链式转发。 只有实际引用的启动转发才引入目录中的模块和初始化依赖，未使用的转发不加载文件。 导出名称区分大小写；名称缺失返回 NULL／错误 127，直接查询缺失序号（包括空洞）返回 NULL／错误 182，查询参数为空指针返回错误 87，成功保留 LastError。 未知模块句柄仍不支持。 有界 API 清单按精确提供方／名称一次性保留调用入口。 解析检查每个查询映像的实时 PE 头和导出元数据，拒绝修改或不可读字节，转发链最多 64 项，并共享准备阶段剩余的元数据额度及执行截止时间。 转发到空洞时返回目标映像基址并保留 LastError；转发到零序号返回错误 87。 返回基址是数据地址，不授予映像头执行权限。 运行时转发可以加载配置目录中的模块，并在返回查询结果前完成初始化。仍不支持实时改写导出表。

`WindowsProcessLoader` 从 `windows.modules` 加载 ASCII DLL 基名，统一管理显式引用、共享依赖和启动模块保留。重复查询转发导出不会增加额外引用。模块目录槽位在重载时使用新的驻留代次。TLS 和 `DllMain` 在同一 CPU 上、被暂停 API 的栈帧下方执行；恢复寄存器保留客户内存写入，并使用实时返回地址。动态附加／分离的保留指针为零。显式加载期间的附加失败在清理后返回错误 1114，同时保留已成功的独立嵌套加载。卸载释放映像映射和 TLS，重载恢复原始映像内容。模型之外对加载器链表或 TLS 指针的修改会明确失败。失败和重载都不会重置文件、映像与元数据工作额度。系统提供方以已映射 PE 的基址作为模块句柄。文件系统搜索、非 ASCII 路径、`LoadLibraryEx` 标志、循环导入及正在初始化或卸载的同一模块的重入转换仍不支持。

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` 共用 PEB 进程参数中的实时客户环境块。名称限 ASCII 且忽略大小写，值为 UTF-16。修改前校验输入、容量及可写内存。快照不受后续修改影响，释放时回收客户内存。模型的环境块上限为 64 KiB；字符串与展开操作有明确边界并检查工作负载截止时间。未知指针归属、格式错误的环境块、ANSI 代码页及展开缓冲区重叠仍不支持。`WindowsEnvironmentTests.cpp` 在可用后端比较原创 x64/ARM64 样例，CI 必须执行独立的原生 Windows 对照。

`WindowsProcessHeap` 统一管理进程堆的分配、[`HeapReAlloc`](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heaprealloc)、释放和尺寸查询。调整大小保留原有有效数据；`HEAP_ZERO_MEMORY` 清零新增字节，`HEAP_REALLOC_IN_PLACE_ONLY` 禁止搬迁。重分配失败时保留旧块，返回 NULL 并设置 `ERROR_NOT_ENOUGH_MEMORY`（8），与原生观测一致。独立页内存使收缩和释放能归还容量，分阶段扩容及有界复制检查工作负载截止时间。自定义堆、异常生成标志、未知归属以及不可访问的复制或清零范围均明确停止。`WindowsHeapTests.cpp` 覆盖两种 ISA、强制搬迁、预算复用和失败原子性；CI 也在原生 Windows 上运行同一原创 EXE。

`WindowsSystemModules` 为两种 ISA 构造有界的 `ntdll.dll`、`kernelbase.dll` 和 `kernel32.dll` PE64 模型映像。ASCII `GetModuleHandleA` / [`GetModuleHandleW`](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulehandlew)、`LoadLibraryA` / `LoadLibraryW` 与 [`GetProcAddress`](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress) 共用其映射基址；PEB/LDR 和 `MEM_IMAGE` 描述同一批映像。静态导入、按名称查询和客户 DLL 转发使用相同 API 跳板与导出解析器。提供方固定驻留，不执行客户初始化回调，普通客户 DLL 全部卸载后不会阻止入口返回。头部或导出元数据改变会停止查询。未知系统导出名称和非零系统序号查询明确停止；已建模名称的大小写不匹配和空名称返回错误 127，空指针查询返回 87。生成的字节和地址属于模型策略，不复刻特定 Windows DLL 布局、原生序号或跨提供方别名。`WindowsSystemTests.cpp` 对照原始 x64/ARM64 EXE 与原生 Windows，并独立观察八次初始线程返回。

`WindowsProcessExceptions` 在同一 CPU 和进程预算内实现 `AddVectoredExceptionHandler`、`RemoveVectoredExceptionHandler` 和 `RaiseException`。有序处理器可注册或移除处理器、触发嵌套异常、调用已建模 API、加载 DLL 以及退出进程。x64/ARM64 数据访问异常和 x64 整数除法异常可在校验客户对 `CONTEXT` 的修改后恢复；通用寄存器、SIMD 和受支持的浮点状态会保留。软件异常经模型提供方中的真实返回指令继续执行。模型限制为最多保留 128 个注册项、嵌套 16 层。非法处置值、被修改的异常指针、不支持的上下文字段和超限均明确失败。ARM64 基于栈帧的 SEH／展开、调试器派发及执行／保护页异常仍不支持。`WindowsExceptionTests.cpp` 将原创 EXE／DLL 场景与原生 Windows 对照；原生 ARM64 KVM/WHP 证据仍待补齐。 [AddVectoredExceptionHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-addvectoredexceptionhandler), [RemoveVectoredExceptionHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-removevectoredexceptionhandler), [RaiseException](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-raiseexception), [CONTEXT x64](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-context), [ARM64_NT_CONTEXT](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-arm64_nt_context). 软件异常记录带有 `EXCEPTION_SOFTWARE_ORIGINATE`（`0x80`），与调用者传入的不可继续标志分别处理；原始 Windows 可执行文件精确核对软件异常和硬件异常的标志值。 [EXCEPTION_RECORD](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_record).

`WindowsProcessContext` 保留每个派发帧的来源。已支持的 x64 数据访问和除法故障在 `CONTEXT.EFlags` 中呈现 RF（`0x10000`）；`RaiseException`（包括软件抛出的访问违规码）保留当前上下文。来源信息贯穿 VEH/VCH 和 SEH 搜索／展开。合法继续执行时恢复不含 RF 的逻辑 CPU 标志；客户修改 RF 会在发布状态前被拒绝。此受限配置不模拟指令断点或客户控制的 RF。`WindowsExceptionTests.cpp` 检查保存记录、恢复，以及拒绝时 CPU／RAM 不变。

Windows ring3 按独立原生观测，将 checked x64 的 `operand_alignment` 故障映射为 `STATUS_ACCESS_VIOLATION`，参数为 `[read, UINT64_MAX]`，存储指令也相同。原因由 CPU 层提供，Windows 不凭向量 13 猜测或重新解码指令。`WindowsAlignmentProcessTests.cpp` 执行原始 PE 指令，覆盖 72 种故障情境和 9 次地址修复重试（`72 + 9`），检查 PC、RF、XMM 和 RAM。未分类或字段不一致的故障仍会拒绝。进程与驱动故障报告保留可空的 `cause` 和十六进制 `error_code`，区分缺失与零。此派发适用于 checked x64 用户态执行契约。

`AddVectoredContinueHandler` 和 `RemoveVectoredContinueHandler` 管理独立的有序列表，与异常处理器共用最多保留 128 个注册项的限制。向量异常处理器接受继续执行后，继续处理器读取同一份可修改的异常记录和 `CONTEXT`；最终上下文校验在这些回调完成后进行，包含嵌套异常与 DLL 通知。两类处理器的句柄不可交叉移除。`WindowsContinuationTests.cpp` 将顺序、提前结束派发、增删、上下文修复、嵌套派发、加载器回调及进程退出的原创 EXE 场景与原生 Windows 对照。已测 Windows x64 向量处理路径允许在设置 `EXCEPTION_NONCONTINUABLE` 时继续执行；这不代表基于栈帧的 SEH 行为。原生 ARM64 执行仍未验证。 [AddVectoredContinueHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-addvectoredcontinuehandler), [RemoveVectoredContinueHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-removevectoredcontinuehandler).

`RtlCaptureContext` 已通过 `kernel32.dll` 和 `ntdll.dll` 支持 x64、ARM64。共享的 `WindowsProcessContext` 与 `IntegerABI` 保存调用者 PC/SP，不修改 CPU 状态或 LastError。原生 Windows 观察确认 x64 标志为 `0x10000f`，未涉及的 home／调试／向量存储保持原样，x87 地址字段保留传统的低 32 位；ARM64 从 LR 保存 PC，并清零记录中的 X0/LR。寄存器、SIMD 与浮点控制来自客体；x64 选择子和 MXCSR 能力掩码遵循配置的客体 CPU。无效、未对齐或部分不可访问的目标记录在写入前明确失败。`WindowsContextTests.cpp` 覆盖静态导入、提供者查询、VEH 回调、跨页输出及失败原子性。`scripts/check_windows_context.py` 在原生 Windows x64、ARM64 上运行原创程序，并单独验证非空 x87 状态。这些 ARM64 API 观察不代表原生 KVM/WHP 执行验证。上下文恢复、栈回溯和动态函数表仍需继续实现。 `WindowsProcessServices.def` 声明精确的模块限制：模型在 `kernelbase.dll` 中查询此符号时返回 `ERROR_PROC_NOT_FOUND`（127），与原生观察一致，不凭空增加导出。 [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

`WindowsProcessSEH` 使用 `os/windows/exception/` 中共享的 `X64SEH`（`NeverDEmulationWindowsException`，无需启用驱动环境），处理 x64 `__C_specific_handler` 和 UNWIND_INFO V1。VEH 搜索结束后支持过滤器、finally 回调、非局部处理器跳转、嵌套／冲突展开以及重定位 EXE/DLL 栈帧，保留非易失 GPR/XMM 状态。过滤器选择继续执行时，VCH 使用同一份 `CONTEXT`。`WindowsSEHTests.cpp` 将 23 个原创场景与原生 Windows 对照；KVM/WHP/Unicorn 共用这些语义。派发在进程预算内重新校验映像代次、头部、展开／作用域字节、语言处理器代码区域及 IAT 绑定。元数据被修改或保留的映像被卸载时明确失败。ARM64 栈式 SEH、C++ EH、动态函数表、通用 RtlUnwind/NtContinue、跨加载器／VEH／VCH 回调边界展开仍不支持。

当记录包含 `EXCEPTION_NONCONTINUABLE` 而 x64 过滤器返回 `EXCEPTION_CONTINUE_EXECUTION` 时，系统使用新上下文派发 `STATUS_NONCONTINUABLE_EXCEPTION`（`0xc0000025`，标志 `0x81`，关联记录指针为空）。先重新运行 VEH，再从保留的逻辑栈重新搜索，在相同深度与执行预算内保留 finally 顺序和 EXE/DLL 栈帧身份。23 个原生场景包括 21 个成功执行和两个终止场景：即使恢复原始 `CONTEXT`，VEH/VCH 接受继续这个二次异常后，它仍未处理。模型将此结果报告为运行时失败。软件异常地址等于保存的 PC；内部派发器地址和寄存器布局由模型定义。 [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

动态卸载回调开始前，模块已退出初始化链表；其映射、名称查询及加载／内存链表成员身份在回调期间仍然有效。入口返回的原生对照单独观察初始线程，不将系统工作线程的存活时间当作入口返回时间。

`WindowsDynamicTests.cpp` 使用原始 x64/ARM64 DLL 和 EXE，对照独立原生 Windows 观测，覆盖引用计数、共享依赖、嵌套加载、附加失败清理、转发查询、进程退出、无入口 DLL 以及重载时重新初始化 TLS。额外回归拒绝被修改的加载器元数据和失效代码指针，保持累计准备额度，并确保被中断 API 的结果仍未完成。Windows CI 强制执行原生对照和 WHP 用例；交叉编译与 Unicorn ARM64 不代表原生 ARM64 已执行验证。

`GetProcAddress` 转发链任何位置缺失库均返回错误 127；显式 `LoadLibrary` 加载目录中缺失的模块返回 126。原生对照和各可用后端都断言全部 41 个已声明加载场景；在 Windows 上，每种 DLL 变体都会重复 16 次验证全部卸载后从入口返回。 `GetProcAddress` 转发目标的初始化失败也在清理后返回 127。进程分离回调保留退出调用方的栈内容。

`WindowsExportTests.cpp` 使用原始 x64/ARM64 DLL 和 EXE，验证转发的代码／数据／序号调用、别名、初始化期间查询、重定位、大小写敏感的缺失项、LastError、循环及非驻留目标、无效指针，以及成功查询后的元数据修改。同一 EXE 具有独立原生 Windows 对照；原生 CI 强制执行 WHP 用例。C ABI／CLI 测试对比完整报告。原生 ARM64 硬件证据仍待补齐。 有导出表和无导出表的 EXE 变体覆盖两种依赖图、PEB 链表顺序、退出通知顺序，以及名称／序号／空指针的错误码。

`WindowsLifetimeTests.cpp` 将冻结的通知序列与独立原生 Windows 进程及 KVM/WHP/Unicorn 执行对比，覆盖正常退出、入口返回、两个 DLL 初始化失败、四处提前退出及无入口 DLL。另行验证回调故障、共享预算、重定位 TLS 字段和 TLS 总容量。原生入口返回探针保留初始线程句柄，重复64 次核对线程退出码及精确线程／进程通知序列。观察完成后终止剩余子进程线程，不将其进程退出码当作入口返回值。

```json
{"windows":{"modules":[{"name":"middle.dll","path":"inputs/middle.dll"},{"name":"leaf.dll","path":"inputs/leaf.dll"}]}}
```

x64 的 GS、ARM64 的 x18 指向 TEB。 支持栈边界、自指针、PID/TID、PEB、进程参数、LastError 和 TLS。 输入严格按 UTF-8 解码为 UTF-16，argv 按 Microsoft CRT 规则加引号。 环境变量名限 ASCII，拒绝忽略大小写后的重名；值可为 Unicode，排序后以双 NUL 结束，不继承主机环境或文件系统。 静态 TLS 复制模板、清零 BSS 并写入 32 位索引；动态 TLS 使用独立 TEB 槽位。 启动和退出按顺序读取实时回调表，所有指令与具名调用共享截止时间和资源额度。 正常进程退出运行退出回调。 入口返回只支持当前没有驻留客户 DLL 的情况；进程退出清理期间再次调用 `ExitProcess` 仍不支持。

精确 API 清单由 `WindowsProcessServices.def` 管理：`ExitProcess`、`RtlExitUserProcess`、标准输出句柄及同步 `WriteFile`、LastError、进程／线程标识与伪句柄、`GetCommandLineW` / `HeapReAlloc`、进程堆分配／释放／大小、动态 TLS，以及 `LoadLibraryA` / `LoadLibraryW` / `FreeLibrary` / `GetModuleHandleA` / `GetModuleHandleW` / `GetProcAddress`。提供方限定为 `kernel32.dll`、`kernelbase.dll`、`ntdll.dll` 并精确匹配导出名。直接 syscall 或伪造回调入口不能选择 API 模型。堆由进程拥有并在释放时回收；输出保留二进制字节，Win32 参数错误与不支持的异步 I/O、用户异常分开处理。指针别名会观察到完成计数的初始清零和实际返回地址的变化。

`windows.native_calls` 报告保留 DLL／函数名、声明的标量参数及可空返回位值，不伪造 NT syscall 编号。`NeverDWindowsProcessTests` 覆盖真实 x64/ARM64 PE 启动、编译器 TLS、回调修改、堆／LastError、别名、畸形元数据、权限故障和预算；`NeverDProcessPublicTests` 验证 CLI/C ABI。Windows CI 直接运行相同 EXE 作为独立行为对照，并要求 WHP 用例通过；原生 ARM64 运行证据仍需要对应机器。

`WriteFile` 的非空输入缓冲区不可读时返回 `ERROR_INVALID_USER_BUFFER`（1784），将完成计数清零，并且不输出任何字节。

`windows.defer_unmodeled` 会加载那些含有模型未实现的加载器事实的镜像，只有当执行依赖其中某一项时才停止。API 清单之外的导出和目录之外的模块被绑定到不透明入口：每个标识解析为一个地址，执行它会以 `unsupported_service` 停止并指出 `module!export`。模型不解释的目录保持不解释，文件未提供后备的元数据不在加载时读取，穿过此类镜像的基于帧的异常分发会停止。`observeProcess` 增加一个 `ProcessObserver`，它在进程启动时和每个执行监视处读取已停止的进程；它不能改变来宾状态，由它结束运行时报告 `observer`。[脱壳](unpack.md)建立在这两者之上。

[PE/COFF](https://learn.microsoft.com/windows/win32/debug/pe-format), [ARM64 ABI](https://learn.microsoft.com/cpp/build/arm64-windows-abi-conventions), [WriteFile](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-writefile), [TLS](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue), [Wine 10.0 loader](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/ntdll/loader.c). [GetProcAddress](https://learn.microsoft.com/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress).

<!-- i18n-section: verification -->

## 验证

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# 在构建共享库／CLI 时：
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

测试为两种 ISA 编译独立 ELF 入口汇编和 C，验证 data/BSS、真实启动元数据、系统调用错误、二进制输出、权限故障、部分写入、不支持的服务和跨执行量预算。TLS 用例初始化独立对齐的块、清零 TLS BSS、安装线程指针并检查切换后保留；x64 还检查 `arch_prctl` 错误不会丢失先前基址。不可用后端明确跳过。公开测试经过共享 C ABI 和 CLI，核对报告与退出码。静态 PIE 用例在自行重定位数据和函数指针前验证 auxv 和原始为零的 RELA 槽。映射测试单独检查选择分析字节源时保留 fixup；动态表测试覆盖缺失 section 及畸形／依赖输入。匿名内存用例覆盖两种 ISA 的分配、保护、空洞、重新映射、堆增长／收缩和可处理的系统调用错误；真实来宾写入验证普通和部分保护更改后的故障。x64 还在 RW/RX 切换之间重写同址代码并调用两版；同一 ELF 在 Linux 原生运行，作为独立结果／故障参照。纯内存测试覆盖预算耗尽、回收和不持有 RAM 的权威映射快照。交叉编译与 Unicorn ARM64 结果不构成原生 ARM64 KVM/WHP 证据。
