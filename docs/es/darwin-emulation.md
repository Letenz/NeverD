**Idiomas**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 813c9673241230afbb295a950aab1e14478b4bd4fe9de2d2f2e27b6fbe34f588 -->

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

ARM64 usa X16, X0–X5 y `svc #0x80`; x64 usa la clase BSD `0x02000000`, RAX y RDI/RSI/RDX/R10/R8/R9. El éxito limpia carry; el error lo activa y devuelve errno positivo. ARM64 limpia X1; x64 limpia RDX al tener éxito y lo conserva ante error. Los cambios de registros de SYSCALL son explícitos. El informe usa `result` y `error=true` para errores BSD; las solicitudes sin retorno o no admitidas carecen de ambos campos. Las reglas proceden de XNU [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c) y [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c), sin incorporar código Apple.

Servicios: `exit`, `write`, `getpid`, `getppid`, `getuid`, `geteuid`, `getgid`, `getegid`, `mmap`, `mprotect`, `munmap`. PID/UID/GID valen 1000 y PPID vale 1. Los descriptores 1 y 2 capturan bytes, incluidos NUL y no UTF8; los cerrados o de solo lectura devuelven EBADF. Una copia parcial conserva los bytes leídos, pero el fallo posterior sigue siendo EFAULT. Una longitud superior a `INT_MAX` devuelve EINVAL antes de comprobar descriptor, puntero o presupuesto: [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).

Se permiten mapeos privados anónimos de datos con `flags=0x1002`, descriptor -1 y offset cero. Longitudes y sugerencias no fijas se redondean hacia arriba a la página OS. Si una sugerencia está ocupada, se busca hacia arriba antes de volver a la ubicación predeterminada. El mmap histórico sin envolver de longitud cero devuelve cero sin asignar; `MAP_UNIX03` queda excluido. Unmap/protect requieren dirección alineada. Se admiten NONE/READ/WRITE, con WRITE implicando READ. Cada página OS posee su memoria física: un unmap parcial libera presupuesto y las páginas nuevas quedan a cero. Un protect que atraviese un hueco o supere permisos máximos deja intacto todo el rango. Fuente: [servicios VM de XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

Quedan excluidos mapeos de archivo/compartidos/fijos/JIT o anónimos ejecutables, traps Mach, syscalls indirectas, hilos, señales, archivos del host/red, dyld, runtimes Objective-C/Swift y Foundation/UIKit. Su uso detiene explícitamente la ejecución. No es un OS Apple completo ni la aplicación iOS Simulator.

## Verificación

Las muestras C propias se generan con Clang y `ld64.lld`, sin SDK Apple ni binarios propietarios. Cubren cinco combinaciones plataforma/ISA, registros Mach-O malformados, páginas de 4/16 KiB y liberación parcial con presupuesto lleno. `NeverDProcessPublicTests` compara C API/CLI; `NEVERD_TEST_LIBNEVERD` y `NEVERD_TEST_DARWIN_FIXTURES` habilitan las mismas cinco combinaciones en Python.

## Archivos y descriptores explícitos

`darwin_files` ofrece a los tres perfiles un catálogo cerrado de archivos de solo lectura. `files` es obligatorio: cada entrada contiene un `path` absoluto canónico del invitado y `bytes_hex` hexadecimal. `stdin_hex` opcional aporta una entrada finita; omitirla significa desconocida y detiene lecturas no vacías, mientras una cadena vacía significa EOF. Sin catálogo open se detiene; un catálogo explícitamente vacío devuelve ENOENT. No se consultan archivos ni entrada del host.

Se añaden `open`, `read`, `pread`, `lseek`, `close`, `dup`, `dup2`, `fcntl` y las entradas nocancel de read/write/open/close/fcntl/pread. Se admiten O_RDONLY/O_CLOEXEC y F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL. Cada open tiene posición independiente; los duplicados comparten posición y conservan flags close-on-exec individuales. pread no cambia la posición. Cerrar o sustituir 0/1/2 afecta a la I/O posterior; duplicar salida conserva destino y presupuesto.

Límites: 256 archivos, 16 MiB totales de rutas/NUL/archivos/entrada, rutas menores de 1024 bytes y componentes de hasta 255. `descriptor_limit` es un techo exclusivo de 3–4096, por defecto 256; JSON conserva 64 KiB. La configuración inválida falla antes de cargar. read superior a INT_MAX devuelve EINVAL antes de consultar FD; EOF no toca el destino y un destino inválido da EFAULT. Un búfer parcialmente escribible detiene la operación antes de copiar o mover la posición. Los errores SET/CUR/END conservan la posición. Rutas relativas, apertura de directorios, escritura, stat, mapeos de archivo, seek disperso y otros fcntl siguen excluidos. Un archivo como antecesor devuelve ENOTDIR. El mismo objeto se contrasta con macOS nativo y C/CLI/Python cubren cinco combinaciones; no demuestra ejecución en un dispositivo iOS.

Verificación Release de 2026-10-05: 381 registros, 177 aprobados, 204 omitidos, ningún fallo y 51/51 requisitos ARM64 HVF ejecutados. Pasaron también siete programas macOS nativos, 35 pruebas públicas C/CLI/informes, cinco combinaciones Python y 66 pruebas del verificador. Los recuentos se solapan. Los nuevos servicios no tienen evidencia nativa Intel HVF/KVM/WHP; Intel HVF sigue sin validar y sus Actions están suspendidas. Faltan el SDK iOS y la comparación con dispositivos.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

La validación independiente exige los 51 casos nativos ARM64 o 34 x64, con `LC_MAIN` y `LC_UNIXTHREAD` en cada plataforma. Casos obligatorios ausentes/omitidos o falta de `ld64.lld` producen fallo.

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
