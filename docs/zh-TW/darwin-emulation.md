**語言**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: b3b4285b341fef4afed8cfe49f7fe8396536b9e63f924d14402ec7d0d3eb0b01 -->

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

服務包含 `exit`、`write`、`getpid`、`getppid`、`getuid`、`geteuid`、`getgid`、`getegid`、`mmap`、`mprotect`、`munmap`。PID/UID/GID 固定為 1000，PPID 為 1。描述元 1、2 擷取原始位元組，包含 NUL 與非 UTF8；其他描述元返回 EBADF。部分複製已取得的資料會保留，但後續錯誤仍為 EFAULT。長度超過 `INT_MAX` 時，先返回 EINVAL，再談描述元、指標或預算檢查，依據 [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c)。

記憶體服務支援私有匿名資料映射：`flags=0x1002`、描述元 -1、offset 零。長度及非固定提示位址向上取整至 OS 頁；提示已占用時先向上搜尋，再回到預設配置區。舊式原始 mmap 零長度返回零且不配置；`MAP_UNIX03` 不在契約內。Unmap/protect 位址必須對齊。支援 NONE/READ/WRITE，WRITE 隱含 READ。每個 OS 頁獨立持有實體記憶體，部分解除映射可釋放預算，新頁面清零。Protect 跨空洞或超過最大權限時，整個範圍維持原狀。來源：[XNU VM 服務](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c)。

檔案／共享／固定／JIT 映射、匿名可執行映射、Mach trap、間接系統呼叫、執行緒、訊號、檔案／網路、dyld、Objective-C/Swift runtime 和 Foundation/UIKit 均不支援，會明確停止。這不是完整 Apple OS，也不是 iOS Simulator 應用程式。

## 驗證

自行撰寫的 C 測試映像以 Clang 與 `ld64.lld` 產生，不依賴 Apple SDK 或專有二進位檔。涵蓋五種平台／ISA、畸形 Mach-O、4/16 KiB 頁與預算已滿時的部分釋放。`NeverDProcessPublicTests` 比對 C API/CLI；設定 `NEVERD_TEST_LIBNEVERD` 與 `NEVERD_TEST_DARWIN_FIXTURES` 可讓 Python SDK 測試相同五種組合。

```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

獨立工作負載驗收要求 ARM64 39 項或 x64 26 項全部執行，包含每個平台的 `LC_MAIN` 與 `LC_UNIXTHREAD`。必需項缺失、跳過或缺少 `ld64.lld` 都會失敗。

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
