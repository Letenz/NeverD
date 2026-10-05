**Sprachen**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 651fc905db4bb2d62a1036bb6495b60cad23dde9b5e655b4b4ca408294a3d83c -->

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

Speicherdienste unterstützen private anonyme Datenmappings mit `flags=0x1002`, Deskriptor -1 und Offset null. Längen und nicht feste Adresshinweise werden auf OS-Seiten aufgerundet. Ein belegter Hinweis sucht zunächst aufwärts, dann am Standardort. Historisches rohes mmap mit Länge null liefert null ohne Allokation; `MAP_UNIX03` wird unterstützt und weist Länge null mit EINVAL ab. Unmap/protect verlangen ausgerichtete Adressen. NONE/READ/WRITE sind möglich; WRITE impliziert READ. Jede OS-Seite besitzt ihre physische Allokation: Teil-Unmap gibt Budget frei, neu zugewiesene Seiten sind genullt. Ein protect über eine Lücke oder oberhalb maximaler Rechte lässt den gesamten Bereich unverändert. Referenz: [XNU-VM-Dienste](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

Shared-/Fixed-/JIT-Mappings, ausführbare anonyme Mappings, Mach-Traps, indirekte Syscalls, Threads, Signale, Hostdateisystem/Netzwerk, dyld, Objective-C/Swift-Laufzeiten und Foundation/UIKit sind ausgeschlossen und stoppen ausdrücklich. Dies ist kein vollständiges Apple-OS und keine iOS-Simulator-Anwendung.

## Validierung

Eigene C-Fixtures werden mit Clang und `ld64.lld` ohne Apple-SDK oder proprietäre Binärdateien erzeugt. Sie decken fünf Plattform-/ISA-Kombinationen, fehlerhafte Mach-O-Datensätze, 4/16-KiB-Seiten und Teilfreigaben bei ausgeschöpftem Budget ab. `NeverDProcessPublicTests` vergleicht C API und CLI; `NEVERD_TEST_LIBNEVERD` und `NEVERD_TEST_DARWIN_FIXTURES` aktivieren dieselben fünf Kombinationen im Python-SDK.

## Explizite Dateien und Deskriptoren

`darwin_files` stellt allen drei Profilen einen geschlossenen Katalog schreibgeschützter Dateien bereit. Das Pflichtfeld `files` enthält kanonische absolute Gastpfade `path` und hexadezimale `bytes_hex`; optional liefert `stdin_hex` einen endlichen Eingabestrom. Fehlende Eingabe ist unbekannt und stoppt nichtleere Leseversuche, eine leere Zeichenfolge bedeutet EOF. Ohne Katalog stoppt open; ein ausdrücklich leerer Katalog ergibt ENOENT. Hostdateien und Hosteingabe werden nicht verwendet.

Hinzu kommen `open`, `read`, `pread`, `lseek`, `close`, `dup`, `dup2`, `fcntl` sowie die nocancel-Einstiege von read/write/open/close/fcntl/pread. Unterstützt sind O_RDONLY/O_CLOEXEC und F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL. Separate opens haben eigene Positionen, Duplikate teilen die Position mit separaten close-on-exec-Flags; pread ändert sie nicht. Schließen oder Ersetzen von 0/1/2 wirkt auf spätere I/O; duplizierte Ausgaben behalten Senke und Budget.

Grenzen: 256 Dateien, 16 MiB für Pfade/NUL/Dateien/Eingabe zusammen, Pfade unter 1024 Byte und Komponenten bis 255 Byte. `descriptor_limit` ist eine exklusive Grenze von 3–4096, Standard 256; JSON bleibt auf 64 KiB begrenzt. Ungültige Optionen scheitern vor dem Laden. read über INT_MAX ergibt EINVAL vor FD-Prüfung; EOF berührt das Ziel nicht, ungültige Ziele ergeben EFAULT. Teilweise beschreibbare Puffer stoppen vor Kopie oder Positionsänderung. SET/CUR/END-Fehler erhalten die Position. Schreiben, altes stat, Sparse-Seeks und weitere fcntl bleiben ausgeschlossen. Dateien als Pfadvorfahren ergeben ENOTDIR. Dasselbe Objekt läuft als nativer macOS-Vergleich; C/CLI/Python prüfen fünf Gastkombinationen. Das ist kein iOS-Gerätenachweis.

Release-Prüfung vom 2026-10-05: 381 Registrierungen, 177 bestanden, 204 übersprungen, keine Fehler; alle 51/51 ARM64-HVF-Pflichtfälle liefen. Auch sieben native macOS-Programme, 35 öffentliche C/CLI-/Berichtsprüfungen, fünf Python-Gastkombinationen und 66 Prüflauf-Tests bestanden. Die Zahlen überlappen. Für die neuen Dateidienste fehlen native Intel-HVF/KVM/WHP-Nachweise. Intel HVF bleibt unvalidiert, seine Actions bleiben ausgesetzt. iOS-SDK und Gerätevergleich fehlen.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## Verzeichnisse und relative Pfade

Optionale `directories` enthalten einen kanonischen absoluten `path` und optional vollständige `metadata`, auch für leere Verzeichnisse. Wurzel und Vorfahren sind implizit; Metadaten erzeugen keine fehlenden Pfade. Modus: `0x4000` plus Rechte; size: explizite Beobachtung in [0, INT64_MAX]. `working_directory` muss ein vorhandenes Verzeichnis bezeichnen; ohne Angabe bleibt CWD unbekannt und wird nicht vom Host übernommen. Maximal 256 angegebene Pfade einschließlich Vorfahren mit Metadaten; Pfade/NUL/Inhalt/Eingabe/CWD zusammen 16 MiB.

`openat` (463), `openat_nocancel` (464), `chdir` (12), `fchdir` (13) und `fstatat64` (470) teilen die Auflösung. Relative Pfade verwenden einen Verzeichnis-FD oder `AT_FDCWD=-2`; absolute ignorieren den FD. Wiederholte Schrägstriche, `.`, `..` und abschließende Schrägstriche prüfen alle Vorfahren: `/file/..` ergibt ENOTDIR, `/missing/..` ENOENT. Fehler sowie Schließen, Wiederverwenden oder Ersetzen des ursprünglichen FD erhalten CWD. `F_GETPATH=50` kopiert auch nach dup den kanonischen Pfad samt NUL und erhält nachfolgende Bytes.

Verzeichnis-read/pread ergibt selbst bei Länge null EISDIR; negative pread-Offsets zuerst EINVAL. SET/CUR teilen den Cursor, END benötigt explizite size; mmap ergibt EINVAL. fstatat64 akzeptiert 0, `AT_SYMLINK_NOFOLLOW=0x20`, `AT_SYMLINK_NOFOLLOW_ANY=0x800` und `AT_FDONLY=0x400` (Pfad ignoriert). Ungültige Bits ergeben EINVAL; `AT_REALDEV=0x200` bleibt ausgeschlossen. Streamidentität bleibt unbekannt, Rechte sind kein Zugriffskontrollmodell; Änderungen fehlen noch. Dasselbe `directories` vergleicht nativen Kernel und fünf Gäste; stat prüft echte Dateien und Verzeichnisse. Intel-HVF-Actions bleiben ausgesetzt.

[XNU VFS](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/fcntl.h).

Verzeichnisprüfung (2026-10-05, Release): 467 Registrierungen, 227 bestanden, 240 übersprungen, keine Fehler; alle 60/60 ARM64-HVF-Pflichtfälle ausgeführt. Zehn native macOS-Programme, 37 C/CLI/Berichtstests ohne Auslassung, fünf Python-Gäste und 66 Werkzeugtests bestanden. Zahlen überlappen. Nachweise: `build-hvf-arm64/darwin-directory-verified-evidence/`. Andere native Backends und physisches iOS bleiben ungeprüft.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## Explizite Verzeichnisschnappschüsse

`getdirentries64` (344) liest den optionalen unveränderlichen `contents` eines vorhandenen `directories`-Eintrags; C++ verwendet `DarwinFileOptions::DirectoryContents`. `entries` enthält in expliziter Reihenfolge alle unmittelbaren Kinder einschließlich `.` und `..`. Ohne Schnappschuss bleibt auch ein leeres Verzeichnis unbekannt. Pfade, stat-Daten oder Hostzugriffe werden nicht abgeleitet.

Jeder Eintrag benötigt `name`, eine von null verschiedene `inode`, `type` (0 unbekannt, 4 Verzeichnis, 8 Datei), `next_offset` und `seek_offset`. Typ und Pfad sowie Inodes desselben aufgelösten Pfades in allen Schnappschüssen und Metadaten müssen übereinstimmen. `next_offset` ist innerhalb des Verzeichnisses eindeutig, positiv und <=INT64_MAX; aufsteigende Werte sind nicht nötig. Null setzt zurück. `seek_offset` ist ein gesonderter vorzeichenloser 64-Bit-d_seekoff-Wert, auch mehrfach null. Ganzzahlen verwenden die verlustfreien Dezimalzeichenfolgen von stat.

`contents.minimum_buffer_size` ist ein erforderliches Nutzdatenminimum von 1–128 MiB, auch bei EOF. Optionales `minimum_buffer_size` eines Eintrags (Standard 0) gilt beim Start an dieser Position. Das Beispiel hält APFS-Beobachtungen fest: 64 Bytes für beide ersten Punkteinträge, 1 bei EOF; sonst muss mindestens ein ganzer Datensatz passen. LP64-Datensätze sind achtfach ausgerichtet, Größe `roundUp(25 + nameBytes, 8)`. Insgesamt höchstens 4096 Einträge; deren Bytes zählen zu 16 MiB. Nur durch Metadaten/Schnappschuss deklarierte Vorfahren zählen einmal zur Grenze von 256 Pfaden. JSON bleibt auf 64 KiB begrenzt.

Unabhängige open-Aufrufe haben eigene Cursor, dup teilt sie. Nur null oder angegebene Werte erlauben Fortsetzung; unbekannte Positionen stoppen ausdrücklich. Zurückgegeben wird das größtmögliche Präfix ganzer Datensätze. Länge >=1024 reserviert die letzten vier angeforderten Bytes für EOF (am Ende 1, sonst 0); nur die Nutzdaten werden auf 128 MiB begrenzt. Die Flagadresse behält die ursprüngliche vorzeichenlose Rechnung samt Überlauf. Reihenfolge: Daten, Cursorfortschritt, ursprüngliche Position, Flags. Späteres EFAULT erhält frühere Effekte; EOF überspringt die leere Datenkopie. Eine nur teilweise schreibbare Einzelkopie stoppt vor dieser Kopie, ohne frühere Effekte zurückzunehmen.

`directory-entries` vergleicht Felder, dup/Zurücksetzen, kleine Lesevorgänge, EOF und Kopierreihenfolge mit macOS. Ein separater Test vergleicht sämtliche erfassten nativen Bytes samt langen Namen mit dem SDK-Layout. Feste Cookies bilden dynamische APFS-Generationen nicht nach. Altes `getdirentries` (196), Änderungen, andere native Backends und physisches iOS bleiben außerhalb dieser Abnahme.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"directories":[{"path":"/empty"},{"path":"/","contents":{
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":73,"type":8,"next_offset":99,"seek_offset":0}]}}]}}
```

[XNU getdirentries64](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [dirent ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [extended flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent_private.h).

Auflistungsprüfung (2026-10-05, Release): 498 Darwin-Fälle, 246 bestanden, 252 wegen fehlender Backends übersprungen, keine Fehler; alle 63/63 ARM64-HVF-Pflichtfälle ausgeführt. Elf native macOS-Programme, 40 C/CLI/Berichtsprüfungen ohne Auslassung, fünf Python-Gastkombinationen mit jeweils acht Dateiszenarien und 66 Werkzeugtests bestanden. Die Zahlen überschneiden sich. Belege: `build-hvf-arm64/darwin-dirents-merged-evidence/`. Intel-HVF-Actions bleiben ausgesetzt; andere native Backends und physisches iOS sind nicht bestätigt.


## Private Dateimappings

`mmap` unterstützt reguläre Katalogdateien mit `MAP_PRIVATE`: `flags=0x2` oder `0x40002` mit `MAP_UNIX03` und OS-seitenausgerichtetem Offset. Auch kurze Längen behalten alle Dateibytes der Seite; der Rest der letzten EOF-Seite ist null. Private Schreibzugriffe ändern nur dieses Mapping, nicht Datei, weitere Mappings, feste Metadaten oder gemeinsame Dateiposition. Das Mapping überlebt close und FD-Wiederverwendung. Auch schreibgeschützte und PROT_NONE-Mappings erhalten Anfangsdaten; `mprotect` kann Schreiben erlauben.

Überlauf des Dateiendes, UNIX03-Länge null oder nicht ausgerichtete UNIX03-Offsets ergeben EINVAL vor FD-Suche; ungültige FD ergeben EBADF vor Budgetprüfung. Historische Länge null prüft weiterhin den FD. Historische nicht ausgerichtete Offsets, Streams, leere Dateiseiten und vollständige Seiten hinter EOF stoppen vor Allokation. macOS lässt EOF-Mappings zu, erzeugt beim Zugriff aber SIGBUS; das Modell erfindet weder lesbare Nullseiten noch Signalzustellung. Gemeinsame, feste, ausführbare und JIT-Mappings bleiben ausgeschlossen.

`DarwinFiles` löst FD und Bytes auf; `DarwinMemory` verwaltet Platzierung, Rechte, Budget und Rücknahme. Daten stammen nur aus `darwin_files`. Das gemeinsame Programm `file-mapping` prüft Kopien, close, Positionen, Fehler und anonyme Wiederverwendung. Ein separater nativer Vergleich prüft einen Offset ungleich null, jedes Seitenbyte und SIGBUS.

[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c)

### Prüfung privater Mappings, 2026-10-05

Release Darwin: 438 eindeutige Registrierungen, 210 bestanden, 228 übersprungen, keine Fehler. Alle 57/57 ARM64-HVF-Pflichtfälle und fünf Unicorn-Gastkombinationen liefen. Neun native macOS-Programme, vollständiger Seitenvergleich bei Offset ungleich null und SIGBUS im isolierten Kindprozess bestanden. 36 öffentliche API-/Berichtsprüfungen liefen ohne Auslassung; Python prüfte fünf Kombinationen einschließlich `file-mapping`. Auch 66 Werkzeug- und 38 Herkunftsregressionen bestanden; Zahlen überlappen. Nachweise: `build-hvf-arm64/darwin-mmap-verified-evidence/`. Keine neuen Intel-HVF/KVM/WHP- oder physischen iOS-Nachweise; Intel-HVF-Actions bleiben ausgesetzt.

## Explizite Dateimetadaten

Ein Dateieintrag kann `metadata` enthalten; dann sind alle unten gezeigten Felder erforderlich. Dezimalzeichenfolgen erhalten die volle Ganzzahlbreite; JSON-Zahlen müssen exakte Ganzzahlen innerhalb ±(2^53−1) sein. device ist 32 Bit mit Vorzeichen, mode/link_count sind 16 Bit ohne Vorzeichen, inode 64 Bit ohne Vorzeichen und uid/gid/flags/generation 32 Bit ohne Vorzeichen. size muss der Bytezahl entsprechen; blocks passt in vorzeichenbehaftete 64 Bit, block_size in nichtnegative vorzeichenbehaftete 32 Bit. Zeiten verwenden vorzeichenbehaftete 64-Bit-Sekunden und 0–999999999 Nanosekunden.

`stat64` (338), `fstat64` (339) und `lstat64` (340) liefern auf ARM64/x64 denselben 144-Byte-LP64-Datensatz. Sie teilen die Pfadauflösung mit open, beachten dup/close und ändern weder FD-Belegung noch Cursor. rdev, Füllbytes und Reserven sind null. Die Beobachtungen bleiben fest: read aktualisiert keine Zeitstempel, mode ändert keine Katalogzugriffsrechte. Fehlende Metadaten, Streamstatus, symbolische Links, altes stat, und erweiterte Sicherheit bleiben ausgeschlossen. Pfad-/FD-Fehler gehen dem Ausgabezeiger voraus; teilweise beschreibbare Ausgabe wird vor jedem Schreiben abgelehnt. Native Tests vergleichen alle Bytes einer echten Datei und die SDK-Offsets; dasselbe eigene Programm prüft alle drei Aufrufe.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","metadata":{
  "device":1,"inode":"18364758544493064720","mode":33188,"link_count":1,
  "uid":1000,"gid":1000,"size":10,"block_size":4096,"blocks":8,
  "flags":0,"generation":0,
  "access_time":{"seconds":-1,"nanoseconds":1},
  "modification_time":{"seconds":2,"nanoseconds":3},
  "change_time":{"seconds":4,"nanoseconds":5},
  "birth_time":{"seconds":6,"nanoseconds":7}
}}]}}
```

[XNU stat.h](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/sys/stat.h)

### Metadatenprüfung und nächste Schritte (2026-10-05)

Mit stat64: 409 eindeutige Registrierungen, 193 bestanden, 216 übersprungen, kein Fehler; alle 54/54 ARM64-HVF-Pflichtfälle liefen, Unicorn deckte fünf Gastkombinationen ab. SDK-/Originaldatensatzvergleich, acht native Programme, 36 API-/Berichtsfälle ohne Auslassung, fünf Python-Kombinationen und 66 Werkzeugtests bestanden; die Zahlen überschneiden sich. Jeder native Fall hat nun eine eigene Ausgabedatei, sodass kürzere Ausgaben keine alten Endbytes behalten. Für die Ergänzungen fehlen native Intel-HVF/KVM/WHP- und physische iOS-Belege.

Als Nächstes: Shared-Mappings und EOF-Seitenfehler, begrenztes Schreiben (EOF-Seiten, Lebensdauer nach close, Fehlerreihenfolge), explizite Zeit-/Systembeobachtungen, nötige Mach-/Thread-Dienste sowie Mach-O-Abhängigkeiten, Rebases/Binds, Initialisierung und TLS. Objective-C/Swift und Foundation/UIKit brauchen ausführbare native Referenzen. Physisches iOS benötigt SDK und Gerät; Intel HVF bleibt unbestätigt, seine Actions bleiben ausgesetzt.



```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

Die eigene Workload-Prüfung verlangt alle 60 nativen ARM64- beziehungsweise 40 x64-Fälle, einschließlich `LC_MAIN` und `LC_UNIXTHREAD` auf jeder Plattform. Fehlende/übersprungene Pflichtfälle oder fehlendes `ld64.lld` führen zum Fehlschlag.

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
