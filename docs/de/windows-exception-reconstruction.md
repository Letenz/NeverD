**Sprachen**: [English](../windows-exception-reconstruction.md) | [简体中文](../zh-CN/windows-exception-reconstruction.md) | [繁體中文](../zh-TW/windows-exception-reconstruction.md) | [日本語](../ja/windows-exception-reconstruction.md) | [한국어](../ko/windows-exception-reconstruction.md) | [Français](../fr/windows-exception-reconstruction.md) | [Deutsch](windows-exception-reconstruction.md) | [Español](../es/windows-exception-reconstruction.md) | [Italiano](../it/windows-exception-reconstruction.md) | [Русский](../ru/windows-exception-reconstruction.md) | [العربية](../ar/windows-exception-reconstruction.md)

# Windows-Ausnahmerekonstruktion

[← Dokumentationsindex](README.md)

NeverD trägt tabellenbasierte Windows-Ausnahmeinformationen durch Laden, Lift,
Dekompilierung und binäres Umschreiben. Die Metadaten sind Teil des ausführbaren
Funktionsvertrags: Kann die Konsistenz von generiertem Code, Runtime-Function-
Records, Sprachtabellen und Guard-Tabellen nicht bewiesen werden, wird die
Umschreibung abgelehnt.

Es gibt drei Supportstufen:

- **Analyse**: Native Darstellung wird geprüft, normalisiert und der IR-Pipeline bereitgestellt.
- **Dekompilierung**: Reduzierbare Schutzbereiche werden explizite HighIR-
  Ausnahmeknoten; andere Formen behalten deterministische native Annotationen.
- **Native Rekonstruktion**: Patch mode lässt LLVM einen vollständigen Ersatzvertrag
  erzeugen und installiert ihn im finalen PE.

Analyse-Support bedeutet nicht automatisch native Rekonstruktion.

## Supportmatrix

| Native Form | Lift und Analyse | High-Level-Ausgabe | Patch mode |
|-------------|------------------|--------------------|------------|
| x64 unwind v1/v2 | Vollständige geprüfte Records, Operationen, Ketten, Handlerdaten und Provenienz | Frame/Unwind-Zusammenfassung und ggf. strukturierte Sprachbereiche | Vollständige primäre Records; erzeugte `.pdata`/`.xdata` ersetzen die alte Closure |
| x64 unwind v3/APX | Eigenes v3-Payload, Epiloge und Operationszählung | Explizite v3-Annotation | Nur Analyse; berührte Funktion wird abgelehnt |
| ARM32/ARM64 packed | Bereiche, packed Felder, primary/fragment identity | Frame/Unwind-Zusammenfassung | Nur vollständige primäre Records ohne Sprachhandler und einzeln adressierbare Fragmente |
| ARM32/ARM64 unpacked | Geprüfter xdata-Header/-Umfang, Handlerzuordnung und Fragmente | Frame/Unwind-Zusammenfassung | Nur vollständige primäre Records ohne Sprachhandler und einzeln adressierbare Fragmente |
| `__C_specific_handler` | Scopes, Filter, finally-Ziele, Handler und Fortsetzungen | Reduzierbar als `__try`/`__except`/`__finally`, sonst annotiert | Native x64-Rekonstruktion vollständiger darstellbarer Scope-Graphen |
| `__CxxFrameHandler3` | Unwind-/Try-Maps, Catches, Objekt-/Frame-Offsets, Fortsetzungen und IP-to-State | Reduzierbare Intervalle als C++ HighIR mit C-kompatiblen Typannotationen | Native x64-Rekonstruktion des unten beschriebenen engen verifier-clean Subsets |
| `__CxxFrameHandler4` | Begrenztes variables Decoding in den gemeinsamen C++-Graphen | Gleicher HighIR-Graph mit FH4-Provenienz | Nur Analyse; berührte Funktion wird abgelehnt |
| `__GSHandlerCheck_SEH/EH/EH4` | Wrapped Personality und geprüfte GS-Cookie-Provenienz | Basissprachgraph plus Wrapper-Annotation | Nur Analyse; Ablehnung ohne Downgrade |
| x86 Registration-Chain-EH | Geprüfte SEH-Ketten und C++-Graphen | HighIR-Regionen mit erhaltener Callback-Identität | PE32-Rekonstruktion der nachgewiesenen Teilmenge; siehe Detailvertrag |

Ein `try` innerhalb eines skalaren Referenz-`catch` kann das äußere lebende Objekt erhalten und dessen privaten Stack wiederherstellen. Gemeinsame Callbacks und allgemeine Objektlebenszeiten bleiben außerhalb der nativen Teilmenge. [PE32 C++](../windows-exception-reconstruction.md).

Malformed Records gelten nie als vollständig. Partial Decoding bleibt zur
Inspektion nutzbar, berechtigt aber nicht zur nativen Generierung. Beweist ein
ARM-xdata-Header trotz beschädigtem Unwind-Body noch einen begrenzten ausführbaren
Fragmentbereich, bleibt dieser für Disassembly erhalten; der Record bleibt
malformed und nicht patchbar.

## Normalisiertes Modell

`ExceptionInfo` gehört `BinaryImage`. Jede `ExceptionFunction` enthält:

- einen geprüften halboffenen Codebereich;
- primary-, chained- oder fragment-Identität;
- native Unwind-Codierung und exakte Runtime/Unwind-Provenienz;
- normalisierte Operationen/Epiloge samt opaken Operanden unbekannter Operationen;
- exakte Personality-Identität und Handlerdaten;
- optionale SEH-Scopes, C++-State-Maps und GS-Cookie-Daten;
- `Complete`-, `Partial`- oder `Malformed`-Status und deterministische Diagnosen.

Der Loader gibt keine rohen Dateizeiger frei. Native RVAs dienen Diagnose und
Ersatz; IR-Verbraucher arbeiten nur mit validierten VAs und Bereichen.

Der bildweite Index erlaubt überlappende chained/fragment Records und liefert
die spezifischste Funktion. Fehlerhafte Verzeichnisse, Bereiche, Zeiger, Zähler,
Zustände, komprimierte Zahlen, Kettenzyklen oder erschöpfte Decode-Budgets senken
den Parse-Status.

Limits gelten pro Tabelle und für den gesamten Funktionsgraphen. Wiederverwendete
Handler-Maps können den Gesamtaufwand nicht vervielfachen. FH3-Records mit
gemeinsamem `FuncInfo` und gleicher Personality bilden eine begrenzte Gruppe:
Catch-Funclets des Parents sind erlaubt, fremde Runtime Functions nicht.

Schmale Ganzzahlargumente behalten ihre nachgewiesene Breite von 8, 16 oder 32 Bit in ECX/EDX;
physische Stack-Slots bleiben 32 Bit breit. Die Quellbefehle bestimmen Vorzeichen- und Nullerweiterung.
Ladevorgänge und lokale Sicherungen vor der FS-Veröffentlichung müssen ESP, EBP und alle
Registrierungsfelder erhalten.

Generierte PE32-C++-Einstiege lassen sich bei vollständigem Quellnachweis erneut laden, liften und rekonstruieren.
Der Loader prüft den exakten FS:[0]-Store, den neu ausgerichteten Frame und die SafeSEH-Zeigerrollen anhand der Bytes.
Die Installation darf einen authentifizierten Einstieg in einer zuvor erzeugten ausführbaren Section patchen,
wenn virtuelle und physische Speicherung eindeutig sind und den analysierten Bytes entsprechen.
Die cdecl/stdcall/thiscall/fastcall-Fixtures prüfen zwei Generationen, beide Patch-Modi und erzwungenes Rebasing
mit der Microsoft-x86-CRT. Jede Generation benötigt neue LowIR- und LLVM-Nachweise; alte Belege erteilen keine
Schreibberechtigung. Generierte ESI-Cleanup-Relays können erneut rekonstruiert werden, wenn ihre Rahmenbasis und der
gespeicherte Eintritts-EBP zum authentifizierten Elternrahmen passen. Der zwischengespeicherte Bytenachweis
wird bei jedem Aufruf neu gebunden; LowIR prüft Objektzugriffe und Rahmenlebenszeit. Echte MSVC-Fixtures
prüfen Wert- und Referenz-Catches, Destruktorreihenfolge und Rückschreiben über zwei Generationen, beide
Patch-Modi und erzwungene Basisadressen. Allgemeine Objektlebenszeiten und Typkonvertierungen bleiben offen.

Trivial kopierte PE32-Ausnahmeobjekte unterstützen Wert- und Referenz-catches, direkte throws
und erneutes Werfen. Unveränderliches ThrowInfo muss genau einen Typ ohne Adressanpassung,
Kopierkonstruktor oder Weiterleitungs-Callback beschreiben. Jedes Byte muss initialisiert
sein; private Frame-Zeiger dürfen nicht entweichen. O0/O1 prüfen Strukturen mit 8 und 12 Bytes,
Feldänderungen, catch-Verzweigungen, beide CLI-Modi und Relokation mit Microsoft CRT. PHI-Kopien
behalten ihre CFG-Kante und Callback-Zuordnung. Die unabhängige Prüfung lehnt zusätzliche Aufrufe,
Speicheraliasse und geänderte Ziele ab. RTTI liefert kein Memberlayout: C++-Syntaxprüfungen nutzen
die echten Testdeklarationen; native Offsets und unbekannte Aggregat-throw-Aufrufe bleiben erhalten.

Ein optionaler Destruktor benötigt einen unabhängigen LowIR-Nachweis als zurückkehrende
Blattfunktion mit privatem Stack und begrenzten ECX-Objektzugriffen. Seine Codebereiche und
Speichereffekte bleiben Teil des erhaltenen Aufrufvertrags. Die O0/O1-Tests prüfen Anzahl,
Reihenfolge und Objektinhalt bei Zerstörung, erneutem Werfen und zwei Rekonstruktionen.

Nichttriviale Laufzeit-Kopierkonstruktoren benötigen einen LowIR-Nachweis mit getrennten
Adressbereichen für das ECX-Ziel und den Quellzeiger auf dem Stack. Die Quelle bleibt
schreibgeschützt; Zugriffe sind begrenzt, jeder Rückkehrpfad initialisiert das gesamte Ziel
und endet mit `RET 4`. Zeiger dürfen nicht entweichen; der CRT-Aufrufer darf nicht beobachtet
und kein ungeprüftes Ziel aufgerufen werden. O0/O1 prüfen vier Kopien und zwölf Zerstörungen
je Ausführung über zwei Generationen. Unbekannte Aggregate behalten ursprüngliche
Initialisierung und Throw-Aufruf; RTTI rechtfertigt kein `throw Type()`. Allgemeine
Konstruktor-Aufrufgraphen, Konvertierungen, virtuelle Basiskopien und allgemeine
Destruktor-Aufrufgraphen bleiben ausgeschlossen.

## IR-Vertrag

Exception-Metadaten durchlaufen alle Repräsentationen, ohne den normalen CFG zu ändern:

- LowIR trennt Blöcke an Bereichsgrenzen, State-Transitions, Filtern, Handlern,
  Cleanup-Aktionen und Fortsetzungen.
- Exception-Successors/-Predecessors bleiben von normalen Kanten getrennt.
- MedIR behält normalisierte Deskriptoren und stabile Exception-Kanten.
- HighIR unterscheidet `SEHTry` und `CxxTry` und bewahrt VAs, Typen, Adjektive,
  Objekt-/Frame-Offsets, Aktionen, Zustände und Fortsetzungen.

Der HighIR-Structurer ist intervallkonservativ: Nur vollständig in einem
kompletten Schutzbereich liegende zusammenhängende Statements werden bewegt,
verschachtelte Bereiche von innen nach außen. Kreuzungen, partielle Graphen,
mehrdeutige Grenzen und out-of-line Funclets bleiben im ursprünglichen Kontrollfluss.

Der C-Backend erzeugt MSVC-SEH-Syntax für reduzierbare Ein-Klausel-Bereiche.
Da HighC ein C-Backend ist, werden C++ Catches/Cleanups als deterministische
C-kompatible Kommentare ausgegeben, nicht als angeblich kompilierbares C++.

## LLVM-Metadatenschema

Jede analysierte Exception Function erhält verlustfreie Metadaten, auch ohne
natives WinEH-Lowering:

- Attachment `neverd.windows.eh`;
- nativer Marker `neverd.windows.eh.native`;
- Modultabelle `neverd.windows.eh.functions`;
- Schemaversion `3`.

Der feste Record bewahrt Status, Encoding, Bereich, native Runtime/Unwind-RVAs,
Recordtyp/Kette, packed Wort, Frame, Personality-Namen, Handler, Unwind-Bytes,
Operationen/Epiloge, SEH-Scopes, C++-Maps, GS-Daten, Diagnosen und Regeneration.
Patch-Validierung verlangt exakte Version und exakte Übereinstimmung mit dem Image.

Natives x64-SEH-Lowering verwendet LLVM WinEH und erzeugt nur für vollständig
darstellbare Scope-Graphen verifier-clean `invoke`/Funclet-Fluss. FH3 verlangt:

- x64 COFF, unwind v1/v2, vollständige Metadaten, gültiger synchroner FH3-Graph;
- kein `noexcept`, async, separated-funclet, GS, FH4 oder unbekannte Flags;
- verschachtelte oder disjunkte, nie kreuzende Intervalle;
- keine Destruktor-/Unwind-Aktion, Catch-Objekterzeugung oder Parent-Frame-Abhängigkeit;
- Handler in einem predecessor- und call-freien normalen Block;
- LLVM `invoke` für jede geschützte potenziell unwindende Operation.

Andernfalls bleibt die IR analysierbar und verlustfrei, aber nativer Ersatz wird
abgelehnt. PE Entry Point, TLS-Callbacks und CRT-Wurzeln sind Erhaltungsgrenzen.

## Patch-Transaktion

Eine unterstützte Umschreibung ist eine einzelne PE-Transaktion:

1. Jede betroffene Funktion gegen geladenen Graphen und LLVM-Metadaten validieren.
2. Code unter Erhalt von Section-Identität, Alignment, Traits und semantischen
   Referenzen kompilieren; lokale Windows-Personality externalisieren und xdata
   an den nachgewiesenen ursprünglichen ausführbaren Handler binden.
3. Unberührte Runtime Functions behalten und die gesamte ersetzte Closure samt
   chained Records entfernen.
4. Code/xdata relokieren, pdata mergen/sortieren, Overlaps ablehnen, Personality-
   Abdeckung beweisen und ein einziges Ersatzverzeichnis installieren.
5. CFG-Modus behalten, `.gfids`/`.gehcont` auflösen, Guard CF/EH-Tabellen mergen
   und load-config aktualisieren. Unaufgelöste Helper brechen ab; CFW,
   Return-Flow Guard, Retpolines und XFG bleiben analysis-only.
6. Das fertige Byte-Image vor dem Schreiben erneut parsen.

Die LLVM-Fork-Erweiterung bleibt generisch: Der Final-Image-Writer bewahrt
Section-Traits und Symbolreferenzen. PE/MSVC-Parsing, Policy, Merging,
load-config und Endvalidierung bleiben in NeverD.

Originale Guard-CF/EH-Continuation-Einträge bleiben erhalten, weil ihre
Trampoline gültige indirekte Ziele bleiben. Erzeugte Ziele müssen in emitted
code liegen; Ergebnistabellen sind strikt nach RVA sortiert.

## Final-Image-Validierung

Ein gepatchtes PE wird abgelehnt, wenn nicht alles gilt:

- LLVM akzeptiert COFF und Machine, Klasse, Sections, Directories, Base und Extent stimmen;
- raw/virtuelle Section-Bereiche sind begrenzt und überschneiden sich nicht;
- Exception Directory ist file-backed und im Image;
- Runtime Functions sind sortiert, nicht leer, nicht überlappend und ausführbar;
- x64 Unwind-RVAs, Header, Versionen, Flags, Handler und Ketten sind gültig;
- finale Imports, Exports und COFF-Symbole erlauben erneutes SEH/FH3-Parsing;
- ARM-Records/xdata enthalten unterstützte Versionen und Bereiche;
- Guard-Felder existieren, wenn Flags Tabellen ankündigen;
- Guard-Pointer/Counts/Strides liegen in Datei/Image, alle Ziele sind sortiert und ausführbar.

Fehler brechen die Transaktion ab; ein Best-Effort-Image wird nie geschrieben.

## Fokussierte Verifikation

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

Das geschützte x64-Fixture wird mit `/guard:cf` und `/guard:ehcont` gebaut und
prüft SEH-Scopes, Guard-Tabellen, HighC, Patch, Reload, Sortierung und Ziele. Das
FH3-Fixture prüft feste Tabellen, Annotationen, Personality, Try/Catch und
IP-to-State. Bei Parseränderungen sind auch ARM-Formatfälle auszuführen.

## Erweiterung nativen Supports

Neue native Formen müssen im selben Change enthalten:

- vollständigen begrenzten Parser und Modellinvarianten;
- HighIR-/LLVM-Metadata-Roundtrip;
- verifier-clean native IR für jede neue Graphform;
- notwendige Section-/Referenzerhaltung;
- gelinktes PE-Fixture für exakte Architektur/Personality/Version;
- Exception-Directory-, load-config- und Final-Image-Validierung;
- explizite Ablehnungstests benachbarter nicht unterstützter Formen.

Decodierbarkeit allein erweitert niemals die Allow-List. Entscheidend ist die
Erhaltung des Runtime-Ausnahmeverhaltens im final gelinkten Image.

Das Cleanup eines ausgewählten Handlers darf zum selben oder zu einem anderen gültigen Capture im Elternframe springen und den Skalarwert ersetzen (null wird zu eins). Ziel und Wert bleiben mit ihren Frame-Effekten verbunden; die Fortsetzung stellt die Register, ESP und Unwind-Grenze des tatsächlichen Ziels wieder her. Ausgesetzte und verschachtelte Finally-Aufrufe werden beendet. MedIR bindet jede Ersetzung an die genaue Callback-Aufrufstelle und das Capture und verlangt abgeschlossene Pfade in einem azyklischen Transfergraphen. Beide Puffer müssen während aller verbleibenden Cleanups gültig bleiben; überlappende Schreibzugriffe, abgelaufene Puffer, unbelegte Zyklen und entweichende Frame-Adressen werden abgewiesen.

Ein durch RaiseException in einem unterbrochenen Finally aufgerufener Suchfilter kann zum selben oder zu einem anderen gültigen Capture springen oder auf einem anderen Zweig zurückkehren. Rückkehr und Sprung behalten getrennte Frame-Ergebnisse; der Sprung beendet den Filter und alle unterbrochenen verschachtelten Finally-Aufrufe und bewahrt sein eigenes Ziel und seinen Wert.

Endliche Schleifen in Finally-Callbacks und Suchfiltern behalten pro Ausführung den vollständigen Frame, einschließlich Registerzählern, Elternframe und privatem Callback-Stack. Wiederkehrende vollständige Zustände, unbelegte Terminierung und erschöpfte Budgets werden abgewiesen. PHI-Kopierblöcke müssen zur exakten Quellkante und zum selben Callback gehören; der Installer prüft den Escape-Index jedes wiederhergestellten Skalarslots und sämtliche Aliase auf private, nichtatomare Zugriffe.

Ein geprüftes SEH3/EH4-finally mit festem Frame darf SavedESP wiederherstellen
und direkt zu einer bereits dem Elterncode zugeordneten Fortsetzung springen.
Ein privater SjLj-Kontext stellt den erzeugten Stack und FS:[0] wieder her,
bewahrt aktuelle logische Register und Frame-Schreibzugriffe des Callbacks und
führt zur exakten Quell-Scope-Grenze. Der Installer prüft Aliase, Erfassung,
Transfer und Auswahl unabhängig; Quellpuffer dürfen diesen Kontext nicht ersetzen.
Ein Runtime-Ausstieg kann den temporären CRT-Unwind-Knoten zurücklassen. Vor einem
weiteren Aufruf oder einer möglicherweise fehlerauslösenden Operation muss der
Quellcode seine aktive Registrierung erneut verknüpfen oder sie bei der Rückkehr
entfernen. Unbewiesene Kettenreparaturen und Ziele außerhalb des geprüften
Eltern-CFG werden abgelehnt.

Eine Fortsetzung darf ausschließlich vom finally-Sprung erreichbar sein, wenn
jeder dekodierte Pfad ihres azyklischen Endstücks in bereits zugeordneten
Elterncode mündet, ohne einen filter- oder finally-Einstieg zu durchqueren.
Dieser physische Nachweis verleiht keine Speicherrechte eines Runtime-Handlers;
SavedESP-Transfer und erreichende Zustände werden unabhängig geprüft. Eine
unverankerte Rückkehr oder ein Zyklus begründet keine Elternzuordnung.


Gewöhnliche finally-Aufrufe übertragen EAX/ECX/EDX/EBX/ESI/EDI explizit über
private Speicherplätze im Elternframe, einschließlich der Werte beim physischen
Nah-Rücksprung. Der Laufzeiteinstieg behält seinen eigenen Registervertrag und
übernimmt keine Eingaben eines früheren gewöhnlichen Aufrufs. Ein ausschließlich
durch einen nachgewiesenen gewöhnlichen finally-Aufruf referenzierter Scope
bleibt erhalten, auch wenn ihn kein try level des Quellcodes aktiviert.

Die Rekonstruktion von `nonlocal_finally_return` aus echtem MSVC x86 wird mit `/Od` und `/O2`, `/GS-` und `/GS`, optimiertem und unoptimiertem Quell-LLVM, beiden Patch-Modi und zwei erzwungenen Basisadressen geprüft. Ein äußerer SEH-Beobachter kontrolliert normale und außergewöhnliche Rückgaben, Stack-Wächter und die Wiederherstellung der Kette. Der GS-Pfad behält den ursprünglichen Wrapper zur Normalisierung des Ausnahmecodes bei, nachdem bewiesen wurde, dass sein skalarer Helfer den Aufrufer-Frame nicht beobachten kann.

Die direkte Rückkehr aus der Elternfunktion unter `/O2` erfordert den Nachweis des benachbarten physischen Aufrufs, der Wiederherstellung des gespeicherten ESP, der ursprünglichen Kettenentfernung und des vollständigen Eltern-Epilogs. LowIR beweist diese Quelleffekte, MedIR bindet die verbleibenden Operationen und der COFF-Installer prüft den Vertrag unabhängig erneut. Ungenutzter synthetischer Registertransport fügt keine physischen Elternparameter hinzu; beobachtete Werte bleiben Abhängigkeiten. Die geprüfte nichtlokale MSVC-`goto`-Form verwendet den folgenden Vertrag für vollständige Abwicklung.

Geprüfte Aufrufe von `_local_unwind2` und `_local_unwind4` behalten die genaue Quelloperation, den authentifizierten Import-Slot, das tatsächliche Argument des Elternframes, die vorzeichenlose Abwicklungsgrenze und die Zustandspaare bei der Rückkehr. EH4 verlangt außerdem den Nachweis der Cookie-Adresse und der initialisierten Frame-Cookies. Überschriebene Importdaten, bedingte Aufrufe oder eine abweichende Quellidentität entziehen den Nachweis. MedIR rekonstruiert genau zwei oder drei Stack-Argumente, ohne zufällig lebende Register zu Argumenten zu machen. Der native Ersatz unterstützt vollständige Abwicklung bis EH3 `-1` oder EH4 `-2`, nachdem die physische Elternframe-Brücke, die SavedESP-Wiederherstellung und die Quelloperationen unabhängig gebunden wurden. Echtes MSVC `nonlocal_finally_goto` wird mit `/Od`, `/O2`, `/GS-`, `/GS`, unverändertem und optimiertem LLVM, beiden Patch-Modi und zwei erzwungenen Basisadressen ausgeführt. Teilgrenzen und die Fortsetzung innerhalb aktiver äußerer Scopes benötigen weiterhin zusätzliche native Verträge.
