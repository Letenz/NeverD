**Sprachen**: [English](../README.md) | [简体中文](../zh-CN/README.md) | [繁體中文](../zh-TW/README.md) | [日本語](../ja/README.md) | [한국어](../ko/README.md) | [Français](../fr/README.md) | [Deutsch](README.md) | [Español](../es/README.md) | [Italiano](../it/README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

<!-- i18n-source: ca9503837987b49f4237fc47501b098aa43bb3e4fcdda4fe38c64eae4aad6b50 -->

[← NeverD-Projekt](project.md)

# NeverD-Dokumentation

Projektüberblick, Build und CLI stehen in der Repository-README. Architektur- und Testreferenzen für Mitwirkende sind hier gebündelt.

**Mobile Unterstützung (experimentelle CLI):** `neverd mobile` rekonstruiert Java aus [Android](android.md)-APK, DEX und smali sowie natives C und unterstützte Objective-C-/Swift-Quellen aus [iOS](ios.md)-IPA, `.app` und Mach-O. JSON-Berichte beschreiben Ergebnisse und Abdeckung. Beginnen Sie mit dem [mobilen Überblick (Englisch)](../mobile.md); Befehle und Grenzen stehen in den Plattformleitfäden.

Englische Anleitungen liegen direkt unter `docs/`. Übersetzungen sind in `ar/`, `de/`, `es/`, `fr/`, `it/`, `ja/`, `ko/`, `ru/`, `zh-CN/` und `zh-TW/` gruppiert. Jedes Sprachverzeichnis enthält den Index `README.md`, die Projektübersicht `project.md`, Fachanleitungen, `CONTRIBUTING.md`, `ATTRIBUTION.md` und `roadmap.md`. Gemeinsame Bilder liegen unter `assets/`.

| Dokument | Beschreibung |
|----------|--------------|
| [README (Deutsch)](project.md) | Überblick, Schnellstart, Build, SDK, CLI |
| [Mitwirken](CONTRIBUTING.md) | Entwicklungsumgebung, Build-Profile, Ablauf, Stil und PR-Anforderungen |
| [Architektur](architecture.md) | IR-Pfade, Komponentengrenzen, striktes Lifting, Supporttiefe und Änderungsorte |
| [Tests](testing.md) | Testsuiten, generierte Fixtures, Unicorn-Roundtrips und inkrementelle Befehle |
| [Desktop-Arbeitsplatz (Englisch)](../gui.md) | Optionale Qt-Quick-Oberfläche, separater Worker, C-ABI, Anmerkungen und MCP-Abläufe |
| [Desktop-Validierung (Englisch)](../gui-qualification.md) | Gemessene GUI-Nachweise, Paketierungsgrenzen und offene Plattformvalidierung |
| [Quelltextrekonstruktion aus Interpretern](interpreter-recovery.md) | Experimentelle Spezialisierung mit `--devirtualize`, CLI-Kontrollgrößen, Ausführungsvertrag, Nachweise und Grenzen; Beweisvorschläge für verschachtelte Schleifen; explizite Erkennungsbudgets und versionierte C-API |
| [Windows-Ausnahmerekonstruktion](windows-exception-reconstruction.md) | SEH/C++-Supportmatrix, IR-Vertrag, native Patch-Regeln und PE-Validierung |
| [CPU-Ausführung](cpu-execution.md) | Konfiguration, Fähigkeitsabfragen, Backend-Verfügbarkeit und typisierte CPU-Ergebnisse |
| [Bitvektor-Beweisbackends](solver.md) | Optionale Z3-Beweise, abgesicherte Synthese, unabhängige Tests und Query-Export |
| [Gastprozess-Emulation](process-emulation.md) | Linux-ELF-Profil, Prozessstart, Dienste, Grenzen und Tests |
| [Emulation von Windows-Treibern](driver-emulation.md) | Begrenzte x64-WDM-Initialisierung, serielle buffered/direct Anforderungen, Work Items, Timer, DPCs, Ereignisse und Warten, Berichte und Grenzen; KMDF-1.33-Nicht-PnP-Treiber-/Objektlebenszeiten und validiertes x64-CFG |
| [Speicher-Audit und Hunt](memory-safety.md) | Heap-Lebensdauer- und Copy-Überlaufanalyse: Identitätsvertrag je Format, Senken-/Quellenkatalog, Urteile, Budgets und JSON-Schema |
| [Native Plugins](plugins.md) | Reine C-Deskriptor-ABI, Callbacks und Ereignisse, Build-/Link-Ablauf, Erkennung und Kompatibilitätsregeln |
| [Python-Plugins](python-plugins.md) | Plugin-Entwicklung, Session-/Event-API, Isolation, Tests und Veröffentlichung |
| [Mobile-Überblick (Englisch)](../mobile.md) | Experimentelle Android-/iOS-CLI, Eingaben, Ausgaben, Berichte und Grenzen |
| [Java-Rekonstruktion für Android](android.md) | APK (einschließlich Multidex), DEX, smali-Dateien/-Verzeichnisse → Java; CLI, JSON-Bericht, Fehlerbehandlung, Grenzen und Verifikation |
| [iOS-Quelltextrekonstruktion](ios.md) | IPA/.app/Mach-O (arm64/x86_64) → natives C und unterstützte Objective-C-/Swift-Quelltexte; Layouts, CLI/Export, JSON-Abdeckung, Grenzen und Ausführungstests |
| [EVM-Dekompilation](evm.md) | Eingaben, Hardforks, IR-Stufen, C-/LLVM-Host-ABI, Solidity-Rekonstruktion und Grenzen |
| [Solana-SBF-Dekompilation](sbf.md) | SBF v0-v4, LLVM IR, C-/Rust-Ausgabe, Verifikation und bekannte Grenzen |
| [Roadmap](roadmap.md) | Status: Native Formate, EVM und Solana SBF implementiert |
| Übersetzte Dokumentation | Die Sprachlinks oben öffnen den jeweiligen Index und die Projektübersicht |
