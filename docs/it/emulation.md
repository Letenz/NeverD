**Lingue**: [English](../emulation.md) | [简体中文](../zh-CN/emulation.md) | [繁體中文](../zh-TW/emulation.md) | [日本語](../ja/emulation.md) | [한국어](../ko/emulation.md) | [Français](../fr/emulation.md) | [Deutsch](../de/emulation.md) | [Español](../es/emulation.md) | [Italiano](emulation.md) | [Русский](../ru/emulation.md) | [العربية](../ar/emulation.md)

<!-- i18n-source: a3e64122b77a690dd856d02f5b2af53973d3bf1affd973bea5f9735aa9dd6722 -->

[← Indice della documentazione](README.md)

# Esecuzione CPU e ambienti guest

<!-- i18n-section: backends -->

## Backend CPU e carichi di lavoro

L’esecuzione CPU separa ammissione ISA, memoria guest, trasporto del backend e politiche OS. `NEVERD_ENABLE_CPU_EMULATION` attiva il livello CPU x64/ARM64; `NEVERD_ENABLE_DRIVER_EMULATION` aggiunge l’ambiente Windows WDM/KMDF x64 limitato. `linux-elf64-v1` esegue processi Linux ELF supportati. Vedere [Esecuzione CPU](cpu-execution.md), [Emulazione dei processi guest](process-emulation.md) e [Emulazione dei driver Windows](driver-emulation.md).

Per i contratti nativi supportati, `auto` sceglie KVM su Linux, WHP su Windows o [HVF su macOS](macos-hvf.md), con la stessa ISA per ospite e host. ISA diverse usano Unicorn; `software-cpu-v1` e l’API V1 originale mantengono l’esecuzione software. Un backend esplicitamente scelto ma indisponibile fallisce senza ripiego. L’esecuzione nativa verifica istruzioni ammesse, indirizzi ed effetti prima dell’ingresso. La virtualizzazione dell’host non determina il sistema ospite: i [profili Darwin](darwin-emulation.md) modellano separatamente macOS, iOS e iOS Simulator. HVF richiede il diritto `com.apple.security.hypervisor`.

`driver-strict` / `checked-x64-v1` copre esecuzione x64 limitata; il caricamento dei driver Windows resta x64. `checked-aarch64-v1` e `checked-user-aarch64-v1` includono ARM64 FP32/FP64 limitato, SIMD fisso e stato completo FPCR/FPSR/vettoriale. La guida Mac documenta l’accettazione nativa ARM64 HVF; restano da completare la verifica dei carichi ARM64 KVM/WHP e l’accettazione completa Intel HVF. Il supporto CPU non implica compatibilità con qualsiasi driver o applicazione.

<!-- i18n-section: windows-processes -->

## Processi e moduli Windows

`windows-pe64-v1` supporta processi console Windows x64/ARM64 limitati con PEB/TEB, TLS statico e dinamico, `DllMain`, API Win32 nominate e grafi DLL espliciti aciclici. I moduli supportano import di codice/dati per nome o ordinale, DIR64, export inoltrati e identità reali del loader. `LoadLibraryA` / `LoadLibraryW`, `FreeLibrary` e `GetProcAddress` usano il catalogo configurato. CRT/GUI, SEH utente ARM64 basato sui frame, thread e compatibilità Windows generale restano incompleti; mancano prove native ARM64 KVM/WHP.

`WindowsSystemModules` costruisce immagini modello PE64 limitate per `ntdll.dll`, `kernelbase.dll` e `kernel32.dll` su entrambe le ISA. Le ricerche ASCII `GetModuleHandleA` / `GetModuleHandleW`, `LoadLibraryA` / `LoadLibraryW` e `GetProcAddress` condividono le basi mappate; PEB/LDR e `MEM_IMAGE` descrivono le stesse immagini. Importazioni statiche, ricerche per nome e inoltri guest usano gli stessi ingressi API e risolutore degli export. I fornitori restano residenti, senza callback guest di inizializzazione, e non impediscono il ritorno dall’ingresso dopo lo scaricamento delle normali DLL guest. Modifiche a intestazioni o metadati di export interrompono la ricerca. Nomi di sistema non modellati e ordinali non nulli arrestano esplicitamente l’esecuzione; differenze di maiuscole nei nomi modellati e nomi vuoti restituiscono 127, una ricerca NULL restituisce 87. Byte e indirizzi generati sono regole del modello; layout delle versioni Windows, ordinali nativi e alias tra fornitori non sono ricostruiti. `WindowsSystemTests.cpp` confronta EXE originali x64/ARM64 con Windows nativo, incluse otto osservazioni indipendenti del ritorno del thread iniziale.

<!-- i18n-section: environment-memory -->

## Ambiente e memoria

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` condividono il blocco ambiente corrente del guest nei parametri di processo del PEB. I nomi ASCII ignorano maiuscole e minuscole; i valori sono UTF-16. Le modifiche verificano input, capacità e permessi di scrittura prima della pubblicazione. Le istantanee restano indipendenti dalle modifiche successive e rilasciano la memoria guest. Il modello limita il blocco a 64 KiB; stringhe ed espansioni hanno limiti e controllano la scadenza. Puntatori di proprietà sconosciuta, blocchi malformati, pagine di codice ANSI e buffer di espansione sovrapposti restano non supportati. `WindowsEnvironmentTests.cpp` confronta fixture originali x64/ARM64 sui backend disponibili; la CI richiede un oracolo Windows nativo indipendente.

`WindowsProcessHeap` unifica allocazione, `HeapReAlloc`, rilascio e interrogazione delle dimensioni dello heap del processo. Il ridimensionamento conserva i byte mantenuti; `HEAP_ZERO_MEMORY` azzera quelli aggiunti e `HEAP_REALLOC_IN_PLACE_ONLY` impedisce lo spostamento. Il ridimensionamento fallito conserva il vecchio blocco e restituisce NULL con `ERROR_NOT_ENOUGH_MEMORY` (8), come nelle osservazioni native. Le pagine indipendenti restituiscono capacità durante riduzione e rilascio; crescita preparata e copie limitate verificano la scadenza. Heap personalizzati, flag di eccezione, proprietà sconosciuta e intervalli inaccessibili arrestano esplicitamente l’esecuzione. `WindowsHeapTests.cpp` copre entrambe le ISA, spostamento forzato, riuso del budget e atomicità degli errori; CI esegue lo stesso EXE originale su Windows nativo.

La memoria virtuale Windows aggiunge `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` e `FlushInstructionCache` per il processo corrente. Il livello OS gestisce le prenotazioni; `AddressSpace` resta responsabile delle pagine impegnate, dei permessi e della memoria sottostante. I test verificano modifiche al codice, errori di accesso e riutilizzo del budget di memoria.

`WriteProcessMemory` segue il comportamento delle pagine impegnate osservato su x64/ARM64 per scritture fino a 4 KiB nel processo corrente. Conserva la protezione di ogni regione, i prefissi copiati, i conteggi dei byte e LastError, inclusi `ERROR_NOACCESS`, `ERROR_PARTIAL_COPY` e il successo dopo un prefisso RX. `WindowsMemoryWriteTests.cpp` controlla tutte le 25 coppie di protezioni; `check_windows_memory_write.py` verifica lo stesso eseguibile originale nella CI Windows nativa. Le destinazioni non impegnate restano esplicitamente non supportate.

<!-- i18n-section: vectored-exceptions -->

## Eccezioni vettoriali e continuazione

`WindowsProcessExceptions` implementa `AddVectoredExceptionHandler`, `RemoveVectoredExceptionHandler` e `RaiseException` sulla stessa CPU e con il budget del processo. I gestori ordinati possono modificare le registrazioni, sollevare eccezioni annidate, chiamare API modellate, caricare DLL e terminare il processo. Le violazioni di accesso ai dati x64/ARM64 e le divisioni intere x64 riprendono dopo la convalida delle modifiche guest a `CONTEXT`, preservando registri generali, SIMD e stato FP supportato. Le eccezioni software riprendono da una vera istruzione di ritorno nel fornitore modellato. Limiti: 128 registrazioni conservate e 16 frame annidati. Risultati non validi, puntatori modificati, campi non supportati e superamenti falliscono esplicitamente. SEH/unwinding ARM64 basato sullo stack, debugger ed errori di esecuzione/guardia restano esclusi. `WindowsExceptionTests.cpp` confronta EXE/DLL originali con Windows nativo; mancano ancora prove native ARM64 KVM/WHP. I record delle eccezioni software includono `EXCEPTION_SOFTWARE_ORIGINATE` (`0x80`), indipendentemente dal flag di non continuabilità del chiamante; l’eseguibile Windows originale verifica i valori esatti dei flag delle eccezioni software e hardware.

`AddVectoredContinueHandler` e `RemoveVectoredContinueHandler` gestiscono un elenco ordinato separato e condividono con i gestori delle eccezioni il limite di 128 registrazioni conservate. Quando un gestore vettorizzato accetta la ripresa, i callback di continuazione ricevono lo stesso record modificabile e `CONTEXT`. La convalida finale avviene dopo questi callback, comprese le eccezioni annidate e le notifiche DLL. Gli handle non possono essere rimossi tramite l’altra famiglia di gestori. `WindowsContinuationTests.cpp` confronta EXE originali con Windows nativo per ordine, arresto anticipato, modifiche alle registrazioni, riparazione del contesto, annidamento, callback del caricatore e uscita del processo. Il percorso vettorizzato verificato su Windows x64 consente la ripresa con `EXCEPTION_NONCONTINUABLE`; ciò non dimostra il comportamento SEH basato sullo stack. L’esecuzione ARM64 nativa resta non verificata.

<!-- i18n-section: caller-context -->

## Contesto del chiamante

`RtlCaptureContext` è disponibile tramite `kernel32.dll` e `ntdll.dll` per x64 e ARM64. I componenti condivisi `WindowsProcessContext` e `IntegerABI` salvano PC/SP del chiamante senza modificare lo stato CPU o LastError. Le osservazioni native di Windows confermano i flag x64 `0x10000f`, la conservazione delle aree home/debug/vettori non scritte e i campi storici degli indirizzi x87 a 32 bit; ARM64 copia LR in PC e azzera X0/LR nel record. Registri, SIMD e controlli floating point provengono dal guest; selettori x64 e maschera delle capacità MXCSR seguono la CPU guest configurata. Destinazioni non valide, non allineate o parzialmente inaccessibili falliscono prima della scrittura. `WindowsContextTests.cpp` verifica import diretti, ricerche dei provider, callback VEH, output tra pagine e atomicità degli errori. `scripts/check_windows_context.py` esegue il programma originale su Windows x64/ARM64, con un oracolo nativo separato per lo stato x87 non vuoto. Queste osservazioni ARM64 non provano l’esecuzione nativa KVM/WHP. Ripristino del contesto, analisi dello stack e tabelle di funzioni dinamiche restano attività distinte. `WindowsProcessServices.def` dichiara vincoli precisi per modulo: la ricerca in `kernelbase.dll` restituisce `ERROR_PROC_NOT_FOUND` (127), come nelle osservazioni native, senza inventare un export. [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

<!-- i18n-section: structured-exceptions -->

## Gestione strutturata delle eccezioni

`WindowsProcessSEH` usa lo `X64SEH` condiviso in `os/windows/exception/` (`NeverDEmulationWindowsException`, disponibile senza driver) per x64 `__C_specific_handler` e UNWIND_INFO V1. Dopo la ricerca VEH supporta filtri, finally, trasferimenti non locali, eccezioni annidate/unwinding in collisione e frame EXE/DLL rilocati, preservando GPR/XMM non volatili. La continuazione tramite filtro esegue VCH sullo stesso `CONTEXT`. `WindowsSEHTests.cpp` confronta 23 scenari originali con Windows nativo; KVM/WHP/Unicorn condividono la semantica. Il budget del processo comprende la riconvalida di generazioni, header, byte di unwinding/ambiti, regioni del gestore di linguaggio e associazioni IAT. Metadati modificati o immagini conservate scaricate falliscono esplicitamente. SEH ARM64 basato sui frame, C++ EH, tabelle dinamiche, RtlUnwind/NtContinue generali e unwinding oltre callback del loader/VEH/VCH restano esclusi.

Per `EXCEPTION_NONCONTINUABLE`, un filtro x64 che restituisce `EXCEPTION_CONTINUE_EXECUTION` genera `STATUS_NONCONTINUABLE_EXCEPTION` (`0xc0000025`, flag `0x81`, record collegato nullo) con un nuovo contesto. VEH viene eseguito nuovamente prima della ricerca nello stack logico conservato, mantenendo l’ordine finally, l’identità dei frame EXE/DLL e gli stessi budget di profondità ed esecuzione. I 23 scenari nativi comprendono 21 esecuzioni riuscite e due terminazioni: accettare in VEH/VCH la continuazione di questa eccezione secondaria la lascia non gestita anche ripristinando il `CONTEXT` originale. Il modello segnala un errore di esecuzione. Gli indirizzi delle eccezioni software coincidono con il PC salvato; indirizzi del dispatcher interno e disposizione dei registri sono scelte del modello. [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

<!-- i18n-section: processor-state -->

## Istruzioni e stato del processore

Il profilo x64 verificato include `MOVS/STOS/LODS` sulla RAM ordinaria e `CLD/STD`, con ripresa, annullamento e verifica delle pagine per elemento. I bit alti a conteggio nullo specifici della CPU e gli operandi di dispositivo STOS/LODS restano fuori dal contratto.

Il profilo x64 verificato supporta anche `CMPS/SCAS` sulla RAM ordinaria con `REPE/REPNE`, flag aritmetici, uscita anticipata, arresti per elemento e ripresa dopo errore. I confronti su dispositivi restano esclusi.

Le sonde native x64 e ARM64 verificano esecuzione completa limitata con diritto esclusivo sulla memoria. Pacchetti XSAVE e cache di tabelle identificate per ISA hanno un’autorità unica.

I campi x64 nativi `FOP/FIP/FDP` seguono le regole di salvataggio/ripristino dell’host: AMD può azzerare metadati x87 inattivi. Le sonde di avvio li verificano con un’eccezione pendente non mascherata.
