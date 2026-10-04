**Idiomas**: [English](../macos-hvf.md) | [简体中文](../zh-CN/macos-hvf.md) | [繁體中文](../zh-TW/macos-hvf.md) | [日本語](../ja/macos-hvf.md) | [한국어](../ko/macos-hvf.md) | [Français](../fr/macos-hvf.md) | [Deutsch](../de/macos-hvf.md) | [Español](macos-hvf.md) | [Italiano](../it/macos-hvf.md) | [Русский](../ru/macos-hvf.md) | [العربية](../ar/macos-hvf.md)

<!-- i18n-source: 48ad654328e8b7f59295e47eee52da3bbc88ab1d81beb58efe1872a8273c9c45 -->

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

ARM64 ejecuta toda la secuencia inmutable de mantenimiento TLB/I-cache en una entrada nativa, con el paso a paso desactivado. Un HVC #1 específico debe coincidir exactamente con el PC de retorno, syndrome, PSTATE y ESR_EL1 sin cambios antes de ejecutar una sola instrucción admitida del huésped. `PSTATE.D` no enmascara las excepciones de depuración dirigidas a EL2. Se captura todo el estado escalar, TLS y FP/SIMD. Intel negocia controles VMCS, usa monitor trap, invalida TLB y transfiere paquetes XSAVE completos. RIP/RFLAGS pasan directamente por VMCS, también al recrear la vCPU. CR0/CR4 respetan máscaras del framework y bits fijos del hardware. La capa ISA completa las salidas autenticadas de lectura CR8; otros accesos a registros de control fallan. Cada vCPU inicializa un `IA32_KERNEL_GS_BASE` privado y gestionado; se interceptan accesos MSR invitados y se rechazan MSR/SWAPGS no admitidos.

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

Se requieren 15 pruebas de transporte ARM64 o 10 Intel, y 23 o 18 comprobaciones obligatorias en la validación CPU completa. Se cubren estado completo, privilegios, permisos, cruces de página, alias, cambios de CPU, reversión, cancelación y reintentos. Intel prueba CR8 antes de la compilación grande. Los artefactos conservan inventario, revisión, anfitrión, resultados e intentos separados. [GitHub](https://docs.github.com/en/actions/concepts/runners/github-hosted-runners) considera experimental la virtualización anidada; conviene mantener un Mac nativo dedicado.

En Intel alojado, `--execution-methods` ejecuta secuencialmente cada método GoogleTest con todos los parámetros CTest, flags, entorno y directorio originales. Rechaza propiedades desconocidas. Cada método tiene un plazo agregado de hasta 120 segundos, sin un plazo independiente por parámetro; después se retira el grupo de procesos con espera acotada. Se guardan XML original, nombres y estados de salida. Un timeout o XML incompleto produce fallo parcial; los casos nativos obligatorios ausentes u omitidos impiden aprobar. Los runners propios mantienen procesos y plazos CTest por caso.

## Evidencia y límites

Las mediciones históricas siguientes preceden a la optimización; los resultados del 2026-10-04 están al final.

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

`runner=hosted-intel` sigue siendo el valor predeterminado del flujo de recuperación. `runner=self-hosted` usa las etiquetas existentes `[self-hosted, macOS, X64, hvf]` para comparar hosts Intel. Ambas opciones exigen x86-64 nativo y la comprobación VM/vCPU. Las imágenes alojadas instalan Ninja; los hosts propios deben disponer de `cmake`, `ninja`, `python3`, `clang` y `codesign`. El aislamiento de fuentes, los límites de repetición, la revisión de pruebas y el tratamiento de fallos son idénticos. Esta opción necesita un runner compatible disponible; no proporciona hardware.

El 2026-10-03, el código limpio `e4a8169e69eb668ed3795efe4bd5f42cf4f287c2` superó la validación local nativa ARM64 Release, con HVF activado, Unicorn desactivado y `NEVERD_LLVM_PREBUILT=ON`. El perfil CPU completo cotejó 20 objetivos, 520 métodos y 7,125 resultados: 882 aprobados, 6,243 omitidos, cero fallos y los 16 casos nativos obligatorios aprobados. El perfil Darwin independiente cotejó 32 métodos y 286 resultados: 65 aprobados, 221 omitidos, cero fallos y los 39 casos nativos obligatorios aprobados. El bucle original de recuperación en un único proceso también completó 1,000 repeticiones consecutivas, seguidas de los 12 casos de transporte nativo aprobados. Se revisaron por separado los registros originales, XML, estados de procesos y definiciones de las fuentes correspondientes. Los totales CPU y Darwin se solapan y no deben sumarse. Estos resultados no demuestran la validación Intel completa ni una compilación con el SDK iOS.

La [ejecución independiente de recuperación Intel](https://github.com/NeverSight/NeverD/actions/runs/37159724276), con el código `bd284894c60427cf4e6a60e661a1fa0df8a070f5`, terminó el 2026-10-03 a las 23:41:30 UTC con una anotación de GitHub que informó de la pérdida de comunicación con el runner alojado. Se conservaron el plan y once artifacts de progreso; sus descargas se verificaron con los SHA-256 del servidor. La última instantánea demuestra 252 repeticiones completas y el inicio de la 253, pero no localiza el fallo final. No hubo resultado final ni registro de retirada del proceso, y la API del registro completo devolvió 404. Por tanto, las 1,000 repeticiones solicitadas siguen sin verificar. El repositorio aún no tenía runners propios. Son pruebas de fallo conservadas, no una corrección de estabilidad ni una validación Intel completa.

El [workflow del repositorio personal](https://github.com/gmh5225/test_mac_intel) permite validar en un runner Intel alojado sin disponer de un Mac Intel local. Fija por separado las revisiones del diagnóstico y del código probado, y registra la del workflow. Evalúe por separado la espera y la estabilidad. Antes de notificar un fallo, el diagnóstico conserva el PID, el proceso padre, el ejecutable, el código de salida, la señal y el motivo de terminación de cada proceso de carga. Tras un fallo de la Action, el recolector espera hasta 20 segundos y solo copia informes macOS IPS cuyo PID, padre, nombre y hora de ejecución coincidan. La ausencia de informes queda explícita. Un fallo de carga sigue deteniendo la ejecución nativa, pero no demuestra un fallo del hipervisor ni una prueba superada.

El 2026-10-04, los primeros trabajos de [macOS 26](https://github.com/gmh5225/test_mac_intel/actions/runs/37175472452) y [macOS 15](https://github.com/gmh5225/test_mac_intel/actions/runs/37175511460) del repositorio personal comenzaron 8 y 5 segundos después de crearse. Fallaron tras la terminación anormal de subprocesos de carga; el controlador canceló y terminó los procesos nativos después de 3 y 105 repeticiones completas, respectivamente. Las pruebas finales se conservaron y verificaron de forma independiente. Un [control exclusivo de carga](https://github.com/gmh5225/test_mac_intel/actions/runs/37177383621) superó las 16 cargas; se verificaron los resúmenes del servidor de sus 17 artefactos, con `native_execution=false`. Estas observaciones distinguen un fallo de carga de una aserción nativa, pero no determinan su causa ni validan las 1.000 repeticiones solicitadas. El control anterior del repositorio de la organización también comenzó en 5 segundos, por lo que estas muestras no demuestran una mejora del tiempo de espera.

Las ejecuciones posteriores de [macOS 26](https://github.com/gmh5225/test_mac_intel/actions/runs/37176652027) y [macOS 15](https://github.com/gmh5225/test_mac_intel/actions/runs/37176990174) terminaron con fallo el 2026-10-04; GitHub indicó explícitamente la pérdida de comunicación con el runner alojado. Se verificaron sus 10 y 16 artefactos. Los últimos registros conservados prueban 175 y 326 repeticiones completas consecutivas, respectivamente, seguidas del inicio de otra; no localizan el fallo final. Faltan los resultados nativos finales y las pruebas de terminación de procesos, y ambos registros completos devuelven HTTP 404. No se solicitó cancelación manual. Las 1.000 repeticiones siguen sin validarse en Intel. [Los fragmentos originales y el manifiesto de ejecuciones y resúmenes](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-04) permanecen disponibles tras caducar los artefactos de Actions.

## Optimización del mantenimiento ARM64 (2026-10-04)

Las cinco operaciones TLB/I-cache se ejecutan juntas en un bloque privado inmutable terminado en HVC #1. El transporte verifica el syndrome completo, el PC de retorno, PSTATE y ESR_EL1 antes de ejecutar exactamente una instrucción admitida del huésped. Solo se desactiva el paso a paso durante el mantenimiento; se conservan todas las barreras, la captura completa y el plazo común de cancelación. No se usa ERET, porque el retorno de excepción deja ESR_EL1 arquitectónicamente UNKNOWN. [Arm](https://documentation-service.arm.com/static/649ae5b238511951cb799288).

En M4 Max con macOS 15.6.1, Release, Apple Clang 17 y LLVM precompilado, la instrumentación independiente contó 252600 entradas antes y 84200 después para las mismas 42044 instrucciones del huésped y 56 de sondeo inicial: de seis entradas a dos. Los tiempos instrumentados quedan excluidos. Tres pruebas de fallos/recuperación superaron 1000 iteraciones consecutivas cada una en un proceso; la cancelación exige una escritura nativa como testigo y un reintento correcto tras modificar el código huésped.

La integración limpia [389bebfdd](https://github.com/NeverSight/NeverD/commit/389bebfdda31a0db19facc7ab8ca5461a8c8c1bc) superó el inventario CPU completo: 2546 éxitos, 4710 omitidos, cero fallos y los 23 casos nativos obligatorios aprobados. Darwin independiente: 130 éxitos, 156 omitidos, cero fallos y los 39 casos nativos obligatorios aprobados. Los inventarios se solapan y no deben sumarse. No se demuestra una validación con SDK ni dispositivos iOS.

Quince pares de procesos alternan el orden, con un calentamiento por carga y sin compilaciones ni pruebas de esta tarea en paralelo. La carga del host compartido fue 26.7–33.0; en la comparación por software, 28.8–32.6. Las celdas muestran mediana [mínimo–máximo] en milisegundos. La aceleración es la mediana de las razones de tiempo emparejadas; el intervalo bootstrap percentil del 95% usa 10000 remuestreos y semilla 20261004. Las colas largas y solo quince pares limitan la generalización.

`4b54908b9` → `056090929` / Release / Apple Clang 17 / LLVM 23 prebuilt / Unicorn `df88be772`.

| Carga | Antes ms [mín–máx] | Después ms [mín–máx] | Aceleración emparejada | Intervalo 95% | Pares más rápidos |
| --- | --- | --- | --- | --- | --- |
| `initialization` | 1.292 [0.804–6.793] | 1.118 [0.797–29.077] | 0.998× | 0.809–1.154 | 7/15 |
| `integer` | 125.426 [92.700–587.385] | 70.229 [54.067–895.051] | 1.595× | 1.373–1.884 | 13/15 |
| `branch` | 251.909 [168.279–974.846] | 155.678 [106.833–1566.522] | 1.495× | 1.055–1.687 | 12/15 |
| `memory` | 231.574 [140.137–1511.815] | 143.496 [96.508–1209.777] | 1.535× | 1.276–1.984 | 13/15 |
| `tls_call` | 347.850 [203.188–2536.731] | 212.467 [136.304–1441.830] | 1.552× | 1.428–2.912 | 14/15 |
| `two_cpu_switch` | 34.629 [25.019–416.105] | 26.684 [18.926–60.159] | 1.389× | 1.283–1.515 | 13/15 |

### Unicorn / HVF optimizado (más de 1 favorece HVF)

| Carga | Aceleración emparejada | Intervalo 95% |
| --- | --- | --- |
| `initialization` | 0.232× | 0.195–0.325 |
| `integer` | 1.878× | 1.437–2.896 |
| `branch` | 2.681× | 1.824–3.317 |
| `memory` | 2.967× | 2.224–3.215 |
| `tls_call` | 2.339× | 0.977–3.257 |
| `two_cpu_switch` | 0.831× | 0.541–1.612 |

Son mediciones de cargas ARM64 verificadas en un anfitrión compartido, no una clasificación universal. Las desconexiones y caídas de procesos de los runners Intel siguen pendientes de investigación independiente. Los perfiles CPU limitados de macOS/iOS no equivalen a emular por completo un sistema o dispositivo Apple.

[Reproducción: `neverd-cpu-bench`, `benchmark_cpu.py`](../testing.md#reproduce-checked-arm64-cpu-measurements).

## Resultados de experimentos Intel aislados (2026-10-04)

El candidato `909672ca6`, con plazo finito en el hilo propietario, sigue siendo experimental. La prueba original de 1000 recuperaciones perdió la comunicación con el runner en macOS 15 y 26; los últimos prefijos conservados muestran 277/278 y 250/251 iteraciones completadas/iniciadas. Las instrucciones ordinarias del mismo candidato, sin cancelación explícita, también perdieron la comunicación en macOS 15 (576/577). GitHub confirmó las tres pérdidas. Faltan el resultado nativo final y el registro de recogida del proceso; los prefijos no localizan el fallo final. [Pruebas originales](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-04-boundaries).

El programa independiente y sin modificar `hvf-edge-cases`, revisión `f150b38`, completó su invitado en modo real y 100000 intentos de llamada de interrupción aleatoria en ambas imágenes, en unos 362 y 398 segundos. Ambos terminaron con código cero y se recogieron sus procesos hijos; hashes, progreso y finalización del invitado se verificaron de forma independiente. Los intentos anteriores de 300 segundos alcanzaron el plazo del observador y fueron terminados y recogidos, sin pérdida del runner. El programa ignora los códigos de retorno de las interrupciones aleatorias: el número de intentos no demuestra entregas individuales. No sustituye la aceptación de NeverD ni una comparación de rendimiento. [Código, registros y auditoría](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-04-upstream).

Estos resultados acotan la investigación, pero no establecen una corrección de producción. Conservar VM e hilos Executor es una comparación diagnóstica, no un cambio de ciclo de vida aceptado. Intel aún debe superar las 1000 recuperaciones originales, el inventario CPU completo y la prueba Darwin independiente con el mismo candidato limpio.

Ambos controles de ciclo de vida superaron ya 1000/1000 iteraciones en las dos imágenes Intel: recreación de vCPU (37195529270, 37195554929) y de VM más vCPU conservando el mismo owner (37196504453, 37196535787). El segundo acredita 8000 eventos nativos ordenados y la destrucción final de la generación 1001 por ejecución; se verificaron las 24/27 huellas, las salidas nativas y del controlador a cero y la recogida de los procesos. Esto acota la comparación con la sustitución completa de Executor, pero no identifica la causa ni demuestra una corrección. El siguiente diagnóstico conservará la VM y sustituirá vCPU e hilo propietario. Siguen pendientes la recuperación original y la validación CPU/Darwin completa.

El cambio de owner no completó 1000 rondas: el uploader sufrió SIGTRAP al analizar cadenas V8 en macOS 15 (37198629082) y SIGSEGV al buscar ámbitos V8 en macOS 26 (37198630903). El controlador canceló y recogió los procesos nativos. Los registros conservan 24/25 y 467/468 rondas completadas/iniciadas, sin aserción nativa ni resultado final. Ambos runners siguieron accesibles y entregaron informes coincidentes. Son interrupciones del observador, no aprobaciones nativas ni pérdidas confirmadas del runner. La causa sigue abierta; se compararán controles de solo subida antes de cambiar el backend.

Los controles sintéticos (37199672430, 37199674303) y las repeticiones exactas de las instantáneas fallidas (37200549588, 37200551385) completaron 16/16 cargas cada uno, sin VM ni invitado. Se verificaron independientemente las 17 huellas por ejecución, salidas cero, ausencia de señales o cancelación y los bytes del commit fijado `e02e8c6`. Los cuatro comparten Node 24.19.0, V8 13.6.233.17-node.51 y el SHA256 del ejecutable. Su UUID Intel coincide con los informes anteriores, sin hash del ejecutable original. El contenido por sí solo no reprodujo la caída; siguen pendientes el efecto de la ejecución nativa simultánea, la causa y la aceptación Intel. [Evidencia verificada](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-04-upload-controls).

La comparación con plazo finito y conservación de toda la sesión Executor/VM/owner superó 1000/1000 iteraciones en macOS 15 (37203540596). En macOS 26 (37203542459) se completaron 772; la 773 falló una aserción al agotarse los 50 ms durante la admisión, antes del callback nativo. El proceso nativo terminó con SIGTRAP por `--gtest_break_on_failure` y fue recogido; el runner siguió accesible. Se verificaron las 43/34 huellas. Es una aserción del test, distinta de un fallo del uploader o una pérdida del runner, y no aísla la vida de la VM como causa. La corrección conserva el plazo cooperativo de 2 s para la admisión e inicia los 50 ms nativos tras preparar el owner, limitados por el plazo externo. Permanecen todos los modos de interrupción, comprobaciones de retorno real y reintento, y el límite independiente de 600 s. Los próximos controladores conservarán antes del guest el SHA256 del archivo Node real, su UUID Mach-O nativo y las versiones Node/V8. Esto identifica el archivo en disco al recogerlo, no prueba la integridad de la memoria ni aporta los hashes ausentes en los fallos anteriores.

El candidato `faad8299b`, que conserva el hilo owner, tampoco superó el recovery1000 original con un Executor nuevo en cada ronda en ambas imágenes. GitHub confirmó la pérdida de los runners macOS 15 (37202068724) y macOS 26 (37202070988). Los 15/12 artefactos verificados conservan 302/303 y 225/226 rondas completadas/iniciadas, sin resultado nativo final ni registro de recogida del proceso. Mantener el hilo no bastó en estas ejecuciones. El candidato se integró en `dev` mediante la [PR #444](https://github.com/NeverSight/NeverD/pull/444) el 2026-10-04 a las 13:16:49 UTC. La integración no demuestra una corrección de ejecución verificada ni una validación CPU/Darwin completa; los fragmentos guardados no identifican el punto del fallo.

El test corregido `7dd7342ec` superó después 1000/1000 recuperaciones consecutivas con sesión conservada en macOS 15 (37204841332) y macOS 26 (37204843517). Se verificaron independientemente 43 artefactos por ejecución, salidas nativa/controlador a cero, recogida de procesos y registros Node idénticos antes/después. Valida la corrección del presupuesto de admisión en este diagnóstico, sin resolver la pérdida del runner con nuevos Executor ni completar la aceptación Intel.

La recuperación corregida con recreación solo del vCPU por ronda (fuente `7dd7342ec`, controlador `31afddad3`, workflow `10bf50753`) superó las 1000 rondas en macOS 15 ([37215096822](https://github.com/gmh5225/test_mac_intel/actions/runs/37215096822), 43 artefactos verificados). Las salidas nativa y del controlador fueron 0 y el hijo se recogió. Se acredita una generación de VM y 1001 generaciones en los límites vCPU; los recuentos totales/de vCPU ejecutados siguen desconocidos. En macOS 26 ([37215098793](https://github.com/gmh5225/test_mac_intel/actions/runs/37215098793), 19 artefactos), el uploader progress-016 recibió SIGSEGV. Su IPS coincide en PID 32519, padre 29104, hora de captura y UUID de Node. El prefijo nativo demuestra 403 rondas completadas / 404 iniciadas, sin fallo de aserción. El controlador canceló y recogió el proceso nativo (SIGKILL), sin resultado nativo final. Es evidencia incompleta, ni pérdida del runner ni aprobación. El SHA-256 de Node 24.19.0 registrado antes de ejecutar fue `1052eb9c7d6c60a79b968e09f75af55a73462b0f6dff0964336d63b5e13eb63c`; identifica el archivo en disco, no demuestra que la memoria del proceso no cambiara. Estos resultados de una fuente fija no validan integraciones posteriores en dev.

La recuperación con recreación de VM/vCPU por ronda también terminó con pérdida de runner confirmada en ambas imágenes: [macOS 15 / 37213675739](https://github.com/gmh5225/test_mac_intel/actions/runs/37213675739), [macOS 26 / 37213681083](https://github.com/gmh5225/test_mac_intel/actions/runs/37213681083). Fuente `7dd7342ec`, controlador `4c702d35a`, workflow `fad0eadf2`. Los 23/26 artefactos verificados conservan prefijos continuos de 502/503 y 575/576 rondas completadas/iniciadas, sin resultado nativo final ni registro de recogida del proceso. GitHub confirma la pérdida de comunicación, no la causa. Los éxitos al recrear VM con instrucciones ordinarias no cubren esta recuperación; el experimento dependiente de reinicio de toda la sesión sigue sin iniciarse.

Los controles `jitless` ([37217523688](https://github.com/gmh5225/test_mac_intel/actions/runs/37217523688) / [37217525863](https://github.com/gmh5225/test_mac_intel/actions/runs/37217525863), macOS 15/26) fallaron antes de iniciar recuperación: el parser HTTP del uploader oficial necesita WebAssembly, oculto por `--jitless`. Ambas cargas del plan salieron con código 1, sin señal. Cada ejecución tiene dos artefactos verificados; el plan se recuperó de la evidencia final, no de una carga previa independiente exitosa. No probaron recuperación nativa, sin excluir la anterior sonda de capacidades HVF. El modo opcional sustituto `js-interpreter` pasa `--no-turbofan --no-maglev --no-sparkplug` solo a los hijos de carga plan/progress, conservando WebAssembly y otras formas de generación de código. Padre, carga nativa y plazos no cambian; provenance/final siguen por defecto. Las comprobaciones locales HTTP real y uploader oficial sin credenciales pasaron, sin demostrar una corrección de estabilidad HVF.
