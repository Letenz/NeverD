**Lingue**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 651fc905db4bb2d62a1036bb6495b60cad23dde9b5e655b4b4ca408294a3d83c -->

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

ARM64 usa X16, X0–X5 e `svc #0x80`; x64 usa la classe BSD `0x02000000`, RAX e RDI/RSI/RDX/R10/R8/R9. Il successo azzera carry; l’errore lo imposta e restituisce errno positivo. ARM64 azzera X1; x64 azzera RDX al successo e lo preserva in errore. Le modifiche ai registri di SYSCALL sono esplicite. Il rapporto usa `result` e `error=true` per errori BSD; richieste senza ritorno o non supportate non hanno questi campi. Le regole seguono XNU [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c) e [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c), senza incorporare codice Apple.

Servizi: `exit`, `write`, `getpid`, `getppid`, `getuid`, `geteuid`, `getgid`, `getegid`, `mmap`, `mprotect`, `munmap`. PID/UID/GID valgono 1000, PPID vale 1. I descrittori 1 e 2 catturano byte, inclusi NUL e non UTF8; quelli chiusi o di sola lettura restituiscono EBADF. Una copia parziale conserva i byte già letti ma il guasto successivo rimane EFAULT. Una lunghezza oltre `INT_MAX` produce EINVAL prima di controllare descrittore, puntatore o budget: [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).

La memoria supporta mapping privati anonimi di dati con `flags=0x1002`, descrittore -1 e offset zero. Lunghezze e suggerimenti non fissi sono arrotondati verso l’alto alla pagina OS. Un suggerimento occupato cerca prima verso indirizzi superiori, poi torna al posizionamento predefinito. Il mmap storico grezzo con lunghezza zero restituisce zero senza allocare; `MAP_UNIX03` è supportato e rifiuta lunghezza zero con EINVAL. Unmap/protect richiedono indirizzi allineati. NONE/READ/WRITE sono supportati e WRITE implica READ. Ogni pagina OS possiede la sua memoria fisica: un unmap parziale libera budget e nuove pagine sono azzerate. Un protect attraverso un buco o oltre i diritti massimi lascia invariato l’intero intervallo. Fonte: [servizi VM XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

Mapping condivisi/fissi/JIT o anonimi eseguibili, trap Mach, syscall indirette, thread, segnali, file host/rete, dyld, runtime Objective-C/Swift e Foundation/UIKit sono esclusi e arrestano esplicitamente l’esecuzione. Questo non è un intero OS Apple né l’applicazione iOS Simulator.

## Verifica

Le fixture C originali sono generate con Clang e `ld64.lld`, senza SDK Apple o binari proprietari. Coprono cinque combinazioni piattaforma/ISA, record Mach-O malformati, pagine 4/16 KiB e rilascio parziale a budget pieno. `NeverDProcessPublicTests` confronta C API/CLI; `NEVERD_TEST_LIBNEVERD` e `NEVERD_TEST_DARWIN_FIXTURES` abilitano le stesse cinque combinazioni in Python.

## File e descrittori espliciti

`darwin_files` offre ai tre profili un catalogo chiuso di file di sola lettura. Il campo obbligatorio `files` contiene `path` guest assoluti canonici e `bytes_hex` esadecimali. `stdin_hex` opzionale fornisce input finito: l’assenza significa sconosciuto e arresta letture non vuote, una stringa vuota indica EOF. Senza catalogo open si arresta; un catalogo esplicitamente vuoto restituisce ENOENT. Non si consultano file o input host.

Si aggiungono `open`, `read`, `pread`, `lseek`, `close`, `dup`, `dup2`, `fcntl` e gli ingressi nocancel di read/write/open/close/fcntl/pread. Sono supportati O_RDONLY/O_CLOEXEC e F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL. Aperture separate hanno posizioni indipendenti; dup condivide la posizione ma mantiene flag close-on-exec distinti. pread non cambia posizione. Chiudere o sostituire 0/1/2 modifica l’I/O successivo; gli output duplicati conservano destinazione e budget.

Limiti: 256 file, 16 MiB complessivi per percorsi/NUL/file/input, percorsi sotto 1024 byte e componenti fino a 255. `descriptor_limit` è un limite esclusivo 3–4096, predefinito 256; JSON resta limitato a 64 KiB. Opzioni invalide falliscono prima del caricamento. read oltre INT_MAX restituisce EINVAL prima del controllo FD; EOF non tocca la destinazione e un indirizzo invalido dà EFAULT. Buffer parzialmente scrivibili arrestano prima di copia o avanzamento. Gli errori SET/CUR/END conservano la posizione. Scrittura, stat precedente, seek sparsi e altri fcntl restano esclusi. Un file come antenato dà ENOTDIR. Lo stesso oggetto viene confrontato con macOS nativo; C/CLI/Python coprono cinque combinazioni guest, non dispositivi iOS.

Verifica Release del 2026-10-05: 381 registrazioni, 177 passate, 204 saltate, zero errori e 51/51 casi ARM64 HVF obbligatori eseguiti. Passano anche sette programmi macOS nativi, 35 test pubblici C/CLI/report, cinque combinazioni Python e 66 test del verificatore. I conteggi si sovrappongono. Mancano prove native Intel HVF/KVM/WHP per i nuovi servizi; Intel HVF resta non validato e le sue Actions sospese. Mancano SDK iOS e confronto su dispositivo.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

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

`stat64` (338), `fstat64` (339) e `lstat64` (340) restituiscono lo stesso record LP64 di 144 byte su ARM64/x64. Condividono la risoluzione di open e rispettano dup/close senza allocare FD né cambiare cursori. rdev, padding e campi riservati sono zero. Le osservazioni sono fisse: read non aggiorna i tempi e mode non cambia l’accesso al catalogo. Metadati mancanti, flussi, link simbolici, stat precedente, e sicurezza estesa restano esclusi. Gli errori di percorso/FD precedono il puntatore di uscita; le uscite parzialmente accessibili sono rifiutate prima della scrittura. Il test nativo confronta tutti i byte di un file reale e gli offset SDK; lo stesso programma originale verifica le tre chiamate.

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

La verifica autonoma richiede tutti i 60 casi nativi ARM64 o 40 x64, compresi `LC_MAIN` e `LC_UNIXTHREAD` su ogni piattaforma. Casi obbligatori mancanti/saltati o assenza di `ld64.lld` causano errore.

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
