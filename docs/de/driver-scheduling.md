# Präemptives Scheduling für Windows-Treiber

Das optionale Objekt `scheduling` aktiviert deterministische Präemption auf CPU0 für x64-Windows-Treiber mit Unicorn, KVM oder WHP. Ohne dieses Objekt bleiben kooperatives Scheduling und eine nur im Leerlauf fortschreitende virtuelle Zeit erhalten. Beide Felder müssen positive Ganzzahlen sein; ihr Produkt muss in `uint64_t` passen. Ein leeres Objekt verwendet die unten gezeigten Standardwerte.

```json
{
  "scheduling": {
    "quantum_instructions": 1024,
    "instruction_time_100ns": 1
  }
}
```

`quantum_instructions` begrenzt die zugelassenen Maschinenbefehlsversuche eines Threads. `instruction_time_100ns` weist jedem Versuch virtuelle Zeit zu und schätzt keine Hardwaregeschwindigkeit. Modell-APIs und Befehlstransaktionen enden vor dem Wechsel. Ereignisgrenzen setzen das restliche Quantum nicht zurück. Der Bericht enthält die Auswahl unter `configuration.scheduling`.

`KeSetPriorityThread` und `KeQueryPriorityThread` greifen bei `PASSIVE_LEVEL` auf die Laufzeitpriorität zu. Setzen akzeptiert 1..31 und liefert den alten Wert; null ist reserviert. Das deterministische Profil beginnt jeden Thread mit 8. Der bereite Thread höchster Priorität läuft; gleiche Prioritäten rotieren nach Bereitschaftsfolge und Quantum. Aufwachen oder Änderungen zugunsten höherer Priorität präemptieren unter DISPATCH_LEVEL vor dem nächsten Gastbefehl und erhalten das Restquantum. Verschachtelte Callbacks und SEH teilen die Threadpriorität. Referenzierte beendete Systemthreadobjekte bleiben bis zur Freigabe abfragbar; Änderungen sind nach Beginn der Beendigung nicht unterstützt. Geliehene Callback-Threadobjekte enden mit ihrem Besitzer, sodass wiederverwendete Stackplätze keine alte Priorität übernehmen.

Kernel-Mutexe gehören dem logischen Thread auch über verschachtelte Callbacks und SEH hinweg. `KeWaitForSingleObject` speichert diese Identität für einen verzögerten Erwerb; der Besitzer kann `KeReleaseMutex` auf jedem seiner Aufrufstapel ausführen. Normale APCs bleiben bis zur letzten rekursiven Freigabe deaktiviert, und die äußerste Rückkehr weist noch gehaltene Mutexe zurück.

`KeWaitForMultipleObjects` unterstützt `WaitAll` und `WaitAny` für 1..64 verschiedene initialisierte Ereignisse, Timer, Semaphore, Mutexobjekte oder referenzierte Systemthreads, nicht alarmierbar in `KernelMode` mit Grund `Executive`. `WaitAll` übernimmt alle Erwerbe gemeinsam; `WaitAny` liefert den kleinsten bereiten Arrayindex und verbraucht nur dieses Objekt. Mehr als drei Objekte erfordern beschreibbaren nicht ausgelagerten `KWAIT_BLOCK`-Speicher. Ein aufgeschobenes Warten speichert das Array und hält alle Objekte sowie die Aufruferblöcke bis Erfolg oder Zeitablauf. Abfragen mit Nullfrist sind bis DISPATCH_LEVEL zulässig; blockierendes Warten erhält einen IRQL bis APC_LEVEL. Doppelte Objekte, alarmierbares/Benutzermodus-Warten und verlassene Mutexobjekte bleiben ausgeschlossen. Jede aufgeschobene Warteoperation besitzt eine nie wiederverwendete Kennung und einen unveränderlichen erfassten Zustand. Die Abfrage einer abgeschlossenen oder veränderten Warteoperation schlägt vor dem Signalverbrauch oder der Referenzfreigabe fehl.

Lauffähige Fortsetzungen, neue Worker und Systemthreads teilen eine Bereitschaftsreihenfolge. Verschachtelte Aufrufe und SEH teilen das Quantum ihres Threads. Wechsel bewahren den vollständigen CPU-Kontext, Threadidentität, APC-Zustand, effektiven IRQL und Prozessabbildungen. PASSIVE/APC ist präemptiv; ab DISPATCH wird der Threadwechsel maskiert. Kritische und geschützte Regionen deaktivieren APCs, aber nicht die Threadpräemption.

Laufende Befehle treiben Timer und DMA chronologisch voran. Abbruch wartet auf den verfügbaren Cancel-Lock und bei WDM auf die Dispatch-Rückkehr. Auslagerbare Provider-Abschlüsse, Energiepolitik und PoFx-Dienste warten auf eine passive Grenze; ursprüngliche Fristen bleiben erhalten, Beobachtungen erfassen die tatsächliche Bearbeitungszeit. Unabhängige Clock-Callbacks und blockierende PoFx-Callbacks im ursprünglichen Thread behalten getrennte Eigentümer. Warteergebnisse stehen an ihrer Frist fest, bevor ein späterer Timer-Reset oder ein Ereignissignal erfolgt, auch wenn ein Instruktionsversuch beide Fristen überspannt.

Windows-Prioritätsklassen und dynamische Anhebungen, parallele CPUs des OS-Modells, beliebige Interrupt-Verschachtelung, APC-Zustellung und Windows-ring3-Threads sind nicht implementiert. Parallele CPU-Ausführung ist eine eigene Fähigkeit. Originale kompilierte Treiber prüfen kurze Quanten, Gleitkomma-/GS-Zustand, zeitliche Wartevorgänge, wechselnde Prozessanbindung, Abbruch und PoFx-Fortsetzungen. Native ARM64-OS-Nachweise werden separat geführt.
