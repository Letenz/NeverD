**Sprachen**: [English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← Dokumentationsindex](README.md)

# Gastprozess-Emulation

`neverd emulate` führt ein Image unter einem expliziten Gastbetriebssystemprofil aus. CPU-Transport, Image-Parsing, Prozesseinstieg und OS-Dienste haben getrennte Zuständigkeiten. `NEVERD_ENABLE_CPU_EMULATION=ON` aktiviert die Funktion; Treiberemulation schließt sie ebenfalls ein.

Das erste Profil `linux-elf64-v1` führt x64-/AArch64-ELF-`ET_EXEC` sowie selbstrelokierende statische PIE-`ET_DYN` auf CPL3 beziehungsweise EL0 aus. Es lädt echte ELF-Segmente, erstellt den initialen Stack, setzt die Ausführung in begrenzten Quanten fort und verarbeitet explizite Linux-Systemaufrufe. Dies ist ein freistehendes Prozessmodell, keine vollständige Linux-Distribution und keine Zusage für beliebige libc-Binaries. Dynamisches Linken, Signale, Threads, Dateisysteme und nicht unterstützte Dienste schlagen ausdrücklich fehl. Der begrenzte x64-Umfang enthält einige SSE/SSE2-Formen; AArch64 bleibt ein Integer-Profil. Windows-, Android-, Darwin- und andere Kernel-Workloads sind separat.

## CLI und SDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

Passende Linux-Hosts wählen KVM, passende Windows-Hosts WHP; andere Host-/Gast-ISA-Kombinationen verwenden Unicorn. Ein nicht verfügbares ausgewähltes Backend ist ein Fehler ohne stillen Fallback. Auch unter Windows bleibt das Gastmodell Linux. Die [CPU-Ausführung](cpu-execution.md) beschreibt Befehlsumfang und Grenzen.

Das CLI schreibt genau einen JSON-Bericht. Rückgabecode 0 bedeutet Gaststatus null, 2 einen anderen Gaststatus, 3 unvollständige Ausführung (einschließlich Fehlern und Limits) und 1 ungültige Einrichtung/API. Der tatsächliche Gaststatus steht in `exit_status`. Der additive C-Einstieg [`neverd_emulate_process_json`](../../include/neverd/sdk/NeverDCAPIProcess.h) erwartet Sitzung, nicht leeren Eingabepfad, explizites Profil und optionale JSON-Optionen. Ergebnis mit `neverd_free_string` freigeben; NULL bedeutet Setupfehler und `neverd_last_error` liefert Details. Gastfehler oder Ressourcenstopps ergeben trotzdem einen Bericht. Ein geladenes Analyse-Image wird weder benötigt noch verändert.

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

## Optionen und Ergebnisse

Optionen sind ein JSON-Objekt bis 64 KiB. Unbekannte/null-Felder, falsche Typen, eingebettete NULs und nichtpositive Limits werden abgelehnt.

| Option | Standard | Vertrag |
|---|---|---|
| `backend` | `auto` | `auto`, `unicorn`, `kvm` oder `whp` |
| `arguments` | Eingabedateiname | Vollständiges argv einschließlich argv[0]; leer wählt den Standard |
| `environment` | `[]` | Explizite Gaststrings; Hostumgebung wird nie übernommen |
| `instruction_limit` | 100000 | Gemeinsame Zahl zugelassener Instruktionsversuche |
| `event_limit` | 10000 | Systemaufrufereignisse, vor der OS-Behandlung belastet |
| `timeout_microseconds` | 5000000 | Monotone Deadline ab Ende des Prozess-Setups |
| `memory_limit` | 67108864 | Budget für physischen/gemappten Speicher |
| `stack_size` | 1048576 | Seitenausgerichteter Stack innerhalb des Budgets |
| `output_limit` | 1048576 | Zusammengefasste stdout/stderr-Bytes |
| `instruction_quantum` | 1024 | Zulassungsintervall bis zur Rückgabe an die Runtime |

`schema_version` ist 1. Der Bericht enthält Profil, Architektur, ausgewähltes Backend samt Grund, `stop_reason`, nullable `exit_status`, Diagnose, Ein-/aktuellen PC, Zähler, Service-Aufzeichnungen und letzten typisierten CPU-Ausgang. Adressen, syscall-Nummern, Registerargumente und rohe Rückgabebits sind Hex-Strings **ohne** `0x`; `stdout_hex`/`stderr_hex` erhalten NUL und ungültiges UTF-8. Ein null syscall-Ergebnis bedeutet keine modellierte Rückgabe (etwa Exit oder nicht unterstützte Anfrage), nicht erfolgreiche Null.

## Semantik des Linux-Profils

Die OS-Policy verwendet dekodierte Program-Header des vorhandenen ELF-Loaders. Sie prüft ABI-Tags, Segmentausrichtung, gemappte Program-Header-Tabellen und User-Adressgrenzen. Ein Mapping-Plan prüft Bereiche, Rechte, Überlappungen und Budget vor der Allokation und veröffentlicht nur einen vollständig vorbereiteten privaten Adressraum. Dateiseiten-Präfix/-Reste bleiben erhalten, BSS wird genullt, Segmentrechte werden beachtet und Stack-Guard-Lücken reserviert. Überlappende Seitenlayouts und widersprüchliche Header werden abgelehnt statt erraten.

Der initiale Stack enthält ausgerichtetes argc/argv/envp/auxv sowie PHDR/PHENT/PHNUM, Entry, Seitengröße und Identitätswerte. Modellierte PID/TID/UID/GID sind 1000. `AT_RANDOM` enthält für reproduzierbare Ausführung die ersten 16 Bytes des Eingabe-SHA-256; das ist eine deterministische Modellpolicy, keine kryptografische Entropie. HWCAP/HWCAP2 sind null; ein vDSO gibt es nicht.

Implementiert sind `write`, `exit`, `exit_group`, `getpid` und `gettid`, mit getrennten Nummern für [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) und [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h). Ein zurückkehrendes x64-SYSCALL setzt RCX/R11-Clobber, RAX und den nächsten PC. ARM64 nutzt x8 als Nummer und x0 als Ergebnis. Unbekannte Aufrufe stoppen als `unsupported_service`; Host-system calls werden nie ausgeführt.

Deskriptor 1 und 2 sind virtuelle Byte-Senken. `write` prüft lesbare User-Seiten, liefert bei später unzugänglicher Seite das lesbare Präfix und `EFAULT`, wenn kein Byte lesbar ist. Ein falscher Deskriptor ergibt `EBADF`; ein gültiger Null-Byte-write greift nicht auf den Zeiger zu. Linux-Pipe-Atomicität oder Dateien werden nicht modelliert. Das Ausgabelimit stoppt vor Veröffentlichung eines zu großen Schreibvorgangs.

## Verifikation

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# In einem Shared-Library-/CLI-Build:
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

Tests kompilieren Original-ELF-Entry-Assembly und C für beide ISAs und prüfen Daten/BSS, Startup-Metadaten, syscall-Fehler, Binärausgabe, Rechte, Teil-Schreibvorgänge, nicht unterstützte Dienste und über Quanten erhaltene Budgets. Nicht verfügbare Backends werden ausdrücklich übersprungen. Die öffentliche Suite prüft C-ABI/CLI und Bericht-/Exit-Code-Übereinstimmung. Cross-Compilation und Unicorn-ARM64 sind kein Nachweis für natives ARM64-KVM/WHP.

## Statische PIEs, TLS und Verifikation

Statische PIEs erhalten einen deterministischen Load Bias von mindestens `0x40000000`, erhöht für größere `PT_LOAD`-Ausrichtung. Gemappte Segmente, Eintritts-PC und `AT_PHDR`/`AT_ENTRY` verwenden denselben Bias; ursprüngliche Program-Header-Werte bleiben unverändert und `AT_BASE` bleibt ohne Interpreter null. Gemappt werden Originaldateibytes, keine Analyse-Pointer-Fixups; der Gast muss Relokationen und Initialisierung selbst ausführen. Der Loader dekodiert `PT_DYNAMIC` aus begrenzten Originaldatei-Datensätzen, unabhängig von Section-Headers. Falls vorhanden, muss die Tabelle lesbar, terminiert und höchstens 4096 Einträge lang sein. `PT_INTERP` sowie externe Abhängigkeits-, Filter- und Audit-Tags werden abgelehnt; es gibt keinen dynamischen Linker, Symbolauflöser oder Konstruktorlauf.

Statische `PT_TLS`-Vorlagen werden als Loader-Fakten validiert: genau eine Vorlage, begrenzte Datei-/Speicherbereiche, passende Ausrichtung und lesbare initialisierte Bytes. Der Gaststart allokiert und initialisiert TLS-Blöcke und setzt den Thread-Pointer; das Linux-Modell erfindet kein libc-spezifisches TCB oder DTV. Compiler-erzeugtes local-exec TLS in freestanding Programmen wird damit unterstützt. Dynamisches TLS und OS-Threads bleiben außerhalb des Umfangs.

Auf x64 unterstützt `arch_prctl` `ARCH_SET_FS`, `ARCH_GET_FS`, `ARCH_SET_GS` und `ARCH_GET_GS`. Set akzeptiert eine User-Basis auch ohne Mapping; spätere Dereferenzierungen prüfen weiterhin Rechte. Kernel-Basen liefern Gast-`EPERM`, ungültige Get-Ziele Gast-`EFAULT`, ohne CPU-Fault. Andere Operationen schlagen ausdrücklich fehl. ARM64 installiert `TPIDR_EL0` mit `MSR`; `MRS`, FS/GS-Speicherzugriffe und Kontextwiederherstellung erhalten Thread-Pointer über Quanten und Backend-Eintritte. Das implementiert keinen Scheduler.

TLS-/PIE-Fixtures prüfen unabhängige ausgerichtete Blöcke, TLS-BSS, verlagerte Auxv-Werte und ursprüngliche Null-RELA-Slots vor den eigenen Relokationen des Gasts. x64-Tests prüfen `arch_prctl`-Fehler ohne Verlust der bisherigen Basis. `NeverDThreadPointerTests` gehört zum Build-/CTest-Aufruf oben; nicht verfügbare Backends bleiben explizite Skips.
