**Sprachen**: [English](../unpack.md) | [简体中文](../zh-CN/unpack.md) | [繁體中文](../zh-TW/unpack.md) | [日本語](../ja/unpack.md) | [한국어](../ko/unpack.md) | [Français](../fr/unpack.md) | [Deutsch](unpack.md) | [Español](../es/unpack.md) | [Italiano](../it/unpack.md) | [Русский](../ru/unpack.md) | [العربية](../ar/unpack.md)

[← Dokumentationsindex](README.md)

# Entpacken gepackter ausführbarer Dateien

`neverd unpack` stellt das Programm wieder her, das eine gepackte ausführbare Datei in ihrem eigenen Adressraum neu aufbaut. Der Befehl führt die Eingabe als begrenzten Gastprozess aus, beobachtet, wo der Stub die Kontrolle an den von ihm erzeugten Code übergibt, und schreibt dieses Abbild als neue Datei desselben Containers. Er devirtualisiert nicht: Funktionen, die ein Schutzprogramm virtualisiert hat, bleiben virtualisiert. Der Build benötigt `NEVERD_ENABLE_CPU_EMULATION=ON`.

## Unterstützte Eingaben

Der Container bestimmt, wie eine Datei validiert und neu aufgebaut wird, der Befehlssatz bestimmt, wie ein Transfer beurteilt wird, und beide bestimmen das Gastprozessprofil. Eine Eingabe außerhalb dieser Tabelle wird namentlich abgelehnt, bevor irgendetwas ausgeführt wird.

| Container (`format`) | Befehlssatz | Gastprofil | Einstiegsnachweis |
| --- | --- | --- | --- |
| PE32+ (`pe64`) | x86-64 | [`windows-pe64-v1`](process-emulation.md) | Laufzeitbeobachtung |
| PE32+ (`pe64`) | ARM64 | [`windows-pe64-v1`](process-emulation.md) | Laufzeitbeobachtung |

PE32+-DLLs werden anhand von `IMAGE_FILE_DLL` erkannt. Eine modellierte Gast-EXE ruft `LoadLibraryA` und anschließend `FreeLibrary` über den normalen Lebenszyklus von Abhängigkeiten, TLS und `DllMain` auf. Der akzeptierte DLL-Eintritt ist ihr Prozess-Anfügeaufruf; Argumente beliebiger Exporte werden nicht erfunden. Namen, Ordinale, Aliase, Daten und Weiterleitungen bleiben erhalten. Zeiger auf eigene Exporte bleiben intern und erzeugen keine Selbstimporte. Das gilt auch für von Hilfsroutinen zurückgegebene Adressen: Ein internes Ergebnis widerruft frühere Import-Reparaturnachweise für diese Stelle.

## Verwendung

```bash
neverd unpack packed.exe -o unpacked.exe
neverd unpack packed.exe -o unpacked.exe \
  --options='{"backend":"unicorn","instruction_limit":400000000,"transfer":2}'
```

Der Befehl gibt einen JSON-Bericht aus. Exit-Code 0 bedeutet, dass das Abbild geschrieben wurde, 3, dass der begrenzte Lauf endete, bevor ein Einstieg akzeptiert wurde (`outcome` ist `no_entry`, und es wird nichts geschrieben), und 1 steht für eine ungültige Eingabe oder Option oder eine fehlgeschlagene Vorbereitung. Der Bericht nennt `format`, `architecture` und `profile`, die tatsächlich ausgeführt wurden. Der C-Einstiegspunkt ist `neverd_unpack_json`; Python stellt `Session.unpack` bereit. Die Optionen sind die [Prozessoptionen](process-emulation.md) zuzüglich `transfer`. Die Vorgaben weichen dort ab, wo ein Stub mehr Ressourcen braucht: 100000000 Befehle, 600 Sekunden und 512 MiB, und `windows.defer_unmodeled` ist eingeschaltet.

## Wie der Einstieg bestimmt wird

Generation null ist das Eingabe-Image beim Abbilden durch den Gastlader. Ein Übergang startet neueren Code. `transfers` erfasst RVA, `generation`, Stapelgleichheit (`stack_balanced`) und Zugehörigkeit zum Eingabe-Eintritt (`program_invocation`). Bei DLLs ist dies das Anfügen an den Prozess. Davor dient der Stapel des ersten Initialisierers als Bezug.

1. Der Standardeintritt verlangt sowohl `stack_balanced` als auch `program_invocation`: Der Stub hat den Stapel des Eingabeaufrufs zurückgegeben. `entry_source` ist `transfer`.
2. OS-eigene TLS-Callbacks, Eintritte abhängiger DLLs und Ablöseaufrufe können auch bei ausgeglichenem Stapel kein Standardeintritt sein. Tiefere Gastaufrufe laufen weiter. Kein Callback wird übersprungen; keine Stubsignatur sagt den Eintritt voraus.
3. `transfer` wählt ausdrücklich eine Listenposition, einschließlich Initialisierungs-Callbacks und Zwischenstufen. Bei Auswahl eines Initialisierungs-Callbacks werden auch das TLS des aktuellen Threads und bereits abgeschlossene Initialisierungen erfasst.

Aus der Gestalt von Compiler-Startcode wird kein Einstieg vorhergesagt. Ein Lauf, der vorher stoppt, meldet `no_entry` mit dem `stop_reason` des Prozesses.

Eine ausgeführte Seite von 4 KiB bleibt beobachtbar. Bestätigte Gast-RAM-Schreibzugriffe invalidieren besuchte Seiten über physische Aliase; das Prozessmodell prüft sie auch nach OS-Diensten vor dem Fortsetzen erneut. Transferbelege vergleichen die tatsächliche, vom CPU-Decoder bestimmte Befehlslänge. Geänderte benachbarte Daten begründen allein keinen Einstieg. Gemischte Generationen und spätere Änderungen bleiben überwacht, einschließlich Callbacks, die zum alten Stub auf derselben Seite zurückkehren.

## Das neu aufgebaute Abbild

Die Abschnitte behalten RVA, beobachteten Speicher einschließlich bereits ausgeführter Initialisierung und Seitenrechte. Der neue Abschnitt `.neverd` enthält das Importverzeichnis und neue IAT-Zellen für Exportaufrufe und Adressladevorgänge; ursprünglicher Nullspeicher bleibt erhalten. `origin` in `imports` unterscheidet aus der Eingabe gebundene `static`-Zellen und vom Gast geschriebene oder zur Reparatur ergänzte `runtime`-Zellen. Da Relokationen generierter Inhalte nicht beobachtet wurden, bleibt das Abbild an der beobachteten Basis, das Relokationsverzeichnis entfällt und `IMAGE_FILE_RELOCS_STRIPPED` wird gesetzt.

Abschnittsbereiche im Benutzermodus runden `VirtualSize` (bei null `SizeOfRawData`) auf Gastseiten auf; `SectionAlignment` bestimmt die RVA-Positionen. Rohdaten innerhalb der letzten abgebildeten Seite bleiben erhalten, Bytes außerhalb des Bereichs werden nicht abgebildet. Die Rekonstruktion behält `FileAlignment` bei und füllt Dateipadding mit Nullen, ohne Abschnittslücken zu lesen. Native Nachweise für diese Regel liegen nur für x64-Windows-DLLs vor. Laufzeitzulassung, Metadatenleser und die Validierung fester Abbilder teilen den logischen Umfang vor der Seitenrundung. Bei `VirtualSize` null gilt `SizeOfRawData`; ein Wert ungleich null begrenzt weiterhin die Metadaten. Der Loader verwendet eine private Leseansicht für LLVM und bewahrt die Originalbytes für Ausführung, Debugidentitäten und Authentifizierung. Treiberabbildungen fallen nicht unter die Benutzerseitenregel.

Dateioffsets für Debugdaten werden nur angepasst, wenn sowohl das Debugverzeichnis als auch alle beibehaltenen Nutzdaten vollständig durch Dateiinhalte gedeckt sind. Bei `AddressOfRawData == 0` behalten vollständig im erhaltenen Overlay liegende Nutzdaten ihren relativen Offset zur neuen Dateiposition des Overlays. Ein ungültiger RVA ungleich null erlaubt keinen Rückgriff auf das Overlay.

Schreibbare Importzellen benötigen keinen nativen IAT-Schutzbereich; ihre Deskriptoren binden weiterhin jede Zelle. Schreibgeschützte Zellen benötigen einen zusammenhängenden Bereich aus schreibgeschützten, nicht ausführbaren Abschnitten. Ein Bereich über schreibbare oder ausführbare Abschnitte wird ausdrücklich abgewiesen, statt deren Speicherschutz zu ändern.

Vorhandene Laufzeit-IAT-Kandidaten müssen ein zusammenhängendes Exportzeigerfeld desselben Anbieters mit intaktem Nullabschluss innerhalb des Originalabschnitts bilden. Der Abschluss des benachbarten Anbieters genügt nicht.

Ein ursprüngliches TLS-Verzeichnis mit passender Allokationsidentität des Loaders kann ohne Callbackfeld oder mit einem leeren Feld vorliegen. Ein vollständig validiertes ursprüngliches Verzeichnis bleibt ohne Nachweis der Callbackausführung nutzbar: Erfasstes TLS wird weiterhin geprüft und bei Bedarf wiederhergestellt; ausstehende Callbacks zählen nicht als abgeschlossen. Durch Suche gefundene Ersatzdatensätze benötigen weiterhin eine vollständige, nullterminierte Callbackliste und einen Nachweis der Callbackausführung. Mehrere Kandidaten führen zu einem ausdrücklichen Fehler. Schutzprogrammspezifische Namen oder Bytes werden nicht verwendet. Bei der Verzeichnisauswahl hat die beobachtete Ausführung generierter Callbacks Vorrang vor einem abgeschlossenen Loader-Initialisierer. Ein erneuter Aufruf eines abgeschlossenen Loader-Initialisierers durch das Programm bleibt ein nachrangiger Beleg, auch wenn der Initialisierer umgeschrieben wurde. Ist kein Ersatz rekonstruierbar und das ursprüngliche TLS-Verzeichnis ungültig, schlägt der Vorgang ausdrücklich fehl, statt `unpacked` zu melden.

`materialized_tls_callbacks` zählt vor der Aufnahme zurückgekehrte TLS-Prozess-Anfügecallbacks des Eingabe-Images. Der Nachweis verlangt einen OS-Aufrufdatensatz oder einen beobachteten generierten Eintritt mit `(image_base, 1, 0)`, gefolgt von seiner ABI-Rücksprungadresse und dem wiederhergestellten Stapelzeiger. Ihre Speicherwirkungen sind bereits im Abbild enthalten. Adapter im neuen Abschnitt `.neverd` vermeiden nur diesen wiederholten Aufruf und leiten andere Benachrichtigungen durch einen Endaufruf an das Original weiter. Verzeichnis, Callbackfeld und Adapter nutzen neuen Speicher; Originalbytes bleiben erhalten. Der Abschnitt ist mit Adaptern ausführbar und nur bei Bedarf neuer IAT-Zellen beschreibbar. Gastinterne Callbacks ohne diesen Abschlussnachweis bleiben unverändert. Beim Anfügen des Prozesses stellt ein Adapter die erfassten TLS-Bytes des Hauptthreads wieder her, wenn sie von der Vorlage abweichen, auch ohne abgeschlossene Callbacks. Fehlende TLS-Nachweise oder abweichende Größen führen zu einem ausdrücklichen Fehler. Die Originalvorlage bleibt für spätere Threads erhalten; andere Benachrichtigungen stellen den erfassten Block nicht wieder her.

Verzögertes Laden erlaubt ausführbare Callback- und Eintrittsziele in nullgefülltem Speicher, deren Code frühere Initialisierer erzeugen. Callbackfelder und TLS-Zuteilungsmetadaten benötigen weiterhin geprüfte Dateiinhalte; strenges Laden behält seine Dateiprüfung. Das OS-Modell liefert die Aufrufzugehörigkeit und benachrichtigt Beobachter beim Vorbereiten eines Aufrufs oder Wiederherstellen eines angehaltenen Aufrufers. An diesen Grenzen werden Übergangswachen neu gesetzt, auch wenn Callback und erzeugter Eintritt dieselbe Seite teilen.

`import_repair` umfasst zwei zusätzliche begrenzte Läufe nach der Einstiegserfassung: Ermittlung der Exportaufrufe und danach Beobachtung des Hilfsroutinenzustands. Jeder Lauf hat eigene Prozessgrenzen. Befehls- und Ereigniszähler werden sättigend addiert; `stop_reason` und Diagnose beschreiben den letzten Lauf, beobachtete Aufrufe nur die Ermittlung. Ohne Fortsetzungsadresse entfällt die Zustandsbeobachtung. Widersprüchliche Exportidentitäten verhindern das Umschreiben. Nur erreichte Pfade sind erfasst; `unpacked` bestätigt weder alle Imports noch erfolgreiche Programmausführung.

Die Bindungsreihenfolge der Importe kann Exportadressen ändern. Die Reparatur behält die Identität aus dem prüfenden Lauf und beobachtet auch erst nach dem Einstieg aufgelöste Exporte; eine Adresse aus einem anderen Lauf erlaubt keine Umschreibung.

Die Ermittlung beobachtet Exportaufrufe einschließlich des nicht modellierten Exports, der die Ausführung stoppt, statt nur Protokolle modellierter Aufrufe zu verwenden. Eine unlesbare ABI-Rückkehrposition kann keinen Hilfsroutinennachweis begründen. Die Reparatur eines reinen Aufrufs erhält den ausdrücklichen Stopp bei nicht unterstützten Diensten.

Beobachtet werden höchstens 256 Kandidatenstarts in den 256 Bytes vor tatsächlichen Exportaufruffortsetzungen. `CALL rel32`, optional mit vorangestelltem GPR-PUSH/POP, kennzeichnet nur eine Beobachtungsgrenze; nachfolgende Bytes sind keine Signatur. Ein Aufruffenster von sechs bis acht Bytes wird nur dann zu `call [rip+IAT]`, wenn am API-Eingang genau eine Rücksprungadresse auf dem Stack liegt und andere Register (einschließlich Flags und SIMD), Mappings und dauerhafter RAM unverändert sind, ohne vorherigen OS-Aufruf. Führende NOPs erhalten die genaue Rücksprungadresse. Sieben oder acht Bytes werden zu `mov r64, [rip+IAT]`, wenn ein bekanntes Exportziel in genau einem GPR zurückkommt, bei ausgeglichenem Stack und denselben Erhaltungsbedingungen. Das Ergebnisregister wird aus Zuständen ermittelt: alle GPR außer RSP, einschließlich R8-R15; acht Bytes behalten ein abschließendes NOP. Nur temporärer Speicher unterhalb des Aufrufer-SP ist ausgenommen; sein Stack wird verglichen. Spätere unreine, ungelöste oder unvollständige Aufrufe verwerfen frühere Nachweise. Der ausgeführte Start muss belegt sein; kein REX-Präfix wird aus dem vorherigen Byte geraten, überlappende Starts mit gemeinsamer Rückkehr werden abgelehnt. Alle validierten Fenster einer widersprüchlichen Fortsetzungsstelle blockieren auch nach deren Ablehnung überlappende Reparaturen, unabhängig von der Reihenfolge der Nachweise. `observed_loads` und `repaired_loads` zählen Ladeoperationen separat.

`ImportObserver` behält über zwischenzeitliche Ausführungsstopps hinweg einen äußeren Snapshot. Ein verschachtelter Kandidat verliert seine früheren Nachweise, da er keinen eigenen Snapshot besitzt. Der äußere Aufruf muss weiterhin den vollständigen Vergleich von Registern, Mappings, persistentem Speicher und OS-Aufrufen bestehen. Rekursive, unterbrochene und unvollständige Aufrufe verlieren ihre Nachweise. `ProcessImportsTests.cpp` und die unabhängig gelinkte DLL-Testdatei prüfen diese Grenzen. Nur ein Kandidat mit beobachteter Export-Fortsetzung behält seinen Snapshot über verschachtelte Stopps hinweg.

Die ursprüngliche Abschnittstabelle, die Anordnung des Importverzeichnisses und die Relokationstabelle werden nicht rekonstruiert; ein Packer stellt sie im Speicher nicht wieder her.

## Identifikation

Die Kompatibilitätsfelder `packer.kind` und `packer.evidence` liefern `unidentified` und ein leeres Array. Schutzprogrammregister, Pack-Header-Parser und Stub-Signaturen wurden entfernt. Die bisherige Identifikations-API validiert nur den Container.

## Grenzen

Geprüfte Ausführung arbeitet befehlsweise. x64 Unicorn/KVM/WHP unterstützt außerdem `direct-user-x64-v1` mit Zeit- und Ereignisgrenzen ohne Befehlszählung. Gemischte Generationen und wiederholte Schreibzugriffe können zusätzliche Prozessorschritte erfordern. Die Wiederherstellung umfasst erreichte Pfade im Hauptabbild; außerhalb erzeugter Code wird nicht zu dessen Einstieg. Initialisierung vor dem Einstieg kann nicht übertragbaren externen Zustand erzeugen; erneut ausgeführte TLS-Callbacks können weitere Effekte haben. Nicht modellierte APIs stoppen ausdrücklich. Die Reparatur erfasst validierte x64-Aufruffenster von sechs bis acht Bytes und Adressladevorgänge von sieben oder acht Bytes; andere Formen und nicht erreichte Pfade bleiben ungelöst. Virtualisierter Code bleibt virtualisiert.

RVA-basierte verzögerte Importe behalten ungelöste interne Thunks und binden bereits aufgelöste Zellen anhand beobachteter Exportidentitäten neu. Der Programmhelfer wiederholt dadurch keine abgeschlossene Arbeit. Eine unabhängige, terminierte Lookup-Tabelle begrenzt jeden Bindungslauf und erhält die folgende ungelöste IAT-Zelle. Der Neuaufbau löscht prozessgebundene DLL-Handles und Bindungscache-Verweise; die verzögerten Metadaten bleiben von der gewöhnlichen IAT-Suche ausgeschlossen. Deskriptoren, Zeigerfelder, Namen, Bereiche und Speicherzuständigkeiten werden geprüft. Unbekannte Ziele, ungültige Thunks, alte VA-Deskriptoren und Speicherkonflikte führen zu expliziten Fehlern.

## Verifikation

`NeverDUnpackTests` prüft Container, sichere IAT-Allokation, Widersprüche und TLS-Ablehnung. `NeverDUnpackExecutionTests` prüft UPX NRV2B/NRV2D/NRV2E/LZMA und CRT über denselben generischen Pfad auf Unicorn/KVM/WHP, vergleicht mit dem unabhängig gelinkten Programm nach dessen Initialisierung und verlangt gleiche Ausgaben verfügbarer Backends. `UnpackGeneratedTests.cpp` erzeugt unabhängige x86-64/ARM64-Programme für Transfers, gestuftes Laden, Imports und direkte x64-KVM/WHP-Ausführung. Native Pflichtfälle dürfen in CI nicht übersprungen werden. `NeverDUnpackPublicTests` deckt C-ABI und CLI ab. `unittests/unpack/fixtures/Makefile` erzeugt die UPX-Beispiele erneut. Das native KVM/WHP-CI-Inventar (`NativeCPUTests.def`) verlangt auch erfolgreiche zentrale Unit-Tests für PE, TLS, Importe und Transfers. Reine Formattests bleiben verfügbar, wenn CPU- und Treiberemulation beide deaktiviert sind.


`UnpackLibraryTests.cpp` packt unabhängige x64/ARM64-DLLs im Test und prüft Abhängigkeitsreihenfolge, normale/generierte TLS-Callbacks, Fehlerbereinigung, Eingabe-/Hostidentität, eigenen Dateizugriff, Namen/Ordinale/Daten/Weiterleitungen und fehlende Selbstimporte. Natives Windows lädt Originale und rekonstruierte DLLs über eine separate EXE und ruft deklarierte Exporte auf; geprüfte und direkte WHP-Fälle sind Pflicht. `CompletedGeneratedTLSCallsRequireTheAttachABI` verwirft geänderte Eintritte/Argumente; `GeneratedCallsNeedTheirReturnedStackAtTheContinuation` verwirft falsche Rücksprungstapel. Dies prüft Entpacken ohne Devirtualisierung.

`ExportObserver` beobachtet auch ausführbare Exporte residenter Gastabhängigkeiten; modellierte Anbieter bleiben an der Dienstverteilung beobachtet. Eigene Eingabe-Exporte sind ausgeschlossen. Moduländerungen erneuern die Haltepunkte; jede Reparatur verlangt die aktuelle Exportidentität. Aufzeichnungen bleiben innerhalb des deklarierten Importlimits. DLL-Tests reparieren System-API- und Gastabhängigkeitshelfer; natives Laden prüft, dass keine emulierte Adresse übrig bleibt.


`WrappedEntriesRequireExplicitTransferEvidence` prüft einen DLL-Wrapper, der den wiederhergestellten Eintritt auf tieferem Stapel aufruft. Standard bleibt `no_entry`; die Wahl des beobachteten Aufrufs mit `transfer` erzeugt eine ladbare DLL. Ein tieferer Aufruf allein unterscheidet Eintritt und Initialisierer nicht.
