**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: 817e1308a48a9a751b9ef618b8094d4082fed99234c2b7381269f524196f5c80 -->

[← Documentation Index](README.md)

# Quellcode- und native Objektpatches

NeverD kompiliert die Ersatzfunktion mit dem ausgewählten Compiler und installiert sie am ausdrücklich angegebenen ursprünglichen Funktionseinstieg. Der Compiler bestimmt die C/C++-Semantik; NeverD übernimmt Adressbindung, Relokationen, Speicher, Unwind-Daten und Veröffentlichung. Binärreparatur und Umschreiben gelifteten Codes behalten ihre eigenen Pipelines. Das übrige Programm wird nicht neu gebaut.

<!-- i18n-section: compiler -->
## Compiler auswählen

Wählen Sie `--compiler=clang`, `--compiler=gcc` oder `--compiler=neverc`. Clang und NeverC im normalen Modus erzeugen LLVM IR; `object` erhält native Maschinenbefehle. Die CLI wählt für GCC standardmäßig Objekte. Im Python-SDK ist für GCC `artifact="object"` ausdrücklich erforderlich. Ein anderer Compiler wird niemals stillschweigend eingesetzt.

Ersetzen Sie Beispieladressen durch bestätigte ursprüngliche Funktionseinstiege. Die Deklaration muss zur ursprünglichen Maschinen-ABI passen, einschließlich Aufrufkonvention und Strukturaufbau; eine Adresse beweist dies nicht. C++ benötigt `--language=c++` und das passende Clang++/G++. Verwenden Sie einen `extern "C"`-Einstieg oder den exakten dekorierten Symbolnamen. Zusätzliche Hilfsfunktionen sind erlaubt. `--entry` und `--func` wählen den Ersatz ausdrücklich; auch das ältere `--from-c` beachtet die Adresse.

```sh
# Clang C -> LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.clang

# NeverC ordinary C -> native object
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC -> native object (CLI default for GCC)
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/gcc \
  --patch-report -o app.gcc

# Independently compiled ELF x64 object
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

<!-- i18n-section: profiles -->
## Geprüfte Profile und Grenzen

| Compiler | Artefakt | Geprüftes OS-Laufzeitprofil |
|---|---|---|
| Clang 22.1.6 | LLVM IR | ELF x64 C/C++; Mach-O x64/ARM64 C/C++; PE x64 C/C++ FH3 |
| Clang 22.1.6 | Natives Objekt | ELF x64 C/C++ |
| GCC/G++ 13.3.0 | Natives Objekt | ELF x64 C/C++ mit kompatibler Itanium-Laufzeit |
| NeverC 3389.1.5 normaler Modus | LLVM IR | ELF x64 C; Mach-O x64/ARM64 C |
| NeverC 3389.1.5 normaler Modus | Natives Objekt | ELF x64 C |

Dies sind geprüfte Profile, keine Zusage für alle Compilerversionen oder Optionen. `dyncode` und NeverC C++ sind nicht zugelassen. GCC erzeugt kein LLVM IR. Native COFF/Mach-O-Objekte sind nicht zugelassen; die Objektinstallation erfordert ELF x64. Unbekannte Relokationen, TLS, Konstruktoren, LTO, common/IFUNC-Symbole und unaufgelöste Imports schlagen ausdrücklich fehl. Der JSON-Beleg enthält Compiler, Argumente, Bindungen und installierte Adressen; native Objekte enthalten außerdem SHA-256.

Die separate CPU-Matrix prüft vier Architekturen (`x86`, `x64`, `ARM32`, `ARM64`) und drei Formate (`ELF`, `PE`, `Mach-O`) auf Quellcodeinstallation oder ausdrückliche Ablehnung. Isolierte CPU-Ausführung bestätigt weder den OS-Loader noch C++-Unwinding oder beliebige ABIs. Dafür gelten die oben angegebenen Laufzeitprofile.

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `REFUSED` | `CPU` |
| `PE` | `REFUSED` | `CPU` | `REFUSED` | `REFUSED` |
| `Mach-O` | `REFUSED` | `CPU` | `REFUSED` | `CPU` |

`CPU` bedeutet Installation und isolierte Funktionsausführung; `REFUSED` bedeutet eine ausdrückliche Ablehnung wegen fehlendem Installationsvertrag. ARM32 ELF benötigt EHABI-Registrierung; 32-Bit-Mach-O benötigt geprüfte Loader-, Frontend- und Unwind-Verträge; PE-Quellcodeinstallation erfordert derzeit x64.

Die CPU-Matrix verwendet eigenständige Arithmetik, Verzweigungen und Hilfsaufrufe. Sie bestätigt keine ELF-x86/ARM64-Datenreferenzen oder ARM64-C++-Imports; zusätzliche Laufzeitproben lehnten deren nicht unterstützte Relokationsverträge ab.

<!-- i18n-section: references -->
## Ursprüngliche Referenzen und Speicher

Bindungen beziehen sich auf externe Deklarationen: `function` verlangt einen bestätigten ursprünglichen Funktionseinstieg oder Import-Stub; `data` verlangt schreibbaren Speicher; `const` verlangt eine konstante IR-Deklaration. Objektdaten benötigen eine Ausdehnung ungleich null. Bereiche dürfen keine Speichergrenzen überschreiten oder RELRO schreibbar machen. Rückrufe in den ersetzten ursprünglichen Funktionskörper werden abgelehnt; dieses Profil hat kein Trampolin mit gesichertem Prolog.

Neuer Code bleibt RX, veränderlicher Zustand RW. Nur vorhandene Imports werden wiederverwendet; GOT/IAT-Slots und aufrufbare Stubs bleiben unterschiedlich. Mach-O benötigt freien Platz für Ladebefehle, etwa `-Wl,-headerpad,0x400`; PE benötigt freie Abschnittskopfplätze. Zu wenig Platz führt zum Fehler. ELF `.data.rel.ro` und ursprünglicher CET/IBT-Schutz benötigen zusätzliche Verträge und werden derzeit abgelehnt.

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

Wachsende Mach-O-Unwind-Tabellen benötigen zusätzlich freien angrenzenden Datei- und virtuellen Speicher; Header-Padding allein reserviert ihn nicht. Die geprüften Eingaben behandeln beide Anforderungen getrennt.

<!-- i18n-section: exceptions -->
## C++-Ausnahmen

ELF verwendet Compiler-FDE/LSDA; Mach-O verwendet compact unwind/DWARF. PE x64 akzeptiert Compiler-FH3-Tabellen mit vorhandener kompatibler MSVC-Laufzeit und passenden Typidentitäten. Type-info-Daten können ausdrückliche Bindungen erfordern. FH4, eigene Personalities, SEH-Quellcode und aktive CFG/EHCont/XFG-Modi werden abgelehnt; ursprüngliche Schutzflags bleiben erhalten.

Laufzeitfälle prüfen normale Rückkehr, internes catch, neue Ausnahmen im ursprünglichen Aufrufer, Ausnahmen eines ursprünglichen Aufgerufenen durch den Ersatz, genau einmalige RAII-Bereinigung, erneutes Werfen, noexcept-Beendigung und unveränderte Funktionen. Die ursprüngliche Aufrufstelle muss Weitergabe bereits zulassen. Entfernte catches um eine bewiesen nicht werfende Funktion kann ein Patch nicht wiederherstellen.

<!-- i18n-section: publication -->
## SDK und Veröffentlichung

Die additive C-API ist `neverd_patch_source` mit `neverd_source_patch_options_v1`; `neverd_patch_source_report` liefert den Beleg. Objektinput benötigt seine Bytelänge. Die ältere API `neverd_patch_from_c` bleibt verfügbar.

Eingaben werden als Snapshot gesichert. Der Kandidat entsteht neben dem Ziel, wird erneut geladen und vor exklusiver Veröffentlichung gegen jeden Einstieg geprüft. Vorhandene Dateien und Symlink-Aliasse werden nicht überschrieben. Eine fehlende Zuordnung lässt die ganze Anfrage scheitern. `--mode=inplace` wird abgelehnt. IR-Transformationen der Sitzung gelten nicht für native Objekte. Mach-O prüft und erneuert Signaturen strikt; PE-Signaturen werden nicht stillschweigend entfernt.

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

<!-- i18n-section: verification -->
## Build und Prüfung

Wählen Sie über `NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` einen kompatiblen Compiler (Clang 22.1.6 geprüft). Der integrierte Build korrigiert nur eine LLVM-Kopie im Buildverzeichnis; Submodul und gemeinsamer Cache bleiben unverändert. Der x64-PLT-Fehler der r3/r4-Pakete kann IR-Patches verhindern; native Objekte umgehen diesen MC-Pfad. `NEVERD_TEST_REQUIRE_FIXED_LLVM=1` verlangt die exakte korrigierte PLT-Erzeugung.

`NeverDSourcePatchTests` enthält native C/API/Objekt/EH-Fälle; `NeverDSourcePatchMatrixTests` prüft alle Kombinationen mit Unicorn. `SourcePatchPE` führt sieben C++-Szenarien mit `NEVERD_TEST_WINDOWS_CRT_LIB`, `NEVERD_TEST_WINDOWS_UCRT_LIB` und `NEVERD_TEST_WINDOWS_SDK_LIB` aus. Außerhalb Windows ist zusätzlich `NEVERD_TEST_WINDOWS_RUNNER` nötig. `NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` macht fehlende Eingaben zum Fehler. Der eigene Windows-Workflow verlangt native Ausführung und lehnt übersprungene Fälle ab.

Die Linux-Nachweise auf dem Entwicklungs-Mac nutzen Docker und einen expliziten Linux-Loader; PE nutzt Wine 11. Mach-O ARM64 läuft direkt auf dem Mac. Native Windows-CI-Ergebnisse müssen gesondert geprüft werden: Ein konfigurierter Job ist kein bestandener Lauf. Historische Zahlen, Baseline-Fehler und Emulatorgrenzen stehen in [den detaillierten Verträgen und Prüfaufzeichnungen](../source-patching-details.md).

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=ON \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDSourcePatchMatrixTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDSourcePatchMatrixTests)$' \
  --output-on-failure --parallel 4
```

Die LLVM-Kopie korrigiert auch Mach-O-x64-GOT-Datenreferenzen: Clangs typisierte catch-Tabelle ist relativ zum Ende des vier Byte langen Relokationsworts. r3/r4 verschiebt sie um vier Byte; die Prüfung vor Veröffentlichung lehnt das ab. Auf dem Entwicklungs-Mac läuft x64 unter Rosetta 2 und ARM64 direkt.
