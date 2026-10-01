**Idiomas**: [English](../cpu-execution.md) | [简体中文](../zh-CN/cpu-execution.md) | [繁體中文](../zh-TW/cpu-execution.md) | [日本語](../ja/cpu-execution.md) | [한국어](../ko/cpu-execution.md) | [Français](../fr/cpu-execution.md) | [Deutsch](../de/cpu-execution.md) | [Español](cpu-execution.md) | [Italiano](../it/cpu-execution.md) | [Русский](../ru/cpu-execution.md) | [العربية](../ar/cpu-execution.md)

[← Índice de documentación](README.md)

# Ejecución de CPU y consultas de capacidades

La ejecución de CPU es independiente del sistema operativo invitado, del cargador de imágenes y de la convención de llamada. `NEVERD_ENABLE_CPU_EMULATION` permite compilarla por separado; `NEVERD_ENABLE_DRIVER_EMULATION` también incluye el modelo de controladores de Windows. La [guía de arquitectura](architecture.md) describe la propiedad, la selección del backend y las limitaciones actuales.

## Configuración

La interfaz pública [`ExecutionConfiguration`](../../include/neverd/emulation/ExecutionConfiguration.h) es compartida por la fábrica de CPU y el informe de capacidades. Los requisitos se validan antes de asignar una CPU o adjuntar un espacio de direcciones. Los valores omitidos usan el perfil fijo del contrato; los valores explícitos no admitidos fallan.

| Campo JSON | Predeterminado | Significado |
|---|---|---|
| `backend` | `auto` | `auto`, `unicorn`, `kvm` o `whp` |
| `contract` | `software-cpu-v1` | Semántica de ejecución versionada |
| `architecture` | `x86_64` | `x86_64` o `aarch64` |
| `privilege` | Perfil del contrato | `flat`, `supervisor` o `user`; debe coincidir con el contrato |
| `virtual_address_bits` | Perfil del contrato | Los perfiles comprobados usan 48 bits; los planos, un espacio de mapeo directo de 64 bits |
| `page_size` | 4096 | Granularidad de mapeo invitado; se rechazan otros valores |
| `required_features` | `[]` | Nombres requeridos de [`ExecutionConfiguration.def`](../../include/neverd/emulation/ExecutionConfiguration.def) |

`driver-strict` acepta x64; `software-cpu-v1`, x64 y ARM64. `checked-x64-v1` y `checked-aarch64-v1` exigen la arquitectura indicada y ejecutan en modo supervisor. `checked-user-x64-v1` y `checked-user-aarch64-v1` ejecutan el inventario acotado correspondiente al contrato en CPL3 y EL0, respectivamente, con aislamiento MMU y salidas explícitas de solicitud de servicio. Admiten Unicorn y KVM/WHP compatibles con el host; `auto` sigue la selección del host. Los perfiles planos no garantizan aislamiento arquitectónico usuario/supervisor. El perfil ARM64 comprobado rechaza FP/SIMD; x64 admite las familias acotadas que se detallan abajo. El x64 supervisor añade transacciones MMIO limitadas y lecturas de cadenas preparadas; los perfiles de usuario rechazan mapeos de dispositivos. Todos los perfiles comprobados siguen rechazando E/S de puertos y requisitos de CPU paralelas. Solo los perfiles de usuario anuncian `service_traps`.

La ejecución de usuario necesita `UserAccessible` y el permiso apropiado `Read`, `Write` o `Execute` en **cada** página mapeada. Los mapeos existentes son de supervisor por defecto; los alias tienen permisos independientes aunque compartan bytes físicos. `UserAccessible` por sí solo no concede acceso. Las operaciones confiables del host y las CPU supervisoras usan RWX. Ejemplo:

```cpp
Configuration.Contract = ExecutionContract::CheckedUserX64;
Configuration.Privilege = ExecutionPrivilege::User;
auto CPU = llvm::cantFail(createExecutionBackend(Configuration, Space)).CPU;
llvm::cantFail(CPU->map(Code, 4096, Read | Write | Execute | UserAccessible));
```

El contrato fija el privilegio. Restaurar contexto o enlazar otro espacio no lo cambia y los selectores de segmento x64 no pueden elevarlo. Un fallo de datos recuperable conserva la instrucción y los registros originales hasta que lo resuelva su propietario. `canAccess` comprueba exactamente los permisos solicitados; incluye `UserAccessible` para consultar visibilidad de usuario. Las tablas de páginas son proyecciones privadas de CPU: no se exponen tablas invitadas mutables ni una API de cambio de privilegio. En ARM64, las páginas de usuario tampoco son ejecutables en EL1.

Los campos desconocidos o nulos, nombres/anchos numéricos inválidos, funciones requeridas duplicadas y combinaciones no admitidas fallan. La entrada se limita a 64 KiB y las fábricas CPU antiguas y opciones C de controladores conservan compatibilidad.

## Consultar sin ejecutar una carga

```bash
neverd cpu-capabilities
neverd cpu-capabilities \
  --configuration='{"contract":"checked-aarch64-v1","architecture":"aarch64"}' \
  --probe-host
```

El esquema es versión 1. El informe separa `requested_configuration` (antes de completar valores del perfil), `configuration` normalizada, `capabilities` semánticas estáticas, `build` (adaptador y compatibilidad ABI) y `host` (null salvo que se pida `--probe-host`). La consulta prueba la inicialización de una CPU temporal sobre RAM privada; no certifica una carga ni ejecución nativa ARM64. La disponibilidad puede cambiar y nunca se sustituye silenciosamente un backend no disponible. El CLI devuelve 0 para un informe válido, incluso si el backend no está disponible, y 1 para configuración o consulta inválida.

## Límites del SDK y C++

[`neverd_cpu_capabilities_json`](../../include/neverd/sdk/NeverDCAPICPU.h) acepta una sesión, JSON de configuración opcional y `ProbeHost` con valor 0 o 1; no requiere una imagen cargada. Libera el resultado con `neverd_free_string`; NULL indica un error descrito por `neverd_last_error`. Las compilaciones sin CPU exportan la misma función e informan de la desactivación. Los plugins Python consultan `session.cpu_capabilities(...)`. En C++ se pueden usar por separado `executionCapabilities`, `resolveExecutionConfiguration`, `queryExecutionBackendBuild` y `probeExecutionBackend`; `createExecutionBackend` conecta una CPU a un espacio existente o crea RAM y un espacio predeterminados.

## Resultados de CPU y presupuestos

`CPU.runUntilExit(PC, TimeoutMicroseconds)` devuelve un [`ExecutionExit`](../../include/neverd/emulation/ExecutionExit.h) tipado. Los errores de configuración devuelven `llvm::Error`; una ejecución iniciada informa de parada, plazo, solicitud de servicio, fallo recuperable, fallo/trampa invitada, operación no admitida o error de dispositivo/backend/parada inexplicada. Fallos de CPU/dispositivo/backend prevalecen sobre una parada o plazo simultáneos, conservando los hechos y detalles independientes. El plazo debe ser positivo y representable como duración y deadline absoluto; si no, falla antes de modificar la CPU. Cada invocación requiere un presupuesto finito; cero no significa ilimitado ni un timeout inmediato válido. El control es cooperativo, no un límite de reloj estricto. El resultado no consume un fallo recuperable: el propietario del SO debe recogerlo e instalar un salto de excepción validado antes de reanudar. Se mantienen `run`, `fault` y `timedOut`; las implementaciones externas que solo sobrescriben `run` rechazan el nuevo límite tipado.

## Solicitudes de servicio

El perfil user-x64 intercepta solo la codificación exacta de `SYSCALL` sin prefijo; user-ARM64 intercepta `SVC #imm16`. `SYSENTER`, `INT`, `HVC`, `BRK` y otros mecanismos siguen sin admitirse. Primero se ejecuta el observador de instrucciones. Si no detiene ni falla, la CPU devuelve `ExecutionExitKind::ServiceRequest` **antes** de ejecutar la instrucción o entrar al backend, con tipo, `PC` original, `NextPC` secuencial e inmediato SVC. No cambian registros, flags, stack ni privilegio: RCX/R11 aún no reciben los clobbers de SYSCALL y ARM64 no entra en un vector de excepción. El inmediato SVC no es un número de servicio universal.

La solicitud queda pendiente y bloquea ejecución, mutaciones, cambios de espacio y captura/restauración de contexto hasta que el propietario del SO la consuma exactamente una vez mediante `takeServiceRequest()` con la CPU detenida. El propietario interpreta la ABI del SO, gestiona el servicio y establece explícitamente registros de resultado y el PC/salto de excepción. Un servicio no admitido falla en esa capa; volver a intentar el PC original produce otra solicitud, nunca un NOP o un resultado exitoso inventado. El evento de servicio prevalece sobre stop/plazo simultáneos, pero los fallos de invitado/backend prevalecen sobre él. Una CPU detenida, HLT, deadline o trampa no demuestra que la carga haya terminado correctamente. El [perfil de procesos Linux](process-emulation.md) tiene su propio modelo de SO y no demuestra compatibilidad con Windows, Android o Darwin. La ejecución nativa en Windows y ARM64 aún necesita validación real.

## Extensiones x64 y estado CPU nativo

El x64 comprobado admite movimientos y lógica SSE/SSE2 heredados acotados, `MOVLHPS`/`MOVHLPS` y las formas escalares enmascaradas `CVTTSS2SI`/`CVTTSD2SI`/`SUBSS`/`SUBSD`. MXCSR conserva estado sticky, redondeo y FTZ; se rechazan DAZ y excepciones no enmascaradas. ARM64 comprobado sigue rechazando FP/SIMD. KVM/WHP sincronizan los 16 registros XMM y MXCSR; las codificaciones y operandos no enumerados siguen rechazados.

El x64 comprobado admite también las formas heredadas enmascaradas `SS`, `SD`, `PS`, `PD` de `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `MIN`, `MAX`. `X64SSEInstructions.def` centraliza anchuras, alineación y admisión. `MaskedSSEArithmeticMatchesIndependentHostExecution` compara registros/RAM con un oráculo CPU anfitrión independiente: cuatro redondeos, FTZ, ceros con signo, subnormales y NaN. `SSEMemoryObserverStopsBeforeResultAndStatusChanges` comprueba la parada antes de los efectos. DAZ, excepciones sin máscara, x87 y AVX siguen excluidos.

El puntero de hilo cubre FS/GS en x64 y `TPIDR_EL0` en ARM64 con codificaciones exactas `MRS`/`MSR`. Los transportes nativos y snapshots CPU preservan ese estado independientemente de la memoria; esto no crea hilos del SO ni bloques TLS. El supervisor x64 admite transacciones MMIO escalares alineadas de 1/2/4 bytes y un elemento MOVS por límite de reinicio. Las lecturas desde dispositivos requieren una vista previa pura y una confirmación como máximo una vez. Los perfiles de usuario rechazan mapeos de dispositivos; también se rechazan RMW, MMIO ancho y E/S de puertos.

KVM y WHP cancelan entradas nativas activas y reconocen la cancelación antes de retirar recursos. KVM usa un hilo privado y desbloquea temporalmente una señal realtime; la señal elegida no debe estar ignorada durante la entrada. No modifica las máscaras ni los handlers del llamador. Si el progreso invitado es incierto, la cancelación es terminal; no se garantiza un plazo de reloj estricto.

## Excepciones síncronas nativas x64

Los `DIV`/`IDIV` checked x64 usan resultados reales del procesador y `#DE`. KVM utiliza una IDT/IST supervisor privada y WHP un mapa explícito; el contexto original y los códigos disponibles se distinguen de los errores de transporte. El SO consume el evento recuperable antes de instalar la continuación. Los controladores Windows traducen la división por cero y el desbordamiento del cociente a `STATUS_INTEGER_DIVIDE_BY_ZERO`, ejecutando filtros SEH, `__finally` y reintentos reales. `NeverDX64ExceptionTests` se compila sin Unicorn; `DriverWDMCPUException` verifica casos WDK originales. Los hosts WHP/ARM64 no disponibles se omiten explícitamente.

## Efectos de RAM preparados

`RAMTransaction` conserva únicamente la unión física de las escrituras declaradas de una instrucción, bajo el bloqueo de ejecución. Restaura la RAM original antes de los observadores de resultados; una cancelación, un error de transporte o una excepción del observador no publica RAM ni registros parciales. Tras revertir la RAM, los fallos de CPU conservan el estado arquitectónico de excepción. Las escrituras simples y dobles de ARM64 usan la misma autoridad. x64 ejecuta `XCHG`, `XADD` y `CMPXCHG` de 8/16/32/64 bits, con alineación natural para formas bloqueadas o con bloqueo implícito. `NeverDRAMTransactionTests` compara resultados con la CPU del host y verifica reversión, alias y permisos; omite explícitamente plataformas no disponibles. Los dispositivos y SMP paralelo siguen fuera del contrato; las instantáneas de CPU no revierten RAM ya confirmada.

## Estado x87 completo

`NeverDEmulationArch` posee los contratos ISA, las tablas de páginas y el formato FP compartido por los transportes nativos y Unicorn. Los contextos x64 conservan control, estado, TOP, etiquetas físicas, código de operación, punteros de instrucciones/datos y ocho registros de 80 bits. `FP0`–`FP7` usan `RegisterValue`; el acceso escalar rechaza el truncamiento. `FPTag` es la máscara física de registros no vacíos. `NeverDX64FPTests` comprueba todos los TOP, operaciones exactas frente a FXSAVE/FXRSTOR del host y restauración. Esto no admite instrucciones x87 en checked ni prueba todos los redondeos. Los hosts nativos no disponibles se omiten explícitamente.

`driver-strict` admite KVM en anfitriones Linux x64 compatibles y WHP en Windows x64 compatibles; `auto` elige ese transporte nativo, y las ISA diferentes usan Unicorn. Unicorn explícito y la API V1 conservan el perfil portátil. La ejecución nativa comprueba direcciones canónicas y efectos antes de entrar; hardware no disponible falla sin alternativa. Instrucciones y comportamiento OS no admitidos fallan explícitamente. Faltan pruebas nativas ARM64/WHP; esto no establece compatibilidad universal de controladores ni de Android/Darwin.

Consulte el perfil seleccionado con `executionCapabilities(Contract, ISA, Backend)`. `NativeLegacyX64` describe la ejecución nativa de controladores x64. `NeverDNativeDriverTests` valida el corpus existente y también puede ejecutarse en una compilación sin Unicorn.

La captura nativa de enteros ARM64 tiene una autoridad ISA común. `AArch64GeneralState.def` enumera X0–X30, SP, PC, NZCV y TPIDR_EL0; `captureAArch64GeneralState` prepara todas las lecturas antes de normalizar NZCV y publicar el resultado completo. KVM y WHP comparten esta función. Una lectura fallida conserva todo el estado de entrada; privilegio, vectores y registros no transferidos permanecen intactos. No se amplía la admisión nativa de FP/SIMD.

Los accesos RAM escalares y por pares de ARM64 comprobado pueden cruzar páginas con almacenamiento separado o alias en EL0 y EL1. La ISA calcula los rangos; el espacio de direcciones compartido valida cada página antes de entrar e informa del primer fragmento fallido. `RAMTransaction` confirma los bytes físicos declarados tras un paso CPU completo. Los fallos y las paradas del observador conservan RAM, registros y actualización de dirección. `NeverDAArch64MemoryTests` usa casos ensamblados en `AArch64CrossPageCases.def`; esto no añade FP/SIMD ni carga de controladores Windows ARM64.

KVM x64 utiliza `KvmRunControl` para preparar el estado, entrar en `KVM_RUN` y recoger el estado en el mismo hilo vCPU privado. La preparación se realiza una sola vez antes de los reintentos por `EINTR`; la recogida solo ocurre tras el retorno correcto de una entrada del host. Los callbacks de transferencia prestados siguen vigentes hasta la confirmación del fin de la entrada. La decodificación ISA, las transacciones RAM, la política del SO y los observadores de ejecución permanecen en el hilo llamador. Un fallo de preparación omite la entrada y la recogida; un fallo de recogida o una cancelación impide publicar el estado invitado.

KVM compara los registros generales y el estado FP/SSE completo con la última recogida de depuración confirmada mediante `X64HostRegisters.def` y `X64FPState.def`, y reinstala las entradas modificadas. Las escrituras del host y las restauraciones de contexto también se comparan; las excepciones, cancelaciones y fallos invalidan la reutilización. El paso único y la lectura del estado general/FP real siguen realizándose en cada instrucción.

La ejecución por hardware no garantiza por sí sola una menor latencia total. La ejecución nativa actual realiza admisión, observación, transferencia de estado y salida de VM por instrucción. Compare las mismas imágenes y escenarios originales con presupuestos idénticos de instrucciones y eventos, e informe de la igualdad de resultados junto con los tiempos; incluya inicio y carga al medir la latencia CLI.
