# Contratos de los kernels Android GKI publicados

NeverD prioriza las ramas publicadas Android GKI 5.10–6.18 antes de otras variantes Linux. La solicitud selecciona expresamente una rama:

```json
{"linux_kernel":{"gki":"android17-6.18"},"linux_files":{"files":[],"descriptor_limit":16}}
```

El contrato API 28 del perfil Android nativo describe las importaciones Bionic, sin elegir el núcleo. GKI controla los contratos implementados de `pidfd_open`, salida vectorial, relojes CPU de proceso codificados y `ppoll` de descriptores de proceso con tiempo cero. No certifica ni inicia un núcleo completo ni deduce dispositivos, espacios de nombres, permisos o procesos. Los servicios no admitidos se detienen explícitamente. Véase la [política GKI oficial](https://source.android.com/docs/core/architecture/kernel/gki-releases).

## Fuentes fijadas

`LinuxGKIKernels.def` sigue estas etiquetas oficiales `r1`, comprobadas el 2026-10-07. Los commits inmutables proporcionan `kernel/pid.c`, `include/uapi/linux/pidfd.h`, `arch/arm64/configs/gki_defconfig`, `kernel/fork.c`, `lib/iov_iter.c` y `fs/read_write.c`. Los flags proceden de UAPI y de la validación de llamadas, no del nivel API Android ni del kernel anfitrión. Las fuentes fijadas de los relojes CPU aparecen en la tabla siguiente.

| Rama solicitada | Etiqueta publicada | Commit fijado | Flags admitidos | Importación iovec |
| --- | --- | --- | --- | --- |
| `android12-5.10` | `android12-5.10-2026-07_r1` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/pid.c) | `PIDFD_NONBLOCK` (`0x800`) | [Copiar primero todos los metadatos](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/lib/iov_iter.c) |
| `android13-5.10` | `android13-5.10-2026-07_r1` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/pid.c) | `PIDFD_NONBLOCK` | [Copiar primero todos los metadatos](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/lib/iov_iter.c) |
| `android13-5.15` | `android13-5.15-2026-09_r1` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/pid.c) | `PIDFD_NONBLOCK` | [Copiar primero todos los metadatos](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/lib/iov_iter.c) |
| `android14-5.15` | `android14-5.15-2026-07_r1` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/pid.c) | `PIDFD_NONBLOCK` | [Copiar primero todos los metadatos](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/lib/iov_iter.c) |
| `android14-6.1` | `android14-6.1-2026-09_r1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/pid.c) | `PIDFD_NONBLOCK` | [Copiar primero todos los metadatos](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/lib/iov_iter.c) |
| `android15-6.6` | `android15-6.6-2026-07_r1` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/pid.c) | `PIDFD_NONBLOCK` | [Ruta de un único búfer](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/lib/iov_iter.c) |
| `android16-6.12` | `android16-6.12-2026-09_r1` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/pid.c) | `PIDFD_NONBLOCK` y `PIDFD_THREAD` (`0x80`) | [Ruta de un único búfer](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/lib/iov_iter.c) |
| `android17-6.18` | `android17-6.18-2026-09_r1` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/pid.c) | `PIDFD_NONBLOCK` y `PIDFD_THREAD` | [Ruta de un único búfer](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/lib/iov_iter.c) |

Estas revisiones definen el contrato. Nuevas versiones o backports requieren verificar fuentes y añadir regresiones. Seleccionar GKI y declarar `pidfd_open` ausente es contradictorio y se rechaza antes de cargar.

## Subconjunto de descriptores de proceso

Las traps x64/AArch64 y `syscall` de Bionic comparten `LinuxServices` y la tabla propia de la carga. Se usan los 32 bits bajos de PID/flags; flags desconocidos o PID con signo no positivos devuelven `EINVAL` antes de asignar. El array opcional `tasks` declara un catálogo fijo y cerrado de otras tareas invitadas vivas:

```json
{"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true},{"id":3000,"group_leader":false}]},"linux_files":{"files":[],"descriptor_limit":16}}
```

El líder actual PID 1000 es implícito incluso con un array vacío. Sin catálogo, buscar otros objetivos sigue sin admitirse; un PID positivo válido fuera del catálogo declarado devuelve `ESRCH` antes de asignar. Sin `PIDFD_THREAD`, una tarea viva no líder devuelve `EINVAL` en 5.10–6.12 y `ENOENT` en [el `pidfd_prepare` de 6.18](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/fork.c). Con el flag admitido, 6.12/6.18 abren no líderes declarados. Los flags se validan antes de buscar el objetivo.

Cada entrada exige `id` entero entre 1..2147483647 y `group_leader` booleano; máximo 4096 entradas. Se rechazan duplicados, campos adicionales, PID 1000 no líder y catálogos sin GKI. Las prioridades deben referirse al catálogo o al proceso actual. El catálogo fijo no se combina con threads Android cooperativos (`thread_limit > 1`); creación, recolección, credenciales y traducción de namespaces necesitan su propia gestión de vida.

Se exige `linux_files`: archivos y pidfds comparten propiedad y límite. Se asigna el menor número libre; agotamiento devuelve `EMFILE`, `close` lo libera y cerrar dos veces da `EBADF`. Un flujo estándar cerrado permite reutilizar su número. No se consultan pidfds, archivos ni procesos anfitriones.

En un pidfd válido, `read`/`write` devuelven `EINVAL` antes de acceder a datos; `lseek` valida el origen y devuelve `ESPIPE`. `writev` importa primero metadatos y valida rangos de usuario: `EFAULT` puede preceder al `EINVAL` de la escritura ausente. No lee ni captura datos. stdout/stderr comparten el importador versionado; véase el [orden VFS](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/fs/read_write.c).

5.10/5.15/6.1 copian todo el array iovec antes de validar longitudes: una negativa seguida de metadatos inaccesibles produce `EFAULT`. Comprueban rangos originales antes del límite, incluso con un vector. 6.6/6.12/6.18 validan secuencialmente y producen `EINVAL`; un búfer se limita antes de validar su rango, mientras varios conservan todos los rangos originales. Son reglas de `copy_iovec_from_user`, `__import_iovec` e `import_ubuf`. Sin GKI permanece la política existente de un búfer, sin inferir versión.

Bionic convierte errores negativos raw en `-1` y `errno` local al thread; el éxito conserva `errno`. Faltan metadatos para `fstat`. Polling bloqueante, notificaciones de salida, señales mediante pidfd, `pidfd_getfd`, `fcntl`, ioctl pidfs y tareas no observadas siguen sin admitirse. Un pidfd no implica planificación ni ciclo de vida.

## Subconjunto de sondeo con tiempo cero

Al seleccionar GKI, `ppoll` raw x64/AArch64 y `syscall` variádico Bionic admiten una timespec explícitamente cero y una máscara temporal de señales nula. `linux_files.descriptor_limit` aporta el límite invitado RLIMIT_NOFILE; los 32 bits inferiores sin signo de `nfds` no pueden superarlo. La tabla compartida no indica disponibilidad para pidfd vivos observados, ignora descriptores negativos y devuelve `POLLNVAL` (`0x20`) para los cerrados. Cada entrada no nula cuenta por separado, incluidos duplicados. La disponibilidad no observada de otros tipos causa una parada explícita.

La copia y validación del tiempo preceden a máscara y descriptores. Una máscara no nula valida su tamaño de ancho completo y extensión legible antes del límite no admitido; una máscara nula ignora el tamaño. Todos los metadatos se importan antes de seleccionar disponibilidad o escribir resultados. Solo se escriben los campos `revents` de 16 bits en orden de entrada; un fallo posterior conserva las escrituras anteriores. El acceso mixto dentro de un campo mantiene el límite compartido de copia parcial no admitida. Incluso un array vacío recibe la comprobación final del rango de usuario. La timespec cero no se reescribe y puede ser legible pero de solo lectura. La llamada no lee ni avanza el reloj de pared.

Las reglas se comprobaron en las ocho versiones inmutables de `fs/select.c`: `ppoll`, `do_sys_poll`, `do_pollfd`, `poll_select_set_timeout` y `poll_select_finish`. La disponibilidad de pidfd vivos sigue `pidfd_poll` en `kernel/fork.c` para las seis primeras versiones y pidfs para las dos últimas:

[5.10 fs/select.c](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/fs/select.c) · [6.18 fs/select.c](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/fs/select.c) · [6.6 kernel/fork.c](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/fork.c) · [6.12 fs/pidfs.c](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/fs/pidfs.c) · [6.18 fs/pidfs.c](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/fs/pidfs.c)

Las notificaciones de salida y recolección varían por versión y quedan fuera del subconjunto de tareas vivas fijas. Las esperas bloqueantes, máscaras temporales, señales y wrappers Bionic `ppoll` con nombre requieren contratos propios.

## Subconjunto de relojes CPU de proceso

Con GKI explícito, `clock_gettime` admite ID negativos codificados de proceso PROF, VIRT y SCHED e interpreta los 32 bits bajos con signo. PID y tipo identifican una muestra explícita de `linux_time`. El proceso actual es implícito; los demás deben declararse líderes de grupo vivos en el catálogo cerrado antes de proporcionar su muestra.

```json
{"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true}]},"linux_time":{"advance_on_idle":true,"clocks":[{"id":1,"seconds":10,"nanoseconds":0},{"id":2,"seconds":3,"nanoseconds":4},{"id":-16006,"seconds":7,"nanoseconds":9}]}}
```

`-16006` es SCHED del PID 2000. PROF y VIRT son independientes. Los ID actuales SCHED 2, -6 (PID cero) y -8006 (PID 1000) comparten muestra; los alias PROF son -8/-8008 y VIRT -7/-8007. Se rechazan alias duplicados incluso con valores iguales. Los segundos CPU son no negativos y los nanosegundos normalizados. El avance inactivo solo cambia los relojes de pared 0, 1 y 7; las muestras CPU quedan fijas. La ejecución no deduce consumo CPU.

El TID propio de la tarea actual también identifica su grupo, incluidos los hilos cooperativos Android sin catálogo externo. Un PID externo ausente del catálogo cerrado o vivo sin ser líder devuelve `EINVAL` antes de acceder al destino. Omitir el catálogo o la muestra de un grupo conocido deja la operación sin soporte antes de copiar. Un tipo inválido devuelve `EINVAL`; una muestra válida puede producir `EFAULT` en la copia de usuario. Los traps conservan errores negativos; solo Bionic actualiza errno y devuelve -1.

Las reglas siguen `pid_for_clock`, `posix_cpu_clock_get`, el despachador y las definiciones de ID de cada revisión fijada:

| Rama solicitada | Fuente de relojes CPU de proceso |
| --- | --- |
| `android12-5.10` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/time/posix-cpu-timers.c) |
| `android13-5.10` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/time/posix-cpu-timers.c) |
| `android13-5.15` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/time/posix-cpu-timers.c) |
| `android14-5.15` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/time/posix-cpu-timers.c) |
| `android14-6.1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/time/posix-cpu-timers.c) |
| `android15-6.6` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/time/posix-cpu-timers.c) |
| `android16-6.12` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/time/posix-cpu-timers.c) |
| `android17-6.18` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/time/posix-cpu-timers.c) |

`init/Kconfig` de las versiones fijadas activa los temporizadores POSIX por defecto; los defconfig GKI no los desactivan. Véase [6.18 init/Kconfig](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/init/Kconfig).

La distinción FD y el enrutamiento CPU siguen también el [despachador 6.18](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/time/posix-timers.c) y las [definiciones de ID](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/include/linux/posix-timers_types.h) fijados. Los relojes FD y CPU codificados por hilo siguen sin soporte. El catálogo es una observación invitada fija; permisos, espacios de nombres, vida del proceso y contabilidad CPU requieren sus propios contratos.

## Validación y cobertura pendiente

`LinuxPIDFDTests.cpp` ejecuta ELF independientes x64/AArch64 O0/O2 para ocho ramas y backends disponibles: flags, tabla compartida, límites/reutilización, orden de errores, metadatos inaccesibles, límite frente a rangos originales, catálogos omitidos/cerrados, no líderes y objetivos antes del agotamiento. `AndroidSyscallTests.cpp` repite propiedad raw/Bionic, búsqueda y errno en seis perfiles O0/O2 con relocaciones normales, Android packed y RELR. Fuentes y ejecuciones prueban este subconjunto; no hay arranque nativo de todas las imágenes GKI fijadas. Ampliar Linux servicio por servicio conservando versiones, configuración y observaciones.

Los casos CPU verifican identidad, orden de salida, tipos independientes, muestras explícitas y separación del avance de pared. `AndroidTimeTests.cpp` comprueba salidas nominales/directas y centinelas; el syscall cooperativo comprueba el alias del TID actual no líder.

`ZeroTimeoutPollRetainsReadinessAndOrderedCopies` verifica llamadas raw O0/O2 de ocho GKI: descriptores vivos, negativos y cerrados, duplicados, reducción de argumentos, orden tiempo/máscara, timespec cero de solo lectura, importación completa antes de disponibilidad y primeros `revents` conservados tras un fallo posterior. `ZeroTimeoutPollKeepsUnobservedBoundaries` mantiene límites desconocidos de núcleo, recursos, máscaras, esperas y disponibilidad. Android `ReleasedGKIZeroTimeoutPollSharesRawAndBionicResults` verifica la tabla y propiedad de errno en seis perfiles de empaquetado.
