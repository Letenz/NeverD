**語言**：[English](../cpu-execution.md) | [简体中文](../zh-CN/cpu-execution.md) | [繁體中文](cpu-execution.md) | [日本語](../ja/cpu-execution.md) | [한국어](../ko/cpu-execution.md) | [Français](../fr/cpu-execution.md) | [Deutsch](../de/cpu-execution.md) | [Español](../es/cpu-execution.md) | [Italiano](../it/cpu-execution.md) | [Русский](../ru/cpu-execution.md) | [العربية](../ar/cpu-execution.md)

[← 文件索引](README.md)

# CPU 執行與能力查詢

CPU 執行獨立於客體 OS、映像載入器與呼叫慣例。啟用 `NEVERD_ENABLE_CPU_EMULATION` 可單獨建置；`NEVERD_ENABLE_DRIVER_EMULATION` 也會包含 Windows 驅動程式環境。[架構指南](architecture.md)說明所有權、後端選擇與目前的平台限制。

`NEVERD_ENABLE_SEMANTIC_TESTS` 預設為 `ON`，控制 `unittests/semantic` 中的測試組及其彙總執行目標。建置不依賴 Unicorn 的原生 CPU 測試時，保留 `BUILD_TESTING=ON`，同時設定 `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` 和 `NEVERD_EMULATION_BACKEND_UNICORN=OFF`。原生 KVM/WHP 測試仍可建置，包括具備對應 SDK 標頭的 Windows ARM64/MSVC 組態。在 Windows ARM64 上啟用 Unicorn 仍需 ARM64 LLVM-MinGW 工具鏈。這項建置解耦不等於 ARM64 原生執行驗證。

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

`driver-strict` 接受 x64；`software-cpu-v1` 接受 x64 與 ARM64。`checked-x64-v1`、`checked-aarch64-v1` 要求相符架構並以 supervisor 權限執行。`checked-user-x64-v1`、`checked-user-aarch64-v1` 分別在 CPL3、EL0 執行與契約相符的有限指令集合，並具備 MMU 隔離與明確的服務要求退出。支援 Unicorn 與符合主機條件的 KVM/WHP；`auto` 沿用主機選擇。flat 設定不保證架構層級的 user/supervisor MMU 隔離。checked ARM64 與 x64 設定支援下文列出的有限 FP/SIMD 指令族。supervisor x64 另支援受限 MMIO 交易與預備讀取的字串傳輸；user 設定拒絕裝置對映。所有 checked 設定仍拒絕連接埠 I/O 與平行 CPU 需求。只有 user 設定會宣告 `service_traps`。

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

schema 版本為 1。報告分別列出補齊設定前的 `requested_configuration`、正規化後的 `configuration`、靜態語意能力 `capabilities`、adapter 建置與 ABI 相容性 `build`，以及僅在指定 `--probe-host` 時提供的 `host`（未指定為 null）。主機探測會在私有 RAM 上初始化暫存 CPU，只證明該初始化流程成功，不能證明任意工作負載相容性。可用性可能改變；不可用後端不會被靜默替換。合法報告即使後端不可用，CLI 仍回傳 0；組態或查詢無效則回傳 1。

## SDK 與 C++ 邊界

[`neverd_cpu_capabilities_json`](../../include/neverd/sdk/NeverDCAPICPU.h) 接收既有 session、選用組態 JSON 與值為 0 或 1 的 `ProbeHost`；不需要已載入的映像。用 `neverd_free_string` 釋放結果；NULL 代表錯誤，可由 `neverd_last_error` 查詢。停用 CPU 的建置仍匯出同一函式並明確回報停用狀態。Python 外掛使用 `session.cpu_capabilities(...)`。C++ 可分別呼叫 `executionCapabilities`、`resolveExecutionConfiguration`、`queryExecutionBackendBuild`、`probeExecutionBackend`；`createExecutionBackend` 將 CPU 附加到既有空間，或建立私有 RAM 與預設空間。

## CPU 結果與預算

`CPU.runUntilExit(PC, TimeoutMicroseconds)` 回傳具型別的 [`ExecutionExit`](../../include/neverd/emulation/ExecutionExit.h)。執行前組態錯誤回傳 `llvm::Error`；執行開始後會明確回報停止、deadline、服務要求、可復原錯誤、客體錯誤／trap、不支援操作、裝置／後端錯誤或無法解釋的引擎停止。CPU／裝置／後端錯誤優先於同時發生的 stop/deadline，但保留獨立事實與錯誤細節。timeout 必須為正值，且可表示為 duration 與絕對 deadline；否則在變更 CPU 前即失敗。每次執行都需要有限預算；0 不代表無限執行，也不是有效的立即 timeout。控制是合作式，不保證硬式牆鐘期限。結果不會消耗可復原錯誤；OS 所屬模型必須先取得錯誤並安裝經驗證的例外轉移，再恢復執行。舊有 `run`、`fault`、`timedOut` API 仍可用；只覆寫 `run` 的外部 CPU 實作會拒絕新型別邊界。

## 服務要求

checked user x64 僅攔截精確、無前綴的 `SYSCALL` 編碼；checked user ARM64 攔截 `SVC #imm16`。`SYSENTER`、`INT`、`HVC`、`BRK` 等機制仍不支援。先執行指令 observer；若沒有停止或錯誤，CPU 會在執行服務指令或進入後端**之前**回傳 `ExecutionExitKind::ServiceRequest`，並帶有類型、原始 `PC`、順序 `NextPC` 與 SVC immediate。暫存器、旗標、堆疊及權限都不變：x64 尚未套用 SYSCALL 的 RCX/R11 clobber，ARM64 也尚未進入例外向量。SVC immediate 不是通用服務編號。

要求會維持 pending，並阻擋後續執行、CPU 修改、位址空間繫結與內容擷取／還原；CPU 停止時由 OS 所屬模型透過 `takeServiceRequest()` 恰好消耗一次。該模型必須解碼 OS ABI、處理服務，並明確選擇結果暫存器與下一個 PC／例外轉移。不支援的服務要在此邊界明確失敗；重試原始 PC 會產生另一個要求，不會隱含變成 NOP 或成功結果。服務事件優先於同時發生的 stop/deadline，但客體或後端錯誤優先級更高。CPU 停止、軟體 HLT、deadline 或 trap 都不代表工作負載成功。獨立的 [Linux 程序設定檔](process-emulation.md)有自己的 OS 模型，不證明 Windows、Android 或 Darwin 可執行。Windows 與 ARM64 原生執行仍需實機執行驗證。

## x64 擴充與原生 CPU 狀態

checked x64 允許有限的舊式 SSE/SSE2 移動與邏輯指令、`MOVLHPS`/`MOVHLPS`，以及帶遮罩的純量 `CVTTSS2SI`/`CVTTSD2SI`/`SUBSS`/`SUBSD`。MXCSR 保留黏滯狀態、捨入與 FTZ；拒絕 DAZ 及未遮罩例外。KVM/WHP 同步全部 16 個 XMM 暫存器及 MXCSR；未列出的編碼和運算元組合仍會拒絕。

checked x64 亦支援帶遮罩的傳統 `ADD`、`SUB`、`MUL`、`DIV`、`SQRT`、`MIN`、`MAX` 的 `SS`、`SD`、`PS`、`PD` 形式。`X64SSEInstructions.def` 統一定義運算元寬度、對齊與准入規則。`MaskedSSEArithmeticMatchesIndependentHostExecution` 以獨立本機 CPU 參照驗證暫存器與 RAM 形式，涵蓋四種捨入模式、FTZ、有符號零、次正规輸入及 NaN；`SSEMemoryObserverStopsBeforeResultAndStatusChanges` 驗證停止請求先於效果提交。DAZ、未遮罩例外、x87、AVX 仍未開放。

thread pointer 包含 x64 FS/GS 基底及 ARM64 `TPIDR_EL0` 的精確 `MRS`/`MSR` 編碼。原生傳輸與 CPU snapshot 會獨立於記憶體保存狀態，但不會建立 OS 執行緒或配置 TLS 區塊。supervisor x64 支援對齊的 1/2/4 位元組純量 MMIO 交易，以及每個重新啟動邊界一個 MOVS 元素。裝置讀取必須先提供無副作用預覽，再至多提交一次。user 設定拒絕裝置對映；RMW、寬 MMIO、連接埠 I/O 也仍不支援。

KVM/WHP 會取消進行中的原生入口，並在釋放執行資源前確認取消。KVM 使用專用執行緒與暫時解除封鎖的 realtime signal；入口期間選定的 signal 不得被忽略。呼叫端的 signal mask/handler 不會變更。若客體進度不明，取消會成為終止性後端錯誤；不保證硬性 wall-clock 期限。

## x64 原生同步例外

checked x64 的 `DIV`/`IDIV` 使用處理器結果與 `#DE`。KVM 透過私有 supervisor IDT/IST 接收例外，WHP 使用明確的例外攔截位圖；原始上下文與可用錯誤碼和傳輸錯誤分開保留。OS 模型先消費可恢復事件，再安裝明確的繼續執行上下文。Windows 驅動將零除與商溢位映射為 `STATUS_INTEGER_DIVIDE_BY_ZERO`，執行實際 SEH filter、`__finally` 與重試。`NeverDX64ExceptionTests` 可停用 Unicorn 建置，`DriverWDMCPUException` 驗證原始 WDK 用例；缺少 ARM64 主機時明確跳過。

## 分階段提交 RAM 效果

`RAMTransaction` 在物理執行租約內，只保存一條指令明確宣告之寫入範圍的物理聯集。結果觀察器執行前恢復原始 RAM；取消、後端傳輸錯誤和觀察器例外不會發布部分 RAM 或暫存器。CPU 例外在 RAM 回復後保留架構例外狀態。ARM64 的單次與成對寫入共用此記憶體權威層。x64 支援 8/16/32/64 位元 `XCHG`、`XADD`、`CMPXCHG`，LOCK 或隱式鎖定形式要求自然對齊。`NeverDRAMTransactionTests` 將結果與獨立宿主 CPU 對照，並驗證回復、別名和權限；不可用的平台明確跳過。裝置交易與平行 SMP 仍不在此契約內；CPU 快照不會撤銷已提交的 RAM。

## 完整 x87 狀態

`NeverDEmulationArch` 獨立負責 ISA、頁表及 FP 狀態佈局，原生與 Unicorn 傳輸共用此層。x64 上下文保存 x87 控制、狀態、TOP、實體標籤、操作碼、指令／資料指標及八個 80 位元暫存器。`FP0`–`FP7` 使用 `RegisterValue`，純量存取拒絕截斷；`FPTag` 是實體非空位圖。`NeverDX64FPTests` 涵蓋全部 TOP、精確運算的主機 FXSAVE/FXRSTOR 對照與上下文還原。這不新增 checked x87 指令，也不證明全部捨入語義；缺少原生主機時明確略過。

`driver-strict` 支援匹配 Linux x64 主機的 KVM 與 Windows x64 主機的 WHP；`auto` 選取對應原生傳輸，跨 ISA 執行選取 Unicorn。明確指定 Unicorn 及原有 V1 API 保留可移植軟體設定。原生執行在進入 CPU 前檢查規範位址和指令效果；硬體不可用時明確失敗且不回退。不支援的指令與 OS 行為仍明確報錯。Windows x64 原生 CI 在停用 Unicorn 的設定下通過全部 299 項必測檢查：71 項 CPU 檢查、26 個內建映像與 46 個 WDK 映像及 40 個情境組合在首選和重定位位址產生的 224 項驅動程式結果，以及 4 項 SEH 邊界檢查 ([`b7d02863`](https://github.com/NeverSight/NeverD/actions/runs/36968730185)). 原生 ARM64 實機證據仍待補充，這不表示相容任意驅動程式或 Android/Darwin 環境。

使用 `executionCapabilities(Contract, ISA, Backend)` 查詢所選後端的能力設定。`NativeLegacyX64` 描述原生 x64 驅動程式執行；`NeverDNativeDriverTests` 驗證原有驅動程式集，也可在停用 Unicorn 的組建中執行。

Checked ARM64 使用統一的完整狀態提交邊界。`Registers.def` 定義 39 個純量欄位及 32 個 128 位元向量暫存器；`captureAArch64State` 暫存所有讀取、套用宣告位寬與 NZCV 正規化，最後一次提交。Unicorn、KVM 和 WHP 傳遞相同清單，包括 TPIDR_EL0、TPIDRRO_EL0、TPIDR_EL1、FPCR 和 FPSR。原生介面透過 CPACR_EL1 啟用 FP/SIMD。任何純量或向量讀取失敗、進入取消，皆保留完整呼叫方狀態。

ARM64 KVM/WHP 初始化執行私有 `AArch64MachineProbe.def` 程式：NOP、向正無窮捨入的 FP32 加法及雙通道 SIMD 加法。每步比較全部 39 個純量欄位與 32 個向量，包括 TLS、NZCV、目的暫存器高位清零及保留和累積的 FPCR/FPSR 狀態。自檢只使用特權級監控儲存，共享一個總截止時間。成功僅驗證這段有界初始化程式；仍需獨立的 ARM64 原生工作負載驗證。

x64 KVM/WHP 原生初始化在私有 supervisor 頁面執行 `X64MachineProbe.def`。單一時限涵蓋 NOP、朝正無窮捨入的 FP32 加法、雙通道 SIMD 加法、FS/GS 載入及 CS/SS/CR8 讀取；每一步比較完整的純量、XMM、實體 x87 和控制狀態。x64 與 ARM64 自檢都必須取得實體記憶體的獨占執行租約。`MemoryProjection` 統一保存快取身分（ISA、位址空間、映射世代、權限及監控變體）和各 ISA 已提交的頁表根歷史。建構器在改寫私有位元組前使快取失效；失敗的重建不能重用部分寫入的頁表，呼叫者也不能傳入過期頁表根。這些自檢僅證明有界初始化；ARM64 原生工作負載仍缺少獨立驗證。

共用 XSAVE 解碼器區分標準格式與壓縮格式的 SSE 初始狀態。XSTATE_BV[1] 清零時，兩種格式都初始化 XMM 暫存器；標準格式仍讀取並驗證 MXCSR，壓縮格式才初始化 MXCSR。`X64XsaveCases.def` 提供獨立的資料配置和原創主機 XRSTOR 程式。`X64XsaveTests.cpp` 檢查拒絕狀態的原子性，並以真實主機執行對照兩種格式，同時保留呼叫端 FP/SSE 狀態。主機架構或所需指令功能不可用時，對照測試明確略過。

`X64FPState.def` 宣告壓縮 AVX、AVX-512、CET_U/CET_S 和 AMX 傳輸配置，包括元件的 64 位元組對齊。存在的擴充資料必須符合架構的全零初始狀態；缺席元件資料與對齊填補不定義狀態。偏移由配置位元決定，未知配置、非初始資料或錯誤長度會在發布前失敗。`CompactedOffsetsFollowLayoutRatherThanPresentBits`、`WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`、`InitialCETComponentsDoNotHideFPState` 和 `InitialWideComponentsDoNotHideFPState` 涵蓋 872 及 10752 位元組 WHP 封包。這項傳輸支援不准入上述擴充指令。

`WhpXsaveRegisters.def` 使用具名 x87/SSE 控制暫存器補充完整 XSAVE 資料封包。最後操作碼及指令/資料位址會明確寫入並從主機讀回；可補齊封包中的零值欄位，但非零中繼資料衝突或共用控制欄位不一致時，會在發布狀態前失敗。`NamedMetadataRestoresOmittedPacketFields` 驗證欄位缺失情境，並保留完整 FP 資料。

原生 `FOP/FIP/FDP` 遵循主機 x87 儲存、還原規則。沒有未遮罩的待處理例外時，AMD 可能清零這些欄位；快照保留實際觀測值。`X64MachineProbe.def` 與精確 NOP/上下文測試使用一致的待處理例外狀態，確保每個欄位有效並逐項比對，不遮蔽差異。主機行程 FXRSTOR64/FXSAVE64 參考程式涵蓋兩種狀態；後端不會以輸入中繼資料取代主機結果。

共用的 `encodeX64XsaveState` / `decodeX64XsaveState` 編解碼層擁有標準及壓縮 FP/SSE 封包、實體 TOP 輪轉、缺失元件的初始狀態和原子驗證。WHP 使用完整 XSAVE API，優先選擇 `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState`，舊 XSAVE API 作為相容路徑。個別的舊 x87 暫存器介面不能取代完整封包。非初始擴充元件、格式錯誤的標頭、非法控制位元和截斷擷取明確失敗。WHP 映射錯誤保留 HRESULT、GPA 和大小供診斷。

`CheckedX64Instructions.def` 透過既有 CPU 後端准入 8/16/32/64 位元無號 `MUL` 和 `CBW/CWDE/CDQE/CWD/CDQ/CQO`。`NeverDX64IntegerTests` 使用獨立的 `X64IntegerCases.def` 編碼和預期值，在兩種特權級驗證部分暫存器保留、32 位元零擴展、乘積高低兩部分、已定義的 CF/OF 結果及符號擴展不改變旗標。一般 RAM 乘法保留完整存取範圍的權限檢查和讀取觀察回呼；故障或觀察回呼停止會保留隱式輸出暫存器及 PC。裝置運算元仍不支援。這些案例也在 checked Unicorn 上執行；不可用的原生後端明確略過。

`X64BitInstructions.def` 支援 16/32/64 位元暫存器及一般 RAM 的 `BT/BTS/BTR/BTC`。暫存器位元索引依運算元寬度解讀為有號數並選取完整資料字；立即數索引限制於基底位址的資料字內。位址寬度截斷先於 FS/GS 基底位址相加。CF 與寫入值由處理器提供；`RAMTransaction` 在觀察回呼接受前保留私有執行結果。完整範圍權限檢查涵蓋獨立頁面配置與別名。停止、回呼失敗或頁面權限不足均保留原始 CPU 與 RAM。LOCK 僅支援自然對齊的記憶體修改形式；MMIO 與硬體平行 SMP 仍不支援。`X64BitStringTests.cpp` 使用獨立編碼與 x64 本機實際執行對照，檢查負索引、寬度截斷、跨頁存取、取消及非法 LOCK 形式。參見 [Intel 指令參考](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)。

`WhpResourceCache.h` 將邏輯 CPU 狀態與 WHP 分割區分離。執行階段保留一個作用中的原生分割區：同一 CPU 連續單步會重用它；切換 CPU 時先銷毀舊分割區，再重建映射、虛擬處理器並還原完整狀態。邏輯 CPU 保留獨立的 `MemoryProjection` 檢視和權威 RAM。取得租約遵守取消訊號和目前截止時間；銷毀非作用中 CPU 不會銷毀其他 CPU 的分割區。x64 保留主機預設 XSAVE 特性組合，並透過 `WHvGetPartitionProperty` 驗證實際分割區，不透過清除相依特性強制縮減遮罩。CPU 協作式切換不提供平行硬體 SMP。

`CheckedAArch64Instructions.def` 和 `AArch64InstructionEffects` 在 EL0/EL1 接納有界的基礎 FP32/FP64 算術、比較、移動與定寬 SIMD 運算。FPCR 支援四種捨入模式、FZ 和 DN；FPSR 保留累積狀態與 QC。不支援的控制位元及狀態位元在修改前拒絕。FP16 算術、SVE/SME、未遮罩例外、選用擴充及未列出的形式明確失敗。這些 CPU 能力不代表已支援 Windows ARM64 驅動程式載入或新增 OS 環境。

`AArch64InstructionEffects` 負責純量及 FP/SIMD 單次、成對 RAM 存取範圍，單一運算元最大 128 位元。共用位址空間在進入 CPU 前驗證每頁；`RAMTransaction` 僅提交完整宣告的實體寫入。128 位元寫入觀察器在生效前依序收到兩個 64 位元字。停止與故障保留 RAM、向量及位址寫回。Xn/Vn 編號重疊合法；位址回繞的成對存取被拒絕。`NeverDAArch64MemoryTests` 使用獨立的 `AArch64CrossPageCases.def` 與 `AArch64VectorMemoryCases.def` 編碼。

KVM x64/ARM64 透過 `KvmRunControl` 在同一專用 vCPU 工作執行緒準備狀態、進入 `KVM_RUN` 並讀取狀態。`EINTR` 重試僅準備一次；取消進入或讀取失敗不能發布。`KvmAArch64Machine.cpp` 在該執行緒執行位址轉換維護與完整純量、向量傳遞，共用一次單步期限。呼叫執行緒僅在確認完成後提交；ISA 解碼、RAM 交易、OS 策略和觀察器仍屬於呼叫執行緒。ARM64 原生執行仍缺少實機證據。

KVM 根據 `X64HostRegisters.def` 和 `X64FPState.def` 將通用暫存器及完整 FP/SSE 狀態與上次確認完成的偵錯退出狀態比較，只重新安裝變更的輸入。主機寫入和上下文恢復也參與比較；例外、取消及失敗會使重用失效。每條指令仍啟用單步並讀取真實的通用及 FP 狀態。

硬體執行本身不保證更低的端到端耗時。目前原生執行逐條進行指令准入、觀察、狀態傳輸和 VM 退出。比較相同原始映像與情境時，應使用一致的指令和事件預算，同時報告結果一致性與耗時；測量 CLI 延遲時應包含啟動和載入。

checked Unicorn 使用 `MachineRunControl`：ARM64 維護、客體執行與完整狀態回讀共用一次單步額度。`UC_HOOK_CODE` 在指令入口檢查借用的停止權杖和期限；同步引擎呼叫返回前解除 hook 借用，機器單步則保留控制直到發佈狀態。Unicorn 與 WHP 暫存完整 CPU 狀態，並在成功步驟發佈前檢查同一控制條件。WHP 在準備前只建立一次額度。已確認的 x64 CPU 例外優先於回讀期間到來的停止要求。回讀取消時，checked RAM 交易捨棄推測寫入；非受限軟體契約不變。 `MachineInterruptedError` 區分已確認取消與主機或回讀失敗。共用 checked CPU 返回 `Stopped` 或 `Deadline`，保留 CPU/RAM 並允許重試；真實故障即使伴隨停止要求也仍是 `BackendFailure`。

`RunDeadline::invoke` 在 WHP 入口已停止或逾期時拒絕呼叫主機，取消期間保留真實主機結果，並在釋放借用的停止標記前確認中斷回呼結束。KVM 和 WHP 在持有執行租約的呼叫執行緒上驗證完整擷取的私有狀態，然後分類同時到達的停止或逾時。真實主機錯誤、擷取失敗以及經過認證的 x64 CPU 例外保持較高優先順序。一般成功狀態在取消檢查結束前保持私有；已確認的中斷捨棄推測性的 CPU/RAM 效果並允許重試。準備、原生執行和擷取共用一次單步寬限。這些控制提供協作式取消，不保證硬性實際時間上限。
