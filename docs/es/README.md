**Idiomas**: [English](../README.md) | [简体中文](../zh-CN/README.md) | [繁體中文](../zh-TW/README.md) | [日本語](../ja/README.md) | [한국어](../ko/README.md) | [Français](../fr/README.md) | [Deutsch](../de/README.md) | [Español](README.md) | [Italiano](../it/README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

<!-- i18n-source: ca9503837987b49f4237fc47501b098aa43bb3e4fcdda4fe38c64eae4aad6b50 -->

[← Proyecto NeverD](project.md)

# Documentación de NeverD

La visión general, la compilación y el CLI están en el README del repositorio. Las referencias de diseño y pruebas para contribuidores se agrupan aquí.

**Soporte móvil (CLI experimental):** `neverd mobile` recupera Java desde APK, DEX y smali de [Android](android.md), y C nativo y fuentes Objective-C/Swift admitidas desde IPA, `.app` y Mach-O de [iOS](ios.md). Los informes JSON describen resultados y cobertura. Consulte primero la [introducción móvil (inglés)](../mobile.md), luego las guías de plataforma para comandos y límites.

Las guías en inglés se encuentran directamente en `docs/`. Las traducciones se agrupan en `ar/`, `de/`, `es/`, `fr/`, `it/`, `ja/`, `ko/`, `ru/`, `zh-CN/` y `zh-TW/`. Cada directorio contiene el índice `README.md`, la presentación `project.md`, guías temáticas, `CONTRIBUTING.md`, `ATTRIBUTION.md` y `roadmap.md`. Las imágenes compartidas permanecen en `assets/`.

| Documento | Descripción |
|-----------|-------------|
| [README (español)](project.md) | Resumen, inicio rápido, compilación, SDK, CLI |
| [Contribución](CONTRIBUTING.md) | Entorno, perfiles de compilación, flujo, estilo y requisitos de PR |
| [Arquitectura](architecture.md) | Rutas IR, límites de componentes, lifting estricto, profundidad de soporte y puntos de edición |
| [Pruebas](testing.md) | Suites, fixtures generadas, recorridos Unicorn y comandos incrementales |
| [Banco de trabajo de escritorio (inglés)](../gui.md) | Interfaz Qt Quick opcional, worker separado, ABI C, anotaciones y flujos MCP |
| [Validación del escritorio (inglés)](../gui-qualification.md) | Evidencia GUI medida, límites del empaquetado y validación de plataformas pendiente |
| [Recuperación de código fuente de intérpretes](interpreter-recovery.md) | Especialización experimental con `--devirtualize`, controles CLI, contrato de ejecución, evidencias y límites; propuestas de prueba para bucles anidados; presupuestos de descubrimiento explícitos y API C versionada |
| [Reconstrucción de excepciones de Windows](windows-exception-reconstruction.md) | Matriz de soporte SEH/C++, contrato IR, reglas de patch nativo y validación PE |
| [Ejecución de CPU](cpu-execution.md) | Configuración, capacidades, disponibilidad de backends y resultados tipados |
| [Pruebas de bitvectors](solver.md) | Pruebas Z3 opcionales, síntesis condicionada, tests independientes y exportación |
| [Emulación de procesos invitados](process-emulation.md) | Perfil Linux ELF, inicio, servicios, límites y pruebas |
| [Emulación de controladores de Windows](driver-emulation.md) | Inicialización WDM x64 acotada, solicitudes seriales buffered/direct, trabajo, temporizadores, DPC, eventos y esperas, informes y límites; vidas de controladores/objetos KMDF 1.33 no PnP y CFG x64 validado |
| [Auditoría y caza de seguridad de memoria](memory-safety.md) | Análisis de vida del montón y desbordamiento de copia: contrato de identidad por formato, catálogo de sumideros/fuentes, veredictos, presupuestos y esquema JSON |
| [Plugins nativos](plugins.md) | ABI de descriptor en C puro, callbacks y eventos, flujo de compilación/enlace, descubrimiento y reglas de compatibilidad |
| [Plugins de Python](python-plugins.md) | Autoría, API de sesión y eventos, aislamiento, pruebas y publicación |
| [Resumen de mobile (inglés)](../mobile.md) | CLI experimental de Android/iOS, entradas, salidas, informes y límites |
| [Recuperación de Java para Android](android.md) | APK (incluido multidex), DEX, archivos/directorios smali → Java; motor nativo integrado en C++20, JADX opcional, CLI, informes JSON, límites y verificación |
| [Recuperación de fuentes iOS](ios.md) | IPA/.app/Mach-O (arm64/x86_64) → C nativo y código Objective-C/Swift compatible; disposiciones, CLI/export, cobertura JSON, límites y pruebas ejecutables |
| [Descompilación EVM](evm.md) | Entradas, hardforks, IR por fases, ABI host C/LLVM, reconstrucción Solidity y límites |
| [Descompilación de Solana SBF](sbf.md) | SBF v0-v4, LLVM IR, salida C/Rust, verificación y límites conocidos |
| [Hoja de ruta](roadmap.md) | Estado: formatos nativos, EVM y Solana SBF implementados |
| Documentación traducida | Los enlaces de idioma anteriores abren el índice y la presentación de cada idioma |
