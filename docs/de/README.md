**Sprachen**: [English](../README.md) | [简体中文](../zh-CN/README.md) | [繁體中文](../zh-TW/README.md) | [日本語](../ja/README.md) | [한국어](../ko/README.md) | [Français](../fr/README.md) | [Deutsch](README.md) | [Español](../es/README.md) | [Italiano](../it/README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

<!-- i18n-source: 60f7be651e94b8828ef39cb35aeea8cbafaa8e706f027def09403b2df2f42228 -->

[← NeverD-Projekt](project.md)

# NeverD-Dokumentation

Projektüberblick, Build und CLI stehen in der Repository-README. Architektur- und Testreferenzen für Mitwirkende sind hier gebündelt.

**Mobile Unterstützung (experimentelle CLI):** `neverd mobile` rekonstruiert Java aus [Android](android.md)-APK, DEX und smali sowie natives C und unterstützte Objective-C-/Swift-Quellen aus [iOS](ios.md)-IPA, `.app` und Mach-O. JSON-Berichte beschreiben Ergebnisse und Abdeckung. Beginnen Sie mit dem [mobilen Überblick (Englisch)](../mobile.md); Befehle und Grenzen stehen in den Plattformleitfäden.

Englische Anleitungen liegen direkt unter `docs/`. Übersetzungen sind in `ar/`, `de/`, `es/`, `fr/`, `it/`, `ja/`, `ko/`, `ru/`, `zh-CN/` und `zh-TW/` gruppiert. Jedes Sprachverzeichnis enthält den Index `README.md`, die Projektübersicht `project.md`, Fachanleitungen, `CONTRIBUTING.md`, `ATTRIBUTION.md` und `roadmap.md`. Gemeinsame Bilder liegen unter `assets/`.

Die CPU-Ausführung trennt ISA-Zulassung, Gastspeicher, Backend-Transport und Gast-OS-Richtlinien. `NEVERD_ENABLE_CPU_EMULATION` aktiviert die x64/ARM64-CPU-Schicht; `NEVERD_ENABLE_DRIVER_EMULATION` ergänzt die begrenzte x64-Windows-WDM/KMDF-Umgebung. `linux-elf64-v1` führt unterstützte Linux-ELF-Prozesse aus. Siehe [CPU-Ausführung](cpu-execution.md), [Gastprozess-Emulation](process-emulation.md) und [Emulation von Windows-Treibern](driver-emulation.md).

Checked-Profile verwenden KVM auf Linux und WHP auf Windows bei gleicher ISA sowie Unicorn bei unterschiedlicher ISA. Native ARM64/WHP-Ausführung bleibt unvalidiert. `driver-strict` verwendet derzeit Unicorn; der KVM/WHP-Treiberpfad benötigt `checked-x64-v1`. Ein ausdrücklich gewähltes, nicht verfügbares Backend schlägt klar fehl. Gemeinsamer RAM, Aliase, gestufte Schreibvorgänge, skalare Atomoperationen, typisierte Ausnahmen und vollständige x64-FP/SSE-Kontexte sind innerhalb der dokumentierten ISA/OS-Verträge implementiert. Dies verspricht weder beliebige Treiberkompatibilität noch implementierte Android/Darwin-Umgebungen.

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
| [Emulation von Windows-Treibern](driver-emulation.md) | Begrenzter x64-WDM/KMDF-Lebenszyklus, Anforderungen, Hardwareszenarien, SEH, PnP-Teilmengen, Backend-Auswahl und Grenzen |
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
