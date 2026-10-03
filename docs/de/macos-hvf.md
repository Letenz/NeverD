**Sprachen**: [English](../macos-hvf.md) | [简体中文](../zh-CN/macos-hvf.md) | [繁體中文](../zh-TW/macos-hvf.md) | [日本語](../ja/macos-hvf.md) | [한국어](../ko/macos-hvf.md) | [Français](../fr/macos-hvf.md) | [Deutsch](macos-hvf.md) | [Español](../es/macos-hvf.md) | [Italiano](../it/macos-hvf.md) | [Русский](../ru/macos-hvf.md) | [العربية](../ar/macos-hvf.md)

<!-- i18n-source: 43701db74dfe3fb59f552a78b88ace1c40c70eef0b1d4d461047864429293865 -->

[← Dokumentationsübersicht](README.md)

# Native CPU-Ausführung unter macOS (HVF)

NeverD verwendet [Hypervisor.framework](https://developer.apple.com/documentation/hypervisor) als macOS-Gegenstück zu KVM und WHP. `--backend hvf` wählt es ausdrücklich aus. `auto` nutzt HVF bei einem geeigneten nativen Vertrag und gleicher Host-/Gast-ISA: ARM64 auf Apple Silicon, x86-64 auf Intel. `software-cpu-v1` und automatische Ausführung über ISA-Grenzen hinweg verwenden Unicorn. Ein unter Rosetta übersetzter Prozess wird abgewiesen.

Erforderlich sind macOS 11 oder neuer und Hardwarevirtualisierung. Andere Abhängigkeiten können eine neuere OS-Version verlangen. Ein ausgewähltes natives Backend meldet fehlende Verfügbarkeit ohne stillen Rückfall. [Virtualization.framework](https://developer.apple.com/documentation/virtualization) stellt vollständige VMs bereit; NeverD benötigt dagegen die vCPU-, Register-, Speicher- und Ausnahmesteuerung von Hypervisor.framework.

## Bauen und signieren

Aktivieren Sie `NEVERD_ENABLE_CPU_EMULATION=ON` oder die Treiberemulation. `NEVERD_EMULATION_BACKEND_HVF` ist standardmäßig `ON` und bindet das Framework nur unter macOS ein. Bei `OFF` bleibt der Name `hvf` gültig, die Fähigkeitsabfrage meldet jedoch `build_disabled`.

Die **ausführbare Prozessdatei** benötigt [`com.apple.security.hypervisor`](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.hypervisor). Nur `libneverd.dylib` zu signieren genügt nicht. CMake signiert CLI, Worker und Tests mit `resources/macos/neverd-hypervisor.entitlements`. `NEVERD_HVF_SIGN_IDENTITY` verwendet standardmäßig die Ad-hoc-Identität `-`; eine vorhandene Signieridentität ist ebenfalls möglich. Beim Paketbau wird die Berechtigung nach der Mach-O-Abhängigkeitskorrektur erneut angewendet und geprüft.

Einbettende Anwendungen signieren ihre eigene Prozessdatei. NeverD signiert keinen installierten Python-Interpreter um. `cpu-capabilities --configuration=JSON --probe-host` prüft den tatsächlichen Prozess. Eigenständige Worker werden standardmäßig signiert; `NEVERD_WORKER_SIGN_HVF=OFF` ist nur für Worker ohne HVF-Bedarf vorgesehen.

## Zuständigkeiten und Ausführung

`backends/hvf/HvfExecutor` besitzt eine VM pro Prozess und eine vCPU. Ein dedizierter Thread erstellt, verwendet und zerstört beide. Logische CPUs teilen den Executor und führen native Eintritte nacheinander aus. Beim Wechsel werden zuerst die zwei physischen Regionen des bisherigen Besitzers ausgehängt. Ein inaktiver CPU-Destruktor verändert keine fremden Mappings. Bindungen werden vor der RAM-Freigabe synchron gelöst; teilweise fehlgeschlagene Registrierungen werden zurückgerollt. Schlägt das Aushängen fehl, wird die VM vor der Speicherfreigabe beendet. Ein nicht behebbarer Abbaufehler stoppt den Prozess.

Host-Mappings verwenden die Host-Seitengröße, einschließlich 16 KiB auf Apple Silicon. Architekturseitentabellen und CPU-Gastbudget bleiben bei 4 KiB. Instruktionszulassung, Rechte, CPU-Zustand, Speichertransaktionen und OS-Dienste bleiben in den zuständigen Schichten. Der native Worker ruft keine Gastbeobachter auf und nimmt keine zentralen Speichersperren des Aufrufers.

ARM64 führt fünf unveränderliche TLB/I-Cache-Wartungsinstruktionen einzeln aus, anschließend die zugelassene Gastinstruktion. `PSTATE.D` maskiert keine nach EL2 geleiteten Debug-Ausnahmen. Skalare Register, TLS und FP/SIMD werden vollständig erfasst. Intel verhandelt VMCS-Steuerungen, nutzt Monitor-Trap-Schritte, TLB-Invalidierung und vollständige XSAVE-Pakete. RIP/RFLAGS werden direkt über VMCS übertragen, auch nach Neuerstellung der vCPU. CR0/CR4 beachten Framework-Masken und feste Hardwarebits. Authentifizierte CR8-Leseausstiege werden in der ISA-Schicht abgeschlossen; andere Steuerregisterzugriffe scheitern. Jede vCPU erhält einen privaten verwalteten `IA32_KERNEL_GS_BASE`; Gast-MSR-Zugriffe bleiben abgefangen und nicht unterstützte MSR/SWAPGS-Instruktionen unzulässig.

Warteschlangenzulassung und Ausführung verwenden denselben Stop-Token und die ursprüngliche Frist. Vorbereitung, Wartung, Eintritt und Zustandserfassung teilen ein Zeitbudget. `RunDeadline` wartet vor der Rückkehr auf die Interrupt-Bestätigung. Nach Abbruch wird die vCPU erneuert, damit verspätete Interrupts den nächsten Auftrag nicht treffen. Unbeteiligte Intel-Host-Interrupts setzen dieselbe Abbruchgeneration fort. Erfassungsfehler und authentifizierte Ausnahmen behalten Vorrang vor gleichzeitigem Stop; gewöhnlicher abgebrochener Zustand wird nicht veröffentlicht. Das ist kooperativer Abbruch, keine harte Echtzeitgarantie.

## Überprüfung

Verwenden Sie Release mit CMake, Ninja, Python 3, Clang, `ld.lld`, `lld-link`, `ld64.lld` und `codesign`. Die LLVM-Linker erzeugen ELF-, PE- und Mach-O-Testdateien. Fehlende obligatorische native Testdateien führen zum Fehlschlag.

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

Apple Silicon kann `-DNEVERD_LLVM_PREBUILT=ON` ergänzen; Intel baut die festgelegte LLVM-Revision aus dem Quellcode. Der [HVF-Workflow](../../.github/workflows/hvf.yml) unterstützt `self-hosted, macOS, ARM64/X64, hvf` sowie `hosted-intel` mit `macos-15-intel`. Zuerst müssen VM/vCPU tatsächlich erstellt und beendet werden. `validation=probe` beweist noch keine Instruktionsausführung; `transport` prüft nur den Transport, `darwin` alle passenden Darwin-Lasten und `full` die vollständigen CPU- und Darwin-Prüfungen.

Die Transportprüfung verlangt 12 ARM64- oder 10 Intel-Fälle, die vollständige CPU-Prüfung 16 beziehungsweise 14 Pflichtfälle. Abgedeckt sind vollständiger Zustand, Privilegien, Speicherrechte, Seitengrenzen, Aliase, CPU-Wechsel, Rollback, Abbruch und Wiederaufnahme. Intel prüft CR8 vor dem großen Build. Inventar, Quellrevision, Host, Ergebnisse und Wiederholungsversuche bleiben als Artefakte erhalten. [GitHub](https://docs.github.com/en/actions/concepts/runners/github-hosted-runners) bezeichnet verschachtelte Virtualisierung als experimentell; für reproduzierbare Abnahme bleibt ein dedizierter nativer Mac sinnvoll.

Auf gehostetem Intel führt `--execution-methods` jede GoogleTest-Methode mit sämtlichen CTest-Parametern, Flags, Umgebungsvariablen und Arbeitsverzeichnis seriell aus. Unbekannte Eigenschaften werden abgewiesen. Die Gesamtfrist pro Methode beträgt höchstens 120 Sekunden; einzelne Parameter haben innerhalb von GoogleTest keine separate Frist. Danach wird die Prozessgruppe zeitlich begrenzt beendet. Original-XML, Namenszuordnung und Exitstatus werden gespeichert. Timeout oder unvollständiges XML ergeben einen Teilfehler; fehlende oder übersprungene native Pflichtfälle können nicht bestehen. Eigene Runner verwenden weiterhin CTest-Prozesse und Fristen pro Testfall.

## Belege und Grenzen

Stand 2026-10-03; die Zeilen überschneiden sich und dürfen nicht addiert werden:

| Umfang | Quelle | Bestanden | Fehler | Übersprungen | Native Pflichtfälle |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 CPU, alle 20 Ziele | `defc93928` | 849 | 0 | 5,993 | 16/16 |
| ARM64 Darwin | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel Darwin | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| Vollständiges Intel-FP-Ziel | `3e01cda5c` | 35 | 0 | 36 | 12 HVF-Fälle |

ARM64 glich 6,842 registrierte Fälle ab; Intel hat 6,840 in denselben 20 Zielen. Zusätzlich bestanden alle 253 nativen Intel-Ausnahme-, Divisions- und Zustandsfälle. Der [Intel-Darwin-Lauf](https://github.com/NeverSight/NeverD/actions/runs/37106013999) prüfte 286 Identitäten und 32 Prozesse sowie zehn Transportfälle, 100 Wiederanläufe und CR8. Sammler und Audits bestanden 124 Prüfungen. CLI, SDKs, Worker und die Signaturen von 186 Mach-O-Dateien wurden integriert geprüft; die Paketabhängigkeiten verlangen macOS 15.0.

Die [vollständige Intel-CPU-Abnahme](https://github.com/NeverSight/NeverD/actions/runs/37106679688) bleibt offen: am 2026-10-03 um 08:24 UTC fehlten nach der konfigurierten Frist Endergebnis und CPU-Artefakt. Build und Vorprüfungen ersetzen diese Abnahme nicht. Ein macOS-Kernelvergleich belegt keinen iOS-Gerätekernel.

Der vergleichbare kurze ARM64-Test benötigte 73.9 ms mit Unicorn und 95.1 ms mit HVF, also etwa 29 % mehr Zeit. Ein Geschwindigkeitsgewinn ist nicht belegt. Normale Instruktionen verursachen sechs native Eintritte; Messungen unter hoher Hostlast sind keine stabilen Durchsatzbelege. [Detaillierte Nachweise](../macos-hvf.md#implementation-validation-2026-10-02-to-2026-10-03) und der [begrenzte Darwin-Vertrag](darwin-emulation.md) beschreiben den genauen Umfang.

## Vollständiges Intel-Inventar in Teilmengen

Der frühere vollständige Lauf `37106679688` endete am 2026-10-03 um 08:35 UTC; GitHub meldete den Verlust der Runner-Kommunikation. Es liegen keine CPU-Ergebnisse vor, also kein Nachweis für einen bestimmten fehlerhaften Gastbefehl. Hosted Intel `full` nutzt jetzt vier Shards mit höchstens zwei gleichzeitigen Jobs. Jeder Job baut und prüft zuerst das vollständige CTest-Inventar der zwanzig Ziele. `--hvf-shard INDEX/COUNT` verteilt ganze Methoden anhand von Ziel und Identität; sämtliche Parameter bleiben zusammen, auch bei unterschiedlichen Ausführungseigenschaften. Jeder Shard bewahrt vollständiges und ausgewähltes Inventar, Ablaufplan, Original-XML, Identitätszuordnungen, Prozessstatus und die Positivliste notwendiger Umgebungsvariablen. Ein Shard ist nur ein Teilnachweis.

Ein separater Linux-Job führt `scripts/audit_hvf_shards.py` aus und leitet Ziele und native Pflichtfälle erneut aus dem ausgecheckten Quellstand ab. Er verlangt denselben Versuch, denselben unveränderten Commit, die richtige macOS-Host-ISA und übereinstimmende normalisierte Ausführungsverträge. Die vier Shards müssen disjunkt sein und gemeinsam exakt das vollständige Inventar ergeben; alle Kindprozesse müssen erfolgreich enden und alle nativen Pflichtfälle bestehen. Fehlende Shards, geänderte Filter, widersprüchliche Zusammenfassungen, unvollständiges XML oder übersprungene Pflichtfälle scheitern. Jeder native Job behält Transport, Wiederaufnahme, CR8 und die unabhängige Darwin-Prüfung. Eigene Runner nutzen weiterhin ungeteiltes CTest. Die Aufteilung allein belegt keine Intel-Abnahme. Bei einem erneuten Versuch müssen alle nativen Jobs erneut laufen, damit die vier CPU-Artefakte zum selben Versuch gehören; Artefakte früherer Versuche werden nicht kombiniert.

## Neueste lokale native Validierung

Unveränderter Quellstand `26f8fa88b`, 2026-10-03. Alle vier ARM64-Shards bestanden die Gesamtprüfung; auch die separate Darwin-Prüfung bestand. Die Zeilen überlappen und dürfen nicht addiert werden.

| Umfang | Registriert | Bestanden | Fehler | Übersprungen | Native Pflichtfälle |
| --- | ---: | ---: | ---: | ---: | ---: |
| CPU, vier Shards | 6,883 | 854 | 0 | 6,029 | 16/16 |
| Darwin | 286 | 65 | 0 | 221 | 39/39 |

`build-hvf-native/hvf-shards-26f8fa88b/aggregate.json` · `build-hvf-native/hvf-shards-26f8fa88b/darwin/summary.json`
