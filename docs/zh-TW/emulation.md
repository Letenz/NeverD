**語言**：[English](../emulation.md) | [简体中文](../zh-CN/emulation.md) | [繁體中文](emulation.md) | [日本語](../ja/emulation.md) | [한국어](../ko/emulation.md) | [Français](../fr/emulation.md) | [Deutsch](../de/emulation.md) | [Español](../es/emulation.md) | [Italiano](../it/emulation.md) | [Русский](../ru/emulation.md) | [العربية](../ar/emulation.md)

<!-- i18n-source: 958937aacc6b214e71730502c8f4d249d0c6417721a28a32c65de1e4fa927d63 -->

[← 文件索引](README.md)

# CPU 執行與客體環境

<!-- i18n-section: backends -->

## CPU 後端與工作負載

CPU 執行分離 ISA 准入、客體記憶體、後端傳輸與客體 OS 策略。`NEVERD_ENABLE_CPU_EMULATION` 啟用 x64/ARM64 CPU 層；`NEVERD_ENABLE_DRIVER_EMULATION` 加入有界 x64 Windows WDM/KMDF 環境。`linux-elf64-v1` 設定檔執行受支援的 Linux ELF 程序。參見[CPU 執行](cpu-execution.md)、[客體程序模擬](process-emulation.md)及[Windows 驅動程式模擬](driver-emulation.md)。

對於支援的原生契約，`auto` 在來賓 ISA 與宿主一致時選擇 Linux 的 KVM、Windows 的 WHP 或 [macOS 的 HVF](macos-hvf.md)。跨 ISA 使用 Unicorn；`software-cpu-v1` 和原有 V1 API 保留軟體執行。明確選擇的後端無法使用時直接失敗，不自動回退。原生執行在進入 CPU 前檢查指令准入、位址及效果。宿主虛擬化不決定來賓 OS：[Darwin 設定](darwin-emulation.md) 獨立建模 macOS、iOS 與 iOS Simulator。HVF 需要 `com.apple.security.hypervisor` 權限。

`driver-strict` / `checked-x64-v1` 涵蓋有界 x64 執行；Windows 驅動載入仍限 x64。`checked-aarch64-v1` 和 `checked-user-aarch64-v1` 包含有界 ARM64 FP32/FP64、定寬 SIMD 及完整 FPCR/FPSR/向量狀態。Mac 指南已記錄 ARM64 HVF 原生驗收；ARM64 KVM/WHP 工作負載驗證與 Intel HVF 完整驗收仍待完成。CPU 執行受支援不代表相容任意驅動或應用。

<!-- i18n-section: windows-processes -->

## Windows 程序與模組

`windows-pe64-v1` 支援有界 Windows x64/ARM64 主控台程序，包括 PEB/TEB、靜態與動態 TLS、`DllMain`、具名 Win32 API 和明確的無環 DLL 圖。客體模組支援依名稱／序號匯入程式碼與資料、DIR64 重定位、轉送匯出及真實載入器串列身分。`LoadLibraryA`／`LoadLibraryW`、`FreeLibrary` 和 `GetProcAddress` 使用設定的模組目錄。CRT／GUI、ARM64 以堆疊框架為基礎的使用者態 SEH、執行緒及通用 Windows 應用程式相容性仍待完成；原生 ARM64 KVM/WHP 證據仍缺失。

`WindowsSystemModules` 為兩種 ISA 建立有界的 `ntdll.dll`、`kernelbase.dll` 與 `kernel32.dll` PE64 模型映像。ASCII `GetModuleHandleA` / `GetModuleHandleW`、`LoadLibraryA` / `LoadLibraryW` 和 `GetProcAddress` 共用映射基址；PEB/LDR 與 `MEM_IMAGE` 描述相同映像。靜態匯入、名稱查詢與客體 DLL 轉送使用相同 API 跳板及匯出解析器。提供者固定駐留，不執行客體初始化回呼，普通客體 DLL 全部卸載後不會阻止進入點傳回。標頭或匯出中繼資料改變會停止查詢。未知系統匯出名稱與非零系統序號查詢明確停止；已建模名稱的大小寫不符及空名稱傳回錯誤 127，空指標查詢傳回 87。產生的位元組與位址屬於模型策略，不重建特定 Windows DLL 配置、原生序號或跨提供者別名。`WindowsSystemTests.cpp` 對照原始 x64/ARM64 EXE 與原生 Windows，並獨立觀察八次初始執行緒傳回。

<!-- i18n-section: environment-memory -->

## 環境變數與記憶體

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` 共用 PEB 程序參數中的即時客體環境區塊。名稱限 ASCII 且忽略大小寫，值為 UTF-16。修改前驗證輸入、容量及可寫記憶體。快照不受後續修改影響，釋放時回收客體記憶體。模型的環境區塊上限為 64 KiB；字串與展開操作有明確邊界並檢查工作負載期限。未知指標歸屬、格式錯誤的環境區塊、ANSI 字碼頁及展開緩衝區重疊仍不支援。`WindowsEnvironmentTests.cpp` 在可用後端比較原創 x64/ARM64 範例，CI 必須執行獨立的原生 Windows 對照。

`WindowsProcessHeap` 統一管理程序堆積的配置、`HeapReAlloc`、釋放和大小查詢。調整大小保留原有有效資料；`HEAP_ZERO_MEMORY` 清零新增位元組，`HEAP_REALLOC_IN_PLACE_ONLY` 禁止搬移。重新配置失敗時保留舊區塊，傳回 NULL 並設定 `ERROR_NOT_ENOUGH_MEMORY`（8），與原生觀測一致。獨立頁記憶體使縮減和釋放能歸還容量，分階段擴充及有界複製檢查工作負載期限。自訂堆積、例外產生旗標、未知歸屬及無法存取的複製或清零範圍均明確停止。`WindowsHeapTests.cpp` 涵蓋兩種 ISA、強制搬移、預算重用及失敗原子性；CI 也在原生 Windows 上執行同一原創 EXE。

Windows 虛擬記憶體新增 `VirtualAlloc`、`VirtualFree`、`VirtualProtect`、`VirtualQuery` 及目前行程的 `FlushInstructionCache`。OS 層管理保留區域，`AddressSpace` 統一管理已認可頁面、權限和實體儲存。測試涵蓋動態程式碼改寫、存取錯誤和記憶體額度回收。

<!-- i18n-section: vectored-exceptions -->

## 向量例外與繼續執行

`WindowsProcessExceptions` 在同一 CPU 與程序預算內實作 `AddVectoredExceptionHandler`、`RemoveVectoredExceptionHandler` 和 `RaiseException`。有序處理器可註冊或移除處理器、觸發巢狀例外、呼叫已建模 API、載入 DLL 及結束程序。x64/ARM64 資料存取例外與 x64 整數除法例外可在驗證客體對 `CONTEXT` 的修改後恢復；一般暫存器、SIMD 與受支援的浮點狀態會保留。軟體例外經模型提供者中的實際返回指令繼續執行。模型最多保留 128 個註冊項、巢狀 16 層。非法處置值、遭修改的例外指標、不支援的內容欄位及超限皆明確失敗。ARM64 以堆疊框架為基礎的 SEH／展開、偵錯器派送及執行／防護頁例外仍不支援。`WindowsExceptionTests.cpp` 將原創 EXE／DLL 情境與原生 Windows 比較；原生 ARM64 KVM/WHP 證據仍待補齊。 軟體例外記錄帶有 `EXCEPTION_SOFTWARE_ORIGINATE`（`0x80`），與呼叫者傳入的不可繼續旗標分別處理；原始 Windows 執行檔精確核對軟體例外和硬體例外的旗標值。

`AddVectoredContinueHandler` 與 `RemoveVectoredContinueHandler` 管理獨立的有序串列，與例外處理器共用最多保留 128 個註冊項的限制。向量例外處理器接受繼續執行後，繼續處理器讀取同一份可修改的例外記錄與 `CONTEXT`；最終內容驗證在這些回呼完成後進行，包含巢狀例外與 DLL 通知。兩類處理器的控制代碼不可交叉移除。`WindowsContinuationTests.cpp` 將順序、提早結束派送、增刪、內容修復、巢狀派送、載入器回呼及程序結束的原創 EXE 案例與原生 Windows 比較。已測 Windows x64 向量處理路徑允許在設定 `EXCEPTION_NONCONTINUABLE` 時繼續執行；這不代表以堆疊框架為基礎的 SEH 行為。原生 ARM64 執行仍未驗證。

<!-- i18n-section: caller-context -->

## 呼叫者上下文

`RtlCaptureContext` 已透過 `kernel32.dll` 和 `ntdll.dll` 支援 x64、ARM64。共用的 `WindowsProcessContext` 與 `IntegerABI` 保存呼叫者 PC/SP，不修改 CPU 狀態或 LastError。原生 Windows 觀察確認 x64 旗標為 `0x10000f`，未涉及的 home／除錯／向量儲存保持原樣，x87 位址欄位保留傳統的低 32 位元；ARM64 從 LR 保存 PC，並清零記錄中的 X0/LR。暫存器、SIMD 與浮點控制來自客體；x64 選擇子和 MXCSR 能力遮罩遵循設定的客體 CPU。無效、未對齊或部分無法存取的目標記錄在寫入前明確失敗。`WindowsContextTests.cpp` 涵蓋靜態匯入、提供者查詢、VEH 回呼、跨頁輸出及失敗原子性。`scripts/check_windows_context.py` 在原生 Windows x64、ARM64 上執行原創程式，並另外驗證非空 x87 狀態。這些 ARM64 API 觀察不代表原生 KVM/WHP 執行驗證。內容還原、堆疊回溯和動態函式表仍待實作。 `WindowsProcessServices.def` 宣告精確的模組限制：模型在 `kernelbase.dll` 中查詢此符號時傳回 `ERROR_PROC_NOT_FOUND`（127），與原生觀察一致，不憑空新增匯出。 [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

<!-- i18n-section: structured-exceptions -->

## 結構化例外處理

`WindowsProcessSEH` 使用 `os/windows/exception/` 中共用的 `X64SEH`（`NeverDEmulationWindowsException`，無需啟用驅動環境），處理 x64 `__C_specific_handler` 和 UNWIND_INFO V1。VEH 搜尋結束後支援篩選器、finally 回呼、非區域處理器跳轉、巢狀／衝突展開與重定位 EXE/DLL 堆疊框架，保留非揮發 GPR/XMM 狀態。篩選器選擇繼續執行時，VCH 使用同一份 `CONTEXT`。`WindowsSEHTests.cpp` 將 23 個原創情境與原生 Windows 比較；KVM/WHP/Unicorn 共用這些語意。派送在程序預算內重新驗證映像世代、標頭、展開／範圍位元組、語言處理器程式碼區域及 IAT 繫結。中繼資料遭修改或保留的映像被卸載時明確失敗。ARM64 框架式 SEH、C++ EH、動態函式表、通用 RtlUnwind/NtContinue、跨載入器／VEH／VCH 回呼邊界展開仍不支援。

當記錄包含 `EXCEPTION_NONCONTINUABLE` 而 x64 篩選器傳回 `EXCEPTION_CONTINUE_EXECUTION` 時，系統使用新上下文派送 `STATUS_NONCONTINUABLE_EXCEPTION`（`0xc0000025`，旗標 `0x81`，關聯記錄指標為空）。先重新執行 VEH，再從保留的邏輯堆疊重新搜尋，在相同深度與執行預算內保留 finally 順序及 EXE/DLL 框架身分。23 個原生情境包含 21 個成功執行及兩個終止情境：即使還原原始 `CONTEXT`，VEH/VCH 接受繼續這個二次例外後，它仍未處理。模型將此結果回報為執行期失敗。軟體例外位址等於儲存的 PC；內部派送器位址及暫存器配置由模型定義。 [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

<!-- i18n-section: processor-state -->

## 指令與處理器狀態

受檢 x64 現支援一般 RAM 上的 `MOVS/STOS/LODS` 與 `CLD/STD`，並逐元素驗證恢復、取消及跨頁存取。依賴 CPU 型號的零次數高位元行為與 STOS/LODS 裝置運算元仍不在契約內。

受檢 x64 也支援一般 RAM 上的 `CMPS/SCAS` 與 `REPE/REPNE`，涵蓋算術旗標、提前終止、逐元素停止與錯誤恢復；裝置比較仍不支援。

x64 與 ARM64 原生啟動自檢在獨占記憶體租約下驗證有界的完整狀態執行。

原生 x64 的 `FOP/FIP/FDP` 遵循主機儲存、還原規則：AMD 可能清零未生效的 x87 例外中繼資料。啟動自檢透過未遮罩的待處理例外驗證這些欄位。
