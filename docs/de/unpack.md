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

## Verwendung

```bash
neverd unpack packed.exe -o unpacked.exe
neverd unpack packed.exe -o unpacked.exe \
  --options='{"backend":"unicorn","instruction_limit":400000000,"transfer":2}'
```

Der Befehl gibt einen JSON-Bericht aus. Exit-Code 0 bedeutet, dass das Abbild geschrieben wurde, 3, dass der begrenzte Lauf endete, bevor ein Einstieg akzeptiert wurde (`outcome` ist `no_entry`, und es wird nichts geschrieben), und 1 steht für eine ungültige Eingabe oder Option oder eine fehlgeschlagene Vorbereitung. Der Bericht nennt `format`, `architecture` und `profile`, die tatsächlich ausgeführt wurden. Der C-Einstiegspunkt ist `neverd_unpack_json`; Python stellt `Session.unpack` bereit. Die Optionen sind die [Prozessoptionen](process-emulation.md) zuzüglich `transfer`. Die Vorgaben weichen dort ab, wo ein Stub mehr Ressourcen braucht: 100000000 Befehle, 600 Sekunden und 512 MiB, und `windows.defer_unmodeled` ist eingeschaltet.

## Wie der Einstieg bestimmt wird

Generation null ist das vom Gastlader abgebildete Image. Ein Übergang beginnt die Ausführung von Code, der neuer als der zuvor ausgeführte ist. `transfers` enthält RVA, `generation`, Stapelgleichheit (`stack_balanced`) und die Zugehörigkeit zum Eintrittsaufruf des Hauptprogramms (`program_invocation`). Vor diesem Aufruf wird mit dem Stapel des ersten Initialisierers verglichen.

1. Der Standardeintritt verlangt sowohl `stack_balanced` als auch `program_invocation`: Der Stub hat den Stapel des Hauptaufrufs zurückgegeben. `entry_source` ist `transfer`.
2. Vom OS aufgerufene TLS- und DLL-Callbacks werden erfasst, können aber auch bei ausgeglichenem Stapel kein Standardeintritt sein. Tiefere Gastaufrufe laufen ebenfalls weiter. Kein Callback wird übersprungen und keine Stubsignatur sagt den Eintritt voraus.
3. `transfer` wählt ausdrücklich eine Listenposition, einschließlich Initialisierungs-Callbacks und Zwischenstufen.

Aus der Gestalt von Compiler-Startcode wird kein Einstieg vorhergesagt. Ein Lauf, der vorher stoppt, meldet `no_entry` mit dem `stop_reason` des Prozesses.

## Das neu aufgebaute Abbild

Die Abschnitte behalten RVA, beobachteten Speicher einschließlich bereits ausgeführter Initialisierung und Seitenrechte. Der neue Abschnitt `.neverd` enthält das Importverzeichnis und neue IAT-Zellen für Exportaufrufe und Adressladevorgänge; ursprünglicher Nullspeicher bleibt erhalten. `origin` in `imports` unterscheidet aus der Eingabe gebundene `static`-Zellen und vom Gast geschriebene oder zur Reparatur ergänzte `runtime`-Zellen. Da Relokationen generierter Inhalte nicht beobachtet wurden, bleibt das Abbild an der beobachteten Basis, das Relokationsverzeichnis entfällt und `IMAGE_FILE_RELOCS_STRIPPED` wird gesetzt.

Vorhandene Laufzeit-IAT-Kandidaten müssen ein zusammenhängendes Exportzeigerfeld desselben Anbieters mit intaktem Nullabschluss innerhalb des Originalabschnitts bilden. Der Abschluss des benachbarten Anbieters genügt nicht.

Die TLS-Wiederherstellung prüft die Allokationsidentität des Loaders, eine vollständige nullterminierte Callback-Liste und beobachtete Aufrufe generierten Codes. Mehrere Kandidaten führen zu einem ausdrücklichen Fehler. Schutzprogrammspezifische Namen oder Bytes werden nicht verwendet. Bei der Verzeichnisauswahl hat die beobachtete Ausführung generierter Callbacks Vorrang vor einem abgeschlossenen Loader-Initialisierer.

`materialized_tls_callbacks` zählt TLS-Callbacks des Hauptimages, deren vom OS ausgeführte Prozessanbindungsaufrufe vor der Aufnahme des Hauptaufrufs normal zurückkehrten. Ihre Speicherwirkungen sind bereits im Abbild enthalten. Adapter im neuen Abschnitt `.neverd` vermeiden nur diesen wiederholten Aufruf und leiten andere Benachrichtigungen durch einen Endaufruf an das Original weiter. Verzeichnis, Callbackfeld und Adapter nutzen neuen Speicher; Originalbytes bleiben erhalten. Der Abschnitt ist mit Adaptern ausführbar und nur bei Bedarf neuer IAT-Zellen beschreibbar. Gastinterne Callbacks ohne diesen Abschlussnachweis bleiben unverändert. Beim Prozessstart stellt der erste Adapter außerdem die erfassten TLS-Bytes des Hauptthreads wieder her, wenn sie von der Vorlage abweichen. Fehlende TLS-Nachweise oder abweichende Größen führen zu einem ausdrücklichen Fehler. Die Originalvorlage bleibt für spätere Threads erhalten; andere Benachrichtigungen stellen den erfassten Block nicht wieder her.

Verzögertes Laden erlaubt ausführbare Callback- und Eintrittsziele in nullgefülltem Speicher, deren Code frühere Initialisierer erzeugen. Callbackfelder und TLS-Zuteilungsmetadaten benötigen weiterhin geprüfte Dateiinhalte; strenges Laden behält seine Dateiprüfung. Das OS-Modell liefert die Aufrufzugehörigkeit und benachrichtigt Beobachter beim Vorbereiten eines Aufrufs oder Wiederherstellen eines angehaltenen Aufrufers. An diesen Grenzen werden Übergangswachen neu gesetzt, auch wenn Callback und erzeugter Eintritt dieselbe Seite teilen.

`import_repair` umfasst zwei zusätzliche begrenzte Läufe nach der Einstiegserfassung: Ermittlung der Exportaufrufe und danach Beobachtung des Hilfsroutinenzustands. Jeder Lauf hat eigene Prozessgrenzen. Befehls- und Ereigniszähler werden sättigend addiert; `stop_reason` und Diagnose beschreiben den letzten Lauf, beobachtete Aufrufe nur die Ermittlung. Ohne Fortsetzungsadresse entfällt die Zustandsbeobachtung. Widersprüchliche Exportidentitäten verhindern das Umschreiben. Nur erreichte Pfade sind erfasst; `unpacked` bestätigt weder alle Imports noch erfolgreiche Programmausführung.

Die Ermittlung beobachtet Exportaufrufe einschließlich des nicht modellierten Exports, der die Ausführung stoppt, statt nur Protokolle modellierter Aufrufe zu verwenden. Eine unlesbare ABI-Rückkehrposition kann keinen Hilfsroutinennachweis begründen. Die Reparatur eines reinen Aufrufs erhält den ausdrücklichen Stopp bei nicht unterstützten Diensten.

Beobachtet werden höchstens 256 Kandidatenstarts in den 256 Bytes vor tatsächlichen Exportaufruffortsetzungen. `CALL rel32`, optional mit vorangestelltem GPR-PUSH/POP, kennzeichnet nur eine Beobachtungsgrenze; nachfolgende Bytes sind keine Signatur. Ein Aufruffenster von sechs bis acht Bytes wird nur dann zu `call [rip+IAT]`, wenn am API-Eingang genau eine Rücksprungadresse auf dem Stack liegt und andere Register (einschließlich Flags und SIMD), Mappings und dauerhafter RAM unverändert sind, ohne vorherigen OS-Aufruf. Führende NOPs erhalten die genaue Rücksprungadresse. Sieben oder acht Bytes werden zu `mov r64, [rip+IAT]`, wenn ein bekanntes Exportziel in genau einem GPR zurückkommt, bei ausgeglichenem Stack und denselben Erhaltungsbedingungen. Das Ergebnisregister wird aus Zuständen ermittelt: alle GPR außer RSP, einschließlich R8-R15; acht Bytes behalten ein abschließendes NOP. Nur temporärer Speicher unterhalb des Aufrufer-SP ist ausgenommen; sein Stack wird verglichen. Spätere unreine, ungelöste oder unvollständige Aufrufe verwerfen frühere Nachweise. Der ausgeführte Start muss belegt sein; kein REX-Präfix wird aus dem vorherigen Byte geraten, überlappende Starts mit gemeinsamer Rückkehr werden abgelehnt. `observed_loads` und `repaired_loads` zählen Ladeoperationen separat.

Die ursprüngliche Abschnittstabelle, die Anordnung des Importverzeichnisses und die Relokationstabelle werden nicht rekonstruiert; ein Packer stellt sie im Speicher nicht wieder her.

## Identifikation

Die Kompatibilitätsfelder `packer.kind` und `packer.evidence` liefern `unidentified` und ein leeres Array. Schutzprogrammregister, Pack-Header-Parser und Stub-Signaturen wurden entfernt. Die bisherige Identifikations-API validiert nur den Container.

## Grenzen

Nur EXE-Dateien werden ausgeführt, keine DLLs. Geprüfte Ausführung arbeitet befehlsweise. x64 Unicorn/KVM/WHP unterstützt außerdem `direct-user-x64-v1` mit Zeit- und Ereignisgrenzen ohne Befehlszählung. Die Verfolgung erfolgt in 4 KiB großen Seiten; Änderungen innerhalb einer bereits ausgeführten Seite derselben Generation ergeben keinen Transfer. Initialisierung vor dem Einstieg kann nicht übertragbaren externen Zustand erzeugen; erneut ausgeführte TLS-Callbacks können weitere Effekte haben. Nicht modellierte APIs stoppen ausdrücklich. Die Reparatur erfasst validierte x64-Aufruffenster von sechs bis acht Bytes und Adressladevorgänge von sieben oder acht Bytes; andere Formen und nicht erreichte Pfade bleiben ungelöst. Virtualisierter Code bleibt virtualisiert.

## Verifikation

`NeverDUnpackTests` prüft Container, sichere IAT-Allokation, Widersprüche und TLS-Ablehnung. `NeverDUnpackExecutionTests` prüft UPX NRV2B/NRV2D/NRV2E/LZMA und CRT über denselben generischen Pfad auf Unicorn/KVM/WHP, vergleicht mit dem unabhängig gelinkten Programm nach dessen Initialisierung und verlangt gleiche Ausgaben verfügbarer Backends. `UnpackGeneratedTests.cpp` erzeugt unabhängige x86-64/ARM64-Programme für Transfers, gestuftes Laden, Imports und direkte x64-KVM/WHP-Ausführung. Native Pflichtfälle dürfen in CI nicht übersprungen werden. `NeverDUnpackPublicTests` deckt C-ABI und CLI ab. `unittests/unpack/fixtures/Makefile` erzeugt die UPX-Beispiele erneut.
