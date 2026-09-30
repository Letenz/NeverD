**Idiomas**: [English](../solver.md) | [简体中文](../zh-CN/solver.md) | [繁體中文](../zh-TW/solver.md) | [日本語](../ja/solver.md) | [한국어](../ko/solver.md) | [Français](../fr/solver.md) | [Deutsch](../de/solver.md) | [Español](solver.md) | [Italiano](../it/solver.md) | [Русский](../ru/solver.md) | [العربية](../ar/solver.md)

[← Índice de documentación](README.md)

# Backends de prueba de bitvectors

NeverD usa por defecto su solver de bitvectors integrado. La derivación MBA exacta no depende de un solver general. La síntesis de expresiones solo acepta un candidato tras demostrar equivalencia; un contraejemplo o consulta inconclusa conserva la expresión original.

## Compilación opcional con Z3

Al activar Z3, CMake descarga mediante `FetchContent` la revisión fijada 4.13.3 y compila una biblioteca estática junto con NeverD. No se requiere instalar Z3 en el sistema:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_Z3=ON -DNEVERD_BUILD_SOLVER_BENCH=ON
cmake --build build-release --target neverd NeverDSolverTests \
  NeverDSymbolicTests neverd-solver-bench --parallel 4
```

`NEVERD_Z3_PROVIDER=FETCH` es el valor predeterminado. La primera configuración descarga el código; las siguientes reutilizan `_deps`. No se compilan la CLI, pruebas, ejemplos, documentación ni bindings de Z3. Sus generadores Python usan el intérprete existente de NeverD. Para una biblioteca instalada, selecciona `-DNEVERD_Z3_PROVIDER=SYSTEM` y opcionalmente `-DZ3_ROOT=...`; si faltan archivos de desarrollo falla y no cambia de proveedor silenciosamente. En modo offline puedes proporcionar un checkout local con `-DFETCHCONTENT_SOURCE_DIR_NEVERD_Z3=...`.

Con `NEVERD_ENABLE_Z3=OFF` (predeterminado), NeverD no descarga, busca ni enlaza Z3. Una solicitud explícita de un backend no disponible falla, sin fallback.

## Síntesis de expresiones condicionada a prueba

```sh
build-release/bin/neverd simplify --synthesize --solver=z3 \
  --solver-timeout-ms=1000 --json '(x >> 4) + ((x >> 2) >> 2)'
```

`--solver=builtin` es el predeterminado. Elegir solver requiere `--synthesize`; el simplificador MBA normal no invoca Z3. El timeout de Z3 se aplica a cada comprobación, sin contar la traducción de la expresión. La cancelación es cooperativa, no un plazo estricto de reloj. Cero selecciona 1000 ms; `--exhaustive` elimina el límite. Los límites de conflictos SAT, propagación y visitas a watch solo se aplican al backend integrado y se rechazan con Z3. Las consultas Z3 incrementan el contador de pruebas; los contadores de trabajo SAT integrado permanecen en cero.

La API C añade `solver_backend` y `solver_timeout_ms` a `neverd_synthesize_options`. Los lectores limitados por tamaño mantienen el backend integrado para clientes antiguos. `neverd_solver_backend_available()` informa de la capacidad de compilación; Python ofrece la misma opción en `synthesize_expression(..., solver='z3', solver_timeout_ms=1000)`. Actualmente se aplica solo a síntesis de expresiones. La ejecución concolic, el análisis de seguridad y los valores predeterminados de optimización IR conservan sus políticas. Los usuarios internos pueden pasar el verificador Z3 mediante el callback de prueba del simplificador semántico.

## Comprobaciones independientes y exportación de consultas

Con Z3 habilitado, `NeverDSolverTests` incluye evaluación independiente de expresiones y comparación entre backends. Las expresiones de referencia se construyen directamente para impedir que un error en los constructores de NeverD simplifique ambos lados del test. Las compilaciones deshabilitadas omiten expresamente esos oráculos y siguen comprobando el contrato del backend no disponible.

```sh
build-release/bin/NeverDSolverTests --gtest_brief=1
build-release/bin/neverd-solver-bench tools/neverd-bench/solver-corpus.txt \
  --width=32 --repeat=5 --backend=both --timeout-ms=1000 \
  --max-conflicts=10000 --dump-dir=/tmp/neverd-queries
```

Cada línea no comentada es `original ; candidate`; si falta el punto y coma, se usa como candidato el resultado del simplificador MBA. La herramienta informa veredictos, reproducción de modelos y tiempos (incluyen creación de sesión, traducción y resolución; excluyen parsing, simplificación MBA, exportación y cierre). Cada repetición crea un solver nuevo. Los modelos SAT deben reproducir la diferencia en el evaluador de expresiones. Veredictos concluyentes opuestos o consultas/modelos inválidos hacen fallar la ejecución; `unknown` se registra, pero no prueba equivalencia.

SMT-LIB exportado incluye el DAG original, aserciones permanentes y supuestos de la última consulta; se puede reproducir con `z3 query-N.smt2`. Registra límites y versiones: los backends usan unidades de presupuesto distintas, así que se comparan cargas acotadas, no trabajo idéntico. Las pruebas de bitvector usan semántica total de ancho fijo. Excepciones de máquina, efectos de memoria y LLVM poison siguen perteneciendo a los límites de lifting y traducción; una prueba de expresión no los certifica.
