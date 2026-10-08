**Idiomas**: [English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← Índice de documentación](README.md)

# Emulación de procesos invitados

`neverd emulate` ejecuta una imagen bajo un perfil explícito de sistema operativo invitado. El transporte CPU, el análisis de la imagen, la entrada del proceso y los servicios del SO tienen propietarios separados. Activa `NEVERD_ENABLE_CPU_EMULATION=ON`; la emulación de controladores también lo incluye.

El primer perfil, `linux-elf64-v1`, ejecuta ELF `ET_EXEC` x64/AArch64 y PIE estáticos `ET_DYN` con autorrelocación en CPL3 o EL0. Carga segmentos ELF reales, construye la pila inicial, reanuda por intervalos y atiende solicitudes explícitas de llamadas al sistema Linux. Es un modelo de proceso independiente, no una distribución Linux completa ni una promesa de ejecutar binarios libc arbitrarios. El enlace dinámico, señales, hilos, sistemas de archivos y servicios no admitidos fallan explícitamente.

<!-- i18n-section: cli-sdk -->

## CLI y SDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

Hosts Linux compatibles seleccionan KVM y hosts Windows compatibles, WHP. Otras combinaciones de ISA anfitrión/invitada usan Unicorn. Un backend elegido que no esté disponible causa error, sin fallback silencioso. El ELF mantiene el perfil Linux aunque se ejecute en Windows. Consulta [ejecución CPU](cpu-execution.md) para el inventario de instrucciones y sus límites.

El CLI emite un único informe JSON. Código de salida 0: estado invitado cero; 2: otro estado; 3: ejecución incompleta (incluidos fallos y límites); 1: configuración/API inválida. El estado real aparece en `exit_status`. La entrada C aditiva [`neverd_emulate_process_json`](../../include/neverd/sdk/NeverDCAPIProcess.h) recibe una sesión, ruta no vacía, perfil explícito y JSON de opciones opcional. Libera el resultado con `neverd_free_string`; NULL indica error de configuración, descrito por `neverd_last_error`. Un fallo invitado o parada de recursos devuelve un informe. No requiere ni modifica la imagen de análisis de la sesión.

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

<!-- i18n-section: options-results -->

## Opciones y resultados

Las opciones son un objeto JSON de hasta 64 KiB. Se rechazan campos desconocidos/null, tipos inválidos, NUL incrustados y límites no positivos.

| Opción | Predeterminado | Contrato |
|---|---|---|
| `backend` | `auto` | `auto`, `unicorn`, `kvm` o `whp` |
| `arguments` | Nombre del archivo | argv completo, incluido argv[0]; vacío usa el predeterminado |
| `environment` | `[]` | Cadenas explícitas del invitado; nunca hereda el entorno del host |
| `instruction_limit` | 100000 | Intentos de instrucción admitidos y compartidos |
| `event_limit` | 10000 | Eventos syscall, contabilizados antes de atender el servicio |
| `timeout_microseconds` | 5000000 | Deadline monotónica iniciada tras preparar el proceso |
| `memory_limit` | 67108864 | Presupuesto de memoria física/mapeada |
| `stack_size` | 1048576 | Pila alineada a página dentro del presupuesto |
| `output_limit` | 1048576 | Bytes combinados capturados de stdout/stderr |
| `instruction_quantum` | 1024 | Intervalo de admisión antes de ceder a la runtime |
| `linux_kernel` | Ausente | Rama GKI explícita, catálogo fijo de tareas invitadas o interfaces del kernel observadas como ausentes |
| `linux_priority` | Ausente | Valores nice explícitos por tarea y autoridad del llamante para los servicios Linux de prioridad sin envoltorio |

`schema_version` vale 1. El informe incluye perfil, arquitectura, backend seleccionado y motivo, `stop_reason`, `exit_status` anulable, diagnóstico, PC de entrada/actual, contadores, registros de servicios y última salida CPU tipada. Direcciones, números syscall, registros de argumentos y bits de retorno son cadenas hexadecimales **sin** `0x`; `stdout_hex`/`stderr_hex` preservan NUL y UTF-8 inválido. Un resultado syscall null significa que no hay retorno modelado (por ejemplo, exit o solicitud no admitida), no un cero exitoso.

<!-- i18n-section: linux-semantics -->

## Semántica del perfil Linux

`writev` comparte estas salidas en x64/ARM64 y Android Bionic. Importa hasta 1024 entradas `iovec` antes de producir salida, rechaza longitudes negativas con `EINVAL`, valida los rangos de usuario y aplica el límite de transferencia de Linux alineado por páginas. Un descriptor inválido devuelve `EBADF` antes de acceder al vector; los metadatos ilegibles devuelven `EFAULT` sin salida. Un fallo posterior de datos conserva el prefijo copiado. El presupuesto abarca todo el vector y ambos flujos antes de publicar. `write` y `writev` usan los 32 bits bajos del descriptor; la cantidad también sigue la importación Linux de 32 bits. Solo Bionic convierte errores negativos en `-1` y `errno`. `LinuxOutputNativeTests` ejecuta diez casos originales en Linux nativo con archivos ordinarios como salida; los casos x64/ARM64 modelados también verifican presupuestos. Véase el [contrato de importación Linux](https://github.com/torvalds/linux/blob/v6.12/lib/iov_iter.c).

La política OS reutiliza las cabeceras de programa ya decodificadas por el cargador ELF. Comprueba etiquetas ABI, alineación de segmentos, tablas de cabeceras mapeadas y límites de direcciones de usuario. Un plan genérico valida extensiones, permisos, solapamientos y presupuesto antes de asignar memoria, y solo publica un espacio privado completamente preparado. Conserva prefijos/colas de páginas de archivo, pone BSS a cero, respeta permisos y reserva huecos de guarda para la pila. Rechaza diseños con páginas solapadas y cabeceras contradictorias; no adivina.

El PIE estático usa un load bias determinista de al menos `0x40000000`, aumentado para respetar la alineación mayor de `PT_LOAD`. Los segmentos, el PC de entrada y `AT_PHDR`/`AT_ENTRY` comparten ese bias; los valores originales de los encabezados no cambian y `AT_BASE` permanece en cero porque no hay intérprete. El mapeo usa bytes del archivo original, no los fixups de análisis; el propio invitado debe realizar sus relocations e inicialización. El loader decodifica `PT_DYNAMIC` desde registros acotados del archivo, sin depender de secciones. Si existe, la tabla debe ser legible, terminar correctamente y tener como máximo 4096 entradas. Se rechazan `PT_INTERP` y tags externos de dependencias/filter/audit; no se proporciona linker dinámico, resolución de símbolos ni ejecución de constructores.

La pila inicial contiene argc/argv/envp/auxv alineados, PHDR/PHENT/PHNUM, entry, tamaño de página e identidades. PID/TID/UID/GID modelados valen 1000. `AT_RANDOM` usa los primeros 16 bytes del SHA-256 de entrada para reproducibilidad; es política determinista del modelo, no entropía criptográfica. HWCAP/HWCAP2 son cero y no existe vDSO.

Se implementan `write`, `writev`, `exit`, `exit_group`, `getpid` y `gettid`, `mmap`, `mprotect`, `munmap`, `brk`, con números separados para [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) y [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h). El retorno de SYSCALL x64 aplica sus clobbers RCX/R11 además de RAX y el PC siguiente. ARM64 usa x8 para el número y x0 para el resultado. Las demás llamadas paran como `unsupported_service`; nunca ejecutan syscalls del host.

Las plantillas TLS estáticas `PT_TLS` se validan como hechos del loader: una plantilla, extensiones acotadas de archivo/memoria, alineación congruente y bytes iniciales legibles. El inicio invitado asigna e inicializa bloques TLS e instala el puntero de hilo; el modelo Linux no inventa un TCB/DTV específico de libc. Esto permite TLS local-exec generado por compilador en programas freestanding. TLS dinámico e hilos del SO siguen fuera del alcance.

En x64, `arch_prctl` admite `ARCH_SET_FS`, `ARCH_GET_FS`, `ARCH_SET_GS` y `ARCH_GET_GS`. Set acepta una base de rango usuario aunque no esté mapeada; toda desreferencia posterior verifica permisos. Las bases de rango kernel devuelven `EPERM` invitado y los destinos Get inválidos `EFAULT`, sin fault de CPU. Otras operaciones fallan explícitamente. ARM64 instala `TPIDR_EL0` con `MSR`; `MRS`, accesos FS/GS y restauración de contexto preservan el puntero entre quanta y entradas de backend. Esto no implementa un scheduler.

Los descriptores 1 y 2 son sumideros virtuales de bytes. `write` valida páginas de usuario legibles; devuelve el prefijo legible si una página posterior no es accesible y `EFAULT` si no puede leerse ningún byte. Un descriptor incorrecto da `EBADF`; una escritura de cero bytes sigue validando el rango de usuario, sin exigir una página mapeada ni leer datos. No se modelan la atomicidad de tuberías Linux ni archivos. El límite de salida detiene antes de publicar una escritura que lo excedería.

Los servicios de memoria anónima comparten el espacio del proceso y el presupuesto físico con la imagen y la pila. `mmap` acepta exactamente `MAP_PRIVATE | MAP_ANONYMOUS`, con `PROT_NONE`, `PROT_READ`, `PROT_READ | PROT_WRITE`, `PROT_READ | PROT_EXEC` o RWX legible. Respeta sugerencias libres alineadas a página; si no, busca huecos desde `0x100000000` y después desde la dirección mínima de usuario, reservando las guardas de pila. Esta colocación determinista no emula ASLR de Linux. Las páginas nuevas son independientes y se inicializan a cero; una retirada parcial puede recuperar páginas no fijadas. Una proyección CPU o vista de respaldo retenida puede mantener viva una asignación retirada hasta terminar su propia vida.

Las longitudes se redondean a páginas. `munmap` tolera huecos y retiradas repetidas; `mprotect` modifica el prefijo mapeado antes de devolver `ENOMEM` ante un hueco. `PROT_NONE` conserva la asignación y sus bytes, pero impide el acceso invitado. El `brk` bruto devuelve el límite solicitado si tiene éxito y el anterior si falla, no la convención cero/menos uno de libc. El límite inicial es el final de imagen alineado a página. El crecimiento respeta otros mapeos y el presupuesto; la reducción conserva los bytes de la página parcial restante. Las reglas y prioridades de error siguen los servicios Linux de [mapeo](https://github.com/torvalds/linux/blob/v6.8/mm/mmap.c) y [protección](https://github.com/torvalds/linux/blob/v6.8/mm/mprotect.c).

Los mapeos de archivos, compartidos o fijos, crecimiento descendente, páginas enormes, bloqueo, claves de protección, permisos solo de ejecución/escritura y otros indicadores siguen sin soporte explícito: se detienen antes de publicar efectos o inventar un retorno. Los errores normales de rango, longitud y alineación del subconjunto admitido devuelven errores invitados y permiten continuar. Ningún servicio reenvía punteros ni peticiones de mapeo al OS anfitrión.

La entrada opcional `linux_kernel` registra la ausencia observada explícitamente de interfaces del kernel. Por ejemplo, un fixture sin implementación de `pidfd_open` utiliza:

```json
{"linux_kernel":{"unavailable_syscalls":["pidfd_open"]}}
```

La llamada directa seleccionada devuelve -ENOSYS antes de validar argumentos, como una entrada de kernel ausente, sin crear descriptores ni modificar memoria invitada. El wrapper `syscall` de Bionic conserva su traducción habitual -1/errno. Omitir la entrada o dar una lista vacía mantiene el límite no soportado de esta interfaz; otras llamadas desconocidas no se convierten en ENOSYS. Actualmente solo se admite `pidfd_open`; se rechazan nombres desconocidos, duplicados y tipos incorrectos. La entrada no deduce versión del kernel, disponibilidad del host ni una implementación funcional de pidfd. Véase la [implementación de llamadas ausentes del kernel](https://github.com/torvalds/linux/blob/master/kernel/sys_ni.c).

Un `linux_kernel.gki` explícito elige una rama Android common publicada de 5.10 a 6.18. El subconjunto implementa `pidfd_open` para el proceso vivo del modelo y tareas invitadas declaradas, además de importar vectores según la versión. Comparte la tabla de `linux_files`; Bionic y traps brutos comparten propiedad y orden de errores. GKI es incompatible con observar `pidfd_open` ausente. El arreglo opcional `linux_kernel.tasks` declara un catálogo fijo y cerrado de tareas vivas adicionales, con entradas como `{ "id": 2000, "group_leader": true }`. Omitirlo deja sin soporte la búsqueda ajena; un arreglo vacío solo conoce al líder de grupo actual. Un PID fuera del catálogo declarado devuelve ESRCH. Catálogo y observaciones de prioridad deben coincidir; no se combinan con hilos cooperativos Android. El nivel API Android no elige el kernel. Véanse ocho revisiones, descriptores, pruebas y límites en los [contratos GKI publicados](../android-gki-kernels.md).

La entrada opcional `linux_priority` declara el estado nice de tareas de prueba con el UID del llamador. `setpriority` y `getpriority` sin envolver comparten ese estado entre Linux ELF64 y Android; no cambian prioridades del host.

```json
{"linux_priority":{"tasks":[{"id":1000,"nice":0}],
                   "cap_sys_nice":false,"rlimit_nice":0}}
```

Los identificadores son valores positivos distintos de 32 bits con signo; nice inicial está entre -20 y 19 y `rlimit_nice` entre 0 y 40. `cap_sys_nice` y `rlimit_nice` tienen valores predeterminados false y 0; el estado siempre es explícito. Una entrada ausente, tareas no declaradas —incluidos nuevos hilos sin estado— y PRIO_PGRP/PRIO_USER detienen el servicio como no compatible, sin inferir herencia ni propiedad. PRIO_PROCESS con who=0 selecciona la tarea actual del invitado; otro valor selecciona la indicada. Un selector inválido devuelve -EINVAL sin envolver. Las peticiones de cambio limitan nice, de 32 bits con signo, a -20..19. Reducir nice requiere CAP_SYS_NICE o suficiente RLIMIT_NICE; la denegación devuelve -EACCES sin modificar el estado. La consulta devuelve `20 - nice`, la codificación 40..1 del kernel, no el resultado traducido de libc.

[Linux setpriority/getpriority](https://man7.org/linux/man-pages/man2/setpriority.2.html).

`linux_signals` proporciona las acciones iniciales de señal para todo el proceso. Una entrada ausente es desconocida y no implica `SIG_DFL`; una lista vacía explícita permite instalar sin consultar la acción anterior. Los cinco campos son obligatorios; los valores sin signo de 64 bits fuera del intervalo exacto de JSON usan cadenas decimales.

```json
{"linux_signals":{"actions":[
  {"signal":11,"handler":0,"flags":0,"restorer":0,"mask":0}
]}}
```

Los números de señal van del 1 al 64. `rt_sigaction` y Bionic comparten el estado, con disposición de campos y orden de errores propios de cada interfaz. No se implementan señales pendientes, entrega, ejecución de manejadores ni máscaras por hilo, y no se usan manejadores del anfitrión. Véase el [contrato completo](../process-emulation.md#linux-profile-semantics).

<a id="windows-pe64-profile"></a>

`linux_kernel.gki` selecciona 5.10–6.18 para `pidfd_open` e importación vectorial; el nivel API Android no elige kernel. `linux_kernel.tasks` declara tareas fijas vivas como `{ "id": 2000, "group_leader": true }`. Omitido: objetivos externos no admitidos; vacío: solo líder actual; fuera del catálogo: ESRCH. Prioridades coherentes y ausencia de threads Android cooperativos son requisitos. La tabla se comparte con `linux_files`; GKI y pidfd declarado ausente son incompatibles. Véase el [contrato completo](android-gki-kernels.md).

El subconjunto GKI también incluye relojes CPU de proceso observados y pidfd `ppoll` con tiempo cero. Este exige una timespec cero explícita, máscara temporal nula y límite de `linux_files`. Solo escribe `revents` ordenados, sin reescribir el tiempo ni avanzar el reloj de pared. Un descriptor cerrado produce POLLNVAL; un pidfd vivo no está disponible. Las esperas bloqueantes y la disponibilidad de otros tipos siguen sin admitirse.

<!-- i18n-section: linux-clocks -->

## Relojes explícitos del invitado

La opción `linux_time` proporciona valores fijos a las llamadas de Linux y a Android Bionic. El modelo no lee el reloj del anfitrión, no avanza el tiempo al ejecutar instrucciones ni supone una época predeterminada.

```json
{"linux_time":{"clocks":[
  {"id":0,"seconds":"4294967297","nanoseconds":987654321},
  {"id":1,"seconds":123,"nanoseconds":456789}],
  "timezone":{"minutes_west":-60,"dst_time":0}}}
```

Se admiten ID estáticos 0–9 y 11 y los ID CPU negativos del GKI elegido. Identidades duplicadas, ID desconocidos y más de 16384 muestras se rechazan. Los segundos son enteros con signo de 64 bits, no negativos para CPU; los nanosegundos están en `[0, 1000000000)`. Los enteros JSON están en `±9007199254740991`, las cadenas decimales conservan todo el rango; la zona horaria usa 32 bits con signo. C++ usa `ProcessOptions::LinuxTime`; otros perfiles OS rechazan la opción.

`-16006` es SCHED del PID 2000. PROF y VIRT son independientes. Los ID actuales SCHED 2, -6 (PID cero) y -8006 (PID 1000) comparten muestra; los alias PROF son -8/-8008 y VIRT -7/-8007. Se rechazan alias duplicados incluso con valores iguales. Los segundos CPU son no negativos y los nanosegundos normalizados. El avance inactivo solo cambia los relojes de pared 0, 1 y 7; las muestras CPU quedan fijas. La ejecución no deduce consumo CPU.

`advance_on_idle: true` activa `nanosleep` relativo x64/ARM64 y los wrappers Android nominales/variadic. Los relojes de pared 0, 1 y 7 pueden coexistir con muestras CPU fijas; otros tipos se excluyen de esta política. Los hilos ejecutables avanzan primero; cuando todos se bloquean, el tiempo llega al primer plazo. Un único hilo avanza directamente. Los relojes dinámicos/FD/de hilo codificados siguen sin soporte.

El TID propio de la tarea actual también identifica su grupo, incluidos los hilos cooperativos Android sin catálogo externo. Un PID externo ausente del catálogo cerrado o vivo sin ser líder devuelve `EINVAL` antes de acceder al destino. Omitir el catálogo o la muestra de un grupo conocido deja la operación sin soporte antes de copiar. Un tipo inválido devuelve `EINVAL`; una muestra válida puede producir `EFAULT` en la copia de usuario. Los traps conservan errores negativos; solo Bionic actualiza errno y devuelve -1. [GKI](android-gki-kernels.md).

<a id="explicit-memory-files"></a>

<!-- i18n-section: linux-files -->

## Archivos explícitos en memoria

`linux_files` proporciona a Linux ELF64 y Android un catálogo cerrado de archivos inmutables. El arreglo obligatorio `files` puede estar vacío; cada entrada requiere la ruta absoluta canónica `path` y los bytes `bytes_hex`. No se consultan archivos del host ni contenido `/proc` implícito. Sin la opción los servicios no están modelados; las rutas ausentes devuelven `ENOENT`.

```json
{"linux_files":{"files":[
  {"path":"/fixture/data","bytes_hex":"00ff410a805a"}],
  "descriptor_limit":256}}
```

Cada apertura tiene su cursor; Bionic, `syscall`, trampas e hilos invitados comparten los descriptores. Cerrar permite reutilizar el menor número libre. Se admiten `open/openat` de solo lectura, `read/close`, `lseek` ordinario, `O_CLOEXEC` y el `O_LARGEFILE` de la arquitectura. Solo Bionic convierte errno. Los fallos de lectura conservan el prefijo copiado. stdin no tiene contenido predeterminado; rutas relativas, directorios, escrituras y enlaces siguen excluidos.

C++: `ProcessOptions::LinuxFiles`. `descriptor_limit`: 3–4096 (256); `files` ≤ 256; `path` < 4096 bytes; component ≤ 255 bytes; data + paths + NUL ≤ 16 MiB; JSON ≤ 64 KiB. [Contract](../process-emulation.md#explicit-memory-files).

Una lectura de longitud cero puede comenzar en el límite del espacio de usuario. Tras validar el rango de direcciones original, si la posición del archivo más la longitud solicitada supera `INT64_MAX`, se devuelve `EINVAL` incluso en EOF, sin modificar el cursor.

Cada entrada puede incluir `metadata` completos; C++ usa `LinuxFileOptions::Metadata` con rutas ya presentes. `fstat`/`fstat64` y syscall comparten observaciones fijas, sin consultar el host, derivar size de los bytes ni mover el cursor. Solo se admiten archivos regulares y campos completos; los enteros de ancho completo usan cadenas decimales. x64/AArch64 escriben 144/128 bytes con rdev y relleno en cero. Un descriptor inválido devuelve `EBADF`; una salida totalmente inaccesible, `EFAULT`. Metadatos ausentes, flujos estándar desconocidos o permisos de escritura mixtos detienen la ejecución sin cambiar bytes. Los campos y límites figuran en el contrato enlazado.

```json
{"linux_files":{"files":[{"path":"/fixture/virtual","bytes_hex":"616263",
  "metadata":{"device":1,"inode":"18446744073709551615","mode":33060,
    "link_count":1,"uid":1000,"gid":1000,"size":0,"block_size":4096,"blocks":0,
    "access_time":{"seconds":0,"nanoseconds":0},
    "modification_time":{"seconds":0,"nanoseconds":0},
    "change_time":{"seconds":0,"nanoseconds":0}}}]}}
```

<!-- i18n-section: windows-pe64 -->

## Perfil Windows PE64

`windows-pe64-v1` admite procesos de consola Windows x64/ARM64 acotados con PEB/TEB, TLS estático y dinámico, `DllMain`, API Win32 con nombre y grafos DLL explícitos sin ciclos. Los módulos admiten código/datos por nombre u ordinal, DIR64, exportaciones reenviadas e identidades reales del cargador. `LoadLibraryA` / `LoadLibraryW`, `FreeLibrary` y `GetProcAddress` usan el catálogo configurado. CRT/GUI, SEH de usuario ARM64 basado en marcos, hilos y compatibilidad general de Windows siguen pendientes; falta evidencia nativa ARM64 KVM/WHP.

`WindowsProcessTime.cpp` gestiona `GetSystemTimeAsFileTime`, `GetTickCount`, `QueryPerformanceCounter`, `QueryPerformanceFrequency` y `ZwDelayExecution` mediante relojes del host. FILETIME cuenta unidades de 100 ns desde 1601; el contador de rendimiento usa un reloj monótono y su frecuencia declarada de 10 MHz, mientras los ticks en milisegundos desbordan a 32 bits. Se completan los retrasos relativos no alertables de hasta 500 ms y el intervalo cero; las esperas alertables, las fechas absolutas positivas y los retrasos mayores detienen explícitamente la ejecución, sin acortar la espera ni devolver éxito. Los servicios conservan LastError y comparten el modelo entre backends CPU. `ZwDelayExecution` solo lee los 8 bits bajos de `BOOLEAN`; los bits no usados del registro de argumento no cambian la política de espera.

[Windows x64 ABI](https://learn.microsoft.com/cpp/build/x64-calling-convention), [QueryPerformanceFrequency](https://learn.microsoft.com/windows/win32/api/profileapi/nf-profileapi-queryperformancefrequency), [FILETIME](https://learn.microsoft.com/windows/win32/api/minwinbase/ns-minwinbase-filetime).

La memoria virtual de Windows incorpora `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` y `FlushInstructionCache` para el proceso actual. La capa OS administra las reservas; `AddressSpace` mantiene la autoridad sobre páginas confirmadas, permisos y almacenamiento. Las pruebas cubren cambios de código, fallos de acceso y reutilización del presupuesto de memoria.

`WriteProcessMemory` sigue el comportamiento de páginas confirmadas observado en x64/ARM64 para escrituras de hasta 4 KiB en el proceso actual. Conserva la protección de cada región, los prefijos copiados, los recuentos de bytes y LastError, incluidos `ERROR_NOACCESS`, `ERROR_PARTIAL_COPY` y el éxito tras un prefijo RX. `WindowsMemoryWriteTests.cpp` comprueba las 25 parejas de protecciones; `check_windows_memory_write.py` verifica el mismo ejecutable original en CI de Windows nativo. Los destinos sin confirmar siguen explícitamente sin soporte.

Las asignaciones privadas admiten `MEM_RESERVE`, `MEM_COMMIT`, `MEM_DECOMMIT`, `MEM_RELEASE` y `MEM_TOP_DOWN`, con reservas alineadas a 64 KiB y páginas de 4 KiB. Reservar no consume RAM del invitado. Volver a confirmar conserva los bytes y actualiza permisos; desconfirmar devuelve cada página. La validación completa del rango y la preparación de asignaciones evitan cambios parciales ante errores ordinarios. La consulta devuelve la estructura x64/ARM64 de 48 bytes y agrupa páginas posteriores de una misma asignación. Imagen, entorno, área de heap, entradas API y márgenes de pila intervienen en la ubicación; la identidad de la pila coincide con el TEB. Si un `VirtualProtect` correcto vuelve de solo lectura la ubicación de salida de los permisos anteriores, los nuevos permisos siguen aplicados, los bytes de salida no cambian y la llamada devuelve éxito. Un cambio de protección sobre páginas sin confirmar devuelve `ERROR_INVALID_ADDRESS`, escribe `PAGE_NOACCESS` en la salida de los permisos anteriores y conserva los permisos de las páginas.

Las protecciones admitidas son `PAGE_NOACCESS`, `PAGE_READONLY`, `PAGE_READWRITE`, `PAGE_EXECUTE_READ` y `PAGE_EXECUTE_READWRITE`. Las páginas de guarda, solo ejecución, copia al escribir, modificadores de caché, páginas grandes, reset/write-watch/marcadores y cambios en mapeos internos del modelo siguen sin soporte explícito. Solo se pueden desconfirmar o liberar asignaciones virtuales privadas.

[VirtualAlloc](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc), [VirtualFree](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualfree), [VirtualProtect](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect), [VirtualQuery](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualquery), [MEMORY_BASIC_INFORMATION](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-memory_basic_information).

```bash
neverd emulate guest.exe --profile=windows-pe64-v1 \
  --options='{"backend":"auto","arguments":["guest.exe","argument"],"environment":["MODE=test"]}'
```

El EXE PE32+ de un solo hilo conserva su base preferida y admite DLL explícitas con entrada opcional y TLS estático. `WindowsProcessOptions::Modules` o JSON `windows.modules` aporta hasta 64 nombres base invitados y rutas de entrada mediante `name` y `path`, sin buscar ni ejecutar DLL del host. Los nombres ASCII ignoran mayúsculas; se rechazan duplicados y sustituciones de proveedores del sistema. Solo se leen archivos alcanzables. Las importaciones nominales/ordinales enlazan exportaciones reales; huecos, símbolos ausentes, ciclos, importaciones enlazadas/diferidas y load configuration/CFG no admitido fallan. DIR64 mueve DLL reubicables en conflicto; colisiones fijas y escrituras en metadatos de enlace fallan antes de publicar la imagen afectada.

`readPEProgramExports` posee exportaciones originales y rangos leídos; `WindowsProcessModules` posee el grafo y puertas API por proveedor/nombre comunes al proceso. `VirtualMemory` reserva imágenes antes de mapear; `AddressSpace` controla páginas y permisos. PEB/LDR contiene imágenes reales y la lista de inicialización conserva el orden de registro del cargador. Este se mantiene separado del orden de llamadas attach según las dependencias. `GetModuleHandleW` acepta NULL o nombres base ASCII, ignora mayúsculas y añade `.dll` sin extensión. Rutas, nombres no ASCII y punto final no están admitidos. Un nombre ausente devuelve 126; el éxito conserva LastError. Los modelos API no son DLL instaladas.

Los bytes de entrada y las extensiones acumuladas de imagen comparten cada uno `memory_limit`; los mapas del entorno también consumen el presupuesto de imagen. La preparación comparte 65,536 registros, 64 MiB de lecturas, nombres acotados y el plazo total, sin garantía temporal estricta de E/S del host. El fixture original EXE→DLL→DLL comprueba reubicaciones, ordinales, datos, identidad API, `MEM_IMAGE`, listas y attach/detach TLS del EXE. `NeverDWindowsProcessTests` incluye el oráculo Windows nativo; `NeverDPEProgramExportsTests`, metadatos inválidos y presupuestos; `NeverDProcessPublicTests`, paridad C ABI/CLI. Los transportes no disponibles se omiten explícitamente.

`WindowsProcessLifetime` ejecuta TLS y después `DllMain` de las DLL en orden de dependencia, seguido de TLS y entrada EXE, con una CPU y presupuesto comunes. Cada módulo recibe índice TLS y bloque alineado independientes, copiados de la imagen reubicada y enlazada en un área de 64 KiB. El argumento reservado TLS es cero; `DllMain` recibe un valor opaco no nulo al iniciar/terminar el proceso. La salida explícita separa las DLL inicializadas en orden inverso de la lista del cargador y después TLS EXE, incluso antes de inicializar el EXE. `DllMain(FALSE)` de inicio termina con `0xc0000142` sin detach. Fallos y presupuesto agotado no inventan limpieza. El retorno de entrada PE con DLL invitadas requiere terminación de hilo no soportada y se detiene explícitamente. `SizeOfZeroFill` no nulo sigue excluido; se admiten los bytes inicializados a cero de la plantilla TLS real. Las DLL sin entrada reciben TLS attach, pero no notificaciones de detach del proceso.

`WindowsProcessExports` comparte la resolución por nombre/ordinal entre importaciones estáticas y `GetProcAddress`, incluidos código, datos, alias y cadenas de reenvío. Solo los reenvíos iniciales utilizados añaden módulos del catálogo y dependencias de inicialización; los demás no cargan archivos. Los nombres distinguen mayúsculas; un nombre ausente devuelve NULL/error 127, un ordinal consultado directamente que falta (incluidos huecos) NULL/error 182 y un argumento de consulta NULL error 87, y el éxito conserva LastError. Los handles desconocidos siguen sin admitirse. Las entradas API exactas proveedor/nombre se reservan una vez desde el registro acotado. Se verifican las cabeceras PE y los metadatos de exportación actuales de cada imagen, rechazando cambios o bytes ilegibles; las cadenas se limitan a 64 entradas y comparten el presupuesto restante de metadatos y el plazo de ejecución. Un reenvío a un hueco devuelve la base de la imagen destino y conserva LastError; al ordinal cero devuelve el error 87. La base es una dirección de datos y no concede permiso para ejecutar las cabeceras. Los reenvíos en ejecución pueden cargar módulos configurados e inicializarlos antes de devolver el resultado. Sigue sin admitirse modificar la tabla de exportación activa.

`WindowsProcessLoader` carga nombres base DLL ASCII de `windows.modules` y gestiona referencias explícitas, dependencias compartidas y retención inicial. Repetir consultas reenviadas no añade referencias. Cada recarga asigna una generación residente nueva al mismo espacio del catálogo. TLS y `DllMain` usan la misma CPU por debajo de las tramas API suspendidas; restaurar registros conserva escrituras invitadas y usa el retorno actual. Los punteros reservados del attach/detach dinámico son cero. Un attach fallido durante una carga explícita devuelve 1114 después de limpiar, conservando las cargas anidadas independientes que tuvieron éxito. Descargar libera imagen y TLS; recargar restaura los bytes originales. Se rechazan cambios externos en listas del cargador o punteros TLS. Los presupuestos de archivos, imágenes y metadatos son acumulativos incluso tras fallos. Los proveedores del sistema usan la base PE mapeada como handle. Búsqueda de archivos, rutas no ASCII, opciones `LoadLibraryEx`, ciclos y transiciones reentrantes del mismo módulo mientras se inicializa o descarga siguen sin admitirse.

Una DLL como entrada usa `WindowsLibraryHost.cpp`: un EXE PE modelado ejecuta `LoadLibraryA` → `FreeLibrary` → `ExitProcess` en la misma CPU invitada. La entrada ocupa una de las 64 posiciones del catálogo, mantiene su nombre base y comparte presupuestos de preparación y ejecución con el anfitrión y dependencias. `GetModuleHandle(NULL)` identifica al anfitrión; `GetModuleFileNameA` acepta handles de módulos residentes. Abrir su propio archivo sigue limitado a la entrada explícita. Los fallos de asociación siguen el error 1114 y la limpieza ordinarios; no se adivinan firmas de exportación.

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` comparten el bloque actual del entorno invitado en los parámetros del proceso del PEB. Los nombres ASCII no distinguen mayúsculas; los valores son UTF-16. Las modificaciones validan entradas, capacidad y permisos de escritura antes de publicarse. Las instantáneas son independientes de los cambios posteriores y liberan su memoria invitada. El modelo limita el bloque a 64 KiB; cadenas y expansiones están acotadas y comprueban el plazo. Siguen sin admitirse punteros de propiedad desconocida, bloques malformados, páginas de códigos ANSI y búferes de expansión superpuestos. `WindowsEnvironmentTests.cpp` compara fixtures originales x64/ARM64 en los backends disponibles; CI exige un oráculo Windows nativo independiente.

`WindowsProcessHeap` unifica asignación, [`HeapReAlloc`](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heaprealloc), liberación y consulta del tamaño del heap del proceso. El cambio de tamaño conserva los bytes retenidos; `HEAP_ZERO_MEMORY` pone a cero los bytes añadidos y `HEAP_REALLOC_IN_PLACE_ONLY` impide mover el bloque. Un cambio de tamaño fallido conserva el bloque y devuelve NULL con `ERROR_NOT_ENOUGH_MEMORY` (8), como en las observaciones nativas. Las páginas independientes devuelven capacidad al reducir o liberar; el crecimiento preparado y las copias acotadas comprueban el plazo. Heaps personalizados, indicadores de excepciones, propiedad desconocida y rangos inaccesibles detienen la ejecución explícitamente. `WindowsHeapTests.cpp` cubre ambas ISA, movimiento forzado, reutilización del presupuesto y fallos atómicos; CI ejecuta el mismo EXE original en Windows nativo.

`WindowsSystemModules` construye imágenes modelo PE64 acotadas de `ntdll.dll`, `kernelbase.dll` y `kernel32.dll` para ambas ISA. Las consultas ASCII `GetModuleHandleA` / [`GetModuleHandleW`](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulehandlew), `LoadLibraryA` / `LoadLibraryW` y [`GetProcAddress`](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress) comparten sus bases mapeadas; PEB/LDR y `MEM_IMAGE` describen esas mismas imágenes. Importaciones estáticas, búsquedas por nombre y reenvíos invitados usan los mismos puntos de entrada API y resolución de exportaciones. Los proveedores permanecen residentes, sin callbacks invitados de inicialización, y no impiden retornar desde la entrada tras descargar las DLL invitadas ordinarias. Cambiar cabeceras o metadatos de exportación detiene la búsqueda. Los nombres de sistema no modelados y ordinales no nulos se rechazan explícitamente; diferencias solo de mayúsculas en nombres modelados y nombres vacíos devuelven 127, una consulta NULL devuelve 87. Los bytes y direcciones generados son política del modelo; no se reconstruyen diseños por versión de Windows, ordinales nativos ni alias entre proveedores. `WindowsSystemTests.cpp` compara EXE originales x64/ARM64 con Windows nativo e incluye ocho observaciones independientes del retorno del hilo inicial.

`WindowsProcessExceptions` implementa `AddVectoredExceptionHandler`, `RemoveVectoredExceptionHandler` y `RaiseException` en la misma CPU y presupuesto del proceso. Los manejadores ordenados pueden modificar registros, generar excepciones anidadas, llamar API modeladas, cargar DLL y terminar el proceso. Las infracciones de datos x64/ARM64 y divisiones enteras x64 reanudan tras validar cambios del invitado en `CONTEXT`, conservando registros generales, SIMD y estado FP admitido. Las excepciones de software continúan mediante una instrucción real de retorno del proveedor modelado. Límites: 128 registros retenidos y 16 marcos anidados. Resultados inválidos, punteros modificados, campos no admitidos y excesos fallan explícitamente. SEH/desenrollado ARM64 basado en pila, depuración y fallos de ejecución/guarda siguen sin soporte. `WindowsExceptionTests.cpp` compara EXE/DLL originales con Windows nativo; aún faltan pruebas nativas ARM64 KVM/WHP. [AddVectoredExceptionHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-addvectoredexceptionhandler), [RemoveVectoredExceptionHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-removevectoredexceptionhandler), [RaiseException](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-raiseexception), [CONTEXT x64](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-context), [ARM64_NT_CONTEXT](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-arm64_nt_context). Los registros de excepciones de software incluyen `EXCEPTION_SOFTWARE_ORIGINATE` (`0x80`), independientemente de la marca de no continuación del llamador; el ejecutable original de Windows comprueba los valores exactos de las marcas de las excepciones de software y hardware. [EXCEPTION_RECORD](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_record).

`WindowsProcessContext` conserva el origen de cada marco de despacho. Los fallos x64 admitidos de acceso a datos y división exponen RF (`0x10000`) en `CONTEXT.EFlags`; `RaiseException`, incluso con códigos de violación de acceso por software, conserva el contexto actual. El origen se mantiene durante VEH/VCH y la búsqueda/desenrollado SEH. Una continuación válida restaura las banderas lógicas de CPU sin RF; los cambios de RF del invitado se rechazan antes de publicar el estado. Este perfil limitado no modela puntos de interrupción de instrucciones ni RF controlado por el invitado. `WindowsExceptionTests.cpp` comprueba registros, restauración y CPU/RAM intactas tras un rechazo.

Según observaciones nativas independientes, Windows ring3 convierte los fallos checked x64 `operand_alignment` en `STATUS_ACCESS_VIOLATION` con parámetros `[read, UINT64_MAX]`, también para escrituras. La capa CPU proporciona la causa; Windows no la adivina a partir del vector 13 ni vuelve a decodificar la instrucción. `WindowsAlignmentProcessTests.cpp` ejecuta instrucciones PE originales en 72 escenarios de fallo y 9 reintentos tras corregir la dirección (`72 + 9`), comprobando PC, RF, XMM y RAM. Los fallos sin clasificar o incoherentes se rechazan. Los informes de procesos y controladores conservan `cause` y `error_code` hexadecimal, ambos anulables, distinguiendo ausencia y cero. Esta entrega se aplica al perfil de usuario checked x64.

`AddVectoredContinueHandler` y `RemoveVectoredContinueHandler` mantienen una lista ordenada independiente y comparten con los manejadores de excepción el límite de 128 registros retenidos. Cuando un manejador vectorizado acepta continuar, los callbacks de continuación reciben el mismo registro modificable y `CONTEXT`. La validación final ocurre después de estos callbacks, incluidas las excepciones anidadas y las notificaciones DLL. No se pueden retirar identificadores mediante la otra familia de manejadores. `WindowsContinuationTests.cpp` compara EXE originales con Windows nativo para orden, terminación anticipada, cambios de registro, reparación del contexto, anidamiento, callbacks del cargador y salida del proceso. La ruta vectorizada probada en Windows x64 permite continuar con `EXCEPTION_NONCONTINUABLE`; esto no demuestra el comportamiento de SEH basado en pila. La ejecución ARM64 nativa sigue sin verificar. [AddVectoredContinueHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-addvectoredcontinuehandler), [RemoveVectoredContinueHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-removevectoredcontinuehandler).

`RtlCaptureContext` está disponible mediante `kernel32.dll` y `ntdll.dll` para x64 y ARM64. Los componentes compartidos `WindowsProcessContext` e `IntegerABI` guardan PC/SP del llamador sin cambiar el estado de CPU ni LastError. Las observaciones nativas de Windows confirman los indicadores x64 `0x10000f`, la conservación de áreas home/depuración/vectores no escritas y los campos históricos de direcciones x87 de 32 bits; ARM64 copia LR a PC y pone a cero X0/LR en el registro. Registros, SIMD y controles flotantes proceden del invitado; los selectores x64 y la máscara de capacidades MXCSR siguen la CPU invitada configurada. Los destinos inválidos, desalineados o parcialmente inaccesibles fallan antes de publicar datos. `WindowsContextTests.cpp` cubre importaciones directas, consultas a proveedores, callbacks VEH, salidas entre páginas y atomicidad de los fallos. `scripts/check_windows_context.py` ejecuta el programa original en Windows x64/ARM64, con un oráculo nativo separado para el estado x87 no vacío. Estas observaciones ARM64 no prueban ejecución nativa KVM/WHP. Restauración de contexto, recorrido de pila y tablas dinámicas de funciones siguen pendientes. `WindowsProcessServices.def` declara restricciones exactas por módulo: la búsqueda en `kernelbase.dll` devuelve `ERROR_PROC_NOT_FOUND` (127), según las observaciones nativas, sin inventar una exportación. [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

`WindowsProcessSEH` usa el `X64SEH` compartido de `os/windows/exception/` (`NeverDEmulationWindowsException`, disponible sin controladores) para x64 `__C_specific_handler` y UNWIND_INFO V1. Tras buscar VEH, admite filtros, finally, transferencia no local, despacho anidado/desenrollado en colisión y marcos EXE/DLL reubicados, preservando GPR/XMM no volátiles. La continuación por filtro ejecuta VCH con el mismo `CONTEXT`. `WindowsSEHTests.cpp` compara 23 escenarios originales con Windows nativo; KVM/WHP/Unicorn comparten la semántica. El presupuesto del proceso cubre la revalidación de generaciones, cabeceras, bytes de desenrollado/ámbitos, regiones del manejador de lenguaje y enlaces IAT. Los metadatos cambiados o imágenes retenidas descargadas fallan explícitamente. SEH ARM64 por marcos, C++ EH, tablas dinámicas, RtlUnwind/NtContinue generales y desenrollado a través de callbacks del cargador/VEH/VCH siguen sin soporte.

Para `EXCEPTION_NONCONTINUABLE`, un filtro x64 que devuelve `EXCEPTION_CONTINUE_EXECUTION` genera `STATUS_NONCONTINUABLE_EXCEPTION` (`0xc0000025`, indicadores `0x81`, registro enlazado nulo) con un contexto nuevo. VEH se ejecuta de nuevo antes de buscar en la pila lógica conservada, manteniendo el orden finally, la identidad de los marcos EXE/DLL y los mismos presupuestos de profundidad y ejecución. Los 23 escenarios nativos incluyen 21 ejecuciones correctas y dos terminaciones: aceptar en VEH/VCH la continuación de esta excepción secundaria la deja sin gestionar incluso tras restaurar el `CONTEXT` original. El modelo informa de un fallo de ejecución. Las direcciones de excepciones de software coinciden con el PC guardado; las direcciones del despachador interno y la disposición de registros son decisiones del modelo. [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

Antes de los callbacks de descarga dinámica, el módulo sale de la lista de inicialización; su mapeo, búsqueda por nombre y pertenencia a las listas de carga/memoria siguen disponibles durante los callbacks. Los oráculos de retorno de entrada observan el hilo inicial independientemente de los hilos de trabajo del sistema.

`WindowsDynamicTests.cpp` compara DLL/EXE originales x64/ARM64 con observaciones independientes de Windows nativo: referencias, dependencias compartidas, cargas anidadas, limpieza tras fallos, reenvíos, salida, DLL sin entrada y TLS nuevo al recargar. Otras regresiones rechazan metadatos alterados y punteros de código caducados, mantienen presupuestos acumulativos y dejan incompletos los resultados API interrumpidos. La CI Windows exige el oráculo nativo y casos WHP; compilar para ARM64 o usar Unicorn no prueba ejecución nativa ARM64.

Una biblioteca ausente en cualquier punto de la cadena de reenvío de `GetProcAddress` devuelve 127; un `LoadLibrary` explícito de un módulo ausente del catálogo devuelve 126. El oráculo nativo y cada backend disponible verifican los 41 escenarios declarados. En Windows, el retorno tras descargar todas las DLL se observa 16 veces por variante de DLL. La inicialización fallida mediante un reenvío de `GetProcAddress` también devuelve 127 tras limpiar. Los callbacks de separación del proceso conservan el contenido de la pila del llamador que termina.

`WindowsExportTests.cpp` usa DLL y EXE originales x64/ARM64 para verificar llamadas reenviadas de código/datos/ordinales, alias, consultas durante inicialización, reubicación, mayúsculas, ausencias, LastError, ciclos, destinos no residentes, punteros inválidos y cambios tras consultas correctas. El mismo EXE tiene un oráculo Windows nativo independiente; los casos WHP son obligatorios en CI nativa. Las pruebas C ABI/CLI comparan informes completos. Sigue pendiente la evidencia de hardware ARM64 nativo. Las variantes EXE con y sin tabla de exportación cubren ambos grafos, el orden PEB y detach, y los errores de nombre/ordinal/NULL.

`WindowsLifetimeTests.cpp` compara trazas fijas con procesos Windows nativos independientes y KVM/WHP/Unicorn: salida normal, retorno de entrada, ambos fallos DLL, cuatro salidas tempranas y DLL sin entrada. También verifica fallos de callbacks, presupuestos comunes, campos TLS reubicados y capacidad total. La prueba nativa de retorno conserva el identificador del hilo inicial y verifica 64 veces su código de salida y la secuencia exacta de notificaciones de hilo/proceso. Los hilos hijos restantes se terminan tras la observación; la salida del proceso no se interpreta como retorno de entrada.

```json
{"windows":{"modules":[{"name":"middle.dll","path":"inputs/middle.dll"},{"name":"leaf.dll","path":"inputs/leaf.dll"}]}}
```

GS en x64 y x18 en ARM64 apuntan a TEB: límites de pila, puntero propio, PID/TID, PEB, parámetros, LastError y TLS. Convierte UTF-8 estricto a UTF-16 y entrecomilla argv según Microsoft CRT. Los nombres de entorno son ASCII, se rechazan duplicados sin distinguir mayúsculas, los valores pueden ser Unicode y el bloque ordenado acaba con dos NUL. No hereda entorno ni archivos del host. TLS estático copia plantilla, pone BSS a cero y escribe un índice de 32 bits; TLS dinámico usa otras ranuras TEB. Attach/detach lee la tabla viva en orden con presupuesto y plazo compartidos. La salida normal del proceso ejecuta detach. El retorno de entrada solo se admite sin DLL invitadas residentes; un segundo `ExitProcess` durante la limpieza de salida sigue sin admitirse.

`WindowsProcessServices.def` define `ExitProcess`, `RtlExitUserProcess`, handles de salida y `WriteFile` síncrono, LastError, ID y pseudohandles de proceso/hilo, `GetCommandLineW` / `HeapReAlloc`, asignación/liberación/tamaño del heap, TLS dinámico y `LoadLibraryA` / `LoadLibraryW` / `FreeLibrary` / `GetModuleHandleA` / `GetModuleHandleW` / `GetProcAddress`. Solo resuelve nombres exactos de `kernel32.dll`, `kernelbase.dll` y `ntdll.dll`. Syscalls directos y puertas falsas no seleccionan modelos. El heap pertenece al proceso y se recupera; la salida conserva bytes binarios. Los errores Win32 se separan de E/S asíncrona y excepciones de usuario no implementadas. Los alias respetan el cero inicial del contador y la dirección de retorno real.

`windows.native_calls` conserva módulo/función, argumentos escalares declarados y resultados anulables, sin inventar números NT. `NeverDWindowsProcessTests` comprueba PE reales, TLS del compilador, cambios de callbacks, heap, alias, metadatos inválidos, privilegios y presupuestos; `NeverDProcessPublicTests` verifica CLI/C ABI. CI de Windows ejecuta el mismo EXE directamente como oráculo independiente y exige pruebas WHP. La evidencia ARM64 nativa aún requiere una máquina adecuada.

Si el búfer de entrada no está vacío y no es legible, `WriteFile` devuelve `ERROR_INVALID_USER_BUFFER` (1784), pone a cero el número de bytes escritos y no produce salida.

`windows.defer_unmodeled` carga una imagen cuyos hechos de carga el modelo no implementa y solo se detiene si la ejecución depende de alguno de ellos. Las exportaciones fuera del inventario de API y los módulos fuera del catálogo se enlazan a entradas opacas: cada identidad se resuelve en una única dirección, y ejecutarla detiene la ejecución como `unsupported_service` nombrando `module!export`. Los directorios que el modelo no interpreta quedan sin interpretar, los metadatos que el archivo no respalda no se leen en la carga, y el despacho de excepciones basado en marcos a través de una imagen así se detiene. `observeProcess` añade un `ProcessObserver` que lee el proceso detenido en su inicio y en cada vigilancia de ejecución; no puede cambiar el estado del invitado, y cuando finaliza la ejecución se informa `observer`. El [desempaquetado](unpack.md) se apoya en ambos.

[PE/COFF](https://learn.microsoft.com/windows/win32/debug/pe-format), [ARM64 ABI](https://learn.microsoft.com/cpp/build/arm64-windows-abi-conventions), [WriteFile](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-writefile), [TLS](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue), [Wine 10.0 loader](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/ntdll/loader.c). [GetProcAddress](https://learn.microsoft.com/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress).

<!-- i18n-section: verification -->

## Verificación

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# En una compilación con biblioteca compartida/CLI:
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

Las pruebas compilan entradas ELF autónomas en ensamblador y C para ambas ISA: data/BSS, inicio real, errores de llamadas, salida binaria, permisos, escrituras parciales, servicios no admitidos y presupuestos entre cuantos. TLS inicializa bloques alineados independientes, BSS y punteros de hilo y comprueba su conservación; x64 verifica errores de `arch_prctl` sin perder la base anterior. Los backends no disponibles se omiten explícitamente. La suite pública compara informes y códigos de salida mediante la ABI C compartida y la CLI. PIE verifica auxv y slots RELA originalmente nulos antes de sus propias reubicaciones de datos y funciones. Las pruebas de mapeo conservan fixups cuando se elige la fuente de análisis; las tablas dinámicas cubren ausencia de secciones y entradas malformadas o dependientes. Ambas ISA ejercitan asignación, protección, huecos, remapeo, crecimiento/reducción del heap y errores tratados. Escrituras invitadas reales fallan tras cambios de protección ordinarios y parciales. x64 reescribe código en la misma dirección entre RW y RX y llama ambas versiones; el mismo ELF se ejecuta nativamente en Linux como oráculo independiente. Las pruebas de memoria cubren agotamiento, recuperación e instantáneas autoritativas sin retener RAM. La compilación cruzada y Unicorn ARM64 no son evidencia de ARM64 KVM/WHP nativo.
