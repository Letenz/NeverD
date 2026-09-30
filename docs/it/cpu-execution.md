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

`driver-strict` accetta x64; `software-cpu-v1` x64 e ARM64. `checked-x64-v1` e `checked-aarch64-v1` richiedono l’architettura indicata e privilegio supervisor. `checked-user-x64-v1` e `checked-user-aarch64-v1` eseguono lo stesso inventario scalare limitato a CPL3 ed EL0, rispettivamente, con isolamento MMU e uscite esplicite di richiesta servizio. Supportano Unicorn e KVM/WHP compatibili con l’host; `auto` segue la selezione dell’host. I profili flat non promettono isolamento MMU utente/supervisor. Tutti i profili checked rifiutano FP/SIMD, MMIO, I/O di porta e requisiti di CPU parallela. Solo i profili user dichiarano `service_traps`.

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
