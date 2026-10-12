**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: b504f377f29d587519311580a92c9cdaed9118057832a8afd81551fbd34802df -->

[← Documentation Index](README.md)

# Parches de código fuente y objetos nativos

NeverD compila una función de reemplazo con el compilador elegido y la instala en una entrada original explícita. El compilador define la semántica C/C++; NeverD gestiona direcciones, reubicaciones, almacenamiento, desenrollado y publicación. La reparación binaria y la reescritura del código elevado conservan sus propias canalizaciones. No se recompila el resto del programa.

<!-- i18n-section: compiler -->
## Elección del compilador

Elija `--compiler=clang`, `--compiler=gcc` o `--compiler=neverc`. Clang y el modo ordinario de NeverC pueden producir IR LLVM; `object` conserva las instrucciones nativas. La CLI selecciona objetos por defecto para GCC. El SDK Python requiere `artifact="object"` explícito para GCC. Nunca se cambia de compilador silenciosamente.

Sustituya las direcciones de ejemplo por entradas originales verificadas. La declaración debe coincidir con la ABI original, incluidas la convención de llamada y la disposición de estructuras; una dirección no demuestra esa coincidencia. C++ requiere `--language=c++` y el ejecutable Clang++/G++ adecuado. Use una entrada `extern "C"` o el nombre decorado exacto. Se permiten funciones auxiliares. `--entry` y `--func` seleccionan el reemplazo; el antiguo `--from-c` también respeta la dirección.

```sh
# Clang C -> LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.clang

# NeverC ordinary C -> native object
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC -> native object (CLI default for GCC)
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/gcc \
  --patch-report -o app.gcc

# Independently compiled ELF x64 object
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

<!-- i18n-section: profiles -->
## Perfiles verificados y límites

| Compilador | Artefacto | Perfil de ejecución del SO verificado |
|---|---|---|
| Clang 22.1.6 | IR LLVM | ELF x64 C/C++; Mach-O x64/ARM64 C/C++; PE x64 C/C++ FH3 |
| Clang 22.1.6 | Objeto nativo | ELF x64 C/C++ |
| GCC/G++ 13.3.0 | Objeto nativo | ELF x64 C/C++ con runtime Itanium compatible |
| NeverC 3389.1.5 modo ordinario | IR LLVM | ELF x64 C; Mach-O x64/ARM64 C |
| NeverC 3389.1.5 modo ordinario | Objeto nativo | ELF x64 C |

Estos son perfiles comprobados, no garantías para todas las versiones u opciones. `dyncode` y NeverC C++ no se admiten. GCC no produce IR LLVM. Los objetos nativos COFF/Mach-O no se admiten; la instalación de objetos exige ELF x64. Las reubicaciones desconocidas, TLS, constructores, LTO, símbolos common/IFUNC e importaciones sin resolver fallan explícitamente. El recibo JSON registra compilador, argumentos, enlaces y direcciones instaladas; los objetos también incluyen SHA-256.

La matriz CPU independiente comprueba cuatro arquitecturas (`x86`, `x64`, `ARM32`, `ARM64`) y tres formatos (`ELF`, `PE`, `Mach-O`), verificando la instalación o el rechazo explícito. La ejecución CPU aislada no certifica el cargador del SO, el desenrollado C++ ni una ABI arbitraria. Para esas conclusiones use los perfiles de ejecución anteriores.

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `REFUSED` | `CPU` |
| `PE` | `REFUSED` | `CPU` | `REFUSED` | `REFUSED` |
| `Mach-O` | `REFUSED` | `CPU` | `REFUSED` | `CPU` |

`CPU` indica instalación y ejecución aislada de funciones; `REFUSED` indica un contrato de instalación ausente. ARM32 ELF exige registro EHABI; Mach-O de 32 bits exige contratos verificados del cargador, frontend y desenrollado; PE requiere actualmente x64 para instalar fuentes.

La matriz CPU usa aritmética, ramas y funciones auxiliares autónomas. No certifica referencias de datos ELF x86/ARM64 ni importaciones C++ ARM64; las pruebas adicionales rechazaron sus contratos de reubicación no admitidos.

<!-- i18n-section: references -->
## Referencias originales y almacenamiento

Los enlaces corresponden a declaraciones externas: `function` requiere una función original o stub de importación verificado; `data` requiere almacenamiento escribible; `const` requiere una declaración IR constante. Los datos de objetos necesitan una extensión distinta de cero. Los rangos no pueden cruzar límites ni conceder escritura en RELRO. Se rechazan callbacks al cuerpo original reemplazado: este perfil no tiene un trampolín que guarde el prólogo.

El código nuevo permanece RX y el estado mutable RW. Solo se reutilizan importaciones existentes; los slots GOT/IAT y los stubs invocables se distinguen. Mach-O necesita espacio para comandos de carga, por ejemplo `-Wl,-headerpad,0x400`; PE necesita espacio para cabeceras de sección. La falta de espacio falla. ELF `.data.rel.ro` y las protecciones CET/IBT originales requieren contratos adicionales y se rechazan por ahora.

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

El crecimiento de tablas Mach-O necesita también espacio libre adyacente en el archivo y en memoria virtual; la reserva de cabeceras no lo proporciona. Las entradas verificadas distinguen ambos requisitos.

<!-- i18n-section: exceptions -->
## Excepciones C++

ELF usa FDE/LSDA del compilador; Mach-O usa compact unwind/DWARF. PE x64 admite tablas FH3 con un runtime MSVC existente compatible e identidades de tipos coherentes. Los datos type-info pueden necesitar enlaces explícitos. FH4, personalities personalizadas, código SEH y modos CFG/EHCont/XFG activos se rechazan; se conservan las protecciones originales.

Los casos ejecutados cubren retorno normal, catch interno, una excepción nueva capturada por el llamador original, excepciones desde un llamado original atravesando el reemplazo, limpieza RAII exactamente una vez, relanzamiento, terminación noexcept y funciones intactas. El llamador original debe permitir ya la propagación. Un parche no restaura catches eliminados alrededor de una función probada sin excepciones.

<!-- i18n-section: publication -->
## SDK y publicación

La API C aditiva es `neverd_patch_source` con `neverd_source_patch_options_v1`; `neverd_patch_source_report` devuelve el recibo. La entrada de objetos indica su longitud en bytes. La API anterior `neverd_patch_from_c` sigue disponible.

Se fijan las entradas y se prepara el candidato junto al destino, se vuelve a cargar y se comprueba cada entrada solicitada antes de publicarlo de forma exclusiva. No se sobrescriben archivos ni alias de enlaces simbólicos existentes. Un mapeo ausente hace fallar toda la petición. `--mode=inplace` se rechaza. Las transformaciones IR de sesión no se aplican a objetos nativos. Mach-O verifica y renueva firmas estrictamente; las firmas PE no se eliminan en silencio.

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

<!-- i18n-section: verification -->
## Compilación y verificación

Elija un compilador compatible con `NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` (Clang 22.1.6 comprobado). La compilación integrada corrige una copia LLVM en el directorio de construcción sin modificar el submódulo ni la caché compartida. El fallo PLT x64 de r3/r4 puede rechazar un parche IR; los objetos nativos evitan esa ruta MC. `NEVERD_TEST_REQUIRE_FIXED_LLVM=1` exige la generación PLT exacta corregida.

`NeverDSourcePatchTests` contiene casos nativos C/API/objetos/excepciones; `NeverDSourcePatchMatrixTests` comprueba todas las combinaciones con Unicorn. `SourcePatchPE` ejecuta siete escenarios C++ con `NEVERD_TEST_WINDOWS_CRT_LIB`, `NEVERD_TEST_WINDOWS_UCRT_LIB` y `NEVERD_TEST_WINDOWS_SDK_LIB`. Fuera de Windows requiere además `NEVERD_TEST_WINDOWS_RUNNER`. `NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` convierte las entradas ausentes en fallo. El workflow dedicado de Windows exige ejecución nativa y rechaza casos omitidos.

Las pruebas Linux en el Mac de desarrollo usan Docker y un cargador Linux explícito; PE usa Wine 11. Mach-O ARM64 se ejecuta en el Mac. Los resultados de CI Windows nativa deben comprobarse aparte: configurar un trabajo no implica que haya pasado. Las cifras históricas, fallos de referencia y límites de emuladores figuran en [los contratos detallados y el registro de validación](../source-patching-details.md).

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=ON \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDSourcePatchMatrixTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDSourcePatchMatrixTests)$' \
  --output-on-failure --parallel 4
```

La copia LLVM también corrige referencias GOT de datos Mach-O x64: la tabla catch tipada de Clang es relativa al final del campo de reubicación de cuatro bytes. r3/r4 la desplaza cuatro bytes; la comprobación previa a publicar rechaza ese resultado. En el Mac, x64 usa Rosetta 2 y ARM64 se ejecuta directamente.

La validación nativa de Windows x64 [96cf7cd59](https://github.com/NeverSight/NeverD/actions/runs/38200205814) superó las 15 pruebas específicas de PE/objetos/trampoline y las 48 pruebas de la matriz de destinos, sin omisiones. El caso FH3 ejecutó siete escenarios de C++. Esta ejecución utilizó Clang 22.1.6 con MSVC y Windows SDK nativos.
