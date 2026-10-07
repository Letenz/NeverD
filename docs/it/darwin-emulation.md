**Lingue**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: dcf1c6fd8dade7571d6c20c70f66ce44bd254c717076498f8a48e46deec70a62 -->

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

Servizi: `exit`, `write`, `getpid`, `getppid`, `getuid`, `geteuid`, `getgid`, `getegid`, `getgroups`, `mmap`, `mprotect`, `munmap`. PID vale1000 e PPID1; UID/GID sono1000 per default o gli ID reali/effettivi distinti dichiarati sotto. I descrittori 1 e 2 catturano byte, inclusi NUL e non UTF8; quelli chiusi o di sola lettura restituiscono EBADF. Una copia parziale conserva i byte già letti ma il guasto successivo rimane EFAULT. Una lunghezza oltre `INT_MAX` produce EINVAL prima di controllare descrittore, puntatore o budget: [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).

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

Programmi originali normali/nocancel confrontano il kernel nativo; test 4K/16K e C/CLI/Python coprono cinque combinazioni. Controllo dei permessi, rimozione di directory, rinomina tra domini di directory iniziali distinti, hard link, metadati del file system nativo, coerenza delle mappe e SIGBUS EOF restano incompleti. Ambiente completo, iOS fisico e Intel HVF non sono validati; Actions Intel resta sospeso.

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

`unlink(10)` / `unlinkat(472)` rimuovono nomi regolari esistenti. la rimozione dei file accetta solo i32 bit bassi 0 o `0x800`; bit sconosciuti danno EINVAL prima di percorso/FD, AT_REMOVEDIR usa il contratto limitato sotto; DATALESS e SYSTEM_DISCARDED restano esclusi. Risoluzione comune: ENOENT, ENOTDIR per file seguito da `/`, EPERM per directory ordinaria, EISDIR per radice composta solo da barre, EBUSY con `.`/`..` finali. I suffissi `.`/`..` sono verificati nativamente.

FD/dup/aperture indipendenti esistenti mantengono dati, cursori e flags; F_GETPATH conserva il vecchio percorso acquisito. Le nuove aperture falliscono, genitori impliciti e CWD restano. Il permesso di scrittura appartiene all’oggetto; close/dup2/modifica successiva recuperano i byte correnti solo dopo l’ultimo descrittore e mapping. I costi iniziali dei percorsi restano; controllo dei permessi, rinomina tra domini di directory iniziali distinti e hard link sono ancora incompleti; le directory iniziali usano l’autorizzazione esplicita descritta sotto.

stat/readdir/SEEK_END del padre diventano sconosciuti per ogni FD/percorso e si fermano prima di copia/cursore. read/pread restano EISDIR; SET/CUR/F_GETPATH/fchdir/risoluzione relativa continuano. La politica nota imposta nlink=0 e ctime fisso; scritture successive non ripristinano nlink=1. Senza politica/dopo EFAULT i metadati restano ignoti. Errori preservano lo stato. `unlinked-file` confronta regole native nome/FD; tempi e invalidazione sono regole esplicite del modello.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"00"}],"directories":[{"path":"/work","mutable":true}]}}
```

[XNU unlink / unlinkat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [F_GETPATH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c).

## Creazione di file regolari

O_CREAT=0x200 crea un file vuoto nel genitore diretto esplicitamente mutable, tramite open/openat normale o nocancel. Il nuovo oggetto è scrivibile; quelli esistenti mantengono WritableFiles. Un FD di sola lettura può creare ma non scrivere. Senza politica di creazione esplicita, stat64 e ricerca sparsa restano sconosciuti. metadata/mutation_policy del vecchio omonimo non vengono mai ereditati.

O_EXCL=0x800 con O_CREAT restituisce EEXIST per file/directory esistenti prima del troncamento; da solo non ha effetto. O_CREAT di sola lettura apre directory esistenti. Ordine: accesso invalido, disponibilità FD, EINVAL per O_CREAT|O_DIRECTORY, percorso. Si crea solo l’ultimo componente originale mancante; antenati assenti e suffissi `/`, `//`, `/.`, `/..` danno ENOENT. Nuovo O_CREAT|O_TRUNC non imposta FWASWRITTEN, mentre il troncamento esistente sì.

Solo l’inserimento invalida le osservazioni del genitore. Vecchi/nuovi omonimi mantengono dati, FD, metadati e mapping indipendenti. Le 256 voci includono elementi iniziali non file e oggetti vivi; percorsi canonici/NUL dinamici e byte correnti contano nei 16 MiB. Dopo unlink, l’ultimo FD/mapping libera i costi dinamici; quelli iniziali restano. Budget esaurito o percorso canonico di almeno 1024 byte arrestano esplicitamente senza inventare ENOSPC o errno nativo né pubblicare nome/FD. created-file confronta macOS nativo e cinque profili; test 4K/16K verificano i limiti. Controllo dei permessi, rinomina tra domini di directory iniziali distinti, link e mutazione directory restano aperti.

[XNU open](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

## Metadati di creazione espliciti e umask del processo

L’opzione `darwin_files.umask` (C++ `InitialUmask`) dichiara la maschera iniziale da 0 a 07777 ottale, indipendentemente dal permesso di creazione. `umask(60)` restituisce la precedente e salva i bit bassi 07777, senza memoria guest o FD libero. L’omissione significa sconosciuta, senza dedurre valori host o predefiniti. Si inizializza una volta e le modifiche riguardano solo le creazioni future, senza cambiare l’input. L’esempio usa 18 decimale, cioè 0022 ottale.

L’opzione `darwin_files.creation_policy` (C++ `CreationPolicy`) fornisce metadati completi ai nuovi oggetti. L’oggetto rigoroso contiene esattamente `first_inode`, `block_size`, `generation`, `creation_time`, `mutation_policy`; tempi e politica di modifica usano i formati esistenti. Richiede umask esplicita, almeno un genitore mutable e metadata completi per ogni genitore autorizzato. block_size è 1..INT32_MAX, generation è uint32; l’unità di allocazione è una potenza di due tra 512 e 16 MiB, indipendente dal blocco/pagina VM, e i nanosecondi sono in [0,1000000000). first_inode è uint64 positivo maggiore di tutti gli inode stat/snapshot, anche di altri dispositivi. Stringhe decimali preservano gli interi oltre l’intervallo esatto JSON.

Solo l’inserimento riuscito di un nuovo oggetto consuma la sequenza globale inode. UINT64_MAX la esaurisce definitivamente; close/unlink/riuso del nome/umask/ricerche non la reinizializzano. Rifiuti per esclusività, FD, percorso, voci o byte non pubblicano nome/FD né incrementano il contatore; O_CREAT esistente non consuma nulla. Il nuovo stat64 eredita device/GID dal genitore diretto, UID effettivo guest selezionato (default1000), mode `S_IFREG | (mode & 0777 & ~umask)`, nlink=1 e size/blocks/flags=0. Blocco, generation e quattro tempi iniziali fissi provengono dalla politica. Dopo l’invalidazione di stat/enumerazione completi del genitore, device/GID restano utilizzabili senza ripristinare l’intero record.

Ogni nodo possiede metadati/allocazione propri, senza ereditare il vecchio omonimo. write/truncate/unlink condividono la politica e preservano inode/mode/birthtime e nlink=0 dopo unlink; EFAULT completo lascia lo stato permanentemente sconosciuto. Nessun effetto retroattivo sui nodi esistenti. `created-file-metadata` confronta permessi, vecchia maschera, UID effettivo, dispositivo/gruppo genitore e durata nativa in cinque profili; `virtual-created-metadata` confronta separatamente tutti i 144 byte. I quattro tempi nativi possono differire. Tempo fisso/allocazione sparsa sono regole virtuali; controllo permessi, cambio credenziali, ACL e APFS nativo restano da implementare.

```json
{"darwin_files":{"files":[],"umask":18}}
```

[XNU creation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU umask](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c). [XNU rename / renameat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Scambio atomico di file e directory create

RENAME_SWAP=0x2 scambia tramite renameatx_np due file regolari esistenti, due directory vive create dal processo, oppure un file e una di queste directory; RENAME_NOFOLLOW_ANY è facoltativo. La directory iniziale esplicita dichiara mutable:true e swap_rename:true; C++ usa DarwinFileOptions::SwapRenameDirectories. I discendenti ereditano la capacità dell’oggetto originale; riutilizzare nomi eliminati non trasferisce dichiarazioni. false o omissione significa ignoto. Device uguali e diritti di modifica non provano il supporto; domini iniziali distinti restano esclusi.

`openat_nocancel`, `fstatat64` e `F_GETPATH=50` usano lo stesso componente file. Dopo lo scambio, percorsi e osservazioni restano dei rispettivi oggetti; la disponibilità dello stat completo segue il contratto dei metadati.

Una destinazione assente, anche con slash finale, dà ENOENT prima di punto/doppio punto sorgente, dominio, autorità o capacità. Operandi directory iniziali o rimossi restano esclusi. Nel dominio ammesso, entrambi gli ordini antenato/discendente e directory/file figlio danno EINVAL; una destinazione file con slash finale dà ENOTDIR. Lo stesso oggetto con componente ordinario non cambia dopo l’autorizzazione, anche senza capacità dichiarata. Il punto sorgente dello stesso oggetto richiede ancora la sensibilità alle maiuscole ignota. RENAME_EXCL=0x4 + RENAME_SWAP=0x2 e flags ignoti danno EINVAL prima dei percorsi; SECLUDE resta escluso.

Entrambi i sottoalberi non vuoti seguono oggetti genitore, inclusi directory rimosse e file orfani trattenuti da FD/mapping. Lo scambio misto muove solo l’esatta radice file; un vecchio orfano omonimo conserva il proprio genitore. FD, dup, CWD e doppio punto seguono oggetti e nuovi genitori. Byte, identità, metadati, diritti, cursori, flags e lease dei file discendenti restano intatti. Radici mosse e genitori immediati applicano le regole namespace esistenti; una policy configurata aggiorna il ctime proprio del file radice, senza policy i metadati completi restano ignoti.

Ogni riferimento riserva percorso+NUL nei 16 MiB iniziali fissi. Tutti i percorsi collegati/trattenuti in entrambe le direzioni sono verificati sotto 1024 byte e il budget condiviso prima di ritirare insieme i vecchi nomi e pubblicare. Nessuna radice è eliminata e non c’è credito di sostituzione, neppure dai byte di una destinazione non aperta. Il primo spostamento di un file iniziale acquisisce costo dinamico; il ritorno non lo azzera e la ripetizione non lo accumula. Nessun nuovo FD, elemento o inode di creazione. Il rifiuto conserva entrambi i namespace, genitori, cursori, osservazioni e mapping. L’originale SDK-free swapped-directory confronta directory e entrambi gli ordini misti su macOS nativo e cinque profili C++/C/CLI/Python.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## Istantanee esplicite delle directory

`getdirentries64` (344) enumera il `contents` immutabile facoltativo di una voce `directories` esistente; C++ usa `DarwinFileOptions::DirectoryContents`. `entries` include in ordine esplicito tutti i figli diretti, `.` e `..`. Senza istantanea anche una directory vuota resta sconosciuta. Non crea percorsi o stat e non consulta l’host.

Ogni voce richiede `name`, `inode` non nullo, `type` (0 sconosciuto, 4 directory, 8 file), `next_offset` e `seek_offset`. Tipo e percorso concordano; gli inode dello stesso percorso risolto concordano tra istantanee e metadati. `next_offset` è positivo, unico nella directory e <=INT64_MAX, senza obbligo di crescere; zero riavvolge. `seek_offset` è l’osservazione d_seekoff distinta, a 64 bit senza segno; sono ammessi zeri ripetuti. Gli interi usano le stringhe decimali senza perdita di stat.

`contents.minimum_buffer_size` richiede un minimo di payload di 1–128 MiB, EOF incluso. Il `minimum_buffer_size` facoltativo della voce (predefinito 0) vincola la lettura che parte lì. L’esempio osserva APFS: 64 byte per i due punti iniziali, 1 a EOF; altrove deve entrare un record intero. LP64 allinea a otto byte, dimensione `roundUp(25 + nameBytes, 8)`. Massimo 4096 voci complessive; i byte dei record contano nei 16 MiB. Gli antenati dichiarati solo da metadati/istantanea contano una volta nei 256 percorsi. JSON resta limitato a 64 KiB.

Open indipendenti hanno cursori separati, dup li condivide. La ripresa accetta solo zero o cookie forniti; posizioni sconosciute arrestano esplicitamente. Ogni chiamata restituisce il massimo prefisso di record interi. Lunghezza >=1024 riserva gli ultimi quattro byte richiesti a EOF (1 alla fine, altrimenti 0); solo il payload è limitato a 128 MiB. L’indirizzo conserva l’aritmetica originale senza segno, overflow compreso. Ordine: dati, avanzamento, posizione precedente, flag. EFAULT successivi mantengono gli effetti precedenti; EOF omette la copia vuota. Una singola copia parzialmente scrivibile si arresta prima di quella copia, conservando gli effetti anteriori.

`directory-entries` confronta campi, dup/riavvolgimento, letture piccole, EOF e ordine delle copie con macOS. Un test separato confronta tutti i byte nativi catturati, nomi lunghi compresi, con il layout SDK. I cookie fissi non riproducono le generazioni dinamiche APFS. Il vecchio `getdirentries` (196), enumerazione dopo modifiche, altri backend nativi e iOS fisico restano fuori da questa verifica.

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

La verifica autonoma richiede tutti i 111 casi nativi ARM64 o 74 x64, compresi `LC_MAIN` e `LC_UNIXTHREAD` su ogni piattaforma. Casi obbligatori mancanti/saltati o assenza di `ld64.lld` causano errore.

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

newp/newlen non nulli significano scrittura. Prima restano lettura nome/MIB e verifica completa lettura/scrittura oldlenp. EUID default o esplicito non-root dà EPERM1 prima di osservazione/output dati. EUID0 ferma kern.osversion unsupported perché la scrittura privilegiata non è modellata; RUID non decide. Altri nodi nativi sola lettura mantengono EPERM1 anche root. Nuova lunghezza0 ignora puntatore; nessun ENOENT inventato per chiavi/alberi/OID dinamici sconosciuti.

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

## Creare e rimuovere directory

`mkdir(136)` e `mkdirat(475)` creano in un genitore diretto esplicitamente modificabile. Le nuove directory ereditano l’autorità sui nomi; quelle iniziali mantengono autorizzazioni proprie. Ereditano soltanto device/GID noti, mai stat completo, dimensione, allocazione, tempi o cookie. Una nuova directory nasconde tutte le osservazioni del precedente file omonimo. `creation_policy` resta per file regolari: i discendenti usano l’identità ereditata e la sequenza globale inode; mkdir non consuma inode di file. Controllo dei permessi e metadati nativi delle directory restano esclusi.

Il percorso comune consente a mkdir un ultimo nome assente seguito solo da barre. Antenato mancante prima di punto/doppio punto dà ENOENT, file antenato ENOTDIR, nome esistente EEXIST. Restano FD/CWD relativi, indipendenza dal FD per percorsi assoluti e priorità degli errori di stringa. Non serve un FD libero; rifiuti di ricerca, autorità, budget o trasporto non pubblicano cambiamenti.

`rmdir(137)` e `unlinkat(472)` con AT_REMOVEDIR(0x80), anche AT_SYMLINK_NOFOLLOW_ANY(0x800), rimuovono directory vuote create dal processo. Bit bassi32 sconosciuti danno EINVAL prima degli input; DATALESS e SYSTEM_DISCARDED restano esclusi. Restano errori noti di percorso/tipo/radice; rimuovere directory iniziali senza autorizzazione removable resta UnsupportedService. Sulle directory ammesse, punto finale dà EINVAL, doppio punto da directory collegata o destinazione non vuota ENOTEMPTY. FD di directory, dup e CWD mantengono l’oggetto originale e non impediscono più la rimozione. File regolari già unlink e mapping non contano come nomi: confronti nativi mantengono byte, inode e ultimo F_GETPATH dopo rimozione/riuso del genitore.

Ogni percorso canonico+NUL e una voce usano il budget comune16 MiB/256 voci. La rimozione seguita dal rilascio di tutti i riferimenti rimborsa solo questo costo, mantenendo file orfani/mapping. Solo il successo invalida stat/enumerazione del genitore; osservazioni complete delle nuove directory restano sconosciute. L’originale `directory-mutations` confronta creazione annidata, rinomina, unlink, rimozione e riuso su macOS nativo e cinque guest tramite C++/C/CLI/Python. Un file orfano mantenuto da una descrizione o lease di mapping trattiene anche le directory genitrici e i costi correnti percorso/NUL e voce, anche dopo la chiusura dei loro FD. Il recupero del file precede la catena delle directory. Spostare un antenato creato vivo aggiorna F_GETPATH; il riuso del nome non assegna i vecchi oggetti al sostituto.

[XNU mkdir/rmdir](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU directory creation lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_lookup.c).

### Verifica delle modifiche alle directory, 2026-10-06

Release Darwin:1.017 registrazioni,609 superate,408 saltate per backend indisponibile,zero errori;102 ARM64 HVF obbligatori eseguiti. Mirati58 superati,12 saltati,inclusi26 nuovi diretti4K/16K. C/CLI/report178/178,inclusi128 Darwin;Python cinque combinazioni in 17.255s,nativi24/24,script66/66. Revisione indipendente di budget,riuso nomi,identità genitore,lease e rollback. Una sonda nativa dopo il primo successo ha rilevato EISDIR per radice solo di barre ed EBUSY con punto/doppio punto finale;decisione comune e test nativo/guest corretti. Risultati iniziali e copie sorgente/binarie conservati. Conteggi sovrapposti,scadenze immutate,Intel HVF Actions sospeso;GitHub CI completa e iOS fisico separati.

`build-hvf-arm64/directory-mutation-validation-summary.json`, `directory-mutation-darwin-final-evidence/`, `directory-mutation-focused-final.xml`, `directory-mutation-public-final.xml`, `directory-mutation-native-final/`, `directory-mutation-before-root-fix/`.

## Identità delle directory trattenute

FD/CWD mantengono l’oggetto eliminato e la catena originale dei genitori anche con nomi riutilizzati. Open del punto ha cursore indipendente,dup condiviso;doppio punto segue il genitore originale. I figli ordinari di directory eliminata danno ENOENT. LOOKUP attraversa genitori eliminati trattenuti,mentre creazione/rimozione/rinomina danno ENOENT. Punto/doppio punto finale della rinomina dà EINVAL prima di quel componente,dopo errori precedenti. F_GETPATH conserva l’ultimo percorso;stat/enumerazione completi restano ignoti. Percorso+NUL e una voce restano conteggiati fino all’ultimo FD/CWD/figlio trattenuto. Close/dup2/cambio CWD/ammissione modifica recuperano catene irraggiungibili;costi iniziali e lease dei file separati. `deleted-directories` confronta nativo e cinque guest. Un file orfano mantenuto da una descrizione o lease di mapping trattiene anche le directory genitrici e i costi correnti percorso/NUL e voce, anche dopo la chiusura dei loro FD. Il recupero del file precede la catena delle directory. Spostare un antenato creato vivo aggiorna F_GETPATH; il riuso del nome non assegna i vecchi oggetti al sostituto.

### Verifica della durata delle directory, 2026-10-06

Release Darwin1.051 casi,631 superati,420 indisponibili saltati,zero errori;105 ARM64 HVF obbligatori eseguiti. Mirati98 superati/12 saltati,diretti iniziali64/64 con14 nuovi. C/CLI/report183/183,133 Darwin;Python cinque combinazioni 18.691s,nativi25/25,script66/66. Sonda nativa aggiuntiva corregge l’ordine del punto finale di rinomina;prime fonti/risultati/copie conservate. Agente principale ha verificato le prove;revisione indipendente finale indisponibile. Conteggi sovrapposti,scadenze immutate,CI completa/iOS fisico separati,Intel HVF Actions sospeso.

`build-hvf-arm64/directory-lifetime-validation-summary.json`, `directory-lifetime-darwin-final-evidence/`, `directory-lifetime-focused-final.xml`, `directory-lifetime-public.xml`, `directory-lifetime-native/`, `directory-lifetime-before-rename-fix/`.

## Rimuovere directory iniziali ammesse esplicitamente

Il Boolean rigoroso `"removable": true` di una voce directory (C++ `DarwinFileOptions::RemovableDirectories`) dichiara una directory ordinaria, non un mount, con una sola identità nello spazio dei nomi. Deve essere una voce iniziale esplicita di `directories`, diversa dalla radice, con genitore immediato esplicitamente modificabile. Si rifiutano modi/flag speciali noti, alias inode (incluse le istantanee) e numeri di dispositivo noti incompatibili tra genitore e destinazione. Numeri uguali da soli non dimostrano l’assenza di mount. Omissione/false restano non supportati; altri tipi JSON sono invalidi. Non fornisce un modello generale di permessi o mount.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/empty","removable":true}],"working_directory":"/empty"}}
```

La rimozione richiede uno spazio dei nomi corrente vuoto. Le sottodirectory iniziali implicite restano dopo unlink del loro ultimo file originale. Il successo invalida le osservazioni complete stat/enumerazione dell’oggetto e del genitore immediato; i vecchi FD/dup/CWD mantengono oggetto e catena originale dei genitori. Gli input immutabili non fanno riapparire nomi rimossi. Un nuovo file o directory con lo stesso nome ha identità separata e non recupera vecchi metadati o istantanee. Gli input del chiamante restano invariati.

Ogni riferimento removable aggiunge percorso e NUL al costo iniziale fisso di 16 MiB. Voci, percorsi, riferimenti e istantanee iniziali rimangono conteggiati dopo rimozione e ultima chiusura, incluso il loro posto nel limite di 256 voci. I nuovi oggetti mantengono la propria contabilità dinamica. Il programma originale `initial-directory-removal` rimuove la directory vuota preesistente del test nativo mentre è aperta, riusa il nome per un file e poi una directory, verifica la conservazione tramite il solo CWD e ripristina la directory vuota. Lo stesso programma viene eseguito tramite C++/C/CLI/Python nelle cinque combinazioni guest.

### Verifica della rimozione delle directory iniziali, 2026-10-06

Le sorgenti finali Release hanno riconciliato 1.089 test Darwin: 657 superati, 432 saltati per backend non disponibile, nessun errore; tutti i 108 casi ARM64 HVF obbligatori sono stati eseguiti. I controlli mirati hanno superato 27/39 casi con 12 esclusioni; è passato anche il controllo aggiuntivo degli alias derivati soltanto dagli snapshot. C/CLI/report pubblici: 191/191; Python: cinque combinazioni in 76,276 s; programmi nativi originali: 26/26; esecutori delle evidenze: 66/66. Un input inode/snapshot incoerente nel test è stato corretto, conservando i risultati falliti.

Due esecuzioni complete precedenti avevano uno e tre timeout nei casi esistenti di file/rinomina. Un’esecuzione strumentata ne ha riprodotto uno con 5,008 s reali e 0,171 s di CPU del processo. I confronti dello stesso metodo e dei programmi precedenti sono passati, ma la causa della latenza resta irrisolta; il successo finale non dimostra stabilità dei tempi. La diagnostica temporanea è stata rimossa, gli hash dei programmi ripristinati e il limite originale del guest di 5 s mantenuto. Audit principale di sorgenti/evidenze completato; revisione indipendente non disponibile. I conteggi si sovrappongono. CI GitHub completa, iOS fisico e Actions Intel HVF sospese restano fuori da questa accettazione locale.

`build-hvf-arm64/initial-directory-validation-summary.json`, `initial-directory-darwin-restored-evidence/`, `initial-directory-public.xml`, `initial-directory-native-evidence/`, `initial-directory-timeout-probe/`.

## Rinomina esclusiva dei file regolari

Dopo la risoluzione di origine e destinazione, RENAME_EXCL restituisce EEXIST per un altro file o directory esistente prima dei controlli di mount e mutazione. Restano prioritari gli errori precedenti del percorso, incluso EINVAL per punto/doppio punto finale. Una destinazione assente usa la stessa transazione limitata, conservando descrizioni aperte, cursori, flag, lease dei mapping e transizioni dei metadati configurate. Lo stesso oggetto resta esplicitamente non supportato: il risultato nativo dipende dalla distinzione maiuscole/minuscole del filesystem, non dimostrata dalle chiavi esatte. Normalizzazione del caso, sorgenti directory iniziali e SECLUDE restano esclusi. Il programma originale `renamed-file` confronta rifiuto senza cambiamenti dei metadati e successo EXCL|NOFOLLOW_ANY su macOS nativo e C++/C/CLI/Python.

Verifica, 2026-10-06 (Release): 1.097 test Darwin, 665 superati, 432 saltati per backend indisponibile, nessun errore; eseguiti tutti i 108 casi ARM64 HVF obbligatori. Mirati: 44 superati, 12 saltati, inclusi otto nuovi casi diretti. C/CLI/report: 191/191; Python: cinque combinazioni in 19,241 s; sonda indipendente: 26 controlli superati. Il primo tentativo nativo ha superato il tempo in return; gli altri 25, incluso renamed-file, sono passati. Tre ricontrolli return sullo stesso binario invariato hanno richiesto 0,014–0,034 s, poi tutti i 26 casi sono passati con il limite originale di 5 s. Il primo errore resta conservato e inspiegato; questi risultati e il precedente successo HVF non provano stabilità della latenza. Audit principale completato, revisione indipendente non disponibile. Conteggi sovrapposti; iOS fisico, CI GitHub completa e Actions Intel HVF sospese fuori dall’accettazione locale.

`build-hvf-arm64/exclusive-rename-validation-summary.json`, `exclusive-rename-darwin-evidence/`, `exclusive-rename-public.xml`, `exclusive-rename-native-evidence/`, `exclusive-rename-native-rechecked-summary.json`, `exclusive-rename-probe/`.


## Rinomina tra directory create

Una directory iniziale e tutti i discendenti creati da questo processo con mkdir/mkdirat condividono un dominio di nomi virtuale. `rename`, `renameat` e `renameatx_np` spostano file regolari tra questi genitori senza un nuovo campo JSON. Dopo aver creato `/work/left` e `/work/right` sotto `/work` iniziale modificabile, `/work/data` può passare ai figli e tra essi. Un `/work/left` iniziale dichiarato separatamente resta un altro dominio, anche con lo stesso device. La topologia generale dei mount resta ignota.

Entrambi i genitori diretti richiedono autorizzazione; le directory create la ereditano con device/GID noti. Identità, proprietario/gruppo, diritto di scrittura e allocazione del file restano invariati. Device discordanti vengono rifiutati. Uno spostamento invalida stat/elenco completi di entrambi i genitori. Directory iniziali rimosse e percorsi riutilizzati restano oggetti distinti; vecchi FD/CWD non ricevono il dominio sostitutivo.

Restano transazione limitata, lease dei mapping, costi percorso/NUL e precedenza degli errori. EXCL con destinazione esistente distinta dà EEXIST prima di dominio/autorizzazione. `renamed-file` confronta spostamento nel figlio creato, sostituzione nel genitore iniziale e ritorno via C++/C/CLI/Python e macOS nativo. Permessi, spostamento di directory iniziali, link fisici/simbolici e metadati APFS nativi restano aperti.

### Verifica tra directory padre, 2026-10-06

La verifica Release finale ha riconciliato 1,115 registrazioni Darwin: 683 superate, 432 saltate per backend non disponibile, nessun errore; eseguiti tutti i 108 casi ARM64 HVF obbligatori. I controlli diretti hanno superato 56/56, inclusi 18 nuovi casi 4K/16K. C/CLI/report pubblico: 191/191; il metodo Python ha coperto cinque profili in 22.254s. Programmi nativi originali: 26/26; sonda separata di chiamate grezze: 34 controlli; script di documentazione/capacità/esecuzione prove: 296/296. I conteggi si sovrappongono. Gli hash dei dieci binari di verifica sono rimasti invariati dopo la modifica CMake limitata a MSVC.

Le due verifiche complete precedenti conservano tre e due timeout nel metodo HVF di file esistente. I confronti del metodo completo, della directory di lavoro e della sessione sono passati senza stabilire la causa; il successo finale non dimostra stabilità della latenza. La sonda confrontava inizialmente /tmp con /private/tmp canonico; ricavare il percorso dal FD radice ha corretto quattro aspettative. Il programma nativo esteso usava mkdir(136) per la pulizia; rmdir(137) ha corretto exit150. Sorgenti ed errori iniziali sono conservati e il limite ospite rimane 5s.

Quattro conflitti LP64 nelle liste di inizializzazione rilevati dalla CI Linux completa ora usano valori uint64_t espliciti; NeverDJumpTableTests riceve /bigobj sotto MSVC. La compilazione effettiva Linux/Windows attende la CI. La precedente CI completa segnalava anche errori separati del corpus Windows EH e dell’annullamento di una PR chiusa. Completata l’autorevisione di sorgenti/prove; non si rivendicano revisione indipendente, iOS fisico o verifica Intel HVF sospesa.

`build-hvf-arm64/cross-parent-rename-validation-summary.json`, `cross-parent-rename-darwin-synced-evidence/`, `cross-parent-rename-darwin-evidence/`, `cross-parent-rename-darwin-rechecked-evidence/`, `cross-parent-rename-public-synced.xml`, `cross-parent-rename-python-synced.log`, `cross-parent-rename-native-fixed-evidence/`, `cross-parent-rename-probe/`.

## Scambio atomico di nomi di file regolari

RENAME_SWAP=0x2 scambia i nomi di due file regolari esistenti tramite renameatx_np, facoltativamente con RENAME_NOFOLLOW_ANY. Una directory iniziale esplicita deve dichiarare mutable:true e swap_rename:true; C++ usa DarwinFileOptions::SwapRenameDirectories. I discendenti creati ereditano la capacità dell'oggetto originale. Eliminare e riutilizzare un percorso non trasferisce la vecchia dichiarazione. false o omissione lascia la capacità ignota; device uguali e autorità sul namespace non dimostrano supporto. Domini iniziali distinti restano esclusi.

Entrambi i percorsi usano il resolver esistente. Un target assente dà ENOENT prima di dominio, autorità e capacità. Operandi directory restano esplicitamente non supportati: swap nativo può scambiare file e directory, quindi EISDIR del rename ordinario non si applica. Lo stesso oggetto autorizzato è un no-op anche senza dichiarazione della capacità. RENAME_EXCL=0x4 + RENAME_SWAP=0x2 e flags ignoti danno EINVAL prima dei percorsi; SECLUDE resta aperto.

Entrambi i file restano collegati. Identità, proprietario/gruppo, byte, autorizzazione di scrittura, descrizioni, cursori, flags e lease dei mapping propri restano invariati. Le politiche virtuali configurate aggiornano ciascuna ctime; politiche assenti o invalidate lasciano i metadati completi ignoti. Uno scambio reale invalida metadati ed enumerazione completi di entrambi i genitori. Non consuma inode di creazione, voce o FD; l'input del chiamante resta invariato.

Ogni riferimento di capacità riserva percorso+NUL nel budget iniziale fisso di 16 MiB. La transazione verifica entrambi i costi dinamici completi prima della pubblicazione; byte e lease ancora collegati non offrono credito di sostituzione. Scambi ripetuti riutilizzano tali costi. Il programma originale renamed-file scambia verso un figlio creato e torna, verifica entrambi gli oggetti e continua la sostituzione ordinaria su macOS nativo e tutti i profili C++/C/CLI/Python. Permessi, topologia dei mount, normalizzazione del caso, spostamento di directory iniziali restano lavori separati.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/work","mutable":true,"swap_rename":true}]}}
```

[Apple volume swap capability](https://developer.apple.com/documentation/foundation/urlresourcevalues/volumesupportsswaprenaming?changes=__1_2), [XNU rename](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

### Verifica swap, 2026-10-06

Release Darwin: 1.133 registrazioni, 701 passate, 432 saltate per backend assente, nessun errore; tutti i 108 casi ARM64 HVF obbligatori eseguiti. Controlli diretti rename 68/68, inclusi 18 nuovi casi di opzioni e 4K/16K. C/CLI/report pubblico 192/192; il metodo Python ha coperto cinque profili in 16,153s. Programmi nativi originali 26/26, sonda indipendente di chiamate grezze 45 controlli. Conteggi sovrapposti; limiti guest invariati.

Due test diretti iniziali avevano aspettative errate per autorità di scrittura ignota e metadati ignoti dopo mutazione; sono state corrette solo le aspettative. Un primo filtro JSON selezionava zero test e non conta; successivamente proprietario reale e suite pubblica completa sono passati. Fonti e risultati iniziali restano preservati. La latenza HVF/nativa precedente è ancora inspiegata; il passaggio non prova stabilità. Autorevisione di fonti/evidenze completata; nessuna revisione indipendente, accettazione iOS fisico, CI GitHub completa o Intel HVF sospeso rivendicata.

`build-hvf-arm64/swap-rename-validation-summary.json`, `swap-rename-darwin-evidence/`, `swap-rename-public.xml`, `swap-rename-python.log`, `swap-rename-native-evidence/`, `swap-rename-probe/`.


## Rename di directory create dal processo

rename, renameat e renameatx_np ordinari spostano una directory creata viva e il sottoalbero entro un dominio iniziale, con autorizzazione di entrambi i genitori immediati. Destinazione assente con slash finali ammessa; file regolare: ENOTDIR; directory non vuota: ENOTEMPTY; spostamento in discendente: EINVAL. Il medesimo nome autorizzato non cambia stato. EXCL dà EEXIST per altra destinazione esistente prima di tipo, ciclo, dominio o autorità. Gli errori di ricerca destinazione precedono i punti sorgente. Punto/doppio punto sorgente verso lo stesso oggetto resta UnsupportedService perché il caso del filesystem è ignoto. Sorgenti iniziali/rimosse, destinazioni directory iniziali, domini distinti, link, permessi e SECLUDE restano esclusi.

L’appartenenza segue oggetti genitori: figli nominati, directory rimosse mantenute e orfani FD/mapping aggiornano percorsi. FD, dup, CWD e doppio punto seguono lo stesso oggetto sorgente e il nuovo genitore. La destinazione vuota sostituita conserva vecchio percorso e genitore; figli ordinari ENOENT, punti/CWD mantengono il vecchio oggetto. I suoi orfani non seguono un altro spostamento del sostituto, anche con testo identico.

Byte, identità, metadati, autorità di scrittura, cursori, flag FD e mapping dei figli restano. Sorgente e genitori invalidano osservazioni complete; le directory create conservano autorità ereditata per creazioni/rename ammessi. Nessuna nuova voce, FD o inode. Prima della pubblicazione si preparano tutte le chiavi e i percorsi vivi/mantenuti entro 1024 byte con NUL e 16 MiB. Solo la destinazione senza FD/CWD/discendenti mantenuti finanzia credito, recuperato una volta. Un rifiuto mantiene nomi, genitori e osservazioni; il solo mapping trattiene costi dei genitori rimossi.

L’originale SDK-free `renamed-directory` confronta macOS nativo e cinque guest via C++/C/CLI/Python; 4K/16K verifica riuso, rollback, capacità esatta, discendenti lunghi, crediti, recupero genitori ed esaurimento voce/FD/inode.


### 2026-10-06

Release finale:1,175 registrazioni Darwin,731 superate,444 skip backend indisponibile,zero errori;111 obbligatori ARM64 HVF eseguiti. Mirato32/44 (12 skip,22 nuovi4K/16K),confini finali4/4. C/CLI165/165 e report32/32 senza skip;Python cinque profili21.759s,nativo27/27,sonda indipendente79. La revisione indipendente conferma le correzioni del separatore root del programma e del confine proprietà FS per punti sorgente allo stesso oggetto. Conservati exit124 iniziali,due aspettative obsolete fallite e sorgenti/binari;verifica completa finale superata. Conteggi sovrapposti,limiti temporali invariati. Vecchi timeout HVF/nativi irrisolti,nessuna prova di stabilità. Intel HVF Actions sospeso;iOS fisico e GitHub CI completo separati.

`build-hvf-arm64/directory-rename-validation-summary.json`, `directory-rename-darwin-final-evidence/`, `directory-rename-public.xml`, `directory-rename-reports.xml`, `directory-rename-python.log`, `directory-rename-native-evidence/`, `directory-rename-probe/`, `directory-rename-initial-fixture/`, `directory-rename-darwin-evidence/failed-source/`.

### Verifica degli scambi di directory e misti, 2026-10-07

La verifica Release Darwin riconcilia 1.219 registrazioni: 763 superate, 456 saltate per backend non disponibili, zero errori. Tutti i 114 casi ARM64 HVF obbligatori sono stati eseguiti. La verifica mirata supera 32/44 casi con 12 indisponibili, inclusi tutti i 24 nuovi casi diretti 4K/16K; il componente file completo supera 317/317. I programmi nativi originali superano 28/28 e una sonda indipendente di chiamate grezze registra 32 osservazioni riuscite su questo filesystem macOS senza distinzione tra maiuscole e minuscole. I conteggi si sovrappongono e i limiti temporali ospiti restano invariati.

Una revisione indipendente del piano e del codice ha verificato la transazione bidirezionale, i costi dei file iniziali, entrambi i sottoalberi conservati, gli orfani trattenuti solo dal mapping e i rimborsi esatti. Il primo test rosso ometteva la dichiarazione esplicita di scambio della radice ed è escluso dall’accettazione; la base corretta falliva nei sei casi selezionati sul precedente rifiuto delle directory. Due asserzioni iniziali per file misti scartavano erroneamente metadati configurati; ora confrontano il record completo cambiando solo ctime. Una tabella locale di puntatori costanti introduceva rebases ARM64 e sei rifiuti di caricamento. Quattro asserzioni scalari la sostituiscono: il programma corretto non ha rebases classici e il loader mantiene il rifiuto dei fixups non supportati.

La prima esecuzione mirata corretta conserva tre timeout HVF di cinque secondi. I controlli individuali, su tre profili e la verifica completa successiva sono passati; la causa resta sconosciuta e ciò non prova stabilità della latenza. Fonti, binari, errori e controlli iniziali sono conservati. Restano incompleti spostamenti di directory iniziali, domini iniziali distinti, dichiarazioni di permessi/mount/maiuscole, dipendenze dinamiche e runtime dei framework. iOS fisico, Intel HVF sospeso e GitHub CI completo sono confini di accettazione separati.

`build-hvf-arm64/directory-swap-research/`: `darwin-final-evidence/`, `darwin-evidence/`, `focused-v5.xml`, `file-owner-v5.xml`, `native-workload-fixed-evidence/`, `native-probe-v2-results.json`, `fixture-fixups-before/`, `hvf-controls.json`.

I binari finali collegati hanno ripetuto con successo la verifica Darwin completa. L’integrazione fissata di dev 0a9a1d28d registra 4.515 casi su 20 componenti condivisi: 4.489 superati, sei Z3 facoltativi e 20 del corpus Windows EH indisponibile saltati, zero errori. Include C/CLI 170/170 e report 32/32. Python copre i cinque profili in 30,780s. Tutte le 12 varianti mobili native architettura/fixup corrispondono a 4.532 osservazioni dei programmi originali; sessione unica e metadati Swift completi corrispondono 12/12. Il generatore di witness Swift riproduce il catalogo con SDK/compilatore registrati dopo la correzione dell’indentazione di un commento. È accettazione locale Release LLVM 23/Apple Clang 17, non di dev successivo o Linux Clang 18.


## Spostamento ordinario dei sottoalberi iniziali dichiarati

Il Boolean rigoroso `"movable": true` su una directory iniziale esplicita diversa dalla radice autorizza il suo rename ordinario e dichiara l’intero sottoalbero iniziale composto da directory ordinarie senza mount o alias di nome. `DarwinFileOptions::MovableDirectories` viene aggiunto dopo i membri aggregati C++ esistenti. Il genitore immediato deve essere mutable. Assenza/false mantiene l’incertezza; altri tipi JSON sono rifiutati. I discendenti conservano i propri diritti mutable/removable/movable e di scrittura. Non autorizza SWAP di directory iniziali, permessi generali o mount. Si rifiutano flags noti, modalità speciali delle directory, file ordinari con più link e alias inode da stat/istantanee. Ogni dominio connesso dalle dichiarazioni può avere al massimo un dispositivo noto, inclusi sottoalberi fratelli e file sotto un antenato senza stat. L’uguaglianza dei dispositivi non connette altri domini.

Gli oggetti conservano nomi, genitori, stat/istantanee e diritti originali. I vecchi percorsi d’ingresso non ricreano nomi spostati o rimossi. Discendenti non aperti, FD/dup/CWD, mapping e discendenti rimossi seguono i loro oggetti. I discendenti immutati conservano stat, istantanee, cookie e SEEK_END; radice spostata e genitori modificati perdono le osservazioni complete. Riutilizzare un nome non eredita osservazioni o diritti. L’input iniziale descrive una singola esecuzione sincrona, senza API di sostituzione durante l’esecuzione. SWAP resta separato: dichiarazione diretta swap_rename degli oggetti iniziali, copia dal genitore a mkdir e conservazione durante lo spostamento. Entrambi i genitori SWAP devono avere supporto dichiarato.

Sostituire una destinazione iniziale vuota richiede anche removable. Prima della pubblicazione si verificano 1023 byte per percorso e il budget comune di 16 MiB. Ogni riferimento movable riserva il percorso originale più NUL senza un’altra voce. Percorsi, riferimenti, istantanee e voci delle directory iniziali restano riservati dopo la rimozione. PathCharge dinamico parte da zero e addebita una volta il percorso corrente di ogni membro collegato o trattenuto. Solo il costo dinamico esistente di un obiettivo subito liberabile fornisce credito; FD/CWD/figli/mapping orfani trattenuti non lo forniscono. Il recupero finale rimborsa una volta. Gli antenati iniziali impliciti non aumentano il limite di 256 voci; il recupero dei file ordinari iniziali è invariato.

Esempio:

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/work","mutable":true,"movable":true}]}}
```

initial-directory-move verifica senza SDK spostamento, cursori condivisi/indipendenti, flags FD, CWD, mapping privato, sostituzione e ripristino dei nomi. Profili guest locali e iOS fisico hanno verifiche separate.

Il limite sullo SWAP iniziale riguarda gli oggetti radice sorgente e destinazione della chiamata. Lo scambio di antenati creati può trasportare discendenti iniziali già spostati, mantenendone lo stato e i costi dinamici. Le restrizioni precedenti si applicano in assenza delle dichiarazioni richieste.

Dopo la revisione indipendente finale su macOS ARM64, il gate Darwin registra 1.264 test: 796 superati, 468 saltati per backend indisponibile, zero errori. Tutti i 117 carichi ARM64 HVF obbligatori sono stati eseguiti. Il sottoinsieme file passa 342/342, con 22 nuovi casi 4K/16K e tre controlli di ammissione. CreationPolicy conserva Device/GID del padre spostato; riuso del nome, sequenza inode e umask corrente restano distinti. Al limite esatto di 16 MiB, 16 scambi avanti/indietro non accumulano costi; il ritorno libera solo la differenza effettiva di sei byte. C/CLI pubblico: 175/175; report: 33/33; kernel nativo: 29/29; sonda originale indipendente: 19 osservazioni riuscite. Le cinque configurazioni Python passano in 27.865 secondi; API pura 71, deriva SDK e runner 49 passano anche. I conteggi si sovrappongono. Sorgenti, binari, tentativi e risultati sono conservati in build-hvf-arm64/initial-directory-move/ e legati al commit. iOS fisico, Intel HVF sospeso, SWAP delle radici iniziali, permessi/mount/case, EOF condiviso, Mach/thread/dyld e framework restano lavori separati.

## Scambio atomico delle radici iniziali dichiarate

Il Boolean rigoroso exchangeable:true (C++ DarwinFileOptions::ExchangeableDirectories, in coda all’aggregato) autorizza solo una radice iniziale esplicita diversa da / come operando RENAME_SWAP. Il padre iniziale diretto deve essere mutable. Assenza/false restano esclusi; altri tipi sono invalidi. Condivide con movable il sottoalbero ordinario senza mount e con nomi unici, più controlli flags/modi speciali/alias/hard link/dispositivi dell’intero componente. L’unione definisce soltanto la topologia. Ogni riferimento riserva percorso originale+NUL separatamente, anche sulla stessa radice, senza altra voce. Dispositivi uguali non uniscono domini.

La sorgente iniziale normale/EXCL richiede ancora movable e la sostituzione iniziale normale removable. exchangeable non concede questi diritti, mutable ai discendenti, scrittura o permessi/mount generali. Oggetti distinti richiedono entrambi i padri reali mutable e con swap_rename indipendente. SWAP omonimo autorizzato di componente normale verifica padri/dispositivi e non modifica nulla, senza prima carica né capacità per oggetti distinti. Dot/case del medesimo oggetto restano sconosciuti; ordine di destinazione assente e dot invariato.

Radici iniziali non vuote, iniziali/create e directory/file si scambiano in entrambe le direzioni. Entrambi gli alberi collegati e trattenuti sono interamente verificati, poi tutti i nomi rimossi prima della pubblicazione. Le radici restano collegate: nessun credito per sostituzione/contenuto né nuovo FD/inode/voce. FD/dup/CWD/cursori, oggetti padre, lease e diritti seguono i propri oggetti; discendenti invariati conservano stat/istantanee. Vecchi oggetti eliminati e nuovi omonimi restano separati. Costi dinamici iniziano a zero, sono addebitati una volta e poi sostituiti; i fissi restano riservati. Errori di percorso/budget preservano entrambi gli stati.

Il carico originale senza SDK initial-directory-swap scambia empty e data preesistenti e li ripristina, controllando discendenti, mapping, CWD, cursori, flags e creazione dopo spostamento. L’accettazione precedente f98068c07 è un distinto risultato normale congelato; solo questa dichiarazione estende SWAP delle radici iniziali.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true,"swap_rename":true},{"path":"/left","exchangeable":true},{"path":"/right","exchangeable":true}]}}
```


La verifica Release su macOS ARM64 registra 1,303 casi Darwin: 823 superati, 480 saltati per backend indisponibili, zero errori; tutti i 120 casi ARM64 HVF obbligatori sono eseguiti. File 361/361 (16 nuovi casi 4K/16K e 3 ammissioni), C/CLI 180/180, parser dei rapporti 34/34, programmi kernel originali 30/30 e sonda indipendente 35 osservazioni passano. Python copre cinque configurazioni in 33.894 secondi; passano 71 test API puri, 49 unità inventario/riferimento e verifiche SDK, formato, capacità, provenienza e documentazione. I conteggi si sovrappongono.

Alla capacità esatta, lo stesso nome e 16 scambi di andata e ritorno conservano oggetti e costi. Se servono sei byte ma ne restano cinque, entrambi i rifiuti conservano alberi, cursori e budget di creazione. Le revisioni indipendenti del piano e del codice finale sono approvate. Tentativi, sorgenti, binari e risultati sono congelati in `build-hvf-arm64/initial-directory-swap/` e legati al commit. Un rapporto eseguito con sovrapposizione è escluso e ripetuto in serie; i marcatori di traduzione sono sincronizzati. Nessun limite temporale o controllo negativo è allentato. Permessi, mount/maiuscole non dichiarati, mappe condivise/EOF, orologi progressivi, Mach/thread/dyld/framework restano incompleti; iOS fisico, Intel HVF sospeso e CI remota di merge sono verifiche separate.

## Osservazioni esplicite dei limiti, in sola lettura

`getrlimit(194)` legge `DarwinSystemOptions::ResourceLimits` (`darwin_system.resource_limits`) nei cinque profili Darwin. Ogni `DarwinResourceLimit`, chiave 0..8, contiene due uint64 little-endian agli offset 0/8: 16 byte. Vale `0 <= current <= maximum <= 9223372036854775807`; zero è esplicito e INT64_MAX indica infinito. Nessuna query al host o modifica dei budget FD, VM, storage o esecuzione. `setrlimit`, applicazione dei limiti, segnali e scheduling restano incompleti.

JSON rigoroso ammette al massimo nove chiavi uniche con esattamente `resource`, `current`, `maximum`, interi esatti o stringhe decimali senza segno. Tipi/campi errati, duplicati, chiavi non canoniche e limiti invertiti falliscono prima del caricamento. Array vuoti/omessi sono ignoti; le chiavi configurate non vengono normalizzate.

Solo la syscall usa i 32 bit bassi del selettore e rimuove `_RLIMIT_POSIX_FLAG=0x1000`. Risorse invalide restituiscono EINVAL prima dell’accesso; osservazioni mancanti fermano come unsupported prima dell’output. Output totalmente non scrivibile restituisce EFAULT; coppie parzialmente scrivibili sono rifiutate senza scrivere. Copie complete non allineate o tra pagine cambiano solo 16 byte; errori backend restano di trasporto. La sonda ARM64 nativa ha passato 23 verifiche di valori/selettori e quattro errori separati; il prefisso parziale invariato su questo host non è una garanzia portabile.

```json
{"darwin_system":{"resource_limits":[{"resource":8,"current":256,"maximum":"9223372036854775807"}]}}
```

macOS ARM64 Release: registrati/passati/omessi non eseguiti/falliti 1337 / 845 / 492 / 0; tutti i 123 HVF richiesti eseguiti. Dodici nuovi casi 4K/16K e oracolo SDK; C/CLI 190/190, parser 37/37, carichi nativi 31/31; Python su cinque profili in 71.978 secondi, 71 verifiche API e 49 runner passate. Passano drift SDK, formato, capacità, provenienza e documentazione; i conteggi si sovrappongono. Evidenze congelate in `build-hvf-arm64/resource-limit-observations/` e legate al commit. Primo errore di build da enum di test, tentativi di collegamento/filtro conservati; validazione corretta seriale senza allentare scadenze o controlli. Applicazione dei limiti, permessi, directory dopo mutazione, mappe condivise/EOF, orologi, Mach/thread/dyld e framework restano da fare. iOS fisico, Intel HVF sospeso e CI remota richiedono accettazione separata.

## Osservazioni esplicite del consumo di risorse in sola lettura

getrusage(117) legge DarwinSystemOptions::ResourceUsageSelf / ResourceUsageChildren opzionali e indipendenti nelle cinque configurazioni Darwin. JSON rigoroso: darwin_system.resource_usage.self / .children. Ogni DarwinResourceUsage richiede int64 user_seconds/system_seconds, uint32 user_microseconds/system_microseconds inferiori a1000000 e quattordici counters int64. Interi esatti o stringhe decimali con segno conservano l’intervallo completo; tipi/campi/microsecondi/lunghezze errati falliscono prima del caricamento. Il compagno assente resta ignoto senza bloccare quello fornito; zero esplicito valido.

Una copia completa little-endian di144 byte: timeval a0/16 (secondi8, microsecondi4, padding zero4), counters da32 ciascuno8. Valori/unità Darwin originali, ru_maxrss senza conversione Linux KiB. Dati fissi senza misura host, contabilità, fork/wait, scheduling o applicazione limiti.

Solo32 bit bassi: 0=SELF, -1=CHILDREN; 0x1000 non valido e nessuna rimozione POSIX flag. Non valido EINVAL prima della memoria, assente unsupported prima dell’output. Copia completa disallineata/interpagina conserva guardie; interamente non scrivibile EFAULT, parziale unsupported prima di ogni byte; errori backend restano di trasporto. SDK cattura una volta per selettore, senza confrontare successivi SELF variabili. Probe nativo13 controlli e4 processi di errore indipendenti con limite5s invariato. Qui partial SELF scrive64 byte prima di EFAULT; nessuna garanzia generale del prefisso.

0..13: `ru_maxrss`, `ru_ixrss`, `ru_idrss`, `ru_isrss`, `ru_minflt`, `ru_majflt`, `ru_nswap`, `ru_inblock`, `ru_oublock`, `ru_msgsnd`, `ru_msgrcv`, `ru_nsignals`, `ru_nvcsw`, `ru_nivcsw`.

```json
{"darwin_system":{"resource_usage":{"self":{"user_seconds":"-9223372036854775808","user_microseconds":999999,"system_seconds":0,"system_microseconds":0,"counters":["9223372036854775807",0,0,0,0,0,0,0,0,0,0,0,0,0]}}}}
```

[Apple getrusage](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getrusage.2.html), [XNU](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c), [SDK layout](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/resource.h).

macOS ARM64 Release registrati/superati/skip senza esecuzione/falliti 1371/867/504/0; 126 HVF obbligatori eseguiti. File361/361, C/CLI 200/200, JSON 40/40, System/SDK 53/53, native 32/32 superati. Dodici nuovi4K/16K e ammissione/SDK; Python cinque configurazioni 42.865s, API71, runner/reference49 e controlli SDK drift/formato/capacità/provenienza/documentazione superati. Conteggi sovrapposti, revisione indipendente superata. Prove congelate in `build-hvf-arm64/resource-usage-observations/` e associate al commit. Prima asserzione x64 EINVAL corretta per mantenere RDX, ARM64 azzera X1; errore/filtro vuoto conservati. Runtime/scadenze/controlli negativi invariati. Limiti, permessi, directory dopo mutazione, shared maps/EOF, orologi, Mach/thread/dyld, framework incompleti; iOS fisico, Intel HVF sospeso e CI merge separati.

Primo gate:866 superati, un timeout5s del rename iOS ARM64 HVF esistente,504 skip. Stesso binario supera il caso in386ms, poi passa il gate completo seriale. Entrambi conservati; causa ignota, nessuna garanzia di latenza.


## Credenziali esplicite, gruppi e proprietario coerente

Credentials opzionale contiene RealUID/EffectiveUID/RealGID/EffectiveGID e GroupAccessList opzionale indipendente. Omissione mantiene quattro getter1000; zero/root esplicito valido, ID0..INT32_MAX. Gruppi1..16, primo=EffectiveGID, ordine/duplicati preservati; assenza sconosciuta, nessuna inferenza host/EGID. darwin_system.credentials richiede esattamente real_uid/effective_uid/real_gid/effective_gid, groups opzionale. Interi senza perdita/validatore centrale rifiutano forma/campi/intervallo/numero/primo incoerente prima del caricamento; profili nonDarwin rifiutati.

getuid24/geteuid25/getgid47/getegid43/getgroups79 condividono un proprietario system. Nuovo file regolare usa UID effettivo, device/GID del genitore diretto; rename/FD mantenuti/riuso nome preservano oggetto, stat input immutato. Root non concede scrittura/mutazione/ACL; setuid/setgid/setgroups e processo/sessione assenti.

getgroups capacità=low32 int con segno: negativo primaEINVAL, sconosciutounsupported, zero noto=count senza puntatore, positivo cortoEINVAL prima memoria; sufficiente copia una volta4*count byte little-endian. 0x1000 positivo, nessun POSIXflag rimosso. Guard completi nonallineati/tra pagine; tutto nonscrivibileEFAULT, parzialeunsupported prima byte, backend rimane trasporto. ErrorBSD mantiene x64RDX/azzera ARM64X1; successo azzera entrambi secondari, report conserva argomenti grezzi.


~~~json
{"darwin_system":{"credentials":{"real_uid":101,"effective_uid":202,
 "real_gid":303,"effective_gid":404,"groups":[404,0,"2147483647",7,7]}}}
~~~


[Apple getgroups contract](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getgroups.2.html), [pinned XNU credential/group ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_prot.c).

registrati/superati/skip indisponibili/falliti: 1413/897/516/0; ARM64 HVF 129/129. System/SDK72/72, File365/365, Report43/43, C/CLI210/210; guest8/12skip; native33/33; Python5 82.918s; API71, runner/reference49. SDK drift / clang-format22.1.2 / capabilities23 / provenance / docs11+negative3: OK. Independent source review: OK.

21 value +5 native fault,5s; host16groups, positive short capacity: OK. Native partial32byte thenEFAULT: observation only. Counts overlap. `build-hvf-arm64/credential-observations/`.

Primo run8 fallimenti (5 budget istruzioni,3 scadenze5s),12skip. Scansione due pagine del fixture limitata alla finestra completa132byte sullo stesso confine (64prima,fino64dati,almeno4dopo); owner diretto verifica ancora due pagine intere. Budget/argomenti/controlli negativi invariati, sorgenti/binari/entrambi run conservati. Geometria trasporto corretta prima esecuzione su review; selezione pubblica confronta contenuti. Permessi/ACL,link,osservazioni directory dopo mutazione,shared maps/EOF,orologi,Mach/thread/dyld/framework incompleti; iOS fisico,IntelHVF sospeso,mergeCI separati.
