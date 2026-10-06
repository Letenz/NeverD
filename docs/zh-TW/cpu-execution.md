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

`driver-strict` 接受 x64；`software-cpu-v1` 接受 x64 與 ARM64。`checked-x64-v1`、`checked-aarch64-v1` 要求相符架構並以 supervisor 權限執行。`checked-user-x64-v1`、`checked-user-aarch64-v1` 分別在 CPL3、EL0 執行與契約相符的有限指令集合，並具備 MMU 隔離與明確的服務要求退出。支援 Unicorn 與符合主機條件的 KVM/WHP；`auto` 沿用主機選擇。flat 設定不保證架構層級的 user/supervisor MMU 隔離。checked ARM64 與 x64 設定支援下文列出的有限 FP/SIMD 指令族。supervisor x64 另支援受限 MMIO 交易與預備讀取的字串傳輸；user 設定拒絕裝置對映。連接埠 I/O 仍不支援。只有 user 設定會宣告 `service_traps`。

使用者執行要求**每個**對映頁同時具備 `UserAccessible` 與適當的 `Read`、`Write` 或 `Execute` 權限。既有對映預設供 supervisor 使用；即使別名共用實體位元組，權限仍各自獨立。只有 `UserAccessible` 不會授予存取權。可信任的主機操作與 supervisor CPU 使用 RWX。範例：

```cpp
Configuration.Contract = ExecutionContract::CheckedUserX64;
Configuration.Privilege = ExecutionPrivilege::User;
auto CPU = llvm::cantFail(createExecutionBackend(Configuration, Space)).CPU;
llvm::cantFail(CPU->map(Code, 4096, Read | Write | Execute | UserAccessible));
```

權限由契約固定；還原內容或繫結位址空間不會改變權限，寫入 x64 區段選擇子也不能提升權限。可復原的資料存取錯誤會保留原指令與暫存器，直到所屬模型處理。`canAccess` 精確檢查要求的權限；查詢 user 可見性時要包含 `UserAccessible`。頁表是 CPU 私有投影，不開放可變更的客體頁表或權限切換 API。ARM64 的 user 頁在 EL1 也不可執行。

未知/null 欄位、無效名稱或數字寬度、重複必要功能及不支援的組合都會失敗。輸入上限為 64 KiB；舊 CPU factory 和驅動程式 C 選項維持相容。

<a id="parallel-cpus-and-mmio"></a>

## 平行 CPU 與裝置原子交易

KVM、WHP 與 Unicorn 的 checked x64/ARM64 可要求 `ExecutionFeature::ParallelCPUs`（`parallel_cpus`）。獨立 CPU 可在不同主機執行緒共享實體 RAM；已證明不寫 RAM 的原生指令可重疊執行，待處理寫入阻止新指令進入，等待讀取結束後才發布。指令效果採順序一致性，支援等待取消和回復。所有執行結束前仍禁止映射修改與主機寫入。同一 CPU 物件只有 `stop()` 支援跨執行緒呼叫。預設執行及 OS 排程仍為協作式，弱記憶體模型探索屬於獨立契約。

平行 WHP CPU 在一個共用分割區內使用獨立 VP，最多同時保留 31 個平行 CPU，另加協作式 VP。私有 GPA 區間與傳輸 RAM 隔離各自投影；ARM64 使用不同 ASID 與非全域位址轉換。每次原生執行前複製程式碼及已宣告的運算元，只有已宣告的輸出位元組進入共用 RAM 交易。觀察者讀取權威 RAM。KVM 與 checked Unicorn 直接使用共用後備記憶體。HVF 與 Unicorn 的 `Software` 契約不宣告此能力。

`MMIOAtomics`（`mmio_atomics`）要求裝置明確提供 `GuestMMIOCallbacks::PrepareAtomic`。checked supervisor x64 支援已接納的交換、比較交換、整數更新與修改型位元操作；ARM64 支援包含 `CASP` 的 LSE。運算元須自然對齊，寬度限 1/2/4/8/16 位元組。x64 在私有暫存頁執行原指令，保留精確暫存器與 FLAGS；ARM64 重用統一 LSE 語義。準備階段無裝置副作用，提交驗證生命週期與版本且只生效一次。提交前停止或失敗保留 CPU/裝置狀態，成功提交優先於同時抵達的停止。禁止退化為普通 Read+Write。核心暫存器庫仍使用宣告的 1/2/4 位元組寬度，並拒絕同值寫入、電源變化或擁有者銷毀後的舊預覽。ARM64 普通裝置存取、裝置獨占監視器、使用者態 MMIO 與任意真實硬體不在此能力內。

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

關於遮罩例外的 x64 指令說明描述可攜基線。KVM/WHP 為 `driver-strict`、`checked-x64-v1` 與 `checked-user-x64-v1` 增加 `precise_simd_exceptions`：原生啟動探針驗證精確 `#XM` 及兩種重試後，才允許未遮罩的 MXCSR 寫入、`LDMXCSR` 與 Windows `CONTEXT` 還原。`ExecutionProfiles.def` 統一負責選擇，`supportsSIMDExceptions` 提供已解析的實例能力。checked Unicorn 仍要求遮罩；本次不擴展 ARM64 或 HVF 的例外能力。

原生 x64 KVM/WHP 透過 `X64SIMDException` 將實際 `#XM` 故障交給驅動程式 C SEH。硬體故障的 `CONTEXT` 保存 XMM0–15 與 MXCSR。篩選器、例外展開的 finally 回呼及選定處理器以 MXCSR `0x1f80`、清除 DF 的狀態執行。負篩選器可修改 XMM 與頂層 `CONTEXT.MxCsr`（依客體 CPU 遮罩截斷）後重試原指令；`FltSave.MxCsr` 不控制核心還原。模型 API 引發仍保留整數/控制記錄；x87/AVX 內容修改仍明確拒絕。

checked x64 允許有限的舊式 SSE/SSE2 移動與邏輯指令、`MOVLHPS`/`MOVHLPS`，以及帶遮罩的純量 `CVTTSS2SI`/`CVTTSD2SI`/`SUBSS`/`SUBSD`。MXCSR 保留黏滯狀態、捨入與 FTZ；可攜執行仍拒絕未遮罩例外。KVM/WHP 同步全部 16 個 XMM 暫存器及 MXCSR；未列出的編碼和運算元組合仍會拒絕。

checked x64 亦支援帶遮罩的傳統 `ADD`、`SUB`、`MUL`、`DIV`、`SQRT`、`MIN`、`MAX` 的 `SS`、`SD`、`PS`、`PD` 形式。`X64SSEInstructions.def` 統一定義運算元寬度、對齊與准入規則。`MaskedSSEArithmeticMatchesIndependentHostExecution` 以獨立本機 CPU 參照驗證暫存器與 RAM 形式，涵蓋四種捨入模式、FTZ、有符號零、次正规輸入及 NaN；`SSEMemoryObserverStopsBeforeResultAndStatusChanges` 驗證停止請求先於效果提交。x87、AVX 仍未開放。

在可攜式軟體設定中，Unicorn 會對傳統 `MINSS/MINSD/MINPS/MINPD` 和 `MAXSS/MAXSD/MAXPS/MAXPD` 選中的值套用 DAZ。選中的次正規輸入變為帶正負號的零；NaN 酬載和既有 MXCSR 狀態保持不變。`test_x86_sse_minmax_daz` 檢查暫存器、RAM 與暫存器別名形式，涵蓋 DAZ 開關、全部捨入模式、FTZ 和黏滯狀態。

KVM/WHP 透過私有 `FXSAVE64` 執行探測 `MXCSR_MASK`，並以帶正負號的次正規數算術驗證宣告的 DAZ 能力。checked Unicorn 提供軟體遮罩。`supportedControlBits` 回傳 CPU 的不可變遮罩；FX/XSAVE、快照與 Windows CONTEXT 使用相同能力。checked `LDMXCSR/STMXCSR` 精確存取 m32，檢查完整 RAM 範圍，並在故障或觀察回呼取消時保留狀態。載入保留位觸發 #GP；可攜執行仍拒絕未遮罩 SIMD 例外。`X64MXCSRTests.cpp` 檢查控制值與重試；`X64DAZData` 以原始主機指令對照全部 28 種已准入 SSE 算術形式，涵蓋 DAZ、捨入與 FTZ。此變更不擴展 HVF 的 DAZ 支援。

原生 x64 啟動驗證共用一個 `5 s` 預算，涵蓋傳輸層冷啟動準備及全部探測步驟；客體執行預算獨立。初始化中斷會回報指令階段、`stop_requested` 和 `deadline_reached`，保留中斷類型及底層傳輸診斷，不重試或接受未完成驗證的 CPU。

`PAUSE`（`F3 90`）透過共用 x64 機器介面在 KVM/WHP 和受限 Unicorn 上執行，包括原生 `driver-strict`。`X64PauseTests.cpp` 驗證完整狀態保留、執行前停止、上下文還原、非法 `LOCK` 拒絕，以及自旋迴圈的逾時和恢復。這條處理器提示指令不負責客體執行緒排程，也不保證特定延遲。

`PUSHFQ`（`9C`）與 16 位元 `PUSHF`（`66 9C`）透過原生 KVM/WHP 及 checked Unicorn 執行，包括 `driver-strict`。共用 ISA 層在 RAM 交易提交前從入棧結果移除內部單步 TF。隱式堆疊定址使用完整 RSP，前綴順序決定運算元寬度。完整範圍權限檢查、觀察器停止及故障重試維持原子性。`X64PushFlagsTests.cpp` 涵蓋全部 256 種允許的旗標組合、九種編碼、跨頁別名、使用者權限、中止及上下文還原。

`POPFQ`（`9D`）與 16 位元 `POPF`（`66 9D`）由共用 x64 ISA 層還原旗標，適用於 KVM、WHP、checked Unicorn 及 `driver-strict`。在設定固定 IOPL 為零的條件下，CPL0 可修改 IF；CPL3 保持 IF 和 IOPL。保留位元及 VM/VIF/VIP 被忽略，RF 清零。有效修改客體 TF、NT、AC、ID 或 CPL0 IOPL 仍不支援，會在發布狀態前明確失敗。完整堆疊讀取先於 FLAGS/RSP/RIP 的原子更新；運算元使用完整 RSP，有效前綴順序決定寬度。堆疊可以唯讀或與可執行記憶體互為別名。共用層完成指令，避免清除傳輸層的內部 TF。`X64PopFlagsTests.cpp` 包含獨立的原生 CPL3 指令對照，檢查每個輸入位元、錯誤、觀察器及後續原生執行。

checked x64 接納 `CLC/STC/CMC` 和 `LAHF/SAHF`。進位指令由傳輸層執行；AH 轉換由 KVM、WHP 和 checked Unicorn 共用一處 ISA 實作，包括原生 `driver-strict`。LAHF 將五個狀態旗標及固定位元寫入 AH；SAHF 只修改 CF/PF/AF/ZF/SF。OF/IF/DF 及未選取的暫存器保持不變。被忽略的前綴，包括所有 REX 值，仍使用隱含 AH。`X64StatusFlagsTests.cpp` 檢查主機原始指令、完整狀態、取消及繼續執行。固定版本的 Unicorn 譯碼器也為可攜式設定保留 REX 下的隱含 AH，並在狀態改變前拒絕這五條指令的 LOCK 形式。

checked x64 透過 KVM、WHP 和 Unicorn 執行 `SHLD/SHRD`，支援 16/32/64 位元目標及 imm8 或 CL 計數。計數採用架構遮罩；16 位元形式遮罩後大於 16 的未定義計數在產生效果前拒絕。RAM 目標依精確寬度檢查讀寫權限，並沿用現有交易：結果觀察器在發布前執行，取消會回復 CPU 與記憶體。LOCK 和裝置運算元仍不支援。

`X64ScalarShiftInstructions.def` 統一定義 `SHL/SHR/SAR`、`ROL/ROR` 和 `RCL/RCR` 的 8/16/32/64 位元目的運算元效果。暫存器與 RAM 形式支援隱含的 1、imm8 或 CL 計數，包括遮罩後為零的計數及 SAL 別名。RAM 寫入經過權限檢查、結果觀察回呼與共用交易；停止或回呼錯誤會還原 CPU 與記憶體。KVM、WHP 與 Unicorn 檢查模式共用此邊界。LOCK 與 MMIO 形式仍不支援。

`X64LoopInstructions.def` 透過處理器傳輸執行 `LOOP/LOOPE/LOOPNE`。位址寬度選擇 RCX 或零擴充的 ECX，FLAGS 保持不變。目標寬度遵循 CPU 模型：Intel 在長模式忽略 `66H`，AMD 保留其 16 位元覆寫，REX.W 優先。原生 KVM/WHP 使用主機處理器，Unicorn 使用預設 Intel Haswell 模型。共用准入規則在產生效果前拒絕 LOCK 和 REP，包括解碼中繼資料省略的前綴。

`X64BranchModel` 讓相對 `JMP/Jcc` 的解碼與執行一致。原生 KVM/WHP 在有時間上限的初始化中，透過不跳躍的 `66H` 分支和 REX.W 優先序探針確定規則；Unicorn 檢查模式使用其 Intel 模型。不可變結果同時決定指令觀察器和 Windows 驅動策略的解碼模式。Intel 在長模式下保留 rel32 近分支和完整目標寬度；AMD 遵守其 16 位元覆寫規則。指令位元組不完整時，在觀察器或 CPU 執行前失敗。

`X64StackInstructions.def` 在 KVM、WHP 和 checked Unicorn（含 `driver-strict`）中支援通用暫存器、一般 RAM 的 16/64 位元 `PUSH/POP` 及立即值 PUSH。PUSH 先讀取來源，再減少 RSP；POP 先增加 RSP，再計算使用 RSP/ESP 的目的位址。位址寬度截斷只適用於顯式運算元。完整範圍權限檢查、有序觀察回呼和單次 RAM 交易確保故障、取消或回呼錯誤時不會部分更新 CPU 與記憶體。LOCK 和裝置運算元仍不支援。

KVM、WHP 與 checked Unicorn 也支援 16/64 位元 `LEAVE`。它始終透過完整 RBP 讀取儲存的堆疊框架，包括帶 `67H` 的情況；16 位元形式保留 RBP 未選取的位元。發生錯誤或取消讀取時保留原始 RSP 與 CPU 上下文。LOCK、REP 與裝置堆疊框架仍不支援。

KVM、WHP 與 checked Unicorn 支援 16/64 位元 `ENTER`，配置量按無號 16 位元解讀，巢狀層級取模 32。存取使用完整 RSP/RBP，並遵循有效前綴順序。客體故障會保留已完成的堆疊寫入，RSP、RBP 與 PC 則維持入口值。最終堆疊檢查涵蓋整個運算元寬度的寫入權限，但不寫入資料。LOCK、REP、APX 前綴及裝置堆疊框架仍不支援。

`X64PackedIntegerInstructions.def` 允許 45 條 legacy SSE2 packed integer 指令，涵蓋回繞／飽和加減、比較、乘法、平均值、極值、位元組差、打包及解包。XMM 和對齊的 128 位元 RAM 來源運算元在 KVM、WHP、Unicorn 上共用現有 checked 路徑。FLAGS 與 MXCSR 保持不變；故障或觀察器取消保留狀態。MMX、VEX/EVEX 和裝置運算元仍不支援。

`X64PackedShiftInstructions.def` 准入十種 legacy SSE2 打包移位。元素移位接受 imm8 或 XMM／對齊的 m128 計數，位元組移位僅接受 imm8。變數計數使用無符號低 64 位元，不按純量移位規則遮罩；高 64 位元不參與計算。即使計數為零或超出位寬，記憶體運算元仍須完整讀取 16 位元組。FLAGS 和 MXCSR 保持不變；MMX、VEX/EVEX 和裝置運算元仍被排除。

`X64VectorOperands.def` 統一定義傳統 SSE 搬移、運算、移位、轉換與遮罩的完整運算元規則。`MOVMSKPS`、`MOVMSKPD` 與 `PMOVMSKB` 從 XMM 擷取符號位元寫入 r32/r64，並清零目的暫存器其餘位元。KVM、WHP 與 checked Unicorn 共用准入規則，保留 FLAGS、MXCSR 與來源暫存器；遮罩的記憶體運算元、MMX 與 VEX/EVEX 形式仍不支援。

`X64ShuffleInstructions.def` 新增 `PSHUFD`、`PSHUFHW`、`PSHUFLW`、`SHUFPS` 與 `SHUFPD`。`X64VectorOperands.def` 要求完整的三個運算元：XMM 目的、XMM 或對齊的 m128 來源，以及 imm8。原始指令依位元選取通道，保留 FLAGS 與 MXCSR；記憶體形式檢查全部 16 位元組，對齊錯誤先於資料觀察。KVM、WHP 與 checked Unicorn 共用這些規則。 同一清單也允許恰有兩個運算元的 `UNPCKLPS`、`UNPCKHPS`、`UNPCKLPD` 與 `UNPCKHPD`，交錯原始目的與來源的位元模式。硬體可以只讀取所選的 64 位元；checked RAM 檢查對齊的 m128 運算元。

`MOVLPS`、`MOVHPS`、`MOVLPD` 和 `MOVHPD` 精確傳輸八位元組 RAM，無對齊要求。`X64VectorInstructions.def` 宣告儲存使用的半部；`X64VectorOperands.def` 限定 XMM/m64 運算元對。載入保留另一個 64 位元半部，高半部儲存觀察者接收高半部資料。KVM、WHP 和 checked Unicorn 共用全範圍權限檢查與 RAM 回復。僅暫存器形式的 `MOVHLPS`/`MOVLHPS` 保留各自語義。

`CVTSI2SS` 與 `CVTSI2SD` 依 MXCSR 捨入規則轉換帶正負號的 32/64 位元整數，並保留精度狀態。共用的 `IntegerSource` 規則僅允許 XMM 目的及 r32/r64 或 m32/m64 來源。傳統指令保留目的的高 96/64 位元；記憶體檢查採用整數寬度。KVM、WHP 和 checked Unicorn 執行原始指令。MMX 和 VEX/EVEX 仍不支援。

`CVTSS2SI` 與 `CVTSD2SI` 透過共用 `IntegerResult` 規則，依 MXCSR 捨入模式產生帶正負號的 32/64 位元整數；`CVTTSS2SI` 與 `CVTTSD2SI` 一律向零截斷。遮罩例外時，NaN 或超出範圍的轉換傳回整數不定值並設定無效狀態；有效但不精確的結果設定精度狀態。既有黏滯位元、FLAGS 和 XMM 來源維持不變。r32 結果清除通用暫存器高半部。RAM 讀取採用浮點來源寬度，與目的寬度無關；FTZ 不會捨棄次正規輸入。KVM、WHP 和 checked Unicorn 共用這些規則。

`COMISS`、`COMISD`、`UCOMISS` 和 `UCOMISD` 透過共用 `Source` 規則比較純量 XMM 或 m32/m64 運算元。它們設定 CF/PF/ZF、清除 OF/SF/AF，保留其他 FLAGS 和來源通道。COMIS 對任意 NaN 設定無效狀態，UCOMIS 僅對 signaling NaN 設定該狀態；NaN 處理優先於次正規狀態。MXCSR 黏滯位元保留，捨入和 FTZ 不影響比較。KVM、WHP 和 checked Unicorn 共用精確記憶體檢查。固定版本的 Unicorn 比較函式重用既有次正規輸入分類邏輯。

`CMPSS`、`CMPSD`、`CMPPS` 和 `CMPPD` 在 KVM、WHP 和 checked Unicorn 上執行八種傳統比較條件。共用 `Source` 規則接納解碼後的條件別名，保留控制值仍不支援。純量形式保留高位通道並讀取 m32/m64，向量形式要求對齊的 m128。FLAGS 與既有 MXCSR 狀態保留，無效及次正規狀態依有效通道累積。Capstone 統一負責指令族 ID 與 SSE 條件，取代僅在 lifter 內修正身分的邏輯；Unicorn 在各比較函式內部分類次正規輸入。

`CVTSS2SD`、`CVTSD2SS`、`CVTPS2PD` 和 `CVTPD2PS` 透過共用 `Source` 規則轉換傳統 SSE 精度。純量結果保留目標高 64/96 位元；打包擴寬讀取 m64 並寫入兩個雙精度數，打包縮窄讀取對齊的 m128、寫入兩個單精度數並清零高 64 位元。KVM、WHP 和 checked Unicorn 執行原始指令，保留 FLAGS，並依捨入與 FTZ 控制累積已遮罩例外的 MXCSR 狀態。Unicorn 在轉換函式中逐一分類有效次正規輸入。未遮罩例外和 VEX/EVEX 仍不支援。

`CVTDQ2PS` 和 `CVTDQ2PD` 透過共用 `Source` 規則轉換打包的有號 32 位元整數。單精度讀取對齊的 m128 並使用 MXCSR 捨入；雙精度讀取可未對齊的 m64，結果精確。目標 XMM 全部位元被替換，FLAGS 和既有 MXCSR 狀態保留，非精確單精度結果累積精度狀態。KVM、WHP 和 checked Unicorn 執行原始指令。Unicorn 在選擇八位元組讀取前辨識兩種打包擴寬轉換。MMX 和 VEX/EVEX 仍不支援。

`CVTPS2DQ` 和 `CVTPD2DQ` 使用 MXCSR 捨入；`CVTTPS2DQ` 和 `CVTTPD2DQ` 向零截斷。共用 `Source` 規則要求對齊的 m128 或 XMM 輸入。NaN 或越界通道產生 signed32 indefinite 並設定無效狀態；其他有效但非精確通道獨立累積精度狀態。單精度輸入產生四個整數，雙精度輸入產生兩個並清零目標高 64 位元。FLAGS 和既有 MXCSR 狀態保留；FTZ 不捨棄次正規輸入。KVM、WHP 和 checked Unicorn 執行原始指令。MMX 和 VEX/EVEX 仍不支援。

`X64AlignmentTests.cpp` 驗證已准入 aligned SSE 指令的未對齊運算元在資料觀察器、權限檢查或裝置回呼之前回報可恢復或終止性的 `#GP(0)`。故障保留完整公開 x64 暫存器內容、PC 與 RAM；位址寬度回繞先於 FS/GS 基底相加，修復位址後重試原指令。直接 KVM/WHP 機器測試獨立驗證硬體邊界。Windows ring3 已派送明確分類的 `operand_alignment` 故障；其他原因的 `#GP` 仍不支援。

thread pointer 包含 x64 FS/GS 基底及 ARM64 `TPIDR_EL0` 的精確 `MRS`/`MSR` 編碼。原生傳輸與 CPU snapshot 會獨立於記憶體保存狀態，但不會建立 OS 執行緒或配置 TLS 區塊。supervisor x64 支援對齊的 1/2/4 位元組純量 MMIO 交易，以及每個重新啟動邊界一個 MOVS 元素。裝置讀取必須先提供無副作用預覽，再至多提交一次。user 設定拒絕裝置對映；RMW、寬 MMIO、連接埠 I/O 也仍不支援。

KVM/WHP 會取消進行中的原生入口，並在釋放執行資源前確認取消。KVM 使用專用執行緒與暫時解除封鎖的 realtime signal；入口期間選定的 signal 不得被忽略。呼叫端的 signal mask/handler 不會變更。若客體進度不明，取消會成為終止性後端錯誤；不保證硬性 wall-clock 期限。

## x64 原生同步例外

checked x64 的 `DIV`/`IDIV` 使用處理器結果與 `#DE`。KVM 透過私有 supervisor IDT/IST 接收例外，WHP 使用明確的例外攔截位圖；原始上下文與可用錯誤碼和傳輸錯誤分開保留。OS 模型先消費可恢復事件，再安裝明確的繼續執行上下文。Windows 驅動將零除與商溢位映射為 `STATUS_INTEGER_DIVIDE_BY_ZERO`，執行實際 SEH filter、`__finally` 與重試。`NeverDX64ExceptionTests` 可停用 Unicorn 建置，`DriverWDMCPUException` 驗證原始 WDK 用例；缺少 ARM64 主機時明確跳過。

## 分階段提交 RAM 效果

`RAMTransaction` 在物理執行租約內，只保存一條指令明確宣告之寫入範圍的物理聯集。結果觀察器執行前恢復原始 RAM；取消、後端傳輸錯誤和觀察器例外不會發布部分 RAM 或暫存器。CPU 例外在 RAM 回復後保留架構例外狀態。ARM64 的單次與成對寫入共用此記憶體權威層。x64 支援 8/16/32/64 位元 `XCHG`、`XADD`、`CMPXCHG`，LOCK 或隱式鎖定形式要求自然對齊。`NeverDRAMTransactionTests` 將結果與獨立宿主 CPU 對照，並驗證回復、別名和權限；不可用的平台明確跳過。裝置原子交易使用獨立提供者；CPU 快照不會撤銷已提交的 RAM。

`CMPXCHG8B` 與 `CMPXCHG16B` 在 KVM、WHP 和 checked Unicorn 的驅動及使用者模式中執行原始指令。比較成功或失敗都需要讀寫權限，故障按寫入存取分類。`CMPXCHG16B` 在存取記憶體前檢查 16 位元組對齊，不滿足時回報 `#GP(0)`。兩次結果觀察屬於同一 RAM 交易；任一次停止或擲出例外，都不會發布暫存器或記憶體變更。未加鎖的 `CMPXCHG8B` 可以跨頁，加鎖操作仍要求自然對齊。`X64WideAtomicTests.cpp` 對照主機原始執行結果和直接原生故障，並檢查別名、前綴、定址、修復重試及取消。原創 Windows 驅動及 ring3 PE 範例涵蓋兩種寬度，WDK 範例也執行 `_InterlockedCompareExchange128`。CPU 模型必須支援 `CMPXCHG16B`。

## 完整 x87 狀態

`NeverDEmulationArch` 獨立負責 ISA、頁表及 FP 狀態佈局，原生與 Unicorn 傳輸共用此層。x64 上下文保存 x87 控制、狀態、TOP、實體標籤、操作碼、指令／資料指標及八個 80 位元暫存器。`FP0`–`FP7` 使用 `RegisterValue`，純量存取拒絕截斷；`FPTag` 是實體非空位圖。`NeverDX64FPTests` 涵蓋全部 TOP、精確運算的主機 FXSAVE/FXRSTOR 對照與上下文還原。這不新增 checked x87 指令，也不證明全部捨入語義；缺少原生主機時明確略過。

`driver-strict` 支援匹配 Linux x64 主機的 KVM 與 Windows x64 主機的 WHP；`auto` 選取對應原生傳輸，跨 ISA 執行選取 Unicorn。明確指定 Unicorn 及原有 V1 API 保留可移植軟體設定。原生執行在進入 CPU 前檢查規範位址和指令效果；硬體不可用時明確失敗且不回退。不支援的指令與 OS 行為仍明確報錯。Windows x64 原生 CI 在停用 Unicorn 的設定下通過全部 359 項必測檢查：131 項 CPU 檢查、26 個內建映像與 46 個 WDK 映像及 40 個情境組合在首選和重定位位址產生的 224 項驅動程式結果，以及 4 項 SEH 邊界檢查 ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). 原生 ARM64 實機證據仍待補充，這不表示相容任意驅動程式或 Android/Darwin 環境。

使用 `executionCapabilities(Contract, ISA, Backend)` 查詢所選後端的能力設定。`NativeLegacyX64` 描述原生 x64 驅動程式執行；`NeverDNativeDriverTests` 驗證原有驅動程式集，也可在停用 Unicorn 的組建中執行。

Checked ARM64 使用統一的完整狀態提交邊界。`Registers.def` 定義 39 個純量欄位及 32 個 128 位元向量暫存器；`captureAArch64State` 暫存所有讀取、套用宣告位寬與 NZCV 正規化，最後一次提交。Unicorn、KVM、WHP 和 HVF 傳遞相同清單，包括 TPIDR_EL0、TPIDRRO_EL0、TPIDR_EL1、FPCR 和 FPSR。原生介面透過 CPACR_EL1 啟用 FP/SIMD。任何純量或向量讀取失敗、進入取消，皆保留完整呼叫方狀態。

ARM64 KVM/WHP/HVF 初始化執行私有 `AArch64MachineProbe.def` 程式：NOP、向正無窮捨入的 FP32 加法及雙通道 SIMD 加法。每步比較全部 39 個純量欄位與 32 個向量，包括 TLS、NZCV、目的暫存器高位清零及保留和累積的 FPCR/FPSR 狀態。自檢只使用特權級監控儲存，共享一個總截止時間。自檢僅證明有界初始化。Linux ARM64 KVM 與 Windows ARM64 WHP 的工作負載驗證仍待完成；macOS 原生結果記錄於 [HVF 指南](macos-hvf.md)。 程式亦包含金鑰停用時的 A/B 返回位址簽署與驗證，以及非防護頁面的四種 BTI 指令。

自檢還執行兩次 `MRS CTR_EL0`，以及 `DC CVAU`、`DSB ISH`、`IC IVAU` 和 `ISB`，核對快取幾何資訊穩定及完整狀態。Checked EL0/EL1 接納原始指令、所有具名基線 DSB 選項及 ISB SY。CTR 來自選定虛擬 CPU，不同傳輸可以不同。快取目標必須在目前權限下指向可讀普通 RAM，允許非對齊位址和別名；其他目標明確報未支援。維護操作不產生資料讀寫觀察事件。投影保證指令執行的一致性，不模擬私有快取內容或平行硬體 SMP。`NeverDAArch64CacheTests` 檢查完整狀態、唯讀頁尾、拒絕項、停止、上下文、預算，以及透過跨頁 RW/RX 別名更新 guest 程式碼；不可用的 KVM/WHP 主機明確跳過。

x64 KVM/WHP/HVF 原生初始化在私有 supervisor 頁面執行 `X64MachineProbe.def`。單一時限涵蓋 NOP、朝正無窮捨入的 FP32 加法、雙通道 SIMD 加法、FS/GS 載入及 CS/SS/CR8 讀取；每一步比較完整的純量、XMM、實體 x87 和控制狀態。x64 與 ARM64 自檢都必須取得實體記憶體的獨占執行租約。`MemoryProjection` 統一保存快取身分（ISA、位址空間、映射世代、權限及監控變體）和各 ISA 已提交的頁表根歷史。建構器在改寫私有位元組前使快取失效；失敗的重建不能重用部分寫入的頁表，呼叫者也不能傳入過期頁表根。自檢僅證明有界初始化。Linux ARM64 KVM 與 Windows ARM64 WHP 的工作負載驗證仍待完成；macOS 原生結果記錄於 [HVF 指南](macos-hvf.md)。

共用 XSAVE 解碼器區分標準格式與壓縮格式的 SSE 初始狀態。XSTATE_BV[1] 清零時，兩種格式都初始化 XMM 暫存器；標準格式仍讀取並驗證 MXCSR，壓縮格式才初始化 MXCSR。`X64XsaveCases.def` 提供獨立的資料配置和原創主機 XRSTOR 程式。`X64XsaveTests.cpp` 檢查拒絕狀態的原子性，並以真實主機執行對照兩種格式，同時保留呼叫端 FP/SSE 狀態。主機架構或所需指令功能不可用時，對照測試明確略過。

`X64FPState.def` 宣告壓縮 AVX、AVX-512、CET_U/CET_S 和 AMX 傳輸配置，包括元件的 64 位元組對齊。存在的擴充資料必須符合架構的全零初始狀態；缺席元件資料與對齊填補不定義狀態。偏移由配置位元決定，未知配置、非初始資料或錯誤長度會在發布前失敗。`CompactedOffsetsFollowLayoutRatherThanPresentBits`、`WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`、`InitialCETComponentsDoNotHideFPState` 和 `InitialWideComponentsDoNotHideFPState` 涵蓋 872 及 10752 位元組 WHP 封包。這項傳輸支援不准入上述擴充指令。

`WhpXsaveRegisters.def` 使用具名 x87/SSE 控制暫存器補充完整 XSAVE 資料封包。最後操作碼及指令/資料位址會明確寫入並從主機讀回；可補齊封包中的零值欄位，但非零中繼資料衝突或共用控制欄位不一致時，會在發布狀態前失敗。`NamedMetadataRestoresOmittedPacketFields` 驗證欄位缺失情境，並保留完整 FP 資料。

原生 `FOP/FIP/FDP` 遵循主機 x87 儲存、還原規則。沒有未遮罩的待處理例外時，AMD 可能清零這些欄位；快照保留實際觀測值。`X64MachineProbe.def` 與精確 NOP/上下文測試使用一致的待處理例外狀態，確保每個欄位有效並逐項比對，不遮蔽差異。主機行程 FXRSTOR64/FXSAVE64 參考程式涵蓋兩種狀態；後端不會以輸入中繼資料取代主機結果。

共用的 `encodeX64XsaveState` / `decodeX64XsaveState` 編解碼層擁有標準及壓縮 FP/SSE 封包、實體 TOP 輪轉、缺失元件的初始狀態和原子驗證。WHP 使用完整 XSAVE API，優先選擇 `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState`，舊 XSAVE API 作為相容路徑。個別的舊 x87 暫存器介面不能取代完整封包。非初始擴充元件、格式錯誤的標頭、非法控制位元和截斷擷取明確失敗。WHP 映射錯誤保留 HRESULT、GPA 和大小供診斷。

`CheckedX64Instructions.def` 透過既有 CPU 後端准入 8/16/32/64 位元無號 `MUL` 和 `CBW/CWDE/CDQE/CWD/CDQ/CQO`。`NeverDX64IntegerTests` 使用獨立的 `X64IntegerCases.def` 編碼和預期值，在兩種特權級驗證部分暫存器保留、32 位元零擴展、乘積高低兩部分、已定義的 CF/OF 結果及符號擴展不改變旗標。一般 RAM 乘法保留完整存取範圍的權限檢查和讀取觀察回呼；故障或觀察回呼停止會保留隱式輸出暫存器及 PC。裝置運算元仍不支援。這些案例也在 checked Unicorn 上執行；不可用的原生後端明確略過。

`X64BitInstructions.def` 支援 16/32/64 位元暫存器及一般 RAM 的 `BT/BTS/BTR/BTC`。暫存器位元索引依運算元寬度解讀為有號數並選取完整資料字；立即數索引限制於基底位址的資料字內。位址寬度截斷先於 FS/GS 基底位址相加。CF 與寫入值由處理器提供；`RAMTransaction` 在觀察回呼接受前保留私有執行結果。完整範圍權限檢查涵蓋獨立頁面配置與別名。停止、回呼失敗或頁面權限不足均保留原始 CPU 與 RAM。LOCK 要求自然對齊；修改型 MMIO 操作要求明確的預備原子交易提供者。`X64BitStringTests.cpp` 使用獨立編碼與 x64 本機實際執行對照，檢查負索引、寬度截斷、跨頁存取、取消及非法 LOCK 形式。參見 [Intel 指令參考](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)。

`X64StringInstructions.def` 統一管理一般 RAM 上 8/16/32/64 位元的 `MOVS/STOS/LODS`；`CLD/STD` 只改變方向旗標。每個 REP 元素在觀察回呼前驗證整個運算元，並於一個可恢復邊界提交。後續錯誤保留先前完成的元素；取消或回呼例外不改變目前元素。FS/GS 僅作用於來源位址，且在位址寬度截斷之後相加。AL/AX 載入保留高位元，EAX 載入零擴展。32 位元位址模式的零次 REP 要求計數高位元為零，MOVS/STOS 還要求參與的位址暫存器高位元為零，否則不同真實 CPU 實作會產生不同結果。MOVS/STOS/LODS 的 REPNE 形式與 STOS/LODS 裝置運算元仍不支援。`X64StringTransferTests.cpp` 用獨立的主機指令對照寬度、方向、重疊和零次數，並分別檢查權限、別名、回繞、錯誤與恢復。原創 WDK 資源驅動程式透過 `driver_resource_strings.def` 執行四種寬度的 STOS/LODS。

`X64StringInstructions.def` 也統一管理一般 RAM 上 8/16/32/64 位元的 `CMPS/SCAS` 與 `REPE/REPNE`。每個元素在觀察回呼前驗證全部讀取運算元，更新六個算術旗標，並於首次符合終止條件時退出。資料錯誤會恢復本次連續 REP 執行開始時的旗標，同時保留已完成的指標與計數更新；公開介面恢復執行時，以已發布的 CPU 狀態重新開始。停止與觀察回呼例外不改變目前元素，提前終止也不會讀取下一個元素。FS/GS 僅影響 CMPS 來源位址；SCAS 保留累加器與未使用的來源暫存器。裝置運算元及有歧義的 32 位元零次數高位元狀態仍不支援。`X64StringComparisonTests.cpp` 以獨立主機指令對照旗標、方向、別名、回繞、權限與恢復，並透過 Linux x64 訊號測試讀取實際錯誤時的暫存器。原創 WDK 資源驅動程式透過 `driver_resource_strings.def` 執行四種寬度的兩類條件重複形式。參見 [Intel 指令參考](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)。 Linux 原生驗證涵蓋首元素執行前後發生的故障，並區分 Intel 還原入口 flags 與 Hyper-V 下 AMD EPYC 7763 保留最後一次比較 flags 的行為（[原生觀測](https://github.com/NeverSight/NeverD/actions/runs/37202522130)）；未知 CPU 廠商會明確失敗。所有後端的 checked 來賓仍統一還原入口 flags。

存活的 WHP CPU 共用一個原生分割區；最終關閉與重新建立由同一登錄表鎖序列化。`WhpResourceCache.h` 重用協作式 VP 0；切換邏輯 CPU 前先銷毀該 VP 並撤銷其映射。平行 CPU 保留獨立 VP 與私有 GPA 區間。暫存器、XSAVE 與取消請求均指向各自的 VP。x64 保留主機預設 XSAVE 功能集，並透過 `WHvGetPartitionProperty` 驗證實際分割區設定。預設排程仍為協作式。

`CheckedBackend` 為每個 CPU 保留一塊取指緩衝區與一筆 `cs_disasm_iter` 指令記錄。每一步重新讀取具執行權限的位元組並解碼；程式碼寫入、別名變更或恢復執行後不會沿用舊解碼結果。執行租約在接觸重用儲存前拒絕遞迴執行。這可消除逐指令的緩衝區與記錄配置，同時保留指令觀察、系統服務攔截及精確故障處理。 固定版本的 Unicorn 單步路徑在讀取後繼指令前結束，包含間接翻譯查找路徑，且不會將內部程式碼寫入重試計為已完成指令。

`WhpX64Processor.h` 將 x64 WHP 暫存器重用狀態歸屬到各個虛擬處理器。固定資料包採用 `WhpX64Registers.def` 與 `X64HostRegisters.def`；每個成功單步仍完整讀取通用、控制、段暫存器及 FP/SSE 狀態。只有完整確認的除錯退出才能省略未變更的輸入，比較忽略保留位與聯合體填補。變更的 CR3、CPL、TLS、通用或 FP 輸入會重新安裝；部分失敗、取消及例外使重用失效。VP 重建從全量安裝開始。這減少重複傳輸，不擴大指令准入，也不宣稱端到端加速。

WHP 將 `WhpXsaveRegisters.def` 中的 x87/SSE 中繼資料與一般暫存器放入同一次 `WHvGetVirtualProcessorRegisters` 呼叫。停止的 vCPU 始終由同一個分區租約保護。發布前仍須完整擷取 XSAVE 並核對全部中繼資料一致性；每步減少一次主機 API 呼叫，不據此宣稱吞吐量提升。

`CheckedAArch64Instructions.def` 和 `AArch64InstructionEffects` 在 EL0/EL1 接納有界的基礎 FP32/FP64 算術、比較、移動與定寬 SIMD 運算。FPCR 支援四種捨入模式、FZ 和 DN；FPSR 保留累積狀態與 QC。不支援的控制位元及狀態位元在修改前拒絕。FP16 算術、SVE/SME、未遮罩例外、選用擴充及未列出的形式明確失敗。這些 CPU 能力不代表已支援 Windows ARM64 驅動程式載入或新增 OS 環境。

`AArch64InstructionEffects` 負責純量及 FP/SIMD 單次、成對 RAM 存取範圍，單一運算元最大 128 位元。共用位址空間在進入 CPU 前驗證每頁；`RAMTransaction` 僅提交完整宣告的實體寫入。128 位元寫入觀察器在生效前依序收到兩個 64 位元字。停止與故障保留 RAM、向量及位址寫回。Xn/Vn 編號重疊合法；位址回繞的成對存取被拒絕。`NeverDAArch64MemoryTests` 使用獨立的 `AArch64CrossPageCases.def` 與 `AArch64VectorMemoryCases.def` 編碼。

KVM x64/ARM64 透過 `KvmRunControl` 在同一專用 vCPU 工作執行緒準備狀態、進入 `KVM_RUN` 並讀取狀態。`EINTR` 重試僅準備一次；取消進入或讀取失敗不能發布。`KvmAArch64Machine.cpp` 在該執行緒執行位址轉換維護與完整純量、向量傳遞，共用一次單步期限。呼叫執行緒僅在確認完成後提交；ISA 解碼、RAM 交易、OS 策略和觀察器仍屬於呼叫執行緒。ARM64 原生執行仍缺少實機證據。

KVM 根據 `X64HostRegisters.def` 和 `X64FPState.def` 將通用暫存器及完整 FP/SSE 狀態與上次確認完成的偵錯退出狀態比較，只重新安裝變更的輸入。主機寫入和上下文恢復也參與比較；例外、取消及失敗會使重用失效。每條指令仍啟用單步並讀取真實的通用及 FP 狀態。

x64 KVM 查詢 `KVM_CAP_SYNC_REGS`，分別啟用主機支援的 `KVM_SYNC_X86_REGS` 與 `KVM_SYNC_X86_SREGS` 擷取集合。已確認完成的 `KVM_RUN` 透過共享區回傳實際暫存器；連續成功單步可省去 `KVM_GET_REGS` 與 `KVM_GET_SREGS`。未支援的集合或選用查詢失敗保留 ioctl 路徑。變更的輸入仍在 `KVM_SET_GUEST_DEBUG` 前安裝，共享髒位保持清零。例外、取消及擷取失敗使重用失效。完整 FP/SSE 擷取仍為必要，未變更的 FP 輸入免去重複 XSAVE 編碼。這減少傳輸呼叫，但不代表已證明端到端加速。[KVM API](https://docs.kernel.org/virt/kvm/api.html#kvm-cap-sync-regs)。

硬體執行本身不保證更低的端到端耗時。目前原生執行逐條進行指令准入、觀察、狀態傳輸和 VM 退出。比較相同原始映像與情境時，應使用一致的指令和事件預算，同時報告結果一致性與耗時；測量 CLI 延遲時應包含啟動和載入。

checked Unicorn 使用 `MachineRunControl`：ARM64 維護、客體執行與完整狀態回讀共用一次單步額度。`UC_HOOK_CODE` 在指令入口檢查借用的停止權杖和期限；同步引擎呼叫返回前解除 hook 借用，機器單步則保留控制直到發佈狀態。Unicorn 與 WHP 暫存完整 CPU 狀態，並在成功步驟發佈前檢查同一控制條件。WHP 在準備前只建立一次額度。已確認的 x64 CPU 例外優先於回讀期間到來的停止要求。回讀取消時，checked RAM 交易捨棄推測寫入；非受限軟體契約不變。 `MachineInterruptedError` 區分已確認取消與主機或回讀失敗。共用 checked CPU 返回 `Stopped` 或 `Deadline`，保留 CPU/RAM 並允許重試；真實故障即使伴隨停止要求也仍是 `BackendFailure`。

`RunDeadline::invoke` 在 WHP 入口已停止或逾期時拒絕呼叫主機，取消期間保留真實主機結果，並在釋放借用的停止標記前確認中斷回呼結束。KVM 和 WHP 在持有執行租約的呼叫執行緒上驗證完整擷取的私有狀態，然後分類同時到達的停止或逾時。真實主機錯誤、擷取失敗以及經過認證的 x64 CPU 例外保持較高優先順序。一般成功狀態在取消檢查結束前保持私有；已確認的中斷捨棄推測性的 CPU/RAM 效果並允許重試。準備、原生執行和擷取共用一次單步寬限。這些控制提供協作式取消，不保證硬性實際時間上限。

## macOS HVF

`hvf` · Hypervisor.framework · Apple Silicon → ARM64 · Intel Mac → x86-64.

[Setup, signing and native hardware validation (English)](../macos-hvf.md)

checked ARM64 支援 8/16/32/64 位元 `LDXR/STXR`、32/64 位元暫存器對 `LDXP/STXP`、對應的 acquire/release 形式及 `CLREX`。KVM、WHP 與 checked Unicorn 共用 ISA 層的獨佔監視器，以 16 位元組實體範圍記錄保留狀態，傳輸層單步退出不會破壞迴圈進展。已提交的寫入即使未改變位元組，也會使保留狀態失效；別名及保留檢視的寫入遵循相同規則。停止保留尚未發布的狀態，快照無法撤銷期間的寫入。checked 獨佔指令採用 FEAT_LSE2 對齊規則：運算元可在同一個 16 位元組對齊區塊內未對齊存取，跨越該區塊才產生 `alignment` 錯誤。同寬條件儲存依保留的實體粒度比對；即使監視器已失效，也先檢查對齊及權限，再決定條件儲存是否成功。獨占指令仍只支援普通 RAM。 ARM64 Unicorn 的 `Software` 契約也使用此監視器，包括軟體與 checked CPU 共用實體 RAM 的情況。 Unicorn 的 `Software` 設定保留引擎的自然對齊模型。


同一 ISA 層支援 FEAT_LSE `CAS/CASP`、`SWP`、`LDADD/LDCLR/LDEOR/LDSET` 與有符號/無符號 min/max，包含位元組、半字、字、雙字及 acquire/release 形式。checked 設定採用上述對齊策略，Unicorn `Software` 保留自然對齊。比較依運算元寬度執行，傳回的舊值零擴展。CAS 比較失敗仍要求寫入權限，並選擇 Arm 允許的舊值回寫，使實體保留狀態失效。回呼看到提交前的 CPU/RAM；取消與同步故障不發布部分原子結果。平行 CPU 與 MMIO 原子交易遵循並行提交契約。`MRS/MSR NZCV` 依架構規則傳輸四個條件旗標，正確處理保留位與零暫存器。 先檢查讀取權限，再檢查寫入權限：不可讀的運算元回報讀取故障，唯讀運算元回報寫入故障，與原始 Windows ARM64 觀測一致。

CPU0 顯式搶占、虛擬時鐘語義與目前邊界見[驅動程式排程](driver-scheduling.md)。
