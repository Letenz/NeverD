**Lingue**: [English](../unpack.md) | [简体中文](../zh-CN/unpack.md) | [繁體中文](../zh-TW/unpack.md) | [日本語](../ja/unpack.md) | [한국어](../ko/unpack.md) | [Français](../fr/unpack.md) | [Deutsch](../de/unpack.md) | [Español](../es/unpack.md) | [Italiano](unpack.md) | [Русский](../ru/unpack.md) | [العربية](../ar/unpack.md)

[← Indice della documentazione](README.md)

# Spacchettamento degli eseguibili compressi

`neverd unpack` recupera il programma che un eseguibile compresso ricostruisce nel proprio spazio di indirizzi. Esegue l'input come processo ospite limitato, osserva dove lo stub cede il controllo al codice che ha prodotto e scrive quell'immagine come nuovo file dello stesso contenitore. Non devirtualizza: le funzioni che un protettore ha virtualizzato restano virtualizzate. Compilare con `NEVERD_ENABLE_CPU_EMULATION=ON`.

## Input supportati

Il contenitore determina come un file viene validato e ricostruito, il set di istruzioni determina come viene giudicato un trasferimento, ed entrambi determinano il profilo di processo ospite. Un input fuori da questa tabella viene rifiutato per nome prima che qualsiasi cosa venga eseguita.

| Contenitore (`format`) | Set di istruzioni | Profilo ospite | Prova dell’ingresso |
| --- | --- | --- | --- |
| PE32+ (`pe64`) | x86-64 | [`windows-pe64-v1`](process-emulation.md) | osservazione durante l’esecuzione |
| PE32+ (`pe64`) | ARM64 | [`windows-pe64-v1`](process-emulation.md) | osservazione durante l’esecuzione |

Gli input DLL PE32+ sono identificati da `IMAGE_FILE_DLL`. Un EXE guest modellato chiama `LoadLibraryA` e poi `FreeLibrary`, usando il normale ciclo di dipendenze, TLS e `DllMain`. L’ingresso DLL accettato è la chiamata di collegamento al processo; non si inventano argomenti per export arbitrari. Nomi, ordinali, alias, dati e inoltri sono conservati. I puntatori agli export propri rimangono interni, senza autoimportazioni.

## Uso

```bash
neverd unpack packed.exe -o unpacked.exe
neverd unpack packed.exe -o unpacked.exe \
  --options='{"backend":"unicorn","instruction_limit":400000000,"transfer":2}'
```

Il comando stampa un rapporto JSON. Il codice di uscita 0 indica che l'immagine è stata scritta, 3 che l'esecuzione limitata è terminata prima che un ingresso fosse accettato (`outcome` vale `no_entry` e non viene scritto nulla), e 1 un input, un'opzione o una preparazione non validi. Il rapporto indica il `format`, l'`architecture` e il `profile` effettivamente eseguiti. Il punto di ingresso C è `neverd_unpack_json`; Python espone `Session.unpack`. Le opzioni sono le [opzioni di processo](process-emulation.md) più `transfer`. I valori predefiniti differiscono dove uno stub richiede più risorse: 100000000 istruzioni, 600 secondi e 512 MiB, e `windows.defer_unmodeled` è attivo.

## Come viene stabilito l'ingresso

La generazione zero è l’immagine di input quando il loader guest la mappa. Un trasferimento avvia codice più recente. `transfers` registra RVA, `generation`, uguaglianza dello stack (`stack_balanced`) e appartenenza all’ingresso dell’input (`program_invocation`). Per DLL è il collegamento al processo. Prima di tale chiamata si usa lo stack del primo inizializzatore.

1. L’ingresso predefinito richiede `stack_balanced` e `program_invocation`: lo stub ha restituito lo stack dell’invocazione di ingresso. `entry_source` è `transfer`.
2. Callback TLS del sistema operativo, ingressi delle DLL dipendenti e chiamate di scollegamento non diventano l’ingresso predefinito, nemmeno con stack bilanciato. Le chiamate guest più profonde proseguono. Nessun callback viene saltato e nessuna firma dello stub predice l’ingresso.
3. `transfer` seleziona esplicitamente una posizione dell’elenco, inclusi callback d’inizializzazione e trasferimenti intermedi.

Nessun ingresso viene dedotto dalla forma del codice di avvio di un compilatore. Un'esecuzione che si ferma prima segnala `no_entry` con lo `stop_reason` del processo.

Una pagina di 4 KiB eseguita resta osservabile. Le scritture RAM guest confermate invalidano le pagine visitate tramite alias fisici; il modello di processo le ricontrolla anche dopo i servizi OS, prima della ripresa. Le prove del trasferimento confrontano la lunghezza effettiva fornita dal decoder CPU: i dati adiacenti modificati non bastano a stabilire un ingresso. Generazioni miste e riscritture restano sorvegliate, inclusi callback che tornano allo stub precedente nella stessa pagina.

## L'immagine ricostruita

Le sezioni conservano RVA, memoria osservata (compresi gli effetti delle inizializzazioni già eseguite) e permessi delle pagine. La nuova sezione `.neverd` contiene la directory degli import e nuove celle IAT per chiamate esportate e caricamenti di indirizzi, preservando lo spazio originale azzerato. `origin` in `imports` distingue le celle `static` collegate dall’input e le `runtime` scritte dall’ospite o aggiunte per la riparazione. Poiché le rilocazioni del contenuto generato non sono state osservate, l’immagine resta alla base osservata, la directory viene rimossa e si imposta `IMAGE_FILE_RELOCS_STRIPPED`.

I candidati IAT esistenti devono formare un array contiguo di puntatori a esportazioni dello stesso fornitore con un terminatore nullo integro nella sezione originale. Il terminatore del fornitore vicino non è sufficiente.

Il recupero TLS usa l’identità dell’allocazione del loader, una lista completa di callback terminata da zero e chiamate osservate al codice generato. Più candidati causano un errore esplicito. Non si usano nomi o byte specifici del protettore. Nella scelta della directory, l’esecuzione osservata di callback generati ha precedenza sul completamento di un inizializzatore del caricatore. Una nuova chiamata del programma a un inizializzatore del loader già completato resta una prova di riserva, anche se tale inizializzatore è stato riscritto.

`materialized_tls_callbacks` conta i callback TLS di collegamento dell’immagine di input tornati prima della cattura. La prova richiede un record di chiamata OS oppure un ingresso osservato nel codice generato con `(image_base, 1, 0)`, seguito dall’indirizzo di ritorno ABI e dal puntatore di stack ripristinato. Gli effetti in memoria sono già nello snapshot. Gli adattatori nella nuova sezione `.neverd` evitano soltanto quella chiamata ripetuta e inoltrano le altre notifiche al callback originale con una chiamata terminale. Directory, tabella e adattatori usano nuovo spazio, preservando i byte originali. La sezione è eseguibile con gli adattatori e scrivibile soltanto se servono nuove celle IAT. I callback interni privi di questa prova mantengono il comportamento originale. All’avvio del processo, il primo adattatore ripristina anche il TLS acquisito del thread principale se differisce dal modello. L’assenza di prove TLS o una dimensione incoerente causa un errore esplicito. Il modello originale resta disponibile per i thread futuri; le altre notifiche non ripristinano il blocco acquisito.

Il caricamento differito ammette destinazioni eseguibili di callback o ingresso in memoria inizialmente azzerata, il cui codice viene prodotto da inizializzatori precedenti. Gli array di callback e i metadati di allocazione TLS richiedono ancora contenuti di file convalidati; il caricamento rigoroso mantiene i controlli. Il modello del sistema operativo indica l’appartenenza dell’invocazione e avvisa l’osservatore quando prepara una chiamata o ripristina un chiamante sospeso. Le sorveglianze vengono riattivate a questi confini, anche se callback e ingresso generato condividono una pagina.

`import_repair` comprende due esecuzioni limitate aggiuntive dopo la cattura dell’ingresso: scoperta delle chiamate esportate e osservazione dello stato delle routine. Ogni esecuzione ha limiti propri. I contatori di istruzioni ed eventi sono la somma saturata; `stop_reason` e diagnostica descrivono l’ultima, mentre le chiamate osservate contano solo la scoperta. Senza continuazioni si omette l’osservazione. Identità di export in conflitto impediscono la riscrittura. Si coprono solo percorsi raggiunti; `unpacked` non certifica tutti gli import o il successo del programma.

L’ordine di associazione degli import può cambiare gli indirizzi delle esportazioni. La riparazione conserva l’identità dell’esecuzione che ha prodotto la prova e osserva le esportazioni risolte dopo l’ingresso; un indirizzo di un’altra esecuzione non autorizza la riscrittura.

La scoperta osserva la gestione degli export, incluso quello non modellato che arresta l’esecuzione, senza dipendere dai soli log delle chiamate modellate. Un ritorno ABI illeggibile non può fondare una prova. Riparare una chiamata pura a un export opaco conserva l’arresto esplicito per servizio non supportato.

Si osservano al massimo 256 inizi candidati nei 256 byte precedenti continuazioni reali di chiamate esportate. `CALL rel32`, con eventuale PUSH/POP GPR precedente, identifica solo un confine; i byte successivi non sono una firma. Una finestra di sei-otto byte diventa `call [rip+IAT]` solo con esattamente un indirizzo di ritorno sullo stack all’ingresso API, altri registri (flag e SIMD inclusi), mappature e RAM persistente invariati e nessuna chiamata OS intermedia. NOP iniziali conservano il ritorno esatto. Sette-otto byte diventano `mov r64, [rip+IAT]` se restituiscono un export noto in un solo GPR, con stack bilanciato e gli stessi vincoli. Il registro risultato deriva dal confronto degli stati: tutti i GPR salvo RSP, inclusi R8-R15; otto byte lasciano un NOP finale. Si esclude solo lo spazio temporaneo sotto lo SP chiamante; il suo stack viene confrontato. Invocazioni successive impure, irrisolte o incomplete invalidano la prova. Serve l’inizio eseguito; nessun REX è dedotto dal byte precedente e inizi sovrapposti con lo stesso ritorno sono rifiutati. `observed_loads` e `repaired_loads` contano i caricamenti separatamente.

`ImportObserver` mantiene un solo snapshot esterno durante gli arresti intermedi. Un candidato annidato perde le prove precedenti perché non dispone di uno snapshot separato. La chiamata esterna richiede comunque il confronto completo di registri, mappature, memoria persistente e chiamate OS. Le invocazioni ricorsive, interrotte o incomplete perdono le proprie prove. `ProcessImportsTests.cpp` e la DLL di test collegata indipendentemente verificano questi confini. Solo un candidato con una continuazione di esportazione osservata conserva lo snapshot attraverso gli arresti annidati.

La tabella delle sezioni originale, la disposizione della directory di importazione e la tabella di rilocazione non vengono ricostruite; un compressore non le ripristina in memoria.

## Identificazione

I campi compatibili `packer.kind` e `packer.evidence` restituiscono `unidentified` e un array vuoto. Registro dei protettori, parser delle intestazioni compresse e firme degli stub sono rimossi. La vecchia API convalida soltanto il contenitore.

## Limiti

L’esecuzione controllata procede per istruzione. x64 Unicorn/KVM/WHP offre anche `direct-user-x64-v1`, limitato da tempo ed eventi senza contare le istruzioni. Generazioni miste e scritture ripetute possono richiedere ulteriori passi del processore. Il recupero copre i percorsi raggiunti nell’immagine di input; il codice generato fuori non diventa il suo ingresso. L’inizializzazione precedente all’ingresso può creare stato esterno non trasferibile e rieseguire callback TLS può avere altri effetti. Le API non modellate arrestano esplicitamente l’esecuzione. La riparazione copre finestre di chiamata x64 convalidate di sei-otto byte e caricamenti di indirizzi di sette-otto byte; altre forme e percorsi non raggiunti restano irrisolti. Il codice virtualizzato resta tale.

## Verifica

`NeverDUnpackTests` verifica contenitori, allocazione sicura IAT, conflitti e rifiuti TLS. `NeverDUnpackExecutionTests` prova UPX NRV2B/NRV2D/NRV2E/LZMA e CRT sullo stesso percorso generico in Unicorn/KVM/WHP, confronta le sezioni con il programma collegato indipendente dopo le proprie inizializzazioni e richiede output identici dai backend disponibili. `UnpackGeneratedTests.cpp` genera programmi indipendenti x86-64/ARM64 per trasferimenti, caricamento a stadi, import ed esecuzione diretta x64 Unicorn/KVM/WHP. I casi nativi obbligatori non possono essere saltati in CI. `NeverDUnpackPublicTests` copre ABI C e CLI. `unittests/unpack/fixtures/Makefile` rigenera gli esempi UPX.


`UnpackLibraryTests.cpp` comprime DLL x64/ARM64 indipendenti nel test e verifica ordine delle dipendenze, callback TLS normali/generati, pulizia dopo fallimento, identità input/host, accesso al proprio file, nomi/ordinali/dati/inoltri e assenza di autoimportazioni. Windows nativo carica DLL originali e ricostruite con un EXE separato e chiama gli export dichiarati; i casi WHP controllati e diretti sono obbligatori. `CompletedGeneratedTLSCallsRequireTheAttachABI` rifiuta ingressi/argomenti cambiati; `GeneratedCallsNeedTheirReturnedStackAtTheContinuation` rifiuta uno stack di ritorno errato. Si verifica lo spacchettamento, senza devirtualizzazione.

`ExportObserver` osserva anche export eseguibili delle dipendenze guest residenti; i provider modellati restano osservati al dispatch dei servizi. Gli export dell’input sono esclusi. Le modifiche ai moduli aggiornano gli arresti e ogni riparazione richiede l’identità attuale. I record rispettano il limite dichiarato degli import. I test DLL riparano helper API e di dipendenze; il caricamento nativo verifica l’assenza di indirizzi emulati residui.


`WrappedEntriesRequireExplicitTransferEvidence` copre un wrapper DLL che chiama l’ingresso ripristinato con stack più profondo. Il risultato predefinito resta `no_entry`; selezionare la chiamata osservata con `transfer` ricostruisce una DLL caricabile. La sola profondità non distingue ingresso e inizializzatore.