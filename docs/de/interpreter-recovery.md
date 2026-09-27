**Sprachen**: [English](../interpreter-recovery.md) | [简体中文](../zh-CN/interpreter-recovery.md) | [繁體中文](../zh-TW/interpreter-recovery.md) | [日本語](../ja/interpreter-recovery.md) | [한국어](../ko/interpreter-recovery.md) | [Français](../fr/interpreter-recovery.md) | [Deutsch](interpreter-recovery.md) | [Español](../es/interpreter-recovery.md) | [Italiano](../it/interpreter-recovery.md) | [Русский](../ru/interpreter-recovery.md) | [العربية](../ar/interpreter-recovery.md)

# Quelltextrekonstruktion aus Interpretern

[← Dokumentationsübersicht](README.md)

Die experimentelle Interpreter-Spezialisierung entfernt statisch aufgelöste
Dispatch-Vorgänge aus einer fertig gelinkten x64-Funktion. Laufzeiteingaben,
Speichereffekte, Verzweigungen und Schleifen bleiben erhalten. Grundlage ist
die Instruktionssemantik, nicht die Erkennung von Handler-Signaturen oder die
Opcode-Tabelle eines bestimmten Schutzprogramms.

```sh
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --recovery-report recovery.json -o recovered.c
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --llvm -o recovered-llvm.c
```

`--vm-control` wählt vollständige allgemeine Register, die Interpreter-Kontexte
unterscheiden. Die Option darf mehrfach vorkommen und gibt keine konkreten
Werte vor. Wählen Sie beispielsweise einen Bytecode-Zeiger, dessen Wert der
Einstiegscode festlegt. Eine als Zähler verwendete Laufzeiteingabe muss dynamisch
bleiben. Fehlende Kontexttrennung kann die Rekonstruktion stoppen, wenn
verschiedene Zeigerwerte zusammentreffen; die Engine darf dies nicht durch
Erraten eines Dispatch-Ziels ausgleichen. Für einen auf den Stack ausgelagerten
Zeiger wählt `--vm-control-stack=-16:8` acht Bytes bei Eintritts-RSP minus 16.
Der Offset bezieht sich auf den Funktionseintritt, nicht auf den inzwischen
veränderten Stackpointer.

Die C-API heißt `neverd_devirtualize_source_v1()` und ist in
`neverd/sdk/NeverDCAPIDevirtualize.h` deklariert. Sie führt eine separate
Transaktion aus, ohne den gewöhnlichen Dekompilationscache der Sitzung zu
verändern. Bei einem Fehler wird kein Quelltext zurückgegeben; eine
JSON-Diagnose kann dennoch vorliegen. Beide zugewiesenen Zeichenketten werden
mit `neverd_free_string()` freigegeben.

## Ausführungsvertrag

Der Binäradapter akzeptiert derzeit fertig gelinkte x64-ELF- und PE-Abbilder an
ihren gemappten Adressen. Mappings, Bytes und Berechtigungen müssen unverändert
bleiben; gleichzeitige Änderungen sind ausgeschlossen. Striktes Lifting bei
Bedarf folgt dem erreichbaren Maschinencode. Nicht unterstützte Instruktionen,
Aufrufe, undurchsichtige Operationen, geordnete Speicherzugriffe, nicht
aufgelöster Kontrollfluss und sprachabhängige Ausnahmebehandlung stoppen die
Rekonstruktion.

Die PE-Rekonstruktion erfordert vollständige Abbildmetadaten einschließlich aller Relokationen und Ausnahmeeinträge. Die CLI lädt sie vor der Anwendung von `--func`; C-API-Aufrufer dürfen die Sitzung nicht zuvor mit `neverd_session_restrict_function()` einschränken. Der Binäradapter lehnt Abbilder ab, die mit einer eingeschränkten Funktionsmenge geladen wurden: Ausgelassene Metadaten beweisen weder die Abwesenheit von Fixups noch die von Ausnahmekanten.

Nur vollständige, dateigestützte und schreibgeschützte Bereiche ohne
überlappende Mappings oder Loader-Fixups dürfen konstante Abbildlesevorgänge
begründen. Schreibbare Tabellen, nicht aufgelöste Relokationen und punktuelle
Laufzeitschnappschüsse belegen keine Unveränderlichkeit. COPY-Relokationen und strukturell unvollständige Ausnahmedirektoren werden
abgelehnt. Ist das PE-Verzeichnis samt Funktionsbereichen vollständig, hindert
ein unbekannter Handler in einer anderen Funktion die Analyse nicht; erreicht
die Wiederherstellung dessen Codebereich, wird sie abgelehnt. Der Gültigkeitsbereich
verlangt normale ABI-Rückgaben: Der Zielbereich jedes Schreibzugriffs externen
Ursprungs muss vom Speicherplatz der Eintritts-Rücksprungadresse getrennt sein.
Dies ist eine ausdrückliche Voraussetzung an Aufrufer und Umgebung, auch bei
Adressen, die aus externen Ganzzahlen berechnet werden. Fehlende Herkunft aus
dem Stackframe beweist keine numerische Überschneidungsfreiheit. Aus dem Frame
abgeleitete Schreibadressen müssen diese Trennung nachweisen; bei der Rückgabe
muss der ursprüngliche Stackpointer wiederhergestellt sein. Herkunftsinformation
übersteht Auslagerungen auf den Stack und Zusammenführungen. Der Verlust eines
affinen Ausdrucks macht daraus keinen externen Zeiger. Stack-Pivots,
Rücksprünge mit Bereinigung der Argumente durch den Aufgerufenen und RET-basierter
Dispatch werden derzeit abgelehnt. Der Binäradapter erzwingt die
Little-Endian-Semantik von x64.

Die Ausgabe umfasst Quelltext und IR für die Analyse. Sie weist keine Sicherheit
für Relokation, Stack-Unwinding, asynchrone Ausnahmen oder binären Ersatz nach.
Der Patch-Modus lehnt diese Option ab. Eine Unterstützung aller Interpreter
oder Schutzkonfigurationen wird nicht zugesichert.

## Aktuelle Grenzen

Eingabeabhängige Bytecode-Adressen und Beziehungen zwischen Decoder-Zuständen werden nur unterstützt, wenn die erforderlichen endlichen Wertebereiche und Korrelationen innerhalb der konfigurierten Grenzen nachweisbar sind. Daraus folgt keine Unterstützung beliebiger indirekter Decodierschemata. Dynamische Verzweigungen und Schleifen können rekonstruiert werden, wenn jedes Dispatch-Ziel bewiesen ist; gewöhnliche Zweigabdeckung genügt dafür nicht. Nicht aufgelöster Kontrollfluss und ausgeschöpfte erforderliche Beweisbudgets sind Fehler: Es wird weder rekonstruierter Quelltext noch ein teilweiser Ersatz veröffentlicht. Native Hilfsaufrufe, Ausnahme- und Wiedereintrittsgrenzen, veränderlicher Code und andere Architekturen bleiben außerhalb des Ausführungsvertrags.

## Gemeinsame Implementierung

`SpecializationProvider` liefert vollständig geliftete Instruktionen und
Nachweise für unveränderliche Lesezugriffe. `NeverDInterpreterSpecialization`
verwendet die vorhandene `SymExec`-Semantik zur partiellen Auswertung von
Ganzzahl- und Kontrolloperationen. Der Binäradapter verantwortet Mappings und
Instruktionsdecodierung; er implementiert keinen zweiten Instruktionsauswerter.

Bei einer symbolischen Leseadresse mit endlichem Wertebereich zählt der integrierte Bitvektor-Solver unter den aktuellen Bedingungen mögliche Adressen auf. Die Menge wird erst akzeptiert, wenn ein abschließendes UNSAT beweist, dass keine weitere Adresse möglich ist, und für jede Adresse ein vollständiger Nachweis eines unveränderlichen, nicht fehlschlagenden Lesezugriffs vorliegt. Ein solcher Zugriff kann in LowIR durch die einmalige Erfassung der Adresse und eine exakte SELECT-Kette ersetzt werden; gewöhnliche nicht zertifizierte Lesezugriffe bleiben dynamisch. Stichproben ersetzen niemals die vollständige Menge. Ausgewählte Kontrollregister und Eintritts-Frame-Slots können begrenzte gemeinsame Wertetupel über Knotengrenzen bewahren, etwa die Beziehung zwischen Cursor und Decodierschlüssel. Zusammenführungen und Erweiterungen bleiben konservativ. Einzelne SAT-Modelle oder unbekannte Solver-Ergebnisse beweisen keine vollständige Adress- oder Zielmenge. Der optionale Z3-Backend wird dafür nicht benötigt.

Ein Knoten wird durch seinen nativen Cursor, den Instruktionsmodus und die ausgewählten konstanten Kontrollregister und Eintritts-Frame-Slots bestimmt. Weitere byteweise Fakten werden durch Schnittmengenbildung zusammengeführt. Schwächt sich ein eingehender Fakt ab, wird der Knoten erneut ausgewertet. So bleiben Programmschleifen als Schleifen erhalten, statt jede beobachtete Iteration einzeln zu entfalten. Alle erreichbaren Werte eines indirekten Ziels müssen zu einer begrenzten, nachweislich vollständigen Menge gehören. Die ausgewählten Ziele werden zu expliziten Vergleichen im Restprogramm und zu CFG-Kanten.

Dynamische Operationen sowie gewöhnliche Lese- und Schreibzugriffe verbleiben
in LowIR. Skalare Konstanten, affine Zeiger relativ zum Eintrittsframe und nachweislich
konstante Frame-Bytes dürfen Knotengrenzen überschreiten. Andere Ausdrücke
werden verworfen, statt sie unbegrenzt zu erweitern. Frame-Speicher verwendet
die konservative Alias-Invalidierung des vorhandenen symbolischen Zustands.
Schreiben über einen unbekannten, möglicherweise überlappenden Zeiger
invalidiert widersprüchliche Fakten. Dabei wird weder ein Stack-Slot als privat
angenommen noch seine Wirkung aufgrund eines unbewiesenen No-Alias-Vertrags
entfernt.

Eindeutige synthetische Instruktionslabels unterscheiden geklonte Kontexte. Die
ursprünglichen Instruktionsgrenzen bleiben in einer separaten Herkunftstabelle;
originale Relokations-, Ausnahme- oder Sprungtabellennachweise werden nicht auf
neue Vorkommen übertragen. Das rekonstruierte LowIR durchläuft vor der
Aufteilung in HighC und LLVM die gewöhnliche LowIR-zu-MedIR-Konvertierung.
Register-, Stack-, CFG-, SSA- und ABI-Behandlung bleiben dadurch gemeinsam.
Der HighC-Pfad erfordert zusätzlich eine erfolgreiche MedIR-Verifikation.

Budgets begrenzen Knoten, Kontexte pro Adresse, Operationen, Knotenauswertungen
und endliche Zielmengen. Bei Budgetüberschreitung oder nicht unterstützter
Semantik wird keine Restfunktion veröffentlicht. Ein vollständiger
Kontrollflussgraph ist von erfolgreicher Quelltextausgabe zu unterscheiden;
die öffentliche API prüft beide Ergebnisse und meldet sie getrennt.

Endliche Leseadressmengen, gemeinsame Kontrolltupel und die Anzahl der Kontrollfelder haben eigene Grenzen. Ein globales Limit für Solver-Abfragen sowie Limits pro Abfrage für Gatter, Konflikte, Propagationen und Besuche überwachter Literale begrenzen den Beweisaufwand; ein Limit für symbolische Knoten begrenzt das Ausdruckswachstum. Der JSON-Bericht enthält diese Budgets sowie `solverQueries` und `relationalWidenings`.

## Nachweise und Tests

Der optionale lokale JSON-Bericht enthält den Eingabehash, gewählte
Kontrollgrößen, Budgets, Status, Arbeitszähler, die Anzahl verbleibender Blöcke,
ursprüngliche Instruktionsorte und die bei der Rekonstruktion verwendeten
unveränderlichen Bytes. Er enthält aus der Eingabe abgeleitete Informationen
und wird ausschließlich an den angeforderten lokalen Pfad geschrieben.

Die öffentlichen Tests verwenden eigenständig entwickelte Maschinen mit
Register- und Stack-Dispatch, jeweils mit Arithmetik, Verzweigungen samt
Zusammenführung und Laufzeitschleifen. Ein unabhängiges vorzeichenloses Oracle
prüft Rückgabewerte, Speicherzugriffe, Überträge und Borrows sowie
Ausgabeschutzwerte. Rekonstruierte HighC- und LLVMC-Quellen werden mit O0/O2 und
Traps für undefiniertes Verhalten kompiliert und gegen dieses Oracle ausgeführt.
Negativfälle prüfen nicht aufgelösten und schreibbaren Dispatch, inkompatible
Byte-Reihenfolge, Ausnahmemetadaten und Budgets.

Weitere eigenständige Fixtures mit endlichen Adressen verwenden eingabeabhängig gewählte schreibgeschützte Datensätze, zusammenhängende Cursor-/Schlüsselfelder und denselben Handler an verschiedenen virtuellen Positionen. Sie decken Verzweigungen mit Zusammenführung und Schleifen ab, deren Datensatzauswahl vom aktuellen Programmzustand abhängt. Ihr unabhängiges natives Oracle verwendet SysV und Win64; beide rekonstruierten C-Pfade werden unter O0/O2 mit Fallen für undefiniertes Verhalten und Ausgabewächtern geprüft. Fehlende Lesezertifikate oder unzureichende Beweisbudgets dürfen kein Teilergebnis veröffentlichen.

Gezielte Testziele stehen in [testing.md](testing.md).
