**Sprachen**: [English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← Dokumentationsindex](README.md)

# Gastprozess-Emulation

`neverd emulate` führt ein Image unter einem expliziten Gastbetriebssystemprofil aus. CPU-Transport, Image-Parsing, Prozesseinstieg und OS-Dienste haben getrennte Zuständigkeiten. `NEVERD_ENABLE_CPU_EMULATION=ON` aktiviert die Funktion; Treiberemulation schließt sie ebenfalls ein.

Das erste Profil `linux-elf64-v1` führt x64-/AArch64-ELF-`ET_EXEC` sowie selbstrelokierende statische PIE-`ET_DYN` auf CPL3 beziehungsweise EL0 aus. Es lädt echte ELF-Segmente, erstellt den initialen Stack, setzt die Ausführung in begrenzten Quanten fort und verarbeitet explizite Linux-Systemaufrufe. Dies ist ein freistehendes Prozessmodell, keine vollständige Linux-Distribution und keine Zusage für beliebige libc-Binaries. Dynamisches Linken, Signale, Threads, Dateisysteme und nicht unterstützte Dienste schlagen ausdrücklich fehl.

<!-- i18n-section: cli-sdk -->

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

<!-- i18n-section: options-results -->

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
| `linux_kernel` | Nicht angegeben | Expliziter GKI-Zweig, fester Gastaufgabenkatalog oder beobachtete fehlende Kernelschnittstellen |
| `linux_priority` | Nicht angegeben | Explizite Nice-Werte je Task und Aufruferrechte für rohe Linux-Prioritätsdienste |

`schema_version` ist 1. Der Bericht enthält Profil, Architektur, ausgewähltes Backend samt Grund, `stop_reason`, nullable `exit_status`, Diagnose, Ein-/aktuellen PC, Zähler, Service-Aufzeichnungen und letzten typisierten CPU-Ausgang. Adressen, syscall-Nummern, Registerargumente und rohe Rückgabebits sind Hex-Strings **ohne** `0x`; `stdout_hex`/`stderr_hex` erhalten NUL und ungültiges UTF-8. Ein null syscall-Ergebnis bedeutet keine modellierte Rückgabe (etwa Exit oder nicht unterstützte Anfrage), nicht erfolgreiche Null.

<!-- i18n-section: linux-semantics -->

## Semantik des Linux-Profils

`writev` nutzt diese Ausgaben auf x64/ARM64 und über Android Bionic. Vor der Ausgabe importiert es bis zu 1024 Gast-`iovec`-Einträge, lehnt negative Längen mit `EINVAL` ab, prüft die Benutzerbereiche und wendet die seitenorientierte Linux-Transfergrenze an. Ungültige Deskriptoren liefern vor dem Vektorzugriff `EBADF`; unlesbare Metadaten liefern `EFAULT` ohne Ausgabe. Ein späterer Datenfehler erhält das kopierte Präfix. Das Budget umfasst vor Veröffentlichung den ganzen Vektor und beide Streams. `write` und `writev` nutzen die unteren 32 Deskriptorbits; auch die Anzahl folgt dem 32-Bit-Import von Linux. Nur Bionic wandelt rohe negative Fehler in `-1` und `errno` um. `LinuxOutputNativeTests` führt zehn eigene Fälle auf Host-Linux mit regulären Dateiumleitungen aus; modellierte x64/ARM64-Fälle prüfen zusätzlich Budgets. Siehe den [Linux-Importvertrag](https://github.com/torvalds/linux/blob/v6.12/lib/iov_iter.c).

Die OS-Policy verwendet dekodierte Program-Header des vorhandenen ELF-Loaders. Sie prüft ABI-Tags, Segmentausrichtung, gemappte Program-Header-Tabellen und User-Adressgrenzen. Ein Mapping-Plan prüft Bereiche, Rechte, Überlappungen und Budget vor der Allokation und veröffentlicht nur einen vollständig vorbereiteten privaten Adressraum. Dateiseiten-Präfix/-Reste bleiben erhalten, BSS wird genullt, Segmentrechte werden beachtet und Stack-Guard-Lücken reserviert. Überlappende Seitenlayouts und widersprüchliche Header werden abgelehnt statt erraten.

Statische PIEs erhalten einen deterministischen Load Bias von mindestens `0x40000000`, erhöht für größere `PT_LOAD`-Ausrichtung. Gemappte Segmente, Eintritts-PC und `AT_PHDR`/`AT_ENTRY` verwenden denselben Bias; ursprüngliche Program-Header-Werte bleiben unverändert und `AT_BASE` bleibt ohne Interpreter null. Gemappt werden Originaldateibytes, keine Analyse-Pointer-Fixups; der Gast muss Relokationen und Initialisierung selbst ausführen. Der Loader dekodiert `PT_DYNAMIC` aus begrenzten Originaldatei-Datensätzen, unabhängig von Section-Headers. Falls vorhanden, muss die Tabelle lesbar, terminiert und höchstens 4096 Einträge lang sein. `PT_INTERP` sowie externe Abhängigkeits-, Filter- und Audit-Tags werden abgelehnt; es gibt keinen dynamischen Linker, Symbolauflöser oder Konstruktorlauf.

Der initiale Stack enthält ausgerichtetes argc/argv/envp/auxv sowie PHDR/PHENT/PHNUM, Entry, Seitengröße und Identitätswerte. Modellierte PID/TID/UID/GID sind 1000. `AT_RANDOM` enthält für reproduzierbare Ausführung die ersten 16 Bytes des Eingabe-SHA-256; das ist eine deterministische Modellpolicy, keine kryptografische Entropie. HWCAP/HWCAP2 sind null; ein vDSO gibt es nicht.

Implementiert sind `write`, `writev`, `exit`, `exit_group`, `getpid` und `gettid`, `mmap`, `mprotect`, `munmap`, `brk`, mit getrennten Nummern für [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) und [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h). Ein zurückkehrendes x64-SYSCALL setzt RCX/R11-Clobber, RAX und den nächsten PC. ARM64 nutzt x8 als Nummer und x0 als Ergebnis. Unbekannte Aufrufe stoppen als `unsupported_service`; Host-system calls werden nie ausgeführt.

Statische `PT_TLS`-Vorlagen werden als Loader-Fakten validiert: genau eine Vorlage, begrenzte Datei-/Speicherbereiche, passende Ausrichtung und lesbare initialisierte Bytes. Der Gaststart allokiert und initialisiert TLS-Blöcke und setzt den Thread-Pointer; das Linux-Modell erfindet kein libc-spezifisches TCB oder DTV. Compiler-erzeugtes local-exec TLS in freestanding Programmen wird damit unterstützt. Dynamisches TLS und OS-Threads bleiben außerhalb des Umfangs.

Auf x64 unterstützt `arch_prctl` `ARCH_SET_FS`, `ARCH_GET_FS`, `ARCH_SET_GS` und `ARCH_GET_GS`. Set akzeptiert eine User-Basis auch ohne Mapping; spätere Dereferenzierungen prüfen weiterhin Rechte. Kernel-Basen liefern Gast-`EPERM`, ungültige Get-Ziele Gast-`EFAULT`, ohne CPU-Fault. Andere Operationen schlagen ausdrücklich fehl. ARM64 installiert `TPIDR_EL0` mit `MSR`; `MRS`, FS/GS-Speicherzugriffe und Kontextwiederherstellung erhalten Thread-Pointer über Quanten und Backend-Eintritte. Das implementiert keinen Scheduler.

Deskriptor 1 und 2 sind virtuelle Byte-Senken. `write` prüft lesbare User-Seiten, liefert bei später unzugänglicher Seite das lesbare Präfix und `EFAULT`, wenn kein Byte lesbar ist. Ein falscher Deskriptor ergibt `EBADF`; auch ein Null-Byte-write prüft den Benutzeradressbereich, benötigt aber keine abgebildete Seite und liest keine Daten. Linux-Pipe-Atomicität oder Dateien werden nicht modelliert. Das Ausgabelimit stoppt vor Veröffentlichung eines zu großen Schreibvorgangs.

Anonyme Speicherdienste nutzen denselben Prozessadressraum und dasselbe physische Budget wie Image und Stack. `mmap` akzeptiert genau `MAP_PRIVATE | MAP_ANONYMOUS` mit gewöhnlichem `PROT_NONE`, `PROT_READ`, `PROT_READ | PROT_WRITE`, `PROT_READ | PROT_EXEC` oder lesbarem RWX. Freie, seitenausgerichtete Hinweise werden übernommen; sonst beginnt die Lückensuche bei `0x100000000`, danach bei der niedrigsten Benutzeradresse und unter Erhalt der Stack-Schutzbereiche. Das deterministische Verfahren emuliert kein Linux-ASLR. Neue Seiten sind unabhängig und nullinitialisiert; teilweises Entfernen kann nicht fixierte Seiten freigeben. CPU-Projektionen oder gehaltene Backing-Views können entfernte Allokationen bis zum Ende ihrer eigenen Lebensdauer erhalten.

Längen werden auf Seiten aufgerundet. `munmap` toleriert Lücken und wiederholtes Entfernen; `mprotect` ändert das gemappte Präfix und liefert an einer Lücke `ENOMEM`. `PROT_NONE` erhält Allokation und Bytes, verweigert aber Gastzugriffe. Der rohe `brk`-Aufruf liefert bei Erfolg die angeforderte Bytegrenze, sonst die alte Grenze, nicht die Null/Minus-eins-Konvention des libc-Wrappers. Die anfängliche Grenze ist das seitenausgerichtete Image-Ende. Wachstum berücksichtigt andere Mappings und das Budget; Schrumpfen erhält Bytes der verbleibenden Teilseite. Regeln und Fehlerpriorität folgen den Linux-Diensten für [Mapping](https://github.com/torvalds/linux/blob/v6.8/mm/mmap.c) und [Schutz](https://github.com/torvalds/linux/blob/v6.8/mm/mprotect.c).

Datei-, gemeinsame und feste Mappings, abwärts wachsender Speicher, große Seiten, Speichersperren, Schutzschlüssel, reine Ausführungs-/Schreibrechte und weitere Flags sind ausdrücklich nicht unterstützt. Sie stoppen vor veröffentlichten Effekten oder erfundenen Rückgabewerten. Normale Bereichs-, Längen- und Ausrichtungsfehler innerhalb der unterstützten Teilmenge liefern Gastfehler und erlauben die Fortsetzung. Kein Gastzeiger oder Mapping-Auftrag wird an das Host-OS weitergereicht.

Die optionale Eingabe `linux_kernel` erfasst ausdrücklich beobachtete fehlende Kernelschnittstellen. Eine Fixture ohne `pidfd_open`-Implementierung verwendet beispielsweise:

```json
{"linux_kernel":{"unavailable_syscalls":["pidfd_open"]}}
```

Der ausgewählte rohe Aufruf liefert wie ein fehlender Kerneleintrag bereits vor der Argumentprüfung -ENOSYS, ohne Deskriptoren oder Gastspeichereffekte zu erzeugen. Bionics `syscall`-Wrapper behält seine übliche -1/errno-Übersetzung. Fehlende Eingaben und leere Listen behalten die bisherige Nichtunterstützung dieser Schnittstelle bei; andere unbekannte Aufrufe werden nicht in ENOSYS umgewandelt. Derzeit ist nur `pidfd_open` zugelassen; unbekannte Namen, Duplikate und falsche Typen werden abgelehnt. Die Eingabe unterstellt weder Kernelversion noch Hostverfügbarkeit oder eine funktionierende pidfd-Implementierung. Siehe die [Kernelimplementierung fehlender Aufrufe](https://github.com/torvalds/linux/blob/master/kernel/sys_ni.c).

Die optionale Eingabe `linux_priority` deklariert den Nice-Zustand von Fixture-Tasks mit der UID des Aufrufers. Rohe `setpriority`- und `getpriority`-Aufrufe teilen diesen Zustand in Linux-ELF64- und nativen Android-Workloads; Hostprioritäten bleiben unverändert.

```json
{"linux_priority":{"tasks":[{"id":1000,"nice":0}],
                   "cap_sys_nice":false,"rlimit_nice":0}}
```

Task-IDs sind eindeutige positive vorzeichenbehaftete 32-Bit-Werte, anfängliche Nice-Werte liegen zwischen -20 und 19, `rlimit_nice` zwischen 0 und 40. `cap_sys_nice` und `rlimit_nice` sind standardmäßig false und null; Task-Zustand wird stets explizit angegeben. Fehlende Eingaben, nicht aufgeführte Tasks sowie PRIO_PGRP/PRIO_USER stoppen als nicht unterstützter Dienst. Das gilt auch für neu erzeugte Threads ohne deklarierten Nice-Zustand; Vererbung oder Eigentümerschaft anderer Tasks wird nicht geraten. PRIO_PROCESS mit who null wählt den aktuellen Gast-Task, sonst den benannten Task.

Ungültige Selektoren liefern roh -EINVAL. Setzanfragen begrenzen das vorzeichenbehaftete 32-Bit-Nice-Argument auf -20..19. Eine Absenkung benötigt CAP_SYS_NICE oder ein ausreichendes RLIMIT_NICE; andernfalls bleibt der Zustand unverändert und der Aufruf liefert roh -EACCES. Rohe Abfragen liefern `20 - nice` mit der Kernelkodierung 40..1, nicht das von libc übersetzte `getpriority`-Ergebnis. Siehe die [Linux-Prioritätsschnittstelle](https://man7.org/linux/man-pages/man2/setpriority.2.html).

`linux_signals` legt die anfänglichen Signalaktionen für den gesamten Prozess fest. Fehlende Einträge sind unbekannt und bedeuten nicht `SIG_DFL`; eine ausdrücklich leere Liste erlaubt das Setzen ohne Abfrage des Vorgängers. Alle fünf Felder sind erforderlich. Für vorzeichenlose 64-Bit-Werte außerhalb des exakten JSON-Zahlenbereichs dienen Dezimalzeichenfolgen.

```json
{"linux_signals":{"actions":[
  {"signal":11,"handler":0,"flags":0,"restorer":0,"mask":0}
]}}
```

Die Signalnummern reichen von 1 bis 64. `rt_sigaction` und Bionic teilen denselben Zustand; Strukturaufbau und Fehlerreihenfolge folgen jeweils der Schnittstelle. Wartende Signale, Zustellung, Handleraufrufe und threadbezogene Masken sind nicht implementiert; Host-Handler werden nicht verwendet. Siehe den [vollständigen Vertrag](../process-emulation.md#linux-profile-semantics).

<a id="windows-pe64-profile"></a>

`linux_kernel.gki` wählt 5.10–6.18 für `pidfd_open` und Vektorimport; Android-API-Stufen wählen keinen Kernel. `linux_kernel.tasks` enthält feste lebende Aufgaben als `{ "id": 2000, "group_leader": true }`. Weglassen lässt fremde Ziele ununterstützt, ein leeres Array kennt nur den laufenden Gruppenführer, außerhalb des Katalogs gilt ESRCH. Prioritäten müssen konsistent sein; kooperative Android-Threads sind ausgeschlossen. Die Tabelle gehört `linux_files`; GKI und ausdrücklich fehlendes pidfd widersprechen sich. Siehe [Vertrag und Grenzen](android-gki-kernels.md).

<!-- i18n-section: linux-clocks -->

## Explizite Gastuhren

Die optionale Eingabe `linux_time` liefert feste Zeitwerte für Linux-Systemaufrufe und Android Bionic. Das Modell liest keine Hostuhr, lässt die Zeit nicht mit ausgeführten Anweisungen fortschreiten und nimmt keinen Standardzeitpunkt an.

```json
{"linux_time":{"clocks":[
  {"id":0,"seconds":"4294967297","nanoseconds":987654321},
  {"id":1,"seconds":123,"nanoseconds":456789}],
  "timezone":{"minutes_west":-60,"dst_time":0}}}
```

Die statischen Uhr-IDs 0–9 und 11 sind zulässig. Jede Uhr ist unabhängig; fehlende Werte bleiben unbekannt. Doppelte oder unbekannte IDs werden abgelehnt. Sekunden sind vorzeichenbehaftete 64-Bit-Werte, Nanosekunden liegen in `[0, 1000000000)`. JSON-Ganzzahlen sind auf `±9007199254740991` begrenzt; Dezimalzeichenfolgen erhalten den gesamten 64-Bit-Bereich. Zeitzonenfelder sind vorzeichenbehaftete 32-Bit-Werte. C++ verwendet `ProcessOptions::LinuxTime`; andere OS-Profile lehnen diese Option ab.

`clock_gettime`, `gettimeofday` und x64-`time` teilen diese Eingaben. Fehlende Werte, dynamische Uhren oder nicht modellierte Teilschreibzugriffe führen zum expliziten Stopp; abgeschlossene Schreibzugriffe bleiben erhalten. Zeitanpassung, Schlafen und reale Geräteuhren bleiben unmodelliert. Schreibreihenfolge, Fehler und Zeiger beschreibt der [vollständige Uhrvertrag](../process-emulation.md#explicit-guest-clocks).

<a id="explicit-memory-files"></a>

<!-- i18n-section: linux-files -->

## Explizite Speicherdateien

`linux_files` gibt Linux ELF64 und Android einen geschlossenen Katalog unveränderlicher Dateien. Das erforderliche Array `files` darf leer sein; jeder Eintrag benötigt den kanonischen absoluten Pfad `path` und Binärdaten `bytes_hex`. Hostdateien und implizite `/proc`-Inhalte werden nicht gelesen. Ohne Option bleiben Dateidienste unmodelliert; fehlende Pfade liefern `ENOENT`.

```json
{"linux_files":{"files":[
  {"path":"/fixture/data","bytes_hex":"00ff410a805a"}],
  "descriptor_limit":256}}
```

Jedes Öffnen hat einen eigenen Cursor; Bionic, `syscall`, rohe Traps und Gastthreads teilen Deskriptoren. Schließen erlaubt die Wiederverwendung der kleinsten freien Nummer. Unterstützt werden schreibgeschütztes `open/openat`, `read/close`, normales `lseek`, `O_CLOEXEC` und das architekturspezifische `O_LARGEFILE`. Nur Bionic wandelt errno um. Lesefehler erhalten das kopierte Präfix. stdin hat keinen Standardinhalt; relative Pfade, Verzeichnisse, Schreiben und Links bleiben ausgeschlossen.

C++: `ProcessOptions::LinuxFiles`. `descriptor_limit`: 3–4096 (256); `files` ≤ 256; `path` < 4096 bytes; component ≤ 255 bytes; data + paths + NUL ≤ 16 MiB; JSON ≤ 64 KiB. [Contract](../process-emulation.md#explicit-memory-files).

Ein Lesezugriff der Länge null darf an der Grenze des Benutzeradressraums beginnen. Nach Prüfung des ursprünglichen Adressbereichs führt eine Dateiposition plus ursprünglicher Anforderungslänge oberhalb von `INT64_MAX` auch bei EOF zu `EINVAL`; der Cursor bleibt unverändert.

Ein Eintrag kann vollständige `metadata` enthalten; C++ verwendet `LinuxFileOptions::Metadata` mit bereits vorhandenen Dateipfaden. `fstat`/`fstat64` und syscall teilen feste Beobachtungen, ohne Hostmetadaten, Größenableitung aus Dateibytes oder Cursoränderung. Nur reguläre Dateien und vollständige Felder sind zugelassen; volle Integerbreiten verwenden Dezimalstrings. x64/AArch64 schreiben 144/128 Bytes, rdev und Padding bleiben null. Ungültige Deskriptoren ergeben `EBADF`, vollständig unbeschreibbare Ausgaben `EFAULT`. Fehlende Metadaten, unbekannte Standardstreams und gemischte Schreibrechte stoppen ohne Byteänderung. Felder und Grenzen stehen im verlinkten Vertrag.

```json
{"linux_files":{"files":[{"path":"/fixture/virtual","bytes_hex":"616263",
  "metadata":{"device":1,"inode":"18446744073709551615","mode":33060,
    "link_count":1,"uid":1000,"gid":1000,"size":0,"block_size":4096,"blocks":0,
    "access_time":{"seconds":0,"nanoseconds":0},
    "modification_time":{"seconds":0,"nanoseconds":0},
    "change_time":{"seconds":0,"nanoseconds":0}}}]}}
```

<!-- i18n-section: windows-pe64 -->

## Windows-PE64-Profil

`windows-pe64-v1` unterstützt begrenzte Windows-x64/ARM64-Konsolenprozesse mit PEB/TEB, statischem und dynamischem TLS, `DllMain`, benannten Win32-APIs und expliziten azyklischen DLL-Graphen. Gastmodule unterstützen Code-/Datenimporte nach Name oder Ordinal, DIR64, weitergeleitete Exports und echte Loader-Listen. `LoadLibraryA` / `LoadLibraryW`, `FreeLibrary` und `GetProcAddress` verwenden den konfigurierten Katalog. CRT/GUI, ARM64-Frame-basiertes Benutzer-SEH, Threads und allgemeine Windows-Kompatibilität bleiben unvollständig; native ARM64-KVM/WHP-Belege fehlen weiterhin.

Der virtuelle Windows-Speicher ergänzt `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` und `FlushInstructionCache` für den aktuellen Prozess. Die OS-Schicht verwaltet Reservierungen; `AddressSpace` bleibt maßgeblich für zugesicherte Seiten, Zugriffsrechte und deren Speicher. Tests prüfen Codeänderungen, Zugriffsfehler und die Wiederverwendung des Speicherbudgets.

Private Zuweisungen unterstützen `MEM_RESERVE`, `MEM_COMMIT`, `MEM_DECOMMIT`, `MEM_RELEASE` und `MEM_TOP_DOWN` mit 64 KiB Reservierungsausrichtung und 4 KiB Seiten. Reservierungen allein verbrauchen kein Gast-RAM. Erneute Zusicherung erhält die Bytes und aktualisiert Rechte; deren Aufhebung gibt einzelne Seiten frei. Bereichsprüfung und vorbereitete Zuweisungen verhindern Teiländerungen bei gewöhnlichen Fehlern. Die Abfrage liefert die 48 Byte große x64/ARM64-Struktur und fasst nachfolgende Seiten nur innerhalb derselben Zuweisung zusammen. Abbild, Umgebung, Heap, API-Einstiege und Stapelränder werden bei der Platzierung berücksichtigt; die Stapelidentität stimmt mit dem TEB überein. Macht ein erfolgreiches `VirtualProtect` den Ausgabebereich für die alten Rechte schreibgeschützt, bleiben die neuen Rechte wirksam und die Ausgabebytes unverändert; der Aufruf meldet weiterhin Erfolg. Eine Rechteänderung über nicht zugesicherte Seiten liefert `ERROR_INVALID_ADDRESS`, schreibt `PAGE_NOACCESS` in die Ausgabe der alten Rechte und lässt die Seitenrechte unverändert.

Unterstützt werden `PAGE_NOACCESS`, `PAGE_READONLY`, `PAGE_READWRITE`, `PAGE_EXECUTE_READ` und `PAGE_EXECUTE_READWRITE`. Schutzseiten, reine Ausführung, Copy-on-Write, Cacheattribute, große Seiten, reset/write-watch/Platzhalter und Änderungen modellinterner Laufzeitabbildungen bleiben ausdrücklich nicht unterstützt. Nur private virtuelle Zuweisungen können zurückgenommen oder freigegeben werden.

[VirtualAlloc](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc), [VirtualFree](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualfree), [VirtualProtect](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect), [VirtualQuery](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualquery), [MEMORY_BASIC_INFORMATION](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-memory_basic_information).

```bash
neverd emulate guest.exe --profile=windows-pe64-v1 \
  --options='{"backend":"auto","arguments":["guest.exe","argument"],"environment":["MODE=test"]}'
```

Das PE32+-EXE mit einem Thread behält seine bevorzugte Basis und erlaubt explizite DLLs mit optionalem Einstieg und statischem TLS. `WindowsProcessOptions::Modules` oder JSON `windows.modules` gibt bis zu 64 Gast-Basisnamen und Host-Eingabepfade über `name` und `path` an; Host-DLLs werden weder gesucht noch ausgeführt. ASCII-Namen sind ohne Groß-/Kleinschreibung eindeutig; Duplikate und Überschreiben von Systemanbietern werden abgelehnt. Nur erreichbare Dateien werden gelesen. Namens-/Ordinalimporte binden echte Exports; Lücken, fehlende Symbole, Zyklen, gebundene/verzögerte Imports und nicht unterstützte Load-Config/CFG schlagen fehl. DIR64 verschiebt bewegliche DLLs bei Kollision; feste Konflikte und Relokationen in Link-Metadaten scheitern vor der Veröffentlichung des betroffenen Abbilds.

`readPEProgramExports` besitzt Original-Exports und Lesebereiche; `WindowsProcessModules` besitzt Graph und prozessweite API-Gates je Anbieter/Name. `VirtualMemory` reserviert alle Images vor dem Mapping, `AddressSpace` verwaltet Seiten und Rechte. PEB/LDR enthält reale Images; die Initialisierungsliste bewahrt die Registrierungsreihenfolge des Loaders. Diese wird getrennt von der abhängigkeitsbasierten Attach-Reihenfolge geführt. `GetModuleHandleW` akzeptiert NULL oder ASCII-Basisnamen, ignoriert Groß-/Kleinschreibung und ergänzt ohne Erweiterung `.dll`. Pfade, Nicht-ASCII und abschließende Punkte bleiben ununterstützt. Fehlende Namen liefern Fehler 126; Erfolg erhält LastError. API-Modelle sind keine installierten DLLs.

Eingabebytes und gesamte Image-Ausdehnungen teilen jeweils `memory_limit`; Laufzeit-Mappings zählen zum Image-Budget. Vorbereitung teilt 65,536 Datensätze, 64 MiB Metadatenlesezugriffe, begrenzte Namen und die Arbeitsfrist, ohne harte Host-E/A-Zeitgarantie. Das eigene EXE→DLL→DLL-Fixture prüft Rebasing, Ordinale, gemeinsame Daten, API-Identität, `MEM_IMAGE`, Listen und EXE-TLS-Attach/Detach. `NeverDWindowsProcessTests` enthält das native Windows-Orakel, `NeverDPEProgramExportsTests` ungültige Metadaten und Budgets, `NeverDProcessPublicTests` C-ABI/CLI-Parität. Nicht verfügbare Backends werden explizit übersprungen.

`WindowsProcessLifetime` führt DLL-TLS und danach `DllMain` in Abhängigkeitsreihenfolge aus, anschließend EXE-TLS und Einstieg, auf einer CPU mit gemeinsamem Budget. Jedes Modul erhält einen eigenen TLS-Index und ausgerichteten Block aus dem relokierten, verknüpften Image im gemeinsamen 64-KiB-Bereich. Das reservierte TLS-Argument ist null; `DllMain` erhält bei Start/Prozessende einen undurchsichtigen Nicht-NULL-Wert. Explizites Prozessende trennt fertig initialisierte DLLs in umgekehrter Loaderlisten-Reihenfolge und danach EXE-TLS, auch vor dessen Initialisierung. Start-`DllMain(FALSE)` beendet mit `0xc0000142` ohne Detach. Fehler und erschöpfte Budgets erfinden keine Bereinigung. PE-Einstiegsreturn mit Gast-DLLs benötigt nicht unterstütztes Thread-Ende und stoppt explizit. Ein von null verschiedenes `SizeOfZeroFill` bleibt ausgeschlossen; Nullbytes im tatsächlichen TLS-Template sind unterstützt. DLLs ohne Einstieg erhalten TLS-Attach, aber keine Prozess-Detach-Benachrichtigung.

`WindowsProcessExports` löst statische Imports und `GetProcAddress` über dieselben Namens-/Ordinalidentitäten auf, einschließlich Code, Daten, Aliasen und Weiterleitungsketten. Nur tatsächlich verwendete Startweiterleitungen ergänzen Katalogmodule und Initialisierungsabhängigkeiten; unbenutzte laden keine Dateien. Namen unterscheiden Groß-/Kleinschreibung; fehlende Namen liefern NULL/Fehler 127, direkt abgefragte fehlende Ordinale einschließlich Lücken NULL/Fehler 182 und ein NULL-Abfrageargument Fehler 87, Erfolg erhält LastError. Unbekannte Modulhandles bleiben ununterstützt. Exakte Anbieter-/Namens-API-Einstiege werden einmal aus dem begrenzten Register reserviert. Die Auflösung prüft aktuelle PE-Header und Exportmetadaten jedes Abbilds, lehnt Änderungen oder unlesbare Bytes ab, begrenzt Ketten auf 64 Einträge und teilt verbleibende Metadatenbudgets und die Ausführungsfrist. Eine Weiterleitung auf eine Lücke liefert die Zielbildbasis und erhält LastError; Ordinal null liefert Fehler 87. Die Basis ist eine Datenadresse und erteilt keine Ausführungsrechte für Image-Header. Laufzeitweiterleitungen können konfigurierte Module laden und vor Rückgabe des Ergebnisses initialisieren. Änderungen aktiver Exporttabellen bleiben ununterstützt.

`WindowsProcessLoader` lädt ASCII-DLL-Basisnamen aus `windows.modules` und verwaltet explizite Referenzen, gemeinsame Abhängigkeiten und den Erhalt von Startmodulen. Wiederholte Weiterleitungsabfragen erhöhen die Referenzzahl nicht. Beim erneuten Laden erhält ein Katalogplatz eine neue residente Generation. TLS und `DllMain` laufen auf derselben CPU unterhalb angehaltener API-Stackframes; die Registerwiederherstellung erhält Gastspeicheränderungen und verwendet die aktuelle Rücksprungadresse. Reservierte Zeiger bei dynamischem Attach/Detach sind null. Fehlgeschlagenes Attach beim expliziten Laden liefert nach Bereinigung Fehler 1114, behält aber erfolgreiche unabhängige verschachtelte Ladevorgänge. Entladen gibt Abbild und TLS frei; erneutes Laden stellt Originalbytes her. Fremde Änderungen an Loader-Listen oder TLS-Zeigern werden abgewiesen. Datei-, Abbild- und Metadatenbudgets bleiben auch nach Fehlern kumulativ. Systemanbieter verwenden ihre eingeblendeten PE-Basen als Modulhandles. Dateisuche, Nicht-ASCII-Pfade, `LoadLibraryEx`-Flags, Importzyklen und reentrante Übergänge desselben gerade initialisierten oder entladenen Moduls bleiben ununterstützt.

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` verwenden denselben aktuellen Gastumgebungsblock in den PEB-Prozessparametern. ASCII-Namen werden ohne Beachtung der Großschreibung verglichen; Werte sind UTF-16. Änderungen prüfen Eingaben, Kapazität und Schreibrechte vor der Veröffentlichung. Momentaufnahmen bleiben unabhängig von späteren Änderungen und geben ihren Gastspeicher beim Freigeben zurück. Das Modell begrenzt den Block auf 64 KiB; Zeichenketten und Ersetzungen sind begrenzt und prüfen die Ausführungsfrist. Unbekannter Zeigerbesitz, fehlerhafte Blöcke, ANSI-Codepages und überlappende Ersetzungspuffer bleiben ununterstützt. `WindowsEnvironmentTests.cpp` vergleicht eigene x64/ARM64-Fixtures auf verfügbaren Backends; CI verlangt ein unabhängiges natives Windows-Orakel.

`WindowsProcessHeap` verwaltet Allokation, [`HeapReAlloc`](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heaprealloc), Freigabe und Größenabfragen des Prozessheaps gemeinsam. Größenänderungen erhalten die verbleibenden Bytes; `HEAP_ZERO_MEMORY` löscht hinzugefügte Bytes, `HEAP_REALLOC_IN_PLACE_ONLY` verbietet Verschiebungen. Fehlgeschlagene Größenänderungen erhalten den alten Block und liefern NULL mit `ERROR_NOT_ENOUGH_MEMORY` (8), entsprechend den nativen Beobachtungen. Separate Seiten geben bei Verkleinerung und Freigabe Kapazität zurück; vorbereitetes Wachstum und begrenzte Kopien prüfen die Ausführungsfrist. Eigene Heaps, Ausnahmeflags, unbekannter Besitz und unzugängliche Kopier- oder Löschbereiche stoppen ausdrücklich. `WindowsHeapTests.cpp` prüft beide ISAs, erzwungene Verschiebung, Budgetwiederverwendung und Fehleratomarität; CI führt dieselbe eigene EXE auch auf nativem Windows aus.

`WindowsSystemModules` erzeugt begrenzte PE64-Modellabbilder für `ntdll.dll`, `kernelbase.dll` und `kernel32.dll` auf beiden ISAs. ASCII-Abfragen über `GetModuleHandleA` / [`GetModuleHandleW`](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulehandlew), `LoadLibraryA` / `LoadLibraryW` und [`GetProcAddress`](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress) teilen deren eingeblendete Basisadressen; PEB/LDR und `MEM_IMAGE` beschreiben dieselben Abbilder. Statische Importe, Namensabfragen und Gastweiterleitungen nutzen dieselben API-Gates und Exportauflösung. Anbieter bleiben fest resident, haben keine Gastinitialisierungs-Callbacks und verhindern nach Entladen gewöhnlicher Gast-DLLs keine Rückkehr vom Einstiegspunkt. Geänderte Header oder Exportmetadaten stoppen die Suche. Unmodellierte Systemexportnamen und von null verschiedene Systemordinale stoppen ausdrücklich; reine Schreibungsabweichungen modellierter Namen sowie leere Namen liefern Fehler 127, eine NULL-Abfrage liefert 87. Erzeugte Bytes und Adressen sind Modellregeln; Windows-Versionslayouts, native Ordinale und anbieterübergreifende Aliase werden nicht rekonstruiert. `WindowsSystemTests.cpp` vergleicht eigene x64/ARM64-EXEs mit nativem Windows und beobachtet acht Rückgaben des Anfangsthreads unabhängig.

`WindowsProcessExceptions` implementiert `AddVectoredExceptionHandler`, `RemoveVectoredExceptionHandler` und `RaiseException` auf derselben CPU mit dem Prozessbudget. Geordnete Handler dürfen Registrierungen ändern, verschachtelte Ausnahmen auslösen, modellierte APIs aufrufen, DLLs laden und den Prozess beenden. Datenzugriffsverletzungen auf x64/ARM64 und x64-Ganzzahldivisionsfehler können nach Prüfung der Gaständerungen an `CONTEXT` fortgesetzt werden; allgemeine Register, SIMD und unterstützter FP-Zustand bleiben erhalten. Softwareausnahmen kehren über eine echte Rücksprunganweisung im modellierten Anbieter zurück. Grenzen sind 128 aufbewahrte Registrierungen und 16 verschachtelte Frames. Ungültige Ergebnisse, geänderte Ausnahmezeiger, nicht unterstützte Felder und Grenzüberschreitungen scheitern ausdrücklich. Stackbasiertes ARM64-SEH/Unwinding, Debugger-Zustellung und Ausführungs-/Schutzseitenfehler bleiben offen. `WindowsExceptionTests.cpp` vergleicht eigene EXE/DLL-Szenarien mit nativem Windows; native ARM64-KVM/WHP-Belege fehlen weiterhin. [AddVectoredExceptionHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-addvectoredexceptionhandler), [RemoveVectoredExceptionHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-removevectoredexceptionhandler), [RaiseException](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-raiseexception), [CONTEXT x64](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-context), [ARM64_NT_CONTEXT](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-arm64_nt_context). Datensätze für Softwareausnahmen tragen `EXCEPTION_SOFTWARE_ORIGINATE` (`0x80`), unabhängig vom durch den Aufrufer gesetzten Flag für nicht fortsetzbare Ausnahmen; die ursprüngliche Windows-Programmdatei prüft die exakten Flagwerte für Software- und Hardwareausnahmen. [EXCEPTION_RECORD](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_record).

`WindowsProcessContext` behält den Ursprung jedes Dispatch-Frames bei. Unterstützte x64-Datenzugriffs- und Divisionsfehler setzen RF (`0x10000`) in `CONTEXT.EFlags`; `RaiseException` behält auch bei Software-Zugriffsverletzungen den aktuellen Kontext. Der Ursprung bleibt während VEH/VCH und SEH-Suche/Unwinding erhalten. Eine gültige Fortsetzung stellt logische CPU-Flags ohne RF wieder her; RF-Änderungen des Gasts werden vor der Zustandsübernahme abgewiesen. Dieses begrenzte Profil modelliert weder Befehls-Breakpoints noch gastgesteuertes RF. `WindowsExceptionTests.cpp` prüft gespeicherte Daten, Wiederherstellung und unveränderte CPU/RAM bei Ablehnung.

Nach unabhängigen nativen Beobachtungen bildet Windows ring3 checked-x64-Fehler mit `operand_alignment` auf `STATUS_ACCESS_VIOLATION` mit `[read, UINT64_MAX]` ab, auch bei Schreibbefehlen. Die CPU-Schicht liefert die Ursache; Windows errät sie nicht aus Vektor 13 und dekodiert den Befehl nicht erneut. `WindowsAlignmentProcessTests.cpp` führt originale PE-Befehle in 72 Fehlerszenarien und 9 Wiederholungen nach Adresskorrektur (`72 + 9`) aus und prüft PC, RF, XMM und RAM. Unklassifizierte oder widersprüchliche Fehler werden abgelehnt. Prozess- und Treiberberichte bewahren nullable `cause` und hexadezimales `error_code`; fehlende Werte bleiben vom Zahlenwert Null verschieden. Diese Zustellung gilt für das checked-x64-Benutzerprofil.

`AddVectoredContinueHandler` und `RemoveVectoredContinueHandler` verwalten eine eigene geordnete Liste; beide Handlerfamilien teilen die Grenze von 128 aufbewahrten Registrierungen. Akzeptiert ein vektorisierter Ausnahmehandler die Fortsetzung, sehen die Fortsetzungshandler denselben veränderbaren Ausnahmedatensatz und `CONTEXT`. Die abschließende Kontextprüfung erfolgt nach diesen Rückrufen, einschließlich verschachtelter Ausnahmen und DLL-Benachrichtigungen. Handles lassen sich nicht über die andere Handlerfamilie entfernen. `WindowsContinuationTests.cpp` vergleicht eigene EXE-Szenarien für Reihenfolge, vorzeitiges Ende, Listenänderungen, Kontextreparatur, Verschachtelung, Loader-Rückrufe und Prozessende mit nativem Windows. Der geprüfte Windows-x64-Vektorpfad erlaubt die Fortsetzung mit `EXCEPTION_NONCONTINUABLE`; dies belegt kein stackbasiertes SEH-Verhalten. Native ARM64-Ausführung bleibt ungeprüft. [AddVectoredContinueHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-addvectoredcontinuehandler), [RemoveVectoredContinueHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-removevectoredcontinuehandler).

`RtlCaptureContext` ist über `kernel32.dll` und `ntdll.dll` für x64 und ARM64 verfügbar. Das gemeinsame `WindowsProcessContext` und `IntegerABI` speichern PC/SP des Aufrufers, ohne CPU-Zustand oder LastError zu ändern. Native Windows-Beobachtungen bestätigen x64-Flags `0x10000f`, unveränderte nicht beschriebene Home-/Debug-/Vektorbereiche und die klassischen 32-Bit-x87-Adressfelder; ARM64 übernimmt LR als PC und setzt X0/LR im Datensatz auf null. Register, SIMD und Gleitkommasteuerung stammen vom Gast; x64-Selektoren und MXCSR-Fähigkeitsmaske folgen der konfigurierten Gast-CPU. Ungültige, falsch ausgerichtete oder teilweise unzugängliche Ziele scheitern vor dem Schreiben. `WindowsContextTests.cpp` prüft direkte Importe, Anbieterabfragen, VEH-Rückrufe, seitenübergreifende Ausgaben und atomare Fehler. `scripts/check_windows_context.py` führt das Originalprogramm unter Windows x64/ARM64 aus und prüft nicht leeren x87-Zustand mit einem eigenen nativen Orakel. Diese ARM64-API-Beobachtungen belegen keine native KVM/WHP-Ausführung. Kontextwiederherstellung, Stack-Walking und dynamische Funktionstabellen bleiben getrennte Aufgaben. `WindowsProcessServices.def` legt genaue Modulbeschränkungen fest: Die Suche in `kernelbase.dll` liefert entsprechend den nativen Beobachtungen `ERROR_PROC_NOT_FOUND` (127), ohne einen zusätzlichen Export zu erfinden. [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

`WindowsProcessSEH` nutzt das gemeinsame `X64SEH` unter `os/windows/exception/` (`NeverDEmulationWindowsException`, auch ohne Treiber) für x64 `__C_specific_handler` und UNWIND_INFO V1. Nach der VEH-Suche folgen Filter, finally-Aufrufe, nichtlokale Handlerübergänge, verschachtelte/kollidierte Abwicklungen und verschobene EXE/DLL-Frames; nichtflüchtige GPR/XMM bleiben erhalten. Bei Filterfortsetzung läuft VCH mit demselben `CONTEXT`. `WindowsSEHTests.cpp` vergleicht 23 eigene Szenarien mit nativem Windows; KVM/WHP/Unicorn teilen die Semantik. Das Prozessbudget umfasst erneute Prüfungen von Image-Generationen, Headern, Unwind-/Scope-Bytes, Codebereichen der Sprachhandler und IAT-Bindungen. Veränderte Metadaten oder entladene beibehaltene Images scheitern ausdrücklich. ARM64-Frame-SEH, C++ EH, dynamische Funktionstabellen, allgemeines RtlUnwind/NtContinue und Abwicklung über Loader-/VEH-/VCH-Callback-Grenzen bleiben ununterstützt.

Bei `EXCEPTION_NONCONTINUABLE` löst ein x64-Filter mit Rückgabe `EXCEPTION_CONTINUE_EXECUTION` eine `STATUS_NONCONTINUABLE_EXCEPTION` (`0xc0000025`, Flags `0x81`, verknüpfter Datensatz null) mit neuem Kontext aus. VEH läuft erneut vor der Suche im erhaltenen logischen Stack; finally-Reihenfolge, EXE/DLL-Frame-Identität sowie Tiefen- und Ausführungsbudgets bleiben erhalten. Die 23 nativen Szenarien umfassen 21 erfolgreiche Ausführungen und zwei Beendigungen: Eine in VEH/VCH akzeptierte Fortsetzung dieser sekundären Ausnahme bleibt auch nach Wiederherstellung des ursprünglichen `CONTEXT` unbehandelt. Das Modell meldet einen Laufzeitfehler. Software-Ausnahmeadressen entsprechen dem gespeicherten PC; interne Dispatcher-Adressen und Registerlayouts sind Modellvorgaben. [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

Vor dynamischen Entlade-Callbacks verlässt das Modul die Initialisierungsliste; Abbildung, Namenssuche sowie Lade- und Speicherlisten bleiben während der Callbacks verfügbar. Native Vergleiche der Einstiegsrückkehr beobachten den initialen Thread unabhängig von System-Worker-Threads.

`WindowsDynamicTests.cpp` vergleicht originale x64/ARM64-DLLs und EXEs mit unabhängigen nativen Windows-Beobachtungen: Referenzen, gemeinsame Abhängigkeiten, verschachteltes Laden, Fehlerbereinigung, Weiterleitungen, Prozessende, DLLs ohne Einstieg und frisches TLS beim Neuladen. Zusätzliche Tests lehnen manipulierte Loader-Metadaten und veraltete Codezeiger ab, erhalten kumulative Budgets und lassen unterbrochene API-Ergebnisse unvollständig. Windows-CI verlangt natives Orakel und WHP-Fälle; Cross-Kompilierung und Unicorn ARM64 belegen keine native ARM64-Ausführung.

Eine fehlende Bibliothek in der Weiterleitungskette von `GetProcAddress` ergibt Fehler 127; ein explizites `LoadLibrary` für ein fehlendes Katalogmodul ergibt 126. Das native Orakel und jedes verfügbare Backend prüfen alle 41 deklarierten Ladeszenarien. Unter Windows wird die Rückkehr nach dem Entladen aller DLLs je DLL-Variante 16-mal beobachtet. Fehlgeschlagene Initialisierung über eine `GetProcAddress`-Weiterleitung liefert nach Bereinigung ebenfalls 127. Prozess-Detach-Callbacks erhalten den Stackinhalt des beendenden Aufrufers.

`WindowsExportTests.cpp` prüft mit ursprünglichen x64/ARM64-DLLs und einem EXE weitergeleitete Code-/Daten-/Ordinalaufrufe, Aliase, Initialisierungsabfragen, Rebasing, Groß-/Kleinschreibung, fehlende Exports, LastError, Zyklen, nicht residente Ziele, ungültige Zeiger und Metadatenänderungen nach erfolgreichen Abfragen. Dasselbe EXE dient einem unabhängigen nativen Windows-Orakel; WHP-Fälle sind in nativer CI Pflicht. C-ABI/CLI-Tests vergleichen vollständige Berichte. Native ARM64-Hardwarebelege stehen noch aus. EXE-Varianten mit und ohne Exporttabelle prüfen beide Graphen, PEB- und Detach-Reihenfolge sowie Fehler für Namen, Ordinale und NULL.

`WindowsLifetimeTests.cpp` vergleicht feste Traces mit unabhängigen nativen Windows-Prozessen und KVM/WHP/Unicorn: normales Ende, Einstiegsreturn, beide DLL-Fehler, vier frühe Enden und DLLs ohne Einstieg. Hinzu kommen Callback-Fehler, gemeinsame Budgets, relokierte TLS-Felder und Gesamtkapazität. Die native Return-Prüfung behält das Handle des Anfangsthreads und prüft 64-mal dessen Exitcode sowie die genaue Thread-/Prozess-Benachrichtigungsfolge. Verbleibende Kindthreads werden nach der Beobachtung beendet; der Prozess-Exitcode gilt nicht als Einstiegsrückgabe.

```json
{"windows":{"modules":[{"name":"middle.dll","path":"inputs/middle.dll"},{"name":"leaf.dll","path":"inputs/leaf.dll"}]}}
```

x64 GS und ARM64 x18 zeigen auf den TEB mit Stack-Grenzen, Selbstzeiger, PID/TID, PEB, Prozessparametern, LastError und TLS. Striktes UTF-8 wird zu UTF-16; argv wird nach Microsoft-CRT-Regeln zitiert. Umgebungsnamen sind ASCII; Duplikate ohne Beachtung der Großschreibung werden abgelehnt. Werte dürfen Unicode enthalten; der sortierte Block endet doppelt mit NUL. Hostumgebung und Dateisystem werden nicht übernommen. Statisches TLS kopiert Vorlage und Null-BSS und setzt einen 32-Bit-Index; dynamisches TLS verwendet eigene TEB-Slots. Attach/Detach liest die jeweils aktuellen Callback-Einträge in Reihenfolge bei gemeinsamem Zeit- und Ressourcenbudget. Normales Prozessende führt Detach aus. Der Eintrittspunkt darf nur ohne residente Gast-DLLs zurückkehren; ein zweites `ExitProcess` während der Prozessbereinigung bleibt ununterstützt.

`WindowsProcessServices.def` definiert `ExitProcess`, `RtlExitUserProcess`, Ausgabehandles und synchrones `WriteFile`, LastError, Prozess-/Thread-IDs und Pseudohandles, `GetCommandLineW` / `HeapReAlloc`, Heap-Allokation/Freigabe/Größe, dynamisches TLS und `LoadLibraryA` / `LoadLibraryW` / `FreeLibrary` / `GetModuleHandleA` / `GetModuleHandleW` / `GetProcAddress`. Aufgelöst werden exakte Namen aus `kernel32.dll`, `kernelbase.dll` und `ntdll.dll`. Direkte Syscalls und gefälschte Callback-Gates wählen keine Modelle. Heap-Speicher gehört dem Prozess und wird freigegeben; Ausgabe bleibt binär. Win32-Fehler sind von nicht unterstützter asynchroner E/A und Benutzerexception-Verarbeitung getrennt. Aliase berücksichtigen das anfängliche Nullsetzen des Ausgabezählers und den tatsächlichen Rückkehrslot.

`windows.native_calls` bewahrt Modul/Funktion, deklarierte skalare Argumente und nullable Ergebnisse ohne erfundene NT-Nummern. `NeverDWindowsProcessTests` prüft echte PE-Dateien, Compiler-TLS, Callback-Änderungen, Heap, Aliase, fehlerhafte Metadaten, Privilegien und Budgets; `NeverDProcessPublicTests` prüft CLI/C ABI. Windows-CI führt dasselbe EXE als unabhängiges Orakel aus und verlangt WHP-Tests. Native ARM64-Laufzeitnachweise benötigen weiterhin passende Hardware.

Bei einem nicht leeren, nicht lesbaren Eingabepuffer liefert `WriteFile` den Fehler `ERROR_INVALID_USER_BUFFER` (1784), setzt die Anzahl geschriebener Bytes auf null und gibt keine Bytes aus.

`windows.defer_unmodeled` lädt ein Abbild, dessen Laderfakten das Modell nicht implementiert, und stoppt nur, wenn die Ausführung von einem davon abhängt. Exporte außerhalb des API-Inventars und Module außerhalb des Katalogs werden an opake Einstiege gebunden: Jede Identität wird zu genau einer Adresse aufgelöst, und ihre Ausführung stoppt als `unsupported_service` unter Nennung von `module!export`. Verzeichnisse, die das Modell nicht interpretiert, bleiben uninterpretiert, Metadaten, die die Datei nicht hinterlegt, werden beim Laden nicht gelesen, und rahmenbasierte Ausnahmebehandlung durch ein solches Abbild stoppt. `observeProcess` ergänzt einen `ProcessObserver`, der den angehaltenen Prozess bei seinem Start und an jeder Ausführungsüberwachung liest; er kann den Gastzustand nicht ändern, und beendet er den Lauf, wird `observer` gemeldet. Das [Entpacken](unpack.md) baut auf beidem auf.

[PE/COFF](https://learn.microsoft.com/windows/win32/debug/pe-format), [ARM64 ABI](https://learn.microsoft.com/cpp/build/arm64-windows-abi-conventions), [WriteFile](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-writefile), [TLS](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue), [Wine 10.0 loader](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/ntdll/loader.c). [GetProcAddress](https://learn.microsoft.com/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress).

<!-- i18n-section: verification -->

## Verifikation

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# In einem Shared-Library-/CLI-Build:
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

Tests übersetzen eigenständige ELF-Einstiege in Assembler und C für beide ISAs. Sie prüfen data/BSS, Startmetadaten, Systemaufruffehler, Binärausgabe, Berechtigungsfehler, Teilzugriffe, nicht unterstützte Dienste und Budgets über Quanten hinweg. TLS-Fixtures initialisieren unabhängige ausgerichtete Blöcke, BSS und Thread-Zeiger und prüfen deren Erhalt; x64 prüft zusätzlich `arch_prctl`-Fehler ohne Verlust der alten Basis. Nicht verfügbare Backends werden ausdrücklich übersprungen. Die öffentliche Suite prüft gemeinsame C-ABI/CLI-Berichte und Exitcodes. PIE-Fixtures prüfen auxv und anfangs leere RELA-Slots vor eigenen Daten-/Funktionszeiger-Relokationen. Mapping-Tests erhalten Analyse-Fixups bei entsprechend gewählter Bytequelle; dynamische Tabellen decken fehlende Sections sowie fehlerhafte/abhängige Eingaben ab. Beide ISAs testen Allokation, Schutz, Lücken, Remapping, Heap-Wachstum/-Schrumpfen und behandelte Fehler. Echte Gastschreibzugriffe prüfen Fehler nach vollständigen und teilweisen Schutzänderungen. x64 ersetzt Code an derselben Adresse zwischen RW und RX und ruft beide Versionen auf; dasselbe ELF läuft unter Linux nativ als unabhängiges Ergebnis-/Fehlerorakel. Reine Speichertests prüfen Budgeterschöpfung, Rückgewinnung und maßgebliche Mapping-Snapshots ohne RAM festzuhalten. Cross-Compilation und Unicorn ARM64 sind kein Nachweis für natives ARM64 KVM/WHP.
