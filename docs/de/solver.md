**Sprachen**: [English](../solver.md) | [简体中文](../zh-CN/solver.md) | [繁體中文](../zh-TW/solver.md) | [日本語](../ja/solver.md) | [한국어](../ko/solver.md) | [Français](../fr/solver.md) | [Deutsch](solver.md) | [Español](../es/solver.md) | [Italiano](../it/solver.md) | [Русский](../ru/solver.md) | [العربية](../ar/solver.md)

[← Dokumentationsindex](README.md)

# Bitvektor-Beweisbackends

Standardmäßig verwendet NeverD seinen eingebauten Bitvektorsolver. Exakte MBA-Ableitung bleibt von allgemeinen Solverbackends unabhängig. Ausdruckssynthese übernimmt einen Kandidaten erst nach einem Äquivalenzbeweis; Gegenbeispiele und offene Abfragen lassen den Originalausdruck unverändert.

<!-- i18n-section: z3-build -->

## Optionaler Z3-Build

Mit aktiviertem Z3 lädt CMake über `FetchContent` die fixierte Quellrevision 4.13.3 und baut eine statische Bibliothek. Eine Systeminstallation ist nicht nötig:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_Z3=ON -DNEVERD_BUILD_SOLVER_BENCH=ON
cmake --build build-release --target neverd NeverDSolverTests \
  NeverDSymbolicTests neverd-solver-bench --parallel 4
```

`NEVERD_Z3_PROVIDER=FETCH` ist Standard. Der erste Configure-Schritt lädt den Quelltext; spätere Builds verwenden `_deps` erneut. Z3-CLI, Tests, Beispiele, Dokumentation und Sprachbindungen werden nicht gebaut. Z3-Python-Generatoren nutzen NeverDs vorhandenen Python-Interpreter. Für eine installierte Bibliothek `-DNEVERD_Z3_PROVIDER=SYSTEM` und optional `-DZ3_ROOT=/path/to/prefix` wählen. Fehlende Entwicklungsdateien führen zum Fehler, ohne stillen Providerwechsel. Offline-Builds können `-DFETCHCONTENT_SOURCE_DIR_NEVERD_Z3=/path/to/z3` auf einen lokalen Checkout setzen.

Bei `NEVERD_ENABLE_Z3=OFF` (Standard) lädt, sucht oder verlinkt NeverD Z3 nicht. Eine explizite Laufzeitanforderung für einen nicht verfügbaren Backend schlägt ohne Fallback fehl.

<!-- i18n-section: synthesis -->

## Beweisgesteuerte Ausdruckssynthese

```sh
build-release/bin/neverd simplify --synthesize --solver=z3 \
  --solver-timeout-ms=1000 --json '(x >> 4) + ((x >> 2) >> 2)'
```

`--solver=builtin` ist Standard. Solverauswahl erfordert `--synthesize`; der normale MBA-Vereinfacher ruft Z3 nicht auf. Das Z3-Timeout gilt pro Prüfung und zählt Ausdrucksübersetzung nicht mit. Abbruch ist kooperativ, keine harte Echtzeitfrist. Null wählt 1000 ms; `--exhaustive` entfernt das Limit. Konflikt-, Propagations- und Watch-Besuchslimits gelten nur für das eingebaute Backend und werden mit Z3 abgelehnt. Z3-Prüfungen erhöhen den Proof-Query-Zähler; eingebaute SAT-Arbeitszähler bleiben null.

Die C-API ergänzt `neverd_synthesize_options` um `solver_backend` und `solver_timeout_ms`. Größenbeschränkte Leser behalten für ältere Aufrufer das eingebaute Backend. `neverd_solver_backend_available()` meldet die Build-Fähigkeit; Python bietet dieselbe Wahl über `synthesize_expression(..., solver='z3', solver_timeout_ms=1000)`. Die Auswahl gilt derzeit nur für Ausdruckssynthese. Concolic-Ausführung, Sicherheitsanalyse und bestehende IR-Optimierungsdefaults behalten ihre Solverrichtlinien. Interne Nutzer können den Z3-Verifier über den Proof-Callback des semantischen Vereinfachers einspeisen.

```python
from neverd_plugin import synthesize_expression

result = synthesize_expression(
    '(x >> 4) + ((x >> 2) >> 2)', solver='z3', solver_timeout_ms=1000
)
```

<!-- i18n-section: checks -->

## Unabhängige Prüfungen und Query-Export

`NeverDSolverTests` enthält bei aktiviertem Z3 eine unabhängige Ausdrucksauswertung und Backendvergleiche. Referenzausdrücke werden direkt erstellt, damit ein Fehler in NeverDs Ausdrucks-Buildern nicht beide Testseiten gleich vereinfacht. Bei deaktiviertem Build werden Oracle-Fälle ausdrücklich übersprungen; der Vertrag für nicht verfügbare Backends wird weiter getestet.

```sh
build-release/bin/NeverDSolverTests --gtest_brief=1
build-release/bin/neverd-solver-bench \
  tools/neverd-bench/solver-corpus.txt --width=32 --repeat=5 \
  --backend=both --timeout-ms=1000 --max-conflicts=10000 \
  --dump-dir=/tmp/neverd-queries > /tmp/neverd-solver-results.json
```

Jede Nicht-Kommentarzeile lautet `original ; candidate`; ohne Semikolon wird das Ergebnis des MBA-Vereinfachers als Kandidat verwendet. Das Tool meldet Urteile, Modell-Replay und Zeiten für Sessionaufbau, Übersetzung und Lösung, nicht aber Parsing, MBA-Vereinfachung, Export oder Abbau. Jede Wiederholung nutzt einen frischen Solver. SAT-Modelle müssen die Differenz im Ausdrucksevaluator reproduzieren. Widersprüchliche eindeutige Urteile sowie ungültige Abfragen/Modelle lassen den Lauf scheitern; `unknown` wird protokolliert und ist kein Äquivalenzbeweis.

SMT-LIB-Export enthält ursprünglichen DAG, dauerhafte Assertions und Annahmen der letzten Abfrage; Replay mit `z3 query-N.smt2`. Ressourcenlimits und Solversionen mitprotokollieren. Die Backends verwenden unterschiedliche Budgeteinheiten: Verglichen werden begrenzte Workloads, nicht identische Arbeit. Bitvektorbeweise folgen der totalen Fixed-Width-Semantik der Ausdruckssprache. Maschinenfehler, Speichereffekte und LLVM poison bleiben Verantwortung der Lift-/Übersetzungsgrenzen; ein Ausdrucksbeweis zertifiziert sie nicht.
