**语言**: [English](../darwin-emulation.md) | [简体中文](darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: adbad5fe5176c4b6c69ffc7eb9b7a9f7f981923e7eaf69b29400f0493cc9d380 -->

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

`darwin_files` 为三个 Darwin profile 提供封闭的初始只读普通文件目录。必填 `files` 的条目包含规范绝对来宾 `path` 和十六进制 `bytes_hex`；可选 `stdin_hex` 提供有限输入流。省略标准输入表示未知，非零读取会明确停止；空字符串表示 EOF。未配置目录时 `open` 停止，显式空目录中的缺失绝对路径返回 ENOENT。不会读取宿主路径或继承宿主输入。

新增 `open`、`read`、`pread`、`lseek`、`close`、`dup`、`dup2`、`fcntl`。read/write/open/close/fcntl/pread 的 nocancel 入口复用同一实现。支持 O_RDONLY/O_CLOEXEC，以及 F_DUPFD、F_DUPFD_CLOEXEC、F_GETFD、F_SETFD、F_GETFL。独立打开有独立游标；复制描述符共享游标，但 close-on-exec 标志独立。`pread` 不移动游标；关闭或替换 0/1/2 会影响后续 I/O，复制输出描述符保留原捕获通道与共享输出预算。

最多 256 个文件；路径及 NUL、文件和输入字节合计最多 16 MiB。路径短于 1024 字节，每个分量最多 255 字节；`descriptor_limit` 为排他上界，取值 3–4096，默认 256。JSON 保留 64 KiB 上限。非法配置及文件/祖先目录冲突在加载镜像前拒绝。

超出 INT_MAX 的读取先返回 EINVAL，再检查描述符；EOF 不访问目标地址，无效目标返回 EFAULT。目标缓冲区只有部分可写时，在写入和移动游标前明确停止。定位支持 SET/CUR/END，负位置和溢出失败保留原游标。旧版 stat 元数据和其他 fcntl 操作仍未实现。文件作为路径祖先返回 ENOTDIR。文件与 nocancel 程序及输出重定向使用同一份自编目标文件对照原生 macOS；C/CLI/Python 覆盖全部五种来宾组合。这不构成 iOS 真机验证。

2026-10-05 的 Release Darwin 验收共 381 项：177 通过、204 跳过、零失败，ARM64 HVF 必需项 51/51 实际执行。原生 macOS 7 个程序、公共 C/CLI 与报告 35 项、Python 五种来宾组合和验收脚本 66 项通过，各计数有重叠。新增文件服务尚无 Intel HVF/KVM/WHP 原生证据；Intel HVF 仍未验证且暂停 Actions。当前宿主没有 iOS SDK，也没有 iOS 真机对照。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## 已有文件的可写内容

每个文件可用严格布尔值 `"writable":true` 显式允许进程内修改；C++ 使用 `DarwinFileOptions::WritableFiles`。省略或 false 保持只读，未知写授权明确停止，不访问宿主或修改输入选项。write(4/397)、pwrite(154/415)、truncate(200)、ftruncate(201) 和 O_TRUNC 共用文件节点；独立 open 共享字节但游标独立，dup 共享游标与状态，最后 close 后重开仍保留内容。增长补零，截断不移动游标，O_RDONLY|O_TRUNC 也截断。

F_SETFL 只改变 O_APPEND，保留访问模式、close-on-exec 和 FWASWRITTEN；F_GETFL 在实际传输非零字节后暴露 0x10000，包括 pwrite 和输出捕获。pwrite 忽略 append、不移动游标。超 INT_MAX 的长度在 FD 检查前返回 EINVAL，pwrite 偏移 -1 更早返回 EINVAL；INT64_MAX 偏移在零写前返回 EFBIG，长度先裁剪再选择追加位置。

成功的 ftruncate（包括大小不变）也为调用描述及其 dup 设置 FWASWRITTEN；O_TRUNC 为新描述设置，包括 O_RDONLY。按路径 truncate 不改变已有描述的标志。

部分可读输入在任何效果前停止；整段 EFAULT 保留字节，非空追加仍把游标移至 EOF。传输后端失败不提交内容或游标。若未配置 `mutation_policy`，非零成功写、截断和非零整段 EFAULT 都使完整 stat 观察失效，因为失败追加也可能改变时间戳；后续 stat 在输出前明确停止。零写保留元数据。16 MiB 是路径/NUL、输入、目录记录、CWD 与当前文件内容的合计逻辑预算，可写路径引用也计入；缩小替换 backing 并回收容量。调用方原始内容和一个有界替换缓冲区属于额外存储。已知 inode 别名及 immutable/append-only 标志暂拒绝。

DarwinMemory 持有映射租约；所有映射区间解除前，write、truncate 和 O_TRUNC 均停止，包括 PROT_NONE 和 FD 已关闭的映射。失败映射和旧式零长度映射不留租约；新映射读取当前内容。只写 FD 直接请求 READ/WRITE mmap 返回 EACCES，PROT_NONE 可成功并经 mprotect 获得读写权限。

原生 writable-files 与 nocancel 程序比较字节、游标、标志和错误顺序；单元测试覆盖 4K/16K，C/CLI/Python 覆盖五种组合。权限强制检查、目录删除、跨父目录重命名、硬链接、真实文件系统的元数据更新、映射一致性和 EOF SIGBUS 仍待实现。完整 macOS/iOS 目标尚未完成，iOS 真机与 Intel HVF 仍无验收，Intel Actions 保持暂停。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233","writable":true}]}}
```

[XNU write](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU vnode](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c).

## 显式可变元数据

文件可在 `writable: true` 与完整 `metadata` 旁配置 `mutation_policy`；C++ 使用 `DarwinFileOptions::MutationPolicies`。这是明确选择的虚拟稀疏分配策略，不推断 APFS 行为，也不读取宿主时钟；省略时仍保留修改后元数据未知的契约。

`allocation_unit` 与 `mutation_time` 及其中 seconds/nanoseconds 均必填，整数沿用无损规则。分配单位必须为 512 字节至 16 MiB 的二次幂，独立于 block_size 和 VM 页。要求普通文件权限（无 set-id/sticky）、flags=0、link_count=1，以及密集初始分配：blocks=ceil(size/allocation_unit)*(allocation_unit/512)。密集是调用方的明确断言，零字节不表示洞。策略路径引用计入 16 MiB 逻辑预算；blocks 是独立的虚拟分配账本，不据此虚构 ENOSPC。

写入分配所有触及的单位，向洞写零也分配。truncate 增长只补零，不分配；缩小时丢弃向上取整 EOF 之后的单位，保留已分配的末尾部分单位。重新增长不会恢复已丢弃的分配。成功的非零写以及每次成功截断（含同大小及空文件 O_TRUNC）更新 size/blocks，并把 mtime/ctime 设为固定输入时间；其他字段不变，读取不推进 atime。路径查询、独立 open、dup 与重开共享节点状态，初始输入不变。

零写、预算/映射拒绝、部分输入拒绝及后端失败保留已知状态。非零整段 EFAULT 仍使元数据失效，后续成功写或截断不能恢复它；stat 输出失败不改变节点。virtual-file-metadata 来宾程序通过五种组合及 C/CLI/Python 检查完整 144 字节记录；分配结果是策略测试，不是原生 APFS 等价证据。原生可写程序另行验证标志、游标和错误顺序。命名空间修改、真实文件系统一致性、Mach 服务与动态运行时加载仍待完成。

```json
{"mutation_policy":{"allocation_unit":4096,"mutation_time":{"seconds":-7,"nanoseconds":123456789}}}
```



## 稀疏文件定位

普通文件配置 mutation_policy 且分配状态仍已知时，lseek 支持 SEEK_HOLE=3、SEEK_DATA=4，读取与 stat 相同的单位账本。首次修改前初始文件明确为密集分配，零字节也不表示洞。输入位于所求类型单位内时返回原偏移，否则返回下一匹配单位起点；末尾洞从 EOF 开始。负偏移返回 EINVAL，到达/越过 EOF（含空文件）或找不到后续数据返回 ENXIO=6。错误保留游标，成功只改变当前 open 描述及其 dup；独立 open 保留自己的游标，重开读取当前分配。元数据、标志和字节不变，whence 高位忽略。

未配置策略、目录以及整段 EFAULT 后永久未知的分配仍明确拒绝。不能由零值或被拒绝的写入/增长推断新分配。自编 sparse-file-seek 原生/来宾程序验证错误、已写字节、EOF 和描述符生命周期，不假设更早的文件系统区段位置；virtual-file-metadata 单独验证精确虚拟几何，C/CLI/Python 覆盖全部五种组合。

[XNU lseek](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## 删除普通文件名称

目录的 `mutable:true`（C++ `MutableDirectories`）显式授权修改直接子项名称，与文件内容的 `writable` 独立；下例允许删除只读文件。缺少授权会明确停止，不根据权限位虚构凭据或 EACCES。准入拒绝父目录/直接普通子文件的非零已知 flags、父目录特殊权限位、子文件 link_count 不为 1，以及父目录或子文件的已知身份别名。身份检查合并 stat 和目录快照的 inode；明确不同的设备保持独立。授权路径计入现有预算。

`unlink(10)` / `unlinkat(472)` 删除现有普通名称；unlinkat 仅接受低 32 位 flags=0 或 `AT_SYMLINK_NOFOLLOW_ANY=0x800`。未知位先返回 EINVAL；已知的删除目录/系统丢弃模式仍不支持。共用解析器保留路径故障、目录 FD、CWD 和绝对路径的优先级；缺失为 ENOENT，文件后斜杠为 ENOTDIR，普通目录为 EPERM，来宾根目录为 EBUSY。原生探针也检查末尾 `.` / `..`。

名称删除后，旧 FD、dup、独立打开对象仍保留数据、游标和状态标志，新打开返回 ENOENT；隐式父目录与 CWD 继续存在。F_GETPATH 保留已捕获旧路径，与原生对照一致。写授权属于文件对象。只有所有描述符和最后一段映射均释放后，close/dup2 或下一次修改才回收当前文件字节预算；初始路径/引用费用仍保留。权限强制检查、跨父目录重命名、硬链接、目录删除仍待实现。

成功删除使直接父目录的 stat/列举观察失效，涵盖旧/新 FD、dup 和路径查询；stat/readdir/SEEK_END 在复制和移动游标前停止。read/pread 仍为 EISDIR，SET/CUR、F_GETPATH、fchdir、相对查找继续可用。有仍可信的文件修改策略时，仅将 nlink 改为 0、ctime 改为固定时间，保留 mtime/atime、数据和分配；后续写入不能恢复 nlink=1。缺少策略或曾发生整段 EFAULT 时，完整文件元数据仍未知。失败不改变状态。原生 `unlinked-file` 比较名称/描述符行为；策略时间与目录失效是明确的模型规则。

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"00"}],"directories":[{"path":"/work","mutable":true}]}}
```

[XNU unlink / unlinkat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [F_GETPATH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c).

## 创建普通文件

`O_CREAT=0x200` 只在显式 mutable 的直接父目录中创建空普通文件，普通/nocancel open、openat 共用实现。新对象获得内容写授权；已有对象仍使用独立的 WritableFiles 授权。只读 FD 可以创建但不能写入。未提供创建策略时，新对象的 stat64 和稀疏定位仍明确未知；新对象始终不继承同名旧对象的 metadata/mutation_policy。

O_CREAT 配合 `O_EXCL=0x800` 对已有文件或目录先返回 EEXIST，不截断、不检查写授权或映射；O_EXCL 单独无效。已有目录可用只读 O_CREAT 打开。顺序为无效访问模式→FD 容量→O_CREAT|O_DIRECTORY 的 EINVAL→路径访问。只能创建原始路径的最后缺失分量；缺失祖先及末尾 `/`、`//`、`/.`、`/..` 仍为 ENOENT。新文件的 O_CREAT|O_TRUNC 不设置 FWASWRITTEN，截断已有文件则设置。

只有实际插入才使父目录 stat/枚举失效。同名重建与仍打开或映射的旧对象拥有独立字节、元数据、描述和映射租约。256 项上限计入固定初始非文件项、具名对象及仍存活的孤立对象；新对象的规范路径/NUL 和当前字节计入 16 MiB，删除且最后 FD/映射释放后才回收动态费用。关闭具名对象不释放名额，初始输入/引用费用仍保留。预算不足或规范路径达到 1024 字节会明确停止，不虚构 ENOSPC 或原生路径错误；失败不留下名称或 FD。

原生 created-file 与五种来宾组合对照，4K/16K 测试覆盖精确容量、失败原子性和旧映射存活时重建。权限强制检查、跨父目录重命名、链接及目录修改仍待完善。

[XNU open](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

## 显式创建元数据与进程 umask

可选 `darwin_files.umask`（C++ `InitialUmask`）单独声明初始进程掩码，范围为八进制 0..07777，不要求创建授权。`umask(60)` 返回旧掩码、保存输入的低 07777 位，不访问来宾内存，也不需要空闲 FD。省略表示未知，不读取宿主或猜测默认值。掩码只初始化一次，只影响后续创建，不改变调用方输入。下例十进制 18 即八进制 0022。

可选 `darwin_files.creation_policy`（C++ `CreationPolicy`）为新对象提供完整元数据。严格对象恰含 `first_inode`、`block_size`、`generation`、`creation_time`、`mutation_policy`，时间及修改策略复用既有格式。必须显式提供 umask，至少授权一个 mutable 父目录，且每个授权父目录都有完整 metadata。block_size 为 1..INT32_MAX，generation 为 uint32；分配单元是 512..16 MiB 的二次幂，独立于块大小和 VM 页，纳秒须在 [0,1000000000)。first_inode 是非零 uint64，严格大于所有 stat/快照中的 inode，包括其他设备；超出 JSON 精确整数范围时使用十进制字符串。

只有成功插入新名称才消耗全局递增 inode；成功使用 UINT64_MAX 后永久耗尽，关闭、删除、同名重建、umask 或后续查找都不能重置。排他、FD、路径、条目或字节预算失败不留下名称、FD 或计数器增量，打开已有 O_CREAT 也不消耗编号。新 stat64 的 device/GID 继承直接父目录，UID 为固定来宾有效用户 1000，mode 为 `S_IFREG | (mode & 0777 & ~umask)`，nlink=1，size/blocks/flags=0；块大小、generation 和四个初始固定时间来自策略。父目录完整 stat/列举失效后，仍可使用不变的 device/GID，但不会恢复完整记录。

新节点独立持有元数据与分配状态，不继承同名旧对象记录。后续写入、截断和删除共用修改策略，保留 inode/mode/birthtime 和已删除对象的 nlink=0；整段 EFAULT 后的未知状态仍不可恢复。策略不追溯修改已有节点。原生 `created-file-metadata` 对照权限、掩码返回值、有效 UID、父设备/组和身份存活，覆盖五种来宾组合；`virtual-created-metadata` 单独比较完整 144 字节记录。原生四个创建时间不一定相等；固定时间及稀疏分配是明确的虚拟文件系统规则。权限强制检查、凭据切换、ACL 和原生 APFS 元数据行为仍待实现。

```json
{"darwin_files":{"files":[],"umask":18}}
```

[XNU creation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU umask](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c). [XNU rename / renameat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## 同一父目录中的普通文件重命名

`rename(128)`、`renameat(465)`、`renameatx_np(488)` 可在同一明确可修改的直接父目录中改名或覆盖普通文件。同名空操作也需要授权，通过后保留全部观察。低 32 位 flags 支持 0 或 `RENAME_NOFOLLOW_ANY=0x10`；未知位及 EXCL+SWAP 在读取路径前返回 EINVAL，其余已知标志明确不支持。共用解析器保留源先于目标、目录 FD、原始斜杠与点组件的错误顺序。目录源立即报告不支持；普通文件目标的末尾点/双点在解析成功后、挂载与授权检查前返回 EINVAL，涵盖嵌套及其他父目录。缺失或非目录祖先仍优先报原错误；获准父目录内的普通目录目标为 EISDIR。

源对象的独立打开、dup 和旧 FD 的 `F_GETPATH` 一起跟随新名；被覆盖对象保留最后关联路径、字节、游标、标志与映射寿命。目标的写授权或元数据不会转移给源。可信策略只改各自 ctime，被覆盖者另设 nlink=0；身份、所有权、创建时间、数据和分配归原对象。缺少策略或整段 EFAULT 后完整元数据仍未知。实际改名使父目录 stat/列举观察失效，同名空操作不改变观察。

改名不消耗新 inode、目录项或空闲 FD。新规范路径/NUL 替换源的动态路径费用，初始输入费用始终保留。只有没有旧描述符和映射持有的目标才能提前计入可回收容量，并仅回收一次；部分 unmap 仍保留整对象费用。规范路径达到 1024 字节或总量超过 16 MiB 时，在修改名称、元数据和描述符前停止。

跨父目录、已知设备冲突、目录移动、swap/exclusive/seclude 和权限强制检查仍待实现。相同 stat 设备号不能证明同一挂载，模型不会猜测 EXDEV。原生/来宾 `renamed-file` 对照身份、路径、覆盖和映射；策略时间与预算属于显式虚拟规则。


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

`stat64` (338)、`fstat64` (339)、`lstat64` (340) 在 ARM64/x64 返回同一 144 字节 LP64 记录。路径解析与 open 共用；FD 查询遵循复制和关闭，既不分配描述符，也不改变游标。普通文件 rdev、填充和保留字段清零。输入提供初始元数据，可选修改策略决定后续变化；读取不推进时间，mode 不改变目录访问授权。缺少元数据、流状态、符号链接、旧版 stat和扩展安全查询明确不支持。路径/FD 错误先于目标地址检查，部分可写目标在任何写入前停止。原生测试逐字节对比真实文件状态并核对 SDK 布局，同一自编原始程序验证三种系统调用。

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
ARM64 三个平台共 93 项，x64 的 macOS 和 Simulator 共 62 项。
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

早期原生程序首次启动超过已有 5 秒门限：独立测得首次 6.056 秒、复用后 0.010 秒。同一二进制随后在原门限下通过 13 项，失败记录保留。宿主负载下的墙钟超时经独立串行复核通过。证据：`build-hvf-arm64/darwin-mach-time-final-evidence/`、`darwin-mach-time-native-recheck/existing-binary-recheck.json`、`darwin-mach-time-public-accepted.xml`，记录的是提交前工作树。当时 ARM64 MRS/MSR NZCV 尚未纳入受检查 CPU 契约，因此测试用整数指令观察标志。下述增量已补上这项 CPU 缺口。

## ARM64 条件标志寄存器

共享的受检查 ARM64 契约在 EL0、EL1 准入精确的 `MRS Xt, NZCV` 和 `MSR NZCV, Xt` 编码。读取只返回第 31–28 位；写入只取输入的这四位，其余位忽略。读到 `XZR` 会丢弃结果；从 `XZR` 写入会清空四个标志，不会读取 SP。各后端执行原始指令。宿主寄存器设置接口的验证、FPCR/FPSR 的受限策略保持不变；邻近且未列出的系统寄存器仍明确不支持。

`NeverDAArch64NZCVTests` 将全部标志组合与宿主原始指令对照，并检查完整标量/向量状态、内存、寄存器边界、观察器停止/失败、上下文恢复重试及共享指令预算。ARM64 `mach-time` 测试现在以真实 MSR/MRS 包围 SVC，覆盖 Mach 标志保持及返回 BSD 的切换。原生 HVF 必需项包含两种特权下的六个方法和宿主对照。ARM64 KVM/WHP、实体 iOS 尚未验证。可写文件、系统信息、推进时钟、Mach IPC/线程、dyld/运行时/框架及设备验收仍是后续环境工作。

[Arm NZCV (DDI0601, 2025-06)](https://developer.arm.com/documentation/ddi0601/2025-06/AArch64-Registers/NZCV--Condition-Flags).


可写文件验证（2026-10-06）：Release Darwin 610 项中 322 通过、288 不可用后端跳过、零失败；ARM64 HVF 必需项 72/72 执行。最终专项 114 项中 102 通过、12 跳过，含最后补充的 EFAULT 元数据断言；15 个原生程序与 111 个公共 C/CLI/报告测试全部通过。计数有重叠。首次原生用例发现 FWASWRITTEN 遗漏，已修复并保留失败证据。没有改变时限；GitHub 完整 CI 与 iOS 真机仍需单独验收，Intel Actions 保持暂停。

`build-hvf-arm64/writable-darwin-evidence/` · `writable-native-final/` · `writable-focused-final.xml` · `writable-public.xml`

Python 首次整组测试在三个 ARM64 目录枚举场景超时；不改参数的诊断中，新增可写场景 10/10 通过，但一个 iOS 目录调用墙钟 5.005 秒、CPU 1.263 秒后超时。同一 5 秒限制下单独复验三个 ARM64 场景均通过（2.43–3.17 秒，10,941 条指令，输出 65）。16 逻辑核的宿主负载为 54–70，支持调度压力解释，不代表延迟稳定；原始失败保留。

最终未改参数的 Python 整组测试通过，覆盖五种 profile/ISA，耗时 41.118 秒；每进程仍限 5 秒，前面的失败和诊断记录独立保留。


元数据策略验证（2026-10-06）：Release 专项148项，124通过、24不可用后端跳过、零失败。完整Darwin645项中343通过、300跳过、两项既有ARM64 HVF目录枚举超时；原参数、原5秒时限的20项复测为8通过/12跳过，受影响项耗时3.818/3.949秒。合计75项必需HVF均有通过观察，但首次整轮失败仍保留。公共C/CLI/报告117/117（含73项Darwin比较）、Python五种组合27.359秒、原生程序15/15和runner66/66通过。分配策略不代表APFS；没有改时限。完整GitHub CI、Intel原生、iOS真机与完整环境仍需单独完成。

`build-hvf-arm64/mutation-metadata-validation-summary.json`; `mutation-metadata-darwin-evidence/`; `mutation-metadata-directory-recheck/`; `mutation-metadata-focused.xml`; `mutation-metadata-public.xml`.

稀疏定位验证（2026-10-06）：Release Darwin 共 671 项，359 通过、312 项后端不可用而跳过、零失败；78 项必需 ARM64 HVF 全部执行，Unicorn 覆盖五种来宾。专项 147 项为 123 通过、24 跳过；16 个原生程序、122 项公共 C/CLI/报告（含 78 项 Darwin 比较）、Python 五组（12.344 秒）和 66 项 runner 全部通过。计数重叠，未改时限，历史失败保留。证据：`build-hvf-arm64/sparse-seek-validation-summary.json`。分配几何属于显式虚拟策略，不能视为 APFS 等价；完整 CI 和 iOS 真机仍需验收，Intel HVF Actions 保持暂停。

删除验证（2026-10-06）：Release Darwin 708 项为 384 通过、324 项后端不可用跳过、零失败，81 项必需 ARM64 HVF 全部执行。专项 156 项为 137 通过/19 跳过；原生 17/17、公共 C/CLI/报告 128/128（83 项 Darwin 比较）、Python 五组 16.268 秒、runner 66/66 均通过。独立设计与实现审查无剩余阻塞；计数重叠、时限未改，无需重试。证据：`build-hvf-arm64/unlink-validation-summary.json`。目录失效与固定策略时间属于模型规则，完整文件系统/运行时和 iOS 真机仍未验收；Intel HVF Actions 继续暂停，完整 GitHub CI 单独验收。

### 创建验证，2026-10-06

Release Darwin 共 748 项：412 通过、336 项后端不可用跳过、零失败，84 项必需 ARM64 HVF 全部执行。专项 162 项为 150 通过/12 跳过；公共 C/CLI/报告 133/133（88 项 Darwin），Python 五组 9.982 秒、原生 18/18、runner 66/66 均通过。初轮 ARM64 正确拒绝测试指针表引入的重定位；改为内联字节后通过，未放宽加载规则，原失败和二进制保留。runner 预期清单同步从 27 改为 28 个工作负载。独立审查无剩余阻塞，并补测容量失败不改变父目录观测。计数重叠，时限未改。完整 GitHub CI、iOS 真机另行验收；Intel HVF Actions 仍暂停。

`build-hvf-arm64/create-validation-summary.json`, `create-darwin-evidence/`, `create-focused.xml`, `create-public.xml`, `create-native-final/`, `create-initial-evidence/`.

### 创建元数据验证，2026-10-06

Release Darwin 共 787 项：439 通过、348 项后端不可用跳过、零失败，87 项必需 ARM64 HVF 全部执行。专项 151 项中 139 通过、12 跳过。公共 C/CLI/报告 145/145 通过，含 98 项 Darwin 输入比较；未改动的 Python 方法在 12.211 秒内通过五组配置。原生程序 19/19、验收脚本 66/66 通过。独立审查无剩余阻塞，新增跨父目录设备/组与全局 inode、首次写入前删除、FD 满且输入不可用时修改 umask 的测试。计数重叠、时限未改，无需失败重试。固定创建/修改时间及分配仍是显式虚拟策略；完整 GitHub CI 与 iOS 真机单独验收，Intel HVF Actions 继续暂停。

`build-hvf-arm64/creation-metadata-validation-summary.json`, `creation-metadata-darwin-evidence/`, `creation-metadata-focused.xml`, `creation-metadata-public.xml`, `creation-metadata-native/`.

### 重命名验证，2026-10-06

Release Darwin 共 835 项：474 通过、360 项后端不可用跳过，既有 macOS ARM64 HVF 虚拟元数据用例超时一次（5.087 秒）。原参数、原 5 秒时限复查该方法：8 通过、12 跳过，受影响项耗时 0.113 秒。两次运行合计覆盖全部 90 个必需 ARM64 HVF 身份；完整门禁仍记录为失败。重命名专项 42/54 通过、12 跳过。公共 C/CLI/报告 150/150，含 103 项 Darwin 输入比较；未改动的 Python 方法在 18.478 秒内通过五组配置。原生 20/20、验收脚本 66/66 通过。独立审查发现嵌套点路径分类错误，4K/16K 回归先复现失败，修复后通过；更早的只读 ftruncate 测试预期已纠正为 EINVAL。保留全部失败和探针版本，计数重叠、时限未改。完整 GitHub CI、iOS 真机另行验收，Intel HVF Actions 继续暂停。

`build-hvf-arm64/rename-validation-summary.json`, `rename-focused-final.xml`, `rename-darwin-final-evidence/`, `rename-metadata-recheck.xml`, `rename-public.xml`, `rename-native-final/`, `rename-review-initial-evidence/`.

## 显式系统观察值

`ProcessOptions::DarwinSystem` / `darwin_system` 为所有 Darwin 配置的 `sysctl(202)` 和原始 `sysctlbyname(274)` 提供固定观察值。各字段均可省略；未提供的值或未列出的键明确停止为不支持。不会查询宿主或推测版本、机型。严格 JSON 与 C++ 校验在加载映像前拒绝非法值和非 Darwin 配置。

`os_revision` 为有符号32位；`cpu_count` 范围为1至 INT32_MAX；`memory_size` 保留全部无符号64位。其余字段是最多1023字节、无内嵌 NUL 的字符串，允许显式空串。返回内容包含结尾 NUL。报告的 CPU 数和内存容量不会改变调度或分配预算。

| JSON 字段 | sysctl 名称 | MIB |
| --- | --- | --- |
| `os_type` | `kern.ostype` | `1,1` |
| `os_release` | `kern.osrelease` | `1,2` |
| `os_revision` | `kern.osrevision` | `1,3` |
| `kernel_version` | `kern.version` | `1,4` |
| `os_version` | `kern.osversion` | `1,65` |
| `machine` | `hw.machine` | `6,1` |
| `model` | `hw.model` | `6,2` |
| `cpu_count` | `hw.ncpu` | `6,3` |
| `memory_size` | `hw.memsize` | `6,24` |

`hw.pagesize` 来自现有客户机内存策略，通常返回8字节；非空输出且容量恰为4时返回4字节。旧 MIB `[6,7]` 与名称 `hw.pagesize_compat` 始终返回4字节。`hw.pagesize` 的动态数字 OID 仍不支持。`hw.memsize` 在容量恰为4时，仅当64位模式是有符号32位值的符号扩展才缩窄，否则返回 ERANGE34，保持输出和长度不变。

MIB 数量取低32位，须为2–12；名称长度取完整64位且须小于1024。先检查全部指定字节，再按首个 NUL 解释名称并移除一个末尾点；空名称返回 ENOENT，部分可读输入仍不支持。非空 `oldlenp` 必须在任何副作用前完整具备8字节读写权限。原生非法长度指针探测未在时限内返回，因此这类指针明确留在不支持边界。空 `oldlenp` 表示容量0；空 `oldp` 仅查询长度。短缓冲区返回 ENOMEM12、不写数据并把长度置0；数据 EFAULT 保持旧长度。输入和容量先取快照，随后写数据，最后写长度，保留别名顺序及后续传输失败前已完成的复制。

只有 `newp` 和 `newlen` 均非零才构成写请求。选中的节点按模型固定非 root 身份，在检查观察值或输出前返回 EPERM1；包括原生允许特权写入的 `kern.osversion`。新长度为0时忽略指针。未知键、其他树和动态 OID 不会被猜测为 ENOENT。

原创 `system-info` 程序检查原生 macOS 与客户机 ABI；`virtual-system` 通过 C++、C/CLI、Python 比较配置的精确字节。独立 SDK 对照将宿主九项观察值显式作为测试输入，比较名称与数字查询输出。这不构成 iOS 真机或 Intel HVF 验收。

```json
{"darwin_system":{"os_type":"Darwin","os_release":"24.test","os_revision":0,"kernel_version":"Virtual kernel","os_version":"V42","machine":"virtual64","model":"VirtualModel","cpu_count":4,"memory_size":"17179869184"}}
```

[XNU sysctl](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_newsysctl.c), [XNU hardware MIB](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mib.c), [Apple sysctl(3)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/sysctl.3.html).

### 系统查询验证，2026-10-06

Release Darwin 共881项：509通过、372因后端不可用跳过、零失败；93项 ARM64 HVF 必需项全部执行。聚焦检查49项中37通过、12跳过。公开 C/CLI/配置报告163/163通过，含113项 Darwin 输入对照。未改变的 Python 方法在15.302秒内通过五种配置；原生程序21/21、验证脚本66/66通过。独立审查无阻塞，补充的错误优先级组合与独立 SDK 捕获对照均通过。新增 SDK 对照曾因缺少 StringExtras 头文件而编译失败，补上后通过，原始日志和源码已保留。原生非法长度指针探测也已保留，明确在支持边界之外。通过测试后仅整理了两个文件头注释，并成功重建。计数重叠、时限不变，无需运行失败复测。完整 GitHub CI 与 iOS 真机仍待分别验证；Intel HVF Actions 保持暂停。

`build-hvf-arm64/sysctl-validation-summary.json`, `sysctl-darwin-final-evidence/`, `sysctl-focused-final.xml`, `sysctl-public.xml`, `sysctl-native-final/`, `sysctl-sdk-build-failure/`, `sysctl-initial-probe-evidence/`.
