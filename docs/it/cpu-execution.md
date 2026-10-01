**Lingue**: [English](../cpu-execution.md) | [简体中文](../zh-CN/cpu-execution.md) | [繁體中文](../zh-TW/cpu-execution.md) | [日本語](../ja/cpu-execution.md) | [한국어](../ko/cpu-execution.md) | [Français](../fr/cpu-execution.md) | [Deutsch](../de/cpu-execution.md) | [Español](../es/cpu-execution.md) | [Italiano](cpu-execution.md) | [Русский](../ru/cpu-execution.md) | [العربية](../ar/cpu-execution.md)

[← Indice della documentazione](README.md)

# Esecuzione CPU e query sulle capacità

L’esecuzione CPU è indipendente dal sistema operativo guest, dal loader dell’immagine e dalla convenzione di chiamata. `NEVERD_ENABLE_CPU_EMULATION` la compila da sola; `NEVERD_ENABLE_DRIVER_EMULATION` include anche il modello dei driver Windows. La [guida all’architettura](architecture.md) descrive responsabilità, selezione del backend e limiti attuali delle piattaforme.

## Configurazione

La pubblica [`ExecutionConfiguration`](../../include/neverd/emulation/ExecutionConfiguration.h) è usata sia dalla factory CPU sia dal report delle capacità. I requisiti sono verificati prima di allocare la CPU o collegare lo spazio degli indirizzi. Valori omessi usano il profilo fisso del contratto; valori espliciti non supportati causano errore.

| Campo JSON | Predefinito | Significato |
|---|---|---|
| `backend` | `auto` | `auto`, `unicorn`, `kvm` oppure `whp` |
| `contract` | `software-cpu-v1` | Semantica di esecuzione versionata |
| `architecture` | `x86_64` | `x86_64` o `aarch64` |
| `privilege` | Profilo del contratto | `flat`, `supervisor` o `user`; deve corrispondere al contratto |
| `virtual_address_bits` | Profilo del contratto | I profili checked usano 48 bit; quelli flat uno spazio di mapping diretto a 64 bit |
| `page_size` | 4096 | Granularità del mapping guest; gli altri valori sono rifiutati |
| `required_features` | `[]` | Nomi richiesti da [`ExecutionConfiguration.def`](../../include/neverd/emulation/ExecutionConfiguration.def) |

`driver-strict` accetta x64; `software-cpu-v1` x64 e ARM64. `checked-x64-v1` e `checked-aarch64-v1` richiedono l’architettura indicata e privilegio supervisor. `checked-user-x64-v1` e `checked-user-aarch64-v1` eseguono l’inventario limitato corrispondente al contratto a CPL3 ed EL0, rispettivamente, con isolamento MMU e uscite esplicite di richiesta servizio. Supportano Unicorn e KVM/WHP compatibili con l’host; `auto` segue la selezione dell’host. I profili flat non promettono isolamento MMU utente/supervisor. I profili ARM64 e x64 checked ammettono le famiglie FP/SIMD limitate descritte sotto. x64 supervisor aggiunge transazioni MMIO limitate e letture stringa preparate; i profili user rifiutano mapping di dispositivi. Tutti i profili checked continuano a rifiutare I/O di porta e requisiti di CPU parallela. Solo i profili user dichiarano `service_traps`.

L’esecuzione user richiede `UserAccessible` oltre al diritto `Read`, `Write` o `Execute` appropriato su **ogni** pagina mappata. I mapping esistenti sono supervisor per default; gli alias hanno diritti indipendenti anche se condividono i byte fisici. `UserAccessible` da solo non autorizza l’accesso. Le operazioni host fidate e le CPU supervisor usano RWX. Esempio:

```cpp
Configuration.Contract = ExecutionContract::CheckedUserX64;
Configuration.Privilege = ExecutionPrivilege::User;
auto CPU = llvm::cantFail(createExecutionBackend(Configuration, Space)).CPU;
llvm::cantFail(CPU->map(Code, 4096, Read | Write | Execute | UserAccessible));
```

Il contratto fissa il privilegio: il ripristino del contesto e il binding dello spazio non lo modificano, e i selettori di segmento x64 non possono elevarlo. Un fault dati recuperabile conserva istruzione e registri originali finché il proprietario non lo gestisce. `canAccess` verifica esattamente i diritti richiesti; aggiungere `UserAccessible` per interrogare la visibilità user. Le page table sono proiezioni CPU private, non espongono tabelle guest mutabili né API per cambiare privilegio. Su ARM64 le pagine user non sono eseguibili anche a EL1.

Campi sconosciuti o nulli, nomi/larghezze numeriche non validi, feature richieste duplicate e combinazioni non supportate falliscono. L’input è limitato a 64 KiB; le vecchie factory CPU e le opzioni C dei driver restano compatibili.

## Interrogare senza eseguire un workload

```bash
neverd cpu-capabilities
neverd cpu-capabilities \
  --configuration='{"contract":"checked-aarch64-v1","architecture":"aarch64"}' \
  --probe-host
```

Lo schema è alla versione 1. Il report separa `requested_configuration` prima dei default di profilo, `configuration` normalizzata, `capabilities` semantiche statiche, `build` per supporto adapter/ABI e `host` (null senza `--probe-host`). Il probe inizializza una CPU temporanea su RAM privata: prova solo l’inizializzazione, non l’idoneità del workload né l’esecuzione ARM64 nativa. La disponibilità può cambiare e un backend indisponibile non viene mai sostituito in silenzio. Il CLI restituisce 0 per un report valido anche se il backend non è disponibile, 1 per configurazione/query non valida.

## Confini SDK e C++

[`neverd_cpu_capabilities_json`](../../include/neverd/sdk/NeverDCAPICPU.h) accetta una sessione, JSON di configurazione facoltativo e `ProbeHost` 0 o 1; non serve un’immagine caricata. Liberare il risultato con `neverd_free_string`; NULL indica un errore descritto da `neverd_last_error`. Le build senza CPU esportano la stessa funzione e segnalano esplicitamente la disattivazione. I plugin Python usano `session.cpu_capabilities(...)`. In C++ `executionCapabilities`, `resolveExecutionConfiguration`, `queryExecutionBackendBuild` e `probeExecutionBackend` sono separabili; `createExecutionBackend` collega una CPU a uno spazio esistente o crea RAM privata e spazio predefinito.

## Esiti CPU e budget

`CPU.runUntilExit(PC, TimeoutMicroseconds)` restituisce un [`ExecutionExit`](../../include/neverd/emulation/ExecutionExit.h) tipizzato. Errori di setup restituiscono `llvm::Error`; una run avviata segnala stop, deadline, service request, fault recuperabile, fault/trap guest, operazione non supportata, errore device/backend o stop del motore inspiegato. Fault CPU/device/backend prevalgono su stop/deadline simultanei, mantenendo i fatti e dettagli indipendenti. Il timeout deve essere positivo e rappresentabile sia come durata sia come deadline assoluta; altrimenti l’errore avviene prima di modificare la CPU. Ogni invocazione richiede un budget finito; zero non significa né infinito né timeout immediato valido. Il controllo è cooperativo, non una deadline rigida. Il risultato non consuma un fault recuperabile: il proprietario OS deve acquisirlo e installare un trasferimento d’eccezione validato prima di riprendere. Restano disponibili `run`, `fault` e `timedOut`; implementazioni CPU esterne che ridefiniscono solo `run` rifiutano il nuovo confine tipizzato.

## Service request

Il profilo user-x64 intercetta solo la codifica esatta senza prefisso di `SYSCALL`; user-ARM64 intercetta `SVC #imm16`. `SYSENTER`, `INT`, `HVC`, `BRK` e altri meccanismi non sono supportati. L’observer delle istruzioni viene eseguito per primo. Se non ferma né causa un fault, la CPU restituisce `ExecutionExitKind::ServiceRequest` **prima** dell’istruzione o del backend, con tipo, `PC` originale, `NextPC` sequenziale e immediato SVC. Registri, flag, stack e privilegio restano invariati: x64 non ha applicato i clobber RCX/R11 di SYSCALL e ARM64 non è entrato in un exception vector. L’immediato SVC non è un numero servizio universale.

La richiesta resta pendente e blocca esecuzione, mutazione CPU, binding dello spazio e cattura/ripristino del contesto finché il proprietario OS la consuma una sola volta con `takeServiceRequest()` a CPU ferma. Il proprietario decodifica la ABI OS, gestisce il servizio e sceglie esplicitamente registri risultato e PC/trasferimento d’eccezione. Un servizio non supportato fallisce a quel confine; riprovare il PC originale genera un’altra richiesta, senza NOP o successo implicito. L’evento servizio prevale su stop/deadline simultanei, mentre fault guest/backend prevalgono su di esso. Stop CPU, HLT software, deadline o trap non provano il successo del workload. Il [profilo processi Linux](process-emulation.md) ha un modello OS separato e non prova compatibilità Windows, Android o Darwin. L’esecuzione nativa Windows/ARM64 richiede ancora validazione runtime.

## Estensioni x64 e stato CPU nativo

Il profilo x64 checked ammette movimenti e logica SSE/SSE2 legacy limitati, `MOVLHPS`/`MOVHLPS` e forme scalari mascherate `CVTTSS2SI`/`CVTTSD2SI`/`SUBSS`/`SUBSD`. MXCSR conserva stato sticky, arrotondamento e FTZ; DAZ ed eccezioni non mascherate sono rifiutati. KVM/WHP sincronizzano tutti i 16 registri XMM e MXCSR; codifiche e operandi non elencati restano esclusi.

Il profilo x64 checked ammette anche le forme legacy mascherate `SS`, `SD`, `PS`, `PD` di `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `MIN`, `MAX`. `X64SSEInstructions.def` centralizza larghezze, allineamento e ammissione. `MaskedSSEArithmeticMatchesIndependentHostExecution` confronta registri/RAM con un riferimento CPU host indipendente: quattro arrotondamenti, FTZ, zeri con segno, subnormali e NaN. `SSEMemoryObserverStopsBeforeResultAndStatusChanges` verifica l’arresto prima degli effetti. DAZ, eccezioni non mascherate, x87 e AVX restano esclusi.

Il thread pointer copre FS/GS su x64 e `TPIDR_EL0` su ARM64 con codifiche `MRS`/`MSR` esatte. Trasporti nativi e snapshot CPU preservano lo stato separatamente dalla memoria; non creano thread OS né blocchi TLS. x64 supervisor ammette transazioni MMIO scalari allineate da 1/2/4 byte e un elemento MOVS per confine di ripresa. Le letture dispositivo richiedono una preview pura e commit al massimo una volta. I profili user rifiutano mapping di dispositivi; RMW, MMIO largo e I/O di porta restano rifiutati.

KVM e WHP annullano gli ingressi nativi attivi e ne riconoscono l’annullamento prima di ritirare le risorse. KVM usa un thread privato e sblocca temporaneamente un segnale realtime; il segnale scelto non deve essere ignorato durante l’ingresso. Maschere e handler del chiamante non cambiano. Se il progresso guest è incerto, l’annullamento è terminale; non è garantita una deadline wall-clock rigida.

## Eccezioni sincrone native x64

Le `DIV`/`IDIV` checked x64 usano risultati reali del processore e `#DE`. KVM usa una IDT/IST supervisor privata, WHP una bitmap esplicita; contesto originale e codici disponibili restano distinti dagli errori di trasporto. Il sistema operativo consuma l’evento recuperabile prima di impostare la continuazione. I driver Windows traducono divisione per zero e overflow del quoziente in `STATUS_INTEGER_DIVIDE_BY_ZERO`, eseguendo veri filtri SEH, `__finally` e tentativi successivi. `NeverDX64ExceptionTests` compila senza Unicorn; `DriverWDMCPUException` verifica casi WDK originali. Gli host WHP/ARM64 non disponibili sono saltati esplicitamente.

## Effetti RAM preparati

`RAMTransaction` conserva soltanto l’unione fisica delle scritture dichiarate di un’istruzione, sotto il blocco di esecuzione. Ripristina la RAM originale prima degli osservatori dei risultati; annullamento, errore di trasporto o eccezione dell’osservatore non pubblicano RAM o registri parziali. Dopo il ripristino della RAM, gli errori CPU mantengono lo stato architetturale di eccezione. Le scritture singole e doppie ARM64 usano la stessa autorità. x64 esegue `XCHG`, `XADD` e `CMPXCHG` a 8/16/32/64 bit, con allineamento naturale per forme bloccate o implicitamente bloccate. `NeverDRAMTransactionTests` confronta i risultati con la CPU host e verifica ripristino, alias e permessi; le piattaforme indisponibili sono saltate esplicitamente. Dispositivi e SMP parallelo restano esclusi; gli snapshot CPU non ripristinano la RAM già confermata.

## Stato x87 completo

`NeverDEmulationArch` possiede i contratti ISA, le tabelle delle pagine e il formato FP condiviso dai trasporti nativi e Unicorn. I contesti x64 conservano controllo, stato, TOP, tag fisici, opcode, puntatori istruzione/dati e otto registri a 80 bit. `FP0`–`FP7` usano `RegisterValue`; gli accessi scalari rifiutano il troncamento. `FPTag` è la maschera fisica dei registri non vuoti. `NeverDX64FPTests` verifica tutti i TOP, operazioni esatte contro FXSAVE/FXRSTOR dell’host e ripristino. Ciò non ammette istruzioni x87 nel contratto checked e non prova tutti gli arrotondamenti. Gli host nativi non disponibili vengono esplicitamente saltati.

`driver-strict` supporta KVM su host Linux x64 compatibili e WHP su host Windows x64 compatibili; `auto` sceglie quel trasporto nativo, mentre ISA diverse usano Unicorn. Unicorn esplicito e la precedente API V1 mantengono il profilo software portabile. L’esecuzione nativa verifica indirizzi canonici ed effetti prima dell’ingresso; hardware assente produce un errore senza ripiego. Istruzioni e comportamento OS non supportati falliscono esplicitamente. Mancano prove native ARM64/WHP; non è stabilita la compatibilità universale dei driver o Android/Darwin.

Interrogare il profilo selezionato con `executionCapabilities(Contract, ISA, Backend)`. `NativeLegacyX64` descrive l’esecuzione nativa dei driver x64. `NeverDNativeDriverTests` verifica il corpus esistente e può essere eseguito anche in una compilazione senza Unicorn.

ARM64 checked usa un unico confine per lo stato completo. `Registers.def` definisce 39 campi scalari e 32 vettori da 128 bit; `captureAArch64State` prepara tutte le letture, applica le larghezze e normalizza NZCV prima di pubblicare una sola volta. Unicorn, KVM e WHP trasferiscono lo stesso inventario, inclusi TPIDR_EL0, TPIDRRO_EL0, TPIDR_EL1, FPCR e FPSR. Gli adattatori nativi abilitano FP/SIMD tramite CPACR_EL1. Letture fallite e ingressi annullati conservano tutto lo stato del chiamante.

`CheckedAArch64Instructions.def` e `AArch64InstructionEffects` ammettono a EL0/EL1 aritmetica FP32/FP64 di base limitata, confronti, trasferimenti e SIMD a larghezza fissa. FPCR conserva quattro arrotondamenti, FZ e DN; FPSR conserva stato cumulativo e QC. I bit non supportati vengono rifiutati prima delle modifiche. FP16 aritmetico, SVE/SME, eccezioni non mascherate, estensioni opzionali e forme non elencate falliscono esplicitamente. Non aggiunge caricamento di driver Windows ARM64 o altri ambienti OS.

`AArch64InstructionEffects` possiede gli intervalli RAM scalari e FP/SIMD singoli o accoppiati, fino a 128 bit per operando. Lo spazio condiviso verifica ogni pagina prima dell’ingresso; `RAMTransaction` conferma solo scritture fisiche dichiarate complete. L’osservatore da 128 bit riceve due parole ordinate da 64 bit prima degli effetti. Stop e fault conservano RAM, vettori e aggiornamento dell’indirizzo. Lo stesso numero Xn/Vn è valido; le coppie con riavvolgimento dell’indirizzo sono rifiutate. `NeverDAArch64MemoryTests` usa `AArch64CrossPageCases.def` e `AArch64VectorMemoryCases.def` indipendenti.

KVM x64/ARM64 usa `KvmRunControl` per preparare, entrare in `KVM_RUN` e acquisire lo stato sullo stesso worker vCPU privato. La preparazione avviene una volta anche con `EINTR`; annullamento e lettura fallita impediscono la pubblicazione. `KvmAArch64Machine.cpp` esegue manutenzione delle traduzioni e trasferimenti scalari/vettoriali completi con una sola scadenza di passo. Il chiamante pubblica dopo conferma e mantiene decodifica ISA, transazioni RAM, politica OS e osservatori. Le prove native ARM64 restano mancanti.

KVM confronta i registri generali e lo stato FP/SSE completo con l’ultima acquisizione di debug confermata tramite `X64HostRegisters.def` e `X64FPState.def`, reinstallando gli ingressi modificati. Il confronto include scritture host e ripristini del contesto; eccezioni, annullamenti ed errori invalidano il riuso. Passo singolo e lettura dello stato generale/FP effettivo restano eseguiti per ogni istruzione.

L’esecuzione hardware da sola non garantisce una latenza complessiva inferiore. L’esecuzione nativa attuale effettua ammissione, osservazione, trasferimento di stato e uscita VM per ogni istruzione. Confrontare le stesse immagini e gli stessi scenari originali con budget identici di istruzioni ed eventi e riportare la corrispondenza dei risultati insieme ai tempi; includere avvio e caricamento nella latenza CLI.

Unicorn verificato usa `MachineRunControl`: un unico margine copre manutenzione ARM64, esecuzione guest e acquisizione completa dello stato. `UC_HOOK_CODE` controlla il token di arresto preso in prestito e la scadenza all’ingresso dell’istruzione. La chiamata sincrona rilascia il riferimento del hook prima del ritorno, ma il passo macchina conserva il controllo fino alla pubblicazione. Unicorn e WHP preparano tutto lo stato CPU e verificano lo stesso controllo prima di pubblicare un passo riuscito. WHP crea il margine una sola volta prima della preparazione. Un’eccezione CPU x64 autenticata ha precedenza su un arresto ricevuto durante l’acquisizione. La transazione RAM verificata scarta scritture speculative quando l’acquisizione è annullata; il contratto software non limitato resta invariato. `MachineInterruptedError` distingue un annullamento confermato da un errore host o di acquisizione. La CPU verificata condivisa restituisce `Stopped` o `Deadline`, conserva CPU/RAM e consente il tentativo successivo; i veri errori restano `BackendFailure` anche con un arresto simultaneo.