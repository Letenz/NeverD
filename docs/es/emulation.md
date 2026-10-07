**Idiomas**: [English](../emulation.md) | [简体中文](../zh-CN/emulation.md) | [繁體中文](../zh-TW/emulation.md) | [日本語](../ja/emulation.md) | [한국어](../ko/emulation.md) | [Français](../fr/emulation.md) | [Deutsch](../de/emulation.md) | [Español](emulation.md) | [Italiano](../it/emulation.md) | [Русский](../ru/emulation.md) | [العربية](../ar/emulation.md)

<!-- i18n-source: a3e64122b77a690dd856d02f5b2af53973d3bf1affd973bea5f9735aa9dd6722 -->

[← Índice de documentación](README.md)

# Ejecución CPU y entornos invitados

<!-- i18n-section: backends -->

## Backends de CPU y cargas de trabajo

La ejecución CPU separa admisión ISA, memoria invitada, transporte del motor y política del SO. `NEVERD_ENABLE_CPU_EMULATION` activa la capa CPU x64/ARM64; `NEVERD_ENABLE_DRIVER_EMULATION` añade el entorno Windows WDM/KMDF x64 acotado. `linux-elf64-v1` ejecuta procesos Linux ELF admitidos. Véase [Ejecución CPU](cpu-execution.md), [Emulación de procesos invitados](process-emulation.md) y [Emulación de controladores Windows](driver-emulation.md).

Para los contratos nativos admitidos, `auto` elige KVM en Linux, WHP en Windows o [HVF en macOS](macos-hvf.md), con la misma ISA en huésped y host. Las ISA diferentes usan Unicorn; `software-cpu-v1` y la API V1 original conservan la ejecución por software. Si el backend elegido explícitamente no está disponible, falla sin alternativa automática. La ejecución nativa comprueba instrucciones admitidas, direcciones y efectos antes de entrar. La virtualización del host no determina el SO invitado: los [perfiles Darwin](darwin-emulation.md) modelan por separado macOS, iOS e iOS Simulator. HVF requiere el permiso `com.apple.security.hypervisor`.

`driver-strict` / `checked-x64-v1` cubre ejecución x64 acotada; la carga de controladores Windows sigue limitada a x64. `checked-aarch64-v1` y `checked-user-aarch64-v1` incluyen ARM64 FP32/FP64 acotado, SIMD fijo y estado FPCR/FPSR/vectorial completo. La guía Mac registra la aceptación nativa ARM64 HVF; quedan pendientes la validación de cargas ARM64 KVM/WHP y la aceptación completa Intel HVF. El soporte de CPU no implica compatibilidad con cualquier controlador o aplicación.

<!-- i18n-section: windows-processes -->

## Procesos y módulos Windows

`windows-pe64-v1` admite procesos de consola Windows x64/ARM64 acotados con PEB/TEB, TLS estático y dinámico, `DllMain`, API Win32 con nombre y grafos DLL explícitos sin ciclos. Los módulos admiten código/datos por nombre u ordinal, DIR64, exportaciones reenviadas e identidades reales del cargador. `LoadLibraryA` / `LoadLibraryW`, `FreeLibrary` y `GetProcAddress` usan el catálogo configurado. CRT/GUI, SEH de usuario ARM64 basado en marcos, hilos y compatibilidad general de Windows siguen pendientes; falta evidencia nativa ARM64 KVM/WHP.

`WindowsSystemModules` construye imágenes modelo PE64 acotadas de `ntdll.dll`, `kernelbase.dll` y `kernel32.dll` para ambas ISA. Las consultas ASCII `GetModuleHandleA` / `GetModuleHandleW`, `LoadLibraryA` / `LoadLibraryW` y `GetProcAddress` comparten sus bases mapeadas; PEB/LDR y `MEM_IMAGE` describen esas mismas imágenes. Importaciones estáticas, búsquedas por nombre y reenvíos invitados usan los mismos puntos de entrada API y resolución de exportaciones. Los proveedores permanecen residentes, sin callbacks invitados de inicialización, y no impiden retornar desde la entrada tras descargar las DLL invitadas ordinarias. Cambiar cabeceras o metadatos de exportación detiene la búsqueda. Los nombres de sistema no modelados y ordinales no nulos se rechazan explícitamente; diferencias solo de mayúsculas en nombres modelados y nombres vacíos devuelven 127, una consulta NULL devuelve 87. Los bytes y direcciones generados son política del modelo; no se reconstruyen diseños por versión de Windows, ordinales nativos ni alias entre proveedores. `WindowsSystemTests.cpp` compara EXE originales x64/ARM64 con Windows nativo e incluye ocho observaciones independientes del retorno del hilo inicial.

<!-- i18n-section: environment-memory -->

## Entorno y memoria

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` comparten el bloque actual del entorno invitado en los parámetros del proceso del PEB. Los nombres ASCII no distinguen mayúsculas; los valores son UTF-16. Las modificaciones validan entradas, capacidad y permisos de escritura antes de publicarse. Las instantáneas son independientes de los cambios posteriores y liberan su memoria invitada. El modelo limita el bloque a 64 KiB; cadenas y expansiones están acotadas y comprueban el plazo. Siguen sin admitirse punteros de propiedad desconocida, bloques malformados, páginas de códigos ANSI y búferes de expansión superpuestos. `WindowsEnvironmentTests.cpp` compara fixtures originales x64/ARM64 en los backends disponibles; CI exige un oráculo Windows nativo independiente.

`WindowsProcessHeap` unifica asignación, `HeapReAlloc`, liberación y consulta del tamaño del heap del proceso. El cambio de tamaño conserva los bytes retenidos; `HEAP_ZERO_MEMORY` pone a cero los bytes añadidos y `HEAP_REALLOC_IN_PLACE_ONLY` impide mover el bloque. Un cambio de tamaño fallido conserva el bloque y devuelve NULL con `ERROR_NOT_ENOUGH_MEMORY` (8), como en las observaciones nativas. Las páginas independientes devuelven capacidad al reducir o liberar; el crecimiento preparado y las copias acotadas comprueban el plazo. Heaps personalizados, indicadores de excepciones, propiedad desconocida y rangos inaccesibles detienen la ejecución explícitamente. `WindowsHeapTests.cpp` cubre ambas ISA, movimiento forzado, reutilización del presupuesto y fallos atómicos; CI ejecuta el mismo EXE original en Windows nativo.

La memoria virtual de Windows incorpora `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` y `FlushInstructionCache` para el proceso actual. La capa OS administra las reservas; `AddressSpace` mantiene la autoridad sobre páginas confirmadas, permisos y almacenamiento. Las pruebas cubren cambios de código, fallos de acceso y reutilización del presupuesto de memoria.

`WriteProcessMemory` sigue el comportamiento de páginas confirmadas observado en x64/ARM64 para escrituras de hasta 4 KiB en el proceso actual. Conserva la protección de cada región, los prefijos copiados, los recuentos de bytes y LastError, incluidos `ERROR_NOACCESS`, `ERROR_PARTIAL_COPY` y el éxito tras un prefijo RX. `WindowsMemoryWriteTests.cpp` comprueba las 25 parejas de protecciones; `check_windows_memory_write.py` verifica el mismo ejecutable original en CI de Windows nativo. Los destinos sin confirmar siguen explícitamente sin soporte.

<!-- i18n-section: vectored-exceptions -->

## Excepciones vectorizadas y continuación

`WindowsProcessExceptions` implementa `AddVectoredExceptionHandler`, `RemoveVectoredExceptionHandler` y `RaiseException` en la misma CPU y presupuesto del proceso. Los manejadores ordenados pueden modificar registros, generar excepciones anidadas, llamar API modeladas, cargar DLL y terminar el proceso. Las infracciones de datos x64/ARM64 y divisiones enteras x64 reanudan tras validar cambios del invitado en `CONTEXT`, conservando registros generales, SIMD y estado FP admitido. Las excepciones de software continúan mediante una instrucción real de retorno del proveedor modelado. Límites: 128 registros retenidos y 16 marcos anidados. Resultados inválidos, punteros modificados, campos no admitidos y excesos fallan explícitamente. SEH/desenrollado ARM64 basado en pila, depuración y fallos de ejecución/guarda siguen sin soporte. `WindowsExceptionTests.cpp` compara EXE/DLL originales con Windows nativo; aún faltan pruebas nativas ARM64 KVM/WHP. Los registros de excepciones de software incluyen `EXCEPTION_SOFTWARE_ORIGINATE` (`0x80`), independientemente de la marca de no continuación del llamador; el ejecutable original de Windows comprueba los valores exactos de las marcas de las excepciones de software y hardware.

`AddVectoredContinueHandler` y `RemoveVectoredContinueHandler` mantienen una lista ordenada independiente y comparten con los manejadores de excepción el límite de 128 registros retenidos. Cuando un manejador vectorizado acepta continuar, los callbacks de continuación reciben el mismo registro modificable y `CONTEXT`. La validación final ocurre después de estos callbacks, incluidas las excepciones anidadas y las notificaciones DLL. No se pueden retirar identificadores mediante la otra familia de manejadores. `WindowsContinuationTests.cpp` compara EXE originales con Windows nativo para orden, terminación anticipada, cambios de registro, reparación del contexto, anidamiento, callbacks del cargador y salida del proceso. La ruta vectorizada probada en Windows x64 permite continuar con `EXCEPTION_NONCONTINUABLE`; esto no demuestra el comportamiento de SEH basado en pila. La ejecución ARM64 nativa sigue sin verificar.

<!-- i18n-section: caller-context -->

## Contexto del llamador

`RtlCaptureContext` está disponible mediante `kernel32.dll` y `ntdll.dll` para x64 y ARM64. Los componentes compartidos `WindowsProcessContext` e `IntegerABI` guardan PC/SP del llamador sin cambiar el estado de CPU ni LastError. Las observaciones nativas de Windows confirman los indicadores x64 `0x10000f`, la conservación de áreas home/depuración/vectores no escritas y los campos históricos de direcciones x87 de 32 bits; ARM64 copia LR a PC y pone a cero X0/LR en el registro. Registros, SIMD y controles flotantes proceden del invitado; los selectores x64 y la máscara de capacidades MXCSR siguen la CPU invitada configurada. Los destinos inválidos, desalineados o parcialmente inaccesibles fallan antes de publicar datos. `WindowsContextTests.cpp` cubre importaciones directas, consultas a proveedores, callbacks VEH, salidas entre páginas y atomicidad de los fallos. `scripts/check_windows_context.py` ejecuta el programa original en Windows x64/ARM64, con un oráculo nativo separado para el estado x87 no vacío. Estas observaciones ARM64 no prueban ejecución nativa KVM/WHP. Restauración de contexto, recorrido de pila y tablas dinámicas de funciones siguen pendientes. `WindowsProcessServices.def` declara restricciones exactas por módulo: la búsqueda en `kernelbase.dll` devuelve `ERROR_PROC_NOT_FOUND` (127), según las observaciones nativas, sin inventar una exportación. [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

<!-- i18n-section: structured-exceptions -->

## Manejo estructurado de excepciones

`WindowsProcessSEH` usa el `X64SEH` compartido de `os/windows/exception/` (`NeverDEmulationWindowsException`, disponible sin controladores) para x64 `__C_specific_handler` y UNWIND_INFO V1. Tras buscar VEH, admite filtros, finally, transferencia no local, despacho anidado/desenrollado en colisión y marcos EXE/DLL reubicados, preservando GPR/XMM no volátiles. La continuación por filtro ejecuta VCH con el mismo `CONTEXT`. `WindowsSEHTests.cpp` compara 23 escenarios originales con Windows nativo; KVM/WHP/Unicorn comparten la semántica. El presupuesto del proceso cubre la revalidación de generaciones, cabeceras, bytes de desenrollado/ámbitos, regiones del manejador de lenguaje y enlaces IAT. Los metadatos cambiados o imágenes retenidas descargadas fallan explícitamente. SEH ARM64 por marcos, C++ EH, tablas dinámicas, RtlUnwind/NtContinue generales y desenrollado a través de callbacks del cargador/VEH/VCH siguen sin soporte.

Para `EXCEPTION_NONCONTINUABLE`, un filtro x64 que devuelve `EXCEPTION_CONTINUE_EXECUTION` genera `STATUS_NONCONTINUABLE_EXCEPTION` (`0xc0000025`, indicadores `0x81`, registro enlazado nulo) con un contexto nuevo. VEH se ejecuta de nuevo antes de buscar en la pila lógica conservada, manteniendo el orden finally, la identidad de los marcos EXE/DLL y los mismos presupuestos de profundidad y ejecución. Los 23 escenarios nativos incluyen 21 ejecuciones correctas y dos terminaciones: aceptar en VEH/VCH la continuación de esta excepción secundaria la deja sin gestionar incluso tras restaurar el `CONTEXT` original. El modelo informa de un fallo de ejecución. Las direcciones de excepciones de software coinciden con el PC guardado; las direcciones del despachador interno y la disposición de registros son decisiones del modelo. [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

<!-- i18n-section: processor-state -->

## Instrucciones y estado del procesador

El perfil x64 verificado incluye `MOVS/STOS/LODS` sobre RAM ordinaria y `CLD/STD`, con reanudación, cancelación y comprobación de páginas por elemento. Los bits altos con contador cero propios de cada CPU y los operandos de dispositivo STOS/LODS quedan fuera del contrato.

El perfil x64 verificado admite también `CMPS/SCAS` sobre RAM ordinaria con `REPE/REPNE`, indicadores aritméticos, salida anticipada, paradas por elemento y recuperación de fallos. Se excluyen las comparaciones de dispositivos.

Las sondas nativas x64 y ARM64 validan ejecución completa acotada con permiso exclusivo de memoria. Los paquetes XSAVE y cachés de tablas identificados por ISA tienen una autoridad única.

Los campos x64 nativos `FOP/FIP/FDP` siguen las reglas de guardado/restauración del host: AMD puede borrar metadatos x87 inactivos. Las pruebas de inicio los validan con una excepción pendiente sin máscara.
