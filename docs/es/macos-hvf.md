**Idiomas**: [English](../macos-hvf.md) | [简体中文](../zh-CN/macos-hvf.md) | [繁體中文](../zh-TW/macos-hvf.md) | [日本語](../ja/macos-hvf.md) | [한국어](../ko/macos-hvf.md) | [Français](../fr/macos-hvf.md) | [Deutsch](../de/macos-hvf.md) | [Español](macos-hvf.md) | [Italiano](../it/macos-hvf.md) | [Русский](../ru/macos-hvf.md) | [العربية](../ar/macos-hvf.md)

<!-- i18n-source: 13c30cb15cacba497163d2d7027684dbbfed986b61111754b7f61993bcbaccbe -->

[← Índice de documentación](README.md)

# Ejecución nativa de CPU en macOS (HVF)

NeverD usa [Hypervisor.framework](https://developer.apple.com/documentation/hypervisor) como equivalente de KVM y WHP en macOS. `--backend hvf` lo selecciona explícitamente; `auto` lo usa si el contrato permite ejecución nativa y coinciden las ISA: ARM64 en Apple Silicon y x86-64 en Intel. `software-cpu-v1` y la selección automática entre ISA distintas usan Unicorn. Se rechazan los ejecutables traducidos por Rosetta.

Se requiere macOS 11 o posterior y virtualización por hardware; otras dependencias pueden exigir una versión superior. La selección nativa explícita comunica su indisponibilidad sin cambiar silenciosamente de backend. [Virtualization.framework](https://developer.apple.com/documentation/virtualization) ofrece máquinas virtuales completas; NeverD necesita controlar vCPU, registros, memoria y excepciones con Hypervisor.framework.

## Compilación y firma

Active `NEVERD_ENABLE_CPU_EMULATION=ON` o la emulación de controladores. `NEVERD_EMULATION_BACKEND_HVF` está en `ON` por defecto y solo enlaza el framework en macOS. Con `OFF`, se conserva el nombre `hvf` y la API de capacidades devuelve `build_disabled`.

El enlace del framework y la firma hypervisor se limitan al destino de compilación macOS (`CMAKE_SYSTEM_NAME=Darwin`). Los destinos móviles de Apple, incluido iOS, no reciben esta dependencia ni este permiso. Un perfil invitado iOS puede seguir usando HVF cuando NeverD se ejecuta en un Mac con la ISA correspondiente.

El **ejecutable del proceso** necesita [`com.apple.security.hypervisor`](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.hypervisor); firmar solo `libneverd.dylib` no basta. CMake firma CLI, worker y pruebas con `resources/macos/neverd-hypervisor.entitlements`. `NEVERD_HVF_SIGN_IDENTITY` usa `-` como firma ad hoc por defecto o una identidad existente. El empaquetado reaplica y comprueba el permiso después de reparar las dependencias Mach-O.

La aplicación que integra la biblioteca firma su propio ejecutable. NeverD no vuelve a firmar Python instalado. `cpu-capabilities --configuration=JSON --probe-host` comprueba el proceso real. El worker independiente se firma por defecto; use `NEVERD_WORKER_SIGN_HVF=OFF` solo si no necesita HVF.

## Responsabilidades y ejecución

`backends/hvf/HvfExecutor` posee una VM por proceso y una vCPU, creadas, usadas y destruidas en un hilo dedicado. Las CPU lógicas comparten el ejecutor y serializan las entradas. Antes de cambiar de CPU se retiran las dos regiones físicas del propietario anterior. Destruir una CPU inactiva no altera los mapeos de otra. Las asociaciones se separan antes de liberar RAM; los registros parciales fallidos se revierten. Si falla la retirada, se destruye la VM antes de liberar el respaldo; un fallo de destrucción irrecuperable detiene el proceso.

Los mapeos anfitriones respetan su tamaño de página, incluidos 16 KiB en Apple Silicon. Las tablas arquitectónicas y el presupuesto de CPU invitada siguen usando 4 KiB. La admisión de instrucciones, los permisos, el estado, las transacciones de memoria y los servicios OS permanecen en sus capas. El worker nativo no llama a observadores invitados ni toma los bloqueos centrales de memoria del llamador.

ARM64 ejecuta por pasos cinco instrucciones inmutables de mantenimiento TLB/I-cache y luego la instrucción admitida. `PSTATE.D` no enmascara excepciones de depuración dirigidas a EL2. Se captura todo el estado escalar, TLS y FP/SIMD. Intel negocia controles VMCS, usa monitor trap, invalida TLB y transfiere paquetes XSAVE completos. RIP/RFLAGS pasan directamente por VMCS, también al recrear la vCPU. CR0/CR4 respetan máscaras del framework y bits fijos del hardware. La capa ISA completa las salidas autenticadas de lectura CR8; otros accesos a registros de control fallan. Cada vCPU inicializa un `IA32_KERNEL_GS_BASE` privado y gestionado; se interceptan accesos MSR invitados y se rechazan MSR/SWAPGS no admitidos.

La cola conserva el token de parada y el plazo original. Preparación, mantenimiento, entrada y captura comparten el presupuesto. `RunDeadline` espera la confirmación de interrupciones antes de volver. La cancelación recrea la vCPU para aislar interrupciones tardías; las interrupciones ajenas del anfitrión Intel se reintentan en la misma generación. Fallos de captura y excepciones autenticadas conservan prioridad sobre una parada simultánea; no se publica el estado ordinario cancelado. La cancelación es cooperativa, sin garantía de tiempo real estricto.

## Validación

Use Release, CMake, Ninja, Python 3, Clang, `ld.lld`, `lld-link`, `ld64.lld` y `codesign`. Los enlazadores LLVM generan las muestras ELF, PE y Mach-O; una muestra nativa obligatoria ausente causa un fallo.

```sh
cmake -S . -B build-hvf -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_BUILD_SHARED=OFF -DNEVERD_ENABLE_PYTHON_PLUGINS=OFF \
  -DNEVERD_ENABLE_CPU_EMULATION=ON \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=OFF \
  -DNEVERD_EMULATION_BACKEND_UNICORN=OFF
python3 scripts/run_native_cpu_ci.py --build build-hvf \
  --evidence build-hvf/native-evidence --require-hvf
```

Apple Silicon puede añadir `-DNEVERD_LLVM_PREBUILT=ON`; Intel compila la revisión fijada de LLVM. El [workflow HVF](../../.github/workflows/hvf.yml) admite runners `self-hosted, macOS, ARM64/X64, hvf` y `hosted-intel` con `macos-15-intel`. Primero exige crear y destruir una VM/vCPU real. `validation=probe` no demuestra ejecución de instrucciones; `transport` solo verifica transporte; `darwin` exige todas las cargas Darwin coincidentes; `full` exige ambas validaciones completas.

Se requieren 12 pruebas de transporte ARM64 o 10 Intel, y 16 o 14 comprobaciones obligatorias en la validación CPU completa. Se cubren estado completo, privilegios, permisos, cruces de página, alias, cambios de CPU, reversión, cancelación y reintentos. Intel prueba CR8 antes de la compilación grande. Los artefactos conservan inventario, revisión, anfitrión, resultados e intentos separados. [GitHub](https://docs.github.com/en/actions/concepts/runners/github-hosted-runners) considera experimental la virtualización anidada; conviene mantener un Mac nativo dedicado.

En Intel alojado, `--execution-methods` ejecuta secuencialmente cada método GoogleTest con todos los parámetros CTest, flags, entorno y directorio originales. Rechaza propiedades desconocidas. Cada método tiene un plazo agregado de hasta 120 segundos, sin un plazo independiente por parámetro; después se retira el grupo de procesos con espera acotada. Se guardan XML original, nombres y estados de salida. Un timeout o XML incompleto produce fallo parcial; los casos nativos obligatorios ausentes u omitidos impiden aprobar. Los runners propios mantienen procesos y plazos CTest por caso.

## Evidencia y límites

Estado del 2026-10-03; las filas se solapan y no se suman:

| Alcance | Fuente | Correctas | Fallidas | Omitidas | Obligatorias nativas |
| --- | --- | ---: | ---: | ---: | ---: |
| CPU ARM64, 20 objetivos | `defc93928` | 849 | 0 | 5,993 | 16/16 |
| Darwin ARM64 | `defc93928` | 65 | 0 | 221 | 39/39 |
| Darwin Intel | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| Objetivo FP Intel completo | `3e01cda5c` | 35 | 0 | 36 | 12 casos HVF |

ARM64 cotejó 6,842 registros; Intel tiene 6,840 en los mismos 20 objetivos. También pasaron los 253 casos nativos Intel de excepciones, división y transición de estado. La [ejecución Darwin Intel](https://github.com/NeverSight/NeverD/actions/runs/37106013999) verificó 286 identidades y 32 procesos, además de diez casos de transporte, 100 recuperaciones y CR8. El recolector y las auditorías superaron 124 comprobaciones. La integración cubrió CLI, SDK, worker y firmas de 186 imágenes Mach-O; las dependencias del paquete exigen macOS 15.0.

La validación CPU Intel completa sigue pendiente. La [ejecución anterior](https://github.com/NeverSight/NeverD/actions/runs/37106679688) terminó el 2026-10-03 a las 08:35 UTC; GitHub notificó la pérdida de comunicación con el runner, sin artefacto CPU. La compilación y las pruebas previas no sustituyen el resultado completo. La referencia del kernel macOS no valida el kernel de un dispositivo iOS.

El pequeño benchmark ARM64 equivalente midió 73.9 ms con Unicorn y 95.1 ms con HVF, aproximadamente un 29 % más de tiempo. No se ha demostrado aceleración. Una instrucción ordinaria requiere seis entradas nativas; las mediciones con gran carga anfitriona no establecen rendimiento estable. Consulte la [evidencia detallada](../macos-hvf.md#implementation-validation-2026-10-02-to-2026-10-03) y el [contrato Darwin limitado](darwin-emulation.md).

## Inventario Intel completo dividido en grupos

La ejecución completa `37106679688` terminó el 2026-10-03 a las 08:35 UTC con una anotación de GitHub sobre pérdida de comunicación con el runner. Los cuatro trabajos de `37116327329` también perdieron la conexión y no produjeron XML CPU. Estos hechos no identifican una instrucción invitada defectuosa. Intel alojado en modo `full` utiliza cuatro trabajos, con un máximo de dos simultáneos. Cada uno ejecuta cuatro lotes consecutivos: dieciséis particiones en total, numeradas `job + 4 × batch`, conservando el conjunto original de cada trabajo. Cada lote compila y comprueba primero el inventario CTest completo de los veinte objetivos. `--hvf-shard INDEX/COUNT` mantiene juntos los métodos completos y todos sus parámetros, aunque difieran sus propiedades de ejecución.

Antes de ejecutar CPU, `scripts/prepare_hvf_batches.py` guarda el inventario completo, las selecciones y los planes de métodos; el workflow sube estos diagnósticos por separado. Una acción compuesta local ejecuta cuatro lotes y sube inmediatamente después de cada uno el XML original, las correspondencias, los estados de proceso y la lista permitida de variables necesarias. Un único plazo externo de 30 minutos incluye los cuatro lotes y sus subidas. Un lote fallido impide ejecutar los siguientes. Tras un fallo o vencimiento, una subida diagnóstica independiente dispone de dos minutos si el runner sigue accesible; cuando se pierde la conexión, solo quedan las pruebas ya subidas. Los planes y paquetes incompletos no cuentan como particiones aprobadas.

Un trabajo Linux independiente ejecuta `scripts/audit_hvf_shards.py` y vuelve a derivar objetivos y requisitos nativos desde las fuentes extraídas. El workflow descarga solo artefactos CPU del intento actual; la auditoría exige las dieciséis particiones del mismo commit sin cambios locales, la ISA macOS correcta y contratos normalizados coincidentes. Los resultados deben ser disjuntos y cubrir exactamente el inventario completo; todos los procesos deben finalizar correctamente y cada requisito nativo pasar. Particiones ausentes, filtros cambiados, resúmenes contradictorios, XML incompleto o requisitos omitidos provocan fallo. Cada trabajo nativo conserva transporte, recuperación, CR8 y validación Darwin independiente. Los runners propios mantienen CTest sin dividir. Los lotes no demuestran por sí solos la aceptación Intel. Cada reintento debe ejecutar de nuevo todos los trabajos nativos; no se combinan artefactos de intentos anteriores.

El primer lote CPU verificado de la [ejecución `37123148209`](https://github.com/NeverSight/NeverD/actions/runs/37123148209), código limpio `f5f29a484`, es la partición `1/16`: 476 resultados registrados en 31 procesos de métodos, con 82 aprobados, 0 fallidos y 394 omitidos; su único caso nativo obligatorio pasó. Se verificó el SHA-256 del artefacto `11274755752` y se cotejaron el XML original, los estados de salida, el inventario y el plan de métodos con el plan previo. Esta evidencia Intel parcial no completa la validación CPU íntegra.

## Última verificación nativa local

Fuentes sin cambios locales `4ce0b8247`, 2026-10-03 UTC. El inventario ARM64 completo pasó en 503 procesos de método; cada resultado XML original, contrato de ejecución y terminación del proceso hijo se verificó de forma independiente. La validación Darwin independiente también pasó. Las filas se solapan y no deben sumarse.

| Ámbito | Registrados | Pasados | Fallidos | Omitidos | Nativos obligatorios |
| --- | ---: | ---: | ---: | ---: | ---: |
| CPU, inventario completo | 7,003 | 867 | 0 | 6,136 | 16/16 |
| Darwin | 286 | 65 | 0 | 221 | 39/39 |

`build-hvf-native/hvf-current-4ce0-full-evidence/summary.json` · `build-hvf-native/hvf-current-4ce0-darwin-evidence/summary.json`

## Diagnóstico Intel independiente

El [workflow de diagnóstico Intel](../../.github/workflows/hvf-intel-diagnostic.yml), de ejecución manual, descarga por separado el controlador y el código probado. `source-ref` exige un SHA de commit completo; `shards` elige entre las dieciséis particiones originales. `first-method` empieza en cero, `method-count=0` selecciona los métodos restantes y `case-index` solo permite elegir un parámetro original si `method-count=1`. Antes de seleccionar se descubre el inventario completo de los veinte objetivos. Se conservan la compilación Release, los comandos, los parámetros y las comprobaciones nativas obligatorias originales.

`intel-image` selecciona `macos-15-intel` de forma predeterminada o `macos-26-intel` para una comparación controlada, igual que en el workflow completo. El título de la ejecución identifica la imagen elegida y la comprobación de disponibilidad sigue exigiendo un host x86-64 nativo. Cambiar de imagen incluye el sistema operativo, el SDK y las herramientas de compilación; no aísla un cambio del kernel.

La compilación dispone de 120 minutos dentro de un trabajo limitado a 180 minutos: la primera compilación completa en macOS 26 tardó 76 minutos. Cada método nativo sigue limitado a 120 segundos y la acción de diagnóstico a 30 minutos.

Antes de cada método, la acción sube su plan inmutable y una instantánea del host. Después conserva el XML original, la finalización de procesos, el estado del controlador y otra instantánea. Se registran memoria, swap, carga, disco e identificadores, estado, CPU, RSS y nombres de ejecutables de los procesos, sin argumentos ni entorno. Los errores de recopilación también quedan registrados. Un fallo de ejecución o subida detiene los métodos posteriores. Cada método mantiene el límite de 120 segundos; un host inaccesible puede impedir la limpieza y la subida final. Entonces solo quedan las pruebas ya subidas. Estos diagnósticos parciales no cumplen la validación CPU completa ni la validación Darwin independiente. El último marcador de inicio identifica un límite de ejecución, no la instrucción guest defectuosa ni la causa raíz.

El workflow completo `Native macOS HVF` también acepta `source-ref` opcional. Por defecto usa el commit del workflow; cualquier valor explícito debe ser un SHA completo. Los trabajos nativos y el auditor agregado descargan y verifican el mismo código. La auditoría coteja las pruebas con el commit probado, aunque el controlador tenga otra revisión.

Para `hosted-intel`, el workflow completo acepta `intel-image=macos-15-intel` (predeterminado) o `macos-26-intel`, incluidos en las [imágenes oficiales de runners](https://github.com/actions/runner-images). Esto permite comparar explícitamente los entornos anfitriones con el mismo `source-ref`; la imagen también cambia el sistema, el SDK y las herramientas. Los requisitos de VM/vCPU, transporte nativo, CR8, CPU completo y Darwin se mantienen. Elegir una imagen no demuestra por sí solo estabilidad ni una corrección de ejecución.

`sample-active-child=true` conserva opcionalmente una instantánea sellada cinco segundos después de observar el registro del proceso hijo nativo: un segundo de muestreo de pilas del proceso hijo nativo cuya identidad se ha verificado, hasta 1 MiB del final de su registro actual y el estado del anfitrión. El valor predeterminado es `false`. El muestreo admite como máximo 166 métodos por job para respetar el límite de artifacts; el comando tiene un plazo de veinte segundos y el informe un límite de 1 MiB. Los fallos de identificación y captura quedan registrados, incluso cuando el código de salida es cero pero no hay informe de pilas. La carga usa un directorio inmutable separado; si falla, cancela el proceso hijo nativo y hace fallar la acción. El muestreo altera la planificación y se etiqueta como evidencia parcial instrumentada. No reinicia el temporizador original del método ni sustituye la validación completa.

El workflow completo también admite `recovery-repetitions=1000` para investigar de forma específica las interrupciones y la recuperación; el valor predeterminado sigue siendo `100`. Esta opción amplía el presupuesto del paso de repetición de tres a diez minutos. Cada prueba nativa conserva su plazo original, sus aserciones y la detención ante el primer fallo. El título de la ejecución identifica el mayor número de repeticiones. Estas no sustituyen las validaciones completas de CPU o Darwin.

El [flujo independiente de diagnóstico de recuperación Intel](../../.github/workflows/hvf-intel-recovery.yml) compila solo `NeverDHvfTests` y ejecuta el filtro original `HvfExecutor.Native*` en un único proceso con `repetitions=100` o `1000`. Exige un `source-ref` exacto, una comprobación nativa Intel de VM/vCPU, Release, HVF activado y Unicorn desactivado. El controlador, el código probado y el cargador oficial de artifacts se obtienen por separado.

La acción sube el plan antes de ejecutar. Durante la ejecución conserva un marcador inicial del proceso, el progreso tras al menos 25 repeticiones completas adicionales y, como máximo una vez, una instantánea de estancamiento si hay salida nueva sin guardar. Agrupa el progreso durante las subidas, limita los artifacts de progreso a 42 y cada copia del registro a 1 MiB, y solo sube directorios sellados. Las subidas no pausan las repeticiones ni reinician sus temporizadores, pero afectan a la planificación del host; son pruebas parciales con instrumentación.

El éxito exige iteraciones consecutivas desde 1 hasta el total solicitado, registros RUN/OK/PASSED emparejados del test nativo exacto en cada iteración, ningún fallo ni omisión, salida cero y retirada confirmada del proceso. El XML sobrescrito en cada repetición no lo demuestra. La ejecución nativa tiene un límite total de tres o diez minutos, más 30 segundos para limpiar el controlador; la acción dispone de 20 minutos. Un fallo de subida cancela la ejecución y la cancelación termina tanto el grupo de procesos nativo como el cargador. Las pruebas finales se suben si el host sigue accesible. Las instantáneas incompletas o truncadas nunca satisfacen la validación completa de CPU o Darwin.
