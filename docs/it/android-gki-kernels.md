# Contratti dei kernel Android GKI pubblicati

NeverD dà priorità ai rami Android GKI pubblicati 5.10–6.18, prima delle altre varianti Linux. La richiesta sceglie esplicitamente il ramo:

```json
{"linux_kernel":{"gki":"android17-6.18"},"linux_files":{"files":[],"descriptor_limit":16}}
```

API 28 descrive gli import Bionic, senza scegliere il kernel. GKI controlla solo `pidfd_open` e gli output vettoriali implementati; non certifica un kernel completo, non carica immagini e non deduce dispositivi, namespace, credenziali o processi. I servizi non supportati falliscono esplicitamente. Vedere la [politica GKI ufficiale](https://source.android.com/docs/core/architecture/kernel/gki-releases).

## Revisioni sorgente fissate

`LinuxGKIKernels.def` segue questi tag ufficiali `r1`, verificati il 2026-10-07. I commit immutabili forniscono `kernel/pid.c`, `include/uapi/linux/pidfd.h`, `arch/arm64/configs/gki_defconfig`, `kernel/fork.c`, `lib/iov_iter.c` e `fs/read_write.c`. I flag derivano da UAPI e controlli delle chiamate, non dal livello API Android o dal kernel host.

| Ramo richiesto | Tag pubblicato | Commit sorgente fissato | Flag ammessi | Import iovec |
| --- | --- | --- | --- | --- |
| `android12-5.10` | `android12-5.10-2026-07_r1` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/pid.c) | `PIDFD_NONBLOCK` (`0x800`) | [Copiare prima tutti i metadati](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/lib/iov_iter.c) |
| `android13-5.10` | `android13-5.10-2026-07_r1` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/pid.c) | `PIDFD_NONBLOCK` | [Copiare prima tutti i metadati](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/lib/iov_iter.c) |
| `android13-5.15` | `android13-5.15-2026-09_r1` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/pid.c) | `PIDFD_NONBLOCK` | [Copiare prima tutti i metadati](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/lib/iov_iter.c) |
| `android14-5.15` | `android14-5.15-2026-07_r1` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/pid.c) | `PIDFD_NONBLOCK` | [Copiare prima tutti i metadati](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/lib/iov_iter.c) |
| `android14-6.1` | `android14-6.1-2026-09_r1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/pid.c) | `PIDFD_NONBLOCK` | [Copiare prima tutti i metadati](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/lib/iov_iter.c) |
| `android15-6.6` | `android15-6.6-2026-07_r1` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/pid.c) | `PIDFD_NONBLOCK` | [Percorso a buffer singolo](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/lib/iov_iter.c) |
| `android16-6.12` | `android16-6.12-2026-09_r1` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/pid.c) | `PIDFD_NONBLOCK` e `PIDFD_THREAD` (`0x80`) | [Percorso a buffer singolo](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/lib/iov_iter.c) |
| `android17-6.18` | `android17-6.18-2026-09_r1` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/pid.c) | `PIDFD_NONBLOCK` e `PIDFD_THREAD` | [Percorso a buffer singolo](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/lib/iov_iter.c) |

Queste revisioni definiscono il contratto. Nuove release o backport richiedono verifica dei sorgenti e regressioni. GKI insieme a `pidfd_open` dichiarato assente viene rifiutato prima del caricamento.

## Sottoinsieme dei descrittori di processo

Trap x64/AArch64 e `syscall` Bionic condividono `LinuxServices` e la tabella del carico. Contano i 32 bit bassi di PID/flag; flag sconosciuti e PID con segno non positivi restituiscono `EINVAL` prima dell’allocazione. L’array opzionale `tasks` è un catalogo fisso e chiuso delle altre attività guest vive:

```json
{"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true},{"id":3000,"group_leader":false}]},"linux_files":{"files":[],"descriptor_limit":16}}
```

Il leader corrente PID 1000 è implicito anche con un array vuoto. Senza catalogo, la ricerca di altri target resta non supportata; un PID positivo valido esterno al catalogo dichiarato restituisce `ESRCH` prima dell’allocazione. Senza `PIDFD_THREAD`, un non-leader vivo restituisce `EINVAL` in 5.10–6.12 e `ENOENT` nel [`pidfd_prepare` 6.18](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/fork.c). Con il flag ammesso, 6.12/6.18 aprono non-leader dichiarati. I flag sono controllati prima della ricerca.

Ogni voce richiede `id` intero in 1..2147483647 e `group_leader` booleano; massimo 4096 voci. Duplicati, campi extra, PID 1000 non-leader e cataloghi senza GKI sono rifiutati. Le priorità devono riferirsi al catalogo o al processo corrente. Il catalogo fisso è incompatibile con thread Android cooperativi (`thread_limit > 1`); creazione, raccolta, credenziali e traduzione dei namespace richiedono una gestione della durata propria.

`linux_files` è richiesto: file e pidfd condividono proprietà e limite. Si assegna il minimo numero libero; esaurimento: `EMFILE`, `close` lo libera, doppia chiusura: `EBADF`. I numeri degli stream standard chiusi sono riutilizzabili. Non si interrogano pidfd, filesystem o processi host.

Su pidfd validi, `read`/`write` danno `EINVAL` prima dei dati; `lseek` verifica l’origine e dà `ESPIPE`. `writev` importa prima metadati e verifica intervalli utente: `EFAULT` può precedere `EINVAL` della scrittura assente. Non legge né acquisisce dati. stdout/stderr usano lo stesso importatore versionato; vedere l’[ordine VFS](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/fs/read_write.c).

5.10/5.15/6.1 copiano l’intero array iovec prima delle lunghezze: una negativa seguita da metadati inaccessibili dà `EFAULT`. Gli intervalli originali si verificano prima del limite, anche con un vettore. 6.6/6.12/6.18 verificano in sequenza e danno qui `EINVAL`; il buffer singolo viene limitato prima del controllo, il percorso multiplo conserva tutti gli intervalli originali. Le regole provengono da `copy_iovec_from_user`, `__import_iovec` e `import_ubuf`. Senza GKI resta la politica esistente a buffer singolo, senza inferire versioni.

Bionic converte errori raw negativi in `-1` ed `errno` locale al thread; il successo conserva `errno`. Mancano metadati per `fstat`. Polling, notifiche di uscita, segnali tramite pidfd, `pidfd_getfd`, `fcntl`, ioctl pidfs e attività non osservate restano non supportati. Un pidfd non implica scheduling o ciclo di vita.

## Verifica e copertura futura

`LinuxPIDFDTests.cpp` esegue ELF indipendenti x64/AArch64 O0/O2 per otto rami e trasporti disponibili: flag, tabella condivisa, limiti/riuso, ordine degli errori, metadati inaccessibili, limite/intervalli originali, cataloghi omessi/chiusi, non-leader e ricerca prima dell’esaurimento. `AndroidSyscallTests.cpp` ripete proprietà raw/Bionic, ricerca ed errno nei sei profili O0/O2 con rilocazioni ordinarie, Android packed e RELR. Sorgenti ed esecuzioni provano il sottoinsieme; non esiste evidenza di avvio nativo di ogni immagine GKI fissata. Estendere Linux servizio per servizio conservando versioni, configurazioni e osservazioni.
