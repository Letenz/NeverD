**Sprachen**: [English](../unpack.md) | [简体中文](../zh-CN/unpack.md) | [繁體中文](../zh-TW/unpack.md) | [日本語](../ja/unpack.md) | [한국어](../ko/unpack.md) | [Français](../fr/unpack.md) | [Deutsch](unpack.md) | [Español](../es/unpack.md) | [Italiano](../it/unpack.md) | [Русский](../ru/unpack.md) | [العربية](../ar/unpack.md)

[← Dokumentationsindex](README.md)

# Entpacken gepackter ausführbarer Dateien

`neverd unpack` stellt das Programm wieder her, das eine gepackte ausführbare Datei in ihrem eigenen Adressraum neu aufbaut. Der Befehl führt die Eingabe als begrenzten Gastprozess aus, beobachtet, wo der Stub die Kontrolle an den von ihm erzeugten Code übergibt, und schreibt dieses Abbild als neue Datei desselben Containers. Er devirtualisiert nicht: Funktionen, die ein Schutzprogramm virtualisiert hat, bleiben virtualisiert. Der Build benötigt `NEVERD_ENABLE_CPU_EMULATION=ON`.

## Unterstützte Eingaben

Der Container bestimmt, wie eine Datei validiert und neu aufgebaut wird, der Befehlssatz bestimmt, wie ein Transfer beurteilt wird, und beide bestimmen das Gastprozessprofil. Eine Eingabe außerhalb dieser Tabelle wird namentlich abgelehnt, bevor irgendetwas ausgeführt wird.

| Container (`format`) | Befehlssatz | Gastprofil | Stub-Wissen |
| --- | --- | --- | --- |
| PE32+ (`pe64`) | x86-64 | [`windows-pe64-v1`](process-emulation.md) | UPX |
| PE32+ (`pe64`) | ARM64 | [`windows-pe64-v1`](process-emulation.md) | keines; nur Beobachtung |

## Verwendung

```bash
neverd unpack packed.exe -o unpacked.exe
neverd unpack packed.exe -o unpacked.exe \
  --options='{"backend":"unicorn","instruction_limit":400000000,"transfer":2}'
```

Der Befehl gibt einen JSON-Bericht aus. Exit-Code 0 bedeutet, dass das Abbild geschrieben wurde, 3, dass der begrenzte Lauf endete, bevor ein Einstieg akzeptiert wurde (`outcome` ist `no_entry`, und es wird nichts geschrieben), und 1 steht für eine ungültige Eingabe oder Option oder eine fehlgeschlagene Vorbereitung. Der Bericht nennt `format`, `architecture` und `profile`, die tatsächlich ausgeführt wurden. Der C-Einstiegspunkt ist `neverd_unpack_json`; Python stellt `Session.unpack` bereit. Die Optionen sind die [Prozessoptionen](process-emulation.md) zuzüglich `transfer`. Die Vorgaben weichen dort ab, wo ein Stub mehr Ressourcen braucht: 100000000 Befehle, 600 Sekunden und 512 MiB, und `windows.defer_unmodeled` ist eingeschaltet.

## Wie der Einstieg bestimmt wird

Generation null ist das Abbild, wie es der Gastlader abgebildet hat. Ein Befehl, dessen Bytes von diesem Abbild abweichen, wurde vom Prozess erzeugt. Ein Transfer ist die erste Ausführung von Code, der neuer ist als der gerade laufende; `transfers` führt jeden mit seiner RVA, seiner `generation` und der Angabe auf, ob der Stapelzeiger seinem Wert beim Prozesseinstieg entspricht (`stack_balanced`).

1. Ein Transfer auf dem Einstiegsstapel ist der Einstieg des Programms: Der Stub hat den erhaltenen Stapel zurückgegeben. `entry_source` ist `transfer`.
2. Ein Transfer auf einem tieferen Stapel ist ein Aufruf des Stubs in das Programm, etwa ein TLS-Callback. Er wird gemeldet, aber nicht akzeptiert. Nennt der identifizierte Stub das Ziel seines letzten Sprungs, wird das Abbild bei diesem Aufruf neu aufgebaut, bevor irgendein Code des Programms gelaufen ist, und die genannte Adresse ist der Einstieg. `entry_source` ist `stub`.
3. `transfer` wählt einen aufgeführten Transfer ausdrücklich anhand seiner Position, für Schutzprogramme, die stufenweise entpacken oder ihr Programm aufrufen.

Aus der Gestalt von Compiler-Startcode wird kein Einstieg vorhergesagt. Ein Lauf, der vorher stoppt, meldet `no_entry` mit dem `stop_reason` des Prozesses.

## Das neu aufgebaute Abbild

Abschnitte behalten ihre RVAs und enthalten den beobachteten Speicher; jeder Abschnitt hat den Zugriff, den seine Seiten beim Transfer hatten. Ein letzter Abschnitt `.neverd` enthält ein neues Importverzeichnis über den Zellen, über die das Programm ohnehin aufruft, sodass weder Code noch Daten verschoben werden. `imports` führt jede Zelle mit ihrem `origin` auf: `static`-Zellen hat der Lader aus dem Verzeichnis der Eingabe gebunden, `runtime`-Zellen hat der Stub geschrieben. Das Abbild ist auf seine beobachtete Basis festgelegt: Relokationen des erzeugten Inhalts wurden nicht beobachtet, daher wird das Relokationsverzeichnis entfernt und `IMAGE_FILE_RELOCS_STRIPPED` gesetzt. Bei UPX wird wieder auf das programmeigene TLS-Verzeichnis verwiesen, weil das gepackte nur den Handler des Stubs erreicht.

Die ursprüngliche Abschnittstabelle, die Anordnung des Importverzeichnisses und die Relokationstabelle werden nicht rekonstruiert; ein Packer stellt sie im Speicher nicht wieder her.

## Identifikation

`packer.kind` benennt ein Schutzprogramm nur anhand von Belegen in der Datei. UPX erfordert zwei von `upx_section_names`, `upx_pack_header` (Magic, Format, Methode und Prüfsumme) und `upx_entry_stub`. Eine nicht identifizierte Eingabe wird dennoch durch Beobachtung entpackt.

## Grenzen

Unterstützt werden nur ausführbare Dateien; DLLs werden nicht ausgeführt. Die geprüfte Ausführung lässt jeweils einen Befehl zu, in der Größenordnung von 10^5 pro Sekunde, sodass ein Stub, der Milliarden von Befehlen benötigt, jedes praktikable Budget übersteigt. Die Ausführung wird je Seite von 4 KiB verfolgt: Code, der in eine Seite geschrieben wird, die bereits Code derselben Generation ausführt, wird nicht als Transfer gemeldet. Code des Programms, der vor dem Einstieg läuft, etwa ein TLS-Callback, der eine nicht modellierte API aufruft, stoppt den Lauf, sofern der Stub seinen Einstieg nicht deklariert. Ein VMProtect-Lader wird entpackt, wenn jeder geschützte Importaufruf noch eine Stelle von sechs Byte ist, die einen aufgelösten Export per Tailcall erreicht. Diese Stellen werden als gewöhnliche Importaufrufe neu geschrieben. Virtualisierter Code bleibt virtualisiert.

## Verifikation

`NeverDUnpackTests` prüft die Identifikation. `NeverDUnpackExecutionTests` entpackt eingecheckte UPX-Testdateien (NRV2B, NRV2D, NRV2E, LZMA und ein Programm mit C-Laufzeitbibliothek) auf Unicorn, KVM und WHP, vergleicht jeden Abschnitt mit dem Original, führt das wiederhergestellte Abbild aus und verlangt identische Bytes von jedem Backend. `UnpackGeneratedTests.cpp` packt innerhalb des Tests ein Programm für x86-64 und ARM64 und prüft gegen die gelinkte Datei einen Lader, der direkt weiterspringt, einen Lader, der zuerst in das Programm aufruft, und einen zweistufigen Lader. `NeverDUnpackPublicTests` deckt die C-ABI und die CLI ab. `unittests/unpack/fixtures/Makefile` erzeugt die UPX-Testdateien neu.
