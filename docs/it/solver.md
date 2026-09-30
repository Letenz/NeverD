**Lingue**: [English](../solver.md) | [简体中文](../zh-CN/solver.md) | [繁體中文](../zh-TW/solver.md) | [日本語](../ja/solver.md) | [한국어](../ko/solver.md) | [Français](../fr/solver.md) | [Deutsch](../de/solver.md) | [Español](../es/solver.md) | [Italiano](solver.md) | [Русский](../ru/solver.md) | [العربية](../ar/solver.md)

[← Indice della documentazione](README.md)

# Backend di prova bitvector

Per impostazione predefinita NeverD usa il solver bitvector integrato. La derivazione MBA esatta resta indipendente da un solver generale. La sintesi di espressioni accetta un candidato solo dopo una prova di equivalenza; controesempi o query inconclusive mantengono l’espressione originale.

## Build Z3 facoltativa

Attivando Z3, CMake scarica tramite `FetchContent` la revisione sorgente fissata 4.13.3 e crea una libreria statica con NeverD; non serve installare Z3 nel sistema:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_Z3=ON -DNEVERD_BUILD_SOLVER_BENCH=ON
cmake --build build-release --target neverd NeverDSolverTests \
  NeverDSymbolicTests neverd-solver-bench --parallel 4
```

Il default è `NEVERD_Z3_PROVIDER=FETCH`. La prima configurazione scarica i sorgenti; le build successive riusano `_deps`. Non vengono buildati CLI, test, esempi, documentazione né binding di Z3. I generatori Python usano l’interprete già richiesto da NeverD. Per una libreria installata scegliere `-DNEVERD_Z3_PROVIDER=SYSTEM` e, facoltativamente, `-DZ3_ROOT=...`; se mancano i file di sviluppo fallisce senza cambiare provider in silenzio. Per build offline fornire una copia locale con `-DFETCHCONTENT_SOURCE_DIR_NEVERD_Z3=...`.

Con `NEVERD_ENABLE_Z3=OFF` (default), NeverD non scarica, cerca o linka Z3. Una richiesta runtime esplicita di un backend non disponibile fallisce senza fallback.

## Sintesi di espressioni con prova

```sh
build-release/bin/neverd simplify --synthesize --solver=z3 \
  --solver-timeout-ms=1000 --json '(x >> 4) + ((x >> 2) >> 2)'
```

`--solver=builtin` è il default. La selezione richiede `--synthesize`; il semplificatore MBA normale non invoca Z3. Il timeout Z3 si applica a ogni verifica, esclusa la traduzione dell’espressione. La cancellazione è cooperativa, non un limite wall-clock rigido. Zero seleziona 1000 ms; `--exhaustive` rimuove il limite. Limiti SAT di conflitti, propagazione e visite ai watch riguardano solo il backend integrato e sono rifiutati con Z3. I controlli Z3 incrementano il contatore delle query di prova; i contatori SAT integrati restano a zero.

La C API aggiunge `solver_backend` e `solver_timeout_ms` a `neverd_synthesize_options`. I reader limitati per dimensione mantengono il backend integrato per i vecchi client. `neverd_solver_backend_available()` indica la capacità della build; Python espone la scelta tramite `synthesize_expression(..., solver='z3', solver_timeout_ms=1000)`. Attualmente vale solo per la sintesi di espressioni. Esecuzione concolic, analisi di sicurezza e default dell’ottimizzazione IR mantengono le policy esistenti. Gli utenti interni possono fornire il verifier Z3 attraverso il proof callback del semplificatore semantico.

## Controlli indipendenti ed esportazione query

Con Z3 abilitato, `NeverDSolverTests` include valutazione indipendente delle espressioni e confronti tra backend. Le espressioni di riferimento sono costruite direttamente, così un bug nei builder di NeverD non può semplificare entrambi i lati del test. Le build senza Z3 saltano esplicitamente gli oracle ma verificano ancora il contratto del backend indisponibile.

```sh
build-release/bin/NeverDSolverTests --gtest_brief=1
build-release/bin/neverd-solver-bench tools/neverd-bench/solver-corpus.txt \
  --width=32 --repeat=5 --backend=both --timeout-ms=1000 \
  --max-conflicts=10000 --dump-dir=/tmp/neverd-queries
```

Ogni riga non commentata è `original ; candidate`; senza punto e virgola il candidato è il risultato del semplificatore MBA. Lo strumento riporta verdetti, replay del modello e tempi (include creazione sessione, traduzione e soluzione; esclude parsing, semplificazione MBA, export e teardown). Ogni ripetizione usa un solver nuovo. I modelli SAT devono riprodurre la differenza nell’evaluator. Verdetti decisivi opposti o query/modelli non validi fanno fallire il run; `unknown` viene registrato ma non prova equivalenza.

L’SMT-LIB esportato include DAG originale, asserzioni permanenti e assunzioni dell’ultima query; replay con `z3 query-N.smt2`. Registrare limiti di risorse e versioni: i backend usano unità di budget diverse, quindi il confronto è tra workload limitati e non tra lavoro identico. Le prove bitvector usano la semantica totale a larghezza fissa del linguaggio. Eccezioni macchina, effetti di memoria e LLVM poison restano responsabilità dei confini di lifting e traduzione; una prova d’espressione non li certifica.
