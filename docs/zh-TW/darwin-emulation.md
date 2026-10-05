**語言**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: b7b738f245c491ef7933aee4dbadbed014cab63ea50c51663817203ef3ef421e -->

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

BSD 呼叫在 ARM64 使用 X16、X0–X5 與 `svc #0x80`；x64 使用 BSD 類別 `0x02000000`、RAX 及 RDI/RSI/RDX/R10/R8/R9。成功清除 carry，失敗設定 carry 並返回正 errno。ARM64 清除 X1；x64 成功清除 RDX、失敗保留 RDX。SYSCALL 的暫存器改寫明確定義。報告以 `result` 與 `error=true` 表達 BSD 錯誤；不返回或不支援的請求沒有這兩個欄位。規則依據 XNU [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c) 與 [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c)，未納入 Apple 實作程式碼。

服務包含 `exit`、`write`、`getpid`、`getppid`、`getuid`、`geteuid`、`getgid`、`getegid`、`mmap`、`mprotect`、`munmap`。PID/UID/GID 固定為 1000，PPID 為 1。描述元 1、2 擷取原始位元組，包含 NUL 與非 UTF8；關閉或唯讀描述元返回 EBADF。部分複製已取得的資料會保留，但後續錯誤仍為 EFAULT。長度超過 `INT_MAX` 時，先返回 EINVAL，再談描述元、指標或預算檢查，依據 [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c)。

記憶體服務支援私有匿名資料映射：`flags=0x1002`、描述元 -1、offset 零。長度及非固定提示位址向上取整至 OS 頁；提示已占用時先向上搜尋，再回到預設配置區。舊式原始 mmap 零長度返回零且不配置；`MAP_UNIX03` 已支援，零長度返回 EINVAL。Unmap/protect 位址必須對齊。支援 NONE/READ/WRITE，WRITE 隱含 READ。每個 OS 頁獨立持有實體記憶體，部分解除映射可釋放預算，新頁面清零。Protect 跨空洞或超過最大權限時，整個範圍維持原狀。來源：[XNU VM 服務](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c)。

共享／固定／JIT 映射、匿名可執行映射、其他 Mach trap、間接系統呼叫、執行緒、訊號、宿主檔案／網路、dyld、Objective-C/Swift runtime 和 Foundation/UIKit 均不支援，會明確停止。這不是完整 Apple OS，也不是 iOS Simulator 應用程式。

## 驗證

自行撰寫的 C 測試映像以 Clang 與 `ld64.lld` 產生，不依賴 Apple SDK 或專有二進位檔。涵蓋五種平台／ISA、畸形 Mach-O、4/16 KiB 頁與預算已滿時的部分釋放。`NeverDProcessPublicTests` 比對 C API/CLI；設定 `NEVERD_TEST_LIBNEVERD` 與 `NEVERD_TEST_DARWIN_FIXTURES` 可讓 Python SDK 測試相同五種組合。

## 明確的檔案輸入與描述元

`darwin_files` 為三個 Darwin profile 提供封閉的初始唯讀檔案目錄。必要的 `files` 項目包含規範絕對客體 `path` 和十六進位 `bytes_hex`；選用 `stdin_hex` 提供有限輸入流。省略表示未知，非零讀取會停止；空字串表示 EOF。未設定目錄時 open 停止，明確空目錄中缺失的絕對路徑返回 ENOENT，不會存取宿主檔案或輸入。

新增 `open`、`read`、`pread`、`lseek`、`close`、`dup`、`dup2`、`fcntl`；read/write/open/close/fcntl/pread 的 nocancel 入口共用實作。支援 O_RDONLY/O_CLOEXEC 與 F_DUPFD、F_DUPFD_CLOEXEC、F_GETFD、F_SETFD、F_GETFL。獨立開啟有獨立游標，複製描述元共用游標但各自保留 close-on-exec；pread 不移動游標。關閉或替換 0/1/2 會影響後續 I/O，複製輸出仍使用原擷取通道與共享預算。

上限為 256 個檔案、路徑/NUL/檔案/輸入合計 16 MiB、路徑少於 1024 位元組、每個分量最多 255 位元組。`descriptor_limit` 為排他上界 3–4096，預設 256；JSON 仍限 64 KiB。非法設定在載入前拒絕。read 超過 INT_MAX 先返回 EINVAL；EOF 不存取目的位址，無效位址返回 EFAULT。部分可寫緩衝區在任何複製或游標改動前停止。SET/CUR/END 定位失敗保留游標。舊版 stat、稀疏定位及其他 fcntl 仍未支援。普通檔案作為祖先返回 ENOTDIR。同一原始目標檔案對照原生 macOS；C/CLI/Python 涵蓋五種客體組合，不代表 iOS 真機驗證。

2026-10-05 Release Darwin 驗收共 381 項：177 通過、204 跳過、零失敗，ARM64 HVF 必需項 51/51 執行。原生 macOS 7 個程式、公共 C/CLI 與報告 35 項、Python 五種客體組合和驗收腳本 66 項通過，計數有重疊。新增檔案服務尚無 Intel HVF/KVM/WHP 原生證據；Intel HVF 仍未驗證且暫停 Actions。宿主缺少 iOS SDK，也沒有 iOS 真機對照。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## 既有檔案的可寫內容

檔案以嚴格布林值 `"writable":true` 明確允許行程內修改；C++ 使用 `DarwinFileOptions::WritableFiles`。省略或 false 維持唯讀，未知授權停止，不存取宿主或改動輸入。write(4/397)、pwrite(154/415)、truncate(200)、ftruncate(201) 與 O_TRUNC 共用內容節點；各次 open 游標獨立，dup 共用游標與狀態，最後 close 後重開仍保留內容。增長補零，截斷保留游標；O_RDONLY|O_TRUNC 也會截斷。

F_SETFL 只改 O_APPEND 並保留存取模式、close-on-exec 與 FWASWRITTEN；實際傳輸非零位元組後 F_GETFL 顯示 0x10000，包含 pwrite 和輸出擷取。pwrite 忽略 append、不移游標。INT_MAX 長度檢查先於 FD，偏移 -1 的 pwrite 更早返回 EINVAL；INT64_MAX 在零寫之前返回 EFBIG，長度先裁剪再選 EOF。

成功的 ftruncate（含大小不變）也為呼叫描述及其 dup 設定 FWASWRITTEN；O_TRUNC 為新描述設定，包含 O_RDONLY。路徑 truncate 不改變既有描述的旗標。

部分可讀輸入在效果前停止；完整 EFAULT 保留內容，但非空追加會移至 EOF。後端失敗不提交內容或游標。未設定 `mutation_policy` 時，非零寫入、截斷及非零完整 EFAULT 使完整 stat 觀察失效，後續查詢在輸出前停止；零寫不失效。16 MiB 為路徑/NUL、輸入、目錄記錄、CWD、現有內容及可寫路徑引用的合計邏輯預算；縮小替換 backing 並回收容量，原始輸入與一個有界替換緩衝區另計。已知 inode 別名和 immutable/append-only 旗標暫拒絕。

映射租約由 DarwinMemory 持有，全部區間 unmap 前拒絕 write、truncate、O_TRUNC，包含 PROT_NONE 和已 close 的 FD；失敗或舊式零長度映射不留租約。新映射取得目前內容。只寫 FD 的 READ/WRITE mmap 為 EACCES；PROT_NONE 可成功並以 mprotect 取得讀寫權限。

原生一般/nocancel 程式比對位元組、游標、旗標及錯誤順序；單元測試涵蓋 4K/16K，C/CLI/Python 涵蓋五種組合。建立、刪除、改名、硬連結、真實檔案系統的中繼資料更新、映射一致性與 EOF SIGBUS 尚缺；完整環境、iOS 實機與 Intel HVF 仍未驗收，Intel Actions 維持暫停。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233","writable":true}]}}
```

[XNU write](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU vnode](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c).

## 明確可變中繼資料

檔案可在 `writable: true` 與完整 `metadata` 旁設定 `mutation_policy`；C++ 使用 `DarwinFileOptions::MutationPolicies`。這是明確選擇的虛擬稀疏分配契約，不推斷 APFS 或讀取宿主時間；省略時修改後中繼資料仍未知。

allocation_unit、mutation_time 及 seconds/nanoseconds 全部必填，整數沿用無損規則。單位為 512 位元組至 16 MiB 的二次冪，獨立於 block_size 與 VM 頁。須為普通檔案權限（無 set-id/sticky）、flags=0、link_count=1，且初始密集分配滿足 blocks=ceil(size/allocation_unit)*(allocation_unit/512)。零位元組不能推斷為洞。策略路徑引用計入 16 MiB 邏輯預算；分配帳本不推導 ENOSPC。

寫入分配所有觸及單位，向洞寫零也分配。truncate 增長只補零；縮小丟棄向上取整 EOF 之後的單位並保留末尾已分配的部分單位，重新增長不恢復已丟棄分配。非零成功寫和每次成功截斷（含同大小、空 O_TRUNC）更新 size/blocks，並設定固定 mtime/ctime；其餘欄位不變，讀取不推進 atime。路徑 stat、獨立 open、dup、重開共用節點，原始輸入不變。

零寫、預算/映射拒絕、部分輸入和後端失敗保留已知狀態；非零完整 EFAULT 仍使中繼資料永久未知，後續成功修改不能恢復。stat 複製失敗不改節點。virtual-file-metadata 經五種組合與 C/CLI/Python 檢查144位元組記錄；分配是策略測試，非 APFS 原生等價證據。原生程式另驗證旗標、游標與錯誤順序。命名空間、真實檔案系統一致性、Mach 與動態執行階段仍待完成。

```json
{"mutation_policy":{"allocation_unit":4096,"mutation_time":{"seconds":-7,"nanoseconds":123456789}}}
```


## 目錄與相對路徑

選用 `directories` 含規範絕對 `path` 和可選完整 `metadata`，可宣告空目錄；根與祖先隱式存在，中繼資料不建立缺失路徑。目錄 mode 為 `0x4000` 加權限，size 為 [0, INT64_MAX] 的明確觀察值。`working_directory` 必須是既有目錄，省略表示 CWD 未知，不繼承宿主。最多 256 個指定檔案/目錄路徑（含僅有中繼資料的祖先）；路徑/NUL/檔案/輸入/CWD 共 16 MiB。

`openat` (463)、`openat_nocancel` (464)、`chdir` (12)、`fchdir` (13)、`fstatat64` (470) 共用解析器。相對路徑使用目錄 FD 或 `AT_FDCWD=-2`，絕對路徑忽略 FD。重複斜線、`.`、`..` 及尾端斜線保留祖先檢查：`/file/..` 為 ENOTDIR，`/missing/..` 為 ENOENT。失敗及原 FD 的關閉、重用或替換不改變 CWD。`F_GETPATH=50` 回傳規範路徑和 NUL，包含複製描述元，不寫入終止符後方。

目錄 read/pread 即使零長度亦為 EISDIR，負 pread 偏移優先 EINVAL。SET/CUR 共用游標，END 需要明確 size；目錄 mmap 為 EINVAL。fstatat64 支援 0、`AT_SYMLINK_NOFOLLOW=0x20`、`AT_SYMLINK_NOFOLLOW_ANY=0x800`、`AT_FDONLY=0x400`（忽略路徑）；非法位為 EINVAL，`AT_REALDEV=0x200` 未支援。串流身份未知，權限不是存取控制模型；寫入仍待完成。`directories` 程式對照原生與五種客體，stat 比對真實檔案及目錄。Intel HVF Actions 保持暫停。

[XNU VFS](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/fcntl.h).

目錄驗收（2026-10-05，Release）：467 項登記，227 通過、240 跳過、零失敗；ARM64 HVF 必需項 60/60 執行。10 個原生 macOS 程式、37 項公共 C/CLI/報告（無跳過）、Python 五種客體和 66 項驗收腳本通過，計數重疊。證據：`build-hvf-arm64/darwin-directory-verified-evidence/`。其他原生後端與 iOS 真機尚未驗收。

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## 明確目錄快照

`getdirentries64` (344) 列舉既有 `directories` 項目的可選唯讀 `contents`；C++ 使用 `DarwinFileOptions::DirectoryContents`。`entries` 必須依明確順序完整列出 `.`、`..` 和所有直接子項。空目錄缺少快照仍表示未知；快照不建立路徑或 stat 中繼資料，也不查詢宿主檔案。

每項必填 `name`、非零 `inode`、`type`（0 未知、4 目錄、8 一般檔案）、`next_offset`、`seek_offset`。類型必須符合路徑；相同解析路徑的 inode 必須與其他快照及中繼資料一致。`next_offset` 是目錄內唯一、非零、不超過 INT64_MAX 的游標標記，不必遞增；零表示回到開頭。`seek_offset` 是獨立的無符號 64 位 d_seekoff 觀察值，可重複為零。整數沿用 stat 的無損十進位字串規則。

`contents.minimum_buffer_size` 必填，表示包括 EOF 的載荷下限，範圍 1–128 MiB。項目可另設 `minimum_buffer_size`（預設 0），約束從該項開始的讀取。範例記錄 APFS 起始兩個點項至少需 64 位元組、EOF 只需 1 位元組；其他位置至少容納完整記錄。LP64 記錄按 8 位元組對齊，大小為 `roundUp(25 + nameBytes, 8)`。所有快照最多 4096 項，編碼位元組計入 16 MiB 預算；僅中繼資料/快照宣告的祖先路徑去重後計入 256 路徑上限；JSON 仍限 64 KiB。

獨立 open 有獨立游標，dup 共用；只接受零或宣告的標記，未知位置明確停止。每次回傳能容納的最大完整記錄前綴。長度 >=1024 時，原請求末四位元組為 EOF 旗標（末尾 1，否則 0），僅記錄載荷限制為 128 MiB；旗標位址保留原無符號長度及回繞。順序是複製資料、移動游標、複製讀取前位置、寫旗標。後續 EFAULT 保留已完成效果；EOF 不複製空資料。單次複製僅部分可寫時，在該次複製前明確停止，保留先前效果。

同一 `directory-entries` 對照原生 macOS 的記錄、dup/回繞、小塊讀取、EOF 和複製順序；獨立測試依 SDK 逐位元組核對捕獲記錄及長檔名。快照標記回繞後不變，不模擬 APFS 動態世代。舊 `getdirentries` (196)、目錄寫入、其他原生後端及 iOS 實機尚未納入驗收。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"directories":[{"path":"/empty"},{"path":"/","contents":{
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":73,"type":8,"next_offset":99,"seek_offset":0}]}}]}}
```

[XNU getdirentries64](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [dirent ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [extended flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent_private.h).

目錄列舉驗收（2026-10-05，Release）：Darwin 共 498 項，246 通過、252 項因後端不可用跳過、零失敗；ARM64 HVF 必需項 63/63 執行。11 個原生 macOS 程式、40 項公共 C/CLI/報告（無跳過）、Python 五種客體组合各八種檔案情境及 66 項驗收腳本通過，計數重疊。證據：`build-hvf-arm64/darwin-dirents-merged-evidence/`。Intel HVF Actions 保持暫停；其他原生後端和 iOS 實機未驗收。


## 私有檔案映射

`mmap` 支援一般目錄檔案的 `MAP_PRIVATE` 映射：`flags=0x2`，或加上 `MAP_UNIX03` 的 `0x40002`；偏移必須按 OS 頁對齊。即使要求長度較短，整頁仍保留原始檔案位元組，EOF 尾頁剩餘部分清零。私有寫入不改變原檔、其他映射、固定中繼資料或共用游標。close 或重用描述元後映射仍有效。唯讀與 PROT_NONE 映射也有完整初始內容，可透過 `mprotect` 增加寫入權限。

檔案末尾溢位、UNIX03 零長度或未對齊偏移在 FD 查詢前返回 EINVAL；無效 FD 在預算檢查前返回 EBADF。舊式零長度仍檢查 FD，然後返回零而不配置。舊式未對齊偏移、串流、空檔頁和完整越過 EOF 的頁在配置前明確停止。原生 macOS 允許映射 EOF 外完整頁，但存取會觸發 SIGBUS；模型不偽造零頁或訊號傳遞。共享、固定、可執行與 JIT 映射仍未支援。

`DarwinFiles` 管理描述元和位元組，`DarwinMemory` 管理配置、權限、預算與回滾；資料只來自 `darwin_files`。`file-mapping` 原生/客體程式檢查私有寫入、close 後壽命、游標、錯誤與匿名頁重用；獨立原生測試比對非零偏移、整頁內容與 SIGBUS 邊界。

[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c)

### 私有映射驗證（2026-10-05）

Release Darwin：438 個唯一登記項，210 通過、228 跳過、零失敗；57/57 個 ARM64 HVF 必需項執行，Unicorn 涵蓋五種客體組合。9 個原生 macOS 程式、非零偏移整頁比對與隔離子程序 SIGBUS 驗證通過。36 項公共介面/報告無跳過，Python 五種組合含 `file-mapping`、66 項驗收腳本和 38 項來源回歸通過，計數重疊。證據：`build-hvf-arm64/darwin-mmap-verified-evidence/`。尚無新增 Intel HVF/KVM/WHP 或 iOS 真機證據；Intel HVF Actions 繼續暫停。

## 明確的檔案中繼資料

檔案項目可提供 `metadata`，下例全部欄位均必填。十進位字串保留完整整數精度，JSON 數字限於 ±(2^53−1) 內的精確整數。device 為有號 32 位，mode/link_count 為無號 16 位，inode 為無號 64 位，uid/gid/flags/generation 為無號 32 位。size 必須等於檔案位元組數；blocks 不超過有號 64 位上限，block_size 為非負有號 32 位。四個時間使用有號 64 位秒及 0–999999999 奈秒，mode 必須符合檔案或目錄類型。

`stat64` (338)、`fstat64` (339)、`lstat64` (340) 在 ARM64/x64 回傳相同的 144 位元組 LP64 記錄。路徑解析與 open 共用；FD 查詢遵循複製及關閉，不配置描述元也不改變游標。rdev、填補和保留欄位清零。輸入提供初始中繼資料，可選修改策略決定後續變更；讀取不推進時間，mode 不改變存取授權。缺少中繼資料、串流狀態、符號連結、舊版 stat及擴充安全查詢仍不支援。路徑/FD 錯誤先於目的位址檢查；部分可寫輸出在寫入前停止。原生測試逐位元組對比真實檔案狀態並核對 SDK 配置，同一自編程式驗證三種呼叫。

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

後續依序補共享映射與 EOF 缺頁、有界寫入（驗證 EOF 頁、close 後映射壽命與錯誤順序），顯式時間/系統資訊、必要 Mach/執行緒服務、Mach-O 相依性與重定位/繫結、初始化/TLS，再以原生程式推進 Objective-C/Swift 與 Foundation/UIKit。iOS 真機比對需要 SDK 與設備；Intel HVF 尚未驗證，Actions 繼續暫停。



```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

獨立工作負載驗收要求 ARM64 69 項或 x64 46 項全部執行，包含每個平台的 `LC_MAIN` 與 `LC_UNIXTHREAD`。必需項缺失、跳過或缺少 `ld64.lld` 都會失敗。

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

## 明確的時間觀察值

`ProcessOptions::DarwinTime` / `darwin_time` 為所有 Darwin profile 提供 raw `gettimeofday` (116) 的固定觀察值，包含第三個 `mach_absolute_time` 輸出。`time_of_day`、`timezone` 和 `mach_absolute_time` 都可省略；省略代表未知，明確的零是有效值，空物件不會補上預設時鐘。模型不讀取主機時鐘、不推斷時區、不推進時間，也不換算絕對 tick。

提供記錄時必須包含全部成員。`seconds` 是無號 32 位，`microseconds` 為 [0, 999999]，`minutes_west` / `dst_time` 為有號 32 位，絕對 tick 為無號 64 位。JSON 沿用無損整數規則：超出安全整數範圍時使用十進位字串。未知欄位、越界值及非 Darwin profile 在載入映像前拒絕。

LP64 `timeval` 為 16 位元組：偏移 0 是零擴展秒數，偏移 8 是 32 位微秒，偏移 12 的四位元組填充為零。時區是兩個有號 32 位欄位，tick 佔八位元組。日曆與絕對時間先聯合取樣，因此請求的兩種觀察值必須在任何複製或指標檢查前存在。接著依序寫入 timeval、timezone、absolute ticks。時區缺失在自身階段停止，保留先前的 timeval；後續 EFAULT 也保留先前寫入，重疊位址依相同順序處理。單次輸出僅部分可寫時，在該次複製前明確停止，保留更早的複製。三個空指標無須設定即可成功；選擇性查詢只要求所請求的值。

原創 `time` 工作負載核對原生行為；來賓 `time-values` 在五種來賓組合的 C/CLI/Python 路徑輸出設定的精確 32 位元組。獨立 SDK 對照從一次原生 raw 呼叫擷取三輸出，再逐位元組比較。此功能不包含時鐘推進、tick 換算、commpage 計數器、計時器或 Mach 時鐘物件/IPC；dyld、執行緒、Objective-C/Swift 和 Foundation/UIKit 仍待完善。Intel HVF Actions 維持暫停，本輪不增加原生 Intel 或實體 iOS 驗收結論。

```json
{"darwin_time":{"time_of_day":{"seconds":4045620583,"microseconds":654321},"timezone":{"minutes_west":-480,"dst_time":-1},"mach_absolute_time":"18364758544493064720"}}
```

[XNU gettimeofday](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_time.c), [time ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/time.h).

時間驗證（2026-10-06，Release）：538 項 Darwin 註冊測試，274 項通過、264 項因後端不可用跳過，零失敗；66/66 項必要 ARM64 HVF 測試全部執行。12 個原創原生 macOS 工作負載及單次取樣的 SDK 位元組對照通過。公共 C/CLI/報告為 43/43，無跳過。Python 五種來賓組合通過，包含精確時間位元組與原有八種檔案情境。66 項執行器測試、本地化、能力清單與格式檢查通過。計數重疊。證據：`build-hvf-arm64/darwin-time-verified-evidence/`、`darwin-time-native-first/`、`darwin-time-public.xml`。

## Mach 時間與返回約定

`darwin_time.timebase` 提供非零無符號 32 位 `numerator` 和 `denominator`；比例原樣保留，不約分或換算。`mach_timebase_info_trap` 索引 89 對應 ARM64 X16=-89 或 x64 RAX=0x01000059，寫入小端分子、分母共八位元組並返回零。完全無效的輸出位址仍返回零；部分可寫輸出在複製前停止，底層傳輸錯誤繼續傳播。缺少 timebase 時在指標檢查前停止，包含空指標。

ARM64 X16=-3、X16=-4 返回完整無符號 64 位 `mach_absolute_time`、`mach_continuous_time`。各自僅需對應觀測值，明確的零有效。x64 對應原生表項會觸發 EXC_SYSCALL，模型明確拒絕。時鐘推進、commpage、計時器及 Mach 時鐘物件/IPC 尚未實作。

分派僅用編號低 32 位，報告保留原始 64 位。ARM64 負數選擇 Mach；x64 Mach 類別為 0x01000000，BSD 為 0x02000000。BSD 3/4 仍為 read/write；未知編號及外來類別停止。綁定統一決定返回約定：Mach 保留旗標與 X1/RDX，BSD 保留既有 carry 規則；x64 仍更新 RCX/R11。Mach 記錄含 `result` 並省略 `error`，即使輸入 carry 已設定。

原創 `mach-time` 對照 ARM64 原生旗標、次結果、高位編號、無效指標與 BSD 切換。`mach-timebase-values` 在五種來賓輸出精確位元組，`mach-clock-values` 在 ARM64 驗證完整 tick；SDK 另驗證配置與原生比例。Intel HVF Actions 維持暫停，x64 軟體與語法檢查不構成原生 Intel 或實體 iOS 驗收。

```json
{"darwin_time":{"timebase":{"numerator":125,"denominator":3},"mach_absolute_time":"18364758544493064720","mach_continuous_time":"18446744073709551615"}}
```

[XNU clock traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/clock.c), [ARM64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [ARM64 special traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/sleh.c), [x64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/x86_64/idt64.s).

Mach 驗證（2026-10-06，Release）：Darwin 569 項中 293 通過、276 項後端不可用而跳過、零失敗；ARM64 HVF 必需 69/69 全部執行。最終通過 13 個原生工作負載及兩個時間 SDK 對照。公開 C/CLI/report 為 100/100、無跳過；Python 覆蓋五種來賓。公開比較依平台與場景獨立執行，明確給予 10 秒來賓預算，產品預設與超時回歸不變。計數重疊。

首次原生啟動曾超過既有 5 秒門限，測得 6.056 秒，復用後為 0.010 秒；同一二進位檔隨後在原門限通過 13 項，保留失敗記錄。宿主負載下的超時經獨立串行複核通過。證據：`build-hvf-arm64/darwin-mach-time-final-evidence/`、`darwin-mach-time-native-recheck/existing-binary-recheck.json`、`darwin-mach-time-public-accepted.xml`，描述提交前工作樹。當時 ARM64 MRS/MSR NZCV 尚未納入受檢查 CPU 契約，因此測試以整數指令觀察旗標。下述增量已補上這項 CPU 缺口。

## ARM64 條件旗標暫存器

共用的受檢查 ARM64 契約在 EL0、EL1 接受精確的 `MRS Xt, NZCV` 與 `MSR NZCV, Xt` 編碼。讀取只回傳第 31–28 位元；寫入只取輸入的這四位，其餘忽略。讀至 `XZR` 會丟棄結果；由 `XZR` 寫入會清除四個旗標，不會讀取 SP。各後端執行原始指令。宿主暫存器設定介面的驗證、FPCR/FPSR 的限制策略不變；鄰近且未列出的系統暫存器仍明確不支援。

`NeverDAArch64NZCVTests` 將全部旗標組合與宿主原始指令對照，並檢查完整純量/向量狀態、記憶體、暫存器邊界、觀察器停止/失敗、還原上下文重試及共用指令預算。ARM64 `mach-time` 現在以真實 MSR/MRS 包圍 SVC，涵蓋 Mach 旗標保持及返回 BSD 的切換。原生 HVF 必要項包含兩種權限下的六個方法與宿主對照。ARM64 KVM/WHP、實體 iOS 尚未驗證。可寫檔案、系統資訊、推進時鐘、Mach IPC/執行緒、dyld/執行環境/框架及裝置驗收仍待完成。

[Arm NZCV (DDI0601, 2025-06)](https://developer.arm.com/documentation/ddi0601/2025-06/AArch64-Registers/NZCV--Condition-Flags).


可寫檔案驗證（2026-10-06）：Release Darwin 610 項中 322 通過、288 後端不可用而跳過、零失敗；ARM64 HVF 必需項 72/72 執行。最終專項 114 項中 102 通過、12 跳過，包含新增 EFAULT 中繼資料斷言；15 個原生程式與 111 個公共 C/CLI/報告測試全數通過。計數重疊。首次原生用例發現 FWASWRITTEN 遺漏，修復後通過，失敗證據保留。時限未變；完整 GitHub CI 與 iOS 實機仍須另行驗收，Intel Actions 暫停。

`build-hvf-arm64/writable-darwin-evidence/` · `writable-native-final/` · `writable-focused-final.xml` · `writable-public.xml`

Python 首次整組測試的三個 ARM64 目錄案例超時。不改參數的診斷中，可寫案例 10/10 通過，但一個 iOS 目錄呼叫牆鐘 5.005 秒、CPU 1.263 秒後超時。同一 5 秒限制下單獨複驗三者全通過（2.43–3.17 秒、10,941 條指令、輸出 65）。16 邏輯核負載為 54–70，支持排程壓力解釋，不保證延遲穩定；保留原失敗。

最終未改參數的 Python 整組測試涵蓋五種 profile/ISA 並通過，耗時41.118秒；每行程仍限5秒，先前失敗及診斷獨立保留。


中繼資料策略驗證（2026-10-06）：Release專項148項為124通過/24跳過。完整Darwin645項為343通過/300跳過/2項既有ARM64 HVF目錄逾時；相同20項與原5秒時限複測為8通過/12跳過，受影響項3.818/3.949秒。75項必需HVF都有通過觀察，首次整輪失敗仍保留。公共C/CLI/報告117/117（Darwin73項）、Python五組27.359秒、原生15/15、runner66/66通過。分配策略非APFS證據；未改時限，完整CI、Intel、iOS實機和完整環境仍待驗收。

`build-hvf-arm64/mutation-metadata-validation-summary.json`; `mutation-metadata-darwin-evidence/`; `mutation-metadata-directory-recheck/`; `mutation-metadata-focused.xml`; `mutation-metadata-public.xml`.
