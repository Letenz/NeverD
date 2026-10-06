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

Lauffähige Fortsetzungen, neue Worker und Systemthreads teilen eine Bereitschaftsreihenfolge. Verschachtelte Aufrufe und SEH teilen das Quantum ihres Threads. Wechsel bewahren den vollständigen CPU-Kontext, Threadidentität, APC-Zustand, effektiven IRQL und Prozessabbildungen. PASSIVE/APC ist präemptiv; ab DISPATCH wird der Threadwechsel maskiert. Kritische und geschützte Regionen deaktivieren APCs, aber nicht die Threadpräemption.

Laufende Befehle treiben Timer und DMA chronologisch voran. Abbruch wartet auf den verfügbaren Cancel-Lock und bei WDM auf die Dispatch-Rückkehr. Auslagerbare Provider-Abschlüsse, Energiepolitik und PoFx-Dienste warten auf eine passive Grenze; ursprüngliche Fristen bleiben erhalten, Beobachtungen erfassen die tatsächliche Bearbeitungszeit. Unabhängige Clock-Callbacks und blockierende PoFx-Callbacks im ursprünglichen Thread behalten getrennte Eigentümer. Warteergebnisse stehen an ihrer Frist fest, bevor ein späterer Timer-Reset oder ein Ereignissignal erfolgt, auch wenn ein Instruktionsversuch beide Fristen überspannt.

Windows-Prioritätsklassen und dynamische Anhebungen, parallele CPUs des OS-Modells, beliebige Interrupt-Verschachtelung, APC-Zustellung und Windows-ring3-Threads sind nicht implementiert. Parallele CPU-Ausführung ist eine eigene Fähigkeit. Originale kompilierte Treiber prüfen kurze Quanten, Gleitkomma-/GS-Zustand, zeitliche Wartevorgänge, wechselnde Prozessanbindung, Abbruch und PoFx-Fortsetzungen. Native ARM64-OS-Nachweise werden separat geführt.
