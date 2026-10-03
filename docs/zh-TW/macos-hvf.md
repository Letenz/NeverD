**語言**: [English](../macos-hvf.md) | [简体中文](../zh-CN/macos-hvf.md) | [繁體中文](macos-hvf.md) | [日本語](../ja/macos-hvf.md) | [한국어](../ko/macos-hvf.md) | [Français](../fr/macos-hvf.md) | [Deutsch](../de/macos-hvf.md) | [Español](../es/macos-hvf.md) | [Italiano](../it/macos-hvf.md) | [Русский](../ru/macos-hvf.md) | [العربية](../ar/macos-hvf.md)

<!-- i18n-source: 26699fc7c6123371ff1bdf772f3c7091876e02768c97f805a3b9d0d19ca3a5db -->

[← 文件索引](README.md)

# macOS 原生 CPU 執行（HVF）

NeverD 使用 Apple 的 [Hypervisor.framework](https://developer.apple.com/documentation/hypervisor)，作為 macOS 上對應 KVM、WHP 的後端。`--backend hvf` 明確選用 HVF；`auto` 在契約允許原生執行且宿主、客體 ISA 相符時選用它：Apple Silicon 執行 ARM64，Intel Mac 執行 x86-64。`software-cpu-v1` 與跨 ISA 的自動選擇仍使用 Unicorn。經 Rosetta 轉譯的執行檔會被拒絕。

傳輸層需要 macOS 11 以上及硬體虛擬化；其他建置相依項目可能要求更高版本。明確選定的原生後端不可用時會回報原因，不會悄悄改用其他後端。[Virtualization.framework](https://developer.apple.com/documentation/virtualization) 是完整虛擬機介面；NeverD 需要 Hypervisor.framework 的 vCPU、暫存器、記憶體映射與例外控制。

## 建置與簽章

啟用 `NEVERD_ENABLE_CPU_EMULATION=ON` 或驅動程式模擬。`NEVERD_EMULATION_BACKEND_HVF` 預設為 `ON`，僅在 macOS 連結框架；設為 `OFF` 後仍能解析 `hvf` 名稱，但能力 API 回報 `build_disabled`。

**行程的執行檔**必須帶有 [`com.apple.security.hypervisor`](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.hypervisor)。只簽署 `libneverd.dylib` 不足。CMake 使用 `resources/macos/neverd-hypervisor.entitlements` 簽署 CLI、worker 與測試；`NEVERD_HVF_SIGN_IDENTITY` 預設為 ad hoc 的 `-`，也可指定現有簽章身分。封裝會在修復 Mach-O 相依關係後重新套用並驗證權限。

嵌入應用程式負責自己的執行檔簽章，NeverD 不會重新簽署已安裝的 Python。`cpu-capabilities --configuration=JSON --probe-host` 檢查實際行程。獨立 worker 預設也會簽署；只有不需要 HVF 時才設定 `NEVERD_WORKER_SIGN_HVF=OFF`。

## 所有權與執行規則

`backends/hvf/HvfExecutor` 擁有每個行程唯一的 VM 與一個 vCPU，由同一專用執行緒建立、使用及銷毀。邏輯 CPU 共用執行器並循序進入。切換 CPU 時，先解除舊擁有者的兩個實體區域，再映射新區域。銷毀閒置 CPU 不會移除其他 CPU 的映射。釋放 RAM 前同步解除綁定；部分註冊失敗會回復。解除映射失敗時先終止 VM，才釋放記憶體；無法復原的原生清理失敗會停止行程。

宿主映射遵循宿主頁大小，包括 Apple Silicon 的 16 KiB；架構頁表及客體 CPU 預算仍為 4 KiB。指令准入、權限、CPU 狀態、記憶體交易及 OS 服務仍由各自層負責。原生 worker 不呼叫客體觀察者，也不取得呼叫端的核心記憶體鎖。

ARM64 先單步執行五條固定的 TLB/I-cache 維護指令，再執行准入的客體指令。`PSTATE.D` 不能遮蔽送往 EL2 的偵錯例外。純量、TLS、FP/SIMD 狀態完整擷取。Intel 協商 VMCS 控制、使用 monitor trap、失效化 TLB 並傳遞完整 XSAVE。RIP/RFLAGS 直接透過 VMCS 安裝與擷取，包含重新建立 vCPU 之後；CR0/CR4 同時遵守框架遮罩及硬體固定位元。經認證的 CR8 讀取退出由 ISA 層完成；其他控制暫存器存取失敗。每個 vCPU 都初始化私有、受管理的 `IA32_KERNEL_GS_BASE`；客體 MSR 存取仍陷出，不支援的 MSR/SWAPGS 不予准入。

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

傳輸層要求 ARM64 12 項、Intel 10 項；完整 CPU 門檻分別為 16、14 個必需項。範圍含完整狀態、權限層級、記憶體權限、跨頁、別名、CPU 切換、回復、取消及重試。Intel 在大型建置前先驗證 CR8。產物保留完整清單、原始碼版本、宿主、結果與各次重跑。[GitHub](https://docs.github.com/en/actions/concepts/runners/github-hosted-runners) 將巢狀虛擬化列為實驗性，仍應保留專用原生 Mac 驗收路徑。

託管 Intel 的 `--execution-methods` 從完整 CTest 清單取得所有參數、旗標、環境及工作目錄，以獨立行程循序執行各 GoogleTest 方法，拒絕未知屬性。方法總期限最多 120 秒，方法內不另套用逐參數期限；逾時後有界回收行程群組。原始 XML、名稱對應與退出狀態均保留。逾時或 XML 不完整會產生部分失敗；必需原生項缺失或跳過都不能通過。自託管仍採用 CTest 逐案例行程及期限。

## 驗收證據與限制

以下為 2026-10-03 記錄；各列有重疊，不能相加：

| 範圍 | 原始碼 | 通過 | 失敗 | 跳過 | 必需原生項 |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 CPU 全部 20 個目標 | `defc93928` | 849 | 0 | 5,993 | 16/16 |
| ARM64 Darwin | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel Darwin | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| Intel 完整浮點目標 | `3e01cda5c` | 35 | 0 | 36 | 12 個 HVF 項 |

ARM64 核對 6,842 個註冊項，Intel 同一組 20 個目標為 6,840 項。Intel 253 個原生例外、除法與狀態切換項也全數通過。[Intel Darwin 工作](https://github.com/NeverSight/NeverD/actions/runs/37106013999) 核對 286 個身分、32 個行程，並通過十項傳輸、100 次恢復及 CR8。蒐集器與稽核共 124 項檢查通過。CLI、SDK、worker 及 186 個 Mach-O 的簽章均有整合驗證；該封裝的相依項目要求 macOS 15.0。

[Intel 完整 CPU 驗收](https://github.com/NeverSight/NeverD/actions/runs/37106679688) 仍待結果：截至 2026-10-03 08:24 UTC，超過設定期限仍無最終結果或 CPU 產物。建置及前置檢查不等於完整驗收。macOS 核心對照也不是 iOS 實體裝置核心證據。

同契約 ARM64 小型基準為 Unicorn 73.9 ms、HVF 95.1 ms，耗時約多 29%，尚未證明加速。一般指令需要六次原生進入；高宿主負載下的量測不能視為穩定效能。詳見[完整證據](../macos-hvf.md#implementation-validation-2026-10-02-to-2026-10-03)及[有限 Darwin 契約](darwin-emulation.md)。
