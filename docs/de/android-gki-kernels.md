# Verträge veröffentlichter Android-GKI-Kernel

NeverD priorisiert veröffentlichte Android-GKI-Zweige von 5.10 bis 6.18 vor weiteren Linux-Varianten. Der Prozessauftrag wählt den Zweig ausdrücklich:

```json
{"linux_kernel":{"gki":"android17-6.18"},"linux_files":{"files":[],"descriptor_limit":16}}
```

Der API-28-Vertrag des nativen Android-Profils beschreibt Bionic-Importe und wählt keinen Kernel. Die GKI-Auswahl steuert die implementierten Verträge für `pidfd_open`, Vektorausgaben, codierte Prozess-CPU-Uhren und Prozessdeskriptor-`ppoll` mit Zeitlimit null. Sie zertifiziert oder startet keinen vollständigen Kernel und erschließt keine Geräte, Namensräume, Berechtigungen oder Prozesslisten. Nicht unterstützte Dienste stoppen ausdrücklich. Siehe die [offizielle GKI-Freigaberichtlinie](https://source.android.com/docs/core/architecture/kernel/gki-releases).

## Festgelegte Quellstände

`LinuxGKIKernels.def` verwendet diese offiziellen `r1`-Tags, geprüft am 2026-10-07. Die unveränderlichen Commits enthalten `kernel/pid.c`, `include/uapi/linux/pidfd.h`, `arch/arm64/configs/gki_defconfig`, `kernel/fork.c`, `lib/iov_iter.c` und `fs/read_write.c`. Flags stammen aus UAPI und Aufrufprüfung, nicht aus Android-API-Stufe oder Hostkernel. Die festgelegten CPU-Uhrquellen stehen in der folgenden Tabelle.

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

Bionic übersetzt negative Rohfehler in `-1` und threadlokales `errno`; Erfolg erhält `errno`. Für `fstat` fehlen Metadaten. Blockierendes Polling, Exit-Meldungen, Signale per pidfd, `pidfd_getfd`, `fcntl`, pidfs-ioctls und unbeobachtete Aufgaben bleiben ununterstützt. Aus pidfds werden weder Scheduling noch Prozesslebensdauer abgeleitet.

## Implementierte Abfragen mit Zeitlimit null

Mit ausgewähltem GKI unterstützen rohe x64/AArch64-`ppoll`-Aufrufe und Bionics variadisches `syscall` eine explizite Null-timespec und eine leere temporäre Signalmaske. `linux_files.descriptor_limit` liefert die Gastgrenze RLIMIT_NOFILE; die unteren vorzeichenlosen 32 Bits von `nfds` dürfen sie nicht überschreiten. Die gemeinsame Tabelle meldet für beobachtete lebende pidfds keine Bereitschaft, ignoriert negative Deskriptoren und liefert für geschlossene Deskriptoren `POLLNVAL` (`0x20`). Jeder Eintrag mit einem Ergebnis ungleich null zählt einzeln, auch Duplikate. Unbeobachtete Bereitschaft anderer Typen führt zum ausdrücklichen Stopp.

Kopie und Prüfung des Zeitlimits gehen Maske und Deskriptoren voraus. Bei einer nichtleeren Maske werden Größe in voller Breite und lesbarer Bereich vor der nicht unterstützten Maskenoperation geprüft; eine Nullmaske ignoriert die Größe. Sämtliche Deskriptormetadaten werden vor Bereitschaftsauswahl und Ausgabe importiert. Nur die 16-Bit-Felder `revents` werden in Eintragsreihenfolge geschrieben; ein späterer Fehler erhält frühere Schreibzugriffe. Gemischter Zugriff innerhalb eines Feldes behält die gemeinsame Grenze für nicht unterstützte Teilkopien. Auch ein leeres Array unterliegt der abschließenden Benutzerbereichsprüfung. Die Null-timespec wird nicht zurückgeschrieben und darf lesbar, aber schreibgeschützt sein. Der Aufruf liest oder verändert keine Wanduhr.

Die Regeln wurden an allen acht unveränderlichen Freigaben in `fs/select.c` geprüft: `ppoll`, `do_sys_poll`, `do_pollfd`, `poll_select_set_timeout` und `poll_select_finish`. Die Bereitschaft lebender pidfds folgt `pidfd_poll` in `kernel/fork.c` der ersten sechs und pidfs der letzten zwei Versionen:

[5.10 fs/select.c](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/fs/select.c) · [6.18 fs/select.c](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/fs/select.c) · [6.6 kernel/fork.c](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/fork.c) · [6.12 fs/pidfs.c](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/fs/pidfs.c) · [6.18 fs/pidfs.c](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/fs/pidfs.c)

Versionsabhängige Exit- und Reaping-Meldungen liegen außerhalb der festen Beobachtungen lebender Aufgaben. Blockierende Wartevorgänge, temporäre Masken, Signale und benannte Bionic-`ppoll`-Wrapper brauchen eigene Verträge.

## Implementierte Prozess-CPU-Uhren

Mit explizitem GKI akzeptiert `clock_gettime` negative codierte Prozess-CPU-IDs für PROF, VIRT und SCHED und interpretiert die unteren 32 Bits mit Vorzeichen. PID und Art bestimmen eine explizite Beobachtung in `linux_time`. Der aktuelle Prozess ist implizit vorhanden; andere müssen vor Angabe eines Wertes im geschlossenen Katalog als lebende Gruppenführer deklariert werden.

```json
{"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true}]},"linux_time":{"advance_on_idle":true,"clocks":[{"id":1,"seconds":10,"nanoseconds":0},{"id":2,"seconds":3,"nanoseconds":4},{"id":-16006,"seconds":7,"nanoseconds":9}]}}
```

`-16006` ist SCHED für PID 2000. PROF und VIRT sind unabhängig. Die aktuellen SCHED-IDs 2, -6 (PID null) und -8006 (PID 1000) teilen einen Wert; PROF-Aliase sind -8/-8008, VIRT -7/-8007. Doppelte Aliase werden auch bei gleichem Wert abgelehnt. CPU-Sekunden sind nicht negativ, Nanosekunden normalisiert. Leerlauffortschritt ändert nur Wanduhren 0, 1 und 7; CPU-Werte bleiben fest. Aus Befehlen wird kein CPU-Verbrauch geschätzt.

Die eigene TID der aktuellen Aufgabe bezeichnet ebenfalls ihre Prozessgruppe, auch bei kooperativen Android-Threads ohne Fremdkatalog. Fehlende fremde PIDs und lebende Nichtführer im geschlossenen Katalog liefern `EINVAL` vor dem Ausgabezugriff. Ein ausgelassener Katalog oder fehlender Wert einer bekannten Gruppe bleibt vor der Kopie nicht unterstützt. Ungültige Arten liefern `EINVAL`, gültige Werte können beim Benutzerkopieren `EFAULT` liefern. Rohe Traps behalten negative Fehler; nur Bionic setzt errno und liefert -1.

Die Regeln folgen `pid_for_clock`, `posix_cpu_clock_get`, dem Uhrverteiler und den ID-Definitionen jeder festgelegten Freigabe:

| Angeforderter Zweig | Quelle der Prozess-CPU-Uhren |
| --- | --- |
| `android12-5.10` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/time/posix-cpu-timers.c) |
| `android13-5.10` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/time/posix-cpu-timers.c) |
| `android13-5.15` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/time/posix-cpu-timers.c) |
| `android14-5.15` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/time/posix-cpu-timers.c) |
| `android14-6.1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/time/posix-cpu-timers.c) |
| `android15-6.6` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/time/posix-cpu-timers.c) |
| `android16-6.12` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/time/posix-cpu-timers.c) |
| `android17-6.18` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/time/posix-cpu-timers.c) |

Die feste `init/Kconfig` aktiviert POSIX-Timer standardmäßig; die GKI-defconfigs deaktivieren sie nicht. Siehe [6.18 init/Kconfig](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/init/Kconfig).

FD-Erkennung und CPU-Routing folgen außerdem dem festgelegten [6.18-Verteiler](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/time/posix-timers.c) und den [Uhr-ID-Definitionen](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/include/linux/posix-timers_types.h). FD-Uhren und codierte CPU-Uhren einzelner Threads bleiben nicht unterstützt. Der Katalog ist eine feste Gastbeobachtung; Rechte, Namensräume, Prozesslebensdauer und CPU-Abrechnung benötigen eigene Verträge.

## Validierung und weitere Abdeckung

`LinuxPIDFDTests.cpp` verwendet unabhängige x64/AArch64-ELF-Aufrufer mit O0/O2 für alle acht Zweige und verfügbare Backends. Geprüft werden Flags, gemeinsame Tabellen, Grenzen/Wiederverwendung, Fehlerreihenfolge, Metadatenfehler, Originalbereiche und Begrenzung, fehlende/abgeschlossene Kataloge, Nichtführer sowie Zielprüfung vor FD-Erschöpfung. `AndroidSyscallTests.cpp` wiederholt raw/Bionic-Eigentum, Zielsuche und errno in sechs O0/O2-Konfigurationen mit normalen, Android-packed- und RELR-Relokationen. Quellstände und Modellausführung belegen diese Teilmenge; native Starts sämtlicher GKI-Abbilder fehlen. Weitere Linux-Ziele benötigen dienstweise Versions-, Konfigurations- und Beobachtungsnachweise.

CPU-Fälle prüfen Identität, Ausgabereihenfolge, unabhängige Arten, explizite Beobachtungen und Trennung vom Wanduhr-Leerlauf. `AndroidTimeTests.cpp` prüft benannte/rohe Ausgaben und Wächter; der kooperative Syscall prüft den Alias der aktuellen Nichtführer-TID.

`ZeroTimeoutPollRetainsReadinessAndOrderedCopies` prüft O0/O2-Rohaufrufe für acht GKI-Versionen: lebende, negative und geschlossene Deskriptoren, Duplikatzählung, Argumentverengung, Zeitlimit-/Maskenreihenfolge, schreibgeschützte Null-timespecs, vollständigen Metadatenimport vor Bereitschaft sowie frühere `revents` bei späteren Schreibfehlern. `ZeroTimeoutPollKeepsUnobservedBoundaries` erhält unbekannte Kernel, Grenzen, Masken, Warte- und Bereitschaftszustände. Androids `ReleasedGKIZeroTimeoutPollSharesRawAndBionicResults` prüft gemeinsame Tabelle und errno-Besitz in sechs Verpackungsprofilen.
