# 已釋出 Android GKI 核心契約

NeverD 優先支援已釋出的 Android GKI 5.10–6.18 分支，再擴充套件其他 Linux 核心變體。程序請求須顯式選擇分支：

```json
{"linux_kernel":{"gki":"android17-6.18"},"linux_files":{"files":[],"descriptor_limit":16}}
```

Android 原生配置的 API 28 契約描述 Bionic 匯入，不決定核心版本。GKI 選擇目前只控制已實現的 `pidfd_open` 和向量輸出語義；它不認證完整核心、不載入核心映像，也不推斷裝置、名稱空間、憑據或程序清單。未支援的服務仍明確停止。參見官方 [GKI 釋出策略](https://source.android.com/docs/core/architecture/kernel/gki-releases)。

## 固定原始碼版本

`LinuxGKIKernels.def` 採用下列正式 `r1` 標籤，核對日期為 2026-10-07。固定提交提供 `kernel/pid.c`、`include/uapi/linux/pidfd.h`、`arch/arm64/configs/gki_defconfig`、`kernel/fork.c`、`lib/iov_iter.c` 與 `fs/read_write.c`；標誌值取自 UAPI 和系統呼叫校驗，不來自 Android API 級別或宿主核心。

| 請求分支 | 正式釋出標籤 | 固定原始碼提交 | 允許的標誌 | Iovec 匯入 |
| --- | --- | --- | --- | --- |
| `android12-5.10` | `android12-5.10-2026-07_r1` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/pid.c) | `PIDFD_NONBLOCK` (`0x800`) | [先複製全部元資料](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/lib/iov_iter.c) |
| `android13-5.10` | `android13-5.10-2026-07_r1` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/pid.c) | `PIDFD_NONBLOCK` | [先複製全部元資料](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/lib/iov_iter.c) |
| `android13-5.15` | `android13-5.15-2026-09_r1` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/pid.c) | `PIDFD_NONBLOCK` | [先複製全部元資料](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/lib/iov_iter.c) |
| `android14-5.15` | `android14-5.15-2026-07_r1` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/pid.c) | `PIDFD_NONBLOCK` | [先複製全部元資料](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/lib/iov_iter.c) |
| `android14-6.1` | `android14-6.1-2026-09_r1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/pid.c) | `PIDFD_NONBLOCK` | [先複製全部元資料](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/lib/iov_iter.c) |
| `android15-6.6` | `android15-6.6-2026-07_r1` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/pid.c) | `PIDFD_NONBLOCK` | [單緩衝區路徑](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/lib/iov_iter.c) |
| `android16-6.12` | `android16-6.12-2026-09_r1` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/pid.c) | `PIDFD_NONBLOCK` 和 `PIDFD_THREAD` (`0x80`) | [單緩衝區路徑](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/lib/iov_iter.c) |
| `android17-6.18` | `android17-6.18-2026-09_r1` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/pid.c) | `PIDFD_NONBLOCK` 和 `PIDFD_THREAD` | [單緩衝區路徑](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/lib/iov_iter.c) |

這些固定版本定義當前契約；後續釋出或回移必須先核對原始碼並補充迴歸。選擇 GKI 卻同時宣告 `pidfd_open` 缺失屬於矛盾配置，在載入映像前拒絕。

## 已實現的程序描述符子集

x64／AArch64 原始陷阱與 Bionic `syscall` 共用 `LinuxServices` 和工作負載所有的描述符表。PID／標誌取低 32 位；未知標誌或非正有符號 PID 在分配描述符前返回 `EINVAL`。當前程序是已知存活的執行緒組首領；其他目標須由可選 `tasks` 陣列顯式提供，該陣列是來賓 PID 名稱空間內固定且封閉的存活任務清單：

```json
{"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true},{"id":3000,"group_leader":false}]},"linux_files":{"files":[],"descriptor_limit":16}}
```

當前程序 PID 1000 隱式存在，空陣列也包含它。未提供清單時，外部目標查詢仍不支援；顯式清單之外的有效正 PID 在分配描述符前返回 `ESRCH`。未帶 `PIDFD_THREAD` 的存活非首領，在固定的 5.10–6.12 版本返回 `EINVAL`，在 [6.18 的 `pidfd_prepare`](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/fork.c) 返回 `ENOENT`。6.12／6.18 接受執行緒標誌時可開啟已宣告的非首領；標誌校驗始終先於目標查詢。

每項必須含 1..2147483647 的整數 `id` 和布林 `group_leader`，最多 4096 項。重複 ID、額外欄位、把 PID 1000 宣告為非首領、未選擇 GKI 的任務清單均拒絕。優先順序觀察值只能引用清單任務或當前程序。固定清單不能與 Android 協作式執行緒模式（`thread_limit > 1`）組合；建立、回收、憑據及名稱空間轉換等生命週期行為需要各自的所有權實現。

請求須宣告 `linux_files`，使普通檔案和程序描述符共用限額及所有權。分配最低空閒編號；耗盡返回 `EMFILE`，`close` 釋放編號，再次關閉返回 `EBADF`。關閉標準流後其編號可複用。不會呼叫宿主 pidfd、檔案系統或程序查詢。

有效 pidfd 的標量 `read`／`write` 在訪問載荷前返回 `EINVAL`；`lseek` 校驗起點後返回 `ESPIPE`。`writev` 先匯入並校驗向量，非法元資料或使用者範圍可能先返回 `EFAULT`，再到缺失寫操作的 `EINVAL`；它不讀取載荷對映，也不採集 pidfd 輸出。pidfd 與 stdout／stderr 共用版本化匯入器，遵循固定原始碼及 [VFS 讀寫順序](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/fs/read_write.c)。

固定 5.10／5.15／6.1 先複製整個 iovec 陣列，再檢查負長度；較早的負長度遇到後續不可訪問元資料返回 `EFAULT`，且單向量也在截斷長度前校驗原始範圍。6.6／6.12／6.18 逐項讀取和校驗，此組合返回 `EINVAL`；單緩衝區先限長再驗使用者範圍，多向量仍檢查全部原始範圍。這些規則來自固定原始碼的 `copy_iovec_from_user`、`__import_iovec`、`import_ubuf`。未選擇 GKI 時保留現有單緩衝區策略，不推斷核心版本。

Bionic 將原始負錯誤轉換為 `-1` 和執行緒區域性 `errno`；成功保留 `errno`。本子集缺少 `fstat` 所需元資料。輪詢、退出通知、pidfd 訊號投遞、`pidfd_getfd`、`fcntl`、pidfs ioctl 和未觀察任務查詢仍不支援；不會從 pidfd 推斷排程或程序生命週期。

## 驗證與後續覆蓋

`LinuxPIDFDTests.cpp` 使用獨立的 x64／AArch64 O0／O2 ELF 樣例，覆蓋八個分支及可用後端，檢查標誌、共享檔案表、限額／複用、標量／向量錯誤順序、元資料故障、限長與原始範圍、清單省略／封閉、非首領和描述符耗盡前的目標查詢。`AndroidSyscallTests.cpp` 在普通、Android packed 和 RELR 的 O0／O2 六種配置中複驗 raw／Bionic 所有權、查詢和 errno。原始碼及模型執行只證明此係統呼叫子集，尚無逐個啟動全部固定 GKI 映像的原生測試；擴充套件其他 Linux 發行版前須逐服務保留版本、配置和觀察值證據。
