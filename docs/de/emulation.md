**Sprachen**: [English](../emulation.md) | [简体中文](../zh-CN/emulation.md) | [繁體中文](../zh-TW/emulation.md) | [日本語](../ja/emulation.md) | [한국어](../ko/emulation.md) | [Français](../fr/emulation.md) | [Deutsch](emulation.md) | [Español](../es/emulation.md) | [Italiano](../it/emulation.md) | [Русский](../ru/emulation.md) | [العربية](../ar/emulation.md)

<!-- i18n-source: a3e64122b77a690dd856d02f5b2af53973d3bf1affd973bea5f9735aa9dd6722 -->

[← Dokumentationsindex](README.md)

# CPU-Ausführung und Gastumgebungen

<!-- i18n-section: backends -->

## CPU-Backends und Workloads

Die CPU-Ausführung trennt ISA-Zulassung, Gastspeicher, Backend-Transport und Gast-OS-Richtlinien. `NEVERD_ENABLE_CPU_EMULATION` aktiviert die x64/ARM64-CPU-Schicht; `NEVERD_ENABLE_DRIVER_EMULATION` ergänzt die begrenzte x64-Windows-WDM/KMDF-Umgebung. `linux-elf64-v1` führt unterstützte Linux-ELF-Prozesse aus. Siehe [CPU-Ausführung](cpu-execution.md), [Gastprozess-Emulation](process-emulation.md) und [Emulation von Windows-Treibern](driver-emulation.md).

Bei unterstützten nativen Verträgen wählt `auto` KVM unter Linux, WHP unter Windows oder [HVF unter macOS](macos-hvf.md), wenn Gast und Host dieselbe ISA haben. Unterschiedliche ISAs nutzen Unicorn; `software-cpu-v1` und die ursprüngliche V1-API bleiben softwarebasiert. Ein ausdrücklich gewähltes, nicht verfügbares Backend scheitert ohne Rückfall. Native Ausführung prüft zugelassene Instruktionen, Adressen und Effekte vor dem Eintritt. Host-Virtualisierung bestimmt kein Gast-OS: Die [Darwin-Profile](darwin-emulation.md) modellieren macOS, iOS und iOS Simulator separat. HVF benötigt die Berechtigung `com.apple.security.hypervisor`.

`driver-strict` / `checked-x64-v1` umfasst begrenzte x64-Ausführung; Windows-Treiber bleiben auf x64 beschränkt. `checked-aarch64-v1` und `checked-user-aarch64-v1` enthalten begrenztes ARM64 FP32/FP64, SIMD fester Breite und vollständigen FPCR/FPSR/Vektorzustand. Die native ARM64-HVF-Abnahme ist im Mac-Leitfaden dokumentiert; ARM64-KVM/WHP-Workload-Prüfungen und die vollständige Intel-HVF-Abnahme stehen noch aus. CPU-Unterstützung belegt keine allgemeine Treiber- oder Anwendungskompatibilität.

<!-- i18n-section: windows-processes -->

## Windows-Prozesse und Module

`windows-pe64-v1` unterstützt begrenzte Windows-x64/ARM64-Konsolenprozesse mit PEB/TEB, statischem und dynamischem TLS, `DllMain`, benannten Win32-APIs und expliziten azyklischen DLL-Graphen. Gastmodule unterstützen Code-/Datenimporte nach Name oder Ordinal, DIR64, weitergeleitete Exports und echte Loader-Listen. `LoadLibraryA` / `LoadLibraryW`, `FreeLibrary` und `GetProcAddress` verwenden den konfigurierten Katalog. CRT/GUI, ARM64-Frame-basiertes Benutzer-SEH, Threads und allgemeine Windows-Kompatibilität bleiben unvollständig; native ARM64-KVM/WHP-Belege fehlen weiterhin.

`WindowsSystemModules` erzeugt begrenzte PE64-Modellabbilder für `ntdll.dll`, `kernelbase.dll` und `kernel32.dll` auf beiden ISAs. ASCII-Abfragen über `GetModuleHandleA` / `GetModuleHandleW`, `LoadLibraryA` / `LoadLibraryW` und `GetProcAddress` teilen deren eingeblendete Basisadressen; PEB/LDR und `MEM_IMAGE` beschreiben dieselben Abbilder. Statische Importe, Namensabfragen und Gastweiterleitungen nutzen dieselben API-Gates und Exportauflösung. Anbieter bleiben fest resident, haben keine Gastinitialisierungs-Callbacks und verhindern nach Entladen gewöhnlicher Gast-DLLs keine Rückkehr vom Einstiegspunkt. Geänderte Header oder Exportmetadaten stoppen die Suche. Unmodellierte Systemexportnamen und von null verschiedene Systemordinale stoppen ausdrücklich; reine Schreibungsabweichungen modellierter Namen sowie leere Namen liefern Fehler 127, eine NULL-Abfrage liefert 87. Erzeugte Bytes und Adressen sind Modellregeln; Windows-Versionslayouts, native Ordinale und anbieterübergreifende Aliase werden nicht rekonstruiert. `WindowsSystemTests.cpp` vergleicht eigene x64/ARM64-EXEs mit nativem Windows und beobachtet acht Rückgaben des Anfangsthreads unabhängig.

<!-- i18n-section: environment-memory -->

## Umgebung und Speicher

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` verwenden denselben aktuellen Gastumgebungsblock in den PEB-Prozessparametern. ASCII-Namen werden ohne Beachtung der Großschreibung verglichen; Werte sind UTF-16. Änderungen prüfen Eingaben, Kapazität und Schreibrechte vor der Veröffentlichung. Momentaufnahmen bleiben unabhängig von späteren Änderungen und geben ihren Gastspeicher beim Freigeben zurück. Das Modell begrenzt den Block auf 64 KiB; Zeichenketten und Ersetzungen sind begrenzt und prüfen die Ausführungsfrist. Unbekannter Zeigerbesitz, fehlerhafte Blöcke, ANSI-Codepages und überlappende Ersetzungspuffer bleiben ununterstützt. `WindowsEnvironmentTests.cpp` vergleicht eigene x64/ARM64-Fixtures auf verfügbaren Backends; CI verlangt ein unabhängiges natives Windows-Orakel.

`WindowsProcessHeap` verwaltet Allokation, `HeapReAlloc`, Freigabe und Größenabfragen des Prozessheaps gemeinsam. Größenänderungen erhalten die verbleibenden Bytes; `HEAP_ZERO_MEMORY` löscht hinzugefügte Bytes, `HEAP_REALLOC_IN_PLACE_ONLY` verbietet Verschiebungen. Fehlgeschlagene Größenänderungen erhalten den alten Block und liefern NULL mit `ERROR_NOT_ENOUGH_MEMORY` (8), entsprechend den nativen Beobachtungen. Separate Seiten geben bei Verkleinerung und Freigabe Kapazität zurück; vorbereitetes Wachstum und begrenzte Kopien prüfen die Ausführungsfrist. Eigene Heaps, Ausnahmeflags, unbekannter Besitz und unzugängliche Kopier- oder Löschbereiche stoppen ausdrücklich. `WindowsHeapTests.cpp` prüft beide ISAs, erzwungene Verschiebung, Budgetwiederverwendung und Fehleratomarität; CI führt dieselbe eigene EXE auch auf nativem Windows aus.

Der virtuelle Windows-Speicher ergänzt `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` und `FlushInstructionCache` für den aktuellen Prozess. Die OS-Schicht verwaltet Reservierungen; `AddressSpace` bleibt maßgeblich für zugesicherte Seiten, Zugriffsrechte und deren Speicher. Tests prüfen Codeänderungen, Zugriffsfehler und die Wiederverwendung des Speicherbudgets.

`WriteProcessMemory` folgt bei Schreibzugriffen von höchstens 4 KiB auf den aktuellen Prozess dem auf x64/ARM64 beobachteten Verhalten zugesicherter Seiten. Der Schutz jedes Bereichs, kopierte Präfixe, Bytezahlen und LastError bleiben erhalten, einschließlich `ERROR_NOACCESS`, `ERROR_PARTIAL_COPY` und Erfolg nach einem RX-Präfix. `WindowsMemoryWriteTests.cpp` prüft alle 25 Schutzpaare; `check_windows_memory_write.py` überprüft dieselbe selbst erstellte ausführbare Datei in nativer Windows-CI. Nicht zugesicherte Zielbereiche bleiben ausdrücklich nicht unterstützt.

<!-- i18n-section: vectored-exceptions -->

## Vektorisierte Ausnahmen und Fortsetzung

`WindowsProcessExceptions` implementiert `AddVectoredExceptionHandler`, `RemoveVectoredExceptionHandler` und `RaiseException` auf derselben CPU mit dem Prozessbudget. Geordnete Handler dürfen Registrierungen ändern, verschachtelte Ausnahmen auslösen, modellierte APIs aufrufen, DLLs laden und den Prozess beenden. Datenzugriffsverletzungen auf x64/ARM64 und x64-Ganzzahldivisionsfehler können nach Prüfung der Gaständerungen an `CONTEXT` fortgesetzt werden; allgemeine Register, SIMD und unterstützter FP-Zustand bleiben erhalten. Softwareausnahmen kehren über eine echte Rücksprunganweisung im modellierten Anbieter zurück. Grenzen sind 128 aufbewahrte Registrierungen und 16 verschachtelte Frames. Ungültige Ergebnisse, geänderte Ausnahmezeiger, nicht unterstützte Felder und Grenzüberschreitungen scheitern ausdrücklich. Stackbasiertes ARM64-SEH/Unwinding, Debugger-Zustellung und Ausführungs-/Schutzseitenfehler bleiben offen. `WindowsExceptionTests.cpp` vergleicht eigene EXE/DLL-Szenarien mit nativem Windows; native ARM64-KVM/WHP-Belege fehlen weiterhin. Datensätze für Softwareausnahmen tragen `EXCEPTION_SOFTWARE_ORIGINATE` (`0x80`), unabhängig vom durch den Aufrufer gesetzten Flag für nicht fortsetzbare Ausnahmen; die ursprüngliche Windows-Programmdatei prüft die exakten Flagwerte für Software- und Hardwareausnahmen.

`AddVectoredContinueHandler` und `RemoveVectoredContinueHandler` verwalten eine eigene geordnete Liste; beide Handlerfamilien teilen die Grenze von 128 aufbewahrten Registrierungen. Akzeptiert ein vektorisierter Ausnahmehandler die Fortsetzung, sehen die Fortsetzungshandler denselben veränderbaren Ausnahmedatensatz und `CONTEXT`. Die abschließende Kontextprüfung erfolgt nach diesen Rückrufen, einschließlich verschachtelter Ausnahmen und DLL-Benachrichtigungen. Handles lassen sich nicht über die andere Handlerfamilie entfernen. `WindowsContinuationTests.cpp` vergleicht eigene EXE-Szenarien für Reihenfolge, vorzeitiges Ende, Listenänderungen, Kontextreparatur, Verschachtelung, Loader-Rückrufe und Prozessende mit nativem Windows. Der geprüfte Windows-x64-Vektorpfad erlaubt die Fortsetzung mit `EXCEPTION_NONCONTINUABLE`; dies belegt kein stackbasiertes SEH-Verhalten. Native ARM64-Ausführung bleibt ungeprüft.

<!-- i18n-section: caller-context -->

## Aufruferkontext

`RtlCaptureContext` ist über `kernel32.dll` und `ntdll.dll` für x64 und ARM64 verfügbar. Das gemeinsame `WindowsProcessContext` und `IntegerABI` speichern PC/SP des Aufrufers, ohne CPU-Zustand oder LastError zu ändern. Native Windows-Beobachtungen bestätigen x64-Flags `0x10000f`, unveränderte nicht beschriebene Home-/Debug-/Vektorbereiche und die klassischen 32-Bit-x87-Adressfelder; ARM64 übernimmt LR als PC und setzt X0/LR im Datensatz auf null. Register, SIMD und Gleitkommasteuerung stammen vom Gast; x64-Selektoren und MXCSR-Fähigkeitsmaske folgen der konfigurierten Gast-CPU. Ungültige, falsch ausgerichtete oder teilweise unzugängliche Ziele scheitern vor dem Schreiben. `WindowsContextTests.cpp` prüft direkte Importe, Anbieterabfragen, VEH-Rückrufe, seitenübergreifende Ausgaben und atomare Fehler. `scripts/check_windows_context.py` führt das Originalprogramm unter Windows x64/ARM64 aus und prüft nicht leeren x87-Zustand mit einem eigenen nativen Orakel. Diese ARM64-API-Beobachtungen belegen keine native KVM/WHP-Ausführung. Kontextwiederherstellung, Stack-Walking und dynamische Funktionstabellen bleiben getrennte Aufgaben. `WindowsProcessServices.def` legt genaue Modulbeschränkungen fest: Die Suche in `kernelbase.dll` liefert entsprechend den nativen Beobachtungen `ERROR_PROC_NOT_FOUND` (127), ohne einen zusätzlichen Export zu erfinden. [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

<!-- i18n-section: structured-exceptions -->

## Strukturierte Ausnahmebehandlung

`WindowsProcessSEH` nutzt das gemeinsame `X64SEH` unter `os/windows/exception/` (`NeverDEmulationWindowsException`, auch ohne Treiber) für x64 `__C_specific_handler` und UNWIND_INFO V1. Nach der VEH-Suche folgen Filter, finally-Aufrufe, nichtlokale Handlerübergänge, verschachtelte/kollidierte Abwicklungen und verschobene EXE/DLL-Frames; nichtflüchtige GPR/XMM bleiben erhalten. Bei Filterfortsetzung läuft VCH mit demselben `CONTEXT`. `WindowsSEHTests.cpp` vergleicht 23 eigene Szenarien mit nativem Windows; KVM/WHP/Unicorn teilen die Semantik. Das Prozessbudget umfasst erneute Prüfungen von Image-Generationen, Headern, Unwind-/Scope-Bytes, Codebereichen der Sprachhandler und IAT-Bindungen. Veränderte Metadaten oder entladene beibehaltene Images scheitern ausdrücklich. ARM64-Frame-SEH, C++ EH, dynamische Funktionstabellen, allgemeines RtlUnwind/NtContinue und Abwicklung über Loader-/VEH-/VCH-Callback-Grenzen bleiben ununterstützt.

Bei `EXCEPTION_NONCONTINUABLE` löst ein x64-Filter mit Rückgabe `EXCEPTION_CONTINUE_EXECUTION` eine `STATUS_NONCONTINUABLE_EXCEPTION` (`0xc0000025`, Flags `0x81`, verknüpfter Datensatz null) mit neuem Kontext aus. VEH läuft erneut vor der Suche im erhaltenen logischen Stack; finally-Reihenfolge, EXE/DLL-Frame-Identität sowie Tiefen- und Ausführungsbudgets bleiben erhalten. Die 23 nativen Szenarien umfassen 21 erfolgreiche Ausführungen und zwei Beendigungen: Eine in VEH/VCH akzeptierte Fortsetzung dieser sekundären Ausnahme bleibt auch nach Wiederherstellung des ursprünglichen `CONTEXT` unbehandelt. Das Modell meldet einen Laufzeitfehler. Software-Ausnahmeadressen entsprechen dem gespeicherten PC; interne Dispatcher-Adressen und Registerlayouts sind Modellvorgaben. [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

<!-- i18n-section: processor-state -->

## Instruktionen und Prozessorzustand

Geprüftes x64 unterstützt `MOVS/STOS/LODS` auf normalem RAM und `CLD/STD` mit Wiederaufnahme, Abbruch und Seitengrenzprüfung je Element. CPU-spezifische obere Bits bei Nullzählern und STOS/LODS-Geräteoperanden bleiben außerhalb des Vertrags.

Geprüftes x64 unterstützt auch `CMPS/SCAS` auf normalem RAM mit `REPE/REPNE`, arithmetischen Flags, vorzeitigem Ende, Stopps je Element und Fehlerwiederaufnahme. Gerätevergleiche bleiben ausgeschlossen.

Native x64- und ARM64-Startproben prüfen begrenzte vollständige Zustandsausführung mit exklusivem Speicherrecht. XSAVE-Pakete und ISA-abhängige Seitentabellen-Caches haben einen eindeutigen Besitzer.

Native x64-Felder `FOP/FIP/FDP` folgen den Sicherungsregeln des Hosts: AMD darf inaktive x87-Ausnahmemetadaten löschen. Startprüfungen validieren sie mit einer ausstehenden unmaskierten Ausnahme.
