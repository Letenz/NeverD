**Sprachen**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 813c9673241230afbb295a950aab1e14478b4bd4fe9de2d2f2e27b6fbe34f588 -->

[← Dokumentationsübersicht](README.md)

# Gastprozessumgebungen für macOS und iOS

`lib/emulation/os/darwin/` modelliert begrenzte, eigenständige Mach-O-Prozesse unabhängig vom Host-CPU-Transport. Aktivieren Sie `NEVERD_ENABLE_CPU_EMULATION`; Windows-Treiberemulation ist nicht erforderlich. `macos/` und `ios/` definieren die expliziten Plattformprofile.

| Profil | Mach-O-Plattform | Gast-ISA | OS-Seitengröße |
| --- | --- | --- | --- |
| `macos-macho64-v1` | macOS | x86-64, Basis-ARM64 | 4 KiB x64; 16 KiB ARM64 |
| `ios-macho64-v1` | iOS-Gerät | Basis-ARM64 | 16 KiB |
| `ios-simulator-macho64-v1` | iOS Simulator | x86-64, Basis-ARM64 | 4 KiB x64; 16 KiB ARM64 |

Geräte- und Simulatorbilder sind nicht austauschbar; die Gastplattform wird nicht aus dem Host abgeleitet. Gleiche ISA unter macOS kann [HVF](macos-hvf.md) nutzen, sonst wählt `auto` Unicorn. Die CPU-Speichergranularität bleibt 4 KiB. [C-, Python- und CLI-APIs](process-emulation.md) teilen Optionen, Grenzen und Berichte.

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

## Bild und Prozessstart

`MachOExecutionImage` erhält die Originalbytes ohne Relokationsänderungen der Analyse. Nur dünne Little-Endian-`MH_EXECUTE`-Bilder mit eindeutiger Plattform und Eintrittsstelle werden zugelassen. Bei Universaldateien muss die gewünschte Architektur zuvor ausdrücklich extrahiert werden.

Die gesamte Datei einschließlich Metadaten und nachgestellter Bytes muss vor Parsing und Kopie in `memory_limit` passen. Der Loader liest einen begrenzten privaten Snapshot einer regulären Datei; NUL-Pfade, kurze Lesevorgänge und Größenänderungen werden abgewiesen. Er hält kein lebendes Dateimapping. Dateibudget und abgebildeter Gastspeicher haben getrennte Obergrenzen desselben Werts; Host-Datei-I/O besitzt keine harte Zeitgarantie.

Segmente behalten aktuelle/maximale Rechte und Nullfüllung. `__PAGEZERO` reserviert Adressen ohne große physische Allokation. Datei-/VM-Bereiche, OS-Seitenausrichtung, gerundete Überlappungen, Header-Zuordnung, ausführbarer Eintritt und Budget werden vor Ausführung geprüft. Das Headersegment muss lesbar und ausführbar sein; Schutzseiten und privates Rückkehrtor bleiben reserviert. Die letzte Dateiseite behält Bytes bis Seitengrenze oder EOF, spätere vollständige VM-Seiten werden genullt: [XNU-Loader](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c).

`LC_MAIN` erhält `argc`, `argv`, `envp` und den apple-Vektor als vier ganzzahlige Argumente. Der Rückgabewert liefert die unteren acht Bits des Exitstatus. `/usr/lib/dyld` ist nur für diesen importfreien Eintritt zulässig; Host-dyld wird nicht ausgeführt. Ein von null verschiedener `stacksize` wird abgewiesen, weil die Aufruferoption `stack_size` das Budget besitzt.

`LC_UNIXTHREAD` verlangt genau einen vollständigen nativen 64-Bit-Allgemeinregistersatz, in dem nur PC belegt ist. Der Startstack enthält argc, terminierte argv/envp und einen terminierten apple-Vektor mit `executable_path=<input filename>`. Eigene SP/Flags, andere Register, zusätzliche Flavors und widersprüchliche Eintritte sind unzulässig. Hostumgebung und Linux-Hilfsvektor werden nicht übernommen. Grundlage: [dyld-Architektur](https://github.com/apple-oss-distributions/dyld/blob/main/doc/dyld4.md).

Externe Dylibs, Imports, Rebases/Chained Fixups, Konstruktoren/Destruktoren, TLS-Sektionen, arm64e/PAC, andere nicht unterstützte CPU-Untertypen, Verschlüsselung und nicht modellierte Ladebefehle scheitern vor Ausführung. PIE ohne Fixups verwendet bevorzugte Adressen, kein ASLR. Signaturblobs sind Metadaten und implementieren weder AMFI noch Entitlement-Regeln.

## Darwin-Dienste

ARM64 nutzt X16, X0–X5 und `svc #0x80`; x64 die BSD-Klasse `0x02000000`, RAX und RDI/RSI/RDX/R10/R8/R9. Erfolg löscht Carry, Fehler setzt Carry und liefert positives errno. ARM64 löscht X1; x64 löscht RDX nur bei Erfolg und erhält es bei Fehlern. SYSCALL-Registeränderungen sind explizit. Der Bericht kennzeichnet BSD-Fehler mit `result` und `error=true`; nicht zurückkehrende oder nicht unterstützte Aufrufe haben keines dieser Felder. Grundlage sind XNU-Eintrittspfade für [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c) und [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c); Apple-Code wurde nicht übernommen.

Unterstützt werden `exit`, `write`, `getpid`, `getppid`, `getuid`, `geteuid`, `getgid`, `getegid`, `mmap`, `mprotect`, `munmap`. PID/UID/GID sind 1000, PPID ist 1. Deskriptoren 1 und 2 erfassen Bytes einschließlich NUL und Nicht-UTF8; geschlossene oder nur lesbare Deskriptoren ergeben EBADF. Bei teilweisem Kopieren bleiben gelesene Bytes erhalten, der nachfolgende Zugriffsfehler bleibt EFAULT. Längen über `INT_MAX` ergeben EINVAL vor Deskriptor-, Zeiger- oder Budgetprüfung: [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).

Speicherdienste unterstützen private anonyme Datenmappings mit `flags=0x1002`, Deskriptor -1 und Offset null. Längen und nicht feste Adresshinweise werden auf OS-Seiten aufgerundet. Ein belegter Hinweis sucht zunächst aufwärts, dann am Standardort. Historisches rohes mmap mit Länge null liefert null ohne Allokation; `MAP_UNIX03` liegt außerhalb des Profils. Unmap/protect verlangen ausgerichtete Adressen. NONE/READ/WRITE sind möglich; WRITE impliziert READ. Jede OS-Seite besitzt ihre physische Allokation: Teil-Unmap gibt Budget frei, neu zugewiesene Seiten sind genullt. Ein protect über eine Lücke oder oberhalb maximaler Rechte lässt den gesamten Bereich unverändert. Referenz: [XNU-VM-Dienste](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

Datei-/Shared-/Fixed-/JIT-Mappings, ausführbare anonyme Mappings, Mach-Traps, indirekte Syscalls, Threads, Signale, Hostdateisystem/Netzwerk, dyld, Objective-C/Swift-Laufzeiten und Foundation/UIKit sind ausgeschlossen und stoppen ausdrücklich. Dies ist kein vollständiges Apple-OS und keine iOS-Simulator-Anwendung.

## Validierung

Eigene C-Fixtures werden mit Clang und `ld64.lld` ohne Apple-SDK oder proprietäre Binärdateien erzeugt. Sie decken fünf Plattform-/ISA-Kombinationen, fehlerhafte Mach-O-Datensätze, 4/16-KiB-Seiten und Teilfreigaben bei ausgeschöpftem Budget ab. `NeverDProcessPublicTests` vergleicht C API und CLI; `NEVERD_TEST_LIBNEVERD` und `NEVERD_TEST_DARWIN_FIXTURES` aktivieren dieselben fünf Kombinationen im Python-SDK.

## Explizite Dateien und Deskriptoren

`darwin_files` stellt allen drei Profilen einen geschlossenen Katalog schreibgeschützter Dateien bereit. Das Pflichtfeld `files` enthält kanonische absolute Gastpfade `path` und hexadezimale `bytes_hex`; optional liefert `stdin_hex` einen endlichen Eingabestrom. Fehlende Eingabe ist unbekannt und stoppt nichtleere Leseversuche, eine leere Zeichenfolge bedeutet EOF. Ohne Katalog stoppt open; ein ausdrücklich leerer Katalog ergibt ENOENT. Hostdateien und Hosteingabe werden nicht verwendet.

Hinzu kommen `open`, `read`, `pread`, `lseek`, `close`, `dup`, `dup2`, `fcntl` sowie die nocancel-Einstiege von read/write/open/close/fcntl/pread. Unterstützt sind O_RDONLY/O_CLOEXEC und F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL. Separate opens haben eigene Positionen, Duplikate teilen die Position mit separaten close-on-exec-Flags; pread ändert sie nicht. Schließen oder Ersetzen von 0/1/2 wirkt auf spätere I/O; duplizierte Ausgaben behalten Senke und Budget.

Grenzen: 256 Dateien, 16 MiB für Pfade/NUL/Dateien/Eingabe zusammen, Pfade unter 1024 Byte und Komponenten bis 255 Byte. `descriptor_limit` ist eine exklusive Grenze von 3–4096, Standard 256; JSON bleibt auf 64 KiB begrenzt. Ungültige Optionen scheitern vor dem Laden. read über INT_MAX ergibt EINVAL vor FD-Prüfung; EOF berührt das Ziel nicht, ungültige Ziele ergeben EFAULT. Teilweise beschreibbare Puffer stoppen vor Kopie oder Positionsänderung. SET/CUR/END-Fehler erhalten die Position. Relative Pfade, Verzeichnis-open, Schreiben, stat, Datei-Mappings, Sparse-Seeks und weitere fcntl bleiben ausgeschlossen. Dateien als Pfadvorfahren ergeben ENOTDIR. Dasselbe Objekt läuft als nativer macOS-Vergleich; C/CLI/Python prüfen fünf Gastkombinationen. Das ist kein iOS-Gerätenachweis.

Release-Prüfung vom 2026-10-05: 381 Registrierungen, 177 bestanden, 204 übersprungen, keine Fehler; alle 51/51 ARM64-HVF-Pflichtfälle liefen. Auch sieben native macOS-Programme, 35 öffentliche C/CLI-/Berichtsprüfungen, fünf Python-Gastkombinationen und 66 Prüflauf-Tests bestanden. Die Zahlen überlappen. Für die neuen Dateidienste fehlen native Intel-HVF/KVM/WHP-Nachweise. Intel HVF bleibt unvalidiert, seine Actions bleiben ausgesetzt. iOS-SDK und Gerätevergleich fehlen.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

Die eigene Workload-Prüfung verlangt alle 51 nativen ARM64- beziehungsweise 34 x64-Fälle, einschließlich `LC_MAIN` und `LC_UNIXTHREAD` auf jeder Plattform. Fehlende/übersprungene Pflichtfälle oder fehlendes `ld64.lld` führen zum Fehlschlag.

```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

Linux verwendet `kvm`, Windows `whp`. Der [Darwin-Workflow](../../.github/workflows/darwin-native.yml) prüft beide x64-Transporte ohne Unicorn und erlaubt einzelne Wiederholungsläufe. Die [Kernelreferenz](../../.github/workflows/darwin-kernel-reference.yml) führt dieselben Programme direkt auf beiden macOS-ISAs aus, ohne NeverD/LLVM. `DarwinNativeCases.def` besitzt Modi, Exitstatus und erwartete Bytes. Nur die Hostreferenz bindet libSystem für den echten dyld-Eintritt. Falsche ISA, Rosetta, Timeout oder Abweichungen scheitern. Diese Referenz belegt keinen iOS-Gerätekernel.

## Nachweise und verbleibender Umfang

Stand 2026-10-03; überlappende Zeilen nicht addieren:

| Transport | Quelle | Bestanden | Fehler | Übersprungen | Native Workloads |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 HVF | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel HVF | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| x64 KVM | `36e11ca8a` | 51 | 0 | 235 | 26/26 |
| x64 WHP | `36e11ca8a` | 51 | 0 | 235 | 26/26 |

Der [Intel-Lauf](https://github.com/NeverSight/NeverD/actions/runs/37106013999) gleicht 286 CTest-Identitäten und 32 Prozesse mit Original-XML ab. Die 234 übersprungenen Fälle sind 65 deaktivierte Unicorn-Fälle, 39 ARM64-Gäste und 130 andere Hostplattformen. Artefakt `11267489438` hat den verifizierten SHA-256 `cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`. Auch [KVM/WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703) wurden unabhängig geprüft. Die [Kernelreferenz](https://github.com/NeverSight/NeverD/actions/runs/37064795867) bestand 4/4 Programme auf jeder ISA, mit Status 37, exakter Ausgabe und leerem stderr.

C API/CLI mit Unicorn: 138 bestanden, 156 übersprungen, keine Fehler. Python deckt alle fünf Kombinationen ab. Die Paketengine stimmt mit 18 ARM64-CLI-Berichten überein; 186 Mach-O-Signaturen wurden geprüft. HVF/Unicorn OFF bestand 38 Prüfungen, übersprang 231 und linkte Hypervisor.framework nicht. Das sind Integrationsnachweise, keine zusätzlichen nativen Ausführungen. Die vollständige Intel-CPU-Abnahme bleibt offen; siehe [HVF](macos-hvf.md) und den [ausführlichen Nachweis](../darwin-emulation.md#hosted-native-verification-2026-10-03).
