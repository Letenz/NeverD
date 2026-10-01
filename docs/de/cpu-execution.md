**Sprachen**: [English](../cpu-execution.md) | [简体中文](../zh-CN/cpu-execution.md) | [繁體中文](../zh-TW/cpu-execution.md) | [日本語](../ja/cpu-execution.md) | [한국어](../ko/cpu-execution.md) | [Français](../fr/cpu-execution.md) | [Deutsch](cpu-execution.md) | [Español](../es/cpu-execution.md) | [Italiano](../it/cpu-execution.md) | [Русский](../ru/cpu-execution.md) | [العربية](../ar/cpu-execution.md)

[← Dokumentationsindex](README.md)

# CPU-Ausführung und Fähigkeitsabfragen

CPU-Ausführung ist unabhängig von Gastbetriebssystem, Image-Loader und Aufrufkonvention. Mit `NEVERD_ENABLE_CPU_EMULATION` lässt sie sich allein bauen; `NEVERD_ENABLE_DRIVER_EMULATION` schließt zusätzlich das Windows-Treibermodell ein. Der [Architekturleitfaden](architecture.md) beschreibt Zuständigkeiten, Backend-Auswahl und Plattformgrenzen.

## Konfiguration

Die öffentliche [`ExecutionConfiguration`](../../include/neverd/emulation/ExecutionConfiguration.h) wird vom CPU-Factory-Code und der Fähigkeitsabfrage gemeinsam verwendet. Anforderungen werden vor CPU-Allokation oder Adressraumzuordnung geprüft. Weggelassene Werte nehmen das feste Vertragsprofil; explizit nicht unterstützte Werte schlagen fehl.

| JSON-Feld | Standard | Bedeutung |
|---|---|---|
| `backend` | `auto` | `auto`, `unicorn`, `kvm` oder `whp` |
| `contract` | `software-cpu-v1` | Versionierte Ausführungssemantik |
| `architecture` | `x86_64` | `x86_64` oder `aarch64` |
| `privilege` | Vertragsprofil | `flat`, `supervisor` oder `user`; muss zum Vertrag passen |
| `virtual_address_bits` | Vertragsprofil | Geprüfte Profile: 48 Bit; flache Profile: direkter 64-Bit-Mapping-Namensraum |
| `page_size` | 4096 | Gast-Mapping-Größe; andere Werte werden abgelehnt |
| `required_features` | `[]` | Erforderliche Namen aus [`ExecutionConfiguration.def`](../../include/neverd/emulation/ExecutionConfiguration.def) |

`driver-strict` akzeptiert x64; `software-cpu-v1` x64 und ARM64. `checked-x64-v1` und `checked-aarch64-v1` verlangen die jeweilige Architektur und Supervisor-Privileg. `checked-user-x64-v1` und `checked-user-aarch64-v1` führen den begrenzten, vertragsabhängigen Befehlsumfang mit MMU-Isolation bei CPL3 beziehungsweise EL0 und expliziten Service-Request-Exits aus. Unterstützt werden Unicorn sowie ein zum Host passendes KVM/WHP; `auto` folgt der Hostauswahl. Flache Profile versprechen keine architektonische User-/Supervisor-Isolation. Die geprüften ARM64- und x64-Profile unterstützen die unten genannten begrenzten FP/SIMD-Familien. Supervisor-x64 ergänzt begrenzte MMIO-Transaktionen und vorbereitete String-Lesezugriffe; User-Profile lehnen Gerätemappings ab. Port-I/O und parallele CPU-Anforderungen bleiben in allen geprüften Profilen ausgeschlossen. Nur die User-Profile melden `service_traps`.

User-Ausführung benötigt auf **jeder** gemappten Seite `UserAccessible` zusätzlich zum passenden Recht `Read`, `Write` oder `Execute`. Bestehende Mappings sind standardmäßig Supervisor-Seiten; Aliase behalten unabhängige Rechte, auch wenn sie dieselben physischen Bytes teilen. `UserAccessible` allein erlaubt keinen Zugriff. Vertrauenswürdige Hostzugriffe und Supervisor-CPUs verwenden RWX. Beispiel:

```cpp
Configuration.Contract = ExecutionContract::CheckedUserX64;
Configuration.Privilege = ExecutionPrivilege::User;
auto CPU = llvm::cantFail(createExecutionBackend(Configuration, Space)).CPU;
llvm::cantFail(CPU->map(Code, 4096, Read | Write | Execute | UserAccessible));
```

Das Privileg ist durch den Vertrag festgelegt; Kontextwiederherstellung oder Adressraumwechsel ändert es nicht, und x64-Segmentselektoren können es nicht erhöhen. Wiederherstellbare Datenzugriffsfehler behalten Instruktion und Register unverändert, bis der zuständige Besitzer sie behandelt. `canAccess` prüft genau die angeforderten Rechte; für User-Sichtbarkeit muss `UserAccessible` mitgeprüft werden. Seitentabellen sind private CPU-Projektionen, keine veränderbaren Gasttabellen oder Privilegwechsel-API. Bei ARM64 sind User-Seiten zusätzlich auf EL1 nicht ausführbar.

Unbekannte oder nullwertige Felder, ungültige Namen/Breiten, doppelte Features und nicht unterstützte Kombinationen schlagen fehl. Eingaben sind auf 64 KiB begrenzt; alte CPU-Factories und Driver-C-Optionen bleiben kompatibel.

## Abfragen ohne Workload-Ausführung

```bash
neverd cpu-capabilities
neverd cpu-capabilities \
  --configuration='{"contract":"checked-aarch64-v1","architecture":"aarch64"}' \
  --probe-host
```

Das Schema hat Version 1. Der Bericht trennt `requested_configuration` (vor Profilvorgaben), normalisierte `configuration`, statische semantische `capabilities`, Adapter-Build/ABI unter `build` und `host` (null ohne `--probe-host`). Eine Host-Abfrage initialisiert eine temporäre CPU über privatem RAM und belegt nur die Initialisierbarkeit, nicht die Kompatibilität beliebiger Workloads. Verfügbarkeit kann sich ändern; nicht verfügbare Backends werden nie stillschweigend ersetzt. CLI liefert für einen gültigen Bericht einschließlich eines nicht verfügbaren Backends 0, für ungültige Konfiguration/Abfrage 1.

## SDK- und C++-Grenzen

[`neverd_cpu_capabilities_json`](../../include/neverd/sdk/NeverDCAPICPU.h) akzeptiert eine Sitzung, optionales Konfigurations-JSON und `ProbeHost` 0 oder 1; ein geladenes Binary ist nicht erforderlich. Ergebnis mit `neverd_free_string` freigeben; NULL signalisiert einen Fehler, dessen Text `neverd_last_error` liefert. CPU-deaktivierte Builds behalten die Funktion und melden die Deaktivierung. Python-Plugins fragen über `session.cpu_capabilities(...)` ab. C++ kann `executionCapabilities`, `resolveExecutionConfiguration`, `queryExecutionBackendBuild` und `probeExecutionBackend` getrennt aufrufen; `createExecutionBackend` bindet eine CPU an einen bestehenden Adressraum oder legt privaten RAM samt Standardraum an.

## CPU-Ergebnisse und Budgets

`CPU.runUntilExit(PC, TimeoutMicroseconds)` liefert ein typisiertes [`ExecutionExit`](../../include/neverd/emulation/ExecutionExit.h). Setupfehler ergeben `llvm::Error`; gestartete Läufe melden explizit Stop, Deadline, Service-Request, wiederherstellbaren Fehler, Gastfehler/-trap, nicht unterstützte Operation, Geräte-/Backendfehler oder unerklärten Engine-Stopp. CPU-/Geräte-/Backendfehler haben Vorrang vor gleichzeitigem Stop oder Deadline; beide unabhängigen Fakten und Fehlerdetails bleiben erhalten. Das Zeitlimit muss positiv sein und sowohl Dauer als auch absolute Deadline darstellen können; andernfalls schlägt der Aufruf vor Änderungen am CPU-Zustand fehl. Jede Ausführung braucht ein endliches Budget; Null bedeutet weder unbegrenzte Ausführung noch einen gültigen Sofort-Timeout. Die Kontrolle ist kooperativ, keine harte Echtzeitgrenze. Wiederherstellbare Fehler werden durch das Ergebnis nicht verbraucht; der OS-Besitzer muss sie übernehmen und einen validierten Ausnahmeübergang installieren. Die alten APIs `run`, `fault` und `timedOut` bleiben bestehen. Externe CPU-Implementierungen, die nur `run` überschreiben, lehnen die neue typisierte Grenze ab.

## Service-Requests

Geprüftes User-x64 fängt nur die genaue, unpräfixierte `SYSCALL`-Kodierung ab; User-ARM64 fängt `SVC #imm16` ab. `SYSENTER`, `INT`, `HVC`, `BRK` und andere Wege bleiben nicht unterstützt. Zuerst läuft der Instruktionsbeobachter. Ohne Stop/Fehler gibt der CPU `ExecutionExitKind::ServiceRequest` **vor** Ausführung der Instruktion oder Eintritt ins Backend zurück, mit Typ, ursprünglichem `PC`, sequenziellem `NextPC` und SVC-Immediate. Register, Flags, Stack und Privileg bleiben unverändert: insbesondere gibt es noch keine RCX/R11-SYSCALL-Clobber und keinen ARM64-Ausnahmevektor. Das SVC-Immediate ist kein universeller Dienstnummernwert.

Die Anfrage bleibt anhängig und blockiert Ausführung, CPU-Mutation, Adressraumwechsel sowie Kontextaufnahme/-wiederherstellung, bis der OS-Besitzer sie im angehaltenen Zustand genau einmal mit `takeServiceRequest()` übernimmt. Der Besitzer dekodiert die OS-ABI, behandelt den Dienst und setzt Ergebnisregister sowie nächsten PC/Ausnahmeübergang explizit. Nicht unterstützte Dienste schlagen dort fehl; erneut am ursprünglichen PC starten erzeugt eine neue Anfrage. Es gibt kein implizites NOP oder erfundenes erfolgreiches Ergebnis. Ein Service-Event hat Vorrang vor Stop/Deadline, Gast- oder Backendfehler haben höhere Priorität. CPU-Stopp, Software-HLT, Deadline oder Gast-Trap beweisen keinen erfolgreichen Workload. Das separate [Linux-Prozessprofil](process-emulation.md) modelliert eigene OS-Dienste und belegt keine Windows-, Android- oder Darwin-Laufzeit. Native Windows- und ARM64-Läufe benötigen weiterhin Laufzeitvalidierung.

## x64-Erweiterungen und nativer CPU-Zustand

Geprüftes x64 unterstützt begrenzte Legacy-SSE/SSE2-Moves und Logik, `MOVLHPS`/`MOVHLPS` sowie maskierte skalare `CVTTSS2SI`/`CVTTSD2SI`/`SUBSS`/`SUBSD`-Formen. MXCSR erhält Sticky-Status, Rundung und FTZ; DAZ und nicht maskierte Ausnahmen werden abgelehnt. KVM/WHP synchronisieren alle 16 XMM-Register und MXCSR; nicht gelistete Codierungen und Operanden bleiben ausgeschlossen.

Geprüftes x64 erlaubt auch maskierte Legacy-Formen `SS`, `SD`, `PS`, `PD` von `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `MIN`, `MAX`. `X64SSEInstructions.def` verwaltet Breiten, Ausrichtung und Zulassung zentral. `MaskedSSEArithmeticMatchesIndependentHostExecution` vergleicht Register/RAM mit einem unabhängigen Host-CPU-Orakel: vier Rundungsmodi, FTZ, vorzeichenbehaftete Nullen, Subnormalzahlen und NaNs. `SSEMemoryObserverStopsBeforeResultAndStatusChanges` prüft den Stopp vor Effekten. DAZ, unmaskierte Ausnahmen, x87 und AVX bleiben ausgeschlossen.

Der Thread-Pointer umfasst x64-FS/GS-Basis und ARM64-`TPIDR_EL0` mit den exakten `MRS`/`MSR`-Codierungen. Native Transporte und CPU-Snapshots erhalten diesen Zustand getrennt vom Speicher; dadurch entstehen weder OS-Threads noch TLS-Blöcke. Supervisor-x64 unterstützt ausgerichtete skalare MMIO-Transaktionen mit 1/2/4 Byte und ein MOVS-Element pro Restart-Grenze. Gerätequellen brauchen einen reinen Prepared-Read mit höchstens einmaligem Commit. Gerätemappings bleiben für User-Profile ausgeschlossen; RMW, breites MMIO und Port-I/O ebenfalls.

KVM und WHP brechen aktive native Eintritte ab und bestätigen den Abbruch, bevor Ausführungsressourcen freigegeben werden. KVM nutzt einen privaten Ausführungsthread und eine temporär entsperrte Realtime-Signalmaske; das gewählte Signal darf während des Eintritts nicht ignoriert werden. Caller-Masken und -Handler bleiben unverändert. Bei ungewissem Gastfortschritt ist Abbruch terminal; eine harte Echtzeitfrist wird nicht garantiert.

## Native synchrone x64-Ausnahmen

Checked x64 führt `DIV`/`IDIV` mit echten Prozessorergebnissen und `#DE` aus. KVM nutzt eine private Supervisor-IDT/IST, WHP eine explizite Ausnahme-Bitmap; ursprünglicher Kontext und verfügbare Fehlercodes bleiben von Transportfehlern getrennt. Das OS konsumiert das wiederaufnehmbare Ereignis vor dem Setzen einer Fortsetzung. Windows-Treiber behandeln Nulldivision und Quotientenüberlauf als `STATUS_INTEGER_DIVIDE_BY_ZERO`, mit echten SEH-Filtern, `__finally` und Wiederholung. `NeverDX64ExceptionTests` baut ohne Unicorn; `DriverWDMCPUException` prüft originale WDK-Fälle. Nicht verfügbare WHP/ARM64-Hosts werden explizit übersprungen.

## Gestufte RAM-Effekte

`RAMTransaction` erfasst unter der physischen Ausführungslease nur die vereinigten deklarierten Schreibbereiche einer Instruktion. Vor Ergebnisbeobachtern wird der ursprüngliche RAM wiederhergestellt; Abbruch, Transportfehler und Beobachterausnahmen veröffentlichen weder Teilwrites noch Register. Prozessorfehler behalten nach RAM-Rollback ihren architektonischen Ausnahmestatus. ARM64-Einzel- und Paarstores verwenden dieselbe Instanz. x64 führt `XCHG`, `XADD` und `CMPXCHG` mit 8/16/32/64 Bit aus; gesperrte und implizit gesperrte Formen erfordern natürliche Ausrichtung. `NeverDRAMTransactionTests` vergleicht Ergebnisse mit der Host-CPU und prüft Rollback, Aliase und Rechte; fehlende Plattformen werden ausdrücklich übersprungen. Geräte und paralleles SMP bleiben ausgeschlossen. CPU-Snapshots setzen bereits bestätigten RAM nicht zurück.

## Vollständiger x87-Zustand

`NeverDEmulationArch` besitzt ISA-Verträge, Seitentabellen und das FP-Layout, das native Transporte und Unicorn gemeinsam nutzen. x64-Kontexte erhalten Steuerung, Status, TOP, physische Tags, Opcode, Befehls-/Datenzeiger und acht 80-Bit-Register. `FP0`–`FP7` verwenden `RegisterValue`; skalare Zugriffe lehnen eine Kürzung ab. `FPTag` ist die physische Maske nicht leerer Register. `NeverDX64FPTests` prüft alle TOP-Werte, exakte Operationen gegen Host-FXSAVE/FXRSTOR und die Wiederherstellung. Dies lässt keine x87-Befehle im checked-Vertrag zu und beweist nicht sämtliche Rundungssemantik. Fehlende native Hosts werden ausdrücklich übersprungen.

`driver-strict` unterstützt KVM auf passenden Linux-x64-Hosts und WHP auf passenden Windows-x64-Hosts; `auto` wählt diesen nativen Transport, unterschiedliche ISAs verwenden Unicorn. Explizites Unicorn und die bisherige V1-API behalten das portable Softwareprofil. Native Ausführung prüft kanonische Adressen und Effekte vor dem Eintritt; fehlende Hardware führt ohne Rückfall zum Fehler. Nicht unterstützte Instruktionen und OS-Verhalten bleiben explizite Fehler. Native ARM64/WHP-Nachweise fehlen weiterhin; allgemeine Treiber- oder Android/Darwin-Kompatibilität ist damit nicht belegt.

Das ausgewählte Profil lässt sich mit `executionCapabilities(Contract, ISA, Backend)` abfragen. `NativeLegacyX64` beschreibt die native Ausführung von x64-Treibern. `NeverDNativeDriverTests` prüft den vorhandenen Treiberkorpus und kann auch in einem Build ohne Unicorn laufen.

Geprüftes ARM64 besitzt eine gemeinsame Grenze für den vollständigen Zustand. `Registers.def` definiert 39 skalare Felder und 32 Vektoren mit 128 Bit; `captureAArch64State` sammelt alle Werte, wendet Bitbreiten an und normalisiert NZCV vor einer einzigen Veröffentlichung. Unicorn, KVM und WHP übertragen denselben Bestand einschließlich TPIDR_EL0, TPIDRRO_EL0, TPIDR_EL1, FPCR und FPSR. Native Adapter aktivieren FP/SIMD über CPACR_EL1. Fehlgeschlagene Lesevorgänge und abgebrochene Eintritte erhalten den gesamten Aufruferzustand.

Der ARM64-KVM/WHP-Start führt das private Programm `AArch64MachineProbe.def` aus: NOP, FP32-Addition mit Rundung gegen positive Unendlichkeit und SIMD-Addition auf zwei Spuren. Jeder Schritt vergleicht alle 39 skalaren Felder und 32 Vektoren, einschließlich TLS, NZCV, gelöschter oberer Ergebnisbits und erhaltenem/kumulativem FPCR/FPSR-Zustand. Die Probe verwendet nur Supervisor-Monitorspeicher und eine gemeinsame Gesamtfrist. Erfolg bestätigt dieses begrenzte Initialisierungsprogramm; unabhängige native ARM64-Workload-Validierung steht weiter aus.

`CheckedAArch64Instructions.def` und `AArch64InstructionEffects` erlauben in EL0/EL1 begrenzte Basis-FP32/FP64-Arithmetik, Vergleiche, Transfers und SIMD fester Breite. FPCR erhält vier Rundungsmodi, FZ und DN; FPSR erhält kumulative Zustände und QC. Nicht unterstützte Bits werden vor Änderungen abgelehnt. FP16-Arithmetik, SVE/SME, unmaskierte Ausnahmen, optionale Erweiterungen und nicht gelistete Formen schlagen ausdrücklich fehl. Windows-ARM64-Treiberladen und weitere OS-Umgebungen werden nicht hinzugefügt.

`AArch64InstructionEffects` besitzt skalare und FP/SIMD-RAM-Bereiche für einzelne und gepaarte Operanden bis 128 Bit. Der gemeinsame Adressraum prüft jede Seite vor Eintritt; `RAMTransaction` übernimmt nur vollständige deklarierte physische Schreibbereiche. Ein 128-Bit-Schreibbeobachter erhält vor Effekten zwei geordnete 64-Bit-Wörter. Stops und Fehler erhalten RAM, Vektoren und Writeback. Gleiche Xn/Vn-Nummern sind gültig; umlaufende Paarbereiche werden abgelehnt. `NeverDAArch64MemoryTests` verwendet unabhängige `AArch64CrossPageCases.def` und `AArch64VectorMemoryCases.def`.

KVM x64/ARM64 verwendet `KvmRunControl` für Vorbereitung, `KVM_RUN` und Zustandserfassung auf demselben privaten vCPU-Worker. Auch bei `EINTR` erfolgt die Vorbereitung einmal; Abbruch oder Lesefehler verhindern Veröffentlichung. `KvmAArch64Machine.cpp` führt Übersetzungspflege und vollständige Skalar-/Vektortransfers unter einer gemeinsamen Schrittfrist aus. Der Aufrufer veröffentlicht nach Bestätigung; ISA-Decodierung, RAM-Transaktionen, OS-Politik und Beobachter bleiben bei ihm. Native ARM64-Laufzeitnachweise stehen aus.

KVM vergleicht allgemeine Register und den vollständigen FP/SSE-Zustand anhand von `X64HostRegisters.def` und `X64FPState.def` mit der letzten bestätigten Debug-Erfassung und setzt geänderte Eingaben erneut. Host-Schreibzugriffe und Kontextwiederherstellung werden mitverglichen; Ausnahmen, Abbruch und Fehler verwerfen die Wiederverwendung. Einzelschrittsteuerung und Lesen des tatsächlichen allgemeinen/FP-Zustands erfolgen weiterhin für jede Instruktion.

Hardwareausführung allein garantiert keine geringere Gesamtlatenz. Die aktuelle native Ausführung führt Zulassungsprüfung, Beobachtung, Zustandsübertragung und VM-Austritt pro Instruktion aus. Vergleichen Sie dieselben ursprünglichen Images und Szenarien mit identischen Instruktions- und Ereignisbudgets und berichten Sie Ergebnisgleichheit zusammen mit Laufzeiten; zur CLI-Latenz gehören Start und Laden.

Geprüftes Unicorn verwendet `MachineRunControl`: Ein Zeitrahmen umfasst ARM64-Pflege, Gastausführung und vollständige Zustandserfassung. `UC_HOOK_CODE` prüft das geliehene Stopptoken und die Frist am Instruktionseingang. Der synchrone Engine-Aufruf gibt die Hook-Referenz vor seiner Rückkehr frei; der Maschinenschritt behält die Kontrolle bis zur Veröffentlichung. Unicorn und WHP erfassen den vollständigen CPU-Zustand zunächst privat und prüfen dieselbe Kontrolle vor Veröffentlichung eines erfolgreichen Schritts. WHP legt den Zeitrahmen einmal vor der Vorbereitung fest. Eine authentifizierte x64-CPU-Ausnahme hat Vorrang vor einem während der Erfassung eintreffenden Stop. Die geprüfte RAM-Transaktion verwirft bei abgebrochener Erfassung spekulative Schreibzugriffe; der uneingeschränkte Softwarevertrag bleibt unverändert. `MachineInterruptedError` trennt bestätigten Abbruch von Host- oder Erfassungsfehlern. Die gemeinsame geprüfte CPU liefert `Stopped` oder `Deadline`, erhält CPU/RAM und erlaubt einen erneuten Versuch; echte Fehler bleiben auch bei gleichzeitigem Stop `BackendFailure`.

`RunDeadline::invoke` weist einen bereits gestoppten oder abgelaufenen WHP-Eintritt vor dem Hostaufruf zurück, bewahrt bei Abbruch das tatsächliche Hostergebnis und bestätigt das Ende der Unterbrechungsrückrufe vor Freigabe des geliehenen Stopptokens. KVM und WHP prüfen das vollständig erfasste private Zustandspaket im aufrufenden Thread mit Ausführungsrecht, bevor sie einen gleichzeitigen Stopp oder Fristablauf einordnen. Tatsächliche Host- und Erfassungsfehler sowie authentifizierte x64-CPU-Ausnahmen behalten Vorrang. Ein gewöhnlicher Erfolgszustand bleibt bis zum Ende der Abbruchprüfung privat; eine bestätigte Unterbrechung verwirft spekulative CPU/RAM-Effekte und erlaubt einen neuen Versuch. Vorbereitung, native Ausführung und Erfassung teilen sich eine einzige Schrittfrist. Die Abbruchkontrolle ist kooperativ und garantiert keine harte Echtzeitgrenze.
