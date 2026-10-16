**Idiomas**: [English](../windows-exception-reconstruction.md) | [简体中文](../zh-CN/windows-exception-reconstruction.md) | [繁體中文](../zh-TW/windows-exception-reconstruction.md) | [日本語](../ja/windows-exception-reconstruction.md) | [한국어](../ko/windows-exception-reconstruction.md) | [Français](../fr/windows-exception-reconstruction.md) | [Deutsch](../de/windows-exception-reconstruction.md) | [Español](windows-exception-reconstruction.md) | [Italiano](../it/windows-exception-reconstruction.md) | [Русский](../ru/windows-exception-reconstruction.md) | [العربية](../ar/windows-exception-reconstruction.md)

# Reconstrucción de excepciones de Windows

[← Índice de documentación](README.md)

NeverD conserva la información de excepciones tabulares de Windows durante la
carga, el lift, la decompilación y la reescritura binaria. Los metadatos forman
parte del contrato ejecutable de una función: se rechaza una reescritura cuando
no puede demostrarse la coherencia del código generado, los records
runtime-function, las tablas de lenguaje y las tablas de protección.

Se distinguen tres niveles de soporte:

- **Análisis**: decodifica la representación nativa en records normalizados y
  comprobados, visibles para la pipeline IR.
- **Decompilación**: convierte regiones protegidas reducibles en nodos de
  excepción HighIR; las demás conservan anotaciones nativas deterministas.
- **Reconstrucción nativa**: patch mode puede pedir a LLVM un contrato de
  excepción de reemplazo completo e instalarlo en el PE final.

El soporte de análisis no implica reconstrucción nativa.

## Matriz de soporte

| Forma nativa | Lift y análisis | Salida de alto nivel | Patch mode |
|--------------|-----------------|----------------------|------------|
| unwind x64 v1/v2 | Records, operaciones, cadenas, datos de handler y procedencia completos y comprobados | Resumen frame/unwind y regiones estructuradas cuando corresponda | Records primary completos; `.pdata`/`.xdata` generadas reemplazan el cierre obsoleto |
| unwind x64 v3/APX | Payload v3, epílogos y contabilidad de operaciones dedicados | Anotación v3 explícita | Solo análisis; se rechaza una función tocada |
| unwind ARM32/ARM64 packed | Rangos, campos packed e identidad primary/fragment | Resumen frame/unwind | Solo records primary completos sin lenguaje ni fragmentos direccionables por separado |
| unwind ARM32/ARM64 unpacked | Header/extensión xdata comprobados, asociación de handler y fragmentos | Resumen frame/unwind | Solo records primary completos sin lenguaje ni fragmentos direccionables por separado |
| `__C_specific_handler` | Scopes, filtros, finally, handlers y continuaciones | `__try`/`__except`/`__finally` si es reducible; anotación en otro caso | Reconstrucción x64 nativa de grafos completos y representables |
| `__CxxFrameHandler3` | Unwind/try maps, catches, offsets de objeto/frame, continuaciones e IP-to-state | Intervalos reducibles como C++ HighIR con anotaciones de tipo compatibles con C | Reconstrucción x64 del subconjunto estrecho y verifier-clean descrito abajo |
| `__CxxFrameHandler4` | Decodificación variable acotada al grafo C++ común | Mismo HighIR con procedencia FH4 | Solo análisis; se rechaza una función tocada |
| `__GSHandlerCheck_SEH/EH/EH4` | Personality envuelta y procedencia GS cookie comprobada | Grafo base y anotación wrapper | Solo análisis; rechazo sin downgrade |
| EH x86 por cadena de registro | Cadenas SEH y grafos C++ verificados | Regiones HighIR con identidad de callbacks conservada | Reconstrucción PE32 del subconjunto demostrado; véase el contrato detallado |

Un `try` dentro de un `catch` de referencia escalar puede conservar el objeto exterior activo y restaurar su pila privada. Los callbacks compartidos y la gestión general del ciclo de vida de objetos siguen fuera del subconjunto nativo. [PE32 C++](../windows-exception-reconstruction.md).

Un record malformed nunca se considera completo. Una decodificación parcial
sirve para inspección, pero no autoriza generación nativa. Si el header xdata
ARM aún demuestra un rango de fragmento ejecutable acotado aunque el cuerpo
unwind esté dañado, el rango permanece para el desensamblado; el record queda
malformed y no es patchable.

## Modelo normalizado

`ExceptionInfo` pertenece a `BinaryImage`. Cada `ExceptionFunction` contiene:

- un rango de código semiabierto comprobado;
- identidad primary, chained o fragment;
- encoding unwind nativo y procedencia runtime/unwind exacta;
- operaciones y epílogos normalizados, incluidos operands opacos desconocidos;
- identidad exacta de personality y datos de handler;
- scopes SEH, maps de estado C++ y datos GS cookie opcionales;
- estado `Complete`, `Partial` o `Malformed` y diagnósticos deterministas.

El loader no expone punteros crudos. Las RVA nativas se conservan para diagnóstico
y reemplazo; los consumidores IR usan solo VA y rangos validados.

El índice de la imagen permite solapamiento chained/fragment y devuelve la
función más específica. Directorios, rangos, punteros, contadores, transiciones,
enteros comprimidos o cadenas dañados, y el agotamiento del presupuesto, reducen
el estado de parse correspondiente.

Los límites se aplican por tabla y al grafo completo de cada función. Reutilizar
una handler map en muchas try entries no supera el presupuesto agregado. Los
records FH3 que comparten `FuncInfo` y personality forman un grupo acotado:
aceptan sus catch funclets, no direcciones runtime ajenas.

Los argumentos enteros estrechos conservan su ancho probado de 8, 16 o 32 bits en ECX/EDX;
las posiciones físicas de pila siguen siendo de 32 bits. Las instrucciones originales determinan
la extensión de signo o por ceros. Las cargas y guardados locales anteriores a la publicación FS
deben preservar ESP, EBP y todos los campos de registro.

Las entradas C++ PE32 generadas se pueden volver a cargar, elevar y reconstruir si pasa la prueba completa del origen.
El cargador verifica en los bytes la escritura exacta FS:[0], el frame realineado y las funciones de los punteros SafeSEH.
La instalación puede modificar una entrada autenticada de una sección ejecutable generada anteriormente si su
almacenamiento virtual y físico es único y coincide con los bytes analizados. Los casos cdecl/stdcall/thiscall/fastcall
comprueban dos generaciones, ambos modos de patch y rebasing forzado con el CRT Microsoft x86.
Cada generación exige nuevas pruebas LowIR y LLVM; un recibo anterior no autoriza otra reescritura.
Los relays cleanup basados en ESI permiten una segunda reconstrucción cuando la base del marco y el
EBP de entrada guardado coinciden con el padre autenticado. La prueba de bytes en caché se vincula a cada
despacho; LowIR prueba los accesos a objetos y la vida del marco guardado. Las muestras MSVC reales
verifican destrucción ordenada y escritura por referencia en dos generaciones, ambos modos patch y bases
forzadas. La vida útil general de objetos y las conversiones de tipos siguen pendientes.

Los objetos PE32 con copia trivial admiten catch por valor o referencia, throw directo y relanzamiento.
El ThrowInfo inmutable debe contener un solo tipo sin ajustes de dirección, constructor de copia,
ni callback de reenvío. Se prueba la inicialización de cada byte y que no escapen punteros
del marco privado. Los casos O0/O1 verifican estructuras de 8 y 12 bytes, cambios de campos, ramas
del catch, ambos modos CLI y reubicación con Microsoft CRT. Las copias PHI conservan su arista CFG
y callback; la validación independiente rechaza llamadas adicionales, alias y destinos alterados.
RTTI no proporciona el diseño de miembros: las pruebas sintácticas C++ usan las declaraciones reales
del ejemplo; la salida conserva offsets nativos y llamadas throw de agregados desconocidos.

Un destructor opcional requiere una prueba LowIR independiente: una función hoja que
retorna, con pila privada y accesos ECX limitados al objeto. Su código y sus efectos en
memoria forman parte del contrato preservado. Los casos O0/O1 comprueban cantidad, orden
y contenido de los objetos destruidos, incluidos relanzamientos y dos reconstrucciones.

Un constructor de copia no trivial requiere una prueba LowIR que separe el destino ECX
del puntero de origen pasado por pila. El origen es de solo lectura; los accesos están
acotados, cada retorno inicializa todo el destino y usa `RET 4`. Se rechazan fugas de
punteros, observación del llamador CRT y llamadas no probadas. Los casos O0/O1 comprueban
cuatro copias y doce destrucciones por ejecución en dos generaciones. Los agregados
desconocidos conservan la llamada throw y la inicialización original; RTTI no justifica
`throw Type()`. Siguen pendientes los grafos generales de constructores, conversiones,
copias de bases virtuales y grafos generales de destructores.

## Contrato IR

Los metadatos atraviesan todas las representaciones sin cambiar el CFG ordinario:

- LowIR divide bloques en límites, estados, filtros, handlers, cleanup y continuaciones.
- Successors/predecessors excepcionales se mantienen separados de los normales.
- MedIR conserva el descriptor normalizado y aristas excepcionales estables.
- HighIR distingue `SEHTry` y `CxxTry` y preserva VA, type descriptors,
  adjectives, offsets, acciones, estados y continuaciones.

El structurer HighIR es conservador: mueve solo un tramo contiguo totalmente
contenido en una región completa y procesa anidamientos desde dentro. Regiones
cruzadas, grafos parciales, límites ambiguos y funclets externos mantienen el
flujo original.

El backend C emite sintaxis MSVC SEH para una región reducible de una cláusula.
Como HighC es un backend C, catches y cleanup C++ se expresan como comentarios C
deterministas, sin afirmar que el resultado sea C++ compilable.

## Esquema de metadatos LLVM

Cada función de excepción analizada recibe metadatos sin pérdida, incluso sin
lowering WinEH nativo:

- attachment `neverd.windows.eh`;
- marcador nativo `neverd.windows.eh.native`;
- tabla de módulo `neverd.windows.eh.functions`;
- versión de esquema `3`.

El record fijo conserva estado, encoding, rango, RVA runtime/unwind, tipo y
cadena, palabra packed, frame, nombres de personality, handler, bytes unwind,
operaciones/epílogos, scopes SEH, maps C++, datos GS, diagnósticos y permiso de
regeneración. El patch exige versión exacta y rango idéntico a la imagen cargada.

El lowering SEH x64 usa LLVM WinEH y solo emite `invoke`/funclets verifier-clean
cuando el grafo completo es representable. FH3 además exige:

- x64 COFF, unwind v1/v2, metadatos completos y grafo FH3 síncrono válido;
- sin `noexcept`, async, separated-funclet, GS, FH4 ni flags desconocidos;
- intervalos anidados o disjuntos, nunca cruzados;
- sin destructor/unwind action, construcción de catch object ni dependencia de parent frame;
- handler en un bloque normal sin predecessor ni call;
- LLVM `invoke` para toda operación protegida que pueda hacer unwind.

En otro caso el IR sigue siendo analizable y sin pérdida, pero se rechaza el
reemplazo nativo. Entry point PE, callbacks TLS y raíces CRT son fronteras de
conservación, no candidatos ABI ordinarios.

## Transacción de patch

Una reescritura soportada es una sola transacción PE:

1. Validar cada función tocada contra el grafo cargado y los metadatos LLVM.
2. Compilar preservando identidad, alineación y traits de secciones y referencias
   semánticas; externalizar la personality Windows local y enlazar xdata al
   handler ejecutable original demostrado.
3. Conservar runtime functions intactas y retirar todo el cierre reemplazado,
   incluidos records chained.
4. Relocar code/xdata, fusionar y ordenar pdata, rechazar solapamientos, demostrar
   la clase de personality e instalar un único directorio de excepciones.
5. Preservar CFG, resolver `.gfids`/`.gehcont`, fusionar Guard CF/EH continuation
   y actualizar load-config. Un helper sin resolver aborta; CFW, return-flow
   guard, retpolines y XFG quedan analysis-only.
6. Volver a analizar la imagen de bytes completa antes de escribir.

La extensión del fork LLVM es genérica: el writer final-image conserva traits de
sección y referencias simbólicas. PE/MSVC, política, fusión, load-config y
validación final permanecen en NeverD.

Las entradas Guard CF/EH continuation originales se conservan porque sus
trampolines siguen siendo objetivos indirectos válidos. Los objetivos generados
deben estar en el código emitido y las tablas estrictamente ordenadas por RVA.

## Validación de imagen final

Se rechaza un PE parcheado salvo que:

- LLVM acepte COFF y coincidan machine, clase, secciones, directorios, base y extensión;
- extensiones raw/virtuales estén acotadas y no se solapen;
- el directorio de excepciones esté respaldado por el archivo y dentro de la imagen;
- runtime functions estén ordenadas, no vacías, sin solape y sean ejecutables;
- RVA/header/version/flags/handler/cadenas unwind x64 sean válidos;
- imports, exports y símbolos COFF finales permitan volver a analizar SEH/FH3;
- records ARM/xdata describan versión y rango soportados;
- existan campos Guard CF/EH cuando los flags anuncian tablas;
- punteros, contadores y strides estén en archivo/imagen y cada objetivo sea ejecutable.

Cualquier fallo aborta el patch; no se escribe una imagen best-effort.

## Verificación enfocada

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

La fixture x64 protegida usa `/guard:cf` y `/guard:ehcont` y comprueba scopes
SEH, Guard, HighC, patch, recarga, orden y objetivos. La fixture FH3 comprueba
tablas fijas, anotaciones, personality, try/catch e IP-to-state. Ejecute también
los casos ARM al cambiar el parser.

## Ampliar el soporte nativo

Cada forma nativa nueva debe incluir en el mismo cambio:

- parser completo y acotado e invariantes del modelo;
- round-trip HighIR/metadatos LLVM;
- IR nativo verifier-clean para cada grafo aceptado;
- conservación necesaria de secciones y referencias;
- fixture PE enlazada para arquitectura/personality/versión exactas;
- validación de exception-directory, load-config e imagen final;
- pruebas de rechazo explícitas para formas no soportadas cercanas.

Poder decodificar un record no basta para ampliar la allow-list. El criterio es
preservar el comportamiento de excepciones en la imagen final enlazada.

El cleanup del handler seleccionado puede saltar a la misma captura o a otra captura válida del marco padre y cambiar el valor escalar (cero pasa a uno). Cada destino y valor conserva sus efectos sobre el marco; la reanudación restaura los registros, ESP y el límite de desenrollado del destino real. El salto cancela los finally suspendidos y anidados. MedIR vincula cada reemplazo con el punto exacto de llamada del callback y su captura, y exige rutas completas en un grafo acíclico. Ambos búferes deben seguir válidos durante todos los cleanups restantes; se rechazan escrituras superpuestas, búferes caducados, ciclos sin prueba y direcciones de marco que escapan.

Un filtro invocado por RaiseException dentro de un finally suspendido puede saltar a la misma captura o a otra válida, o retornar por otra rama. Los efectos de retorno y salto se mantienen separados; el salto termina el filtro y todos los finally suspendidos y anidados, conservando su destino y valor.

Los bucles finitos de callbacks finally y filtros conservan un marco completo por paso, incluidos contadores en registros, celdas del padre y pila privada. Se rechazan estados completos recurrentes, terminación no demostrada y presupuestos agotados. Cada bloque de copia PHI debe conservar su arista fuente y su callback; el instalador comprueba el índice escape exacto del almacenamiento escalar recuperado y los accesos privados no atómicos de todos sus alias.

Un finally SEH3/EH4 con marco fijo verificado puede restaurar SavedESP y saltar
a una continuación ya atribuida al padre. Un contexto SjLj privado restaura la
pila generada y FS:[0], conserva los registros lógicos actuales y las escrituras
del callback, y entra en el límite exacto del scope original. El instalador
verifica por separado alias, captura, transferencia y selección; un búfer original
no puede sustituir este contexto. Una salida del runtime puede abandonar el nodo
temporal de desenrollado del CRT: antes de otra llamada u operación que pueda
provocar una excepción, el código original debe reconectar su registration activo
o retirarlo al retornar. Se rechazan reparaciones no probadas y destinos fuera
del CFG padre verificado.

Una continuación puede alcanzarse únicamente desde el salto del finally si
todos los caminos decodificados de su tramo final acíclico confluyen en código
padre ya atribuido, sin atravesar una entrada de filter o finally. Esta prueba
física no concede permisos de memoria del handler del runtime; la transferencia
SavedESP y los estados de entrada se verifican por separado. Un retorno sin
conexión al padre o un ciclo no puede establecer esa pertenencia.


Las llamadas ordinarias a finally intercambian EAX/ECX/EDX/EBX/ESI/EDI mediante
posiciones privadas del marco padre, incluidos los valores del retorno cercano
físico. La entrada del runtime conserva su propio contrato de registros y no
reutiliza entradas de una llamada ordinaria anterior. También se conserva un
scope referenciado únicamente por una llamada ordinaria a finally autenticada,
aunque ningún try level del código fuente lo active.

La reconstrucción de `nonlocal_finally_return` de MSVC x86 se verifica con `/Od` y `/O2`, `/GS-` y `/GS`, LLVM de origen optimizado y sin optimizar, ambos modos de parche y dos bases forzadas. Un observador SEH externo comprueba retornos normales y excepcionales, centinelas de pila y restauración de la cadena. La ruta GS conserva el envoltorio original de normalización del código de excepción tras demostrar que su auxiliar escalar no puede observar el marco del llamador.

El retorno directo al padre con `/O2` exige validar la llamada física adyacente, la restauración del ESP guardado, la retirada original de la cadena y el epílogo completo del padre. LowIR demuestra estos efectos de origen, MedIR vincula las operaciones conservadas y el instalador COFF vuelve a comprobar el contrato de forma independiente. El transporte sintético de registros no utilizados no añade parámetros físicos al padre; los valores observados conservan sus dependencias. La forma `goto` no local verificada de MSVC usa el contrato de desenrollado completo descrito a continuación.

Las llamadas verificadas a `_local_unwind2` y `_local_unwind4` conservan la operación fuente exacta, la entrada de importación autenticada, el argumento real del marco padre, el límite sin signo y los pares de estados de retorno. EH4 exige además demostrar la dirección del cookie y la inicialización de los cookies del marco. Una importación sobrescrita, una llamada condicional o una identidad fuente distinta invalida la prueba. MedIR recupera exactamente dos o tres argumentos de pila sin convertir registros vivos incidentales en argumentos. La sustitución nativa admite el desenrollado completo hasta EH3 `-1` o EH4 `-2` tras vincular de forma independiente el puente del marco padre físico, la restauración de SavedESP y las operaciones fuente. El `nonlocal_finally_goto` real de MSVC se ejecuta con `/Od`, `/O2`, `/GS-`, `/GS`, LLVM sin optimizar y optimizado, ambos modos de parche y dos bases forzadas. Los límites parciales y la reanudación dentro de ámbitos externos activos siguen necesitando contratos nativos adicionales.
