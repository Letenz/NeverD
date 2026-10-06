**Lingue**: [English](../unpack.md) | [简体中文](../zh-CN/unpack.md) | [繁體中文](../zh-TW/unpack.md) | [日本語](../ja/unpack.md) | [한국어](../ko/unpack.md) | [Français](../fr/unpack.md) | [Deutsch](../de/unpack.md) | [Español](../es/unpack.md) | [Italiano](unpack.md) | [Русский](../ru/unpack.md) | [العربية](../ar/unpack.md)

[← Indice della documentazione](README.md)

# Spacchettamento degli eseguibili compressi

`neverd unpack` recupera il programma che un eseguibile compresso ricostruisce nel proprio spazio di indirizzi. Esegue l'input come processo ospite limitato, osserva dove lo stub cede il controllo al codice che ha prodotto e scrive quell'immagine come nuovo file dello stesso contenitore. Non devirtualizza: le funzioni che un protettore ha virtualizzato restano virtualizzate. Compilare con `NEVERD_ENABLE_CPU_EMULATION=ON`.

## Input supportati

Il contenitore determina come un file viene validato e ricostruito, il set di istruzioni determina come viene giudicato un trasferimento, ed entrambi determinano il profilo di processo ospite. Un input fuori da questa tabella viene rifiutato per nome prima che qualsiasi cosa venga eseguita.

| Contenitore (`format`) | Set di istruzioni | Profilo ospite | Conoscenza dello stub |
| --- | --- | --- | --- |
| PE32+ (`pe64`) | x86-64 | [`windows-pe64-v1`](process-emulation.md) | UPX |
| PE32+ (`pe64`) | ARM64 | [`windows-pe64-v1`](process-emulation.md) | nessuna; sola osservazione |

## Uso

```bash
neverd unpack packed.exe -o unpacked.exe
neverd unpack packed.exe -o unpacked.exe \
  --options='{"backend":"unicorn","instruction_limit":400000000,"transfer":2}'
```

Il comando stampa un rapporto JSON. Il codice di uscita 0 indica che l'immagine è stata scritta, 3 che l'esecuzione limitata è terminata prima che un ingresso fosse accettato (`outcome` vale `no_entry` e non viene scritto nulla), e 1 un input, un'opzione o una preparazione non validi. Il rapporto indica il `format`, l'`architecture` e il `profile` effettivamente eseguiti. Il punto di ingresso C è `neverd_unpack_json`; Python espone `Session.unpack`. Le opzioni sono le [opzioni di processo](process-emulation.md) più `transfer`. I valori predefiniti differiscono dove uno stub richiede più risorse: 100000000 istruzioni, 600 secondi e 512 MiB, e `windows.defer_unmodeled` è attivo.

## Come viene stabilito l'ingresso

La generazione zero è l'immagine così come il caricatore ospite l'ha mappata. Un'istruzione i cui byte differiscono da quell'immagine è stata generata dal processo. Un trasferimento è la prima esecuzione di codice più recente di quello che era in esecuzione; `transfers` elenca ciascuno con la sua RVA, la sua `generation` e se il puntatore dello stack è uguale al suo valore all'ingresso del processo (`stack_balanced`).

1. Un trasferimento sullo stack di ingresso è l'ingresso del programma: lo stub ha restituito lo stack che aveva ricevuto. `entry_source` vale `transfer`.
2. Un trasferimento su uno stack più profondo è una chiamata che lo stub fa al programma, per esempio una callback TLS. Viene segnalato, ma non accettato. Quando lo stub identificato nomina la destinazione del suo salto finale, l'immagine viene ricostruita a quella chiamata, prima che sia stato eseguito qualsiasi codice del programma, e l'indirizzo nominato è l'ingresso. `entry_source` vale `stub`.
3. `transfer` seleziona esplicitamente un trasferimento dell'elenco in base alla posizione, per i protettori che spacchettano a stadi o che chiamano il proprio programma.

Nessun ingresso viene dedotto dalla forma del codice di avvio di un compilatore. Un'esecuzione che si ferma prima segnala `no_entry` con lo `stop_reason` del processo.

## L'immagine ricostruita

Le sezioni mantengono le loro RVA e contengono la memoria osservata; ogni sezione ha l'accesso che le sue pagine avevano al momento del trasferimento. Una sezione finale `.neverd` contiene una nuova directory di importazione sopra le celle attraverso le quali il programma già chiama, così né codice né dati si spostano. `imports` elenca ogni cella con la sua `origin`: le celle `static` sono state collegate dal caricatore a partire dalla directory dell'input stesso, le celle `runtime` sono state scritte dallo stub. L'immagine è fissata alla base osservata: non sono state osservate rilocazioni del contenuto generato, quindi la directory di rilocazione viene rimossa e viene impostato `IMAGE_FILE_RELOCS_STRIPPED`. Per UPX viene indicata di nuovo la directory TLS propria del programma, perché quella compressa raggiunge soltanto il gestore dello stub.

La tabella delle sezioni originale, la disposizione della directory di importazione e la tabella di rilocazione non vengono ricostruite; un compressore non le ripristina in memoria.

## Identificazione

`packer.kind` nomina un protettore solo in base a prove presenti nel file. UPX ne richiede due fra `upx_section_names`, `upx_pack_header` (numero magico, formato, metodo e checksum) e `upx_entry_stub`. Un input non identificato viene comunque spacchettato tramite osservazione.

## Limiti

Sono supportati solo eseguibili; le DLL non vengono eseguite. L'esecuzione controllata ammette un'istruzione alla volta, nell'ordine di 10^5 al secondo, quindi uno stub che richiede miliardi di istruzioni supera qualsiasi budget realistico. L'esecuzione è tracciata per pagina da 4 KiB: il codice scritto in una pagina che sta già eseguendo codice della stessa generazione non viene segnalato come trasferimento. Il codice del programma che gira prima dell'ingresso, per esempio una callback TLS che chiama un'API non modellata, ferma l'esecuzione a meno che lo stub dichiari il proprio ingresso. I caricatori VMProtect non sono ancora supportati.

## Verifica

`NeverDUnpackTests` controlla l'identificazione. `NeverDUnpackExecutionTests` spacchetta campioni UPX inclusi nel repository (NRV2B, NRV2D, NRV2E, LZMA e un programma con runtime C) su Unicorn, KVM e WHP, confronta ogni sezione con l'originale, esegue l'immagine recuperata e richiede byte identici da ogni backend. `UnpackGeneratedTests.cpp` comprime un programma all'interno del test per x86-64 e ARM64 e controlla, rispetto al file collegato, un caricatore che esce direttamente, un caricatore che prima chiama il programma e un caricatore a due stadi. `NeverDUnpackPublicTests` copre l'ABI C e la CLI. `unittests/unpack/fixtures/Makefile` rigenera i campioni UPX.
