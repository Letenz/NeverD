**Idiomas**: [English](../macos-hvf.md) | [简体中文](../zh-CN/macos-hvf.md) | [繁體中文](../zh-TW/macos-hvf.md) | [日本語](../ja/macos-hvf.md) | [한국어](../ko/macos-hvf.md) | [Français](../fr/macos-hvf.md) | [Deutsch](../de/macos-hvf.md) | [Español](macos-hvf.md) | [Italiano](../it/macos-hvf.md) | [Русский](../ru/macos-hvf.md) | [العربية](../ar/macos-hvf.md)

<!-- i18n-source: ff6f78bcd5a46c6e393db7da09a7b0eacd53c1553602c1f9226947d2d02c86d6 -->

[← Índice de documentación](README.md)

# Ejecución nativa de CPU en macOS (HVF)

NeverD usa [Hypervisor.framework](https://developer.apple.com/documentation/hypervisor) como equivalente de KVM y WHP en macOS. `--backend hvf` lo selecciona explícitamente; `auto` lo usa si el contrato permite ejecución nativa y coinciden las ISA: ARM64 en Apple Silicon y x86-64 en Intel. `software-cpu-v1` y la selección automática entre ISA distintas usan Unicorn. Se rechazan los ejecutables traducidos por Rosetta.

Se requiere macOS 11 o posterior y virtualización por hardware; otras dependencias pueden exigir una versión superior. La selección nativa explícita comunica su indisponibilidad sin cambiar silenciosamente de backend. [Virtualization.framework](https://developer.apple.com/documentation/virtualization) ofrece máquinas virtuales completas; NeverD necesita controlar vCPU, registros, memoria y excepciones con Hypervisor.framework.

## Compilación y firma

Active `NEVERD_ENABLE_CPU_EMULATION=ON` o la emulación de controladores. `NEVERD_EMULATION_BACKEND_HVF` está en `ON` por defecto y solo enlaza el framework en macOS. Con `OFF`, se conserva el nombre `hvf` y la API de capacidades devuelve `build_disabled`.

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

La [CPU Intel completa](https://github.com/NeverSight/NeverD/actions/runs/37106679688) sigue sin validar: a las 08:24 UTC del 2026-10-03 faltaban el resultado final y el artefacto CPU tras el plazo configurado. La compilación y las pruebas previas no sustituyen esa evidencia. La referencia del kernel macOS no valida el kernel de un dispositivo iOS.

El pequeño benchmark ARM64 equivalente midió 73.9 ms con Unicorn y 95.1 ms con HVF, aproximadamente un 29 % más de tiempo. No se ha demostrado aceleración. Una instrucción ordinaria requiere seis entradas nativas; las mediciones con gran carga anfitriona no establecen rendimiento estable. Consulte la [evidencia detallada](../macos-hvf.md#implementation-validation-2026-10-02-to-2026-10-03) y el [contrato Darwin limitado](darwin-emulation.md).

## Inventario Intel completo dividido en grupos

La ejecución completa anterior `37106679688` terminó el 2026-10-03 a las 08:35 UTC con una anotación de GitHub sobre pérdida de comunicación con el runner. No produjo resultados CPU; esto no identifica una instrucción invitada defectuosa. Intel alojado en modo `full` usa ahora cuatro grupos, con un máximo de dos trabajos simultáneos. Cada trabajo compila y comprueba primero el inventario CTest completo de los veinte objetivos. `--hvf-shard INDEX/COUNT` asigna métodos completos por objetivo e identidad, conservando todos sus parámetros juntos aunque difieran sus propiedades de ejecución. Cada grupo guarda inventarios completo y seleccionado, plan, XML original, correspondencias, estados de proceso y lista permitida de variables necesarias. Un grupo solo aporta evidencia parcial.

Un trabajo Linux independiente ejecuta `scripts/audit_hvf_shards.py` y vuelve a derivar objetivos y requisitos nativos desde las fuentes extraídas. Exige el mismo intento, el mismo commit sin cambios locales, la ISA macOS correcta y contratos normalizados coincidentes. Los cuatro grupos deben ser disjuntos y su unión igual al inventario completo; todos los procesos deben finalizar correctamente y cada requisito nativo pasar. Grupos ausentes, filtros cambiados, resúmenes contradictorios, XML incompleto o requisitos nativos omitidos provocan fallo. Cada trabajo nativo conserva transporte, recuperación, CR8 y la validación Darwin independiente. Los runners propios mantienen CTest sin dividir. Dividir la ejecución no demuestra por sí solo la aceptación Intel.
