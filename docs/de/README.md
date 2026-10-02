**Sprachen**: [English](../README.md) | [简体中文](../zh-CN/README.md) | [繁體中文](../zh-TW/README.md) | [日本語](../ja/README.md) | [한국어](../ko/README.md) | [Français](../fr/README.md) | [Deutsch](README.md) | [Español](../es/README.md) | [Italiano](../it/README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

<!-- i18n-source: d615d9f900a3fb234918f3725d77c7385215434f8d6c98da6284f831d1adf8d8 -->

[← NeverD-Projekt](project.md)

# NeverD-Dokumentation

Projektüberblick, Build und CLI stehen in der Repository-README. Architektur- und Testreferenzen für Mitwirkende sind hier gebündelt.

**Mobile Unterstützung (experimentelle CLI):** `neverd mobile` rekonstruiert Java aus [Android](android.md)-APK, DEX und smali sowie natives C und unterstützte Objective-C-/Swift-Quellen aus [iOS](ios.md)-IPA, `.app` und Mach-O. JSON-Berichte beschreiben Ergebnisse und Abdeckung. Beginnen Sie mit dem [mobilen Überblick](mobile.md); Befehle und Grenzen stehen in den Plattformleitfäden.

Englische Anleitungen liegen direkt unter `docs/`. Übersetzungen sind in `ar/`, `de/`, `es/`, `fr/`, `it/`, `ja/`, `ko/`, `ru/`, `zh-CN/` und `zh-TW/` gruppiert. Jedes Sprachverzeichnis enthält den Index `README.md`, die Projektübersicht `project.md`, Fachanleitungen, `CONTRIBUTING.md`, `ATTRIBUTION.md` und `roadmap.md`. Gemeinsame Bilder liegen unter `assets/`.

Die CPU-Ausführung trennt ISA-Zulassung, Gastspeicher, Backend-Transport und Gast-OS-Richtlinien. `NEVERD_ENABLE_CPU_EMULATION` aktiviert die x64/ARM64-CPU-Schicht; `NEVERD_ENABLE_DRIVER_EMULATION` ergänzt die begrenzte x64-Windows-WDM/KMDF-Umgebung. `linux-elf64-v1` führt unterstützte Linux-ELF-Prozesse aus. Siehe [CPU-Ausführung](cpu-execution.md), [Gastprozess-Emulation](process-emulation.md) und [Emulation von Windows-Treibern](driver-emulation.md).

`driver-strict` / `checked-x64-v1` unterstützt KVM auf passenden Linux-x64-Hosts und WHP auf passenden Windows-x64-Hosts; `auto` wählt diesen nativen Transport, unterschiedliche ISAs verwenden Unicorn. Explizites Unicorn und die bisherige V1-API behalten das portable Softwareprofil. Native Ausführung prüft kanonische Adressen und Effekte vor dem Eintritt; fehlende Hardware führt ohne Rückfall zum Fehler. Nicht unterstützte Instruktionen und OS-Verhalten bleiben explizite Fehler. Die native Windows-x64-CI besteht bei deaktiviertem Unicorn alle 359 Pflichtprüfungen: 131 CPU-Prüfungen, 224 Treiberergebnisse aus 26 eingebauten Images, 46 WDK-Images und 40 Szenariofällen an bevorzugten und verschobenen Adressen sowie vier SEH-Grenzprüfungen ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). Native ARM64-Nachweise fehlen weiterhin; allgemeine Treiber- oder Android/Darwin-Kompatibilität ist damit nicht belegt.

`checked-aarch64-v1` und `checked-user-aarch64-v1` bieten begrenztes ARM64 FP32/FP64, SIMD fester Breite und vollständigen FPCR/FPSR/Vektorzustand. Passende Linux-ARM64-Hosts verwenden KVM, Windows ARM64 WHP und andere ISAs Unicorn. Native ARM64-Laufzeitnachweise fehlen weiterhin; Windows-Treiberladen bleibt auf x64 begrenzt.

| Dokument | Beschreibung |
|----------|--------------|
| [README (Deutsch)](project.md) | Überblick, Schnellstart, Build, SDK, CLI |
| [Mitwirken](CONTRIBUTING.md) | Entwicklungsumgebung, Build-Profile, Ablauf, Stil und PR-Anforderungen |
| [Architektur](architecture.md) | IR-Pfade, Komponentengrenzen, striktes Lifting, Supporttiefe und Änderungsorte |
| [Tests](testing.md) | Testsuiten, generierte Fixtures, Unicorn-Roundtrips und inkrementelle Befehle |
| [Desktop-Arbeitsplatz (Englisch)](../gui.md) | Optionale Qt-Quick-Oberfläche, separater Worker, C-ABI, Anmerkungen und MCP-Abläufe |
| [Desktop-Validierung (Englisch)](../gui-qualification.md) | Gemessene GUI-Nachweise, Paketierungsgrenzen und offene Plattformvalidierung |
| [Quelltextrekonstruktion aus Interpretern](interpreter-recovery.md) | Experimentelle Spezialisierung mit `--devirtualize`, CLI-Kontrollgrößen, Ausführungsvertrag, Nachweise und Grenzen; Beweisvorschläge für verschachtelte Schleifen; explizite Erkennungsbudgets und versionierte C-API; API für exakte Beweise von nativem Code zu LLVM |
| [Windows-Ausnahmerekonstruktion](windows-exception-reconstruction.md) | SEH/C++-Supportmatrix, IR-Vertrag, native Patch-Regeln und PE-Validierung |
| [CPU-Ausführung](cpu-execution.md) | Konfiguration, Fähigkeitsabfragen, Backend-Verfügbarkeit und typisierte CPU-Ergebnisse |
| [Bitvektor-Beweisbackends](solver.md) | Optionale Z3-Beweise, abgesicherte Synthese, unabhängige Tests und Query-Export |
| [Gastprozess-Emulation](process-emulation.md) | Linux-ELF-Profil, Prozessstart, Dienste, Grenzen und Tests |
| [Emulation von Windows-Treibern](driver-emulation.md) | Begrenzter x64-WDM/KMDF-Lebenszyklus, Anforderungen, Hardwareszenarien, SEH, PnP-Teilmengen, Backend-Auswahl und Grenzen |
| [Speicher-Audit und Hunt](memory-safety.md) | Heap-Lebensdauer- und Copy-Überlaufanalyse: Identitätsvertrag je Format, Senken-/Quellenkatalog, Urteile, Budgets und JSON-Schema |
| [Native Plugins](plugins.md) | Reine C-Deskriptor-ABI, Callbacks und Ereignisse, Build-/Link-Ablauf, Erkennung und Kompatibilitätsregeln |
| [Python-Plugins](python-plugins.md) | Plugin-Entwicklung, Session-/Event-API, Isolation, Tests und Veröffentlichung |
| [Mobile-Überblick](mobile.md) | Experimentelle Android-/iOS-CLI, Eingaben, Ausgaben, Berichte und Grenzen |
| [Java-Rekonstruktion für Android](android.md) | APK (einschließlich Multidex), DEX, smali-Dateien/-Verzeichnisse → Java; CLI, JSON-Bericht, Fehlerbehandlung, Grenzen und Verifikation |
| [iOS-Quelltextrekonstruktion](ios.md) | IPA/.app/Mach-O (arm64/x86_64) → natives C und unterstützte Objective-C-/Swift-Quelltexte; Layouts, CLI/Export, JSON-Abdeckung, Grenzen und Ausführungstests |
| [EVM-Dekompilation](evm.md) | Eingaben, Hardforks, IR-Stufen, C-/LLVM-Host-ABI, Solidity-Rekonstruktion und Grenzen |
| [Solana-SBF-Dekompilation](sbf.md) | SBF v0-v4, LLVM IR, C-/Rust-Ausgabe, Verifikation und bekannte Grenzen |
| [Roadmap](roadmap.md) | Status: Native Formate, EVM und Solana SBF implementiert |
| Übersetzte Dokumentation | Die Sprachlinks oben öffnen den jeweiligen Index und die Projektübersicht |

Native x64- und ARM64-Startproben prüfen begrenzte vollständige Zustandsausführung mit exklusivem Speicherrecht. XSAVE-Pakete und ISA-abhängige Seitentabellen-Caches haben einen eindeutigen Besitzer; native ARM64-Lastnachweise bleiben unvollständig.

Native x64-Felder `FOP/FIP/FDP` folgen den Sicherungsregeln des Hosts: AMD darf inaktive x87-Ausnahmemetadaten löschen. Startprüfungen validieren sie mit einer ausstehenden unmaskierten Ausnahme.
