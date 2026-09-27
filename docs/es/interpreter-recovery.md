**Idiomas**: [English](../interpreter-recovery.md) | [简体中文](../zh-CN/interpreter-recovery.md) | [繁體中文](../zh-TW/interpreter-recovery.md) | [日本語](../ja/interpreter-recovery.md) | [한국어](../ko/interpreter-recovery.md) | [Français](../fr/interpreter-recovery.md) | [Deutsch](../de/interpreter-recovery.md) | [Español](interpreter-recovery.md) | [Italiano](../it/interpreter-recovery.md) | [Русский](../ru/interpreter-recovery.md) | [العربية](../ar/interpreter-recovery.md)

# Recuperación de código fuente a partir de intérpretes

[← Índice de documentación](README.md)

La etapa experimental de especialización de intérpretes elimina el despacho
resuelto estáticamente de una función x64 ya enlazada, conservando sus entradas
en tiempo de ejecución, efectos de memoria, bifurcaciones y bucles. Utiliza la
semántica de las instrucciones, no firmas de manejadores ni la tabla de opcodes
de un protector concreto.

```sh
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --recovery-report recovery.json -o recovered.c
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --llvm -o recovered-llvm.c
```

`--vm-control` selecciona registros generales completos que distinguen los
contextos del intérprete. Puede repetirse y no proporciona valores concretos.
Por ejemplo, seleccione un cursor de bytecode cuyo valor establezca el código
de entrada. Una entrada de ejecución usada como contador debe seguir siendo
dinámica. La falta de separación entre contextos puede detener la recuperación
cuando confluyen valores distintos del cursor; el motor no debe compensarlo
adivinando un destino de despacho. Para un cursor guardado en la pila,
`--vm-control-stack=-16:8` selecciona ocho bytes en RSP de entrada menos 16.
El desplazamiento se refiere a la entrada de la función, no al puntero de pila
tras sus ajustes.

La API C es `neverd_devirtualize_source_v1()`, declarada en
`neverd/sdk/NeverDCAPIDevirtualize.h`. Ejecuta una transacción independiente sin
modificar la caché de descompilación habitual de la sesión. Un fallo no devuelve
código fuente, aunque puede devolver un diagnóstico JSON. Ambas cadenas
asignadas se liberan con `neverd_free_string()`.

## Contrato de ejecución

El adaptador binario admite actualmente imágenes x64 ELF y PE enlazadas en sus
direcciones de mapeo. Los mapeos, bytes y permisos deben permanecer fijos, sin
modificaciones concurrentes. El lifting estricto bajo demanda sigue el código
máquina alcanzable. Las instrucciones no admitidas, llamadas, operaciones
opacas, accesos de memoria ordenados, control sin resolver y manejo de
excepciones del lenguaje detienen la recuperación.

La recuperación PE requiere todos los metadatos de la imagen, incluidas las reubicaciones globales y los registros de excepciones. La CLI los carga antes de aplicar `--func`; quienes usan la API C no deben restringir previamente la sesión mediante `neverd_session_restrict_function()`. El adaptador rechaza imágenes cargadas con un conjunto limitado de funciones, porque los metadatos omitidos no prueban la ausencia de correcciones ni de aristas de excepción.

Solo los rangos completos de solo lectura respaldados por el archivo, sin
mapeos superpuestos ni ajustes del cargador, pueden proporcionar lecturas
constantes de la imagen. Las tablas modificables, relocaciones sin resolver y
capturas puntuales de ejecución no demuestran la inmutabilidad. Las
relocaciones COPY y los metadatos incompletos de excepciones se rechazan de
forma conservadora. El dominio admitido exige retornos ABI ordinarios: el rango
de destino de cada escritura de origen externo debe ser disjunto del espacio
de la dirección de retorno al entrar. Es una precondición explícita del
llamador y del entorno, incluso para direcciones calculadas a partir de enteros
externos; la ausencia de procedencia del marco de pila no demuestra la
disjunción numérica. Las direcciones de escritura derivadas del marco deben
demostrar esa disjunción y el puntero de pila original debe restaurarse al
retornar. La información de origen se conserva al guardar valores en la pila
y en las uniones; perder una expresión afín no la convierte en un puntero
externo. Actualmente se rechazan los cambios de base de pila, los retornos que
retiran argumentos desde la función llamada y el despacho basado en RET.
El adaptador impone la semántica little-endian de x64.

Se producen código fuente e IR para análisis. Esto no demuestra la seguridad
de las relocaciones, el desenrollado de pila, las excepciones asíncronas ni la
sustitución binaria. El modo patch rechaza esta opción. No se afirma que todos
los intérpretes o configuraciones de protección estén admitidos.

## Límites actuales

Las direcciones de bytecode dependientes de la entrada y las relaciones entre estados del decodificador solo se admiten cuando los dominios finitos y las correlaciones necesarios pueden demostrarse dentro de los límites configurados. Esto no demuestra compatibilidad con todos los esquemas de decodificación indirecta. Se pueden recuperar ramas y bucles dinámicos si se demuestra cada destino de despacho; la cobertura de ramas ordinarias no basta para probarlo. El control sin resolver o el agotamiento de un presupuesto de prueba necesario son fallos y no publican código recuperado ni sustituciones parciales. Las llamadas auxiliares nativas, los límites de excepciones o reentrada, el código mutable y otras arquitecturas siguen fuera del contrato del adaptador.

## Implementación compartida

`SpecializationProvider` proporciona instrucciones íntegramente elevadas y
evidencias de lecturas inmutables. `NeverDInterpreterSpecialization` utiliza la
semántica existente de `SymExec` para evaluar parcialmente operaciones enteras
y de control. El adaptador binario se ocupa de los mapeos y la decodificación;
no implementa un segundo evaluador de instrucciones.

Para una dirección simbólica de lectura con dominio finito, el solver de vectores de bits integrado enumera las direcciones candidatas bajo las restricciones actuales. Solo se acepta el conjunto tras un resultado UNSAT final que demuestre que no existen otras direcciones, y cada dirección debe contar con un certificado completo de lectura inmutable que no produzca un fallo de memoria. Esa lectura certificada puede sustituirse en LowIR por una captura de la dirección y una cadena exacta de SELECT; las lecturas ordinarias no certificadas siguen siendo dinámicas. Una muestra de direcciones nunca sustituye al conjunto completo. Los registros de control y las posiciones del marco de entrada seleccionados pueden conservar tuplas conjuntas acotadas entre nodos, como la relación entre un cursor y su clave de decodificación. Las uniones y ampliaciones siguen siendo conservadoras. Los modelos SAT parciales o los resultados desconocidos no prueban la exhaustividad de direcciones ni destinos. Este mecanismo no necesita el backend Z3 opcional.

Cada nodo se identifica mediante su cursor nativo, modo de instrucción y las constantes seleccionadas de registros de control y espacios del marco de entrada. Los demás hechos a nivel de byte se combinan por intersección. Cuando se debilita un hecho entrante, se vuelve a evaluar el nodo. Así se conservan los bucles del programa, en lugar de expandir cada iteración observada. Todos los valores alcanzables de un destino indirecto deben pertenecer a un conjunto acotado cuya exhaustividad esté demostrada; los destinos seleccionados se convierten en comparaciones residuales explícitas y aristas del CFG.

Las operaciones dinámicas y las lecturas/escrituras ordinarias permanecen en
LowIR. Las constantes escalares, los punteros afines relativos al marco de entrada y
los bytes del marco cuya constancia se haya probado pueden pasar entre nodos;
las demás expresiones se descartan en lugar de expandirse sin límite. La
memoria del marco utiliza la invalidación conservadora de alias del estado
simbólico existente. Una escritura mediante un puntero desconocido que pueda
solaparse invalida los hechos en conflicto. No se presupone que un espacio de
pila sea privado ni se eliminan sus efectos mediante un contrato de ausencia
de alias sin demostrar.

Etiquetas sintéticas únicas de instrucciones distinguen los contextos clonados.
Los límites originales de las instrucciones se conservan en un mapa de origen
separado; los certificados originales de relocaciones, excepciones o tablas
de saltos no se copian a las nuevas apariciones. El LowIR recuperado pasa por
la conversión habitual de LowIR a MedIR antes de separar las rutas HighC y LLVM,
compartiendo el tratamiento de registros, pila, CFG, SSA y ABI. La ruta HighC
también exige que la verificación de MedIR termine correctamente.

Los presupuestos de nodos, contextos por dirección, operaciones, evaluaciones
de nodos y destinos finitos acotan el análisis. Si se agota un presupuesto o
aparece semántica no admitida, no se publica ninguna función residual. Un grafo
de control completo no equivale a una emisión de código fuente correcta;
la API pública comprueba ambos resultados e informa de la diferencia.

Los conjuntos finitos de direcciones de lectura, las tuplas conjuntas de control y el número de campos de control también tienen límites explícitos. Un límite global de consultas al solver y límites por consulta de puertas, conflictos, propagaciones y visitas a literales vigilados acotan el trabajo de prueba; el límite de nodos simbólicos acota el crecimiento de las expresiones. El informe JSON incluye esos presupuestos junto con `solverQueries` y `relationalWidenings`.

## Evidencias y pruebas

El informe JSON local opcional incluye el hash de entrada, controles elegidos,
presupuestos, estado, contadores de trabajo, número de bloques residuales,
ubicaciones originales de instrucciones y bytes inmutables usados durante la
recuperación. Contiene información derivada de la entrada y solo se escribe
en la ruta local solicitada.

Las pruebas públicas utilizan máquinas originales con despacho por registros
y por pila, cada una con programas aritméticos, bifurcaciones con unión y bucles
en tiempo de ejecución. Un oráculo independiente sin signo comprueba retornos,
escrituras de memoria, acarreos/préstamos y valores centinela de salida. El
HighC y LLVMC recuperado se compila con O0/O2 y trampas de comportamiento
indefinido, y se ejecuta frente al oráculo. Los casos negativos cubren el
despacho sin resolver o modificable, el orden de bytes incompatible, los
metadatos de excepciones y los presupuestos.

Otras fixtures originales de direcciones finitas usan registros de datos de solo lectura elegidos por la entrada, campos de control cursor/clave relacionados y un mismo handler en distintas posiciones virtuales. Cubren ramas con uniones y bucles cuya selección de registro depende del estado actual del programa. Su oráculo nativo independiente usa las convenciones SysV y Win64; ambas rutas C recuperadas se comprueban en O0/O2 con trampas de comportamiento indefinido y centinelas de salida. La falta de certificados de lectura o de presupuesto de prueba no debe publicar resultados parciales.

Consulte [testing.md](testing.md) para los objetivos de prueba específicos.
