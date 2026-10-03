**語言**: [English](../README.md) | [简体中文](../zh-CN/README.md) | [繁體中文](README.md) | [日本語](../ja/README.md) | [한국어](../ko/README.md) | [Français](../fr/README.md) | [Deutsch](../de/README.md) | [Español](../es/README.md) | [Italiano](../it/README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

<!-- i18n-source: 2266006a7b377ad4fcf7286e06f099b4964f598b0a7bb518c4da1e0cb68aad8e -->

[← NeverD 專案](project.md)

# NeverD 文件

專案概覽、建置與 CLI 說明見儲存庫 README。面向貢獻者的設計與測試資料統一收錄於此。

**行動平台支援（實驗性 CLI）：** `neverd mobile` 支援從 [Android](android.md) APK、DEX、smali 恢復 Java，以及從 [iOS](ios.md) IPA、`.app`、Mach-O 恢復原生 C 與受支援的 Objective-C/Swift 原始碼。JSON 報告記錄恢復結果及涵蓋範圍。先閱讀[行動平台總覽](mobile.md)，再參考平台指南的命令與限制。

英文指南直接位於 `docs/`。譯文按語言分布於 `ar/`、`de/`、`es/`、`fr/`、`it/`、`ja/`、`ko/`、`ru/`、`zh-CN/` 與 `zh-TW/` 目錄。各語言目錄包含文件索引 `README.md`、專案概覽 `project.md`、主題指南、`CONTRIBUTING.md`、`ATTRIBUTION.md` 與 `roadmap.md`。共用圖片位於 `assets/`。

| 文件 | 說明 |
|------|------|
| [專案說明（繁體中文）](project.md) | 概覽、快速開始、建置、SDK、CLI |
| [貢獻指南](CONTRIBUTING.md) | 開發環境、建置設定、工作流程、風格與 PR 要求 |
| [架構](architecture.md) | IR 路徑、元件邊界、嚴格提升、支援深度與修改位置 |
| [測試](testing.md) | 測試套件、產生的 fixture、Unicorn 往返與增量命令 |
| [桌面工作台 (英文)](../gui.md) | 可選 Qt Quick 介面、獨立工作行程、C ABI、註記與 MCP 工作流程 |
| [桌面驗收紀錄 (英文)](../gui-qualification.md) | 已量測的 GUI 證據、封裝邊界與尚未完成的平台驗收 |
| [直譯器原始碼還原](interpreter-recovery.md) | 實驗性 x64 直譯器特化、HighC/LLVMC 輸出、執行前提、證據與限制; 巢狀迴圈證明候選; 明確的探索預算與版本化 C API; 精確的原生到 LLVM 證明 API |
| [Windows 例外重建](windows-exception-reconstruction.md) | SEH/C++ 展開支援矩陣、IR 契約、原生 patch 規則與 PE 驗證 |
| [CPU 執行與客體環境](emulation.md) | 後端選擇、來賓環境、原生驗證與目前限制 |
| [CPU 執行](cpu-execution.md) | 組態、能力查詢、後端可用性與型別化結果 |
| [Bitvector 證明後端](solver.md) | 選用 Z3 證明、門控合成、獨立檢查與查詢匯出 |
| [客體程序模擬](process-emulation.md) | Linux ELF 設定檔、程序啟動、服務、限制與測試 |
| [macOS/iOS 行程環境](darwin-emulation.md) | Mach-O 啟動、裝置與模擬器平台、Darwin 服務及分頁規則 |
| [macOS HVF](macos-hvf.md) | 主機同架構硬體執行、簽章權限、封裝與驗證 |
| [Windows 驅動程式模擬](driver-emulation.md) | 有界 x64 WDM/KMDF 生命週期、請求、硬體情境、SEH、PnP 子集、後端選擇及限制 |
| [記憶體安全稽核與獵取](memory-safety.md) | 堆積生命週期與拷貝越界分析：各格式身分契約、匯/源目錄、判定、預算與 JSON 模式 |
| [原生外掛](plugins.md) | 純 C 描述元 ABI、回呼與事件、建置/連結流程、探索順序及相容性規則 |
| [Python 外掛](python-plugins.md) | 外掛撰寫、工作階段與事件 API、隔離、測試及發佈 |
| [行動平台支援總覽](mobile.md) | Android / iOS 輸入、原始碼輸出、CLI 流程與限制 |
| [Android Java 還原](android.md) | APK（multidex）、DEX、smali 檔案／目錄 → Java。CLI 流程、執行環境、選項、JSON 報告、錯誤處理與驗證限制 |
| [iOS 原始碼還原](ios.md) | IPA、`.app`、Mach-O → 原生 C 與受支援的 Objective-C / Swift 原始碼。輸入選擇、方法與配置、CLI/export、JSON 覆蓋率報告、限制及執行驗證 |
| [EVM 反編譯](evm.md) | EVM 輸入、硬分叉、分級 IR、C/LLVM host ABI、Solidity 重建與限制 |
| [Solana SBF 反編譯](sbf.md) | SBF v0-v4、LLVM IR、C/Rust 輸出、驗證與已知限制 |
| [路線圖](roadmap.md) | 狀態：原生格式、EVM 與 Solana SBF 均已實作 |
| 本地化文件 | 使用上方語言連結開啟各語言的文件索引與專案概覽 |
