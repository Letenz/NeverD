# Scheduling preemptive dei driver Windows

L’oggetto facoltativo `scheduling` attiva la prelazione deterministica su CPU0 per driver Windows x64 con Unicorn, KVM o WHP. Se omesso, mantiene lo scheduling cooperativo e il tempo virtuale che avanza solo durante l’inattività. Entrambi i campi sono interi positivi e il prodotto deve rientrare in `uint64_t`. Un oggetto vuoto usa i valori sotto.

```json
{
  "scheduling": {
    "quantum_instructions": 1024,
    "instruction_time_100ns": 1
  }
}
```

`quantum_instructions` limita i tentativi ammessi di istruzioni macchina per thread. `instruction_time_100ns` assegna tempo virtuale a ogni tentativo, senza stimare la velocità hardware. Le API del modello e le transazioni delle istruzioni terminano prima del cambio. Gli eventi non azzerano il quanto restante. Il rapporto conserva la politica in `configuration.scheduling`.

`KeSetPriorityThread` e `KeQueryPriorityThread` accedono alla priorità corrente a `PASSIVE_LEVEL`. Sono accettati 1..31 e viene restituito il valore precedente; zero è riservato. Il profilo deterministico avvia ogni thread a 8. Viene scelto il thread pronto di priorità maggiore; le parità ruotano per ordine di disponibilità e quanto. Un risveglio o cambio verso priorità superiore interrompe prima della successiva istruzione guest sotto DISPATCH_LEVEL e conserva il quanto rimanente. Callback annidate e SEH condividono la priorità del thread. Gli oggetti di thread di sistema terminati ma referenziati restano interrogabili fino al ritiro; modificarli durante o dopo la terminazione non è supportato. Gli oggetti prestati delle callback scadono con il thread proprietario, senza trasmettere vecchie priorità al riuso dello stack.

Il mutex del kernel appartiene al thread logico attraverso callback annidati e SEH. `KeWaitForSingleObject` conserva questa identità per l’acquisizione differita; il proprietario può eseguire `KeReleaseMutex` da qualunque sua pila. Le APC normali restano disabilitate fino all’ultimo rilascio ricorsivo e il ritorno esterno rifiuta mutex ancora posseduti.

`KeWaitForMultipleObjects` supporta `WaitAll` e `WaitAny` su 1..64 eventi, timer, semafori, mutex inizializzati o thread di sistema referenziati distinti, in `KernelMode` non interrompibile con motivo `Executive`. `WaitAll` conferma insieme tutte le acquisizioni; `WaitAny` restituisce il più piccolo indice pronto e consuma solo quell’oggetto. Oltre tre oggetti occorre memoria `KWAIT_BLOCK` scrivibile e non paginabile. L’attesa differita salva l’array e conserva tutti gli oggetti e i blocchi del chiamante fino a successo o scadenza. Il controllo con timeout zero ammette DISPATCH_LEVEL; l’attesa bloccante conserva un IRQL fino ad APC_LEVEL. Duplicati, attese interrompibili/in modalità utente e abbandono di mutex restano esclusi. Ogni attesa differita ha un identificatore mai riutilizzato e uno stato acquisito immutabile; interrogare un’attesa completata o modificata genera un errore prima di consumare segnali o rilasciare riferimenti.

Continuazioni pronte, nuovi worker e thread di sistema condividono l’ordine di disponibilità. Chiamate annidate e SEH condividono il quanto del thread. Il cambio preserva l’intero contesto CPU, identità del thread, stato APC, IRQL effettivo e mapping del processo. PASSIVE/APC consente la prelazione; da DISPATCH in su il cambio è mascherato. Le regioni critiche e protette disattivano APC, senza impedire la prelazione.

Le istruzioni fanno avanzare timer e DMA in ordine di scadenza. L’annullamento attende il cancel lock disponibile e, per WDM, il ritorno da dispatch. Completamenti provider paginabili, politica energetica e PoFx attendono un confine PASSIVE; le scadenze restano pendenti e le osservazioni riportano il momento effettivo del servizio. I callback indipendenti dell’orologio mantengono una proprietà distinta dai callback PoFx bloccanti del thread originale. Il risultato di un’attesa viene fissato alla scadenza, prima di un successivo reset del timer o segnale, anche se un tentativo di istruzione attraversa entrambe le scadenze.

Non sono implementati classi e aumenti dinamici della priorità Windows, CPU parallele del modello OS, annidamento arbitrario delle interruzioni, consegna APC o thread Windows ring3. L’esecuzione CPU parallela è una capacità separata. Driver originali compilati verificano quanti brevi, stato floating point/GS, attese temporizzate, collegamenti di processo intercalati, annullamento e continuazioni PoFx. Le prove native ARM64 restano separate.
