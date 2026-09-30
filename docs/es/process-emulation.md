**Idiomas**: [English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← Índice de documentación](README.md)

# Emulación de procesos invitados

`neverd emulate` ejecuta una imagen bajo un perfil explícito de sistema operativo invitado. El transporte CPU, el análisis de la imagen, la entrada del proceso y los servicios del SO tienen propietarios separados. Activa `NEVERD_ENABLE_CPU_EMULATION=ON`; la emulación de controladores también lo incluye.

El primer perfil, `linux-elf64-v1`, ejecuta ELF `ET_EXEC` x64/AArch64 y PIE estáticos `ET_DYN` con autorrelocación en CPL3 o EL0. Carga segmentos ELF reales, construye la pila inicial, reanuda por intervalos y atiende solicitudes explícitas de llamadas al sistema Linux. Es un modelo de proceso independiente, no una distribución Linux completa ni una promesa de ejecutar binarios libc arbitrarios. El enlace dinámico, señales, hilos, sistemas de archivos y servicios no admitidos fallan explícitamente. El perfil x64 admite algunas formas SSE/SSE2 limitadas; AArch64 sigue siendo entero. Windows, Android, Darwin y otras cargas de kernel son trabajos separados.

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

`schema_version` vale 1. El informe incluye perfil, arquitectura, backend seleccionado y motivo, `stop_reason`, `exit_status` anulable, diagnóstico, PC de entrada/actual, contadores, registros de servicios y última salida CPU tipada. Direcciones, números syscall, registros de argumentos y bits de retorno son cadenas hexadecimales **sin** `0x`; `stdout_hex`/`stderr_hex` preservan NUL y UTF-8 inválido. Un resultado syscall null significa que no hay retorno modelado (por ejemplo, exit o solicitud no admitida), no un cero exitoso.

## Semántica del perfil Linux

La política OS reutiliza las cabeceras de programa ya decodificadas por el cargador ELF. Comprueba etiquetas ABI, alineación de segmentos, tablas de cabeceras mapeadas y límites de direcciones de usuario. Un plan genérico valida extensiones, permisos, solapamientos y presupuesto antes de asignar memoria, y solo publica un espacio privado completamente preparado. Conserva prefijos/colas de páginas de archivo, pone BSS a cero, respeta permisos y reserva huecos de guarda para la pila. Rechaza diseños con páginas solapadas y cabeceras contradictorias; no adivina.

La pila inicial contiene argc/argv/envp/auxv alineados, PHDR/PHENT/PHNUM, entry, tamaño de página e identidades. PID/TID/UID/GID modelados valen 1000. `AT_RANDOM` usa los primeros 16 bytes del SHA-256 de entrada para reproducibilidad; es política determinista del modelo, no entropía criptográfica. HWCAP/HWCAP2 son cero y no existe vDSO.

Se implementan `write`, `exit`, `exit_group`, `getpid` y `gettid`, con números separados para [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) y [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h). El retorno de SYSCALL x64 aplica sus clobbers RCX/R11 además de RAX y el PC siguiente. ARM64 usa x8 para el número y x0 para el resultado. Las demás llamadas paran como `unsupported_service`; nunca ejecutan syscalls del host.

Los descriptores 1 y 2 son sumideros virtuales de bytes. `write` valida páginas de usuario legibles; devuelve el prefijo legible si una página posterior no es accesible y `EFAULT` si no puede leerse ningún byte. Un descriptor incorrecto da `EBADF`; una escritura de cero bytes con descriptor válido no accede al puntero. No se modelan la atomicidad de tuberías Linux ni archivos. El límite de salida detiene antes de publicar una escritura que lo excedería.

## Verificación

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# En una compilación con biblioteca compartida/CLI:
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

Las pruebas compilan entry assembly ELF original y C para ambas ISA. Verifican datos/BSS, metadatos iniciales, errores syscall, salida binaria, permisos, escrituras parciales, servicios no admitidos y conservación del presupuesto entre intervalos. Los backends no disponibles se marcan explícitamente como omitidos. La suite pública comprueba la C ABI/CLI y la coincidencia entre informe y código de salida. La compilación cruzada y Unicorn ARM64 no prueban KVM/WHP ARM64 nativo.

## PIE estático, TLS y verificación

El PIE estático usa un load bias determinista de al menos `0x40000000`, aumentado para respetar la alineación mayor de `PT_LOAD`. Los segmentos, el PC de entrada y `AT_PHDR`/`AT_ENTRY` comparten ese bias; los valores originales de los encabezados no cambian y `AT_BASE` permanece en cero porque no hay intérprete. El mapeo usa bytes del archivo original, no los fixups de análisis; el propio invitado debe realizar sus relocations e inicialización. El loader decodifica `PT_DYNAMIC` desde registros acotados del archivo, sin depender de secciones. Si existe, la tabla debe ser legible, terminar correctamente y tener como máximo 4096 entradas. Se rechazan `PT_INTERP` y tags externos de dependencias/filter/audit; no se proporciona linker dinámico, resolución de símbolos ni ejecución de constructores.

Las plantillas TLS estáticas `PT_TLS` se validan como hechos del loader: una plantilla, extensiones acotadas de archivo/memoria, alineación congruente y bytes iniciales legibles. El inicio invitado asigna e inicializa bloques TLS e instala el puntero de hilo; el modelo Linux no inventa un TCB/DTV específico de libc. Esto permite TLS local-exec generado por compilador en programas freestanding. TLS dinámico e hilos del SO siguen fuera del alcance.

En x64, `arch_prctl` admite `ARCH_SET_FS`, `ARCH_GET_FS`, `ARCH_SET_GS` y `ARCH_GET_GS`. Set acepta una base de rango usuario aunque no esté mapeada; toda desreferencia posterior verifica permisos. Las bases de rango kernel devuelven `EPERM` invitado y los destinos Get inválidos `EFAULT`, sin fault de CPU. Otras operaciones fallan explícitamente. ARM64 instala `TPIDR_EL0` con `MSR`; `MRS`, accesos FS/GS y restauración de contexto preservan el puntero entre quanta y entradas de backend. Esto no implementa un scheduler.

Las fixtures PIE/TLS comprueban bloques independientes alineados, BSS TLS, valores auxv reubicados y slots RELA originales a cero antes de las relocations propias del invitado. Las pruebas x64 validan errores `arch_prctl` sin perder la base anterior. Añade `NeverDThreadPointerTests` al comando de compilación/CTest de arriba; los backends ausentes siguen siendo skips explícitos.
