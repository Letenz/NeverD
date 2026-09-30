**Lingue**: [English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← Indice della documentazione](README.md)

# Emulazione dei processi guest

`neverd emulate` esegue un’immagine con un profilo esplicito del sistema operativo guest. Trasporto CPU, parsing dell’immagine, ingresso del processo e servizi OS hanno responsabilità separate. Attivare `NEVERD_ENABLE_CPU_EMULATION=ON`; è incluso anche dall’emulazione dei driver.

Il primo profilo, `linux-elf64-v1`, esegue ELF `ET_EXEC` x64/AArch64 e PIE statici `ET_DYN` autorilocanti a CPL3 o EL0. Carica veri segmenti ELF, crea lo stack iniziale, riprende a quanti e gestisce richieste esplicite di system call Linux. È un modello di processo autonomo, non una distribuzione Linux completa né una promessa di eseguire binari libc arbitrari. Linking dinamico, segnali, thread, filesystem e servizi non supportati falliscono esplicitamente. Il profilo x64 ammette alcune forme SSE/SSE2 limitate; AArch64 resta integer-only. Windows, Android, Darwin e altri workload kernel sono separati.

## CLI e SDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

Host Linux compatibili selezionano KVM e host Windows compatibili WHP; le altre combinazioni ISA host/guest usano Unicorn. Un backend selezionato ma indisponibile è un errore, senza fallback silenzioso. L’immagine ELF usa il modello Linux anche se eseguita su Windows. Vedi [esecuzione CPU](cpu-execution.md) per inventario istruzioni e limiti.

Il CLI emette un singolo report JSON. Exit code 0 per status guest zero, 2 per un altro status, 3 per esecuzione incompleta (inclusi fault e limiti), 1 per setup/API non valido. Lo status guest effettivo è in `exit_status`. L’entry point C aggiuntivo [`neverd_emulate_process_json`](../../include/neverd/sdk/NeverDCAPIProcess.h) accetta sessione, path non vuoto, profilo esplicito e JSON opzioni facoltativo. Liberare il risultato con `neverd_free_string`; NULL segnala errore di setup descritto da `neverd_last_error`. Fault guest o stop per risorse restituiscono un report. L’immagine d’analisi caricata nella sessione non serve e non viene modificata.

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

## Opzioni e risultati

Le opzioni sono un oggetto JSON massimo 64 KiB. Campi sconosciuti/null, tipi errati, NUL incorporati e limiti non positivi vengono rifiutati.

| Opzione | Predefinito | Contratto |
|---|---|---|
| `backend` | `auto` | `auto`, `unicorn`, `kvm` o `whp` |
| `arguments` | Nome input | argv completo incluso argv[0]; vuoto usa il default |
| `environment` | `[]` | Stringhe guest esplicite; non eredita l’ambiente host |
| `instruction_limit` | 100000 | Tentativi d’istruzione ammessi condivisi |
| `event_limit` | 10000 | Eventi syscall, addebitati prima del servizio OS |
| `timeout_microseconds` | 5000000 | Deadline monotona avviata dopo il setup del processo |
| `memory_limit` | 67108864 | Budget di memoria fisica/mappata |
| `stack_size` | 1048576 | Stack allineato alla pagina entro il budget |
| `output_limit` | 1048576 | Byte complessivi catturati da stdout/stderr |
| `instruction_quantum` | 1024 | Intervallo d’ammissione prima di cedere alla runtime |

`schema_version` è 1. Il report contiene profilo, architettura, backend e motivo della scelta, `stop_reason`, `exit_status` nullable, diagnostica, PC di ingresso/corrente, contatori, record dei servizi e ultimo esito CPU tipizzato. Indirizzi, numeri syscall, registri argomento e bit di ritorno sono stringhe esadecimali **senza** `0x`; `stdout_hex`/`stderr_hex` preservano NUL e UTF-8 non valido. Un risultato syscall null significa nessun ritorno modellato (per esempio exit o richiesta non supportata), non zero riuscito.

## Semantica del profilo Linux

La policy OS riusa gli header di programma già decodificati dal loader ELF. Verifica tag ABI, allineamento segmenti, tabelle degli header mappate e limiti degli indirizzi user. Un piano generico controlla estensioni, permessi, sovrapposizioni e budget prima dell’allocazione e pubblica solo uno spazio privato completo. Conserva prefissi/code delle pagine file, azzera BSS, rispetta i permessi e riserva guard gap dello stack. Layout con pagine sovrapposte e header contraddittori sono rifiutati, non indovinati.

Lo stack iniziale contiene argc/argv/envp/auxv allineati, PHDR/PHENT/PHNUM, entry, dimensione pagina e identità. PID/TID/UID/GID modellati valgono 1000. `AT_RANDOM` contiene i primi 16 byte dello SHA-256 dell’input per riproducibilità; è una policy deterministica del modello, non entropia crittografica. HWCAP/HWCAP2 sono zero; non esiste vDSO.

Sono implementate `write`, `exit`, `exit_group`, `getpid` e `gettid`, con numeri distinti per [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) e [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h). Il ritorno di SYSCALL x64 applica i clobber RCX/R11, RAX e il PC successivo. ARM64 usa x8 per il numero e x0 per il risultato. Le altre chiamate si fermano come `unsupported_service`; non vengono eseguite syscall host.

I descrittori 1 e 2 sono sink virtuali di byte. `write` convalida pagine user leggibili, restituisce il prefisso leggibile se una pagina successiva non è accessibile ed `EFAULT` se nessun byte è leggibile. Un descrittore errato restituisce `EBADF`; una write di zero byte con descrittore valido non accede al puntatore. Atomicità delle pipe Linux e file non sono modellati. Il limite output ferma prima di pubblicare una scrittura eccedente.

## Verifica

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# In una build shared-library/CLI:
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

I test compilano originali ELF entry assembly e C per entrambe le ISA. Coprono dati/BSS, metadati d’avvio, errori syscall, output binario, permessi, write parziali, servizi non supportati e budget conservato tra quanti. Backend non disponibili sono skip espliciti. La suite pubblica attraversa C ABI e CLI e confronta report/codice d’uscita. Cross-compilazione e Unicorn ARM64 non provano KVM/WHP ARM64 nativo.

## PIE statico, TLS e verifica

Il PIE statico usa un load bias deterministico di almeno `0x40000000`, aumentato per rispettare l’allineamento `PT_LOAD` maggiore. Segmenti, PC d’ingresso e `AT_PHDR`/`AT_ENTRY` usano lo stesso bias; i program header originali restano invariati e `AT_BASE` vale zero senza interpreter. Il mapping usa i byte originali del file, non i fixup dell’analisi; il guest esegue autonomamente relocation e inizializzazione. Il loader decodifica `PT_DYNAMIC` da record limitati del file originale, senza dipendere dalle section. Se presente, la tabella deve essere leggibile, terminata e contenere al massimo 4096 entry. `PT_INTERP` e tag esterni di dipendenze/filter/audit sono rifiutati; non c’è dynamic linker, risoluzione simboli o esecuzione costruttori.

I template TLS statici `PT_TLS` sono validati come dati del loader: un solo template, estensioni file/memoria limitate, allineamento congruente e byte iniziali leggibili. L’avvio guest alloca e inizializza i blocchi TLS e installa il thread pointer; il modello Linux non inventa un TCB/DTV specifico di libc. Questo permette TLS local-exec generato dal compilatore nei programmi freestanding. TLS dinamico e thread OS restano fuori ambito.

Su x64, `arch_prctl` supporta `ARCH_SET_FS`, `ARCH_GET_FS`, `ARCH_SET_GS` e `ARCH_GET_GS`. Set accetta una base user anche non mappata; le dereference successive controllano comunque i permessi. Basi kernel restituiscono `EPERM` guest; destinazioni Get non valide restituiscono `EFAULT` senza fault CPU. Le altre operazioni falliscono esplicitamente. ARM64 installa `TPIDR_EL0` con `MSR`; `MRS`, accessi FS/GS e restore del contesto preservano il thread pointer fra quanti e ingressi backend. Non implementa uno scheduler.

Le fixture PIE/TLS verificano blocchi indipendenti allineati, BSS TLS, auxv rilocati e slot RELA originali a zero prima delle relocation guest. I test x64 controllano gli errori `arch_prctl` senza perdere la base precedente. Aggiungi `NeverDThreadPointerTests` ai comandi build/CTest sopra; backend assenti restano skip espliciti.
