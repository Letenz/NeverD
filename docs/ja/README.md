**Languages**: [English](../README.md) | [简体中文](../zh-CN/README.md) | [繁體中文](../zh-TW/README.md) | [日本語](README.md) | [한국어](../ko/README.md) | [Français](../fr/README.md) | [Deutsch](../de/README.md) | [Español](../es/README.md) | [Italiano](../it/README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

[← NeverD プロジェクト](project.md)

# NeverD ドキュメント

プロジェクト概要・ビルド・CLI はリポジトリ README にあります。コントリビューター向けの設計・テスト資料をここにまとめています。

実験的な `neverd mobile` CLI は Android と iOS のソース復元に対応しています。Android は APK（multidex を含む）、DEX、smali ファイル／ディレクトリから Java と JSON レポートを生成します。iOS は IPA、`.app`、Mach-O（arm64 / x86_64）からネイティブ C、対応する Objective-C / Swift ソースと JSON カバレッジレポートを生成します。復元範囲と制限事項は各ガイドを参照してください。

CPU 実行は ISA 検証、ゲストメモリー、バックエンド転送、ゲスト OS 方針を分離します。`NEVERD_ENABLE_CPU_EMULATION` は x64/ARM64 CPU 層を有効にし、`NEVERD_ENABLE_DRIVER_EMULATION` は範囲を限定した x64 Windows WDM/KMDF 環境を追加します。`linux-elf64-v1` は対応する Linux ELF プロセスを実行します。[CPU 実行](cpu-execution.md)、[ゲストプロセスのエミュレーション](process-emulation.md)、[Windows ドライバーエミュレーション](driver-emulation.md)を参照してください。

checked プロファイルは ISA が一致する Linux ホストで KVM、Windows ホストで WHP、異なる ISA 間で Unicorn を使います。ネイティブ ARM64/WHP の実行検証は未完了です。現在 `driver-strict` は Unicorn を使い、ドライバーの KVM/WHP 経路には `checked-x64-v1` が必要です。明示したバックエンドが利用できなければ明確に失敗します。共有 RAM、エイリアス、段階的書き込み、スカラー原子操作、型付き例外、完全な x64 FP/SSE コンテキストは記載された命令と OS 契約の範囲で実装されています。任意のドライバーへの互換性や Android/Darwin 環境の実装を意味しません。

| 文書 | 説明 |
|------|------|
| [プロジェクト説明（日本語）](project.md) | 概要、クイックスタート、ビルド、SDK、CLI |
| [貢献ガイド](CONTRIBUTING.md) | 開発環境、ビルドプロファイル、ワークフロー、スタイル、PR 要件 |
| [アーキテクチャ](architecture.md) | IR 経路、コンポーネント境界、strict lifting、サポート深度、変更箇所 |
| [テスト](testing.md) | テストスイート、生成 fixture、Unicorn ラウンドトリップ、増分コマンド |
| [CPU 実行](cpu-execution.md) | 構成、機能照会、バックエンド可用性、型付き結果 |
| [ゲストプロセスのエミュレーション](process-emulation.md) | Linux ELF プロファイル、起動、サービス、制限、テスト |
| [Bitvector 証明バックエンド](solver.md) | オプションの Z3 証明、証明付き合成、独立検査、query export |
| [インタープリターのソース復元](interpreter-recovery.md) | 実験的な x64 インタープリター特化、HighC/LLVMC 出力、実行前提、証拠と制限; 入れ子ループの証明候補; 明示的な探索予算とバージョン付き C API |
| [Windows ドライバーエミュレーション](driver-emulation.md) | 有界 x64 WDM/KMDF ライフサイクル、要求、ハードウェアシナリオ、SEH、PnP サブセット、バックエンド選択と制限 |
| [Windows 例外再構築](windows-exception-reconstruction.md) | SEH/C++ サポート表、IR 契約、ネイティブ patch 規則、PE 検証 |
| [メモリ安全性の監査とハント](memory-safety.md) | ヒープ寿命とコピー越境解析：形式ごとの識別契約、シンク／ソースカタログ、判定、予算、JSON スキーマ |
| [ネイティブプラグイン](plugins.md) | 純粋 C descriptor ABI、callback と event、build/link workflow、discovery、互換性規則 |
| [Python プラグイン](python-plugins.md) | プラグイン作成、セッション／イベント API、分離、テスト、公開 |
| [EVM 逆コンパイル](evm.md) | 入力、hardfork、段階 IR、C/LLVM host ABI、Solidity 復元、制限 |
| [Solana SBF 逆コンパイル](sbf.md) | SBF v0-v4、LLVM IR、C/Rust 出力、検証、既知の制限 |
| [モバイル対応の概要（English）](../mobile.md) | Android / iOS の入力、ソース出力、CLI の流れと制限 |
| [Android の Java 復元](android.md) | APK（multidex）・DEX・smali ファイル／ディレクトリ → Java。CLI 手順、実行環境、オプション、JSON レポート、エラー処理、検証の限界 |
| [iOS ソース復元](ios.md) | IPA・`.app`・Mach-O → ネイティブ C と対応する Objective-C / Swift ソース。入力選択、メソッド本体と配置、CLI/export、JSON カバレッジレポート、制限と実行検証 |
| [ロードマップ](roadmap.md) | 状態：native format、EVM、Solana SBF を実装済み |
| [English README](../../README.md) | 英語版 |
| [他言語 README](../README.md) | その他のローカライズ版 |
