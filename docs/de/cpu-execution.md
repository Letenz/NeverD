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

`driver-strict` akzeptiert x64; `software-cpu-v1` x64 und ARM64. `checked-x64-v1` und `checked-aarch64-v1` verlangen die jeweilige Architektur und Supervisor-Privileg. `checked-user-x64-v1` und `checked-user-aarch64-v1` führen den begrenzten, vertragsabhängigen Befehlsumfang mit MMU-Isolation bei CPL3 beziehungsweise EL0 und expliziten Service-Request-Exits aus. Unterstützt werden Unicorn sowie ein zum Host passendes KVM/WHP; `auto` folgt der Hostauswahl. Flache Profile versprechen keine architektonische User-/Supervisor-Isolation. Das geprüfte ARM64-Profil lehnt FP/SIMD ab; x64 unterstützt die unten genannten begrenzten Familien. Supervisor-x64 ergänzt begrenzte MMIO-Transaktionen und vorbereitete String-Lesezugriffe; User-Profile lehnen Gerätemappings ab. Port-I/O und parallele CPU-Anforderungen bleiben in allen geprüften Profilen ausgeschlossen. Nur die User-Profile melden `service_traps`.

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

Das Schema hat Version 1. Der Bericht trennt `requested_configuration` (vor Profilvorgaben), normalisierte `configuration`, statische semantische `capabilities`, Adapter-Build/ABI unter `build` und `host` (null ohne `--probe-host`). Eine Host-Abfrage initialisiert eine temporäre CPU über privatem RAM und belegt nur die Initialisierbarkeit, nicht die Eignung eines Workloads oder native ARM64-Ausführung. Verfügbarkeit kann sich ändern; nicht verfügbare Backends werden nie stillschweigend ersetzt. CLI liefert für einen gültigen Bericht einschließlich eines nicht verfügbaren Backends 0, für ungültige Konfiguration/Abfrage 1.

## SDK- und C++-Grenzen

[`neverd_cpu_capabilities_json`](../../include/neverd/sdk/NeverDCAPICPU.h) akzeptiert eine Sitzung, optionales Konfigurations-JSON und `ProbeHost` 0 oder 1; ein geladenes Binary ist nicht erforderlich. Ergebnis mit `neverd_free_string` freigeben; NULL signalisiert einen Fehler, dessen Text `neverd_last_error` liefert. CPU-deaktivierte Builds behalten die Funktion und melden die Deaktivierung. Python-Plugins fragen über `session.cpu_capabilities(...)` ab. C++ kann `executionCapabilities`, `resolveExecutionConfiguration`, `queryExecutionBackendBuild` und `probeExecutionBackend` getrennt aufrufen; `createExecutionBackend` bindet eine CPU an einen bestehenden Adressraum oder legt privaten RAM samt Standardraum an.

## CPU-Ergebnisse und Budgets

`CPU.runUntilExit(PC, TimeoutMicroseconds)` liefert ein typisiertes [`ExecutionExit`](../../include/neverd/emulation/ExecutionExit.h). Setupfehler ergeben `llvm::Error`; gestartete Läufe melden explizit Stop, Deadline, Service-Request, wiederherstellbaren Fehler, Gastfehler/-trap, nicht unterstützte Operation, Geräte-/Backendfehler oder unerklärten Engine-Stopp. CPU-/Geräte-/Backendfehler haben Vorrang vor gleichzeitigem Stop oder Deadline; beide unabhängigen Fakten und Fehlerdetails bleiben erhalten. Das Zeitlimit muss positiv sein und sowohl Dauer als auch absolute Deadline darstellen können; andernfalls schlägt der Aufruf vor Änderungen am CPU-Zustand fehl. Jede Ausführung braucht ein endliches Budget; Null bedeutet weder unbegrenzte Ausführung noch einen gültigen Sofort-Timeout. Die Kontrolle ist kooperativ, keine harte Echtzeitgrenze. Wiederherstellbare Fehler werden durch das Ergebnis nicht verbraucht; der OS-Besitzer muss sie übernehmen und einen validierten Ausnahmeübergang installieren. Die alten APIs `run`, `fault` und `timedOut` bleiben bestehen. Externe CPU-Implementierungen, die nur `run` überschreiben, lehnen die neue typisierte Grenze ab.

## Service-Requests

Geprüftes User-x64 fängt nur die genaue, unpräfixierte `SYSCALL`-Kodierung ab; User-ARM64 fängt `SVC #imm16` ab. `SYSENTER`, `INT`, `HVC`, `BRK` und andere Wege bleiben nicht unterstützt. Zuerst läuft der Instruktionsbeobachter. Ohne Stop/Fehler gibt der CPU `ExecutionExitKind::ServiceRequest` **vor** Ausführung der Instruktion oder Eintritt ins Backend zurück, mit Typ, ursprünglichem `PC`, sequenziellem `NextPC` und SVC-Immediate. Register, Flags, Stack und Privileg bleiben unverändert: insbesondere gibt es noch keine RCX/R11-SYSCALL-Clobber und keinen ARM64-Ausnahmevektor. Das SVC-Immediate ist kein universeller Dienstnummernwert.

Die Anfrage bleibt anhängig und blockiert Ausführung, CPU-Mutation, Adressraumwechsel sowie Kontextaufnahme/-wiederherstellung, bis der OS-Besitzer sie im angehaltenen Zustand genau einmal mit `takeServiceRequest()` übernimmt. Der Besitzer dekodiert die OS-ABI, behandelt den Dienst und setzt Ergebnisregister sowie nächsten PC/Ausnahmeübergang explizit. Nicht unterstützte Dienste schlagen dort fehl; erneut am ursprünglichen PC starten erzeugt eine neue Anfrage. Es gibt kein implizites NOP oder erfundenes erfolgreiches Ergebnis. Ein Service-Event hat Vorrang vor Stop/Deadline, Gast- oder Backendfehler haben höhere Priorität. CPU-Stopp, Software-HLT, Deadline oder Gast-Trap beweisen keinen erfolgreichen Workload. Das separate [Linux-Prozessprofil](process-emulation.md) modelliert eigene OS-Dienste und belegt keine Windows-, Android- oder Darwin-Laufzeit. Native Windows- und ARM64-Läufe benötigen weiterhin Laufzeitvalidierung.

## x64-Erweiterungen und nativer CPU-Zustand

Geprüftes x64 unterstützt begrenzte Legacy-SSE/SSE2-Moves und Logik, `MOVLHPS`/`MOVHLPS` sowie maskierte skalare `CVTTSS2SI`/`CVTTSD2SI`/`SUBSS`/`SUBSD`-Formen. MXCSR erhält Sticky-Status, Rundung und FTZ; DAZ und nicht maskierte Ausnahmen werden abgelehnt. Geprüftes ARM64 lehnt FP/SIMD weiterhin ab. KVM/WHP synchronisieren alle 16 XMM-Register und MXCSR; nicht gelistete Codierungen und Operanden bleiben ausgeschlossen.

Der Thread-Pointer umfasst x64-FS/GS-Basis und ARM64-`TPIDR_EL0` mit den exakten `MRS`/`MSR`-Codierungen. Native Transporte und CPU-Snapshots erhalten diesen Zustand getrennt vom Speicher; dadurch entstehen weder OS-Threads noch TLS-Blöcke. Supervisor-x64 unterstützt ausgerichtete skalare MMIO-Transaktionen mit 1/2/4 Byte und ein MOVS-Element pro Restart-Grenze. Gerätequellen brauchen einen reinen Prepared-Read mit höchstens einmaligem Commit. Gerätemappings bleiben für User-Profile ausgeschlossen; RMW, breites MMIO und Port-I/O ebenfalls.

KVM und WHP brechen aktive native Eintritte ab und bestätigen den Abbruch, bevor Ausführungsressourcen freigegeben werden. KVM nutzt einen privaten Ausführungsthread und eine temporär entsperrte Realtime-Signalmaske; das gewählte Signal darf während des Eintritts nicht ignoriert werden. Caller-Masken und -Handler bleiben unverändert. Bei ungewissem Gastfortschritt ist Abbruch terminal; eine harte Echtzeitfrist wird nicht garantiert.

## Native synchrone x64-Ausnahmen

Checked x64 führt `DIV`/`IDIV` mit echten Prozessorergebnissen und `#DE` aus. KVM nutzt eine private Supervisor-IDT/IST, WHP eine explizite Ausnahme-Bitmap; ursprünglicher Kontext und verfügbare Fehlercodes bleiben von Transportfehlern getrennt. Das OS konsumiert das wiederaufnehmbare Ereignis vor dem Setzen einer Fortsetzung. Windows-Treiber behandeln Nulldivision und Quotientenüberlauf als `STATUS_INTEGER_DIVIDE_BY_ZERO`, mit echten SEH-Filtern, `__finally` und Wiederholung. `NeverDX64ExceptionTests` baut ohne Unicorn; `DriverWDMCPUException` prüft originale WDK-Fälle. Nicht verfügbare WHP/ARM64-Hosts werden explizit übersprungen.
