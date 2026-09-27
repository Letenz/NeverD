**Lingue**: [English](../interpreter-recovery.md) | [简体中文](../zh-CN/interpreter-recovery.md) | [繁體中文](../zh-TW/interpreter-recovery.md) | [日本語](../ja/interpreter-recovery.md) | [한국어](../ko/interpreter-recovery.md) | [Français](../fr/interpreter-recovery.md) | [Deutsch](../de/interpreter-recovery.md) | [Español](../es/interpreter-recovery.md) | [Italiano](interpreter-recovery.md) | [Русский](../ru/interpreter-recovery.md) | [العربية](../ar/interpreter-recovery.md)

# Recupero del sorgente da interpreti

[← Indice della documentazione](README.md)

La fase sperimentale di specializzazione degli interpreti elimina il dispatch
risolto staticamente da una funzione x64 già collegata, conservandone gli input
a runtime, gli effetti sulla memoria, le diramazioni e i cicli. Usa la semantica
delle istruzioni, senza firme degli handler né tabelle di opcode specifiche di
un particolare sistema di protezione.

```sh
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --recovery-report recovery.json -o recovered.c
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --llvm -o recovered-llvm.c
```

`--vm-control` seleziona registri generali interi che distinguono i contesti
dell’interprete. Può essere ripetuto e non fornisce valori concreti. Si può
selezionare, per esempio, un cursore del bytecode il cui valore viene stabilito
dal codice d’ingresso. Un input a runtime usato come contatore deve restare
dinamico. Una separazione insufficiente dei contesti può interrompere il
recupero quando confluiscono valori diversi del cursore; il motore non deve
compensare indovinando una destinazione di dispatch. Per un cursore salvato
sullo stack, `--vm-control-stack=-16:8` seleziona otto byte a RSP d’ingresso meno
16. L’offset è relativo all’ingresso della funzione, non al puntatore dello
stack dopo le successive modifiche.

L’API C è `neverd_devirtualize_source_v1()`, dichiarata in
`neverd/sdk/NeverDCAPIDevirtualize.h`. Esegue una transazione separata senza
modificare la normale cache di decompilazione della sessione. Un errore non
restituisce sorgente, ma può comunque restituire una diagnosi JSON. Entrambe
le stringhe allocate si liberano con `neverd_free_string()`.

## Contratto di esecuzione

L’adattatore binario accetta attualmente immagini x64 ELF e PE già collegate,
agli indirizzi in cui sono mappate. Mappature, byte e permessi devono restare
invariati, senza modifiche concorrenti. Il lifting rigoroso su richiesta segue
il codice macchina raggiungibile. Istruzioni non supportate, chiamate,
operazioni opache, accessi alla memoria ordinati, controllo non risolto e
gestione delle eccezioni del linguaggio interrompono il recupero.

Solo intervalli completi di sola lettura, sostenuti dal file e privi di
mappature sovrapposte o correzioni del loader, possono fornire letture costanti
dell’immagine. Tabelle scrivibili, rilocazioni non risolte e istantanee campionate
durante l’esecuzione non provano l’immutabilità. Rilocazioni COPY e metadati
delle eccezioni incompleti vengono rifiutati in modo conservativo. Il dominio
ammesso richiede ritorni ABI ordinari: l’intervallo di destinazione di ogni
scrittura di origine esterna deve essere disgiunto dallo slot dell’indirizzo
di ritorno all’ingresso. È una precondizione esplicita per chiamante e ambiente,
anche per indirizzi calcolati da interi esterni; l’assenza di provenienza dal
frame dello stack non prova la disgiunzione numerica. Gli indirizzi di scrittura
derivati dal frame devono dimostrare tale disgiunzione e il puntatore dello
stack originale deve essere ripristinato al ritorno. Le informazioni di origine
sopravvivono ai salvataggi sullo stack e alle confluenze; perdere un’espressione
affine non la trasforma in un puntatore esterno. Pivot dello stack, ritorni che
rimuovono gli argomenti nella funzione chiamata e dispatch basato su RET sono
attualmente rifiutati. L’adattatore impone la semantica little-endian di x64.

Il risultato è sorgente e IR per l’analisi. Non dimostra la sicurezza di
rilocazione, unwinding, eccezioni asincrone o sostituzione binaria. La modalità
patch rifiuta questa opzione. Non si garantisce il supporto di ogni interprete
o configurazione di protezione.

## Limiti attuali

Gli indirizzi del bytecode dipendenti dagli input e le relazioni tra gli stati
del decoder non vengono risolti in generale. Una diramazione o un ciclo dinamico
può essere recuperato se le destinazioni di dispatch sono provate, ma la
copertura delle normali diramazioni non dimostra il supporto di ogni schema di
decodifica indiretta. Controllo non risolto e budget esauriti sono errori: non
vengono pubblicati né sorgente recuperato né sostituzioni parziali. Chiamate a
funzioni ausiliarie native, confini di eccezione e rientro, codice modificabile
e altre architetture restano fuori dal contratto di questo primo adattatore.

## Implementazione condivisa

`SpecializationProvider` fornisce istruzioni completamente sottoposte a lifting
e prove delle letture immutabili. `NeverDInterpreterSpecialization` usa la
semantica esistente di `SymExec` per valutare parzialmente le operazioni intere
e di controllo. L’adattatore binario gestisce mappature e decodifica; non
implementa un secondo valutatore delle istruzioni.

Un nodo è identificato dal cursore nativo, dalla modalità delle istruzioni e
dalle costanti selezionate dei registri di controllo e degli slot del frame
d’ingresso. Gli altri fatti a livello di byte si uniscono per intersezione.
Quando un fatto in ingresso si indebolisce, il nodo viene valutato nuovamente.
Così i cicli del programma restano cicli, senza espandere ogni iterazione
osservata. Tutte le foglie di una destinazione indiretta devono formare un
insieme esatto e limitato di costanti; le destinazioni selezionate diventano
confronti residui espliciti e archi del CFG.

Operazioni dinamiche e normali letture/scritture restano in LowIR. Solo le
letture immutabili certificate diventano costanti. Costanti scalari, puntatori
affini relativi al frame d’ingresso e byte del frame provati costanti possono
attraversare i nodi; le altre espressioni vengono scartate anziché espanse senza
limite. La memoria del frame usa l’invalidazione conservativa degli alias dello
stato simbolico esistente. Una scrittura tramite un puntatore sconosciuto che
potrebbe sovrapporsi invalida i fatti in conflitto. Non si presume che uno slot
dello stack sia privato né se ne eliminano gli effetti in base a un contratto
di assenza di alias non dimostrato.

Etichette sintetiche uniche delle istruzioni distinguono i contesti clonati.
I confini delle istruzioni originali restano in una mappa d’origine separata;
le certificazioni originali di rilocazioni, eccezioni e tabelle di salto non
vengono copiate sulle nuove occorrenze. Il LowIR recuperato entra nella normale
conversione LowIR-MedIR prima della separazione tra HighC e LLVM, condividendo
la gestione di registri, stack, CFG, SSA e ABI. Il percorso HighC richiede anche
che la verifica MedIR riesca.

Budget per nodi, contesti per indirizzo, operazioni, valutazioni dei nodi e
destinazioni finite limitano l’analisi. Se un budget si esaurisce o la semantica
non è supportata, non viene pubblicata alcuna funzione residua. Un grafo di
controllo completo è distinto dalla corretta emissione del sorgente; l’API
pubblica verifica entrambi i risultati e ne segnala la differenza.

## Evidenze e test

Il report JSON locale facoltativo include hash dell’input, controlli scelti,
budget, stato, contatori del lavoro, numero di blocchi residui, posizioni delle
istruzioni originali e byte immutabili usati nel recupero. Contiene informazioni
derivate dall’input e viene scritto solo nel percorso locale richiesto.

I test pubblici usano macchine originali con dispatch tramite registri e stack,
ciascuna con programmi aritmetici, diramazioni con confluenze e cicli a runtime.
Un oracolo indipendente senza segno verifica ritorni, scritture in memoria,
riporti/prestiti e sentinelle di uscita. HighC e LLVMC recuperati vengono compilati
a O0/O2 con trap per comportamento indefinito ed eseguiti contro l’oracolo.
I casi negativi coprono dispatch non risolto o scrivibile, ordine dei byte
incompatibile, metadati delle eccezioni e budget.

I target di test mirati sono descritti in [testing.md](testing.md).
