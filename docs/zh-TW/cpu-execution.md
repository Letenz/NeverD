**語言**：[English](../cpu-execution.md) | [简体中文](../zh-CN/cpu-execution.md) | [繁體中文](cpu-execution.md) | [日本語](../ja/cpu-execution.md) | [한국어](../ko/cpu-execution.md) | [Français](../fr/cpu-execution.md) | [Deutsch](../de/cpu-execution.md) | [Español](../es/cpu-execution.md) | [Italiano](../it/cpu-execution.md) | [Русский](../ru/cpu-execution.md) | [العربية](../ar/cpu-execution.md)

[← 文件索引](README.md)

# CPU 執行與能力查詢

CPU 執行獨立於客體 OS、映像載入器與呼叫慣例。啟用 `NEVERD_ENABLE_CPU_EMULATION` 可單獨建置；`NEVERD_ENABLE_DRIVER_EMULATION` 也會包含 Windows 驅動程式環境。[架構指南](architecture.md)說明所有權、後端選擇與目前的平台限制。

## 組態

公開 [`ExecutionConfiguration`](../../include/neverd/emulation/ExecutionConfiguration.h) 同時供 CPU factory 與能力報告使用。配置 CPU 或附加位址空間前會先驗證需求。省略值採用契約固定設定；明確指定的不支援值會失敗。

| JSON 欄位 | 預設值 | 意義 |
|---|---|---|
| `backend` | `auto` | `auto`、`unicorn`、`kvm` 或 `whp` |
| `contract` | `software-cpu-v1` | 有版本的執行語意 |
| `architecture` | `x86_64` | `x86_64` 或 `aarch64` |
| `privilege` | 契約設定 | `flat`、`supervisor` 或 `user`，必須符合所選契約 |
| `virtual_address_bits` | 契約設定 | checked 設定為 48 位元；flat 設定為 64 位元直接對映命名空間 |
| `page_size` | 4096 | 客體對映粒度；拒絕其他值 |
| `required_features` | `[]` | [`ExecutionConfiguration.def`](../../include/neverd/emulation/ExecutionConfiguration.def) 中的必要功能名稱 |

`driver-strict` 接受 x64；`software-cpu-v1` 接受 x64 與 ARM64。`checked-x64-v1`、`checked-aarch64-v1` 要求相符架構並以 supervisor 權限執行。`checked-user-x64-v1`、`checked-user-aarch64-v1` 分別在 CPL3、EL0 執行相同的有限純量指令集合，並具備 MMU 隔離與明確的服務要求退出。支援 Unicorn 與符合主機條件的 KVM/WHP；`auto` 沿用主機選擇。flat 設定不保證架構層級的 user/supervisor MMU 隔離。所有 checked 設定均拒絕 FP/SIMD、MMIO、連接埠 I/O 與平行 CPU 需求。只有 user 設定會宣告 `service_traps`。

使用者執行要求**每個**對映頁同時具備 `UserAccessible` 與適當的 `Read`、`Write` 或 `Execute` 權限。既有對映預設供 supervisor 使用；即使別名共用實體位元組，權限仍各自獨立。只有 `UserAccessible` 不會授予存取權。可信任的主機操作與 supervisor CPU 使用 RWX。範例：

```cpp
Configuration.Contract = ExecutionContract::CheckedUserX64;
Configuration.Privilege = ExecutionPrivilege::User;
auto CPU = llvm::cantFail(createExecutionBackend(Configuration, Space)).CPU;
llvm::cantFail(CPU->map(Code, 4096, Read | Write | Execute | UserAccessible));
```

權限由契約固定；還原內容或繫結位址空間不會改變權限，寫入 x64 區段選擇子也不能提升權限。可復原的資料存取錯誤會保留原指令與暫存器，直到所屬模型處理。`canAccess` 精確檢查要求的權限；查詢 user 可見性時要包含 `UserAccessible`。頁表是 CPU 私有投影，不開放可變更的客體頁表或權限切換 API。ARM64 的 user 頁在 EL1 也不可執行。

未知/null 欄位、無效名稱或數字寬度、重複必要功能及不支援的組合都會失敗。輸入上限為 64 KiB；舊 CPU factory 和驅動程式 C 選項維持相容。

## 不執行工作負載的查詢

```bash
neverd cpu-capabilities
neverd cpu-capabilities \
  --configuration='{"contract":"checked-aarch64-v1","architecture":"aarch64"}' \
  --probe-host
```

schema 版本為 1。報告分別列出補齊設定前的 `requested_configuration`、正規化後的 `configuration`、靜態語意能力 `capabilities`、adapter 建置與 ABI 相容性 `build`，以及僅在指定 `--probe-host` 時提供的 `host`（未指定為 null）。主機探測會在私有 RAM 上初始化暫存 CPU，只證明可初始化，不代表工作負載適用或具備原生 ARM64 執行能力。可用性可能改變；不可用後端不會被靜默替換。合法報告即使後端不可用，CLI 仍回傳 0；組態或查詢無效則回傳 1。

## SDK 與 C++ 邊界

[`neverd_cpu_capabilities_json`](../../include/neverd/sdk/NeverDCAPICPU.h) 接收既有 session、選用組態 JSON 與值為 0 或 1 的 `ProbeHost`；不需要已載入的映像。用 `neverd_free_string` 釋放結果；NULL 代表錯誤，可由 `neverd_last_error` 查詢。停用 CPU 的建置仍匯出同一函式並明確回報停用狀態。Python 外掛使用 `session.cpu_capabilities(...)`。C++ 可分別呼叫 `executionCapabilities`、`resolveExecutionConfiguration`、`queryExecutionBackendBuild`、`probeExecutionBackend`；`createExecutionBackend` 將 CPU 附加到既有空間，或建立私有 RAM 與預設空間。

## CPU 結果與預算

`CPU.runUntilExit(PC, TimeoutMicroseconds)` 回傳具型別的 [`ExecutionExit`](../../include/neverd/emulation/ExecutionExit.h)。執行前組態錯誤回傳 `llvm::Error`；執行開始後會明確回報停止、deadline、服務要求、可復原錯誤、客體錯誤／trap、不支援操作、裝置／後端錯誤或無法解釋的引擎停止。CPU／裝置／後端錯誤優先於同時發生的 stop/deadline，但保留獨立事實與錯誤細節。timeout 必須為正值，且可表示為 duration 與絕對 deadline；否則在變更 CPU 前即失敗。每次執行都需要有限預算；0 不代表無限執行，也不是有效的立即 timeout。控制是合作式，不保證硬式牆鐘期限。結果不會消耗可復原錯誤；OS 所屬模型必須先取得錯誤並安裝經驗證的例外轉移，再恢復執行。舊有 `run`、`fault`、`timedOut` API 仍可用；只覆寫 `run` 的外部 CPU 實作會拒絕新型別邊界。

## 服務要求

checked user x64 僅攔截精確、無前綴的 `SYSCALL` 編碼；checked user ARM64 攔截 `SVC #imm16`。`SYSENTER`、`INT`、`HVC`、`BRK` 等機制仍不支援。先執行指令 observer；若沒有停止或錯誤，CPU 會在執行服務指令或進入後端**之前**回傳 `ExecutionExitKind::ServiceRequest`，並帶有類型、原始 `PC`、順序 `NextPC` 與 SVC immediate。暫存器、旗標、堆疊及權限都不變：x64 尚未套用 SYSCALL 的 RCX/R11 clobber，ARM64 也尚未進入例外向量。SVC immediate 不是通用服務編號。

要求會維持 pending，並阻擋後續執行、CPU 修改、位址空間繫結與內容擷取／還原；CPU 停止時由 OS 所屬模型透過 `takeServiceRequest()` 恰好消耗一次。該模型必須解碼 OS ABI、處理服務，並明確選擇結果暫存器與下一個 PC／例外轉移。不支援的服務要在此邊界明確失敗；重試原始 PC 會產生另一個要求，不會隱含變成 NOP 或成功結果。服務事件優先於同時發生的 stop/deadline，但客體或後端錯誤優先級更高。CPU 停止、軟體 HLT、deadline 或 trap 都不代表工作負載成功。獨立的 [Linux 程序設定檔](process-emulation.md)有自己的 OS 模型，不證明 Windows、Android 或 Darwin 可執行。Windows 與 ARM64 原生執行仍需實機執行驗證。
