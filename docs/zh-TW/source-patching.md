**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: 817e1308a48a9a751b9ef618b8094d4082fed99234c2b7381269f524196f5c80 -->

[← Documentation Index](README.md)

# 原始碼與原生物件 patch

NeverD 使用指定編譯器編譯替換函式，再安裝到明確的原函式入口。編譯器負責 C/C++ 語言語意；NeverD 負責位址繫結、重定位、儲存、展開記錄與發佈。二進位修復與提升後的重寫繼續使用各自管線，不需重建程式其餘部分。

<!-- i18n-section: compiler -->
## 選擇編譯器

使用 `--compiler=clang`、`--compiler=gcc` 或 `--compiler=neverc`。Clang 與普通模式 NeverC 可產生 LLVM IR；`object` 製品保留編譯器產生的機器指令。CLI 對 GCC 預設選擇物件；Python SDK 使用 GCC 時須指定 `artifact="object"`。選定的編譯器不會靜默退回其他編譯器。

示例位址須替換為已確認的原函式入口。替換宣告必須符合原機器 ABI，包括呼叫慣例與結構配置；位址本身不能證明宣告正確。C++ 需要 `--language=c++` 與正確的 Clang++/G++ 路徑。建議使用 `extern "C"` 入口，否則傳入精確的修飾名稱。允許額外輔助函式。`--entry` 與 `--func` 共同明確指定替換目標；舊入口 `--from-c` 亦遵守指定位址。

```sh
# Clang C -> LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.clang

# NeverC ordinary C -> native object
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC -> native object (CLI default for GCC)
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/gcc \
  --patch-report -o app.gcc

# Independently compiled ELF x64 object
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

<!-- i18n-section: profiles -->
## 已驗證設定與限制

| 編譯器 | 製品 | 已驗證的作業系統執行設定 |
|---|---|---|
| Clang 22.1.6 | LLVM IR | ELF x64 C/C++；Mach-O x64/ARM64 C/C++；PE x64 C/C++ FH3 |
| Clang 22.1.6 | 原生物件 | ELF x64 C/C++ |
| GCC/G++ 13.3.0 | 原生物件 | ELF x64 C/C++，需要相容的 Itanium 執行階段 |
| NeverC 3389.1.5 普通模式 | LLVM IR | ELF x64 C；Mach-O x64/ARM64 C |
| NeverC 3389.1.5 普通模式 | 原生物件 | ELF x64 C |

這些是經過驗證的設定，不代表所有編譯器版本或選項皆相容。尚未接入 `dyncode` 或 NeverC C++。GCC 無法產生 LLVM IR。原生 COFF/Mach-O 物件尚不支援；物件安裝要求 ELF x64。未知重定位、TLS、建構函式、LTO、common/IFUNC 符號及無法解析的匯入皆明確失敗。JSON 憑據記錄編譯器身分、參數、繫結與安裝位址；原生物件另記錄 SHA-256。

獨立 CPU 矩陣涵蓋四種架構（`x86`、`x64`、`ARM32`、`ARM64`）與三種格式（`ELF`、`PE`、`Mach-O`），檢查原始碼安裝或明確拒絕。隔離的 CPU 執行不能驗證作業系統載入器、C++ 展開器或任意 ABI；這些結論應依據上表執行設定。

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `REFUSED` | `CPU` |
| `PE` | `REFUSED` | `CPU` | `REFUSED` | `REFUSED` |
| `Mach-O` | `REFUSED` | `CPU` | `REFUSED` | `CPU` |

`CPU` 表示原始碼安裝與隔離函式執行；`REFUSED` 表示明確缺少安裝契約。ARM32 ELF 需要 EHABI 註冊；32 位 Mach-O 需要經驗證的載入器、前端與展開契約；PE 原始碼安裝目前要求 x64。

CPU 矩陣使用獨立的算術、分支與輔助函式呼叫。其結果不能驗證 ELF x86/ARM64 資料參照或 ARM64 C++ 匯入；額外執行探測因尚未支援的重定位契約而明確拒絕安裝。

<!-- i18n-section: references -->
## 原程式參照與儲存

繫結對應外部宣告：`function` 要求已確認的原函式或匯入樁；`data` 要求可寫儲存；`const` 要求 IR 中為 const 宣告。物件資料繫結須提供非零範圍。範圍不可跨越儲存邊界或賦予 RELRO 寫入權限。不能回呼被替換函式的原始本體：此設定尚無保存原序言的回呼跳板。

新程式碼保持 RX，新可變狀態保持 RW。僅重用既有匯入，並區分 GOT/IAT 槽與可呼叫樁。Mach-O 需要空餘載入命令空間，例如 `-Wl,-headerpad,0x400`；PE 需要空餘區段標頭空間。空間不足會失敗。ELF `.data.rel.ro` 與原 CET/IBT 保護需要額外契約，目前明確拒絕。

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

Mach-O 展開表增長另需相鄰的空餘檔案與虛擬空間；只預留標頭空間並不滿足此要求。測試樣本分別處理這兩種空間限制。

<!-- i18n-section: exceptions -->
## C++ 例外

ELF 使用編譯器 FDE/LSDA；Mach-O 使用緊湊展開/DWARF。PE x64 支援編譯器 FH3 表，要求原程式已有相容的 MSVC 執行階段與型別身分；type-info 資料可能需要明確繫結。FH4、自訂 personality、SEH 原始碼與啟用的 CFG/EHCont/XFG 模式會被拒絕，原保護旗標維持不變。

執行案例涵蓋正常返回、內部捕捉、新例外由原呼叫者捕捉、原被呼叫函式的例外穿過替換函式、恰好一次 RAII 清理、重新擲出、noexcept 終止及未替換函式。原呼叫點必須允許例外傳播；patch 無法恢復原編譯器因函式不擲出例外而刪除的捕捉路徑。

<!-- i18n-section: publication -->
## SDK 與發佈

追加式 C API 為 `neverd_patch_source`，選項結構為 `neverd_source_patch_options_v1`；`neverd_patch_source_report` 傳回憑據。物件輸入須提供位元組長度。舊 API `neverd_patch_from_c` 仍可使用。

輸入先保存快照，候選檔暫存於目的地旁，重新載入並核對每個請求入口後才獨占發佈。不覆寫現有檔案或符號連結別名。缺少任一映射會使整個請求失敗。拒絕 `--mode=inplace`。工作階段 IR 變換不能用於原生物件。Mach-O 嚴格驗證簽章並重新簽署；不會靜默移除 PE 簽章。

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

<!-- i18n-section: verification -->
## 建置與驗證

透過 `NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` 指定相容的原始碼編譯器（已驗證 Clang 22.1.6）。整合建置僅修正建置目錄中的 LLVM 副本，不改子模組或共用預建快取。r3/r4 預建版的 x64 PLT 問題可能導致 IR patch 被拒絕；原生物件避開此 MC 路徑。`NEVERD_TEST_REQUIRE_FIXED_LLVM=1` 要求產生精確且已修正的 PLT 參照。

`NeverDSourcePatchTests` 包含原生 C/API/物件/例外案例；`NeverDSourcePatchMatrixTests` 以 Unicorn 檢查所有目標組合。設定 `NEVERD_TEST_WINDOWS_CRT_LIB`、`NEVERD_TEST_WINDOWS_UCRT_LIB` 及 `NEVERD_TEST_WINDOWS_SDK_LIB` 後，`SourcePatchPE` 執行七種 C++ 場景。非 Windows 宿主另需 `NEVERD_TEST_WINDOWS_RUNNER`。`NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` 將缺少執行輸入視為失敗。Windows 專項工作流程要求原生執行且拒絕略過案例。

開發 Mac 上的 Linux 證據來自 Docker 與明確的 Linux 載入器；PE 證據使用 Wine 11。Mach-O ARM64 在 Mac 本機執行。Windows 原生 CI 結果須另行查看：設定工作流程不等於執行通過。歷史數量、基線失敗與模擬環境限制見[詳細契約與驗證記錄](../source-patching-details.md)。

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=ON \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDSourcePatchMatrixTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDSourcePatchMatrixTests)$' \
  --output-on-failure --parallel 4
```

LLVM 建置副本亦修正 Mach-O x64 的 GOT 資料參照：Clang 型別捕捉表以四位元組重定位欄位末尾為基準。r3/r4 預建版會偏移四位元組，發佈前檢查會拒絕未修正的結果。開發 Mac 的 x64 Mach-O 案例透過 Rosetta 2 執行，ARM64 案例直接執行。
