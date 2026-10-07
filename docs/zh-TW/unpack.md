**語言**：[English](../unpack.md) | [简体中文](../zh-CN/unpack.md) | [繁體中文](unpack.md) | [日本語](../ja/unpack.md) | [한국어](../ko/unpack.md) | [Français](../fr/unpack.md) | [Deutsch](../de/unpack.md) | [Español](../es/unpack.md) | [Italiano](../it/unpack.md) | [Русский](../ru/unpack.md) | [العربية](../ar/unpack.md)

[← 文件索引](README.md)

# 加殼可執行檔的脫殼

`neverd unpack` 還原加殼的可執行檔在自身位址空間中重建出的程式。它把輸入當作有界的來賓行程執行，觀察外殼把控制權交給它所產生程式碼的位置，並把該時刻的映像寫成同一容器格式的新檔案。它不做去虛擬化：被保護器虛擬化的函式仍保持虛擬化。建置時需要 `NEVERD_ENABLE_CPU_EMULATION=ON`。

## 支援的輸入

容器格式決定檔案如何驗證和重建，指令集決定如何判定一次轉移，兩者共同決定來賓行程設定檔。不在此表中的輸入會在執行任何程式碼之前被依名稱拒絕。

| 容器 (`format`) | 指令集 | 來賓設定檔 | 外殼知識 |
| --- | --- | --- | --- |
| PE32+ (`pe64`) | x86-64 | [`windows-pe64-v1`](process-emulation.md) | UPX |
| PE32+ (`pe64`) | ARM64 | [`windows-pe64-v1`](process-emulation.md) | 無；僅靠觀察 |

## 用法

```bash
neverd unpack packed.exe -o unpacked.exe
neverd unpack packed.exe -o unpacked.exe \
  --options='{"backend":"unicorn","instruction_limit":400000000,"transfer":2}'
```

該命令輸出一份 JSON 報告。結束碼 0 表示映像已寫出；3 表示有界執行在接受入口之前結束（`outcome` 為 `no_entry`，不寫入任何檔案）；1 表示輸入、選項無效或準備失敗。報告會給出實際執行的 `format`、`architecture` 和 `profile`。C 入口是 `neverd_unpack_json`；Python 提供 `Session.unpack`。選項為[行程選項](process-emulation.md)加上 `transfer`。外殼需要更多資源之處預設值不同：100000000 條指令、600 秒、512 MiB，並且 `windows.defer_unmodeled` 預設開啟。

## 入口如何確定

第零代是來賓載入器對映後的映像。位元組與該映像不同的指令是行程自己產生的。一次轉移是指首次執行比目前正在執行的程式碼更新的程式碼；`transfers` 列出每一次轉移的 RVA、`generation`，以及堆疊指標是否等於行程入口時的值（`stack_balanced`）。

1. 在入口堆疊上發生的轉移就是程式入口：外殼已經歸還了它得到的堆疊。此時 `entry_source` 為 `transfer`。
2. 在更深的堆疊上發生的轉移是外殼對程式發起的呼叫，例如 TLS 回呼。它會被報告，但不會被接受。當已識別的外殼指明其最終跳躍目標時，映像在該呼叫處重建（此時程式的任何程式碼都尚未執行），所指明的位址即為入口。此時 `entry_source` 為 `stub`。
3. `transfer` 依位置明確選擇清單中的某次轉移，用於分階段脫殼或以呼叫方式進入程式的保護器。

不會根據編譯器啟動程式碼的形態去預測入口。若執行先行停止，則報告 `no_entry` 以及行程的 `stop_reason`。

## 重建的映像

各節保持原有 RVA，內容為觀察到的記憶體；每個節的存取權限取其頁面在轉移時刻的權限。末尾的 `.neverd` 節保存一個新的匯入目錄，它涵蓋程式原本就透過其呼叫的儲存格，因此程式碼和資料都不會移動。`imports` 列出每個儲存格及其 `origin`：`static` 儲存格由載入器根據輸入自身的目錄繫結，`runtime` 儲存格由外殼寫入。映像固定在觀察到的基底位址上：未觀察到針對產生內容的重定位，因此移除重定位目錄並設定 `IMAGE_FILE_RELOCS_STRIPPED`。對於 UPX，會重新指向程式自身的 TLS 目錄，因為加殼後的目錄只能到達外殼的處理函式。

原始節表、匯入目錄配置和重定位表不會被重建；加殼器並不會在記憶體中還原它們。

## 識別

`packer.kind` 只依據檔案中的證據來命名保護器。UPX 需要 `upx_section_names`、`upx_pack_header`（魔數、格式、演算法和校驗和）與 `upx_entry_stub` 三者中的兩項。未識別的輸入仍可透過觀察來脫殼。

## 限制

僅支援可執行檔；不執行 DLL。受檢執行一次只放行一條指令，量級約為每秒 10^5 條，因此需要數十億條指令的外殼會超出任何實際預算。執行依 4 KiB 分頁追蹤：寫入到正在執行同一代程式碼的分頁中的程式碼，不會被報告為一次轉移。在入口之前執行的程式碼（例如呼叫了未建模 API 的 TLS 回呼）會使執行停止，除非外殼宣告了自己的入口。當每個受保護的匯入呼叫仍是六位元組、並且以尾呼叫進入已解析的匯出時，VMProtect 載入器可以被脫殼。這些呼叫點會改寫成普通的匯入呼叫。虛擬化的程式碼仍保持虛擬化。

## 驗證

`NeverDUnpackTests` 檢查識別。`NeverDUnpackExecutionTests` 在 Unicorn、KVM 和 WHP 上對已入庫的 UPX 樣本（NRV2B、NRV2D、NRV2E、LZMA 以及一個帶 C 執行階段的程式）脫殼，把每個節與原件比較，執行還原出的映像，並要求所有後端產出逐位元組相同的結果。`UnpackGeneratedTests.cpp` 在測試內部為 x86-64 和 ARM64 替一個程式加殼，並對照連結產物檢查三種載入器：直接離開的、先呼叫程式再離開的，以及兩級載入器。`NeverDUnpackPublicTests` 涵蓋 C ABI 與 CLI。`unittests/unpack/fixtures/Makefile` 用於重新產生 UPX 樣本。
