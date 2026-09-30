**語言**：[English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← 文件索引](README.md)

# 客體程序模擬

`neverd emulate` 會在明確指定的客體 OS 設定檔下執行映像。CPU 傳輸、映像解析、程序進入點與 OS 服務由不同邊界負責。啟用 `NEVERD_ENABLE_CPU_EMULATION=ON`；驅動程式模擬也會包含它。

首個設定檔 `linux-elf64-v1` 會在 CPL3 或 EL0 執行 x64 與 AArch64 freestanding ELF `ET_EXEC`。它載入真實 ELF 區段、建構初始堆疊、依指令量恢復執行，並處理明確的 Linux 系統呼叫要求。這是獨立程序模型，不是完整 Linux 發行版，也不保證任意 libc 二進位檔都能執行。動態連結、`PT_TLS`、訊號、執行緒、FP/SIMD 及不支援的服務都會明確失敗。Windows、Android、Darwin 和其他核心工作負載另行處理。

## CLI 與 SDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

相符的 Linux 主機選擇 KVM，相符的 Windows 主機選擇 WHP；其他主機／客體 ISA 組合使用 Unicorn。所選後端不可用時會報錯，不會靜默回退。在 Windows 執行 ELF 時仍使用 Linux 程序模型。CPU 指令範圍與限制請見[CPU 執行](cpu-execution.md)。

CLI 輸出一份 JSON 報告。客體狀態為 0 時 CLI 回傳 0，其他狀態回傳 2，執行未完成（含故障與限制）回傳 3，設定/API 無效回傳 1。實際客體狀態位於 `exit_status`。新增 C 入口 [`neverd_emulate_process_json`](../../include/neverd/sdk/NeverDCAPIProcess.h) 接受 session、非空輸入路徑、明確設定檔與選用 options JSON。使用 `neverd_free_string` 釋放結果；NULL 表示設定失敗，可由 `neverd_last_error` 取得原因。客體故障或資源停止都會回傳報告。session 已載入的分析映像不需要，也不會被變更。

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

## 選項與結果

選項是最多 64 KiB 的 JSON 物件。未知/null 欄位、型別錯誤、字串內嵌 NUL、非正數限制都會遭拒。

| 選項 | 預設值 | 契約 |
|---|---|---|
| `backend` | `auto` | `auto`、`unicorn`、`kvm` 或 `whp` |
| `arguments` | 輸入檔名 | 完整 argv，包含 argv[0]；空值採預設值 |
| `environment` | `[]` | 明確的客體字串；不繼承主機環境 |
| `instruction_limit` | 100000 | 共用的已准入指令嘗試數 |
| `event_limit` | 10000 | 系統呼叫事件數，在 OS 服務處理前扣除 |
| `timeout_microseconds` | 5000000 | 程序設定完成後開始的單調 deadline |
| `memory_limit` | 67108864 | 實體／對映記憶體預算 |
| `stack_size` | 1048576 | 預算內按頁對齊的堆疊 |
| `output_limit` | 1048576 | 擷取的 stdout/stderr 總位元組數 |
| `instruction_quantum` | 1024 | 交還 runtime 前的准入間隔 |

`schema_version` 為 1。結果包含 profile、架構、所選後端與原因、`stop_reason`、可為 null 的 `exit_status`、診斷、進入／目前 PC、計數器、服務記錄與最後的型別化 CPU exit。位址、syscall 編號、參數暫存器及原始回傳位元均為**不含** `0x` 的十六進位字串；`stdout_hex`／`stderr_hex` 保留 NUL 與無效 UTF-8。syscall 結果為 null 表示沒有建模回傳值（例如 exit 或不支援要求），不代表成功回傳 0。

## Linux 設定檔語意

OS 政策重用既有 ELF 載入器解碼的 program headers。它驗證 ABI 標籤、區段對齊、已對映的 program-header tables 和使用者位址範圍。通用對映計畫會在配置前檢查範圍、權限、重疊和預算，且只公開完全準備好的私有位址空間。保留檔案頁面前／尾位元組、將 BSS 清零、遵循區段權限並為堆疊保留 guard gaps。頁面重疊版面與矛盾 header 會被拒絕，不會猜測。

初始堆疊包含對齊的 argc/argv/envp/auxv、PHDR/PHENT/PHNUM、entry、頁面大小與 identity 值。模型 PID/TID/UID/GID 均為 1000。為了可重現，`AT_RANDOM` 使用輸入 SHA-256 的前 16 個位元組；這是確定性模型政策，不是密碼學熵。HWCAP/HWCAP2 為 0，沒有 vDSO。

已實作 `write`、`exit`、`exit_group`、`getpid`、`gettid`，編號分別採用 [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) 與 [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h)。x64 SYSCALL 返回時會套用 RCX/R11 clobber、RAX 與下一個 PC；ARM64 使用 x8 作為編號，x0 作為結果。未知呼叫會以 `unsupported_service` 停止，絕不執行主機 syscall。

描述元 1、2 是虛擬位元組 sink。`write` 會驗證可讀的 user pages；若後續頁面無法存取，回傳可讀前綴；若沒有任何位元組可讀，回傳客體 `EFAULT`。錯誤描述元回傳 `EBADF`；有效描述元的零位元組寫入不會讀取指標。此處不模擬 Linux pipe 原子性或檔案物件。輸出超過限制時，會在發布寫入前停止。

## 驗證

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate)Tests$' --output-on-failure
# shared-library/CLI 建置：
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

測試為兩種 ISA 編譯原始 ELF entry assembly 與 C，檢查資料/BSS、啟動中繼資料、syscall 錯誤、二進位輸出、權限故障、部分寫入、不支援服務及跨量子預算保留。不可用後端會明確標示為 skip。公開測試會經過 C ABI/CLI，並核對報告與退出碼。交叉編譯和 Unicorn ARM64 都不是原生 ARM64 KVM/WHP 的執行證據。
