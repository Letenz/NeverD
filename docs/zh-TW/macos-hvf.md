**語言**: [English](../macos-hvf.md) | [简体中文](../zh-CN/macos-hvf.md) | [繁體中文](macos-hvf.md) | [日本語](../ja/macos-hvf.md) | [한국어](../ko/macos-hvf.md) | [Français](../fr/macos-hvf.md) | [Deutsch](../de/macos-hvf.md) | [Español](../es/macos-hvf.md) | [Italiano](../it/macos-hvf.md) | [Русский](../ru/macos-hvf.md) | [العربية](../ar/macos-hvf.md)

<!-- i18n-source: 8f26e60b2d8386a9fac2779c8ee4c580c21df3c09d7a2eabe46de97a72474384 -->

[← 文件索引](README.md)

# macOS 原生 CPU 執行（HVF）

NeverD 使用 Apple 的 [Hypervisor.framework](https://developer.apple.com/documentation/hypervisor)，作為 macOS 上對應 KVM、WHP 的後端。`--backend hvf` 明確選用 HVF；`auto` 在契約允許原生執行且宿主、客體 ISA 相符時選用它：Apple Silicon 執行 ARM64，Intel Mac 執行 x86-64。`software-cpu-v1` 與跨 ISA 的自動選擇仍使用 Unicorn。經 Rosetta 轉譯的執行檔會被拒絕。

傳輸層需要 macOS 11 以上及硬體虛擬化；其他建置相依項目可能要求更高版本。明確選定的原生後端不可用時會回報原因，不會悄悄改用其他後端。[Virtualization.framework](https://developer.apple.com/documentation/virtualization) 是完整虛擬機介面；NeverD 需要 Hypervisor.framework 的 vCPU、暫存器、記憶體映射與例外控制。

## 建置與簽章

啟用 `NEVERD_ENABLE_CPU_EMULATION=ON` 或驅動程式模擬。`NEVERD_EMULATION_BACKEND_HVF` 預設為 `ON`，僅在 macOS 連結框架；設為 `OFF` 後仍能解析 `hvf` 名稱，但能力 API 回報 `build_disabled`。

框架連結及 hypervisor 簽署僅適用於 macOS 建置目標（`CMAKE_SYSTEM_NAME=Darwin`）。iOS 等 Apple 行動平台目標不會附加此框架相依或權限。NeverD 本身在架構相符的 Mac 上執行時，iOS 客體設定仍可使用 HVF。

**行程的執行檔**必須帶有 [`com.apple.security.hypervisor`](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.hypervisor)。只簽署 `libneverd.dylib` 不足。CMake 使用 `resources/macos/neverd-hypervisor.entitlements` 簽署 CLI、worker 與測試；`NEVERD_HVF_SIGN_IDENTITY` 預設為 ad hoc 的 `-`，也可指定現有簽章身分。封裝會在修復 Mach-O 相依關係後重新套用並驗證權限。

嵌入應用程式負責自己的執行檔簽章，NeverD 不會重新簽署已安裝的 Python。`cpu-capabilities --configuration=JSON --probe-host` 檢查實際行程。獨立 worker 預設也會簽署；只有不需要 HVF 時才設定 `NEVERD_WORKER_SIGN_HVF=OFF`。

## 所有權與執行規則

`backends/hvf/HvfExecutor` 擁有每個行程唯一的 VM 與一個 vCPU，由同一專用執行緒建立、使用及銷毀。邏輯 CPU 共用執行器並循序進入。切換 CPU 時，先解除舊擁有者的兩個實體區域，再映射新區域。銷毀閒置 CPU 不會移除其他 CPU 的映射。釋放 RAM 前同步解除綁定；部分註冊失敗會回復。解除映射失敗時先終止 VM，才釋放記憶體；無法復原的原生清理失敗會停止行程。

宿主映射遵循宿主頁大小，包括 Apple Silicon 的 16 KiB；架構頁表及客體 CPU 預算仍為 4 KiB。指令准入、權限、CPU 狀態、記憶體交易及 OS 服務仍由各自層負責。原生 worker 不呼叫客體觀察者，也不取得呼叫端的核心記憶體鎖。

ARM64 關閉軟體單步後，在一次原生進入中執行完整的固定 TLB/I-cache 維護序列。專用 HVC #1 必須通過返回 PC、syndrome、PSTATE 和 ESR_EL1 校驗，才會單步執行准入的客體指令。`PSTATE.D` 不能遮蔽送往 EL2 的偵錯例外。純量、TLS、FP/SIMD 狀態完整擷取。Intel 協商 VMCS 控制、使用 monitor trap、失效化 TLB 並傳遞完整 XSAVE。RIP/RFLAGS 直接透過 VMCS 安裝與擷取，包含重新建立 vCPU 之後；CR0/CR4 同時遵守框架遮罩及硬體固定位元。經認證的 CR8 讀取退出由 ISA 層完成；其他控制暫存器存取失敗。每個 vCPU 都初始化私有、受管理的 `IA32_KERNEL_GS_BASE`；客體 MSR 存取仍陷出，不支援的 MSR/SWAPGS 不予准入。

排隊與執行保留原始停止 token 及期限；準備、維護、進入與擷取共用預算。`RunDeadline` 等待中斷確認後才返回；取消會重建 vCPU，防止延遲中斷影響下一次執行。不相關的 Intel 宿主中斷在同一取消世代內重試。擷取錯誤及經認證的例外優先於同時發生的停止；一般取消狀態不會發布。這是合作式取消，沒有硬即時保證。

## 驗證方式

使用 Release，以及 CMake、Ninja、Python 3、Clang、`ld.lld`、`lld-link`、`ld64.lld`、`codesign`。三個 LLVM 連結器分別產生 ELF、PE、Mach-O 測試映像；缺少必需原生映像會使驗收失敗。

```sh
cmake -S . -B build-hvf -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_BUILD_SHARED=OFF -DNEVERD_ENABLE_PYTHON_PLUGINS=OFF \
  -DNEVERD_ENABLE_CPU_EMULATION=ON \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=OFF \
  -DNEVERD_EMULATION_BACKEND_UNICORN=OFF
python3 scripts/run_native_cpu_ci.py --build build-hvf \
  --evidence build-hvf/native-evidence --require-hvf
```

Apple Silicon 可加上 `-DNEVERD_LLVM_PREBUILT=ON`；Intel 從固定版本 LLVM 原始碼建置。[HVF 工作流程](../../.github/workflows/hvf.yml) 支援 `self-hosted, macOS, ARM64/X64, hvf`，也可用 `hosted-intel` 選擇 `macos-15-intel`。先驗證實際建立與銷毀 VM/vCPU。`validation=probe` 不能證明指令執行；`transport` 只驗證傳輸層；`darwin` 要求所有相符的 Darwin 工作負載；`full` 要求 CPU 與 Darwin 兩項完整驗收。

傳輸層要求 ARM64 15 項、Intel 10 項；完整 CPU 門檻分別為 23、18 個必需項。範圍含完整狀態、權限層級、記憶體權限、跨頁、別名、CPU 切換、回復、取消及重試。Intel 在大型建置前先驗證 CR8。產物保留完整清單、原始碼版本、宿主、結果與各次重跑。[GitHub](https://docs.github.com/en/actions/concepts/runners/github-hosted-runners) 將巢狀虛擬化列為實驗性，仍應保留專用原生 Mac 驗收路徑。

託管 Intel 的 `--execution-methods` 從完整 CTest 清單取得所有參數、旗標、環境及工作目錄，以獨立行程循序執行各 GoogleTest 方法，拒絕未知屬性。方法總期限最多 120 秒，方法內不另套用逐參數期限；逾時後有界回收行程群組。原始 XML、名稱對應與退出狀態均保留。逾時或 XML 不完整會產生部分失敗；必需原生項缺失或跳過都不能通過。自託管仍採用 CTest 逐案例行程及期限。

## 驗收證據與限制

以下為最佳化前的歷史結果；2026-10-04 的目前測量見文末。

以下為 2026-10-03 記錄；各列有重疊，不能相加：

| 範圍 | 原始碼 | 通過 | 失敗 | 跳過 | 必需原生項 |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 CPU 全部 20 個目標 | `defc93928` | 849 | 0 | 5,993 | 16/16 |
| ARM64 Darwin | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel Darwin | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| Intel 完整浮點目標 | `3e01cda5c` | 35 | 0 | 36 | 12 個 HVF 項 |

ARM64 核對 6,842 個註冊項，Intel 同一組 20 個目標為 6,840 項。Intel 253 個原生例外、除法與狀態切換項也全數通過。[Intel Darwin 工作](https://github.com/NeverSight/NeverD/actions/runs/37106013999) 核對 286 個身分、32 個行程，並通過十項傳輸、100 次恢復及 CR8。蒐集器與稽核共 124 項檢查通過。CLI、SDK、worker 及 186 個 Mach-O 的簽章均有整合驗證；該封裝的相依項目要求 macOS 15.0。

Intel 完整 CPU 驗收仍未完成。[先前執行](https://github.com/NeverSight/NeverD/actions/runs/37106679688) 於 2026-10-03 08:35 UTC 結束，GitHub 記錄執行器失聯，未產生 CPU 產物。建置及前置檢查不等於完整驗收。macOS 核心對照也不是 iOS 實體裝置核心證據。

同契約 ARM64 小型基準為 Unicorn 73.9 ms、HVF 95.1 ms，耗時約多 29%，尚未證明加速。一般指令需要六次原生進入；高宿主負載下的量測不能視為穩定效能。詳見[完整證據](../macos-hvf.md#implementation-validation-2026-10-02-to-2026-10-03)及[有限 Darwin 契約](darwin-emulation.md)。

## Intel 完整清單分片驗收

先前完整執行 `37106679688` 於 2026-10-03 08:35 UTC 結束，GitHub 記錄執行器失聯。執行 `37116327329` 的四個作業皆失聯，未產生 CPU XML。這些現象不能定位某條客體指令。託管 Intel 的 `full` 使用四個作業，最多兩個並行；每個作業連續執行四批，共十六片，編號為 `job + 4 × batch`，保留原本每個作業的測試集合。每批仍先建置並檢查完整二十個目標的 CTest 清單。`--hvf-shard INDEX/COUNT` 保留整個方法及全部參數，即使執行屬性不同也不拆散。

CPU 執行前，`scripts/prepare_hvf_batches.py` 保存完整清單、各片所選清單與方法計畫，由工作流程獨立上傳這些診斷。一個本機 composite action 連續執行四批，每批結束立即上傳原始 XML、身分對應、行程狀態及必要環境變數白名單。外層統一的 30 分鐘期限涵蓋四批執行與上傳；某批失敗後不再執行後續批次。失敗或逾時後，只要執行器仍可通訊，另有獨立的兩分鐘診斷上傳；宿主失聯時只能依靠先前已上傳的證據。計畫及未完成的診斷包不能算作通過的分片。

獨立 Linux 作業執行 `scripts/audit_hvf_shards.py`，由檢出的原始碼重新推導目標及必需原生項。工作流程僅下載本次嘗試的 CPU 產物；稽核要求同一乾淨提交的全部十六片、正確 macOS 宿主 ISA、一致的正規化執行契約，且分片互斥、聯集恰好等於完整清單。所有子行程必須成功結束，全部必需原生項必須通過；漏片、篩選器變更、摘要不符、XML 不完整及必需原生項跳過皆失敗。每個原生作業仍執行 transport、恢復、CR8 及獨立 Darwin 驗證；自託管保留未分片 CTest。分批保存本身不代表 Intel 已通過驗收。重試須重新執行全部原生作業，不混用先前嘗試的產物。

[執行 `37123148209`](https://github.com/NeverSight/NeverD/actions/runs/37123148209) 在乾淨原始碼 `f5f29a484` 上產生的首份已核驗 CPU 批次為第 `1/16` 片：476 個註冊結果、31 個方法行程，82 通過、0 失敗、394 跳過；該片唯一的必要原生項通過。產物 `11274755752` 已核驗 SHA-256，原始 XML、子行程退出狀態、清單與方法計畫均與執行前計畫核對一致。這只是 Intel 的部分證據，不代表完整 CPU 驗收通過。

## 最新本機原生驗證

乾淨原始碼 `4ce0b8247`，2026-10-03 UTC：ARM64 完整清單以 503 個方法行程執行通過，原始 XML、執行契約與子行程回收狀態均已獨立核對，獨立 Darwin 驗證亦通過。兩列覆蓋重疊，不可相加。

| 範圍 | 註冊 | 通過 | 失敗 | 跳過 | 必需原生項 |
| --- | ---: | ---: | ---: | ---: | ---: |
| CPU，完整清單 | 7,003 | 867 | 0 | 6,136 | 16/16 |
| Darwin | 286 | 65 | 0 | 221 | 39/39 |

`build-hvf-native/hvf-current-4ce0-full-evidence/summary.json` · `build-hvf-native/hvf-current-4ce0-darwin-evidence/summary.json`

## 獨立 Intel 診斷

手動觸發的 [Intel 診斷工作流程](../../.github/workflows/hvf-intel-diagnostic.yml) 分別檢出控制器與受測原始碼。`source-ref` 必須是完整提交 SHA；`shards` 選取原十六分片中的片號。`first-method` 從零計數，`method-count=0` 選取剩餘方法；只有 `method-count=1` 時，`case-index` 才能選取一個原始參數。先探索全部二十個目標的完整清單，再進行選取；保留原 Release 建置、命令、參數與原生必要檢查。

`intel-image` 與完整工作流程一致，預設選擇 `macos-15-intel`，也可選擇 `macos-26-intel` 做受控對照。執行標題標明所選映像檔，可用性檢查仍要求原生 x86-64 主機。切換映像檔同時涉及作業系統、SDK 與工具鏈，不能據此單獨歸因於核心變更。

在總計 180 分鐘的工作內，編譯具有獨立的 120 分鐘預算：首輪 macOS 26 完整建置耗時 76 分鐘。原生方法執行仍限制為 120 秒，診斷 Action 仍限制為 30 分鐘。

每個方法開始前，Action 上傳不可變執行計畫與主機快照；結束後保存原始 XML、行程回收、控制器狀態與第二份快照。快照記錄記憶體、交換空間、負載、磁碟，以及行程編號、狀態、CPU、RSS 與執行檔名稱，不包含行程參數或環境；蒐集錯誤也會保留。執行或上傳失敗即停止後續方法。每個方法仍有 120 秒執行上限；主機失聯可能阻止清理及最終上傳，此時只能使用已上傳的證據。這些局部診斷不能取代完整 CPU 或獨立 Darwin 驗收。最後一個開始標記只能定位執行邊界，不能直接確定故障指令或根因。

完整的 `Native macOS HVF` 工作流程也接受選用的 `source-ref`，預設使用工作流程所在提交，指定時必須提供完整 SHA。原生工作與彙總稽核皆會檢出並驗證同一份原始碼；即使控制器版本不同，稽核仍依受測原始碼提交核對證據。

選擇 `hosted-intel` 時，完整工作流程支援 `intel-image=macos-15-intel`（預設）或 `macos-26-intel`，兩者均列於[官方 runner 映像清單](https://github.com/actions/runner-images)。可保持相同的 `source-ref`，明確比較宿主環境；映像也會改變系統、SDK 與工具鏈。VM/vCPU、原生傳輸、CR8、完整 CPU 與 Darwin 的要求不變。選擇映像本身不能證明穩定性或執行階段修正。

`sample-active-child=true` 可在觀測到原生子程序登記五秒後保存一次封存的執行中快照：對已核驗身分的原生子程序取樣一秒呼叫堆疊、擷取最多 1 MiB 的目前日誌尾端，並記錄主機狀態。預設值為 `false`。為遵守 artifact 數量限制，啟用取樣的每個作業最多選取 166 個方法；取樣命令限時二十秒，報告上限 1 MiB。身分核驗與蒐集失敗都會記錄，包括命令回傳 0 卻沒有呼叫堆疊報告的情況。執行中快照從獨立的不可變目錄上傳；上傳失敗會取消原生子程序並使 action 失敗。取樣會改變排程，因此標記為帶取樣的部分證據，不重設原始方法計時，也不能取代完整驗收。

完整工作流也支援 `recovery-repetitions=1000`，用於集中調查中斷與恢復問題；預設仍為 `100`。選擇 1000 次時，重複測試步驟的總預算從三分鐘變為十分鐘。每次原生測試保留原始期限、斷言與遇錯停止行為，執行標題會標明加長的重複設定。這些重複測試不能取代完整 CPU 或 Darwin 驗收。

獨立的 [Intel 復原診斷工作流程](../../.github/workflows/hvf-intel-recovery.yml) 只建置 `NeverDHvfTests`，在同一處理程序中使用原始 `HvfExecutor.Native*` 篩選器，選擇 `repetitions=100` 或 `1000`。它要求精確的 `source-ref`、原生 Intel VM/vCPU 探測、Release、啟用 HVF 並停用 Unicorn。控制器、受測原始碼與官方 artifact 上傳器分別簽出。

Action 在執行前上傳計畫。執行期間保存初始處理程序標記，每增加至少 25 個完整回合保存進度；若仍有尚未保存的新日誌且進度停滯，額外保存至多一次現場。上傳期間合併進度，進度 artifact 最多 42 份，每份日誌副本最多 1 MiB，只上傳封存目錄。上傳不會暫停各回合或重設計時器，但仍影響主機排程，因此屬於附帶觀測的部分證據。

成功要求從第 1 回合到指定次數連續，每回合準確配對原生測試的 RUN/OK/PASSED，沒有失敗或略過、結束碼為零且確認處理程序已回收。重複執行時遭覆寫的 XML 無法證明這些條件。原生執行總預算為三分鐘或十分鐘，控制器另外保留 30 秒清理，Action 上限為 20 分鐘。上傳失敗會取消執行，取消作業會回收原生處理程序群組與上傳器。主機仍可連線時上傳最終證據。不完整或截斷的快照不能滿足完整 CPU 或 Darwin 驗收。

復原工作流程仍預設使用 `runner=hosted-intel`。`runner=self-hosted` 沿用 `[self-hosted, macOS, X64, hvf]` 標籤，可用於 Intel 主機對照。兩種方式都要求原生 x86-64 與 VM/vCPU 探測通過。託管映像會安裝 Ninja；自架主機須預先提供 `cmake`、`ninja`、`python3`、`clang` 和 `codesign`。原始碼隔離、重複次數限制、證據核驗與失敗處理維持一致。此入口需要已有可用的相符 runner，不會建立硬體。

2026-10-03，乾淨原始碼 `e4a8169e69eb668ed3795efe4bd5f42cf4f287c2` 在本機原生 ARM64 Release 組態下通過驗證：啟用 HVF、停用 Unicorn，使用 `NEVERD_LLVM_PREBUILT=ON`。完整 CPU 組態核對了 20 個目標、520 個方法與 7,125 個結果：882 項通過、6,243 項略過、零失敗，16 項必要原生案例全數通過。獨立 Darwin 組態核對了 32 個方法與 286 個結果：65 項通過、221 項略過、零失敗，39 項必要原生案例全數通過。原始單一處理程序復原迴圈也連續完成 1,000 次重複，接著 12 項原生傳輸測試全部通過。原始日誌、XML、處理程序狀態與相應原始碼定義均已獨立核驗。CPU 與 Darwin 的總數有重疊，不能相加。這些結果不代表 Intel 完整驗收通過，也不代表已完成 iOS SDK 建置。

使用原始碼 `bd284894c60427cf4e6a60e661a1fa0df8a070f5` 的[獨立 Intel 復原診斷執行](https://github.com/NeverSight/NeverD/actions/runs/37159724276)於 2026-10-03 23:41:30 UTC 結束，GitHub 註記明確回報託管主機失聯。計畫與十一份進度 artifact 得以保留，下載後皆核對服務端 SHA-256。最後保存的快照證明完整完成 252 回合並開始第 253 回合，不能據此定位最終故障。沒有最終結果或處理程序回收記錄，完整作業日誌介面傳回 404，因此要求的 1,000 回合仍未驗證。儲存庫當時仍無自架 runner。這些是保存下來的失敗證據，不代表穩定性已修復或 Intel 已通過完整驗收。

可使用[個人倉庫工作流程](https://github.com/gmh5225/test_mac_intel)在託管 Intel runner 上驗證，不需要本機 Intel 硬體。它分別固定 NeverD 診斷程式和被測原始碼，並獨立記錄工作流程版本。排隊時間與執行穩定性須分別判斷。恢復診斷現在會在報告失敗前保存每個上傳子程序的 PID、父程序、執行檔、結束碼、訊號及終止原因。Action 失敗後，收集器最多等待 20 秒，只複製與該子程序 PID、父程序、程序名稱及執行時間相符的 macOS IPS 當機報告；缺少報告會明確記錄。上傳失敗仍會終止原生執行，不能據此判定 Hypervisor 故障或測試通過。

2026-10-04，個人儲存庫首次 [macOS 26](https://github.com/gmh5225/test_mac_intel/actions/runs/37175472452) 與 [macOS 15](https://github.com/gmh5225/test_mac_intel/actions/runs/37175511460) 作業分別在建立後 8 秒及 5 秒啟動。兩者均因上傳子程序異常退出而失敗；控制器取消並回收原生子程序，分別留下 3 輪及 105 輪完整紀錄。最終證據包已保存並獨立核驗。另一次[純上傳對照](https://github.com/gmh5225/test_mac_intel/actions/runs/37177383621) 的 16 次上傳全部通過，17 份產物均核對伺服器摘要，並標示 `native_execution=false`。這些證據區分上傳失敗與原生斷言失敗，尚不能定位根因，也不能算作所要求的 1,000 輪通過。先前組織儲存庫的對照也在 5 秒後啟動，因此這些樣本不能證明更換儲存庫改善了排隊速度。

後續 [macOS 26](https://github.com/gmh5225/test_mac_intel/actions/runs/37176652027) 與 [macOS 15](https://github.com/gmh5225/test_mac_intel/actions/runs/37176990174) 兩次執行均於 2026-10-04 失敗結束，GitHub 明確回報託管 runner 失聯。分別保存的 10 份及 16 份產物均已核驗。最後保留的原始日誌證明分別連續完成 175 輪及 326 輪，隨後各開始一輪；不能據此定位最終故障。兩次皆缺少原生最終結果與程序回收紀錄，完整作業日誌介面皆回傳 HTTP 404。沒有發出人工取消要求。Intel 的 1,000 輪門檻仍未驗證通過。[保留的原始日誌片段與執行、摘要清單](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-04)可在 Actions 產物保留期結束後繼續查閱。

## ARM64 維護路徑最佳化（2026-10-04）

五條 TLB/I-cache 維護操作現在於同一個不可變私有程式碼段連續執行，以專用 HVC #1 退出。傳輸層嚴格核對完整 syndrome、返回 PC、PSTATE 和 ESR_EL1，隨後只單步執行一條准入的客體指令。僅在維護期間關閉軟體單步；所有屏障、完整狀態擷取和共用取消期限均保留。不使用 ERET，因為例外返回會使 ESR_EL1 在架構上成為 UNKNOWN。 [Arm](https://documentation-service.arm.com/static/649ae5b238511951cb799288).

實機為 M4 Max、macOS 15.6.1，使用 Release、Apple Clang 17 和預編譯 LLVM。獨立計數中，相同的 42044 條客體指令及 56 條啟動探針指令，原生進入從 252600 次降至 84200 次，即每條指令從六次降至兩次。計數執行的耗時不納入效能測量。三項故障／恢復測試在同一程序各連續通過 1000 輪；取消測試要求原生寫入標記，並驗證改寫客體程式碼後成功重試。

乾淨整合版本 [389bebfdd](https://github.com/NeverSight/NeverD/commit/389bebfdda31a0db19facc7ab8ca5461a8c8c1bc) 的完整 CPU 清單：2546 通過、4710 略過、零失敗，23 項必要原生案例全部通過。獨立 Darwin 清單：130 通過、156 略過、零失敗，39 項必要原生案例全部通過。兩者涵蓋範圍重疊，不能相加；不代表 iOS SDK 或裝置上的獨立驗證。

15 組程序配對交替執行，每個負載暖機一次；測量時本工作未執行編譯或測試。共用宿主負載為 26.7–33.0，軟體對比為 28.8–32.6。表中為中位數［最小–最大］毫秒；加速比為逐對耗時比的中位數，95% 百分位 bootstrap 區間採用 10000 次重抽樣、種子 20261004。明顯長尾與僅 15 組樣本限制了結論的適用範圍。

`4b54908b9` → `056090929` / Release / Apple Clang 17 / LLVM 23 prebuilt / Unicorn `df88be772`.

| 負載 | 最佳化前 ms［最小–最大］ | 最佳化後 ms［最小–最大］ | 配對加速比 | 95% 區間 | 較快配對 |
| --- | --- | --- | --- | --- | --- |
| `initialization` | 1.292 [0.804–6.793] | 1.118 [0.797–29.077] | 0.998× | 0.809–1.154 | 7/15 |
| `integer` | 125.426 [92.700–587.385] | 70.229 [54.067–895.051] | 1.595× | 1.373–1.884 | 13/15 |
| `branch` | 251.909 [168.279–974.846] | 155.678 [106.833–1566.522] | 1.495× | 1.055–1.687 | 12/15 |
| `memory` | 231.574 [140.137–1511.815] | 143.496 [96.508–1209.777] | 1.535× | 1.276–1.984 | 13/15 |
| `tls_call` | 347.850 [203.188–2536.731] | 212.467 [136.304–1441.830] | 1.552× | 1.428–2.912 | 14/15 |
| `two_cpu_switch` | 34.629 [25.019–416.105] | 26.684 [18.926–60.159] | 1.389× | 1.283–1.515 | 13/15 |

### Unicorn / 最佳化後 HVF（大於 1 表示 HVF 較快）

| 負載 | 配對加速比 | 95% 區間 |
| --- | --- | --- |
| `initialization` | 0.232× | 0.195–0.325 |
| `integer` | 1.878× | 1.437–2.896 |
| `branch` | 2.681× | 1.824–3.317 |
| `memory` | 2.967× | 2.224–3.215 |
| `tls_call` | 2.339× | 0.977–3.257 |
| `two_cpu_switch` | 0.831× | 0.541–1.612 |

這些是共用宿主上的 checked ARM64 工作負載實測，不是通用效能排名。Intel runner 失聯與程序崩潰仍待單獨定位。受限的 macOS/iOS CPU 環境並未因此變成完整 Apple OS 或裝置模擬。

[重現方法: `neverd-cpu-bench`, `benchmark_cpu.py`](../testing.md#reproduce-checked-arm64-cpu-measurements).

## Intel 隔離實驗結果（2026-10-04）

有限 owner 期限候選 `909672ca6` 仍屬實驗方案。原始 1000 輪恢復測試在 macOS 15 與 26 上均失聯；最後保留的紀錄分別證明 277/278、250/251 輪已完成/已開始。同一候選的普通指令測試沒有主動取消操作，在 macOS 15 上也失聯（576/577）。三次均有 GitHub 失聯註記，均缺少最終原生結果與程序回收紀錄；保留的紀錄無法定位最終故障。[原始證據](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-04-boundaries)。

獨立且未修改的 `hvf-edge-cases` 程式 `f150b38` 在兩個映像上完成 real-mode guest 與 100000 次隨機中斷呼叫嘗試，耗時約 362、398 秒。兩次均正常結束並回收子程序；產物摘要、原始碼雜湊、進度標記與 guest 完成輸出均已獨立核驗。先前 300 秒嘗試達到觀察器期限後被回收，沒有 runner 失聯。上游程式忽略隨機中斷回傳值，呼叫次數不代表逐次成功交付。此對照不構成 NeverD 驗收或效能比較。[原始碼、紀錄與稽核](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-04-upstream)。

這些結果縮小調查範圍，但尚未證明正式修復。保留 VM 與 Executor 執行緒僅用於診斷對照，尚未成為接受的生命週期變更。Intel 仍須在同一個乾淨候選上通過原始 1000 輪恢復、完整 CPU 清單與獨立 Darwin 門檻。
