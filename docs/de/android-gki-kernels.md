# Verträge veröffentlichter Android-GKI-Kernel

NeverD priorisiert veröffentlichte Android-GKI-Zweige von 5.10 bis 6.18 vor weiteren Linux-Varianten. Der Prozessauftrag wählt den Zweig ausdrücklich:

```json
{"linux_kernel":{"gki":"android17-6.18"},"linux_files":{"files":[],"descriptor_limit":16}}
```

API 28 des Android-Profils beschreibt Bionic-Importe und bestimmt keine Kernelversion. GKI steuert derzeit nur `pidfd_open` und vektorielle Ausgabe. Die Auswahl zertifiziert keinen vollständigen Kernel, lädt kein Kernelabbild und erschließt weder Geräte noch Namensräume, Berechtigungen oder Prozesse. Nicht unterstützte Dienste brechen ausdrücklich ab. Siehe die offizielle [GKI-Veröffentlichungsrichtlinie](https://source.android.com/docs/core/architecture/kernel/gki-releases).

## Festgelegte Quellstände

`LinuxGKIKernels.def` verwendet diese offiziellen `r1`-Tags, geprüft am 2026-10-07. Die unveränderlichen Commits enthalten `kernel/pid.c`, `include/uapi/linux/pidfd.h`, `arch/arm64/configs/gki_defconfig`, `kernel/fork.c`, `lib/iov_iter.c` und `fs/read_write.c`. Flags stammen aus UAPI und Aufrufprüfung, nicht aus Android-API-Stufe oder Hostkernel.

| Angeforderter Zweig | Veröffentlichungstag | Fester Quellcommit | Zulässige Flags | Iovec-Import |
| --- | --- | --- | --- | --- |
| `android12-5.10` | `android12-5.10-2026-07_r1` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/pid.c) | `PIDFD_NONBLOCK` (`0x800`) | [Zuerst sämtliche Metadaten kopieren](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/lib/iov_iter.c) |
| `android13-5.10` | `android13-5.10-2026-07_r1` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/pid.c) | `PIDFD_NONBLOCK` | [Zuerst sämtliche Metadaten kopieren](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/lib/iov_iter.c) |
| `android13-5.15` | `android13-5.15-2026-09_r1` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/pid.c) | `PIDFD_NONBLOCK` | [Zuerst sämtliche Metadaten kopieren](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/lib/iov_iter.c) |
| `android14-5.15` | `android14-5.15-2026-07_r1` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/pid.c) | `PIDFD_NONBLOCK` | [Zuerst sämtliche Metadaten kopieren](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/lib/iov_iter.c) |
| `android14-6.1` | `android14-6.1-2026-09_r1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/pid.c) | `PIDFD_NONBLOCK` | [Zuerst sämtliche Metadaten kopieren](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/lib/iov_iter.c) |
| `android15-6.6` | `android15-6.6-2026-07_r1` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/pid.c) | `PIDFD_NONBLOCK` | [Pfad für einen Puffer](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/lib/iov_iter.c) |
| `android16-6.12` | `android16-6.12-2026-09_r1` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/pid.c) | `PIDFD_NONBLOCK` und `PIDFD_THREAD` (`0x80`) | [Pfad für einen Puffer](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/lib/iov_iter.c) |
| `android17-6.18` | `android17-6.18-2026-09_r1` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/pid.c) | `PIDFD_NONBLOCK` und `PIDFD_THREAD` | [Pfad für einen Puffer](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/lib/iov_iter.c) |

Diese Quellstände definieren den Vertrag. Neue Releases oder Rückportierungen benötigen Quellprüfung und Regressionen. GKI zusammen mit ausdrücklich fehlendem `pidfd_open` wird vor dem Laden abgelehnt.

## Implementierte Prozessdeskriptoren

x64/AArch64-Traps und Bionic `syscall` teilen `LinuxServices` und die Deskriptortabelle des Workloads. Nur die unteren 32 PID-/Flag-Bits zählen. Unbekannte Flags oder nichtpositive vorzeichenbehaftete PIDs liefern vor der Zuteilung `EINVAL`. Andere lebende Gastaufgaben können im optionalen, festen und abgeschlossenen `tasks`-Katalog stehen:

```json
{"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true},{"id":3000,"group_leader":false}]},"linux_files":{"files":[],"descriptor_limit":16}}
```

Der laufende Gruppenführer PID 1000 bleibt implizit, auch bei leerem Array. Ohne Katalog bleibt die Suche fremder Ziele ununterstützt; außerhalb eines ausdrücklich angegebenen Katalogs liefert eine gültige positive PID vor der Zuteilung `ESRCH`. Lebende Nichtführer ohne `PIDFD_THREAD` liefern unter 5.10–6.12 `EINVAL`, unter [6.18 in `pidfd_prepare`](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/fork.c) `ENOENT`. Mit zugelassenem Thread-Flag öffnen 6.12/6.18 deklarierte Nichtführer. Flagprüfung erfolgt stets vor Zielsuche.

Jeder Eintrag benötigt eine ganzzahlige `id` von 1..2147483647 und boolesches `group_leader`; höchstens 4096 Einträge. Doppelte IDs, Zusatzfelder, PID 1000 als Nichtführer und Kataloge ohne GKI sind ungültig. Prioritätsbeobachtungen müssen Katalogaufgaben oder den laufenden Prozess nennen. Der feste Katalog ist mit kooperativen Android-Threads (`thread_limit > 1`) unvereinbar. Erzeugung, Freigabe, Berechtigungen und Namensraumübersetzung brauchen eigene Lebensdauerverwaltung.

`linux_files` ist erforderlich. Dateien und pidfds teilen Eigentum und Grenze; zugeteilt wird die kleinste freie Nummer. Erschöpfung ergibt `EMFILE`; `close` gibt sie frei, erneutes Schließen ergibt `EBADF`. Geschlossene Standardstreams dürfen wiederverwendet werden. Es erfolgen keine Host-pidfd-, Dateisystem- oder Prozessabfragen.

Bei gültigen pidfds liefern `read`/`write` vor Nutzdatenzugriff `EINVAL`, `lseek` nach Prüfung des Ursprungs `ESPIPE`. `writev` importiert zuerst Metadaten und prüft Benutzerbereiche: `EFAULT` kann dem `EINVAL` der fehlenden Schreiboperation vorausgehen. Nutzdaten werden weder gelesen noch ausgegeben. Derselbe versionsabhängige Importer gilt für stdout/stderr; siehe [VFS-Reihenfolge](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/fs/read_write.c).

5.10/5.15/6.1 kopieren sämtliche iovec-Metadaten vor der Längenprüfung: Eine frühe negative Länge mit später unzugänglichen Metadaten ergibt `EFAULT`. Originalbereiche werden vor der Übertragungsgrenze geprüft, auch bei einem Vektor. 6.6/6.12/6.18 prüfen nacheinander und liefern dabei `EINVAL`; bei einem Puffer wird zuerst begrenzt, bei mehreren werden weiter alle Originalbereiche geprüft. Grundlage sind `copy_iovec_from_user`, `__import_iovec` und `import_ubuf`. Ohne GKI bleibt die bestehende Einpufferregel, ohne Versionsannahme.

Bionic übersetzt negative Rohfehler in `-1` und threadlokales `errno`; Erfolg erhält `errno`. Für `fstat` fehlen Metadaten. Polling, Exit-Meldungen, Signale per pidfd, `pidfd_getfd`, `fcntl`, pidfs-ioctls und unbeobachtete Aufgaben bleiben ununterstützt. Aus pidfds werden weder Scheduling noch Prozesslebensdauer abgeleitet.

## Validierung und weitere Abdeckung

`LinuxPIDFDTests.cpp` verwendet unabhängige x64/AArch64-ELF-Aufrufer mit O0/O2 für alle acht Zweige und verfügbare Backends. Geprüft werden Flags, gemeinsame Tabellen, Grenzen/Wiederverwendung, Fehlerreihenfolge, Metadatenfehler, Originalbereiche und Begrenzung, fehlende/abgeschlossene Kataloge, Nichtführer sowie Zielprüfung vor FD-Erschöpfung. `AndroidSyscallTests.cpp` wiederholt raw/Bionic-Eigentum, Zielsuche und errno in sechs O0/O2-Konfigurationen mit normalen, Android-packed- und RELR-Relokationen. Quellstände und Modellausführung belegen diese Teilmenge; native Starts sämtlicher GKI-Abbilder fehlen. Weitere Linux-Ziele benötigen dienstweise Versions-, Konfigurations- und Beobachtungsnachweise.
