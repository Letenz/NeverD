# 已发布 Android GKI 内核契约

NeverD 优先支持已发布的 Android GKI 5.10–6.18 分支，再扩展其他 Linux 内核变体。进程请求须显式选择分支：

```json
{"linux_kernel":{"gki":"android17-6.18"},"linux_files":{"files":[],"descriptor_limit":16}}
```

Android 原生配置的 API 28 契约描述 Bionic 导入，不决定内核版本。GKI 选择目前只控制已实现的 `pidfd_open` 和向量输出语义；它不认证完整内核、不加载内核映像，也不推断设备、命名空间、凭据或进程清单。未支持的服务仍明确停止。参见官方 [GKI 发布策略](https://source.android.com/docs/core/architecture/kernel/gki-releases)。

## 固定源码版本

`LinuxGKIKernels.def` 采用下列正式 `r1` 标签，核对日期为 2026-10-07。固定提交提供 `kernel/pid.c`、`include/uapi/linux/pidfd.h`、`arch/arm64/configs/gki_defconfig`、`kernel/fork.c`、`lib/iov_iter.c` 与 `fs/read_write.c`；标志值取自 UAPI 和系统调用校验，不来自 Android API 级别或宿主内核。

| 请求分支 | 正式发布标签 | 固定源码提交 | 允许的标志 | Iovec 导入 |
| --- | --- | --- | --- | --- |
| `android12-5.10` | `android12-5.10-2026-07_r1` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/pid.c) | `PIDFD_NONBLOCK` (`0x800`) | [先复制全部元数据](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/lib/iov_iter.c) |
| `android13-5.10` | `android13-5.10-2026-07_r1` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/pid.c) | `PIDFD_NONBLOCK` | [先复制全部元数据](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/lib/iov_iter.c) |
| `android13-5.15` | `android13-5.15-2026-09_r1` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/pid.c) | `PIDFD_NONBLOCK` | [先复制全部元数据](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/lib/iov_iter.c) |
| `android14-5.15` | `android14-5.15-2026-07_r1` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/pid.c) | `PIDFD_NONBLOCK` | [先复制全部元数据](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/lib/iov_iter.c) |
| `android14-6.1` | `android14-6.1-2026-09_r1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/pid.c) | `PIDFD_NONBLOCK` | [先复制全部元数据](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/lib/iov_iter.c) |
| `android15-6.6` | `android15-6.6-2026-07_r1` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/pid.c) | `PIDFD_NONBLOCK` | [单缓冲区路径](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/lib/iov_iter.c) |
| `android16-6.12` | `android16-6.12-2026-09_r1` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/pid.c) | `PIDFD_NONBLOCK` 和 `PIDFD_THREAD` (`0x80`) | [单缓冲区路径](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/lib/iov_iter.c) |
| `android17-6.18` | `android17-6.18-2026-09_r1` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/pid.c) | `PIDFD_NONBLOCK` 和 `PIDFD_THREAD` | [单缓冲区路径](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/lib/iov_iter.c) |

这些固定版本定义当前契约；后续发布或回移必须先核对源码并补充回归。选择 GKI 却同时声明 `pidfd_open` 缺失属于矛盾配置，在加载映像前拒绝。

## 已实现的进程描述符子集

x64／AArch64 原始陷阱与 Bionic `syscall` 共用 `LinuxServices` 和工作负载所有的描述符表。PID／标志取低 32 位；未知标志或非正有符号 PID 在分配描述符前返回 `EINVAL`。当前进程是已知存活的线程组首领；其他目标须由可选 `tasks` 数组显式提供，该数组是来宾 PID 命名空间内固定且封闭的存活任务清单：

```json
{"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true},{"id":3000,"group_leader":false}]},"linux_files":{"files":[],"descriptor_limit":16}}
```

当前进程 PID 1000 隐式存在，空数组也包含它。未提供清单时，外部目标查找仍不支持；显式清单之外的有效正 PID 在分配描述符前返回 `ESRCH`。未带 `PIDFD_THREAD` 的存活非首领，在固定的 5.10–6.12 版本返回 `EINVAL`，在 [6.18 的 `pidfd_prepare`](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/fork.c) 返回 `ENOENT`。6.12／6.18 接受线程标志时可打开已声明的非首领；标志校验始终先于目标查找。

每项必须含 1..2147483647 的整数 `id` 和布尔 `group_leader`，最多 4096 项。重复 ID、额外字段、把 PID 1000 声明为非首领、未选择 GKI 的任务清单均拒绝。优先级观察值只能引用清单任务或当前进程。固定清单不能与 Android 协作式线程模式（`thread_limit > 1`）组合；创建、回收、凭据及命名空间转换等生命周期行为需要各自的所有权实现。

请求须声明 `linux_files`，使普通文件和进程描述符共用限额及所有权。分配最低空闲编号；耗尽返回 `EMFILE`，`close` 释放编号，再次关闭返回 `EBADF`。关闭标准流后其编号可复用。不会调用宿主 pidfd、文件系统或进程查询。

有效 pidfd 的标量 `read`／`write` 在访问载荷前返回 `EINVAL`；`lseek` 校验起点后返回 `ESPIPE`。`writev` 先导入并校验向量，非法元数据或用户范围可能先返回 `EFAULT`，再到缺失写操作的 `EINVAL`；它不读取载荷映射，也不采集 pidfd 输出。pidfd 与 stdout／stderr 共用版本化导入器，遵循固定源码及 [VFS 读写顺序](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/fs/read_write.c)。

固定 5.10／5.15／6.1 先复制整个 iovec 数组，再检查负长度；较早的负长度遇到后续不可访问元数据返回 `EFAULT`，且单向量也在截断长度前校验原始范围。6.6／6.12／6.18 逐项读取和校验，此组合返回 `EINVAL`；单缓冲区先限长再验用户范围，多向量仍检查全部原始范围。这些规则来自固定源码的 `copy_iovec_from_user`、`__import_iovec`、`import_ubuf`。未选择 GKI 时保留现有单缓冲区策略，不推断内核版本。

Bionic 将原始负错误转换为 `-1` 和线程局部 `errno`；成功保留 `errno`。本子集缺少 `fstat` 所需元数据。轮询、退出通知、pidfd 信号投递、`pidfd_getfd`、`fcntl`、pidfs ioctl 和未观察任务查找仍不支持；不会从 pidfd 推断调度或进程生命周期。

## 验证与后续覆盖

`LinuxPIDFDTests.cpp` 使用独立的 x64／AArch64 O0／O2 ELF 样例，覆盖八个分支及可用后端，检查标志、共享文件表、限额／复用、标量／向量错误顺序、元数据故障、限长与原始范围、清单省略／封闭、非首领和描述符耗尽前的目标查找。`AndroidSyscallTests.cpp` 在普通、Android packed 和 RELR 的 O0／O2 六种配置中复验 raw／Bionic 所有权、查找和 errno。源码及模型执行只证明此系统调用子集，尚无逐个启动全部固定 GKI 映像的原生测试；扩展其他 Linux 发行版前须逐服务保留版本、配置和观察值证据。
