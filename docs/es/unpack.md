**Idiomas**: [English](../unpack.md) | [简体中文](../zh-CN/unpack.md) | [繁體中文](../zh-TW/unpack.md) | [日本語](../ja/unpack.md) | [한국어](../ko/unpack.md) | [Français](../fr/unpack.md) | [Deutsch](../de/unpack.md) | [Español](unpack.md) | [Italiano](../it/unpack.md) | [Русский](../ru/unpack.md) | [العربية](../ar/unpack.md)

[← Índice de documentación](README.md)

# Desempaquetado de ejecutables empaquetados

`neverd unpack` recupera el programa que un ejecutable empaquetado reconstruye en su propio espacio de direcciones. Ejecuta la entrada como un proceso invitado acotado, observa dónde el stub entrega el control al código que ha producido y escribe esa imagen como un nuevo archivo del mismo contenedor. No desvirtualiza: las funciones que un protector virtualizó siguen virtualizadas. Compile con `NEVERD_ENABLE_CPU_EMULATION=ON`.

## Entradas admitidas

El contenedor determina cómo se valida y reconstruye un archivo, el conjunto de instrucciones determina cómo se juzga una transferencia, y ambos determinan el perfil de proceso invitado. Una entrada fuera de esta tabla se rechaza por su nombre antes de que se ejecute nada.

| Contenedor (`format`) | Conjunto de instrucciones | Perfil invitado | Prueba de entrada |
| --- | --- | --- | --- |
| PE32+ (`pe64`) | x86-64 | [`windows-pe64-v1`](process-emulation.md) | observación en ejecución |
| PE32+ (`pe64`) | ARM64 | [`windows-pe64-v1`](process-emulation.md) | observación en ejecución |

Las DLL PE32+ se identifican por `IMAGE_FILE_DLL`. Un EXE invitado modelado llama a `LoadLibraryA` y después a `FreeLibrary` mediante el ciclo ordinario de dependencias, TLS y `DllMain`. La entrada DLL aceptada es su llamada de asociación al proceso; no se inventan argumentos para exportaciones arbitrarias. Se conservan nombres, ordinales, alias, datos y reenvíos. Los punteros a exportaciones propias permanecen internos, sin autoimportaciones.

## Uso

```bash
neverd unpack packed.exe -o unpacked.exe
neverd unpack packed.exe -o unpacked.exe \
  --options='{"backend":"unicorn","instruction_limit":400000000,"transfer":2}'
```

El comando imprime un informe JSON. El código de salida 0 significa que la imagen se escribió, 3 que la ejecución acotada terminó antes de que se aceptara una entrada (`outcome` es `no_entry` y no se escribe nada), y 1 que la entrada, una opción o la preparación no son válidas. El informe indica el `format`, la `architecture` y el `profile` que realmente se ejecutaron. El punto de entrada en C es `neverd_unpack_json`; Python ofrece `Session.unpack`. Las opciones son las [opciones de proceso](process-emulation.md) más `transfer`. Los valores predeterminados difieren donde un stub necesita más recursos: 100000000 instrucciones, 600 segundos y 512 MiB, y `windows.defer_unmodeled` está activado.

## Cómo se establece la entrada

La generación cero es la imagen de entrada al mapearla el cargador invitado. Una transferencia inicia código más nuevo. `transfers` registra RVA, `generation`, igualdad de pila (`stack_balanced`) y pertenencia a la entrada de la imagen (`program_invocation`). Para DLL es la asociación al proceso. Antes de esa llamada se usa la pila del primer inicializador.

1. La entrada predeterminada exige `stack_balanced` y `program_invocation`: el stub ha devuelto la pila de la invocación de entrada. `entry_source` es `transfer`.
2. Los callbacks TLS del SO, entradas de DLL dependientes y llamadas de separación no pueden ser la entrada predeterminada aunque tengan pila equilibrada. Las llamadas invitadas más profundas continúan. No se omiten callbacks ni se predice la entrada con firmas del stub.
3. `transfer` selecciona explícitamente una posición de la lista, incluidos callbacks de inicialización y etapas intermedias.

No se predice ninguna entrada a partir de la forma del código de arranque de un compilador. Una ejecución que se detiene antes informa `no_entry` junto con el `stop_reason` del proceso.

Una página de 4 KiB ejecutada sigue siendo observable. Las escrituras RAM del invitado confirmadas invalidan páginas visitadas mediante alias físicos; el modelo de proceso también las revisa después de los servicios del SO, antes de reanudar. La evidencia de transferencia compara la longitud real indicada por el decodificador CPU: cambiar datos vecinos no establece por sí solo una entrada. Se siguen vigilando las generaciones mezcladas y las reescrituras, incluidos callbacks que vuelven al stub antiguo de la misma página.

## La imagen reconstruida

Las secciones conservan RVA, memoria observada (incluidos los efectos de inicializadores ya ejecutados) y permisos. La nueva sección `.neverd` contiene el directorio de importaciones y las nuevas celdas IAT para llamadas exportadas y cargas de direcciones, preservando las zonas de ceros originales. `origin` en `imports` distingue las celdas `static` enlazadas desde la entrada y las `runtime` escritas por el invitado o añadidas al reparar. Como no se observaron las reubicaciones del contenido generado, la imagen queda en la base observada, se elimina ese directorio y se activa `IMAGE_FILE_RELOCS_STRIPPED`.

Los candidatos IAT de ejecución existentes deben formar una matriz contigua de punteros a exportaciones del mismo proveedor con un terminador nulo intacto dentro de la sección original. El terminador del proveedor vecino no sirve.

La recuperación TLS utiliza la identidad de asignación del cargador, una lista completa de callbacks terminada en cero y las llamadas observadas al código generado. Varios candidatos producen un error explícito. No se usan nombres ni bytes específicos de un protector. Al elegir el directorio, la ejecución observada de callbacks generados tiene prioridad sobre un inicializador del cargador ya completado. Si el programa vuelve a llamar a un inicializador del cargador ya completado, sigue siendo una prueba de respaldo, incluso si ese inicializador se ha reescrito.

`materialized_tls_callbacks` cuenta callbacks TLS de asociación de la imagen de entrada que retornaron antes de la captura. La prueba requiere un registro de llamada del SO o una entrada observada en código generado con `(image_base, 1, 0)`, seguida de su dirección de retorno ABI y el puntero de pila restaurado. Sus efectos en memoria ya están en la instantánea. Los adaptadores de la nueva sección `.neverd` evitan solo esa llamada repetida y transfieren las otras notificaciones al callback original mediante llamada terminal. Directorio, tabla y adaptadores usan almacenamiento nuevo; los bytes originales se conservan. La sección es ejecutable si contiene adaptadores y escribible solo si lo exigen nuevas celdas IAT. Los callbacks internos sin esta evidencia conservan su comportamiento. Al adjuntar el proceso, el primer adaptador también restaura el TLS capturado del hilo principal si difiere de la plantilla. La ausencia de evidencia TLS o un tamaño incompatible provoca un fallo explícito. La plantilla original se conserva para futuros hilos; las demás notificaciones nunca restauran el bloque capturado.

La carga diferida permite destinos ejecutables de callback o entrada en memoria inicialmente nula cuyo código generan inicializadores anteriores. Las matrices de callbacks y los metadatos de asignación TLS siguen necesitando contenido de archivo validado; la carga estricta conserva sus comprobaciones. El modelo del SO indica la procedencia de la invocación y avisa al observador al preparar una llamada o restaurar un llamante suspendido. Las vigilancias se reactivan en esos límites, incluso si el callback y la entrada generada comparten página.

`import_repair` cubre dos ejecuciones acotadas adicionales tras capturar la entrada: descubrimiento de llamadas exportadas y observación del estado de sus rutinas. Cada ejecución tiene sus propios límites. Los contadores de instrucciones y eventos son su suma saturada; `stop_reason` y el diagnóstico describen la última, y las llamadas observadas cuentan solo el descubrimiento. Sin continuaciones se omite la observación. Identidades exportadas contradictorias impiden reescribir el sitio. Solo se cubren rutas alcanzadas; `unpacked` no certifica todos los imports ni el éxito del programa.

El orden de enlace de las importaciones puede cambiar las direcciones de exportación. La reparación conserva la identidad de la ejecución que aportó la prueba y observa las exportaciones resueltas después de la entrada; una dirección de otra ejecución no autoriza la reescritura.

El descubrimiento observa el despacho de exports, incluido el no modelado que detiene la ejecución, sin depender solo de registros de llamadas modeladas. Un retorno ABI ilegible no puede justificar una rutina. Reparar una llamada pura a un export opaco conserva la parada explícita por servicio no admitido.

Se observan hasta 256 inicios candidatos en los 256 bytes anteriores a continuaciones reales de llamadas exportadas. `CALL rel32`, con PUSH/POP GPR opcional delante, solo identifica un límite; los bytes siguientes no son una firma. Una ventana de seis a ocho bytes pasa a `call [rip+IAT]` solo si la entrada API tiene una única dirección de retorno apilada, los demás registros (flags y SIMD incluidos), mapeos y RAM persistente intactos, sin llamada previa al SO. Los NOP iniciales conservan el retorno exacto. Siete u ocho bytes pasan a `mov r64, [rip+IAT]` si devuelven un export conocido en un solo GPR, con pila equilibrada y las mismas garantías. El registro resultado se deduce del estado: cualquier GPR salvo RSP, incluidos R8-R15; ocho bytes dejan un NOP final. Solo se excluye espacio temporal bajo el SP del llamador; su pila se compara. Invocaciones posteriores impuras, sin resolver o incompletas invalidan la prueba. Se exige el inicio ejecutado; no se deduce REX del byte anterior y se rechazan inicios solapados con un mismo retorno. `observed_loads` y `repaired_loads` cuentan cargas por separado.

La tabla de secciones original, la disposición del directorio de importación y la tabla de reubicación no se reconstruyen; un empaquetador no las restaura en memoria.

## Identificación

Los campos compatibles `packer.kind` y `packer.evidence` devuelven `unidentified` y una matriz vacía. Se eliminaron el registro de protectores, el analizador de cabeceras comprimidas y las firmas de stub. La API antigua solo valida el contenedor.

## Límites

La ejecución comprobada trabaja por instrucción. x64 Unicorn/KVM/WHP también ofrece `direct-user-x64-v1`, limitado por tiempo y eventos sin contar instrucciones. Las generaciones mezcladas y las escrituras repetidas pueden requerir pasos adicionales del procesador. La recuperación cubre rutas alcanzadas en la imagen de entrada; el código generado fuera no se convierte en su entrada. La inicialización previa puede crear estado externo no transferible, y repetir callbacks TLS puede tener efectos adicionales. Las API no modeladas detienen explícitamente la ejecución. La reparación cubre ventanas de llamada x64 validadas de seis a ocho bytes y cargas de direcciones de siete u ocho bytes; otras formas y rutas no alcanzadas siguen sin resolver. El código virtualizado permanece virtualizado.

## Verificación

`NeverDUnpackTests` comprueba contenedores, asignación segura de IAT, conflictos y rechazos TLS. `NeverDUnpackExecutionTests` prueba UPX NRV2B/NRV2D/NRV2E/LZMA y CRT por la misma ruta genérica en Unicorn/KVM/WHP, compara con el programa enlazado independiente tras su propia inicialización y exige salidas iguales de los backends disponibles. `UnpackGeneratedTests.cpp` genera programas x86-64/ARM64 independientes para transferencias, carga por etapas, imports y ejecución directa x64 Unicorn/KVM/WHP. Los casos nativos obligatorios no pueden omitirse en CI. `NeverDUnpackPublicTests` cubre la ABI C y la CLI. `unittests/unpack/fixtures/Makefile` regenera los ejemplos UPX.


`UnpackLibraryTests.cpp` empaqueta DLL x64/ARM64 independientes en el test y comprueba orden de dependencias, callbacks TLS ordinarios/generados, limpieza tras fallos, identidades entrada/anfitrión, acceso al propio archivo, nombres/ordinales/datos/reenvíos y ausencia de autoimportaciones. Windows nativo carga DLL originales y reconstruidas con un EXE separado y llama a exportaciones declaradas; WHP comprobado y directo son obligatorios. `CompletedGeneratedTLSCallsRequireTheAttachABI` rechaza entradas/argumentos cambiados; `GeneratedCallsNeedTheirReturnedStackAtTheContinuation` rechaza una pila de retorno incorrecta. Se verifica desempaquetado, sin desvirtualización.

`ExportObserver` también observa exportaciones ejecutables de dependencias invitadas residentes; los proveedores modelados siguen observándose en el despacho de servicios. Se excluyen exportaciones de la propia entrada. Los cambios de módulos actualizan las paradas y cada reparación exige identidad actual. Los registros respetan el límite de importaciones declarado. Los tests DLL reparan un helper API y otro de dependencia; la carga nativa verifica que no queden direcciones emuladas.


`WrappedEntriesRequireExplicitTransferEvidence` cubre un wrapper DLL que llama a la entrada restaurada con una pila más profunda. El resultado predeterminado sigue siendo `no_entry`; seleccionar la llamada observada con `transfer` reconstruye una DLL cargable. La profundidad sola no distingue entrada e inicializador.