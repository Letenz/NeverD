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

`driver-strict` akzeptiert x64; `software-cpu-v1` x64 und ARM64. `checked-x64-v1` und `checked-aarch64-v1` verlangen die jeweilige Architektur und Supervisor-Privileg. `checked-user-x64-v1` und `checked-user-aarch64-v1` führen denselben begrenzten skalaren Befehlsumfang mit MMU-Isolation bei CPL3 beziehungsweise EL0 und expliziten Service-Request-Exits aus. Unterstützt werden Unicorn sowie ein zum Host passendes KVM/WHP; `auto` folgt der Hostauswahl. Flache Profile versprechen keine architektonische User-/Supervisor-Isolation. Alle geprüften Profile lehnen FP/SIMD, MMIO, Port-I/O und parallele CPU-Anforderungen ab. Nur die User-Profile melden `service_traps`.

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
