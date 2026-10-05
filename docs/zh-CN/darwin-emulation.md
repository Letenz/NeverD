**语言**: [English](../darwin-emulation.md) | [简体中文](darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 41b69637eb61adb006df941e3612df0726c8573ac4f5d79cb9f052de1ce96817 -->

[← 文档索引](README.md)

# macOS / iOS 来宾进程环境

`os/darwin/` 提供共享的 Mach-O 启动、Darwin 系统调用和内存模型；
`macos/`、`ios/` 分别定义平台契约。它们属于来宾 OS 层，HVF 属于宿主 CPU
后端，二者独立选择。
启用 `NEVERD_ENABLE_CPU_EMULATION` 即可，不需要开启 Windows 驱动模拟。

| Profile | 平台 | 架构 |
| --- | --- | --- |
| `macos-macho64-v1` | macOS | x64、ARM64 |
| `ios-macho64-v1` | iOS 设备 | ARM64 |
| `ios-simulator-macho64-v1` | iOS Simulator | x64、ARM64 |

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

本轮范围对齐现有 Linux/Windows 的有限进程模型：原始 Mach-O 字节、数据和 BSS、
段权限、`LC_MAIN` 的参数及返回、规范 `LC_UNIXTHREAD` 的启动栈、显式 argv/envp/apple
向量、退出、标准输出与错误输出、固定身份、匿名内存映射、保护和释放。
ARM64 使用 16 KiB OS 页，x64 使用 4 KiB；CPU 页表仍以 4 KiB 为基础。
`__PAGEZERO` 只保留地址，不消耗数 GiB 内存。
完整输入文件（包括未映射的元数据和尾部字节）在解析和复制前必须符合 `memory_limit`。
加载器只对普通文件做有界读取，拒绝含 NUL 的路径、短读和文件大小变化，解析独立快照，
不保留宿主文件映射。输入文件和来宾映射各有一个同值预算；宿主文件 I/O 没有硬实时保证。
Mach-O 文件尾页保留同页原始字节，后续完整虚拟页清零。初始数据、栈和匿名页都按
OS 页独立持有物理内存，因此部分解除映射能释放预算，重新分配的页面保持清零。

BSD 系统调用遵循 Darwin ABI。ARM64 从 X16 读取服务号，x64 使用 BSD 类别前缀；
错误返回正 errno 并置 carry。JSON 中的 `error` 字段区分错误和值相同的成功结果。
不会复用 Linux 的负 errno。内存保护跨空洞或最大权限边界失败时，整段原权限保持不变。
部分 `write` 复制的字节会保留，后续访存错误仍返回 EFAULT。
原始 `write` 长度超过 `INT_MAX` 时，直接返回 EINVAL，检查发生在描述符、来宾指针和
输出预算之前。此顺序已有真实 macOS 系统调用对照。

服务清单为 `exit`、`write`、`getpid`、`getppid`、`getuid`、`geteuid`、`getgid`、
`getegid`、`mmap`、`mprotect`、`munmap`。PID/UID/GID 固定为 1000，PPID 为 1。
匿名数据映射要求 `flags=0x1002`、描述符 -1 和零偏移；长度和非固定提示地址向上按 OS 页取整。
旧式原始 mmap 的零长度请求返回零而不分配内存；`MAP_UNIX03` 已支持，零长度返回 EINVAL。
Unmap/protect 地址必须对齐；允许 NONE/READ/WRITE，WRITE 隐含 READ，不接受匿名可执行映射。

设备与模拟器的 Mach-O 平台标记必须分别匹配。此版本接受不依赖动态库、重定位、
初始化函数或 TLS 的独立可执行文件。需要 dyld 链接、Mach IPC、线程、宿主文件系统、
Objective-C/Swift 运行时、Foundation/UIKit，或 arm64e/PAC 的输入会明确拒绝；
不会用空实现假装执行成功。它不等同于完整 macOS/iOS 系统，也不是 Apple 的 Simulator。

测试使用自行编写、由 Clang/LLD 生成的五种 Mach-O 平台/架构用例，另有独立构造的
线程入口和畸形文件测试、4 KiB/16 KiB 内存测试，以及 C API/CLI 报告一致性测试。
Python SDK 也通过真实共享库执行五种平台/架构组合。

## 显式文件输入与描述符

`darwin_files` 为三个 Darwin profile 提供封闭的只读普通文件目录。必填 `files` 的条目包含规范绝对来宾 `path` 和十六进制 `bytes_hex`；可选 `stdin_hex` 提供有限输入流。省略标准输入表示未知，非零读取会明确停止；空字符串表示 EOF。未配置目录时 `open` 停止，显式空目录中的缺失绝对路径返回 ENOENT。不会读取宿主路径或继承宿主输入。

新增 `open`、`read`、`pread`、`lseek`、`close`、`dup`、`dup2`、`fcntl`。read/write/open/close/fcntl/pread 的 nocancel 入口复用同一实现。支持 O_RDONLY/O_CLOEXEC，以及 F_DUPFD、F_DUPFD_CLOEXEC、F_GETFD、F_SETFD、F_GETFL。独立打开有独立游标；复制描述符共享游标，但 close-on-exec 标志独立。`pread` 不移动游标；关闭或替换 0/1/2 会影响后续 I/O，复制输出描述符保留原捕获通道与共享输出预算。

最多 256 个文件；路径及 NUL、文件和输入字节合计最多 16 MiB。路径短于 1024 字节，每个分量最多 255 字节；`descriptor_limit` 为排他上界，取值 3–4096，默认 256。JSON 保留 64 KiB 上限。非法配置及文件/祖先目录冲突在加载镜像前拒绝。

超出 INT_MAX 的读取先返回 EINVAL，再检查描述符；EOF 不访问目标地址，无效目标返回 EFAULT。目标缓冲区只有部分可写时，在写入和移动游标前明确停止。定位支持 SET/CUR/END，负位置和溢出失败保留原游标。可写文件、旧版 stat 元数据、稀疏定位和其他 fcntl 操作仍未实现。文件作为路径祖先返回 ENOTDIR。文件与 nocancel 程序及输出重定向使用同一份自编目标文件对照原生 macOS；C/CLI/Python 覆盖全部五种来宾组合。这不构成 iOS 真机验证。

2026-10-05 的 Release Darwin 验收共 381 项：177 通过、204 跳过、零失败，ARM64 HVF 必需项 51/51 实际执行。原生 macOS 7 个程序、公共 C/CLI 与报告 35 项、Python 五种来宾组合和验收脚本 66 项通过，各计数有重叠。新增文件服务尚无 Intel HVF/KVM/WHP 原生证据；Intel HVF 仍未验证且暂停 Actions。当前宿主没有 iOS SDK，也没有 iOS 真机对照。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## 目录与相对路径

可选 `directories` 条目包含规范绝对 `path` 和可选的完整 `metadata`，可声明空目录。根和祖先目录隐式存在，元数据不能创建缺失路径。目录 mode 为 `0x4000` 加权限位，size 为 [0, INT64_MAX] 内的明确观察值；不会推断时间或大小。`working_directory` 必须指向已有规范目录，省略表示 CWD 未知，不继承宿主目录。最多 256 个显式文件/目录路径（含仅声明元数据的祖先）；路径、NUL、文件、输入和 CWD 合计 16 MiB。

`openat` (463)、`openat_nocancel` (464)、`chdir` (12)、`fchdir` (13)、`fstatat64` (470) 与 open/stat64 共用解析器。相对路径使用目录 FD 或 `AT_FDCWD=-2`，绝对路径忽略 FD。重复分隔符、`.`、`..` 和末尾斜杠逐级检查：`/file/..` 为 ENOTDIR，`/missing/..` 为 ENOENT。失败保留 CWD，关闭、复用或覆盖原目录 FD 不改变 CWD。`F_GETPATH=50` 在复制描述符上仍返回规范路径和 NUL，终止符之后不写入。

目录 read/pread 即使零长度也返回 EISDIR，负 pread 偏移先返回 EINVAL。SET/CUR 共用游标，END 需要显式 size；目录 mmap 返回 EINVAL。fstatat64 支持 0、`AT_SYMLINK_NOFOLLOW=0x20`、`AT_SYMLINK_NOFOLLOW_ANY=0x800`、`AT_FDONLY=0x400`（完全忽略路径）。非法位返回 EINVAL；`AT_REALDEV=0x200` 明确不支持。流的路径和目录身份未知，权限不是访问控制模型；目录写入仍待实现。同一 `directories` 程序对照原生内核及五种 guest，原生 stat 逐字节比对文件和目录。Intel HVF Actions 保持暂停。

[XNU VFS](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/fcntl.h).

目录验收（2026-10-05，Release）：Darwin 467 项登记，227 通过、240 跳过、零失败，ARM64 HVF 必需项 60/60 执行。10 个原生 macOS 程序、37 项公共 C/CLI/报告（无跳过）、Python 五种 guest 和 66 项验收脚本通过，计数重叠。证据：`build-hvf-arm64/darwin-directory-verified-evidence/`。本轮仍无其他原生后端和 iOS 真机验收。

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## 显式目录快照

`getdirentries64` (344) 枚举已有 `directories` 条目的可选只读 `contents`；C++ 使用 `DarwinFileOptions::DirectoryContents`。`entries` 必须按明确顺序完整列出 `.`、`..` 和所有直接子项。即使空目录，缺少快照仍表示未知；快照不创建路径或 stat 元数据，也不查询宿主文件。

每项必填 `name`、非零 `inode`、`type`（0 未知、4 目录、8 普通文件）、`next_offset`、`seek_offset`。类型必须匹配路径；相同解析路径的 inode 必须与其他快照及元数据一致。`next_offset` 是目录内唯一、非零、不超过 INT64_MAX 的游标标记，无需递增；零表示回绕到开头。`seek_offset` 是独立的无符号 64 位 d_seekoff 观察值，可重复为零。整数沿用 stat 元数据的无损十进制字符串规则。

`contents.minimum_buffer_size` 必填，表示包括 EOF 在内的有效载荷下限，范围为 1–128 MiB。条目可另设 `minimum_buffer_size`（默认 0），约束从该项开始的读取。示例记录 APFS 开头两个点条目至少需要 64 字节、EOF 只需 1 字节的观察；其他位置至少容纳一个完整记录。LP64 记录按 8 字节对齐，长度为 `roundUp(25 + nameBytes, 8)`。全部快照最多 4096 项，编码字节计入 16 MiB 输入预算；仅元数据/快照声明的祖先路径去重后计入 256 路径上限，JSON 仍限 64 KiB。

独立 open 使用独立游标，dup 共享游标；仅零和声明的标记可恢复遍历，未知位置明确停止。每次返回能容纳的最大完整记录前缀。长度 >=1024 时，原请求末尾四字节为 EOF 标志（到末尾为 1，否则 0），仅记录载荷截断到 128 MiB；标志地址保留原无符号长度及回绕运算。顺序为写数据、移动游标、写读取前位置、写标志。后续 EFAULT 保留此前效果；EOF 不访问空数据缓冲区。单次复制仅部分可写时，在该次复制前明确停止，保留此前复制和游标效果。

同一 `directory-entries` 程序对照原生 macOS 的记录字段、dup/回绕、小块读取、EOF 与复制顺序；独立测试按 SDK 布局逐字节核对捕获的原生记录和长文件名。快照标记在回绕后保持固定，不模拟 APFS 每次回绕时变化的游标值。旧 `getdirentries` (196)、目录写入、其他原生后端和 iOS 真机尚未纳入验收。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"directories":[{"path":"/empty"},{"path":"/","contents":{
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":73,"type":8,"next_offset":99,"seek_offset":0}]}}]}}
```

[XNU getdirentries64](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [dirent ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [extended flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent_private.h).

目录枚举验收（2026-10-05，Release）：Darwin 共 498 项，246 通过、252 项因后端不可用跳过、零失败；ARM64 HVF 必需项 63/63 实际执行。11 个原生 macOS 程序、40 项公共 C/CLI/报告（无跳过）、Python 五种来宾组合各八种文件场景和 66 项验收脚本通过，计数有重叠。证据：`build-hvf-arm64/darwin-dirents-merged-evidence/`。Intel HVF Actions 保持暂停；其他原生后端和 iOS 真机未验收。


## 私有文件映射

`mmap` 支持普通目录文件的 `MAP_PRIVATE` 映射：`flags=0x2`，或添加 `MAP_UNIX03` 后使用 `0x40002`，文件偏移必须按 OS 页对齐。映射保留整页内的原文件字节，即使请求长度较短；EOF 尾页剩余字节清零。私有写入仅修改当前映射，不改变原文件、其他映射、固定元数据或共享描述符游标。关闭或复用描述符不影响已有映射。支持只读和 PROT_NONE 的初始内容，以及后续 `mprotect` 加写权限。

文件末尾算术溢出、UNIX03 零长度或未对齐偏移在 FD 查询前返回 EINVAL；坏 FD 在预算检查前返回 EBADF。旧式零长度仍检查 FD，再返回零且不分配。旧式未对齐偏移、流描述符、空文件页及完整越过 EOF 的页，在分配前明确停止。真实 macOS 允许映射完整 EOF 外页面，但访问触发 SIGBUS；当前模型不伪造可读零页或信号投递。共享、固定、可执行及 JIT 映射仍未支持。

`DarwinFiles` 统一解析描述符和字节，`DarwinMemory` 负责分配、权限、预算及回滚。文件仅来自 `darwin_files`。同一 `file-mapping` 程序检查私有写入、close 后寿命、游标、错误顺序与匿名页重用；独立原生对照检查非零文件偏移、整页内容及真实 SIGBUS 边界。

[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c)

### 私有映射验证（2026-10-05）

Release Darwin 门禁核对 438 个唯一登记项：210 通过、228 跳过、零失败，57/57 个 ARM64 HVF 必需项实际执行；Unicorn 覆盖五种来宾组合。9 个原生 macOS 程序通过，独立原生测试逐字节核对非零偏移的 EOF 尾页，并在隔离子进程确认下一整页触发 SIGBUS。36 项公共接口/报告测试无跳过，Python 覆盖五种组合及 `file-mapping`；66 项验收脚本与 38 项来源检查回归通过。计数有重叠。证据位于 `build-hvf-arm64/darwin-mmap-verified-evidence/`。本轮仍无 Intel HVF/KVM/WHP 或 iOS 真机验收，Intel HVF Actions 保持暂停。

## 显式文件元数据

文件条目可添加 `metadata`；提供时，下例全部字段均必填。十进制字符串保留完整整数精度，JSON 数字限于 ±(2^53−1) 内的精确整数。device 为有符号 32 位，mode/link_count 为无符号 16 位，inode 为无符号 64 位，uid/gid/flags/generation 为无符号 32 位；size 必须等于文件字节数，blocks 不超过有符号 64 位上限，block_size 为非负有符号 32 位。四个时间使用有符号 64 位秒和 0–999999999 纳秒，mode 必须匹配文件或目录类型。

`stat64` (338)、`fstat64` (339)、`lstat64` (340) 在 ARM64/x64 返回同一 144 字节 LP64 记录。路径解析与 open 共用；FD 查询遵循复制和关闭，既不分配描述符，也不改变游标。普通文件 rdev、填充和保留字段清零。元数据是调用方的固定观察值；读取不推进时间，mode 不改变目录访问授权。缺少元数据、流状态、符号链接、旧版 stat和扩展安全查询明确不支持。路径/FD 错误先于目标地址检查，部分可写目标在任何写入前停止。原生测试逐字节对比真实文件状态并核对 SDK 布局，同一自编原始程序验证三种系统调用。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","metadata":{
  "device":1,"inode":"18364758544493064720","mode":33188,"link_count":1,
  "uid":1000,"gid":1000,"size":10,"block_size":4096,"blocks":8,
  "flags":0,"generation":0,
  "access_time":{"seconds":-1,"nanoseconds":1},
  "modification_time":{"seconds":2,"nanoseconds":3},
  "change_time":{"seconds":4,"nanoseconds":5},
  "birth_time":{"seconds":6,"nanoseconds":7}
}}]}}
```

[XNU stat.h](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/sys/stat.h)

### 元数据验证与剩余工作（2026-10-05）

加入 stat64 后的 Release 验收共 409 个唯一登记项：193 通过、216 跳过、零失败；54/54 项 ARM64 HVF 必需测试实际执行，Unicorn 覆盖五种来宾组合。SDK 布局与真实文件整条记录对比、8 个原生 macOS 程序、36 个公共接口/报告测试（无跳过）、Python 五种组合和 66 个验收脚本测试均通过，计数有重叠。原生测试改为每例独立输出文件，修复短输出残留旧尾字节的问题。新增能力仍无 Intel HVF/KVM/WHP 或 iOS 真机证据。

后续优先顺序：先补共享映射与 EOF 缺页、有界写入，验证 EOF 页、close 后映射寿命与错误顺序；再补显式时间/系统信息、必要 Mach/线程服务和 Mach-O 依赖、重定位/绑定、初始化/TLS；最后通过真实程序推进 Objective-C/Swift 与 Foundation/UIKit。iOS 真机对照需要 SDK 和设备环境；Intel HVF 尚未验证，Actions 继续暂停。



```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

HVF 必需门禁也包含这些用例。完整支持边界、来源和命令见[英文说明](../darwin-emulation.md)。
Intel HVF 的 10 项原生 transport 和全部 26 个 Darwin 工作负载均已通过，完整 CPU 清单仍单独验收；当前状态见
[HVF 验证记录](macos-hvf.md)，不能把内核参考程序成功当作后端通过。

新增的完整原生工作负载门禁要求本机架构的每个 Darwin 进程用例都实际通过：
ARM64 三个平台共 69 项，x64 的 macOS 和 Simulator 共 46 项。
每种平台都必须执行 `LC_MAIN` 和独立编写的 `LC_UNIXTHREAD` 程序；源码清单回归确保
以后新增的 Darwin 进程用例也进入必需集合。
macOS 本机构建还会把同一份自编目标文件链接为宿主参考程序，对照返回、退出、内存保护/
重用和超长写入的错误顺序，检查实际退出状态和逐字节输出。只有宿主程序为真实 dyld
入口链接 libSystem，来宾镜像仍不依赖动态库。这项原生对照是 HVF 门禁必需项，
不代表 iOS 真机执行证据。
独立的[原生内核工作流](../../.github/workflows/darwin-kernel-reference.yml)还会直接在
Intel 与 Apple Silicon macOS 上执行这些程序，无需构建 NeverD 或 LLVM。
`DarwinNativeCases.def` 统一维护 C++ 测试和独立宿主脚本的模式、退出状态及期望输出；
架构不符、Rosetta、超时或结果不符均失败，JSON 保留源码、系统和编译器信息。
`scripts/run_native_cpu_ci.py --require-darwin-backend hvf` 可以在本机验收；
Linux 使用 `kvm`，Windows 使用 `whp`，同时传入 `--build` 和 `--evidence` 路径。
```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

缺少用例注册、跳过必需用例或缺少 `ld64.lld` 均不能通过。
[原生 Darwin 专项工作流](../../.github/workflows/darwin-native.yml)提供关闭 Unicorn
后的 x64 KVM/WHP 构建和验收；它需要 runner 实际提供虚拟化能力，保留完整测试证据。
工作流也可以单独选择 `kvm` 或 `whp`。

## 本机验证（2026-10-03）

Apple M4 Max / macOS 15.6.1，Release 构建，源码为
`36e11ca8a3d80aecf585d3328018839ce7fdb989`：

| 范围 | 通过 | 失败 | 跳过 | 必需原生用例 |
| --- | ---: | ---: | ---: | ---: |
| 完整 HVF 门禁，关闭 Unicorn，20 个目标 | 834 | 0 | 5,903 | 13 / 13 |
| 每个 Darwin 工作负载，关闭 Unicorn | 65 | 0 | 221 | 39 / 39 |
| Darwin 与公开 C API/CLI，包含 Unicorn | 138 | 0 | 156 | — |

统计有重叠，不能相加。原生摘要记录干净源码，没有缺失注册或未执行的必需用例。
跳过项属于异架构、其他后端或关闭的软件后端。65 项 Darwin 检查包含加载器、内存、
每个 ARM64 工作负载，以及真实宿主内核对照。加入原生中断清单后，完整 HVF 门禁
现要求 ARM64 的 16 项或 Intel 的 14 项检查；Darwin 专项门禁另要求全部 39 / 26 项进程工作负载。
后续干净源码 `561ebf37b9eaaec08043ac5816b2e083ecccaf68` 的 ARM64 完整门禁达到
841 项通过、0 失败、5,939 项跳过，16 个必需用例全部执行；完整证据位于
`build-hvf-native/hvf-cancellation-full-evidence/`。

证据位于 `build-hvf-native/hvf-release-evidence/`、
`build-hvf-native/darwin-release-evidence/` 和
`build-hvf/verification/darwin-release-public.xml`。回归覆盖未映射尾部字节的文件预算、
各平台的线程入口、超长写入错误顺序及格式化换行后的测试清单解析。
原生清单、结果核对和 CI 脚本的 106 项回归通过；能力、文档、来源及格式检查也通过。

后续同步 `dev` 后，干净源码 `f4bf8dde5cbc33d18ce053cb0722a54047e5a2d0`
再次通过独立 ARM64 Darwin 门禁：65 项通过、0 失败、221 项跳过，39 个必需原生
工作负载全部执行。证据位于 `build-hvf-native/hvf-final-dev-darwin-evidence/`。
这次复跑验证 Darwin 目标，不代表同期合入的其他 Windows 进程改动已经通过新的完整 CPU 门禁。

后续干净源码 `d5864c055116a687546320e4acf0788ef4a4e735` 的方法级执行再次通过
39 个 ARM64 Darwin 工作负载（65 通过、221 跳过），286 个 CTest 身份与原始 GoogleTest
XML 全部核对一致。相同源码的完整 20 个目标覆盖 6,842 项，849 通过、0 失败、5,993 跳过，
16 个必需原生项全通过；包含后续 Windows 环境变更，摘要明确记录方法级进程隔离。
证据位于 `build-hvf-native/hvf-method-clean-{full,darwin}-evidence/`。

较早的集成验证已覆盖五种平台/架构的 Python SDK 调用；包内引擎与关闭 Unicorn 的
CLI 在三种 ARM64 平台的 18 个场景中报告一致。桌面包通过 186 个 Mach-O 的依赖、
签名和 Cocoa 启动检查。同时关闭 HVF 与 Unicorn 的配置通过 38 项检查、跳过 231 项
后端用例，且没有 Hypervisor.framework 依赖；这属于构建隔离证据。
提交 `e078b129c` 的[桌面 GUI 工作流](https://github.com/NeverSight/NeverD/actions/runs/37053518872)
也在 macOS、Windows、Ubuntu 三个平台通过。

## 托管宿主原生验证（2026-10-03）

[Intel HVF Darwin 门禁](https://github.com/NeverSight/NeverD/actions/runs/37106013999)
在干净源码 `8dcc74c59da303176801b99747a60339161b824b` 上通过 macOS 与 iOS Simulator
全部 **26/26** 个 x64 原生工作负载。286 个 CTest 身份均与原始 GoogleTest XML 核对一致：
**52 通过、0 失败、234 跳过**，无缺失、重复或未执行的必需项。跳过项分别为 65 个关闭的
Unicorn、39 个 ARM64 来宾和 130 个其他宿主后端；32 个方法进程全部正常退出。
原始程序与 macOS 宿主内核的对照也通过；这不代表 iOS device 内核或更广泛的 Intel CPU 验收。

产物 `11267489438` 已下载，SHA-256 为
`cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`，与 GitHub 元数据一致。
本地证据位于 `build-hvf/verification/hvf-intel-darwin-accepted/`。托管 Intel 使用已说明的
方法级串行执行策略，保留全部参数与 CTest 环境；宿主为四个逻辑 CPU 的 macOS x86-64、Darwin 24.6.0。
同一轮还通过 10 项 transport、100 次中断恢复和独立 CR8 回归；这些重叠检查不加入 Darwin 总数。

两个 x64 后端都在关闭 Unicorn 后通过最新 Darwin 专项门禁，包含文件预算、独立线程
入口及超长写入回归。源码均为 `36e11ca8a3d80aecf585d3328018839ce7fdb989`，
macOS 与 iOS Simulator 共 26 项必需进程用例全部执行成功：

| 宿主 / 后端 | 通过 | 失败 | 跳过 | 必需原生用例 |
| --- | ---: | ---: | ---: | ---: |
| [Windows / WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703/job/111023370601) | 51 | 0 | 235 | 26 / 26 |
| [Ubuntu 24.04 / KVM](https://github.com/NeverSight/NeverD/actions/runs/37062839703/job/111023370857) | 51 | 0 | 235 | 26 / 26 |

产物 `darwin-native-whp-x64` 和 `darwin-native-kvm-x64` 保留完整清单、JUnit、CTest
日志及源码/宿主摘要。两个工作区均干净，没有缺失或未执行的必需用例；下载产物的
SHA-256 已与 GitHub 摘要核对。跳过项包括仅适用于 macOS 的内核对照、异架构和其他后端。

这些结果验证所列传输上的有限 Darwin 模型。Intel Mac HVF 和各后端更广泛的 CPU
行为仍需各自的门禁验证。

## 独立 macOS 内核对照（2026-10-03）

提交 `e727d3eab7086063bb392444bd55014ac48d43c3` 的原始程序使用 Apple Clang 17.0.0
编译后，在两种 macOS 15.7.9 宿主上直接执行成功：

| 宿主架构 | 原生工作负载通过 |
| --- | ---: |
| [Apple Silicon ARM64](https://github.com/NeverSight/NeverD/actions/runs/37064795867/job/111029848093) | 4 / 4 |
| [Intel x86-64](https://github.com/NeverSight/NeverD/actions/runs/37064795867/job/111029847805) | 4 / 4 |

两个摘要均记录干净源码，`return`、`exit`、`memory`、`write-length` 都返回 37，
输出逐字节匹配且 stderr 为空；产物摘要也已核对。这为两种架构的原始工作负载提供
独立内核语义依据；模拟执行仍由上面的 HVF/KVM/WHP 结果覆盖。

## 显式时间观察值

`ProcessOptions::DarwinTime` / `darwin_time` 为所有 Darwin profile 提供 raw `gettimeofday` (116) 的固定观察值，包括第三个 `mach_absolute_time` 输出。`time_of_day`、`timezone` 和 `mach_absolute_time` 都可省略；省略表示未知，显式零是有效值，空对象不会补默认时钟。模型不读取宿主时钟、不推断时区、不推进时间，也不换算绝对 tick。

提供一个记录时，其全部成员必须齐全。`seconds` 是无符号 32 位，`microseconds` 为 [0, 999999]，`minutes_west` / `dst_time` 为有符号 32 位，绝对 tick 为无符号 64 位。JSON 使用共享的无损整数规则：超出安全整数范围时使用十进制字符串。未知字段、越界值及非 Darwin profile 在加载镜像前拒绝。

LP64 `timeval` 占 16 字节：偏移 0 是零扩展的秒数，偏移 8 是 32 位微秒，偏移 12 的四字节填充为零。时区是两个有符号 32 位字段，tick 占八字节。日历时间和绝对时间先联合采样，因此被请求的两种观察值必须在任何复制或指针检查前都存在。随后依次写入 timeval、timezone、absolute ticks。时区缺失在自己的阶段停止，保留已写入的 timeval；后续 EFAULT 同样保留此前写入，重叠地址遵循相同顺序。单次输出仅部分可写时，在该次复制前明确停止，保留更早的复制。三个空指针无需配置即可成功；选择性查询仅要求所请求的值。

原始 `time` 工作负载核对原生行为；来宾 `time-values` 在五种来宾组合的 C/CLI/Python 路径输出配置的精确 32 字节。独立 SDK 对照从一次原生 raw 调用采集三输出，再逐字节比较。这不包含自动推进时钟、tick 换算、commpage 计数器、定时器或 Mach 时钟对象/IPC；dyld、线程、Objective-C/Swift 和 Foundation/UIKit 仍需完善。Intel HVF Actions 保持暂停，本轮不增加原生 Intel 或实体 iOS 验收结论。

```json
{"darwin_time":{"time_of_day":{"seconds":4045620583,"microseconds":654321},"timezone":{"minutes_west":-480,"dst_time":-1},"mach_absolute_time":"18364758544493064720"}}
```

[XNU gettimeofday](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_time.c), [time ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/time.h).

时间验证（2026-10-06，Release）：538 项 Darwin 注册测试，274 项通过、264 项因后端不可用跳过，零失败；66/66 项必需 ARM64 HVF 测试全部执行。12 个原始原生 macOS 工作负载及单次采样的 SDK 字节对照通过。公共 C/CLI/报告为 43/43，无跳过。Python 的五种来宾组合通过，包括精确时间字节及原有八种文件场景。66 项运行器测试、本地化、能力清单和格式检查通过。计数有重叠。证据：`build-hvf-arm64/darwin-time-verified-evidence/`、`darwin-time-native-first/`、`darwin-time-public.xml`。

## Mach 时间与返回约定

`darwin_time.timebase` 显式提供 `numerator` 和 `denominator`，均为非零无符号 32 位数；比例原样保留，不约分或换算。`mach_timebase_info_trap` 的索引为 89：ARM64 的 X16=-89，x64 的 RAX=0x01000059。输出为小端分子、分母共八字节，返回零。与 XNU 一致，完全无效的输出地址也返回零；部分可写输出在复制前明确停止，因为尚未建模其前缀写入。底层传输错误继续传播。缺少 timebase 时先停止，再检查指针，空指针也一样。

ARM64 特殊调用 X16=-3、X16=-4 分别返回完整无符号 64 位 `mach_absolute_time`、`mach_continuous_time`。各自只需要对应观测值，显式零有效。x64 的对应 Mach 表项会在原生内核触发 EXC_SYSCALL，因此模型明确拒绝。这不实现自动推进、commpage、定时器或 Mach 时钟对象/IPC。

服务分派只使用编号寄存器的低 32 位，报告保留原始 64 位。ARM64 负数选择 Mach；x64 的 Mach 类别为 0x01000000，BSD 类别为 0x02000000。命名空间独立，BSD 3/4 仍为 read/write；未知编号和其他架构的类别明确停止。解析后的绑定统一拥有返回约定：Mach 保留标志位和 X1/RDX，BSD 仍遵循 carry 和次结果规则；x64 仍更新 SYSCALL 返回所需的 RCX/R11。Mach 返回记录含 `result`，始终省略 `error`，即使输入 carry 已置位。

原始 `mach-time` 工作负载对照 ARM64 原生内核的标志位、次结果、高位编号、坏指针和 BSD 切换。`mach-timebase-values` 在五种来宾组合输出精确配置字节，`mach-clock-values` 在 ARM64 验证完整 tick；独立 SDK 对照验证布局和原生比例。Intel HVF Actions 继续暂停；x64 软件测试及语法检查不代表原生 Intel 或实体 iOS 验收。

```json
{"darwin_time":{"timebase":{"numerator":125,"denominator":3},"mach_absolute_time":"18364758544493064720","mach_continuous_time":"18446744073709551615"}}
```

[XNU clock traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/clock.c), [ARM64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [ARM64 special traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/sleh.c), [x64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/x86_64/idt64.s).

Mach 验证（2026-10-06，Release）：569 项唯一 Darwin 注册，293 通过、276 项后端不可用而跳过、零失败；69/69 项必需 ARM64 HVF 全部执行。最终运行通过全部 13 个原生工作负载及两个时间 SDK 对照。公开 C/CLI/report 为 100/100、无跳过；Python 覆盖五种来宾组合。公开接口比较按平台和场景独立运行，显式给出 10 秒来宾预算；产品默认值和超时回归保持不变。计数有重叠。

早期原生程序首次启动超过已有 5 秒门限：独立测得首次 6.056 秒、复用后 0.010 秒。同一二进制随后在原门限下通过 13 项，失败记录保留。宿主负载下的墙钟超时经独立串行复核通过。证据：`build-hvf-arm64/darwin-mach-time-final-evidence/`、`darwin-mach-time-native-recheck/existing-binary-recheck.json`、`darwin-mach-time-public-accepted.xml`，记录的是提交前工作树。ARM64 MRS/MSR NZCV 尚未纳入受检查 CPU 契约，测试用整数指令设置并读取四个标志位。这项 CPU 缺口，以及可写文件、系统信息、dyld/运行时/框架、实体 iOS 仍待完成。
