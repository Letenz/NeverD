**語言**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 2ad5b9e4d32165a7e7d460cc2290a5cd1bd7718c03cc6573e1fd3b5967d1d4d6 -->

[← 文件索引](README.md)

# macOS 與 iOS 客體行程環境

`lib/emulation/os/darwin/` 提供有界、獨立的 Mach-O 行程模型，與宿主 CPU 傳輸層分離。啟用 `NEVERD_ENABLE_CPU_EMULATION` 即可，不必開啟 Windows 驅動模擬。`macos/` 與 `ios/` 分別定義明確的平台契約。

| Profile | Mach-O 平台 | 客體 ISA | OS 頁大小 |
| --- | --- | --- | --- |
| `macos-macho64-v1` | macOS | x86-64、基礎 ARM64 | x64 4 KiB；ARM64 16 KiB |
| `ios-macho64-v1` | iOS 實體裝置 | 基礎 ARM64 | 16 KiB |
| `ios-simulator-macho64-v1` | iOS Simulator | x86-64、基礎 ARM64 | x64 4 KiB；ARM64 16 KiB |

裝置映像與模擬器映像不可互換，宿主不決定客體平台。macOS 上 ISA 相符時可用 [HVF](macos-hvf.md)，跨 ISA 的 `auto` 使用 Unicorn。CPU 映射粒度仍為 4 KiB。[C、Python 與 CLI API](process-emulation.md) 共用選項、限制及報告。

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

## 映像與啟動

`MachOExecutionImage` 保留原始位元組，不套用分析階段的重定位修補。只接受 little-endian、thin `MH_EXECUTE`，且平台與進入點必須明確。通用映像需要先明確擷取所需架構切片。

完整檔案包含中繼資料及尾端位元組，在解析與複製前都必須符合 `memory_limit`。載入器從一般檔案讀取有界的私有快照，拒絕 NUL 路徑、短讀及大小變化，不保留即時檔案映射。檔案與客體記憶體各有獨立、同值的上限；宿主檔案 I/O 沒有硬即時期限。

區段保留目前／最大權限與零填入。`__PAGEZERO` 只保留位址，不配置整段實體記憶體。執行前檢查檔案／VM 範圍、OS 頁對齊、取整後重疊、標頭歸屬、可執行進入點及預算。標頭區段必須可讀且可執行；保護頁和私有返回入口持續保留。檔案最後一頁保留至頁界或 EOF 的原始位元組，其後完整 VM 頁清零，依據 [XNU 載入器](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c)。

`LC_MAIN` 接收 `argc`、`argv`、`envp` 和 apple 向量四個整數參數；返回值低八位元成為退出狀態。只在無匯入的這種進入交接中接受 `/usr/lib/dyld`，不執行宿主 dyld。非零 `stacksize` 會拒絕，因為呼叫端明確的 `stack_size` 掌握預算。

`LC_UNIXTHREAD` 要求恰好一份完整的原生 64 位元一般暫存器紀錄，只有 PC 已設定。起始堆疊含 argc、以零結尾的 argv/envp，以及含 `executable_path=<input filename>` 的零結尾 apple 向量。自訂 SP、旗標、其他暫存器、額外 flavor 或衝突進入點均拒絕。不繼承宿主環境或 Linux 輔助向量。參考 [dyld 架構](https://github.com/apple-oss-distributions/dyld/blob/main/doc/dyld4.md)。

外部 dylib、匯入、rebases/chained fixups、建構／解構函式、TLS 區段、arm64e/PAC、其他不支援的 CPU 子型別、加密內容與未建模載入命令，都在執行前失敗。無 fixup 的 PIE 使用偏好位址，並非 ASLR。簽章資料只是中繼資料，不實作 AMFI 或 entitlement 政策。

## Darwin 服務

ARM64 使用 X16、X0–X5 與 `svc #0x80`；x64 使用 BSD 類別 `0x02000000`、RAX 及 RDI/RSI/RDX/R10/R8/R9。成功清除 carry，失敗設定 carry 並返回正 errno。ARM64 清除 X1；x64 成功清除 RDX、失敗保留 RDX。SYSCALL 的暫存器改寫明確定義。報告以 `result` 與 `error=true` 表達 BSD 錯誤；不返回或不支援的請求沒有這兩個欄位。規則依據 XNU [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c) 與 [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c)，未納入 Apple 實作程式碼。

服務包含 `exit`、`write`、`getpid`、`getppid`、`getuid`、`geteuid`、`getgid`、`getegid`、`mmap`、`mprotect`、`munmap`。PID/UID/GID 固定為 1000，PPID 為 1。描述元 1、2 擷取原始位元組，包含 NUL 與非 UTF8；關閉或唯讀描述元返回 EBADF。部分複製已取得的資料會保留，但後續錯誤仍為 EFAULT。長度超過 `INT_MAX` 時，先返回 EINVAL，再談描述元、指標或預算檢查，依據 [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c)。

記憶體服務支援私有匿名資料映射：`flags=0x1002`、描述元 -1、offset 零。長度及非固定提示位址向上取整至 OS 頁；提示已占用時先向上搜尋，再回到預設配置區。舊式原始 mmap 零長度返回零且不配置；`MAP_UNIX03` 已支援，零長度返回 EINVAL。Unmap/protect 位址必須對齊。支援 NONE/READ/WRITE，WRITE 隱含 READ。每個 OS 頁獨立持有實體記憶體，部分解除映射可釋放預算，新頁面清零。Protect 跨空洞或超過最大權限時，整個範圍維持原狀。來源：[XNU VM 服務](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c)。

共享／固定／JIT 映射、匿名可執行映射、Mach trap、間接系統呼叫、執行緒、訊號、宿主檔案／網路、dyld、Objective-C/Swift runtime 和 Foundation/UIKit 均不支援，會明確停止。這不是完整 Apple OS，也不是 iOS Simulator 應用程式。

## 驗證

自行撰寫的 C 測試映像以 Clang 與 `ld64.lld` 產生，不依賴 Apple SDK 或專有二進位檔。涵蓋五種平台／ISA、畸形 Mach-O、4/16 KiB 頁與預算已滿時的部分釋放。`NeverDProcessPublicTests` 比對 C API/CLI；設定 `NEVERD_TEST_LIBNEVERD` 與 `NEVERD_TEST_DARWIN_FIXTURES` 可讓 Python SDK 測試相同五種組合。

## 明確的檔案輸入與描述元

`darwin_files` 為三個 Darwin profile 提供封閉唯讀檔案目錄。必要的 `files` 項目包含規範絕對客體 `path` 和十六進位 `bytes_hex`；選用 `stdin_hex` 提供有限輸入流。省略表示未知，非零讀取會停止；空字串表示 EOF。未設定目錄時 open 停止，明確空目錄中缺失的絕對路徑返回 ENOENT，不會存取宿主檔案或輸入。

新增 `open`、`read`、`pread`、`lseek`、`close`、`dup`、`dup2`、`fcntl`；read/write/open/close/fcntl/pread 的 nocancel 入口共用實作。支援 O_RDONLY/O_CLOEXEC 與 F_DUPFD、F_DUPFD_CLOEXEC、F_GETFD、F_SETFD、F_GETFL。獨立開啟有獨立游標，複製描述元共用游標但各自保留 close-on-exec；pread 不移動游標。關閉或替換 0/1/2 會影響後續 I/O，複製輸出仍使用原擷取通道與共享預算。

上限為 256 個檔案、路徑/NUL/檔案/輸入合計 16 MiB、路徑少於 1024 位元組、每個分量最多 255 位元組。`descriptor_limit` 為排他上界 3–4096，預設 256；JSON 仍限 64 KiB。非法設定在載入前拒絕。read 超過 INT_MAX 先返回 EINVAL；EOF 不存取目的位址，無效位址返回 EFAULT。部分可寫緩衝區在任何複製或游標改動前停止。SET/CUR/END 定位失敗保留游標。寫入檔案、舊版 stat、稀疏定位及其他 fcntl 仍未支援。普通檔案作為祖先返回 ENOTDIR。同一原始目標檔案對照原生 macOS；C/CLI/Python 涵蓋五種客體組合，不代表 iOS 真機驗證。

2026-10-05 Release Darwin 驗收共 381 項：177 通過、204 跳過、零失敗，ARM64 HVF 必需項 51/51 執行。原生 macOS 7 個程式、公共 C/CLI 與報告 35 項、Python 五種客體組合和驗收腳本 66 項通過，計數有重疊。新增檔案服務尚無 Intel HVF/KVM/WHP 原生證據；Intel HVF 仍未驗證且暫停 Actions。宿主缺少 iOS SDK，也沒有 iOS 真機對照。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## 目錄與相對路徑

選用 `directories` 含規範絕對 `path` 和可選完整 `metadata`，可宣告空目錄；根與祖先隱式存在，中繼資料不建立缺失路徑。目錄 mode 為 `0x4000` 加權限，size 為 [0, INT64_MAX] 的明確觀察值。`working_directory` 必須是既有目錄，省略表示 CWD 未知，不繼承宿主。最多 256 個指定檔案/目錄路徑（含僅有中繼資料的祖先）；路徑/NUL/檔案/輸入/CWD 共 16 MiB。

`openat` (463)、`openat_nocancel` (464)、`chdir` (12)、`fchdir` (13)、`fstatat64` (470) 共用解析器。相對路徑使用目錄 FD 或 `AT_FDCWD=-2`，絕對路徑忽略 FD。重複斜線、`.`、`..` 及尾端斜線保留祖先檢查：`/file/..` 為 ENOTDIR，`/missing/..` 為 ENOENT。失敗及原 FD 的關閉、重用或替換不改變 CWD。`F_GETPATH=50` 回傳規範路徑和 NUL，包含複製描述元，不寫入終止符後方。

目錄 read/pread 即使零長度亦為 EISDIR，負 pread 偏移優先 EINVAL。SET/CUR 共用游標，END 需要明確 size；目錄 mmap 為 EINVAL。fstatat64 支援 0、`AT_SYMLINK_NOFOLLOW=0x20`、`AT_SYMLINK_NOFOLLOW_ANY=0x800`、`AT_FDONLY=0x400`（忽略路徑）；非法位為 EINVAL，`AT_REALDEV=0x200` 未支援。串流身份未知，權限不是存取控制模型；列舉與寫入仍待完成。`directories` 程式對照原生與五種客體，stat 比對真實檔案及目錄。Intel HVF Actions 保持暫停。

[XNU VFS](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/fcntl.h).

目錄驗收（2026-10-05，Release）：467 項登記，227 通過、240 跳過、零失敗；ARM64 HVF 必需項 60/60 執行。10 個原生 macOS 程式、37 項公共 C/CLI/報告（無跳過）、Python 五種客體和 66 項驗收腳本通過，計數重疊。證據：`build-hvf-arm64/darwin-directory-verified-evidence/`。其他原生後端與 iOS 真機尚未驗收。

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```


## 私有檔案映射

`mmap` 支援一般目錄檔案的 `MAP_PRIVATE` 映射：`flags=0x2`，或加上 `MAP_UNIX03` 的 `0x40002`；偏移必須按 OS 頁對齊。即使要求長度較短，整頁仍保留原始檔案位元組，EOF 尾頁剩餘部分清零。私有寫入不改變原檔、其他映射、固定中繼資料或共用游標。close 或重用描述元後映射仍有效。唯讀與 PROT_NONE 映射也有完整初始內容，可透過 `mprotect` 增加寫入權限。

檔案末尾溢位、UNIX03 零長度或未對齊偏移在 FD 查詢前返回 EINVAL；無效 FD 在預算檢查前返回 EBADF。舊式零長度仍檢查 FD，然後返回零而不配置。舊式未對齊偏移、串流、空檔頁和完整越過 EOF 的頁在配置前明確停止。原生 macOS 允許映射 EOF 外完整頁，但存取會觸發 SIGBUS；模型不偽造零頁或訊號傳遞。共享、固定、可執行與 JIT 映射仍未支援。

`DarwinFiles` 管理描述元和位元組，`DarwinMemory` 管理配置、權限、預算與回滾；資料只來自 `darwin_files`。`file-mapping` 原生/客體程式檢查私有寫入、close 後壽命、游標、錯誤與匿名頁重用；獨立原生測試比對非零偏移、整頁內容與 SIGBUS 邊界。

[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c)

### 私有映射驗證（2026-10-05）

Release Darwin：438 個唯一登記項，210 通過、228 跳過、零失敗；57/57 個 ARM64 HVF 必需項執行，Unicorn 涵蓋五種客體組合。9 個原生 macOS 程式、非零偏移整頁比對與隔離子程序 SIGBUS 驗證通過。36 項公共介面/報告無跳過，Python 五種組合含 `file-mapping`、66 項驗收腳本和 38 項來源回歸通過，計數重疊。證據：`build-hvf-arm64/darwin-mmap-verified-evidence/`。尚無新增 Intel HVF/KVM/WHP 或 iOS 真機證據；Intel HVF Actions 繼續暫停。

## 明確的檔案中繼資料

檔案項目可提供 `metadata`，下例全部欄位均必填。十進位字串保留完整整數精度，JSON 數字限於 ±(2^53−1) 內的精確整數。device 為有號 32 位，mode/link_count 為無號 16 位，inode 為無號 64 位，uid/gid/flags/generation 為無號 32 位。size 必須等於檔案位元組數；blocks 不超過有號 64 位上限，block_size 為非負有號 32 位。四個時間使用有號 64 位秒及 0–999999999 奈秒，mode 必須符合檔案或目錄類型。

`stat64` (338)、`fstat64` (339)、`lstat64` (340) 在 ARM64/x64 回傳相同的 144 位元組 LP64 記錄。路徑解析與 open 共用；FD 查詢遵循複製及關閉，不配置描述元也不改變游標。rdev、填補和保留欄位清零。中繼資料是呼叫方的固定觀察值，讀取不推進時間，mode 不改變存取授權。缺少中繼資料、串流狀態、符號連結、舊版 stat及擴充安全查詢仍不支援。路徑/FD 錯誤先於目的位址檢查；部分可寫輸出在寫入前停止。原生測試逐位元組對比真實檔案狀態並核對 SDK 配置，同一自編程式驗證三種呼叫。

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

### 中繼資料驗證與後續工作（2026-10-05）

stat64 的 Release 驗收共 409 個唯一項目：193 通過、216 跳過、零失敗；54/54 個 ARM64 HVF 必需項目執行，Unicorn 涵蓋五種客體組合。SDK 配置及真實檔案完整記錄比對、8 個原生程式、36 個公共介面/報告測試（無跳過）、Python 五種組合和 66 個驗收腳本測試均通過，計數有重疊。原生測試每例使用獨立輸出檔案，修復短輸出殘留舊尾端位元組的問題。新增功能仍無 Intel HVF/KVM/WHP 或 iOS 真機證據。

後續依序補共享映射與 EOF 缺頁、目錄列舉及有界寫入（驗證 EOF 頁、close 後映射壽命與錯誤順序），顯式時間/系統資訊、必要 Mach/執行緒服務、Mach-O 相依性與重定位/繫結、初始化/TLS，再以原生程式推進 Objective-C/Swift 與 Foundation/UIKit。iOS 真機比對需要 SDK 與設備；Intel HVF 尚未驗證，Actions 繼續暫停。



```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

獨立工作負載驗收要求 ARM64 60 項或 x64 40 項全部執行，包含每個平台的 `LC_MAIN` 與 `LC_UNIXTHREAD`。必需項缺失、跳過或缺少 `ld64.lld` 都會失敗。

```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

Linux 使用 `kvm`、Windows 使用 `whp`。[Darwin 工作流程](../../.github/workflows/darwin-native.yml) 在關閉 Unicorn 後驗證兩種 x64 後端，也能單獨重跑。[核心參考工作流程](../../.github/workflows/darwin-kernel-reference.yml) 不建置 NeverD/LLVM，直接在兩種 macOS ISA 執行原始程式。`DarwinNativeCases.def` 統一模式、退出狀態及預期位元組；只有宿主參考程式為實際 dyld 入口連結 libSystem。架構不符、Rosetta、逾時或結果差異均失敗，不能當作 iOS 裝置核心證據。

## 證據與待完成範圍

以下為 2026-10-03 記錄，重疊列不能相加：

| 後端 | 原始碼 | 通過 | 失敗 | 跳過 | 原生工作負載 |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 HVF | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel HVF | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| x64 KVM | `36e11ca8a` | 51 | 0 | 235 | 26/26 |
| x64 WHP | `36e11ca8a` | 51 | 0 | 235 | 26/26 |

[Intel 工作](https://github.com/NeverSight/NeverD/actions/runs/37106013999) 核對 286 個 CTest 身分、32 個行程及原始 XML。234 個跳過項為 65 個停用的 Unicorn、39 個 ARM64 客體、130 個其他宿主平台。產物 `11267489438` 的 SHA-256 已驗證為 `cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`。[KVM/WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703) 也獨立核驗。[核心對照](https://github.com/NeverSight/NeverD/actions/runs/37064795867) 在兩種 ISA 各通過 4/4 個程式，退出狀態 37、輸出逐位元組一致、stderr 為空。

開啟 Unicorn 的 C API/CLI 檢查為 138 通過、156 跳過、零失敗。Python 涵蓋五種組合；封裝引擎與 18 份 ARM64 CLI 報告相符，186 個 Mach-O 簽章通過。同時關閉 HVF/Unicorn 時，38 項通過、231 項跳過，且不連結 Hypervisor.framework。這些是整合證據，不能增加原生執行計數。Intel 完整 CPU 仍未驗收；詳見 [HVF](macos-hvf.md)及[完整記錄](../darwin-emulation.md#hosted-native-verification-2026-10-03)。
