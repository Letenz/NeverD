**Idiomas**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 868e3a532347e28dedc1fc91cdb30d0b59a7a017afd6e52c53aa3344d055195e -->

[← Índice de documentación](README.md)

# Entornos de procesos invitados macOS e iOS

`lib/emulation/os/darwin/` modela procesos Mach-O autónomos y acotados, separados del transporte CPU anfitrión. Active `NEVERD_ENABLE_CPU_EMULATION`; no necesita emulación de controladores Windows. `macos/` e `ios/` definen perfiles explícitos.

| Perfil | Plataforma Mach-O | ISA invitadas | Página OS |
| --- | --- | --- | --- |
| `macos-macho64-v1` | macOS | x86-64, ARM64 base | 4 KiB x64; 16 KiB ARM64 |
| `ios-macho64-v1` | dispositivo iOS | ARM64 base | 16 KiB |
| `ios-simulator-macho64-v1` | iOS Simulator | x86-64, ARM64 base | 4 KiB x64; 16 KiB ARM64 |

Un binario de dispositivo no es una imagen de simulador; el anfitrión no decide la plataforma invitada. Con ISA coincidente, macOS puede usar [HVF](macos-hvf.md); entre ISA distintas, `auto` usa Unicorn. La granularidad CPU sigue siendo 4 KiB. Las [API C, Python y CLI](process-emulation.md) comparten opciones, límites e informes.

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

## Imagen y arranque

`MachOExecutionImage` conserva bytes originales sin modificaciones de relocalización del análisis. Solo admite imágenes thin little-endian `MH_EXECUTE` con plataforma y entrada inequívocas. Las imágenes universales requieren extraer explícitamente la arquitectura deseada.

El archivo entero, incluidos metadatos y bytes finales, debe caber en `memory_limit` antes de analizarlo o copiarlo. Se lee una instantánea privada y acotada de un archivo regular; se rechazan rutas con NUL, lecturas cortas y cambios de tamaño. No se mantiene un mapeo vivo del archivo. Archivo y memoria invitada tienen límites separados del mismo valor; las E/S anfitrionas no ofrecen plazo de tiempo real estricto.

Los segmentos conservan permisos actuales/máximos y relleno a cero. `__PAGEZERO` reserva direcciones sin asignar toda su extensión. Se comprueban rangos de archivo/VM, alineación OS, solapamientos redondeados, propiedad de la cabecera, entrada ejecutable y presupuesto. El segmento de cabecera debe ser legible y ejecutable; las páginas de guarda y la puerta privada de retorno permanecen reservadas. La última página de archivo conserva bytes hasta el límite de página o EOF; las páginas VM completas posteriores se ponen a cero según el [cargador XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c).

`LC_MAIN` recibe `argc`, `argv`, `envp` y el vector apple como cuatro argumentos enteros; el retorno aporta los ocho bits bajos del estado de salida. Se admite `/usr/lib/dyld` solo para esta entrada sin imports, sin ejecutar el dyld anfitrión. Se rechaza un `stacksize` distinto de cero: la opción `stack_size` del llamador fija el presupuesto.

`LC_UNIXTHREAD` exige exactamente un registro completo de estado general nativo de 64 bits, con solo PC establecido. La pila contiene argc, argv/envp terminados y un vector apple terminado con `executable_path=<input filename>`. Se rechazan SP/flags propios, otros registros, flavors adicionales y entradas contradictorias. No se heredan el entorno anfitrión ni un vector auxiliar Linux. Referencia: [arquitectura dyld](https://github.com/apple-oss-distributions/dyld/blob/main/doc/dyld4.md).

Dylibs externas, imports, rebases/chained fixups, constructores/destructores, secciones TLS, arm64e/PAC, otros subtipos CPU no admitidos, cifrado y comandos no modelados fallan antes de ejecutar. PIE sin fixups usa direcciones preferidas, sin ASLR. Los blobs de firma son metadatos, no una implementación de AMFI o políticas de entitlements.

## Servicios Darwin

Las llamadas BSD en ARM64 usan X16, X0–X5 y `svc #0x80`; x64 usa la clase BSD `0x02000000`, RAX y RDI/RSI/RDX/R10/R8/R9. El éxito limpia carry; el error lo activa y devuelve errno positivo. ARM64 limpia X1; x64 limpia RDX al tener éxito y lo conserva ante error. Los cambios de registros de SYSCALL son explícitos. El informe usa `result` y `error=true` para errores BSD; las solicitudes sin retorno o no admitidas carecen de ambos campos. Las reglas proceden de XNU [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c) y [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c), sin incorporar código Apple.

Servicios: `exit`, `write`, `getpid`, `getppid`, `getuid`, `geteuid`, `getgid`, `getegid`, `mmap`, `mprotect`, `munmap`. PID/UID/GID valen 1000 y PPID vale 1. Los descriptores 1 y 2 capturan bytes, incluidos NUL y no UTF8; los cerrados o de solo lectura devuelven EBADF. Una copia parcial conserva los bytes leídos, pero el fallo posterior sigue siendo EFAULT. Una longitud superior a `INT_MAX` devuelve EINVAL antes de comprobar descriptor, puntero o presupuesto: [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).

Se permiten mapeos privados anónimos de datos con `flags=0x1002`, descriptor -1 y offset cero. Longitudes y sugerencias no fijas se redondean hacia arriba a la página OS. Si una sugerencia está ocupada, se busca hacia arriba antes de volver a la ubicación predeterminada. El mmap histórico sin envolver de longitud cero devuelve cero sin asignar; `MAP_UNIX03` se admite y rechaza longitud cero con EINVAL. Unmap/protect requieren dirección alineada. Se admiten NONE/READ/WRITE, con WRITE implicando READ. Cada página OS posee su memoria física: un unmap parcial libera presupuesto y las páginas nuevas quedan a cero. Un protect que atraviese un hueco o supere permisos máximos deja intacto todo el rango. Fuente: [servicios VM de XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

Quedan excluidos mapeos compartidos/fijos/JIT o anónimos ejecutables, otros traps Mach, syscalls indirectas, hilos, señales, archivos del host/red, dyld, runtimes Objective-C/Swift y Foundation/UIKit. Su uso detiene explícitamente la ejecución. No es un OS Apple completo ni la aplicación iOS Simulator.

## Verificación

Las muestras C propias se generan con Clang y `ld64.lld`, sin SDK Apple ni binarios propietarios. Cubren cinco combinaciones plataforma/ISA, registros Mach-O malformados, páginas de 4/16 KiB y liberación parcial con presupuesto lleno. `NeverDProcessPublicTests` compara C API/CLI; `NEVERD_TEST_LIBNEVERD` y `NEVERD_TEST_DARWIN_FIXTURES` habilitan las mismas cinco combinaciones en Python.

## Archivos y descriptores explícitos

`darwin_files` ofrece a los tres perfiles un catálogo cerrado de archivos inicialmente de solo lectura. `files` es obligatorio: cada entrada contiene un `path` absoluto canónico del invitado y `bytes_hex` hexadecimal. `stdin_hex` opcional aporta una entrada finita; omitirla significa desconocida y detiene lecturas no vacías, mientras una cadena vacía significa EOF. Sin catálogo open se detiene; un catálogo explícitamente vacío devuelve ENOENT. No se consultan archivos ni entrada del host.

Se añaden `open`, `read`, `pread`, `lseek`, `close`, `dup`, `dup2`, `fcntl` y las entradas nocancel de read/write/open/close/fcntl/pread. Se admiten O_RDONLY/O_CLOEXEC y F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL. Cada open tiene posición independiente; los duplicados comparten posición y conservan flags close-on-exec individuales. pread no cambia la posición. Cerrar o sustituir 0/1/2 afecta a la I/O posterior; duplicar salida conserva destino y presupuesto.

Límites: 256 archivos, 16 MiB totales de rutas/NUL/archivos/entrada, rutas menores de 1024 bytes y componentes de hasta 255. `descriptor_limit` es un techo exclusivo de 3–4096, por defecto 256; JSON conserva 64 KiB. La configuración inválida falla antes de cargar. read superior a INT_MAX devuelve EINVAL antes de consultar FD; EOF no toca el destino y un destino inválido da EFAULT. Un búfer parcialmente escribible detiene la operación antes de copiar o mover la posición. Los errores SET/CUR/END conservan la posición. Stat antiguo y otros fcntl siguen excluidos. Un archivo como antecesor devuelve ENOTDIR. El mismo objeto se contrasta con macOS nativo y C/CLI/Python cubren cinco combinaciones; no demuestra ejecución en un dispositivo iOS.

Verificación Release de 2026-10-05: 381 registros, 177 aprobados, 204 omitidos, ningún fallo y 51/51 requisitos ARM64 HVF ejecutados. Pasaron también siete programas macOS nativos, 35 pruebas públicas C/CLI/informes, cinco combinaciones Python y 66 pruebas del verificador. Los recuentos se solapan. Los nuevos servicios no tienen evidencia nativa Intel HVF/KVM/WHP; Intel HVF sigue sin validar y sus Actions están suspendidas. Faltan el SDK iOS y la comparación con dispositivos.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## Modificar archivos existentes

El booleano estricto `"writable":true` o `DarwinFileOptions::WritableFiles` autoriza cambios locales al proceso. Ausente/false conserva solo lectura; un permiso desconocido detiene el servicio. No cambia el host ni los datos iniciales. write(4/397), pwrite(154/415), truncate(200), ftruncate(201) y O_TRUNC comparten contenido; open mantiene posiciones independientes, dup comparte posición y estado, y el contenido sobrevive al último close. El crecimiento rellena ceros y truncar conserva posiciones, incluso O_RDONLY|O_TRUNC.

F_SETFL cambia solo O_APPEND y conserva acceso, close-on-exec y FWASWRITTEN. F_GETFL muestra 0x10000 tras transferir bytes no vacíos, también con pwrite y salida capturada. pwrite ignora append y conserva posición. INT_MAX se comprueba antes de FD; -1 en pwrite devuelve EINVAL antes aún. INT64_MAX devuelve EFBIG antes del caso vacío; se recorta la longitud antes de elegir EOF.

ftruncate exitoso, incluso sin cambiar tamaño, marca FWASWRITTEN en la descripción invocada y sus dup. O_TRUNC marca la nueva descripción, incluso O_RDONLY; truncate por ruta no marca las existentes.

La entrada parcialmente legible se detiene antes de efectos. EFAULT completo conserva bytes, pero append no vacío mueve la posición a EOF. El fallo de transporte no confirma contenido ni posición. Sin `mutation_policy`, escritura no vacía, truncado y EFAULT completo no vacío invalidan la observación stat completa; consultas posteriores paran antes de copiar. La escritura vacía la conserva. Los 16 MiB suman rutas/NUL, entrada, registros, CWD, contenido actual y referencias de rutas escribibles. Reducir sustituye el almacenamiento y libera capacidad; entrada inicial y un búfer acotado adicional quedan fuera del límite lógico. Se rechazan alias inode conocidos y flags immutable/append-only.

DarwinMemory mantiene reservas hasta el último unmap, incluso PROT_NONE y FD cerrados; las mutaciones paran mientras existan. Fallos y mmap antiguo de longitud cero no retienen reservas. Nuevos mapas ven bytes actuales. O_WRONLY con READ/WRITE da EACCES; PROT_NONE puede ganar lectura/escritura mediante mprotect.

Programas originales normal/nocancel comparan el kernel nativo; pruebas 4K/16K y C/CLI/Python cubren cinco combinaciones. Metadatos de creación, borrar directorios, renombrar, enlaces físicos, metadatos del sistema de archivos nativo, coherencia de mapas y SIGBUS EOF siguen pendientes. El entorno completo, dispositivos iOS e Intel HVF no están validados; Actions Intel permanece suspendido.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233","writable":true}]}}
```

[XNU write](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU vnode](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c).

## Metadatos mutables explícitos

Cada archivo puede añadir mutation_policy junto a `writable: true` y metadata completos; C++ usa `DarwinFileOptions::MutationPolicies`. Es un contrato virtual de asignación dispersa explícito, sin inferir APFS ni consultar el reloj host. Si se omite, los metadatos posteriores siguen siendo desconocidos.

allocation_unit, mutation_time y seconds/nanoseconds son obligatorios, con las reglas enteras sin pérdida existentes. La unidad es una potencia de dos entre512 bytes y16 MiB, independiente de block_size y páginas VM. Se requieren permisos normales sin set-id/sticky, flags=0, link_count=1 y asignación inicial densa: blocks=ceil(size/allocation_unit)*(allocation_unit/512). Ceros no implican huecos. La referencia de ruta cuenta en el límite lógico de16 MiB; la contabilidad de bloques no inventa ENOSPC.

Escribir asigna toda unidad tocada, incluso ceros en huecos. truncate al crecer añade ceros sin asignar; al reducir descarta unidades después de EOF redondeado hacia arriba y retiene la última parcial. Volver a crecer no restaura asignaciones descartadas. Escrituras no vacías exitosas y todo truncate exitoso, incluso del mismo tamaño u O_TRUNC vacío, actualizan size/blocks y fijan mtime/ctime al tiempo suministrado. Otros campos y entradas se conservan; read no avanza atime. Stat por ruta, open independientes, dup y reapertura comparten nodo.

Escritura vacía, rechazo por presupuesto/mapas, entrada parcial rechazada y fallo de transporte conservan el estado. EFAULT completo no vacío lo vuelve desconocido; un éxito posterior no lo reconstruye. Fallar la copia stat no cambia el nodo. virtual-file-metadata verifica144 bytes en cinco perfiles y C/CLI/Python: es prueba de política, no equivalencia APFS. Los programas nativos verifican aparte flags, posiciones y errores. Espacio de nombres, coherencia nativa, Mach y carga dinámica siguen pendientes.

```json
{"mutation_policy":{"allocation_unit":4096,"mutation_time":{"seconds":-7,"nanoseconds":123456789}}}
```



## Posicionamiento en archivos dispersos

Con mutation_policy y asignación conocida, lseek admite SEEK_HOLE=3 y SEEK_DATA=4 en archivos regulares usando el mismo registro que stat. La entrada inicial es densa, incluso sus ceros. Devuelve la posición de entrada dentro de una unidad del tipo buscado, o el inicio de la siguiente coincidente. El hueco final comienza en EOF. Un negativo da EINVAL; en/después de EOF, archivo vacío o sin datos posteriores, ENXIO=6. El fallo conserva el cursor; el éxito solo cambia la descripción y sus dup. Otros open conservan sus cursores y reabrir ve la asignación actual. Metadatos, flags y bytes no cambian; se ignoran bits altos de whence.

Sin política, para directorios o tras EFAULT completo con asignación desconocida, sigue sin soporte. Ceros y cambios rechazados no permiten inferir asignaciones. sparse-file-seek compara errores, bytes escritos, EOF y vida de descripciones nativas/invitadas sin asumir límites anteriores del FS. virtual-file-metadata comprueba aparte la geometría exacta de la política; C/CLI/Python cubren cinco perfiles.

[XNU lseek](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Eliminar nombres de archivos regulares

`mutable:true` por directorio (C++ `MutableDirectories`) autoriza cambios de nombres inmediatos, independientemente de `writable`. Sin autorización se detiene explícitamente. Se rechazan flags conocidos no nulos, permisos especiales del padre, link_count≠1 del hijo y alias conocidos del padre/hijo, combinando stat e inodes de snapshots. Dispositivos explícitamente distintos siguen separados; los caminos cuentan en el presupuesto.

`unlink(10)` / `unlinkat(472)` eliminan nombres regulares existentes. unlinkat acepta los32 bits bajos 0 o `0x800`; bits desconocidos dan EINVAL antes de ruta/FD y otros modos de borrado conocidos siguen sin soporte. Resolución común: ENOENT, ENOTDIR tras archivo con `/`, EPERM en directorio ordinario, EBUSY en raíz. Se verificaron nativamente finales `.`/`..`.

FD/dup/aperturas independientes previos conservan datos, cursores y flags; F_GETPATH devuelve la ruta anterior capturada. Nuevas aperturas fallan, padres implícitos y CWD permanecen. La autorización de escritura pertenece al objeto; close/dup2/la siguiente mutación recuperan sus bytes actuales sólo tras el último descriptor y mapa. Los costes iniciales de rutas permanecen; metadatos de creación, renombrado, enlaces físicos y borrado de directorios siguen pendientes.

stat/readdir/SEEK_END del padre quedan desconocidos para todo FD/ruta y paran antes de copiar o mover cursores. read/pread mantienen EISDIR; SET/CUR/F_GETPATH/fchdir/resolución relativa continúan. La política conocida establece nlink=0 y ctime fijo; posteriores escrituras no restauran nlink=1. Sin política/tras EFAULT, metadatos desconocidos. Los fallos preservan estado. `unlinked-file` compara reglas nativas de nombre/FD; tiempos e invalidación son reglas explícitas del modelo.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"00"}],"directories":[{"path":"/work","mutable":true}]}}
```

[XNU unlink / unlinkat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [F_GETPATH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c).

## Crear archivos normales

O_CREAT=0x200 crea un archivo vacío bajo un padre directo explícitamente mutable mediante open/openat normal o nocancel. El objeto nuevo permite escritura; los existentes conservan su autorización WritableFiles. Un FD de lectura puede crear pero no escribir. No se inventan credenciales, umask, inode o stat ni se heredan metadata/mutation_policy del objeto anterior con igual nombre; stat64 y búsqueda dispersa siguen desconocidos.

O_EXCL=0x800 con O_CREAT devuelve EEXIST para archivos/directorios existentes antes de truncar; solo no tiene efecto. O_CREAT de lectura abre directorios existentes. Orden: modo inválido, capacidad FD, EINVAL por O_CREAT|O_DIRECTORY, ruta. Solo se crea el último componente original ausente; ancestros ausentes y sufijos `/`, `//`, `/.`, `/..` dan ENOENT. O_CREAT|O_TRUNC nuevo no marca FWASWRITTEN; truncar uno existente sí.

Solo insertar invalida las observaciones del padre. Objetos nuevos/antiguos homónimos mantienen datos, FD, metadatos y mapas independientes. Las 256 entradas incluyen elementos iniciales no archivo y objetos vivos; rutas canónicas/NUL dinámicas y bytes actuales cuentan en 16 MiB. Tras unlink, el último FD/mapa libera costes dinámicos; los iniciales permanecen. Agotar presupuesto o llegar a 1024 bytes de ruta canónica detiene explícitamente sin inventar ENOSPC o errno de ruta nativo ni publicar nombre/FD. created-file compara macOS nativo y cinco perfiles; pruebas 4K/16K verifican límites. Quedan metadatos de creación, renombrado, enlaces y mutación de directorios.

[XNU open](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

## Directorios y rutas relativas

`directories` permite `path` absolutos canónicos con `metadata` completo opcional, incluidos directorios vacíos. Raíz y antecesores son implícitos; los metadatos no crean rutas ausentes. Mode usa `0x4000` y permisos; size es una observación explícita en [0, INT64_MAX]. `working_directory` debe existir; omitirlo deja CWD desconocido, sin heredar el host. Máximo 256 rutas declaradas, incluidos antecesores con metadatos; rutas/NUL/contenido/entrada/CWD suman 16 MiB.

`openat` (463), `openat_nocancel` (464), `chdir` (12), `fchdir` (13) y `fstatat64` (470) comparten resolución. Las rutas relativas usan FD de directorio o `AT_FDCWD=-2`; las absolutas ignoran FD. Separadores repetidos, `.`, `..` y barra final verifican cada antecesor: `/file/..` da ENOTDIR, `/missing/..` ENOENT. Los fallos y cerrar, reutilizar o sustituir el FD original conservan CWD. `F_GETPATH=50` copia ruta canónica y NUL incluso tras dup, sin tocar bytes posteriores.

read/pread de directorio da EISDIR aun con longitud cero; un offset pread negativo devuelve primero EINVAL. SET/CUR comparten cursor, END exige size explícito; mmap da EINVAL. fstatat64 acepta 0, `AT_SYMLINK_NOFOLLOW=0x20`, `AT_SYMLINK_NOFOLLOW_ANY=0x800` y `AT_FDONLY=0x400` (ignora la ruta). Bits inválidos dan EINVAL; `AT_REALDEV=0x200` sigue excluido. Identidades de flujos desconocidas, permisos sin control de acceso; mutación queda pendiente. El mismo `directories` compara el núcleo nativo y cinco invitados; stat contrasta archivos y directorios reales. Intel HVF Actions sigue suspendido.

[XNU VFS](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/fcntl.h).

Validación de directorios (2026-10-05, Release): 467 registros, 227 aprobados, 240 omitidos, cero fallos; 60/60 requisitos ARM64 HVF ejecutados. Pasaron 10 programas macOS nativos, 37 pruebas C/CLI/informes sin omisiones, cinco invitados Python y 66 pruebas de herramientas. Recuentos solapados. Evidencia: `build-hvf-arm64/darwin-directory-verified-evidence/`. Otros transportes nativos e iOS físico siguen sin validar.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## Instantáneas explícitas de directorio

`getdirentries64` (344) recorre el `contents` inmutable opcional de una entrada `directories` existente; C++ usa `DarwinFileOptions::DirectoryContents`. `entries` incluye en orden explícito todos los hijos directos, `.` y `..`. Sin instantánea, incluso un directorio vacío sigue desconocido. No crea rutas ni stat ni consulta al anfitrión.

Cada entrada exige `name`, `inode` no nulo, `type` (0 desconocido, 4 directorio, 8 archivo), `next_offset` y `seek_offset`. El tipo coincide con la ruta; el inode de la misma ruta resuelta coincide entre instantáneas y metadatos. `next_offset` es positivo, único en ese directorio y <=INT64_MAX, sin orden creciente obligatorio; cero rebobina. `seek_offset` es una observación d_seekoff separada de 64 bits sin signo y admite ceros repetidos. Los enteros usan las cadenas decimales sin pérdida de stat.

`contents.minimum_buffer_size` exige un mínimo de carga de 1–128 MiB, incluido EOF. El `minimum_buffer_size` opcional por entrada (predeterminado 0) limita llamadas que empiezan allí. El ejemplo observa APFS: 64 bytes para los dos puntos iniciales, 1 en EOF; otras posiciones deben alojar un registro entero. LP64 usa alineación de ocho bytes y tamaño `roundUp(25 + nameBytes, 8)`. Máximo 4096 entradas en total; sus bytes cuentan en 16 MiB. Las rutas ancestrales declaradas solo por metadatos/instantánea cuentan una vez en las 256 rutas. JSON conserva 64 KiB.

Open independientes tienen cursores propios, dup los comparte. Solo cero o valores suministrados permiten continuar; una posición desconocida detiene explícitamente. Cada llamada devuelve el máximo prefijo de registros completos. Longitud >=1024 reserva los cuatro últimos bytes solicitados para EOF (1 al final, 0 en otro caso); solo la carga se limita a 128 MiB. La dirección conserva la aritmética original sin signo, incluido el desbordamiento. Orden: datos, avance del cursor, posición anterior, indicadores. Un EFAULT posterior conserva efectos previos; EOF omite la copia vacía. Una copia individual parcialmente escribible se detiene antes de esa copia, conservando los efectos anteriores.

`directory-entries` compara campos, dup/rebobinado, lecturas pequeñas, EOF y orden de copias con macOS. Otro test compara todos los bytes nativos capturados, con nombres largos, y el diseño SDK. Los cookies fijos no reproducen generaciones dinámicas APFS. El antiguo `getdirentries` (196), mutaciones, otros transportes nativos e iOS físico quedan fuera de esta validación.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"directories":[{"path":"/empty"},{"path":"/","contents":{
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":73,"type":8,"next_offset":99,"seek_offset":0}]}}]}}
```

[XNU getdirentries64](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [dirent ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [extended flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent_private.h).

Validación de enumeración (2026-10-05, Release): 498 casos Darwin, 246 aprobados, 252 omitidos por backend no disponible, cero fallos; 63/63 casos ARM64 HVF obligatorios ejecutados. Pasaron 11 programas macOS nativos, 40 controles C/CLI/informes sin omisiones, cinco combinaciones Python con ocho escenarios de archivos cada una y 66 tests de herramientas. Los conteos se superponen. Evidencia: `build-hvf-arm64/darwin-dirents-merged-evidence/`. Intel HVF Actions sigue suspendido; otros transportes nativos e iOS físico no están validados.


## Mapeos privados de archivos

`mmap` acepta archivos regulares del catálogo con `MAP_PRIVATE`: `flags=0x2` o `0x40002` con `MAP_UNIX03` y offset alineado a la página OS. Conserva todos los bytes del archivo dentro de la página aunque la longitud pedida sea menor; el resto de la página final EOF es cero. Las escrituras privadas solo cambian ese mapeo, sin alterar archivo, otros mapeos, metadatos fijos ni cursor compartido. El mapeo sobrevive a close y a reutilizar el FD. Los mapeos de solo lectura y PROT_NONE reciben sus bytes iniciales; `mprotect` puede permitir escritura.

El desbordamiento del final, longitud UNIX03 cero y offset UNIX03 desalineado dan EINVAL antes de buscar FD; un FD inválido da EBADF antes del presupuesto. La longitud histórica cero también verifica el FD. Offsets históricos desalineados, flujos, archivos vacíos y páginas completas más allá de EOF detienen antes de asignar. macOS permite esos mapeos EOF pero el acceso produce SIGBUS; el modelo no inventa páginas cero legibles ni entrega de señales. Mapeos compartidos, fijos, ejecutables y JIT siguen excluidos.

`DarwinFiles` resuelve FD y bytes; `DarwinMemory` gestiona ubicación, permisos, presupuesto y reversión. Los datos vienen solo de `darwin_files`. `file-mapping` verifica copias, close, cursores, errores y reutilización anónima; otra comparación nativa verifica un offset no nulo, la página entera y SIGBUS.

[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c)

### Verificación de mapeos privados, 2026-10-05

Release Darwin: 438 registros únicos, 210 aprobados, 228 omitidos, cero fallos. Se ejecutaron 57/57 requisitos ARM64 HVF y cinco combinaciones Unicorn. Pasaron nueve programas macOS nativos, la comparación de toda una página con offset no nulo y SIGBUS en un proceso hijo aislado. Los 36 casos API/informe no tuvieron omisiones; Python cubrió cinco combinaciones con `file-mapping`, además de 66 pruebas de herramientas y 38 de procedencia. Los recuentos se solapan. Evidencia: `build-hvf-arm64/darwin-mmap-verified-evidence/`. Sin nueva evidencia Intel HVF/KVM/WHP o iOS físico; Intel HVF Actions sigue suspendido.

## Metadatos explícitos de archivos

Cada archivo puede incluir `metadata`; todos los campos siguientes son obligatorios. Las cadenas decimales conservan el ancho completo; los números JSON deben ser enteros exactos dentro de ±(2^53−1). device es de 32 bits con signo, mode/link_count de 16 sin signo, inode de 64 sin signo y uid/gid/flags/generation de 32 sin signo. size debe coincidir con los bytes; blocks cabe en 64 bits con signo y block_size en 32 con signo no negativos. Los tiempos usan segundos de 64 bits con signo y 0–999999999 nanosegundos.

`stat64` (338), `fstat64` (339) y `lstat64` (340) devuelven el mismo registro LP64 de 144 bytes en ARM64/x64. Comparten la resolución de open y respetan dup/close sin asignar FD ni mover cursores. rdev, relleno y campos reservados son cero. Las entradas aportan metadatos iniciales y la política opcional regula cambios; read no actualiza tiempos y mode no cambia el acceso al catálogo. Metadatos ausentes, flujos, enlaces simbólicos, stat antiguo, y seguridad ampliada siguen excluidos. Los errores de ruta/FD preceden al puntero de salida; las salidas parcialmente accesibles se rechazan antes de escribir. Las pruebas nativas comparan todos los bytes de un archivo real y los offsets del SDK; el mismo programa original comprueba las tres llamadas.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","metadata":{
  "device":1,"inode":"18364758544493064720","mode":33188,"link_count":1,
  "uid":1000,"gid":1000,"size":10,"block_size":4096,"blocks":8,
  "flags":0,"generation":0,
  "access_time":{"seconds":-1,"nanoseconds":1},
  "modification_time":{"seconds":2,"nanoseconds":3},
  "change_time":{"seconds":4,"nanoseconds":5},
  "birth_time":{"seconds":6,"nanoseconds":7}
}}]}}
```

[XNU stat.h](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/sys/stat.h)

### Verificación de metadatos y próximos pasos (2026-10-05)

Con stat64: 409 registros únicos, 193 aprobados, 216 omitidos, ningún fallo; se ejecutaron 54/54 casos ARM64 HVF obligatorios y cinco combinaciones Unicorn. Pasaron la comparación SDK/registro real, ocho programas nativos, 36 casos API/informe sin omisiones, cinco combinaciones Python y 66 pruebas de herramientas; los recuentos se solapan. Cada caso nativo tiene su propio archivo de salida, evitando residuos tras salidas más cortas. Los añadidos carecen de evidencia nativa Intel HVF/KVM/WHP o iOS físico.

Después: mapeos compartidos y fallos de página EOF, escritura acotada (páginas EOF, duración tras close, orden de errores), observaciones explícitas de tiempo/sistema, servicios Mach/hilos necesarios y dependencias Mach-O, rebases/binds, inicialización y TLS. Objective-C/Swift y Foundation/UIKit requieren programas nativos de referencia. iOS físico necesita SDK y dispositivo; Intel HVF sigue sin validar y sus Actions suspendidas.



```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

La validación independiente exige los 84 casos nativos ARM64 o 56 x64, con `LC_MAIN` y `LC_UNIXTHREAD` en cada plataforma. Casos obligatorios ausentes/omitidos o falta de `ld64.lld` producen fallo.

```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

Use `kvm` en Linux o `whp` en Windows. El [workflow Darwin](../../.github/workflows/darwin-native.yml) prueba ambos transportes x64 sin Unicorn y admite repeticiones individuales. La [referencia de kernel](../../.github/workflows/darwin-kernel-reference.yml) ejecuta los programas directamente en ambas ISA macOS sin NeverD/LLVM. `DarwinNativeCases.def` fija modos, estados y bytes esperados. Solo la referencia anfitriona enlaza libSystem para la entrada real de dyld. ISA incorrecta, Rosetta, timeout o diferencias hacen fallar la prueba; no constituyen evidencia del kernel de un dispositivo iOS.

## Evidencia y alcance pendiente

Resultados del 2026-10-03; no sume filas solapadas:

| Transporte | Fuente | Correctas | Fallidas | Omitidas | Cargas nativas |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 HVF | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel HVF | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| x64 KVM | `36e11ca8a` | 51 | 0 | 235 | 26/26 |
| x64 WHP | `36e11ca8a` | 51 | 0 | 235 | 26/26 |

La [ejecución Intel](https://github.com/NeverSight/NeverD/actions/runs/37106013999) coteja 286 identidades CTest y 32 procesos con XML original. Las 234 omisiones son 65 casos Unicorn desactivados, 39 invitados ARM64 y 130 de otras plataformas anfitrionas. El artefacto `11267489438` tiene SHA-256 verificado `cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`. También se verificaron [KVM/WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703) de forma independiente. La [referencia de kernel](https://github.com/NeverSight/NeverD/actions/runs/37064795867) pasó 4/4 programas por ISA, con estado 37, salida exacta y stderr vacío.

C API/CLI con Unicorn: 138 correctas, 156 omitidas, cero fallos. Python cubre las cinco combinaciones; el motor empaquetado coincide con 18 informes CLI ARM64 y supera las firmas de 186 imágenes Mach-O. HVF/Unicorn OFF pasa 38 comprobaciones, omite 231 y no enlaza Hypervisor.framework. Son pruebas de integración, no más ejecuciones nativas. La CPU Intel completa sigue sin validar; consulte [HVF](macos-hvf.md) y el [registro detallado](../darwin-emulation.md#hosted-native-verification-2026-10-03).

## Observaciones temporales explícitas

`ProcessOptions::DarwinTime` / `darwin_time` aporta observaciones fijas para la llamada directa `gettimeofday` (116), incluida su tercera salida `mach_absolute_time`, en todos los perfiles Darwin. `time_of_day`, `timezone` y `mach_absolute_time` son opcionales: ausencia significa desconocido; cero explícito es un valor. Un objeto vacío no crea relojes predeterminados. No se lee el reloj anfitrión, se deduce la zona horaria, avanza el tiempo ni convierten los ticks absolutos.

Cada registro proporcionado exige todos sus miembros. `seconds` es de 32 bits sin signo, `microseconds` pertenece a [0, 999999], `minutes_west` / `dst_time` son de 32 bits con signo y los ticks de 64 bits sin signo. JSON usa las reglas enteras sin pérdida; fuera del intervalo seguro se requieren cadenas decimales. Campos desconocidos, rangos inválidos y perfiles ajenos a Darwin se rechazan antes de cargar la imagen.

El `timeval` LP64 ocupa 16 bytes: segundos extendidos con ceros en 0, microsegundos de 32 bits en 8 y cuatro bytes cero en 12. La zona tiene dos campos de 32 bits con signo y los ticks ocho bytes. La hora civil y absoluta se muestrean juntas inicialmente; toda observación solicitada debe existir antes de copiar o comprobar punteros. Luego se copian timeval, timezone y absolute ticks. Una zona ausente o un EFAULT posterior conserva las escrituras anteriores; los alias siguen ese orden. Una salida individual parcialmente escribible detiene la operación antes de esa copia y conserva las previas. Todos los punteros nulos funcionan sin configuración; consultas selectivas solo exigen los valores solicitados.

El programa original `time` comprueba el comportamiento nativo; `time-values` emite los 32 bytes configurados mediante C/CLI/Python en las cinco combinaciones invitadas. Un oráculo SDK compara cada byte con tres salidas capturadas en una sola llamada nativa directa. Quedan pendientes relojes que avanzan, conversión, contadores commpage, temporizadores y objetos de reloj Mach/IPC, además de dyld, hilos, Objective-C/Swift y Foundation/UIKit. Intel HVF Actions sigue suspendido; no se añade validación nativa Intel ni de iOS físico.

```json
{"darwin_time":{"time_of_day":{"seconds":4045620583,"microseconds":654321},"timezone":{"minutes_west":-480,"dst_time":-1},"mach_absolute_time":"18364758544493064720"}}
```

[XNU gettimeofday](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_time.c), [time ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/time.h).

Validación temporal (2026-10-06, Release): 538 casos Darwin, 274 aprobados, 264 omitidos por backend no disponible, sin fallos; ejecutados los 66/66 casos ARM64 HVF obligatorios. Pasan los 12 programas nativos macOS y la comparación SDK de una sola muestra. C/CLI/informes: 43/43, sin omisiones. Python pasa en cinco combinaciones, incluidos los bytes temporales exactos y ocho modos de archivos existentes. Pasan los 66 tests de herramientas, localización, capacidades y formato. Recuentos superpuestos. Evidencias: `build-hvf-arm64/darwin-time-verified-evidence/`, `darwin-time-native-first/`, `darwin-time-public.xml`.

## Tiempo Mach y convenciones de retorno

`darwin_time.timebase` aporta `numerator` y `denominator`, enteros de 32 bits sin signo y distintos de cero. La razón se conserva sin reducir ni convertir. `mach_timebase_info_trap`, índice 89, usa ARM64 X16=-89 o x64 RAX=0x01000059. Escribe ocho bytes little-endian (numerador, denominador) y devuelve cero, incluso con una dirección de salida totalmente inválida. Una salida parcialmente escribible detiene antes de copiar; los errores del transporte se propagan. La ausencia de configuración detiene antes de comprobar el puntero, incluso nulo.

ARM64 X16=-3 y X16=-4 devuelven los 64 bits sin signo de `mach_absolute_time` y `mach_continuous_time`. Cada llamada solo necesita su propio valor; el cero explícito es válido. Las entradas nativas x64 correspondientes generan EXC_SYSCALL y no están admitidas. Siguen pendientes relojes que avanzan, commpage, temporizadores y objetos de reloj Mach/IPC.

La resolución usa los 32 bits bajos del número; el informe conserva los 64 originales. Los negativos ARM64 eligen Mach; x64 usa 0x01000000 para Mach y 0x02000000 para BSD. BSD 3/4 siguen siendo read/write; números desconocidos y clases ajenas detienen. La entrada resuelta determina el retorno: Mach conserva flags y X1/RDX, BSD mantiene sus reglas carry; x64 sigue actualizando RCX/R11. Los informes Mach incluyen `result` y omiten `error`, incluso con carry inicial activo.

`mach-time` compara flags, resultado secundario, bits altos, punteros inválidos y transiciones BSD con el núcleo ARM64 nativo. `mach-timebase-values` verifica bytes exactos en cinco invitados, `mach-clock-values` en ARM64; el SDK comprueba estructura y razón capturada. Intel HVF Actions sigue suspendido; pruebas de software y sintaxis x64 no validan Intel nativo ni iOS físico.

```json
{"darwin_time":{"timebase":{"numerator":125,"denominator":3},"mach_absolute_time":"18364758544493064720","mach_continuous_time":"18446744073709551615"}}
```

[XNU clock traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/clock.c), [ARM64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [ARM64 special traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/sleh.c), [x64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/x86_64/idt64.s).

Validación Mach (2026-10-06, Release): 569 casos Darwin, 293 aprobados, 276 omitidos por backend no disponible, cero fallos; se ejecutaron los 69/69 obligatorios ARM64 HVF. La ejecución final aprobó 13 programas nativos y dos oráculos temporales SDK. C/CLI/report: 100/100 sin omisiones; Python cubrió cinco invitados. Las comparaciones públicas se aíslan por plataforma y escenario con presupuesto invitado explícito de 10 segundos; valores predeterminados y regresiones de plazo siguen iguales. Los recuentos se solapan.

Los primeros arranques nativos excedieron el límite existente de 5 segundos: medición independiente de 6.056 segundos y 0.010 al reutilizar. El mismo binario aprobó después 13 casos bajo el límite original; se conservan los fallos. La verificación secuencial separada aprobó tras los tiempos agotados bajo carga. Evidencias: `build-hvf-arm64/darwin-mach-time-final-evidence/`, `darwin-mach-time-native-recheck/existing-binary-recheck.json`, `darwin-mach-time-public-accepted.xml`, del árbol previo al commit. En esa revisión, ARM64 MRS/MSR NZCV no formaban parte del contrato checked; el test observaba los flags con instrucciones enteras. El cambio siguiente resuelve esa carencia de CPU.

## Registro de flags de condición ARM64

El contrato ARM64 checked compartido admite las codificaciones exactas `MRS Xt, NZCV` y `MSR NZCV, Xt` en EL0/EL1. Las lecturas devuelven solo los bits 31–28; las escrituras toman esos cuatro bits de entrada e ignoran los demás. Leer hacia `XZR` descarta el resultado; escribir desde `XZR` borra los flags sin leer SP. Cada backend ejecuta las instrucciones originales. La validación del setter del host y los límites FPCR/FPSR no cambian; los registros de sistema vecinos no declarados siguen sin admitirse.

`NeverDAArch64NZCVTests` compara todas las combinaciones con instrucciones del host y verifica estado escalar/vectorial completo, memoria, registros límite, parada/fallo del observador, restauración del contexto y presupuestos compartidos. ARM64 `mach-time` usa ahora MSR/MRS reales alrededor de SVC para comprobar la conservación Mach y la transición a BSD. Los requisitos HVF nativos incluyen seis métodos en ambos privilegios y el oráculo del host. ARM64 KVM/WHP e iOS físico siguen sin validar. Quedan archivos escribibles, información del sistema, relojes que avanzan, Mach IPC/hilos, dyld/runtimes/frameworks y aceptación en dispositivos.

[Arm NZCV (DDI0601, 2025-06)](https://developer.arm.com/documentation/ddi0601/2025-06/AArch64-Registers/NZCV--Condition-Flags).


Validación de archivos modificables (2026-10-06): Release Darwin, 610 registros, 322 aprobados, 288 omitidos por backend no disponible, cero fallos; 72/72 requisitos ARM64 HVF ejecutados. La revisión final con nuevas aserciones EFAULT/metadatos suma 102 aprobados y 12 omitidos de 114. Pasaron los 15 programas nativos y 111 pruebas públicas C/CLI/informes. Los recuentos se solapan. La primera ejecución nativa detectó FWASWRITTEN; se corrigió y se conserva el fallo original. Sin cambios de límites temporales. CI GitHub completa e iOS físico siguen aparte; Actions Intel suspendido.

`build-hvf-arm64/writable-darwin-evidence/` · `writable-native-final/` · `writable-focused-final.xml` · `writable-public.xml`

Python superó inicialmente cinco segundos en tres casos ARM64 de directorio. Con argumentos idénticos pasaron los diez escenarios nuevos de escritura; uno iOS agotó5,005 s reales con1,263 s CPU. Las tres repeticiones aisladas pasaron con el mismo límite en2,43–3,17 s,10.941 instrucciones y salida65. Carga54–70 con16 CPU lógicas apoya presión de planificación, no garantiza latencia; se conservan fallos originales.

El método Python final sin cambios pasó las cinco combinaciones en41,118 s, conservando cinco segundos por proceso y los fallos/diagnósticos anteriores por separado.


Validación de metadatos (2026-10-06): Release focalizado148=124 aprobados/24 omitidos. Darwin completo645=343 aprobados/300 omitidos/2 tiempos agotados de directorios ARM64 HVF existentes. Repetición idéntica20=8 aprobados/12 omitidos, casos afectados3.818/3.949s con límite original5s. Las75 identidades HVF requeridas tienen observaciones exitosas; se conserva el primer fallo. C/CLI/informes117/117 con73 Darwin, Python cinco perfiles27.359s, nativo15/15, runners66/66 aprobados. Asignación virtual, no prueba APFS. Sin cambiar plazos; CI completa, Intel, iOS físico y entorno completo pendientes.

`build-hvf-arm64/mutation-metadata-validation-summary.json`; `mutation-metadata-darwin-evidence/`; `mutation-metadata-directory-recheck/`; `mutation-metadata-focused.xml`; `mutation-metadata-public.xml`.

Validación de búsqueda dispersa (2026-10-06): Release Darwin, 671 casos, 359 aprobados, 312 omitidos por backends no disponibles, sin fallos. Se ejecutaron los 78 casos ARM64 HVF obligatorios; Unicorn cubrió cinco perfiles. Pruebas específicas: 123 aprobadas de 147, 24 omitidas. Pasaron 16 programas nativos, 122 comprobaciones C/CLI/report (78 comparaciones Darwin), cinco perfiles Python (12.344 s) y 66 pruebas del runner. Los recuentos se solapan; no cambiaron los límites y se conservan los fallos históricos. Evidencia: `build-hvf-arm64/sparse-seek-validation-summary.json`. La asignación es una política virtual explícita, sin equivalencia APFS. CI completa e iOS físico siguen pendientes; Intel HVF Actions permanece suspendido.

Validación unlink (2026-10-06): Release Darwin708 casos,384 aprobados,324 omitidos por backends no disponibles, sin fallos;81 ARM64 HVF obligatorios ejecutados. Específicos156:137 aprobados/19 omitidos. Nativos17/17, C/CLI/report128/128 (Darwin83), Python cinco perfiles16.268s, runner66/66 aprobados. Revisión independiente sin bloqueos pendientes. Recuentos solapados, plazos intactos, sin reintentos. Evidencia: `build-hvf-arm64/unlink-validation-summary.json`. Invalidación/tiempos fijos son reglas del modelo; sistema de archivos/runtime completo e iOS físico siguen pendientes. Intel HVF Actions suspendido; CI completa separada.

### Validación de creación, 2026-10-06

Release Darwin:748 casos,412 aprobados,336 omitidos por backend ausente, cero fallos;84 requisitos ARM64 HVF ejecutados. Dirigidos162:150 aprobados/12 omitidos. C/CLI/informes133/133 (Darwin88), Python5 perfiles9.982s, nativos18/18, runners66/66 aprobados. ARM64 rechazó correctamente las rebases de la tabla de punteros del test inicial; bytes internos la corrigieron sin relajar el cargador. Se conservan fallos/binarios iniciales; inventario esperado27→28. Revisión independiente sin bloqueos, incluida preservación del padre ante límite de capacidad. Cuentas solapadas y plazos iguales. CI completa/iOS físico separados; Actions Intel HVF suspendido.

`build-hvf-arm64/create-validation-summary.json`, `create-darwin-evidence/`, `create-focused.xml`, `create-public.xml`, `create-native-final/`, `create-initial-evidence/`.
