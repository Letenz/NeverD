**Sprachen**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 8828ab2cc1f47bed24529d8d47a05ee929790480d47f96fcb54d6ebc101ae2ce -->

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

BSD-Aufrufe auf ARM64 nutzen X16, X0–X5 und `svc #0x80`; x64 die BSD-Klasse `0x02000000`, RAX und RDI/RSI/RDX/R10/R8/R9. Erfolg löscht Carry, Fehler setzt Carry und liefert positives errno. ARM64 löscht X1; x64 löscht RDX nur bei Erfolg und erhält es bei Fehlern. SYSCALL-Registeränderungen sind explizit. Der Bericht kennzeichnet BSD-Fehler mit `result` und `error=true`; nicht zurückkehrende oder nicht unterstützte Aufrufe haben keines dieser Felder. Grundlage sind XNU-Eintrittspfade für [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c) und [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c); Apple-Code wurde nicht übernommen.

Unterstützt werden `exit`, `write`, `getpid`, `getppid`, `getuid`, `geteuid`, `getgid`, `getegid`, `mmap`, `mprotect`, `munmap`. PID/UID/GID sind 1000, PPID ist 1. Deskriptoren 1 und 2 erfassen Bytes einschließlich NUL und Nicht-UTF8; geschlossene oder nur lesbare Deskriptoren ergeben EBADF. Bei teilweisem Kopieren bleiben gelesene Bytes erhalten, der nachfolgende Zugriffsfehler bleibt EFAULT. Längen über `INT_MAX` ergeben EINVAL vor Deskriptor-, Zeiger- oder Budgetprüfung: [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).

Speicherdienste unterstützen private anonyme Datenmappings mit `flags=0x1002`, Deskriptor -1 und Offset null. Längen und nicht feste Adresshinweise werden auf OS-Seiten aufgerundet. Ein belegter Hinweis sucht zunächst aufwärts, dann am Standardort. Historisches rohes mmap mit Länge null liefert null ohne Allokation; `MAP_UNIX03` wird unterstützt und weist Länge null mit EINVAL ab. Unmap/protect verlangen ausgerichtete Adressen. NONE/READ/WRITE sind möglich; WRITE impliziert READ. Jede OS-Seite besitzt ihre physische Allokation: Teil-Unmap gibt Budget frei, neu zugewiesene Seiten sind genullt. Ein protect über eine Lücke oder oberhalb maximaler Rechte lässt den gesamten Bereich unverändert. Referenz: [XNU-VM-Dienste](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

Shared-/Fixed-/JIT-Mappings, ausführbare anonyme Mappings, andere Mach-Traps, indirekte Syscalls, Threads, Signale, Hostdateisystem/Netzwerk, dyld, Objective-C/Swift-Laufzeiten und Foundation/UIKit sind ausgeschlossen und stoppen ausdrücklich. Dies ist kein vollständiges Apple-OS und keine iOS-Simulator-Anwendung.

## Validierung

Eigene C-Fixtures werden mit Clang und `ld64.lld` ohne Apple-SDK oder proprietäre Binärdateien erzeugt. Sie decken fünf Plattform-/ISA-Kombinationen, fehlerhafte Mach-O-Datensätze, 4/16-KiB-Seiten und Teilfreigaben bei ausgeschöpftem Budget ab. `NeverDProcessPublicTests` vergleicht C API und CLI; `NEVERD_TEST_LIBNEVERD` und `NEVERD_TEST_DARWIN_FIXTURES` aktivieren dieselben fünf Kombinationen im Python-SDK.

## Explizite Dateien und Deskriptoren

`darwin_files` stellt allen drei Profilen einen geschlossenen Katalog zunächst schreibgeschützter Dateien bereit. Das Pflichtfeld `files` enthält kanonische absolute Gastpfade `path` und hexadezimale `bytes_hex`; optional liefert `stdin_hex` einen endlichen Eingabestrom. Fehlende Eingabe ist unbekannt und stoppt nichtleere Leseversuche, eine leere Zeichenfolge bedeutet EOF. Ohne Katalog stoppt open; ein ausdrücklich leerer Katalog ergibt ENOENT. Hostdateien und Hosteingabe werden nicht verwendet.

Hinzu kommen `open`, `read`, `pread`, `lseek`, `close`, `dup`, `dup2`, `fcntl` sowie die nocancel-Einstiege von read/write/open/close/fcntl/pread. Unterstützt sind O_RDONLY/O_CLOEXEC und F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL. Separate opens haben eigene Positionen, Duplikate teilen die Position mit separaten close-on-exec-Flags; pread ändert sie nicht. Schließen oder Ersetzen von 0/1/2 wirkt auf spätere I/O; duplizierte Ausgaben behalten Senke und Budget.

Grenzen: 256 Dateien, 16 MiB für Pfade/NUL/Dateien/Eingabe zusammen, Pfade unter 1024 Byte und Komponenten bis 255 Byte. `descriptor_limit` ist eine exklusive Grenze von 3–4096, Standard 256; JSON bleibt auf 64 KiB begrenzt. Ungültige Optionen scheitern vor dem Laden. read über INT_MAX ergibt EINVAL vor FD-Prüfung; EOF berührt das Ziel nicht, ungültige Ziele ergeben EFAULT. Teilweise beschreibbare Puffer stoppen vor Kopie oder Positionsänderung. SET/CUR/END-Fehler erhalten die Position. Altes stat und weitere fcntl bleiben ausgeschlossen. Dateien als Pfadvorfahren ergeben ENOTDIR. Dasselbe Objekt läuft als nativer macOS-Vergleich; C/CLI/Python prüfen fünf Gastkombinationen. Das ist kein iOS-Gerätenachweis.

Release-Prüfung vom 2026-10-05: 381 Registrierungen, 177 bestanden, 204 übersprungen, keine Fehler; alle 51/51 ARM64-HVF-Pflichtfälle liefen. Auch sieben native macOS-Programme, 35 öffentliche C/CLI-/Berichtsprüfungen, fünf Python-Gastkombinationen und 66 Prüflauf-Tests bestanden. Die Zahlen überlappen. Für die neuen Dateidienste fehlen native Intel-HVF/KVM/WHP-Nachweise. Intel HVF bleibt unvalidiert, seine Actions bleiben ausgesetzt. iOS-SDK und Gerätevergleich fehlen.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## Vorhandene Dateien ändern

Das strikte Boolean `"writable":true` beziehungsweise `DarwinFileOptions::WritableFiles` erlaubt Änderungen innerhalb des Prozesses. Fehlend/false bleibt schreibgeschützt; unbekannte Berechtigung stoppt. Host und übergebene Anfangsdaten bleiben unverändert. write(4/397), pwrite(154/415), truncate(200), ftruncate(201) und O_TRUNC teilen Inhalte; open besitzt eigene Positionen, dup teilt Position und Status. Inhalte überleben den letzten close. Wachstum füllt mit Nullen, Kürzung erhält Positionen, auch bei O_RDONLY|O_TRUNC.

F_SETFL ändert nur O_APPEND und erhält Zugriff, close-on-exec und FWASWRITTEN. F_GETFL zeigt nach tatsächlich übertragenen Bytes 0x10000, auch bei pwrite und erfasster Ausgabe. pwrite ignoriert append und erhält die Position. INT_MAX wird vor FD geprüft, pwrite mit -1 liefert noch früher EINVAL. INT64_MAX liefert EFBIG vor der Leerprüfung; die Länge wird vor Wahl des EOF gekürzt.

Erfolgreiches ftruncate setzt FWASWRITTEN auch bei gleicher Größe auf der aufgerufenen Beschreibung und deren dup. O_TRUNC setzt es auf der neuen Beschreibung, auch bei O_RDONLY; pfadbasiertes truncate verändert keine vorhandenen Beschreibungsflags.

Teilweise lesbare Eingaben stoppen vor Effekten. Vollständiges EFAULT erhält Bytes, nichtleeres append setzt die Position jedoch auf EOF. Transportfehler übernehmen weder Inhalt noch Position. Ohne `mutation_policy` verwerfen nichtleere Schreibvorgänge, Kürzungen und vollständiges nichtleeres EFAULT die komplette stat-Beobachtung; spätere Abfragen stoppen vor Ausgabe. Leere Writes erhalten sie. 16 MiB zählen Pfade/NUL, Eingabe, Verzeichnisdaten, CWD, aktuelle Inhalte und Referenzen schreibbarer Pfade. Kürzung ersetzt den Speicher und gibt Kapazität frei; Anfangsdaten und ein begrenzter Ersatzpuffer kommen hinzu. Bekannte inode-Aliase und immutable/append-only-Flags werden abgelehnt.

DarwinMemory hält Mapping-Leases bis zum letzten unmap, auch bei PROT_NONE oder geschlossenen FDs; Änderungen bleiben bis dahin gesperrt. Fehler und alte Null-Längen-Mappings behalten keine Lease. Neue Mappings sehen aktuelle Bytes. O_WRONLY mit READ/WRITE ergibt EACCES; PROT_NONE darf später per mprotect lesen/schreiben.

Originale normale/nocancel-Programme vergleichen den nativen Kernel; 4K/16K-Tests sowie C/CLI/Python prüfen fünf Kombinationen. Rechteprüfung, Verzeichnislöschung, Umbenennen zwischen Elternverzeichnissen, Hardlinks, native Dateisystem-Metadaten, Mapping-Kohärenz und EOF-SIGBUS fehlen weiterhin. Vollständige Umgebung, iOS-Gerät und Intel HVF sind nicht abgenommen; Intel-Actions bleiben ausgesetzt.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233","writable":true}]}}
```

[XNU write](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU vnode](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c).

## Explizite veränderliche Metadaten

Eine Datei kann neben `writable: true` und vollständigen metadata eine mutation_policy angeben; C++ nutzt `DarwinFileOptions::MutationPolicies`. Dies ist ein expliziter virtueller Vertrag für lückenhafte Belegung, keine Ableitung von APFS oder Hostzeit. Ohne Policy bleiben Metadaten nach Änderungen unbekannt.

allocation_unit, mutation_time und seconds/nanoseconds sind Pflichtfelder mit den bestehenden verlustfreien Ganzzahlregeln. Die Einheit ist eine Zweierpotenz von512 Bytes bis16 MiB, unabhängig von block_size und VM-Seiten. Erforderlich sind normale Rechte ohne set-id/sticky, flags=0, link_count=1 und anfänglich dichte Belegung: blocks=ceil(size/allocation_unit)*(allocation_unit/512). Nullbytes beweisen keine Lücken. Policy-Pfadreferenzen zählen zum16-MiB-Limit; die Belegungsbilanz erzeugt kein angenommenes ENOSPC.

Writes belegen alle berührten Einheiten, auch Nullen in Lücken. truncate-Wachstum fügt nur Nullbytes hinzu; Kürzen verwirft Einheiten hinter aufgerundetem EOF und behält die letzte teilweise belegte Einheit. Erneutes Wachstum stellt verworfene Belegung nicht wieder her. Erfolgreiche nichtleere Writes und jedes erfolgreiche truncate, auch bei gleicher Größe oder leerem O_TRUNC, aktualisieren size/blocks und setzen mtime/ctime auf die feste Vorgabe. Andere Felder und Eingaben bleiben erhalten; Lesen erhöht atime nicht. Pfad-stat, unabhängige open, dup und Wiederöffnen teilen denselben Knoten.

Leere Writes, Budget-/Mapping-Ablehnung, abgelehnte Teilzugriffe und Backendfehler erhalten bekannte Metadaten. Vollständiges nichtleeres EFAULT macht sie unbekannt; späterer Erfolg stellt sie nicht wieder her. Fehler beim stat-Ausgeben ändern den Knoten nicht. virtual-file-metadata prüft144 Bytes über fünf Profile und C/CLI/Python: Policy-Test, kein APFS-Gleichheitsnachweis. Native Programme prüfen Flags, Positionen und Fehler separat. Namensraum, native Kohärenz, Mach und dynamische Laufzeit fehlen weiterhin.

```json
{"mutation_policy":{"allocation_unit":4096,"mutation_time":{"seconds":-7,"nanoseconds":123456789}}}
```



## Positionierung in Dateien mit Lücken

Mit mutation_policy und bekannter Belegung unterstützt lseek SEEK_HOLE=3 und SEEK_DATA=4 für reguläre Dateien und liest dieselbe Bilanz wie stat. Anfangsdaten sind dicht belegt, auch Nullbytes. Innerhalb einer passenden Einheit gilt der Eingabeoffset, sonst der Anfang der nächsten passenden Einheit. Die letzte Lücke beginnt bei EOF. Negative Werte ergeben EINVAL; ab EOF, auch bei leeren Dateien, oder ohne spätere Daten gilt ENXIO=6. Fehler erhalten den Cursor; Erfolg ändert nur diese Beschreibung und dup. Andere open behalten eigene Cursor, Wiederöffnen sieht aktuelle Belegung. Metadaten, Flags und Bytes bleiben gleich; hohe whence-Bits werden ignoriert.

Ohne Policy, bei Verzeichnissen oder nach vollständigem EFAULT mit unbekannter Belegung bleibt der Aufruf unsupported. Nullwerte und abgelehnte Änderungen begründen keine Belegung. Das eigene sparse-file-seek vergleicht native/Gast-Fehler, geschriebene Bytes, EOF und Beschreibungslaufzeit ohne Annahmen über frühere FS-Extents. virtual-file-metadata prüft genaue Policy-Geometrie getrennt; C/CLI/Python decken fünf Profile ab.

[XNU lseek](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Namen regulärer Dateien entfernen

`mutable:true` pro Verzeichnis (C++ `MutableDirectories`) erlaubt ausdrücklich Änderungen direkter Namen, unabhängig vom Dateiinhalt `writable`. Ohne Freigabe wird abgebrochen. Bekannte Flags ungleich null, besondere Elternrechte, link_count≠1 des Kindes sowie bekannte Eltern-/Kind-Aliase werden abgewiesen. Identitäten kombinieren stat und Snapshot-Inodes; ausdrücklich verschiedene Geräte bleiben getrennt. Pfade zählen zum bestehenden Budget.

`unlink(10)` / `unlinkat(472)` entfernen vorhandene reguläre Namen. unlinkat unterstützt nur die unteren32 Bits 0 oder `0x800`; unbekannte Bits liefern EINVAL vor Pfad/FD, andere bekannte Löschmodi bleiben unmodelliert. Gemeinsame Auflösung: ENOENT, ENOTDIR nach Datei mit `/`, EPERM für normale Verzeichnisse, EBUSY für die Wurzel. Endkomponenten `.`/`..` wurden nativ geprüft.

Alte FD/dup/unabhängige Opens behalten Daten, Position und Flags; F_GETPATH liefert den erfassten alten Pfad. Neue Opens scheitern, implizite Eltern und CWD bleiben. Schreibfreigaben gehören zum Objekt; das aktuelle Bytebudget wird erst nach letztem Deskriptor und letzter Mapping-Range durch close/dup2/nächste Mutation zurückgewonnen. Ursprüngliche Pfadkosten bleiben; Rechteprüfung, Umbenennung zwischen Elternverzeichnissen, Hardlinks und Verzeichnislöschung fehlen noch.

Eltern-stat/readdir/SEEK_END werden für alte/neue FD und Pfade unbekannt und stoppen vor Kopie/Cursoränderung. read/pread bleiben EISDIR; SET/CUR/F_GETPATH/fchdir/relative Auflösung funktionieren weiter. Eine bekannte Richtlinie setzt nur nlink=0 und feste ctime; spätere Schreibvorgänge stellen nlink=1 nicht wieder her. Ohne Richtlinie/nach EFAULT bleiben Metadaten unbekannt. Fehler erhalten den Zustand. `unlinked-file` vergleicht native Namen-/FD-Regeln; Zeit und Invalidierung sind explizite Modellregeln.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"00"}],"directories":[{"path":"/work","mutable":true}]}}
```

[XNU unlink / unlinkat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [F_GETPATH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c).

## Reguläre Dateien erstellen

O_CREAT=0x200 erstellt eine leere Datei direkt unter einem explizit mutable Elternverzeichnis, über normales/nocancel open und openat. Neue Objekte sind beschreibbar; vorhandene behalten ihre WritableFiles-Freigabe. Ein Nur-Lese-FD kann erstellen, aber nicht schreiben. Ohne explizite Erstellungsrichtlinie bleiben stat64 und Sparse-Suche unbekannt. metadata/mutation_policy eines früheren gleichnamigen Objekts werden nie übernommen.

O_EXCL=0x800 mit O_CREAT liefert bei vorhandenen Dateien/Verzeichnissen EEXIST vor Kürzung; allein ist es wirkungslos. Ein vorhandenes Verzeichnis lässt sich mit Nur-Lese-O_CREAT öffnen. Reihenfolge: ungültiger Zugriffsmodus, FD-Platz, EINVAL für O_CREAT|O_DIRECTORY, Pfad. Nur die letzte ursprüngliche fehlende Komponente kann entstehen; fehlende Vorfahren und `/`, `//`, `/.`, `/..` am Ende liefern ENOENT. Neues O_CREAT|O_TRUNC setzt FWASWRITTEN nicht, Kürzung vorhandener Dateien dagegen schon.

Nur Einfügen invalidiert Elternbeobachtungen. Gleichnamige alte/neue Objekte behalten getrennte Daten, FD, Metadaten und Mapping-Leases. 256 Einträge umfassen feste ursprüngliche Nicht-Datei-Einträge und lebende Dateien; dynamische kanonische Pfade/NUL und aktuelle Bytes zählen zu 16 MiB. Nach unlink gibt erst der letzte FD/Mapping die dynamischen Kosten frei, ursprüngliche Kosten bleiben. Budgetende oder kanonische Pfade ab 1024 Bytes stoppen ausdrücklich ohne erfundenes ENOSPC oder natives Pfad-errno, ohne Namen/FD zu veröffentlichen. created-file vergleicht natives macOS und fünf Profile; 4K/16K-Tests prüfen Grenzen. Rechteprüfung, Umbenennung zwischen Elternverzeichnissen, Links und Verzeichnismutation bleiben offen.

[XNU open](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

## Explizite Erstellungsmetadaten und Prozess-umask

Optionales `darwin_files.umask` (C++ `InitialUmask`) gibt unabhängig von Erstellungsrechten die Anfangsmaske von oktal 0 bis 07777 an. `umask(60)` liefert die vorige Maske und speichert die unteren 07777 Bits, ohne Gast-Speicherzugriff oder freien FD. Weglassen bedeutet unbekannt; Host-/Standardwerte werden nicht geraten. Die einmalige Initialisierung und spätere Änderungen betreffen nur künftige Erstellungen, nicht die Eingabe. Im Beispiel entspricht dezimal 18 dem oktalen 0022.

Optionales `darwin_files.creation_policy` (C++ `CreationPolicy`) liefert vollständige Metadaten neuer Objekte. Das strikte Objekt enthält genau `first_inode`, `block_size`, `generation`, `creation_time`, `mutation_policy`; Zeit und Mutationsrichtlinie nutzen die bestehenden Formate. Erforderlich sind eine explizite umask, mindestens ein mutable Elternverzeichnis und vollständige metadata für jedes freigegebene Elternverzeichnis. block_size liegt in 1..INT32_MAX, generation ist uint32. Die Allokationseinheit ist eine Zweierpotenz von 512 bis 16 MiB, unabhängig von Block-/VM-Seitengröße; Nanosekunden liegen in [0,1000000000). first_inode ist ein positiver uint64 größer als alle stat/Snapshot-inodes, auch anderer Geräte. Dezimalstrings erhalten Werte außerhalb des exakten JSON-Ganzzahlbereichs.

Nur erfolgreiche neue Einfügungen verbrauchen die globale inode-Folge; UINT64_MAX erschöpft sie dauerhaft. close/unlink/Namensreuse/umask/Nachschlagen setzen sie nicht zurück. Exklusiv-, FD-, Pfad-, Eintrags- und Bytebudgetfehler veröffentlichen weder Namen/FD noch Zählerfortschritt; bestehendes O_CREAT verbraucht nichts. Neue stat64-Daten übernehmen device/GID vom direkten Elternverzeichnis, UID=1000 vom festen effektiven Gastbenutzer, mode `S_IFREG | (mode & 0777 & ~umask)`, nlink=1 und size/blocks/flags=0. Blockgröße, generation und vier feste Anfangszeiten stammen aus der Richtlinie. Nach Invalidierung des vollständigen Eltern-stat/Eintragsbilds bleiben device/GID nutzbar, ohne den vollständigen Datensatz wiederherzustellen.

Neue Knoten besitzen eigene Metadaten/Allokation und erben nichts vom alten gleichnamigen Objekt. write/truncate/unlink teilen die Richtlinie und erhalten inode/mode/birthtime sowie nlink=0 nach unlink; vollständiges EFAULT bleibt dauerhaft unbekannt. Bestehende Knoten ändern sich nicht rückwirkend. `created-file-metadata` vergleicht native Rechte, Maskenrückgabe, effektive UID, Eltern-Gerät/Gruppe und Lebensdauer in fünf Profilen; `virtual-created-metadata` prüft separat alle 144 Bytes. Native vier Zeiten müssen nicht gleich sein. Feste Zeit/sparse Allokation sind virtuelle Regeln; Rechteprüfung, Identitätswechsel, ACL und natives APFS-Verhalten bleiben offen.

```json
{"darwin_files":{"files":[],"umask":18}}
```

[XNU creation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU umask](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c). [XNU rename / renameat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Reguläre Dateien innerhalb desselben Elternverzeichnisses umbenennen

`rename(128)`, `renameat(465)` und `renameatx_np(488)` benennen reguläre Dateien im selben ausdrücklich veränderbaren direkten Elternverzeichnis um oder ersetzen sie. Auch derselbe Name braucht diese Freigabe; danach bleiben alle Beobachtungen unverändert. Die unteren 32 Flag-Bits erlauben 0 oder `RENAME_NOFOLLOW_ANY=0x10`. Unbekannte Bits und EXCL+SWAP liefern EINVAL vor dem Pfadzugriff, andere bekannte Flags sind nicht unterstützt. Der gemeinsame Resolver behält Quellpriorität, FD- und ursprüngliche Komponentenprüfungen. Verzeichnisquellen werden sofort abgelehnt; erfolgreich aufgelöste abschließende Punkt- oder Zwei-Punkt-Komponenten liefern EINVAL vor Mount/Freigabe, auch bei verschachtelten oder anderen Eltern. Fehlende oder nicht als Verzeichnis nutzbare Vorfahren behalten Vorrang. Gewöhnliche Verzeichnisziele im zugelassenen Elternverzeichnis liefern EISDIR.

Alte FD, unabhängige open und dup folgen dem neuen Quellnamen über `F_GETPATH`. Das ersetzte Objekt behält letzten Pfad, Bytes, Cursor, Flags und Mapping-Lebensdauer; Schreibrechte und Metadaten gehen nicht auf die Quelle über. Bekannte Richtlinien ändern nur den jeweiligen ctime und beim Ziel nlink=0; Identität, Eigentümer, Geburtszeit und Belegung bleiben erhalten. Ohne Richtlinie/nach vollständigem EFAULT bleiben Metadaten unbekannt. Nur echtes Verschieben invalidiert Eltern-stat/Aufzählung.

Keine neue inode, Eintragskapazität oder freie FD nötig. Neuer Pfad/NUL ersetzt dynamische Quellkosten; ursprüngliche Kosten bleiben. Nur Ziele ohne alte FD/Mappings geben sofort Kapazität frei, genau einmal; teilweises unmap behält die vollen Objektkosten. Pfade ab 1024 Bytes oder mehr als 16 MiB stoppen vor Änderungen. Andere Eltern, widersprüchliche bekannte Geräte, Verzeichnisverschiebung, swap/exclusive/seclude und Rechteprüfung bleiben offen. Gleiche stat-Geräte beweisen keinen gemeinsamen Mount; EXDEV wird nicht erfunden. `renamed-file` vergleicht native/Gast-Identität, Pfade, Ersatz und Mappings; Zeiten/Budgets sind virtuelle Regeln.


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

`stat64` (338), `fstat64` (339) und `lstat64` (340) liefern auf ARM64/x64 denselben 144-Byte-LP64-Datensatz. Sie teilen die Pfadauflösung mit open, beachten dup/close und ändern weder FD-Belegung noch Cursor. rdev, Füllbytes und Reserven sind null. Eingaben liefern anfängliche Metadaten; die optionale Policy steuert Änderungen. read aktualisiert keine Zeitstempel, mode ändert keine Katalogzugriffsrechte. Fehlende Metadaten, Streamstatus, symbolische Links, altes stat, und erweiterte Sicherheit bleiben ausgeschlossen. Pfad-/FD-Fehler gehen dem Ausgabezeiger voraus; teilweise beschreibbare Ausgabe wird vor jedem Schreiben abgelehnt. Native Tests vergleichen alle Bytes einer echten Datei und die SDK-Offsets; dasselbe eigene Programm prüft alle drei Aufrufe.

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

Die eigene Workload-Prüfung verlangt alle 96 nativen ARM64- beziehungsweise 64 x64-Fälle, einschließlich `LC_MAIN` und `LC_UNIXTHREAD` auf jeder Plattform. Fehlende/übersprungene Pflichtfälle oder fehlendes `ld64.lld` führen zum Fehlschlag.

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

## Explizite Zeitbeobachtungen

`ProcessOptions::DarwinTime` / `darwin_time` liefert feste Beobachtungen für den rohen Aufruf `gettimeofday` (116), einschließlich der dritten Ausgabe `mach_absolute_time`, auf allen Darwin-Profilen. `time_of_day`, `timezone` und `mach_absolute_time` sind jeweils optional: Fehlen bedeutet unbekannt, eine explizite Null ist ein Wert. Ein leeres Objekt erzeugt keine Standarduhr. Das Modell liest keine Hostuhr, leitet keine Zeitzone ab, lässt Zeit nicht fortschreiten und rechnet absolute Ticks nicht um.

Jeder angegebene Datensatz benötigt alle Felder. `seconds` ist vorzeichenlos mit 32 Bit, `microseconds` liegt in [0, 999999], `minutes_west` / `dst_time` sind vorzeichenbehaftet mit 32 Bit, Ticks vorzeichenlos mit 64 Bit. JSON verwendet die gemeinsamen verlustfreien Ganzzahlregeln; außerhalb des sicheren Bereichs sind Dezimalstrings nötig. Unbekannte Felder, ungültige Bereiche und andere Profile werden vor dem Laden abgewiesen.

LP64 `timeval` umfasst 16 Byte: mit Null erweiterte Sekunden bei Offset 0, 32-Bit-Mikrosekunden bei 8 und vier Nullbytes bei 12. Die Zeitzone hat zwei vorzeichenbehaftete 32-Bit-Felder, Ticks acht Byte. Kalender- und Absolutzeit bilden eine gemeinsame erste Messung; alle angeforderten Beobachtungen müssen vor Kopien und Zeigerprüfungen vorhanden sein. Danach folgen timeval, timezone und absolute ticks. Eine fehlende Zeitzone oder ein späterer EFAULT erhält frühere Schreibvorgänge; überlappende Adressen folgen derselben Reihenfolge. Eine nur teilweise beschreibbare Einzelausgabe stoppt vor ihrer Kopie und erhält frühere Kopien. Nur Nullzeiger benötigen keine Konfiguration, selektive Anfragen nur ihre angeforderten Werte.

Das selbst geschriebene Programm `time` prüft natives Verhalten; `time-values` liefert die konfigurierten 32 Byte über C/CLI/Python für alle fünf Gastkombinationen. Ein separates SDK-Orakel vergleicht jedes Byte mit drei Ausgaben eines einzigen nativen Rohaufrufs. Fortschreitende Uhren, Umrechnung, commpage-Zähler, Timer und Mach-Uhrobjekte/IPC bleiben offen, ebenso dyld, Threads, Objective-C/Swift und Foundation/UIKit. Intel HVF Actions bleibt ausgesetzt; native Intel- und physische iOS-Abnahme wird nicht hinzugefügt.

```json
{"darwin_time":{"time_of_day":{"seconds":4045620583,"microseconds":654321},"timezone":{"minutes_west":-480,"dst_time":-1},"mach_absolute_time":"18364758544493064720"}}
```

[XNU gettimeofday](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_time.c), [time ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/time.h).

Zeitprüfung (2026-10-06, Release): 538 Darwin-Fälle, 274 bestanden, 264 wegen fehlender Backends übersprungen, keine Fehler; alle 66/66 vorgeschriebenen ARM64-HVF-Fälle ausgeführt. Die 12 nativen macOS-Programme und der SDK-Bytevergleich eines einzelnen Samples bestehen. C/CLI/Berichte: 43/43 ohne Auslassungen. Python besteht für fünf Gastkombinationen mit exakten Zeitbytes und acht bestehenden Dateimodi. Alle 66 Runner-Tests sowie Übersetzungs-, Fähigkeits- und Formatprüfungen bestehen. Zählungen überschneiden sich. Nachweise: `build-hvf-arm64/darwin-time-verified-evidence/`, `darwin-time-native-first/`, `darwin-time-public.xml`.

## Mach-Zeit und Rückgabekonventionen

`darwin_time.timebase` liefert `numerator` und `denominator` als vorzeichenlose 32-Bit-Werte ungleich null. Das Verhältnis bleibt ungekürzt und wird nicht umgerechnet. `mach_timebase_info_trap` mit Index 89 verwendet ARM64 X16=-89 oder x64 RAX=0x01000059. Es schreibt acht Little-Endian-Bytes (Zähler, Nenner) und liefert null, auch bei vollständig ungültiger Ausgabeadresse. Teilweise beschreibbare Ausgaben stoppen vor dem Kopieren; Transportfehler werden weitergegeben. Fehlende Konfiguration stoppt vor der Zeigerprüfung, auch bei null.

ARM64 X16=-3 und X16=-4 liefern alle 64 vorzeichenlosen Bits von `mach_absolute_time` und `mach_continuous_time`. Jeder Aufruf benötigt nur seinen eigenen Wert; explizite null ist gültig. Die entsprechenden nativen x64-Tabelleneinträge lösen EXC_SYSCALL aus und bleiben ununterstützt. Fortschreitende Uhren, commpage, Timer und Mach-Uhrobjekte/IPC fehlen weiterhin.

Die Auflösung verwendet nur die unteren 32 Nummernbits; der Bericht behält alle ursprünglichen 64 Bits. Negative ARM64-Zahlen wählen Mach; x64 verwendet 0x01000000 für Mach und 0x02000000 für BSD. BSD 3/4 bleiben read/write; unbekannte Nummern und fremde Klassen stoppen. Die aufgelöste Bindung bestimmt die Rückgabe: Mach erhält Flags und X1/RDX, BSD behält seine Carry-Regeln. x64 aktualisiert weiterhin RCX/R11. Mach-Berichte enthalten `result`, aber kein `error`, selbst bei gesetztem Eingangs-Carry.

`mach-time` vergleicht Flags, sekundäres Ergebnis, hohe Nummernbits, ungültige Zeiger und BSD-Wechsel mit dem nativen ARM64-Kernel. `mach-timebase-values` prüft exakte Bytes auf fünf Gästen, `mach-clock-values` auf ARM64; das SDK prüft Layout und erfasstes Verhältnis. Intel HVF Actions bleibt ausgesetzt; x64-Software- und Syntaxprüfungen sind keine native Intel- oder physische iOS-Abnahme.

```json
{"darwin_time":{"timebase":{"numerator":125,"denominator":3},"mach_absolute_time":"18364758544493064720","mach_continuous_time":"18446744073709551615"}}
```

[XNU clock traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/clock.c), [ARM64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [ARM64 special traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/sleh.c), [x64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/x86_64/idt64.s).

Mach-Prüfung (2026-10-06, Release): 569 Darwin-Fälle, 293 bestanden, 276 wegen fehlendem Backend übersprungen, null Fehler; alle 69/69 ARM64-HVF-Pflichtfälle ausgeführt. Im letzten Lauf bestanden 13 native Programme und beide Zeit-SDK-Orakel. C/CLI/report: 100/100 ohne Überspringen; Python deckt fünf Gäste ab. Öffentliche Vergleiche laufen getrennt je Plattform und Szenario mit explizitem Gastbudget von 10 Sekunden; Produktvorgaben und Deadline-Regressionen bleiben unverändert. Zahlen überschneiden sich.

Erste native Starts überschritten die bestehende Fünfsekundengrenze: unabhängig gemessene 6.056 Sekunden, danach 0.010 bei Wiederverwendung. Dasselbe Programm bestand anschließend 13 Fälle mit der ursprünglichen Grenze; Fehlerberichte bleiben erhalten. Nach Zeitüberschreitungen unter Hostlast bestand die getrennte serielle Prüfung. Belege: `build-hvf-arm64/darwin-mach-time-final-evidence/`, `darwin-mach-time-native-recheck/existing-binary-recheck.json`, `darwin-mach-time-public-accepted.xml`; Arbeitsbaum vor dem Commit. In jener Revision fehlten ARM64 MRS/MSR NZCV im checked-Vertrag; der Test beobachtete die Flags daher mit Ganzzahlbefehlen. Die folgende Änderung schließt diese CPU-Lücke.

## ARM64-Bedingungsflagregister

Der gemeinsame checked-ARM64-Vertrag lässt die exakten Codierungen `MRS Xt, NZCV` und `MSR NZCV, Xt` in EL0/EL1 zu. Lesen liefert nur Bits 31–28; Schreiben übernimmt diese vier Eingabebits und ignoriert alle anderen. Lesen nach `XZR` verwirft das Ergebnis; Schreiben aus `XZR` löscht die Flags, ohne SP zu lesen. Jedes Backend führt die Originalbefehle aus. Host-Setter-Prüfungen und FPCR/FPSR-Grenzen bleiben unverändert; benachbarte, nicht aufgeführte Systemregister bleiben unzulässig.

`NeverDAArch64NZCVTests` vergleicht alle Flagkombinationen mit Hostbefehlen und prüft vollständigen Skalar-/Vektorzustand, Speicher, Registergrenzen, Beobachterstopp/-fehler, Kontextwiederherstellung und gemeinsame Befehlsbudgets. ARM64 `mach-time` verwendet jetzt echte MSR/MRS um SVC und prüft Mach-Erhaltung sowie den Übergang zu BSD. Native HVF-Anforderungen enthalten alle sechs Methoden in beiden Privilegstufen und das Host-Orakel. ARM64 KVM/WHP und physisches iOS bleiben ungeprüft. Beschreibbare Dateien, Systeminformationen, fortschreitende Uhren, Mach IPC/Threads, dyld/Laufzeiten/Frameworks und Geräteabnahme bleiben weitere Umgebungsarbeit.

[Arm NZCV (DDI0601, 2025-06)](https://developer.arm.com/documentation/ddi0601/2025-06/AArch64-Registers/NZCV--Condition-Flags).


Prüfung veränderbarer Dateien (2026-10-06): Release Darwin, 610 Registrierungen, 322 bestanden, 288 wegen fehlendem Backend übersprungen, keine Fehler; ARM64 HVF 72/72 Pflichtfälle ausgeführt. Abschließende gezielte Tests einschließlich neuer EFAULT-Metadatenprüfungen: 102 bestanden, 12 übersprungen von 114. Alle 15 nativen Programme und 111 öffentlichen C/CLI-/Berichtstests bestanden ebenfalls. Zahlen überlappen. Der erste native Lauf fand die fehlende FWASWRITTEN-Behandlung; nach Korrektur bestanden, Fehlerbeleg aufbewahrt. Keine Frist verändert. Vollständige GitHub-CI und iOS-Geräte bleiben separat; Intel-Actions ausgesetzt.

`build-hvf-arm64/writable-darwin-evidence/` · `writable-native-final/` · `writable-focused-final.xml` · `writable-public.xml`

Python überschritt zuerst in drei ARM64-Verzeichnisfällen fünf Sekunden. Unveränderte Argumente bestanden alle zehn neuen Schreibfälle; ein iOS-Fall benötigte 5,005 s Wandzeit bei1,263 s CPU und lief ab. Alle drei isolierten Wiederholungen bestanden mit gleicher Grenze in2,43–3,17 s, jeweils10.941 Instruktionen und Ausgabe65. Last54–70 bei16 logischen CPUs spricht für Scheduling-Druck, garantiert keine Latenz; Erstfehler bleiben erhalten.

Die abschließende unveränderte Python-Methode bestand alle fünf Kombinationen in41,118 s bei weiterhin fünf Sekunden je Prozess. Frühere Fehler und Diagnosen bleiben getrennt erhalten.


Metadatenprüfung (2026-10-06): Release gezielt148=124 bestanden/24 übersprungen. Gesamtes Darwin645=343 bestanden/300 übersprungen/2 bestehende ARM64-HVF-Verzeichnis-Timeouts. Gleicher20-Fälle-Lauf mit ursprünglichen5s:8 bestanden/12 übersprungen, betroffene Fälle3.818/3.949s. Alle75 Pflicht-HVF-Fälle haben erfolgreiche Beobachtungen; erster Fehlerlauf bleibt erhalten. C/CLI/Reports117/117 mit73 Darwin, Python fünf Profile27.359s, nativ15/15, Runner66/66 bestanden. Virtuelle Belegung ist kein APFS-Nachweis. Keine Friständerung; volleCI, Intel, iOS-Gerät und Gesamtumgebung bleiben offen.

`build-hvf-arm64/mutation-metadata-validation-summary.json`; `mutation-metadata-darwin-evidence/`; `mutation-metadata-directory-recheck/`; `mutation-metadata-focused.xml`; `mutation-metadata-public.xml`.

Sparse-Seek-Prüfung (2026-10-06): Release Darwin umfasst 671 Fälle: 359 bestanden, 312 wegen nicht verfügbarer Backends übersprungen, keine Fehler. Alle 78 verpflichtenden ARM64-HVF-Fälle wurden ausgeführt; Unicorn deckt fünf Gastprofile ab. Gezielte Tests: 123 von 147 bestanden, 24 übersprungen. Alle 16 nativen Programme, 122 C/CLI/report-Prüfungen (78 Darwin-Vergleiche), fünf Python-Profile (12.344 s) und 66 Runner-Tests bestanden. Zahlen überschneiden sich; Zeitlimits und frühere Fehlerprotokolle bleiben erhalten. Nachweise: `build-hvf-arm64/sparse-seek-validation-summary.json`. Die Belegung ist eine explizite virtuelle Richtlinie, keine APFS-Gleichheit. Vollständige CI und physisches iOS bleiben offen; Intel HVF Actions bleibt ausgesetzt.

Unlink-Prüfung (2026-10-06): Release Darwin708 Fälle,384 bestanden,324 wegen fehlender Backends übersprungen, keine Fehler; alle81 ARM64-HVF-Pflichtfälle ausgeführt. Gezielte156:137 bestanden/19 übersprungen. Native17/17, C/CLI/report128/128 (Darwin83), Python fünf Profile16.268s, Runner66/66 bestanden. Unabhängige Entwurfs-/Implementierungsprüfung ohne verbleibende Blocker. Zahlen überlappen, Zeitlimits unverändert, keine Wiederholung nötig. Nachweise: `build-hvf-arm64/unlink-validation-summary.json`. Invalidierung/feste Zeiten sind Modellregeln; vollständiges Dateisystem/Runtime und physisches iOS bleiben offen. Intel HVF Actions ausgesetzt, vollständige CI separat.

### Erstellungsprüfung, 2026-10-06

Release Darwin:748 Fälle,412 bestanden,336 wegen fehlender Backends übersprungen, keine Fehler; alle84 ARM64-HVF-Pflichtfälle ausgeführt. Gezielt162:150 bestanden/12 übersprungen. C/CLI/Berichte133/133 (Darwin88), Python5 Profile9.982s, nativ18/18, Runner66/66 bestanden. ARM64 verweigerte zunächst korrekt die Pointer-Tabellen-Rebases des Tests. Inline-Bytes beheben die Fixture ohne Lockerung des Loaders; ursprüngliche Fehler/Binärdateien bleiben. Inventarerwartung27→28 aktualisiert. Unabhängige Prüfung ohne Blocker, einschließlich Elternbeobachtungen nach Budgetfehler. Zahlen überlappen, Zeitlimits unverändert. Vollständige CI/physisches iOS separat; Intel HVF Actions ausgesetzt.

`build-hvf-arm64/create-validation-summary.json`, `create-darwin-evidence/`, `create-focused.xml`, `create-public.xml`, `create-native-final/`, `create-initial-evidence/`.

### Prüfung der Erstellungsmetadaten, 2026-10-06

Release Darwin: 787 Registrierungen, 439 bestanden, 348 wegen nicht verfügbarer Backends übersprungen, keine Fehler; alle 87 ARM64-HVF-Pflichtfälle ausgeführt. Gezielt: 139/151 bestanden, 12 übersprungen. C/CLI/Bericht: 145/145 einschließlich 98 Darwin-Eingabevergleichen; unveränderte Python-Methode mit fünf Profilen in 12.211 Sekunden bestanden. Native Programme 19/19, Prüfrunner 66/66. Unabhängige Prüfung ohne Blocker; zusätzliche Fälle prüfen verschiedene Eltern-device/GID und globale inode-Folge, unlink vor erstem Schreiben sowie umask ohne freien FD/lesbare Eingabe. Zahlen überlappen, Zeitlimits unverändert, keine Fehlerwiederholung nötig. Feste Erstellungs-/Änderungszeit und Allokation bleiben virtuelle Richtlinien. Vollständige GitHub CI und physisches iOS bleiben separat; Intel HVF Actions ausgesetzt.

`build-hvf-arm64/creation-metadata-validation-summary.json`, `creation-metadata-darwin-evidence/`, `creation-metadata-focused.xml`, `creation-metadata-public.xml`, `creation-metadata-native/`.

### Umbenennung geprüft, 2026-10-06

Release Darwin: 835 Registrierungen,474 bestanden,360 nicht verfügbare Backends, ein bestehender macOS-ARM 64-HVF-Timeout für virtuelle Metadaten (5.087 s). Unveränderte Methode/Argumente und 5 s-Limit erneut geprüft: 8 bestanden,12 übersprungen, betroffene Identität 0.113 s. Alle 90 erforderlichen ARM 64-HVF-Identitäten haben erfolgreiche Beobachtungen über beide Läufe; das vollständige Gate bleibt als fehlgeschlagen dokumentiert. Fokus 42/54 bestanden,12 übersprungen; C/CLI/Bericht 150/150, davon 103 Darwin-Vergleiche; unverändertes Python mit fünf Profilen 18.478 s; native 20/20, Skripte 66/66. Unabhängige Prüfung fand verschachtelte Punkt-Klassifikation: 4 K/16 K vor Korrektur fehlgeschlagen, danach bestanden. Frühere readonly-ftruncate-Testannahme auf EINVAL korrigiert. Fehler und Probeversionen bleiben erhalten, Zählungen überlappen, Fristen unverändert. Vollständige GitHub CI/iOS-Geräte separat; Intel HVF Actions ausgesetzt.

`build-hvf-arm64/rename-validation-summary.json`, `rename-focused-final.xml`, `rename-darwin-final-evidence/`, `rename-metadata-recheck.xml`, `rename-public.xml`, `rename-native-final/`, `rename-review-initial-evidence/`.

## Explizite Systembeobachtungen

`ProcessOptions::DarwinSystem` / `darwin_system` liefert feste Beobachtungen für `sysctl(202)` und das rohe `sysctlbyname(274)` in jedem Darwin-Profil. Alle Felder sind optional; fehlende Werte oder nicht aufgeführte Schlüssel bleiben ausdrücklich nicht unterstützt. Es gibt keine Hostabfragen oder abgeleiteten Versions-/Modellvorgaben. Striktes JSON und C++ prüfen Werte und lehnen andere Profile vor dem Laden ab.

`os_revision` ist vorzeichenbehaftet mit 32 Bit; `cpu_count` liegt zwischen 1 und INT32_MAX; `memory_size` erhält alle 64 vorzeichenlosen Bits. Andere Felder sind Zeichenketten bis 1023 Bytes ohne eingebettetes NUL; ausdrücklich leere Zeichenketten sind gültig. Die Ausgabe enthält das abschließende NUL. CPU- und Speicherangaben verändern weder Scheduling noch Zuteilungsbudget.

| JSON-Feld | sysctl-Name | MIB |
| --- | --- | --- |
| `os_type` | `kern.ostype` | `1,1` |
| `os_release` | `kern.osrelease` | `1,2` |
| `os_revision` | `kern.osrevision` | `1,3` |
| `kernel_version` | `kern.version` | `1,4` |
| `os_version` | `kern.osversion` | `1,65` |
| `machine` | `hw.machine` | `6,1` |
| `model` | `hw.model` | `6,2` |
| `cpu_count` | `hw.ncpu` | `6,3` |
| `memory_size` | `hw.memsize` | `6,24` |

`hw.pagesize` stammt aus der vorhandenen Gastspeicherrichtlinie: normalerweise acht Bytes, vier bei nicht null Ausgabe und genau vier Bytes Kapazität. Das alte MIB `[6,7]` und `hw.pagesize_compat` liefern immer vier Bytes. Die dynamische numerische OID von `hw.pagesize` bleibt nicht unterstützt. `hw.memsize` wird bei Kapazität vier nur verkürzt, wenn das 64-Bit-Muster einer Vorzeichenerweiterung eines 32-Bit-Werts entspricht; sonst bewahrt ERANGE34 Ausgabe und Länge.

Die MIB-Anzahl verwendet die unteren 32 Bits und muss 2–12 sein; die Namenslänge verwendet 64 Bits und muss kleiner als 1024 sein. Alle angegebenen Bytes werden vor dem ersten NUL und dem Entfernen eines abschließenden Punkts geprüft. Leere Namen ergeben ENOENT; teilweise lesbare Eingaben bleiben nicht unterstützt. Ein nicht null `oldlenp` muss vor Effekten für acht Bytes vollständig les- und schreibbar sein. Native Versuche mit fehlerhaften Längenzeigern kehrten nicht fristgerecht zurück; diese Zeiger bleiben außerhalb des unterstützten Bereichs. Null `oldlenp` bedeutet Kapazität null; null `oldp` fragt nur die Größe ab. Ein kurzer Puffer ergibt ENOMEM12 ohne Datenänderung und schreibt Länge null. Daten-EFAULT bewahrt die alte Länge. Eingabe und Kapazität werden vor den Daten erfasst; die Länge wird zuletzt kopiert. Aliase und frühere Kopien bleiben bei späteren Transportfehlern erhalten.

Nur gemeinsam von null verschiedene `newp` und `newlen` bilden eine Schreibanfrage. Ausgewählte Knoten liefern für die feste Nicht-root-Identität EPERM1 vor Wert- oder Ausgabeprüfung, einschließlich des nativ privilegiert schreibbaren `kern.osversion`. Neue Länge null ignoriert den Zeiger. Für unbekannte Schlüssel, Bäume und dynamische OIDs wird kein ENOENT erfunden.

Das eigene Programm `system-info` prüft native macOS- und Gast-ABI; `virtual-system` vergleicht konfigurierte Bytes über C++, C/CLI und Python. Ein separates SDK-Orakel erfasst neun Hostbeobachtungen als explizite Testeingaben und vergleicht benannte und numerische Ausgaben. Das bestätigt weder physisches iOS noch Intel HVF.

```json
{"darwin_system":{"os_type":"Darwin","os_release":"24.test","os_revision":0,"kernel_version":"Virtual kernel","os_version":"V42","machine":"virtual64","model":"VirtualModel","cpu_count":4,"memory_size":"17179869184"}}
```

[XNU sysctl](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_newsysctl.c), [XNU hardware MIB](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mib.c), [Apple sysctl(3)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man 3/sysctl.3.html).

### Prüfung der Systemabfragen, 2026-10-06

Release Darwin: 881 Registrierungen, 509 bestanden, 372 wegen nicht verfügbarer Backends übersprungen, keine Fehler; alle 93 erforderlichen ARM64-HVF-Identitäten ausgeführt. Fokus: 37/49 bestanden, 12 übersprungen. C/CLI/Bericht: 163/163, davon 113 Darwin-Vergleiche. Unveränderte Python-Methode: fünf Profile in 15.302 s; native Programme 21/21, Skripte 66/66. Unabhängige Prüfung ohne Blocker; zusätzliche Fehlerprioritäten und SDK-Orakel bestanden. Ein neuer Orakel-Build scheiterte am fehlenden StringExtras-Header und gelang nach Ergänzung; Quelle und Log bleiben erhalten. Native Versuche mit fehlerhaften Längenzeigern bleiben dokumentiert und außerhalb des Vertrags. Nach den Tests wurden nur zwei Dateikopfkommentare bereinigt und erfolgreich neu gebaut. Zählungen überlappen, Fristen bleiben gleich; keine Laufzeitfehler-Nachprüfung nötig. Vollständige GitHub CI und physisches iOS separat; Intel-HVF-Actions ausgesetzt.

`build-hvf-arm64/sysctl-validation-summary.json`, `sysctl-darwin-final-evidence/`, `sysctl-focused-final.xml`, `sysctl-public.xml`, `sysctl-native-final/`, `sysctl-sdk-build-failure/`, `sysctl-initial-probe-evidence/`.

## Vektorielle Datei-E/A und Ausgabenerfassung

`readv`/`writev`, `preadv`/`pwritev` und nocancel teilen die skalare Datei- und Ausgabelogik, ohne neue Optionen oder Hostzugriffe. Ein LP64-iovec enthält Adresse und Länge mit je acht Bytes. Die vorzeichenbehafteten unteren32 Bits von iovcnt müssen1–1024 ergeben. Das ganze Array wird vor der Deskriptorsuche kopiert; Ausgabealiase ändern den Auftrag nicht. Teilweise lesbare Arrays bleiben nicht unterstützt.

Zugriffsrechte und Positionierbarkeit eines Streams werden vor den Längen geprüft. Einzelwerte und Summe müssen in INT64_MAX passen, bei Dateien und Verzeichnissen zusätzlich in INT_MAX. Endliches stdin wird auf verfügbare Bytes begrenzt; Erfassung behält das Ausgabebudget. pwritev lehnt jeden negativen Offset vor dem Array ab; preadv prüft ihn nach Deskriptor und Längen. Leere Elemente ignorieren die Adresse, jedoch nicht Deskriptor-, Typ- und Offsetregeln. Nach EOF werden restliche Elemente nicht berührt. Positionierte Aufrufe erhalten den Cursor, pwritev ignoriert Append. Normales Append kürzt den gesamten Auftrag einmal am ursprünglichen Cursor und wählt erst dann EOF.

Ein späteres vollständig ungültiges Element liefert EFAULT und erhält vorherige Bytes, normalen Cursorfortschritt und FWASWRITTEN nach dem Schreiben mindestens eines Bytes. Ein zugelassener nichtleerer Schreibvorgang mit Datenpuffer-EFAULT invalidiert vollständige Metadaten; Argumentfehler, Modellablehnungen und Backend-Fehler erhalten sie. Ein teilweise schreibbares Leseziel stoppt mit UnsupportedService ohne Kopie dieses Elements; frühere Kopien bleiben. Eine teilweise lesbare Dateischreibquelle bleibt vor allen Dateieffekten nicht unterstützt. Berechtigung, Mapping-Leases und Gesamtspeicherbudget werden vorab geprüft; Backend-Prüf- oder Lesefehler veröffentlichen keine Datei- oder Ausgabebytes.

Erfassung prüft zuerst das gemeinsame stdout/stderr-Budget. Ein Element über der Benutzeradressgrenze liefert keine Bytes, vorherige Elemente bleiben erhalten. Andere lesbare Präfixe werden mit EFAULT erfasst. Skalare Bereichsfehler bleiben vor dem Budget priorisiert. Duplizierte oder umgeleitete Deskriptoren behalten ihr Ziel. Das originale `vectored-io` prüft acht Eingänge auf nativem macOS, fünf Gastkombinationen und C/CLI/Python. Cancellation, Pipes, Threads und physische iOS-Abnahme kommen nicht hinzu.

`readv`: 120/411; `writev`: 121/412; `preadv`: 540/542; `pwritev`: 541/543.

[XNU vector calls](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU iovec lengths](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_subr.c), [XNU vnode I/O](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

### Prüfung der Vektor-E/A, 2026-10-06

Release Darwin:937 Registrierungen,553 bestanden,384 wegen fehlendem Backend übersprungen, keine Fehler; alle96 erforderlichen ARM64-HVF-Identitäten ausgeführt. Gezielt45/57 bestanden,12 übersprungen. C/CLI/Report168/168, darunter118 Darwin-Eingabevergleiche; Python deckt fünf Kombinationen in 20.397s ab. Nativ22/22, Prüfskripte66/66. Die unabhängige Prüfung ergänzte einen Fehlerfall für positioniertes Schreiben mit Lücke und prüft Cursor, tatsächliches EOF, Metadatenverweigerung und exakte Restkapazität. Beim ersten Build verwies ein alter Test noch auf eine entfernte interne Abfrage; er prüft nun echte Ausgaben. Ein optional<bool>-Fehler in der neuen Ereignisassertion meldete acht erfolgreiche Gastläufe als fehlgeschlagen; korrigierte Prüfungen bestanden. Quellen und Logs beider Fehler bleiben erhalten. Zählungen überlappen, Fristen unverändert. Vollständige GitHub-CI und physisches iOS separat; Intel HVF Actions bleibt ausgesetzt.

`build-hvf-arm64/vector-validation-summary.json`, `vector-darwin-final-evidence/`, `vector-focused-final.xml`, `vector-public.xml`, `vector-native-initial/`, `vector-initial-build-failure/`, `vector-initial-assertion-evidence/`.
