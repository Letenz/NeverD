**Lingue**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: db2f5a72fda6a5195b548852dda4cebabfdea38e499c54e1b88f809dd2fc1055 -->

[← Indice della documentazione](README.md)

# Ambienti di processo guest macOS e iOS

`lib/emulation/os/darwin/` modella processi Mach-O autonomi con limiti espliciti, separati dal trasporto CPU host. Attivare `NEVERD_ENABLE_CPU_EMULATION`; l’emulazione dei driver Windows non è necessaria. `macos/` e `ios/` definiscono profili di piattaforma espliciti.

| Profilo | Piattaforma Mach-O | ISA guest | Pagina OS |
| --- | --- | --- | --- |
| `macos-macho64-v1` | macOS | x86-64, ARM64 di base | 4 KiB x64; 16 KiB ARM64 |
| `ios-macho64-v1` | dispositivo iOS | ARM64 di base | 16 KiB |
| `ios-simulator-macho64-v1` | iOS Simulator | x86-64, ARM64 di base | 4 KiB x64; 16 KiB ARM64 |

Un binario per dispositivo non è un’immagine simulatore; il profilo guest non viene dedotto dall’host. Con ISA coincidenti macOS può usare [HVF](macos-hvf.md), altrimenti `auto` usa Unicorn. La granularità CPU rimane 4 KiB. Le [API C, Python e CLI](process-emulation.md) condividono opzioni, limiti e rapporti.

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

## Immagine e avvio

`MachOExecutionImage` mantiene i byte originali senza patch di rilocazione dell’analisi. Sono ammesse soltanto immagini thin little-endian `MH_EXECUTE` con piattaforma e ingresso univoci. Un’immagine universale richiede l’estrazione esplicita della slice desiderata.

L’intero file, inclusi metadati e byte finali, deve rispettare `memory_limit` prima dell’analisi o copia. Il loader legge uno snapshot privato e limitato di un file regolare; rifiuta percorsi con NUL, letture incomplete e variazioni di dimensione. Non mantiene un mapping vivo del file. File e memoria guest hanno tetti separati dello stesso valore; l’I/O host non ha una garanzia temporale rigida.

I segmenti mantengono permessi correnti/massimi e riempimento a zero. `__PAGEZERO` riserva indirizzi senza allocarne l’intera estensione. Sono verificati intervalli file/VM, allineamenti OS, sovrapposizioni arrotondate, proprietà dell’header, ingresso eseguibile e budget. Il segmento dell’header deve essere leggibile ed eseguibile; pagine di guardia e ingresso privato di ritorno restano riservati. L’ultima pagina del file conserva byte fino al confine pagina o EOF; le pagine VM complete successive sono azzerate secondo il [loader XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c).

`LC_MAIN` riceve `argc`, `argv`, `envp` e il vettore apple come quattro argomenti interi; il ritorno produce gli otto bit bassi dello stato d’uscita. `/usr/lib/dyld` è ammesso solo per questo passaggio d’ingresso senza import, senza eseguire dyld host. Un `stacksize` diverso da zero è rifiutato: il budget appartiene all’opzione `stack_size` del chiamante.

`LC_UNIXTHREAD` richiede un solo record completo dei registri generali nativi a 64 bit, con il solo PC valorizzato. Lo stack contiene argc, argv/envp terminati e un vettore apple terminato con `executable_path=<input filename>`. SP/flag personalizzati, altri registri, flavor aggiuntivi e ingressi conflittuali sono rifiutati. Non sono ereditati ambiente host o vettore ausiliario Linux. Riferimento: [architettura dyld](https://github.com/apple-oss-distributions/dyld/blob/main/doc/dyld4.md).

Dylib esterne, import, rebases/chained fixups, costruttori/distruttori, sezioni TLS, arm64e/PAC, sottotipi CPU non supportati, payload cifrati e comandi non modellati falliscono prima dell’esecuzione. PIE senza fixup usa gli indirizzi preferiti, senza ASLR. Le firme sono metadati, non un modello AMFI o una politica di entitlements.

## Servizi Darwin

Le chiamate BSD su ARM64 usano X16, X0–X5 e `svc #0x80`; x64 usa la classe BSD `0x02000000`, RAX e RDI/RSI/RDX/R10/R8/R9. Il successo azzera carry; l’errore lo imposta e restituisce errno positivo. ARM64 azzera X1; x64 azzera RDX al successo e lo preserva in errore. Le modifiche ai registri di SYSCALL sono esplicite. Il rapporto usa `result` e `error=true` per errori BSD; richieste senza ritorno o non supportate non hanno questi campi. Le regole seguono XNU [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c) e [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c), senza incorporare codice Apple.

Servizi: `exit`, `write`, `getpid`, `getppid`, `getuid`, `geteuid`, `getgid`, `getegid`, `mmap`, `mprotect`, `munmap`. PID/UID/GID valgono 1000, PPID vale 1. I descrittori 1 e 2 catturano byte, inclusi NUL e non UTF8; quelli chiusi o di sola lettura restituiscono EBADF. Una copia parziale conserva i byte già letti ma il guasto successivo rimane EFAULT. Una lunghezza oltre `INT_MAX` produce EINVAL prima di controllare descrittore, puntatore o budget: [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).

La memoria supporta mapping privati anonimi di dati con `flags=0x1002`, descrittore -1 e offset zero. Lunghezze e suggerimenti non fissi sono arrotondati verso l’alto alla pagina OS. Un suggerimento occupato cerca prima verso indirizzi superiori, poi torna al posizionamento predefinito. Il mmap storico grezzo con lunghezza zero restituisce zero senza allocare; `MAP_UNIX03` è supportato e rifiuta lunghezza zero con EINVAL. Unmap/protect richiedono indirizzi allineati. NONE/READ/WRITE sono supportati e WRITE implica READ. Ogni pagina OS possiede la sua memoria fisica: un unmap parziale libera budget e nuove pagine sono azzerate. Un protect attraverso un buco o oltre i diritti massimi lascia invariato l’intero intervallo. Fonte: [servizi VM XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

Mapping condivisi/fissi/JIT o anonimi eseguibili, altri trap Mach, syscall indirette, thread, segnali, file host/rete, dyld, runtime Objective-C/Swift e Foundation/UIKit sono esclusi e arrestano esplicitamente l’esecuzione. Questo non è un intero OS Apple né l’applicazione iOS Simulator.

## Verifica

Le fixture C originali sono generate con Clang e `ld64.lld`, senza SDK Apple o binari proprietari. Coprono cinque combinazioni piattaforma/ISA, record Mach-O malformati, pagine 4/16 KiB e rilascio parziale a budget pieno. `NeverDProcessPublicTests` confronta C API/CLI; `NEVERD_TEST_LIBNEVERD` e `NEVERD_TEST_DARWIN_FIXTURES` abilitano le stesse cinque combinazioni in Python.

## File e descrittori espliciti

`darwin_files` offre ai tre profili un catalogo chiuso di file inizialmente di sola lettura. Il campo obbligatorio `files` contiene `path` guest assoluti canonici e `bytes_hex` esadecimali. `stdin_hex` opzionale fornisce input finito: l’assenza significa sconosciuto e arresta letture non vuote, una stringa vuota indica EOF. Senza catalogo open si arresta; un catalogo esplicitamente vuoto restituisce ENOENT. Non si consultano file o input host.

Si aggiungono `open`, `read`, `pread`, `lseek`, `close`, `dup`, `dup2`, `fcntl` e gli ingressi nocancel di read/write/open/close/fcntl/pread. Sono supportati O_RDONLY/O_CLOEXEC e F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL. Aperture separate hanno posizioni indipendenti; dup condivide la posizione ma mantiene flag close-on-exec distinti. pread non cambia posizione. Chiudere o sostituire 0/1/2 modifica l’I/O successivo; gli output duplicati conservano destinazione e budget.

Limiti: 256 file, 16 MiB complessivi per percorsi/NUL/file/input, percorsi sotto 1024 byte e componenti fino a 255. `descriptor_limit` è un limite esclusivo 3–4096, predefinito 256; JSON resta limitato a 64 KiB. Opzioni invalide falliscono prima del caricamento. read oltre INT_MAX restituisce EINVAL prima del controllo FD; EOF non tocca la destinazione e un indirizzo invalido dà EFAULT. Buffer parzialmente scrivibili arrestano prima di copia o avanzamento. Gli errori SET/CUR/END conservano la posizione. Stat precedente e altri fcntl restano esclusi. Un file come antenato dà ENOTDIR. Lo stesso oggetto viene confrontato con macOS nativo; C/CLI/Python coprono cinque combinazioni guest, non dispositivi iOS.

Verifica Release del 2026-10-05: 381 registrazioni, 177 passate, 204 saltate, zero errori e 51/51 casi ARM64 HVF obbligatori eseguiti. Passano anche sette programmi macOS nativi, 35 test pubblici C/CLI/report, cinque combinazioni Python e 66 test del verificatore. I conteggi si sovrappongono. Mancano prove native Intel HVF/KVM/WHP per i nuovi servizi; Intel HVF resta non validato e le sue Actions sospese. Mancano SDK iOS e confronto su dispositivo.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## Modificare file esistenti

Il booleano rigoroso `"writable":true` oppure `DarwinFileOptions::WritableFiles` autorizza modifiche locali al processo. Assente/false mantiene sola lettura; un’autorizzazione ignota arresta il servizio. Host e dati iniziali restano invariati. write(4/397), pwrite(154/415), truncate(200), ftruncate(201) e O_TRUNC condividono contenuti; open mantiene posizioni indipendenti, dup condivide posizione e stato, e i contenuti sopravvivono all’ultimo close. L’estensione aggiunge zeri; il troncamento conserva le posizioni, anche O_RDONLY|O_TRUNC.

F_SETFL cambia solo O_APPEND e conserva accesso, close-on-exec e FWASWRITTEN. F_GETFL espone 0x10000 dopo byte effettivamente trasferiti, inclusi pwrite e output catturato. pwrite ignora append e conserva la posizione. INT_MAX viene controllato prima del FD; pwrite a -1 restituisce EINVAL ancora prima. INT64_MAX dà EFBIG prima del caso vuoto; la lunghezza viene ridotta prima di scegliere EOF.

ftruncate riuscito, anche a parità di dimensione, imposta FWASWRITTEN sulla descrizione chiamata e sui suoi dup. O_TRUNC lo imposta sulla nuova descrizione, anche O_RDONLY; truncate per percorso non cambia quelle esistenti.

Input parzialmente leggibile si arresta prima degli effetti. EFAULT completo conserva i byte, ma append non vuoto sposta la posizione a EOF. Errori del trasporto non confermano contenuti o posizione. Senza `mutation_policy`, scritture non vuote, troncamenti ed EFAULT completi non vuoti invalidano l’intera osservazione stat; le query successive si arrestano prima della copia. Una scrittura vuota la conserva. I 16 MiB contano percorsi/NUL, input, record, CWD, contenuti attuali e riferimenti ai percorsi scrivibili. Ridurre sostituisce il backing e libera capacità; input originale e un buffer di sostituzione limitato sono aggiuntivi. Alias inode noti e flag immutable/append-only sono rifiutati.

DarwinMemory trattiene lease fino all’ultimo unmap, inclusi PROT_NONE e FD chiusi; le modifiche si arrestano finché esistono. Errori e vecchi mmap di lunghezza zero non trattengono lease. Le nuove mappe vedono i byte attuali. O_WRONLY con READ/WRITE dà EACCES; PROT_NONE può acquisire lettura/scrittura tramite mprotect.

Programmi originali normali/nocancel confrontano il kernel nativo; test 4K/16K e C/CLI/Python coprono cinque combinazioni. Controllo dei permessi, rimozione di directory, rinomina tra genitori, hard link, metadati del file system nativo, coerenza delle mappe e SIGBUS EOF restano incompleti. Ambiente completo, iOS fisico e Intel HVF non sono validati; Actions Intel resta sospeso.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233","writable":true}]}}
```

[XNU write](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU vnode](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c).

## Metadati mutabili espliciti

Un file può aggiungere mutation_policy accanto a `writable: true` e metadata completi; C++ usa `DarwinFileOptions::MutationPolicies`. È un contratto virtuale esplicito di allocazione sparsa, senza dedurre APFS o leggere l’orologio host. In sua assenza i metadati dopo modifica restano ignoti.

allocation_unit, mutation_time e seconds/nanoseconds sono obbligatori e seguono le regole intere senza perdita. L’unità è una potenza di due fra512 byte e16 MiB, indipendente da block_size e pagine VM. Servono permessi ordinari senza set-id/sticky, flags=0, link_count=1 e allocazione iniziale densa: blocks=ceil(size/allocation_unit)*(allocation_unit/512). Gli zeri non implicano buchi. Il riferimento al percorso conta nei16 MiB logici; il registro di allocazione non inventa ENOSPC.

La scrittura alloca ogni unità toccata, anche zeri nei buchi. truncate in crescita aggiunge zeri senza allocare; riducendo scarta le unità oltre EOF arrotondato in alto e conserva l’ultima parziale. Ricrescere non recupera le allocazioni scartate. Scritture non vuote riuscite e ogni truncate riuscito, anche di uguale dimensione o O_TRUNC vuoto, aggiornano size/blocks e impostano mtime/ctime al tempo fisso. Altri campi e input restano uguali; read non avanza atime. Stat per percorso, open indipendenti, dup e riapertura condividono il nodo.

Scritture vuote, rifiuti di budget/mappe, input parziale rifiutato ed errori backend conservano lo stato. EFAULT completo non vuoto lo rende ignoto; successi successivi non lo ricostruiscono. La copia stat fallita non cambia il nodo. virtual-file-metadata verifica144 byte su cinque profili e C/CLI/Python: test della politica, non equivalenza APFS. I programmi nativi verificano separatamente flag, posizioni ed errori. Namespace, coerenza nativa, Mach e caricamento dinamico restano incompleti.

```json
{"mutation_policy":{"allocation_unit":4096,"mutation_time":{"seconds":-7,"nanoseconds":123456789}}}
```



## Posizionamento nei file sparsi

Con mutation_policy e allocazione nota, lseek accetta SEEK_HOLE=3 e SEEK_DATA=4 sui file regolari usando lo stesso registro di stat. L’input iniziale è denso anche nei byte zero. In un’unità del tipo richiesto restituisce l’offset dato, altrimenti l’inizio della successiva unità corrispondente. Il buco terminale parte da EOF. Valori negativi danno EINVAL; a/oltre EOF, anche nel file vuoto, o senza dati successivi danno ENXIO=6. Gli errori conservano il cursore; il successo cambia solo la descrizione e i suoi dup. Altri open hanno cursori indipendenti e riaprire vede l’allocazione attuale. Metadati, flag e byte non cambiano; i bit alti di whence sono ignorati.

Senza politica, per directory o dopo EFAULT completo con allocazione ignota il servizio resta escluso. Zeri e modifiche rifiutate non implicano allocazione. sparse-file-seek confronta errori, byte scritti, EOF e vita delle descrizioni native/guest senza assumere precedenti confini del FS. virtual-file-metadata controlla separatamente la geometria esatta della politica; C/CLI/Python coprono cinque profili.

[XNU lseek](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Rimozione dei nomi di file regolari

`mutable:true` per directory (C++ `MutableDirectories`) autorizza i cambiamenti dei nomi immediati, indipendentemente da `writable`. Senza autorizzazione il modello si ferma. Rifiuta flags noti non nulli, permessi speciali del padre, link_count≠1 del figlio e alias noti padre/figlio; unisce stat e inode degli snapshot, distinguendo dispositivi esplicitamente diversi. I percorsi consumano il budget esistente.

`unlink(10)` / `unlinkat(472)` rimuovono nomi regolari esistenti. unlinkat accetta solo i32 bit bassi 0 o `0x800`; bit sconosciuti danno EINVAL prima di percorso/FD, altri modi noti di rimozione restano non supportati. Risoluzione comune: ENOENT, ENOTDIR per file seguito da `/`, EPERM per directory ordinaria, EBUSY per radice. I suffissi `.`/`..` sono verificati nativamente.

FD/dup/aperture indipendenti esistenti mantengono dati, cursori e flags; F_GETPATH conserva il vecchio percorso acquisito. Le nuove aperture falliscono, genitori impliciti e CWD restano. Il permesso di scrittura appartiene all’oggetto; close/dup2/modifica successiva recuperano i byte correnti solo dopo l’ultimo descrittore e mapping. I costi iniziali dei percorsi restano; controllo dei permessi, rinomina tra genitori, hard link e rimozione directory sono ancora incompleti.

stat/readdir/SEEK_END del padre diventano sconosciuti per ogni FD/percorso e si fermano prima di copia/cursore. read/pread restano EISDIR; SET/CUR/F_GETPATH/fchdir/risoluzione relativa continuano. La politica nota imposta nlink=0 e ctime fisso; scritture successive non ripristinano nlink=1. Senza politica/dopo EFAULT i metadati restano ignoti. Errori preservano lo stato. `unlinked-file` confronta regole native nome/FD; tempi e invalidazione sono regole esplicite del modello.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"00"}],"directories":[{"path":"/work","mutable":true}]}}
```

[XNU unlink / unlinkat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [F_GETPATH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c).

## Creazione di file regolari

O_CREAT=0x200 crea un file vuoto nel genitore diretto esplicitamente mutable, tramite open/openat normale o nocancel. Il nuovo oggetto è scrivibile; quelli esistenti mantengono WritableFiles. Un FD di sola lettura può creare ma non scrivere. Senza politica di creazione esplicita, stat64 e ricerca sparsa restano sconosciuti. metadata/mutation_policy del vecchio omonimo non vengono mai ereditati.

O_EXCL=0x800 con O_CREAT restituisce EEXIST per file/directory esistenti prima del troncamento; da solo non ha effetto. O_CREAT di sola lettura apre directory esistenti. Ordine: accesso invalido, disponibilità FD, EINVAL per O_CREAT|O_DIRECTORY, percorso. Si crea solo l’ultimo componente originale mancante; antenati assenti e suffissi `/`, `//`, `/.`, `/..` danno ENOENT. Nuovo O_CREAT|O_TRUNC non imposta FWASWRITTEN, mentre il troncamento esistente sì.

Solo l’inserimento invalida le osservazioni del genitore. Vecchi/nuovi omonimi mantengono dati, FD, metadati e mapping indipendenti. Le 256 voci includono elementi iniziali non file e oggetti vivi; percorsi canonici/NUL dinamici e byte correnti contano nei 16 MiB. Dopo unlink, l’ultimo FD/mapping libera i costi dinamici; quelli iniziali restano. Budget esaurito o percorso canonico di almeno 1024 byte arrestano esplicitamente senza inventare ENOSPC o errno nativo né pubblicare nome/FD. created-file confronta macOS nativo e cinque profili; test 4K/16K verificano i limiti. Controllo dei permessi, rinomina tra genitori, link e mutazione directory restano aperti.

[XNU open](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

## Metadati di creazione espliciti e umask del processo

L’opzione `darwin_files.umask` (C++ `InitialUmask`) dichiara la maschera iniziale da 0 a 07777 ottale, indipendentemente dal permesso di creazione. `umask(60)` restituisce la precedente e salva i bit bassi 07777, senza memoria guest o FD libero. L’omissione significa sconosciuta, senza dedurre valori host o predefiniti. Si inizializza una volta e le modifiche riguardano solo le creazioni future, senza cambiare l’input. L’esempio usa 18 decimale, cioè 0022 ottale.

L’opzione `darwin_files.creation_policy` (C++ `CreationPolicy`) fornisce metadati completi ai nuovi oggetti. L’oggetto rigoroso contiene esattamente `first_inode`, `block_size`, `generation`, `creation_time`, `mutation_policy`; tempi e politica di modifica usano i formati esistenti. Richiede umask esplicita, almeno un genitore mutable e metadata completi per ogni genitore autorizzato. block_size è 1..INT32_MAX, generation è uint32; l’unità di allocazione è una potenza di due tra 512 e 16 MiB, indipendente dal blocco/pagina VM, e i nanosecondi sono in [0,1000000000). first_inode è uint64 positivo maggiore di tutti gli inode stat/snapshot, anche di altri dispositivi. Stringhe decimali preservano gli interi oltre l’intervallo esatto JSON.

Solo l’inserimento riuscito di un nuovo oggetto consuma la sequenza globale inode. UINT64_MAX la esaurisce definitivamente; close/unlink/riuso del nome/umask/ricerche non la reinizializzano. Rifiuti per esclusività, FD, percorso, voci o byte non pubblicano nome/FD né incrementano il contatore; O_CREAT esistente non consuma nulla. Il nuovo stat64 eredita device/GID dal genitore diretto, UID=1000 dall’utente effettivo guest fisso, mode `S_IFREG | (mode & 0777 & ~umask)`, nlink=1 e size/blocks/flags=0. Blocco, generation e quattro tempi iniziali fissi provengono dalla politica. Dopo l’invalidazione di stat/enumerazione completi del genitore, device/GID restano utilizzabili senza ripristinare l’intero record.

Ogni nodo possiede metadati/allocazione propri, senza ereditare il vecchio omonimo. write/truncate/unlink condividono la politica e preservano inode/mode/birthtime e nlink=0 dopo unlink; EFAULT completo lascia lo stato permanentemente sconosciuto. Nessun effetto retroattivo sui nodi esistenti. `created-file-metadata` confronta permessi, vecchia maschera, UID effettivo, dispositivo/gruppo genitore e durata nativa in cinque profili; `virtual-created-metadata` confronta separatamente tutti i 144 byte. I quattro tempi nativi possono differire. Tempo fisso/allocazione sparsa sono regole virtuali; controllo permessi, cambio credenziali, ACL e APFS nativo restano da implementare.

```json
{"darwin_files":{"files":[],"umask":18}}
```

[XNU creation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU umask](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c). [XNU rename / renameat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Rinomina di file regolari nello stesso genitore

`rename(128)`, `renameat(465)` e `renameatx_np(488)` rinominano o sostituiscono nel medesimo genitore diretto esplicitamente modificabile. Anche lo stesso nome richiede autorizzazione; se ammesso conserva le osservazioni. I 32 bit bassi accettano 0 o `RENAME_NOFOLLOW_ANY=0x10`; bit ignoti ed EXCL+SWAP danno EINVAL prima dei percorsi, altri flag noti non sono supportati. Il risolutore condiviso conserva precedenza sorgente, FD e controlli dei componenti originali. Una sorgente directory si arresta subito; un punto/doppio punto finale risolto dà EINVAL prima di mount/autorizzazione, anche con genitori annidati o diversi. Gli errori degli antenati assenti/non directory conservano precedenza. Una directory ordinaria nel genitore ammesso dà EISDIR.

Vecchi FD, open indipendenti e dup seguono il nuovo nome con `F_GETPATH`. L’oggetto sostituito conserva ultimo percorso, byte, cursori, flag e mapping; permessi di scrittura e metadati non passano alla sorgente. Le politiche note aggiornano ciascun ctime e nlink=0 della destinazione, preservando identità, proprietario, nascita e allocazione. Senza politica/dopo EFAULT completo, i metadati restano ignoti. Solo una modifica reale invalida stat/elenco del genitore.

Nessun nuovo inode, voce o FD libero necessario. Percorso/NUL sostituisce il costo dinamico sorgente; i costi iniziali restano. Solo una destinazione senza vecchi FD/mapping contribuisce subito alla capacità, recuperata una volta; unmap parziale conserva il costo intero. Percorso da 1024 byte o oltre 16 MiB arresta prima delle modifiche. Altri genitori, device noti discordanti, directory, swap/exclusive/seclude e permessi restano aperti. Stesso device stat non prova stesso mount; EXDEV non viene inventato. `renamed-file` confronta identità, percorsi, sostituzione e mapping nativi/guest; tempi e budget sono regole virtuali.


## Directory e percorsi relativi

`directories` ammette `path` assoluti canonici con `metadata` completo opzionale, anche per directory vuote. Radice e antenati sono impliciti; i metadati non creano percorsi assenti. Mode è `0x4000` più permessi; size è un’osservazione esplicita in [0, INT64_MAX]. `working_directory` deve esistere; ometterlo lascia CWD sconosciuta, senza ereditarla dall’host. Massimo 256 percorsi dichiarati, inclusi antenati con metadati; percorsi/NUL/contenuto/input/CWD totalizzano 16 MiB.

`openat` (463), `openat_nocancel` (464), `chdir` (12), `fchdir` (13) e `fstatat64` (470) condividono il risolutore. I percorsi relativi usano FD di directory o `AT_FDCWD=-2`; quelli assoluti ignorano FD. Separatori ripetuti, `.`, `..` e barra finale controllano ogni antenato: `/file/..` dà ENOTDIR, `/missing/..` ENOENT. Errori e chiusura, riutilizzo o sostituzione del FD originale conservano CWD. `F_GETPATH=50` copia percorso canonico e NUL anche dopo dup, senza modificare i byte successivi.

read/pread di directory dà EISDIR anche con lunghezza zero; offset pread negativo dà prima EINVAL. SET/CUR condividono il cursore, END richiede size esplicita; mmap dà EINVAL. fstatat64 ammette 0, `AT_SYMLINK_NOFOLLOW=0x20`, `AT_SYMLINK_NOFOLLOW_ANY=0x800` e `AT_FDONLY=0x400` (ignora il percorso). Bit invalidi danno EINVAL; `AT_REALDEV=0x200` resta escluso. Identità dei flussi sconosciute, permessi senza controllo accessi; le modifiche restano da implementare. Lo stesso `directories` confronta kernel nativo e cinque guest; stat controlla file e directory reali. Intel HVF Actions resta sospeso.

[XNU VFS](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/fcntl.h).

Verifica directory (2026-10-05, Release): 467 registrazioni, 227 superate, 240 saltate, zero errori; eseguiti 60/60 requisiti ARM64 HVF. Superati 10 programmi macOS nativi, 37 test C/CLI/report senza omissioni, cinque guest Python e 66 test degli strumenti. Conteggi sovrapposti. Evidenze: `build-hvf-arm64/darwin-directory-verified-evidence/`. Altri backend nativi e iOS fisico restano da verificare.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## Istantanee esplicite delle directory

`getdirentries64` (344) enumera il `contents` immutabile facoltativo di una voce `directories` esistente; C++ usa `DarwinFileOptions::DirectoryContents`. `entries` include in ordine esplicito tutti i figli diretti, `.` e `..`. Senza istantanea anche una directory vuota resta sconosciuta. Non crea percorsi o stat e non consulta l’host.

Ogni voce richiede `name`, `inode` non nullo, `type` (0 sconosciuto, 4 directory, 8 file), `next_offset` e `seek_offset`. Tipo e percorso concordano; gli inode dello stesso percorso risolto concordano tra istantanee e metadati. `next_offset` è positivo, unico nella directory e <=INT64_MAX, senza obbligo di crescere; zero riavvolge. `seek_offset` è l’osservazione d_seekoff distinta, a 64 bit senza segno; sono ammessi zeri ripetuti. Gli interi usano le stringhe decimali senza perdita di stat.

`contents.minimum_buffer_size` richiede un minimo di payload di 1–128 MiB, EOF incluso. Il `minimum_buffer_size` facoltativo della voce (predefinito 0) vincola la lettura che parte lì. L’esempio osserva APFS: 64 byte per i due punti iniziali, 1 a EOF; altrove deve entrare un record intero. LP64 allinea a otto byte, dimensione `roundUp(25 + nameBytes, 8)`. Massimo 4096 voci complessive; i byte dei record contano nei 16 MiB. Gli antenati dichiarati solo da metadati/istantanea contano una volta nei 256 percorsi. JSON resta limitato a 64 KiB.

Open indipendenti hanno cursori separati, dup li condivide. La ripresa accetta solo zero o cookie forniti; posizioni sconosciute arrestano esplicitamente. Ogni chiamata restituisce il massimo prefisso di record interi. Lunghezza >=1024 riserva gli ultimi quattro byte richiesti a EOF (1 alla fine, altrimenti 0); solo il payload è limitato a 128 MiB. L’indirizzo conserva l’aritmetica originale senza segno, overflow compreso. Ordine: dati, avanzamento, posizione precedente, flag. EFAULT successivi mantengono gli effetti precedenti; EOF omette la copia vuota. Una singola copia parzialmente scrivibile si arresta prima di quella copia, conservando gli effetti anteriori.

`directory-entries` confronta campi, dup/riavvolgimento, letture piccole, EOF e ordine delle copie con macOS. Un test separato confronta tutti i byte nativi catturati, nomi lunghi compresi, con il layout SDK. I cookie fissi non riproducono le generazioni dinamiche APFS. Il vecchio `getdirentries` (196), le modifiche, altri backend nativi e iOS fisico restano fuori da questa verifica.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"directories":[{"path":"/empty"},{"path":"/","contents":{
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":73,"type":8,"next_offset":99,"seek_offset":0}]}}]}}
```

[XNU getdirentries64](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [dirent ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [extended flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent_private.h).

Verifica enumerazione (2026-10-05, Release): 498 casi Darwin, 246 superati, 252 saltati per backend indisponibile, zero errori; eseguiti tutti i 63/63 casi ARM64 HVF obbligatori. Superati 11 programmi macOS nativi, 40 controlli C/CLI/report senza salti, cinque combinazioni Python con otto scenari di file ciascuna e 66 test degli strumenti. Conteggi sovrapposti. Prove: `build-hvf-arm64/darwin-dirents-merged-evidence/`. Intel HVF Actions resta sospeso; altri backend nativi e iOS fisico non sono verificati.


## Mapping privati di file

`mmap` accetta file regolari del catalogo con `MAP_PRIVATE`: `flags=0x2` o `0x40002` con `MAP_UNIX03`, con offset allineato alla pagina OS. Anche una richiesta breve mantiene tutti i byte del file nella pagina; il resto dell’ultima pagina EOF è zero. La scrittura privata modifica solo quel mapping, non file, altri mapping, metadati fissi o posizione condivisa. Il mapping sopravvive a close e al riuso del FD. Anche sola lettura e PROT_NONE ricevono i byte iniziali; `mprotect` può abilitare la scrittura.

Overflow della fine del file, lunghezza UNIX03 zero e offset UNIX03 non allineato danno EINVAL prima della ricerca FD; un FD invalido dà EBADF prima del budget. Anche la lunghezza storica zero controlla il FD. Offset storici non allineati, stream, file vuoti e pagine interamente oltre EOF arrestano prima dell’allocazione. macOS permette questi mapping EOF ma l’accesso genera SIGBUS; il modello non inventa pagine zero leggibili o consegna di segnali. Mapping condivisi, fissi, eseguibili e JIT restano esclusi.

`DarwinFiles` risolve FD e byte; `DarwinMemory` gestisce posizionamento, diritti, budget e rollback. I dati arrivano solo da `darwin_files`. Il programma comune `file-mapping` verifica copie, close, posizioni, errori e riuso anonimo; un confronto nativo separato verifica offset non nullo, intera pagina e SIGBUS.

[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c)

### Verifica dei mapping privati, 2026-10-05

Release Darwin: 438 registrazioni uniche, 210 superate, 228 saltate, zero errori; eseguiti tutti i 57/57 requisiti ARM64 HVF e cinque combinazioni Unicorn. Superati nove programmi macOS nativi, confronto dell’intera pagina con offset non nullo e SIGBUS in un figlio isolato. I 36 controlli API/report non hanno omissioni; Python copre cinque combinazioni con `file-mapping`, e passano 66 test degli strumenti e 38 della provenienza. I conteggi si sovrappongono. Evidenze: `build-hvf-arm64/darwin-mmap-verified-evidence/`. Nessuna nuova prova Intel HVF/KVM/WHP o iOS fisico; Intel HVF Actions resta sospeso.

## Metadati espliciti dei file

Una voce può aggiungere `metadata`; tutti i campi seguenti sono obbligatori. Le stringhe decimali mantengono l’intera larghezza; i numeri JSON sono interi esatti entro ±(2^53−1). device è a 32 bit con segno, mode/link_count a 16 senza segno, inode a 64 senza segno e uid/gid/flags/generation a 32 senza segno. size deve corrispondere ai byte; blocks rientra nei 64 bit con segno e block_size nei 32 bit con segno non negativi. I tempi usano secondi a 64 bit con segno e 0–999999999 nanosecondi.

`stat64` (338), `fstat64` (339) e `lstat64` (340) restituiscono lo stesso record LP64 di 144 byte su ARM64/x64. Condividono la risoluzione di open e rispettano dup/close senza allocare FD né cambiare cursori. rdev, padding e campi riservati sono zero. Gli input forniscono i metadati iniziali e la politica facoltativa governa le modifiche; read non aggiorna i tempi e mode non cambia l’accesso al catalogo. Metadati mancanti, flussi, link simbolici, stat precedente, e sicurezza estesa restano esclusi. Gli errori di percorso/FD precedono il puntatore di uscita; le uscite parzialmente accessibili sono rifiutate prima della scrittura. Il test nativo confronta tutti i byte di un file reale e gli offset SDK; lo stesso programma originale verifica le tre chiamate.

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

### Verifica dei metadati e passi successivi (2026-10-05)

Con stat64: 409 registrazioni uniche, 193 passate, 216 saltate, zero errori; eseguiti 54/54 casi ARM64 HVF obbligatori e cinque combinazioni Unicorn. Passano il confronto SDK/record reale, otto programmi nativi, 36 casi API/report senza salti, cinque combinazioni Python e 66 test degli strumenti; i conteggi si sovrappongono. Ogni caso nativo usa il proprio file di uscita, evitando byte residui dopo uscite più brevi. Mancano prove native Intel HVF/KVM/WHP o iOS fisico per queste aggiunte.

Seguono mapping condivisi e fault EOF, scritture limitate (pagine EOF, durata dopo close, ordine degli errori), osservazioni esplicite tempo/sistema, servizi Mach/thread necessari e dipendenze Mach-O, rebases/binds, inizializzatori e TLS. Objective-C/Swift e Foundation/UIKit richiedono programmi nativi di riferimento. iOS fisico necessita SDK e dispositivo; Intel HVF resta non verificato e le sue Actions sospese.



```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

La verifica autonoma richiede tutti i 99 casi nativi ARM64 o 66 x64, compresi `LC_MAIN` e `LC_UNIXTHREAD` su ogni piattaforma. Casi obbligatori mancanti/saltati o assenza di `ld64.lld` causano errore.

```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

Usare `kvm` su Linux o `whp` su Windows. Il [workflow Darwin](../../.github/workflows/darwin-native.yml) esegue entrambi i trasporti x64 senza Unicorn e permette ripetizioni separate. Il [riferimento kernel](../../.github/workflows/darwin-kernel-reference.yml) esegue i programmi direttamente su entrambe le ISA macOS, senza NeverD/LLVM. `DarwinNativeCases.def` definisce modi, stati e byte previsti. Solo il riferimento host collega libSystem per il vero ingresso dyld. ISA errata, Rosetta, timeout o divergenze fanno fallire il controllo; non è prova del kernel su dispositivo iOS.

## Evidenze e ambito residuo

Risultati al 2026-10-03; le righe sovrapposte non si sommano:

| Trasporto | Sorgente | Superati | Falliti | Saltati | Carichi nativi |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 HVF | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel HVF | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| x64 KVM | `36e11ca8a` | 51 | 0 | 235 | 26/26 |
| x64 WHP | `36e11ca8a` | 51 | 0 | 235 | 26/26 |

Il [run Intel](https://github.com/NeverSight/NeverD/actions/runs/37106013999) riconcilia 286 identità CTest e 32 processi con XML originale. I 234 casi saltati comprendono 65 casi Unicorn disabilitati, 39 guest ARM64 e 130 altre piattaforme host. L’artefatto `11267489438` ha SHA-256 verificato `cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`. Anche [KVM/WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703) sono stati controllati indipendentemente. Il [riferimento kernel](https://github.com/NeverSight/NeverD/actions/runs/37064795867) supera 4/4 programmi per ISA, con stato 37, output esatto e stderr vuoto.

C API/CLI con Unicorn: 138 superati, 156 saltati, nessun errore. Python copre tutte le cinque combinazioni; il motore nel pacchetto coincide con 18 rapporti CLI ARM64 e 186 immagini Mach-O superano i controlli di firma. HVF/Unicorn OFF supera 38 controlli, ne salta 231 e non collega Hypervisor.framework. Sono prove d’integrazione, non ulteriori esecuzioni native. La CPU Intel completa resta da validare; vedere [HVF](macos-hvf.md) e il [registro dettagliato](../darwin-emulation.md#hosted-native-verification-2026-10-03).

## Osservazioni temporali esplicite

`ProcessOptions::DarwinTime` / `darwin_time` fornisce osservazioni fisse alla chiamata diretta `gettimeofday` (116), inclusa la terza uscita `mach_absolute_time`, su tutti i profili Darwin. `time_of_day`, `timezone` e `mach_absolute_time` sono facoltativi: l’assenza significa sconosciuto, lo zero esplicito è un valore. Un oggetto vuoto non crea orologi predefiniti. Il modello non legge l’orologio host, non deduce il fuso, non fa avanzare il tempo e non converte i tick assoluti.

Ogni record fornito richiede tutti i membri. `seconds` è senza segno a 32 bit, `microseconds` appartiene a [0, 999999], `minutes_west` / `dst_time` hanno segno a 32 bit, i tick sono senza segno a 64 bit. JSON segue le regole intere senza perdita; fuori dall’intervallo sicuro servono stringhe decimali. Campi sconosciuti, intervalli errati e profili non Darwin sono rifiutati prima del caricamento.

Il `timeval` LP64 occupa 16 byte: secondi estesi con zero a offset 0, microsecondi a 32 bit a 8 e quattro byte zero a 12. Il fuso ha due campi con segno a 32 bit, i tick otto byte. Tempo civile e assoluto costituiscono un unico campione iniziale: tutte le osservazioni richieste devono esistere prima delle copie e dei controlli dei puntatori. Seguono timeval, timezone e absolute ticks. Fuso mancante o EFAULT successivo conservano le scritture precedenti; gli alias seguono lo stesso ordine. Un’uscita singola parzialmente scrivibile causa arresto prima della sua copia, conservando quelle precedenti. Tutti i puntatori nulli riescono senza configurazione; le richieste selettive richiedono solo i valori domandati.

Il programma originale `time` verifica il comportamento nativo; `time-values` emette i 32 byte configurati tramite C/CLI/Python nelle cinque combinazioni guest. Un oracolo SDK confronta ogni byte con tre uscite di una sola chiamata nativa diretta. Restano esclusi orologi in avanzamento, conversione, contatori commpage, timer e oggetti orologio Mach/IPC, oltre al lavoro su dyld, thread, Objective-C/Swift e Foundation/UIKit. Intel HVF Actions resta sospeso; non si aggiunge accettazione nativa Intel o iOS fisico.

```json
{"darwin_time":{"time_of_day":{"seconds":4045620583,"microseconds":654321},"timezone":{"minutes_west":-480,"dst_time":-1},"mach_absolute_time":"18364758544493064720"}}
```

[XNU gettimeofday](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_time.c), [time ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/time.h).

Verifica temporale (2026-10-06, Release): 538 casi Darwin, 274 superati, 264 saltati per backend indisponibile, nessun errore; eseguiti tutti i 66/66 casi ARM64 HVF obbligatori. Passano i 12 programmi macOS nativi e il confronto SDK di un solo campione. C/CLI/report: 43/43 senza salti. Python passa su cinque combinazioni, inclusi i byte temporali esatti e gli otto modi di file esistenti. Passano 66 test degli strumenti, localizzazione, capacità e formato. Conteggi sovrapposti. Prove: `build-hvf-arm64/darwin-time-verified-evidence/`, `darwin-time-native-first/`, `darwin-time-public.xml`.

## Tempo Mach e convenzioni di ritorno

`darwin_time.timebase` fornisce `numerator` e `denominator`, interi senza segno a 32 bit diversi da zero. Il rapporto resta esatto, senza riduzione o conversione. `mach_timebase_info_trap`, indice 89, usa ARM64 X16=-89 o x64 RAX=0x01000059. Scrive otto byte little-endian (numeratore, denominatore) e restituisce zero anche con un indirizzo di uscita completamente invalido. Un’uscita parzialmente scrivibile arresta prima della copia; gli errori del trasporto si propagano. La configurazione assente arresta prima del controllo del puntatore, anche nullo.

ARM64 X16=-3 e X16=-4 restituiscono tutti i 64 bit senza segno di `mach_absolute_time` e `mach_continuous_time`. Ogni chiamata richiede solo il proprio valore; zero esplicito è valido. Le corrispondenti voci native x64 generano EXC_SYSCALL e non sono supportate. Restano esclusi avanzamento degli orologi, commpage, timer e oggetti orologio Mach/IPC.

La risoluzione usa i 32 bit bassi del numero; il rapporto conserva i 64 originali. I negativi ARM64 selezionano Mach; x64 usa 0x01000000 per Mach e 0x02000000 per BSD. BSD 3/4 restano read/write; numeri sconosciuti e classi estranee arrestano. La voce risolta determina il ritorno: Mach preserva flag e X1/RDX, BSD mantiene le regole carry; x64 aggiorna ancora RCX/R11. I rapporti Mach contengono `result` e omettono `error`, anche con carry iniziale attivo.

`mach-time` confronta flag, risultato secondario, bit alti, puntatori invalidi e transizioni BSD con il kernel ARM64 nativo. `mach-timebase-values` verifica byte esatti su cinque guest, `mach-clock-values` su ARM64; l’SDK verifica layout e rapporto osservato. Intel HVF Actions resta sospeso; test software e sintattici x64 non costituiscono accettazione Intel nativa o iOS fisica.

```json
{"darwin_time":{"timebase":{"numerator":125,"denominator":3},"mach_absolute_time":"18364758544493064720","mach_continuous_time":"18446744073709551615"}}
```

[XNU clock traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/clock.c), [ARM64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [ARM64 special traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/sleh.c), [x64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/x86_64/idt64.s).

Validazione Mach (2026-10-06, Release): 569 casi Darwin, 293 superati, 276 saltati per backend indisponibile, zero errori; eseguiti tutti i 69/69 obbligatori ARM64 HVF. Il passaggio finale supera 13 programmi nativi e due oracoli temporali SDK. C/CLI/report: 100/100 senza salti; Python copre cinque guest. I confronti pubblici sono separati per piattaforma e scenario con budget guest esplicito di 10 secondi; valori predefiniti e regressioni sulle scadenze restano invariati. I conteggi si sovrappongono.

I primi avvii nativi superavano il limite esistente di 5 secondi: misura indipendente di 6.056 secondi e 0.010 al riuso. Lo stesso binario ha poi superato 13 casi col limite originale; i fallimenti sono conservati. La verifica seriale separata supera i precedenti timeout sotto carico host. Prove: `build-hvf-arm64/darwin-mach-time-final-evidence/`, `darwin-mach-time-native-recheck/existing-binary-recheck.json`, `darwin-mach-time-public-accepted.xml`, dall’albero prima del commit. In quella revisione ARM64 MRS/MSR NZCV erano fuori dal contratto checked; il test osservava i flag con istruzioni intere. La modifica seguente colma questa lacuna CPU.

## Registro dei flag di condizione ARM64

Il contratto ARM64 checked condiviso ammette le codifiche esatte `MRS Xt, NZCV` e `MSR NZCV, Xt` a EL0/EL1. Le letture restituiscono solo i bit 31–28; le scritture selezionano quei quattro bit in ingresso e ignorano gli altri. Leggere verso `XZR` scarta il risultato; scrivere da `XZR` azzera i flag senza leggere SP. Ogni backend esegue le istruzioni originali. La validazione del setter host e i limiti FPCR/FPSR non cambiano; i registri di sistema vicini non elencati restano non supportati.

`NeverDAArch64NZCVTests` confronta tutte le combinazioni con le istruzioni host e verifica stato scalare/vettoriale completo, memoria, registri limite, arresto/errore degli osservatori, ripristino del contesto e budget condivisi. ARM64 `mach-time` usa ora veri MSR/MRS attorno a SVC per controllare conservazione Mach e transizione a BSD. I requisiti HVF nativi includono i sei metodi a entrambi i privilegi e l’oracolo host. ARM64 KVM/WHP e iOS fisico restano non validati. Restano file scrivibili, informazioni di sistema, clock che avanzano, Mach IPC/thread, dyld/runtime/framework e accettazione su dispositivo.

[Arm NZCV (DDI0601, 2025-06)](https://developer.arm.com/documentation/ddi0601/2025-06/AArch64-Registers/NZCV--Condition-Flags).


Validazione dei file modificabili (2026-10-06): Release Darwin, 610 registrazioni, 322 passate, 288 saltate per backend indisponibile, zero errori; 72/72 requisiti ARM64 HVF eseguiti. Verifica finale con nuove asserzioni EFAULT/metadati: 102 passate, 12 saltate su 114. Passano anche tutti i 15 programmi nativi e 111 test pubblici C/CLI/report. I conteggi si sovrappongono. Il primo tentativo nativo ha individuato FWASWRITTEN, corretto prima dei successi; l’errore è conservato. Nessuna scadenza cambiata. CI GitHub completa e iOS fisico restano separati; Actions Intel sospeso.

`build-hvf-arm64/writable-darwin-evidence/` · `writable-native-final/` · `writable-focused-final.xml` · `writable-public.xml`

Python ha inizialmente superato cinque secondi in tre casi directory ARM64. A parità di argomenti passano tutti i dieci nuovi casi scrivibili; un caso iOS scade a5,005 s reali con1,263 s CPU. Le tre ripetizioni isolate passano con lo stesso limite in2,43–3,17 s,10.941 istruzioni e output65. Carico54–70 su16 CPU logiche suggerisce pressione dello scheduler, senza garanzie di latenza; errori iniziali conservati.

Il metodo Python finale invariato ha superato le cinque combinazioni in41,118 s, mantenendo cinque secondi per processo e separati errori/diagnostica precedenti.


Verifica metadati (2026-10-06): Release mirato148=124 riusciti/24 saltati. Darwin completo645=343 riusciti/300 saltati/2 timeout nei test directory ARM64 HVF esistenti. Ripetizione identica20=8 riusciti/12 saltati, casi interessati3.818/3.949s entro il limite originale5s. Tutte75 le identità HVF richieste hanno osservazioni riuscite; il primo fallimento resta conservato. C/CLI/report117/117 con73 Darwin, Python cinque profili27.359s, nativo15/15, runner66/66 riusciti. Allocazione virtuale, non prova APFS. Nessun limite cambiato; CI completa, Intel, iOS fisico e ambiente completo restano aperti.

`build-hvf-arm64/mutation-metadata-validation-summary.json`; `mutation-metadata-darwin-evidence/`; `mutation-metadata-directory-recheck/`; `mutation-metadata-focused.xml`; `mutation-metadata-public.xml`.

Verifica del posizionamento sparse (2026-10-06): Release Darwin, 671 casi, 359 superati, 312 saltati per backend indisponibili, nessun errore. Eseguiti tutti i 78 casi ARM64 HVF obbligatori; Unicorn copre cinque profili. Test mirati: 123 superati su 147, 24 saltati. Superati 16 programmi nativi, 122 controlli C/CLI/report (78 confronti Darwin), cinque profili Python (12.344 s) e 66 test del runner. Conteggi sovrapposti, scadenze invariate ed errori precedenti conservati. Evidenze: `build-hvf-arm64/sparse-seek-validation-summary.json`. La geometria segue una politica virtuale esplicita, non equivale ad APFS. CI completa e iOS fisico restano da verificare; Intel HVF Actions resta sospeso.

Verifica unlink (2026-10-06): Release Darwin708 casi,384 superati,324 saltati per backend indisponibili, nessun errore;81 ARM64 HVF obbligatori eseguiti. Mirati156:137 superati/19 saltati. Nativi17/17, C/CLI/report128/128 (Darwin83), Python cinque profili16.268s, runner66/66 superati. Revisione indipendente senza blocchi residui. Conteggi sovrapposti, scadenze invariate, nessuna ripetizione necessaria. Evidenze: `build-hvf-arm64/unlink-validation-summary.json`. Invalidazione/tempi fissi sono regole del modello; file system/runtime completo e iOS fisico restano da verificare. Intel HVF Actions sospeso; CI completa separata.

### Verifica creazione, 2026-10-06

Release Darwin:748 casi,412 passati,336 saltati per backend assenti, nessun errore;84 obbligatori ARM64 HVF eseguiti. Mirati162:150 passati/12 saltati. C/CLI/report133/133 (Darwin88), Python5 profili9.982s, nativi18/18, runner66/66 passati. ARM64 inizialmente rifiutava correttamente i rebase della tabella di puntatori del test; byte inline li eliminano senza ampliare il loader. Fallimenti/binari iniziali conservati; inventario atteso27→28. Revisione indipendente senza blocchi, compresa conservazione del genitore dopo budget insufficiente. Conteggi sovrapposti, limiti temporali invariati. CI completa/iOS fisico separati; Actions Intel HVF sospeso.

`build-hvf-arm64/create-validation-summary.json`, `create-darwin-evidence/`, `create-focused.xml`, `create-public.xml`, `create-native-final/`, `create-initial-evidence/`.

### Verifica dei metadati di creazione, 2026-10-06

Release Darwin: 787 registrazioni, 439 superate, 348 saltate per backend indisponibili, zero errori; tutti gli 87 ARM64 HVF obbligatori eseguiti. Mirati: 139/151 superati, 12 saltati. C/CLI/report: 145/145, inclusi 98 confronti input Darwin; metodo Python invariato, cinque profili in 12.211 secondi. Nativi 19/19 e verificatori 66/66 superati. Revisione indipendente senza blocchi; nuovi casi per device/GID di genitori distinti e inode globale, unlink prima della scrittura, umask senza FD libero/input utilizzabile. Conteggi sovrapposti, scadenze invariate, nessuna ripetizione per errore. Tempi fissi di creazione/modifica e allocazione restano politiche virtuali. GitHub CI completa e iOS fisico separati; Intel HVF Actions sospeso.

`build-hvf-arm64/creation-metadata-validation-summary.json`, `creation-metadata-darwin-evidence/`, `creation-metadata-focused.xml`, `creation-metadata-public.xml`, `creation-metadata-native/`.

### Verifica della rinomina, 2026-10-06

Release Darwin: 835 registrazioni,474 superate,360 saltate e un timeout preesistente dei metadati virtuali macOS ARM 64 HVF (5.087 s). Ricontrollo con stessi argomenti e limite 5 s: 8 superate,12 saltate; identità interessata 0.113 s. Tutte 90 identità ARM 64 HVF obbligatorie hanno osservazioni riuscite tra i due avvii; il gate completo resta registrato come fallito. Mirati 42/54 superati,12 saltati; C/CLI/report 150/150, inclusi 103 confronti Darwin; Python invariato, cinque profili 18.478 s; nativi 20/20, script 66/66. Revisione indipendente ha corretto punti annidati: regressione 4 K/16 K fallita prima e riuscita dopo. Precedente aspettativa ftruncate sola lettura corretta a EINVAL. Errori e versioni delle sonde conservati, conteggi sovrapposti, scadenze invariate. GitHub CI completa e iOS fisico separati; Intel HVF Actions sospeso.

`build-hvf-arm64/rename-validation-summary.json`, `rename-focused-final.xml`, `rename-darwin-final-evidence/`, `rename-metadata-recheck.xml`, `rename-public.xml`, `rename-native-final/`, `rename-review-initial-evidence/`.

## Osservazioni esplicite del sistema

`ProcessOptions::DarwinSystem` / `darwin_system` fornisce osservazioni fisse a `sysctl(202)` e al `sysctlbyname(274)` diretto in ogni profilo Darwin. Ogni campo è facoltativo; valori assenti o chiavi non elencate restano non supportati. Nessuna query all’host o versione/modello implicito. La validazione rigorosa JSON e C++ rifiuta valori errati e profili non Darwin prima del caricamento.

`os_revision` è a 32 bit con segno; `cpu_count` va da 1 a INT32_MAX; `memory_size` conserva tutti i 64 bit senza segno. Gli altri campi sono stringhe fino a 1023 byte senza NUL interno; la stringa vuota esplicita è valida. L’output include il NUL finale. Le informazioni su CPU e memoria non modificano scheduling o budget di allocazione.

| Campo JSON | Nome sysctl | MIB |
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

`hw.pagesize` deriva dalla politica di memoria guest esistente: normalmente otto byte, quattro con output non nullo e capacità esattamente quattro. Il MIB storico `[6,7]` e `hw.pagesize_compat` restituiscono sempre quattro byte. L’OID numerico dinamico di `hw.pagesize` non è supportato. Anche `hw.memsize` si restringe con capacità quattro solo se il suo schema a 64 bit è l’estensione del segno di un intero a 32 bit; altrimenti ERANGE34 preserva output e lunghezza.

Il conteggio MIB usa i 32 bit inferiori e deve essere 2–12; la lunghezza del nome usa 64 bit e deve essere inferiore a 1024. Tutti i byte sono verificati prima di interpretare il primo NUL e rimuovere un punto finale. Il nome vuoto restituisce ENOENT; l’input parzialmente leggibile resta non supportato. `oldlenp` non nullo richiede otto byte interamente leggibili e scrivibili prima degli effetti. Le prove native con puntatori di lunghezza errati non sono tornate entro il limite: restano esplicitamente fuori ambito. `oldlenp` nullo significa capacità zero; `oldp` nullo richiede solo la dimensione. Un buffer corto restituisce ENOMEM12, lascia i dati intatti e scrive lunghezza zero. EFAULT sui dati conserva la lunghezza precedente. Input e capacità sono acquisiti prima dei dati e la lunghezza è copiata per ultima, preservando alias e copie già completate in caso di successivo errore di trasporto.

Solo `newp` e `newlen` entrambi non zero costituiscono una scrittura. I nodi selezionati restituiscono EPERM1 per l’identità fissa non root prima dei controlli su valore e output, incluso `kern.osversion`, scrivibile nativamente con privilegi. Lunghezza nuova zero ignora il puntatore. Nessun ENOENT viene inventato per chiavi, alberi o OID dinamici sconosciuti.

Il programma originale `system-info` verifica ABI nativa macOS e guest; `virtual-system` confronta byte configurati tramite C++, C/CLI e Python. Un oracolo SDK acquisisce nove osservazioni host come input espliciti del test e confronta output per nome e numero. Non certifica iOS fisico o Intel HVF.

```json
{"darwin_system":{"os_type":"Darwin","os_release":"24.test","os_revision":0,"kernel_version":"Virtual kernel","os_version":"V42","machine":"virtual64","model":"VirtualModel","cpu_count":4,"memory_size":"17179869184"}}
```

[XNU sysctl](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_newsysctl.c), [XNU hardware MIB](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mib.c), [Apple sysctl(3)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man 3/sysctl.3.html).

### Verifica delle query di sistema, 2026-10-06

Release Darwin: 881 registrazioni, 509 superate, 372 saltate per backend indisponibile, nessun errore; eseguite tutte le 93 identità ARM64 HVF obbligatorie. Mirate: 37/49 superate, 12 saltate. C/CLI/report: 163/163, inclusi 113 confronti Darwin. Metodo Python invariato: cinque profili in 15.302 s; programmi nativi 21/21 e script 66/66. Revisione indipendente senza blocchi; priorità aggiuntive e oracolo SDK superati. Una compilazione del nuovo oracolo è fallita per StringExtras mancante, poi è riuscita aggiungendolo; codice e log preservati. Le sonde native con lunghezza non valida restano conservate e fuori contratto. Dopo i test sono stati sistemati solo due commenti iniziali, con ricompilazione riuscita. Conteggi sovrapposti, limiti invariati, nessuna ripetizione per errori runtime necessaria. GitHub CI completa e iOS fisico separati; Intel HVF Actions sospeso.

`build-hvf-arm64/sysctl-validation-summary.json`, `sysctl-darwin-final-evidence/`, `sysctl-focused-final.xml`, `sysctl-public.xml`, `sysctl-native-final/`, `sysctl-sdk-build-failure/`, `sysctl-initial-probe-evidence/`.

## I/O vettoriale dei file e cattura

`readv`/`writev`, `preadv`/`pwritev` e nocancel condividono la logica scalare di file e cattura, senza nuove opzioni o accessi host. Ogni iovec LP64 contiene indirizzo e lunghezza di otto byte. I32 bit bassi con segno di iovcnt devono essere1–1024. L’intero array viene copiato prima della ricerca del descrittore, quindi gli alias di output non cambiano la richiesta. Un array parzialmente leggibile resta non supportato.

Permessi e posizionabilità dello stream precedono le lunghezze. Ogni valore e la somma devono rientrare in INT64_MAX; file e directory richiedono inoltre una somma <= INT_MAX. Lo stdin finito si limita ai byte disponibili e la cattura mantiene il proprio budget. pwritev rifiuta ogni offset negativo prima dell’array; preadv lo verifica dopo descrittore e lunghezze. Gli elementi vuoti ignorano l’indirizzo, mantenendo i controlli di descrittore, tipo e offset. Dopo EOF non si accede alla coda. Le chiamate posizionate conservano il cursore e pwritev ignora append. L’append ordinario riduce l’intera richiesta una sola volta secondo il cursore iniziale, poi sceglie EOF.

Un elemento successivo totalmente invalido restituisce EFAULT mantenendo byte precedenti, avanzamento del cursore normale e FWASWRITTEN dopo aver scritto almeno un byte. Una scrittura non vuota ammessa che restituisce EFAULT per un buffer dati invalida i metadati completi; errori di argomento, rifiuti del modello e guasti backend li conservano. Una destinazione di lettura parzialmente scrivibile arresta con UnsupportedService senza copiare quell’elemento, conservando le copie precedenti. Una sorgente file parzialmente leggibile viene rifiutata prima di ogni effetto sul file. Autorizzazione, lease dei mapping e budget totale precedono la scrittura; errori di verifica o lettura del backend non pubblicano byte di file o cattura.

La cattura verifica prima il budget comune stdout/stderr. Un elemento che oltrepassa il limite degli indirizzi utente non contribuisce byte; quelli precedenti restano. Altri prefissi leggibili vengono catturati con EFAULT. La priorità scalare dell’errore di intervallo sul budget resta invariata. Descrittori duplicati o reindirizzati mantengono la destinazione. L’originale `vectored-io` verifica otto ingressi su macOS nativo, cinque combinazioni guest e C/CLI/Python. Non aggiunge cancellazione, pipe, thread o accettazione iOS fisica.

`readv`: 120/411; `writev`: 121/412; `preadv`: 540/542; `pwritev`: 541/543.

[XNU vector calls](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU iovec lengths](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_subr.c), [XNU vnode I/O](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

### Verifica I/O vettoriale, 2026-10-06

Release Darwin:937 registrazioni,553 superate,384 saltate per backend indisponibile, zero errori; tutte96 le identità ARM64 HVF richieste eseguite. Mirati45/57 superati,12 saltati. C/CLI/report168/168, inclusi118 confronti Darwin; Python copre cinque combinazioni in 20.397s. Nativi22/22, script66/66. La revisione indipendente ha aggiunto un errore di scrittura posizionata sparsa, verificando cursore, EOF reale, rifiuto dei metadati e capacità residua esatta. La prima compilazione usava ancora una query interna rimossa in un vecchio test, ora sostituita dal controllo dell’output reale. Un errore optional<bool> nella nuova asserzione degli eventi ha segnalato otto esecuzioni guest riuscite come fallite; dopo la correzione tutti i controlli interessati sono passati. Fonti e log di entrambi gli errori conservati. Conteggi sovrapposti, scadenze immutate. GitHub CI completa e iOS fisico separati; Intel HVF Actions sospeso.

`build-hvf-arm64/vector-validation-summary.json`, `vector-darwin-final-evidence/`, `vector-focused-final.xml`, `vector-public.xml`, `vector-native-initial/`, `vector-initial-build-failure/`, `vector-initial-assertion-evidence/`.

## Query di esistenza dei file

`access(33)` e `faccessat(466)` interrogano il catalogo virtuale corrente senza allocare descrittori o cambiare contenuto, cursori, flag e metadati. F_OK conferma il nome secondo il contratto di percorso esistente. I metadati non concedono o revocano accesso al catalogo; non vengono verificati i permessi nativi di ricerca degli antenati, ACL o MAC. Osservazioni stat mancanti o invalidate non impediscono la query. I nomi rimossi restituiscono ENOENT anche se vecchi FD o mapping mantengono l’oggetto; creazione, riuso e rinomina seguono lo spazio corrente.

Il modo usa i 32 bit bassi. R/W/X occupa i bit 0–2, i diritti estesi 9–21. `(mode & 0x003ffe07) == 0` indica esistenza; gli altri bit, incluso il segno, vengono ignorati senza EINVAL. Le richieste di permessi restano UnsupportedService dopo una ricerca riuscita, senza dedurli da metadati o concessioni di modifica. Gli errori noti di percorso/descrittore vengono prima.

Faccessat accetta qualsiasi combinazione dei flag bassi AT_EACCESS(0x10), AT_SYMLINK_NOFOLLOW(0x20), AT_SYMLINK_NOFOLLOW_ANY(0x800). Altri flag danno EINVAL prima di percorso o FD, anche senza catalogo. Il catalogo ammesso non contiene link simbolici e le identità reale/effettiva sono fisse. I percorsi assoluti ignorano dirfd; quelli relativi mantengono le regole CWD/FD di directory. La copia fino al primo NUL precede il controllo del FD relativo; un byte mancante dà EFAULT. Anche il percorso relativo vuoto verifica il FD: sconosciuto EBADF, file normale ENOTDIR, altrimenti ENOENT. Catalogo assente e identità directory dello stream sconosciuta restano non supportati.

L’originale `file-access` confronta entrambe le chiamate, bit ignorati, flag e ordine su macOS nativo e cinque guest tramite C++/C/CLI/Python. NOFOLLOW_ANY usa un FD relativo per evitare i link host `/tmp` o `/var`. Test diretti coprono nomi vivi, bit misti, FD esauriti, indipendenza dei metadati ed errori di memoria.

[XNU access and faccessat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU access mode bits](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/unistd.h).

### Verifica dell’esistenza dei file, 2026-10-06

Release Darwin:971 registrazioni,575 superate,396 saltate per backend indisponibile, zero errori; tutte99 le identità ARM64 HVF richieste eseguite. Mirati23/35 superati,12 saltati, inclusi14 diretti. C/CLI/report173/173, inclusi123 confronti Darwin. Python ha verificato cinque combinazioni in 16.235s; nativi23/23 e script66/66. Revisione indipendente di progetto e codice senza blocchi. Conservati il risultato NOFOLLOW_ANY iniziale dovuto al link host /tmp e il confronto con percorso canonico; il programma comune usa un FD di directory relativo. Conteggi sovrapposti, scadenze immutate, nessun errore di esecuzione da riprovare. GitHub CI completa e iOS fisico separati; Intel HVF Actions sospeso.

`build-hvf-arm64/access-validation-summary.json`, `access-darwin-final-evidence/`, `access-focused-final.xml`, `access-public.xml`, `access-native-initial/`, `access-evidence/`.
