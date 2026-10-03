**Idiomas**: [English](../README.md) | [简体中文](../zh-CN/README.md) | [繁體中文](../zh-TW/README.md) | [日本語](../ja/README.md) | [한국어](../ko/README.md) | [Français](../fr/README.md) | [Deutsch](../de/README.md) | [Español](README.md) | [Italiano](../it/README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

<!-- i18n-source: 4ad5761d6370d9ae8a26c9cf015210842c3c7817d1426b5df704408d1f0d1b7a -->

[← Proyecto NeverD](project.md)

# Documentación de NeverD

La visión general, la compilación y el CLI están en el README del repositorio. Las referencias de diseño y pruebas para contribuidores se agrupan aquí.

**Soporte móvil (CLI experimental):** `neverd mobile` recupera Java desde APK, DEX y smali de [Android](android.md), y C nativo y fuentes Objective-C/Swift admitidas desde IPA, `.app` y Mach-O de [iOS](ios.md). Los informes JSON describen resultados y cobertura. Consulte primero la [introducción móvil](mobile.md), luego las guías de plataforma para comandos y límites.

Las guías en inglés se encuentran directamente en `docs/`. Las traducciones se agrupan en `ar/`, `de/`, `es/`, `fr/`, `it/`, `ja/`, `ko/`, `ru/`, `zh-CN/` y `zh-TW/`. Cada directorio contiene el índice `README.md`, la presentación `project.md`, guías temáticas, `CONTRIBUTING.md`, `ATTRIBUTION.md` y `roadmap.md`. Las imágenes compartidas permanecen en `assets/`.

La ejecución CPU separa admisión ISA, memoria invitada, transporte del motor y política del SO. `NEVERD_ENABLE_CPU_EMULATION` activa la capa CPU x64/ARM64; `NEVERD_ENABLE_DRIVER_EMULATION` añade el entorno Windows WDM/KMDF x64 acotado. `linux-elf64-v1` ejecuta procesos Linux ELF admitidos. Véase [Ejecución CPU](cpu-execution.md), [Emulación de procesos invitados](process-emulation.md) y [Emulación de controladores Windows](driver-emulation.md).

`driver-strict` / `checked-x64-v1` admite KVM en anfitriones Linux x64 compatibles y WHP en Windows x64 compatibles; `auto` elige ese transporte nativo, y las ISA diferentes usan Unicorn. Unicorn explícito y la API V1 conservan el perfil portátil. La ejecución nativa comprueba direcciones canónicas y efectos antes de entrar; hardware no disponible falla sin alternativa. Instrucciones y comportamiento OS no admitidos fallan explícitamente. La CI nativa de Windows x64 con Unicorn desactivado supera las 359 comprobaciones obligatorias: 131 de CPU, 224 resultados de controladores de 26 imágenes integradas, 46 imágenes WDK y 40 casos de escenarios en las direcciones preferidas y reubicadas, más cuatro comprobaciones de límites SEH ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). Faltan pruebas nativas ARM64; esto no establece compatibilidad universal de controladores ni de Android/Darwin.

`checked-aarch64-v1` y `checked-user-aarch64-v1` ofrecen ARM64 FP32/FP64 y SIMD fijos acotados, con estado FPCR/FPSR/vectorial completo. Linux ARM64 coincidente usa KVM, Windows ARM64 usa WHP y otra ISA usa Unicorn. Siguen pendientes las pruebas nativas ARM64; la carga de controladores Windows sigue limitada a x64.

| Documento | Descripción |
|-----------|-------------|
| [README (español)](project.md) | Resumen, inicio rápido, compilación, SDK, CLI |
| [Contribución](CONTRIBUTING.md) | Entorno, perfiles de compilación, flujo, estilo y requisitos de PR |
| [Arquitectura](architecture.md) | Rutas IR, límites de componentes, lifting estricto, profundidad de soporte y puntos de edición |
| [Pruebas](testing.md) | Suites, fixtures generadas, recorridos Unicorn y comandos incrementales |
| [Banco de trabajo de escritorio (inglés)](../gui.md) | Interfaz Qt Quick opcional, worker separado, ABI C, anotaciones y flujos MCP |
| [Validación del escritorio (inglés)](../gui-qualification.md) | Evidencia GUI medida, límites del empaquetado y validación de plataformas pendiente |
| [Recuperación de código fuente de intérpretes](interpreter-recovery.md) | Especialización experimental con `--devirtualize`, controles CLI, contrato de ejecución, evidencias y límites; propuestas de prueba para bucles anidados; presupuestos de descubrimiento explícitos y API C versionada; API de prueba exacta de código nativo a LLVM |
| [Reconstrucción de excepciones de Windows](windows-exception-reconstruction.md) | Matriz de soporte SEH/C++, contrato IR, reglas de patch nativo y validación PE |
| [Ejecución de CPU](cpu-execution.md) | Configuración, capacidades, disponibilidad de backends y resultados tipados |
| [Pruebas de bitvectors](solver.md) | Pruebas Z3 opcionales, síntesis condicionada, tests independientes y exportación |
| [Emulación de procesos invitados](process-emulation.md) | Perfil Linux ELF, inicio, servicios, límites y pruebas |
| [Entornos de procesos macOS/iOS](darwin-emulation.md) | Inicio Mach-O, plataformas de dispositivo y simulador, servicios Darwin y reglas de páginas |
| [macOS HVF](macos-hvf.md) | Ejecución por hardware con la ISA del host, permisos de firma, empaquetado y validación |
| [Emulación de controladores de Windows](driver-emulation.md) | Ciclo WDM/KMDF x64 acotado, solicitudes, escenarios de hardware, SEH, subconjuntos PnP, selección del motor y límites |
| [Auditoría y caza de seguridad de memoria](memory-safety.md) | Análisis de vida del montón y desbordamiento de copia: contrato de identidad por formato, catálogo de sumideros/fuentes, veredictos, presupuestos y esquema JSON |
| [Plugins nativos](plugins.md) | ABI de descriptor en C puro, callbacks y eventos, flujo de compilación/enlace, descubrimiento y reglas de compatibilidad |
| [Plugins de Python](python-plugins.md) | Autoría, API de sesión y eventos, aislamiento, pruebas y publicación |
| [Resumen de mobile](mobile.md) | CLI experimental de Android/iOS, entradas, salidas, informes y límites |
| [Recuperación de Java para Android](android.md) | APK (incluido multidex), DEX, archivos/directorios smali → Java; motor nativo integrado en C++20, CLI, informes JSON, límites y verificación |
| [Recuperación de fuentes iOS](ios.md) | IPA/.app/Mach-O (arm64/x86_64) → C nativo y código Objective-C/Swift compatible; disposiciones, CLI/export, cobertura JSON, límites y pruebas ejecutables |
| [Descompilación EVM](evm.md) | Entradas, hardforks, IR por fases, ABI host C/LLVM, reconstrucción Solidity y límites |
| [Descompilación de Solana SBF](sbf.md) | SBF v0-v4, LLVM IR, salida C/Rust, verificación y límites conocidos |
| [Hoja de ruta](roadmap.md) | Estado: formatos nativos, EVM y Solana SBF implementados |
| Documentación traducida | Los enlaces de idioma anteriores abren el índice y la presentación de cada idioma |

Las sondas nativas x64 y ARM64 validan ejecución completa acotada con permiso exclusivo de memoria. Los paquetes XSAVE y cachés de tablas identificados por ISA tienen una autoridad única; la evidencia de cargas nativas ARM64 sigue incompleta.

Los campos x64 nativos `FOP/FIP/FDP` siguen las reglas de guardado/restauración del host: AMD puede borrar metadatos x87 inactivos. Las pruebas de inicio los validan con una excepción pendiente sin máscara.
