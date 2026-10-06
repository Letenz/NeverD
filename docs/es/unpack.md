**Idiomas**: [English](../unpack.md) | [简体中文](../zh-CN/unpack.md) | [繁體中文](../zh-TW/unpack.md) | [日本語](../ja/unpack.md) | [한국어](../ko/unpack.md) | [Français](../fr/unpack.md) | [Deutsch](../de/unpack.md) | [Español](unpack.md) | [Italiano](../it/unpack.md) | [Русский](../ru/unpack.md) | [العربية](../ar/unpack.md)

[← Índice de documentación](README.md)

# Desempaquetado de ejecutables empaquetados

`neverd unpack` recupera el programa que un ejecutable empaquetado reconstruye en su propio espacio de direcciones. Ejecuta la entrada como un proceso invitado acotado, observa dónde el stub entrega el control al código que ha producido y escribe esa imagen como un nuevo archivo del mismo contenedor. No desvirtualiza: las funciones que un protector virtualizó siguen virtualizadas. Compile con `NEVERD_ENABLE_CPU_EMULATION=ON`.

## Entradas admitidas

El contenedor determina cómo se valida y reconstruye un archivo, el conjunto de instrucciones determina cómo se juzga una transferencia, y ambos determinan el perfil de proceso invitado. Una entrada fuera de esta tabla se rechaza por su nombre antes de que se ejecute nada.

| Contenedor (`format`) | Conjunto de instrucciones | Perfil invitado | Conocimiento del stub |
| --- | --- | --- | --- |
| PE32+ (`pe64`) | x86-64 | [`windows-pe64-v1`](process-emulation.md) | UPX |
| PE32+ (`pe64`) | ARM64 | [`windows-pe64-v1`](process-emulation.md) | ninguno; solo observación |

## Uso

```bash
neverd unpack packed.exe -o unpacked.exe
neverd unpack packed.exe -o unpacked.exe \
  --options='{"backend":"unicorn","instruction_limit":400000000,"transfer":2}'
```

El comando imprime un informe JSON. El código de salida 0 significa que la imagen se escribió, 3 que la ejecución acotada terminó antes de que se aceptara una entrada (`outcome` es `no_entry` y no se escribe nada), y 1 que la entrada, una opción o la preparación no son válidas. El informe indica el `format`, la `architecture` y el `profile` que realmente se ejecutaron. El punto de entrada en C es `neverd_unpack_json`; Python ofrece `Session.unpack`. Las opciones son las [opciones de proceso](process-emulation.md) más `transfer`. Los valores predeterminados difieren donde un stub necesita más recursos: 100000000 instrucciones, 600 segundos y 512 MiB, y `windows.defer_unmodeled` está activado.

## Cómo se establece la entrada

La generación cero es la imagen tal como la mapeó el cargador invitado. Una instrucción cuyos bytes difieren de esa imagen fue generada por el proceso. Una transferencia es la primera ejecución de código más nuevo que el que se estaba ejecutando; `transfers` enumera cada una con su RVA, su `generation` y si el puntero de pila es igual a su valor en la entrada del proceso (`stack_balanced`).

1. Una transferencia sobre la pila de entrada es la entrada del programa: el stub ha devuelto la pila que recibió. `entry_source` es `transfer`.
2. Una transferencia sobre una pila más profunda es una llamada que el stub hace al programa, como una devolución de llamada TLS. Se informa, pero no se acepta. Cuando el stub identificado nombra el destino de su salto final, la imagen se reconstruye en esa llamada, antes de que se haya ejecutado ningún código del programa, y la dirección nombrada es la entrada. `entry_source` es `stub`.
3. `transfer` selecciona explícitamente una transferencia de la lista por su posición, para protectores que desempaquetan por etapas o que llaman a su programa.

No se predice ninguna entrada a partir de la forma del código de arranque de un compilador. Una ejecución que se detiene antes informa `no_entry` junto con el `stop_reason` del proceso.

## La imagen reconstruida

Las secciones conservan sus RVA y contienen la memoria observada; cada sección tiene el acceso que sus páginas tenían en la transferencia. Una sección final `.neverd` contiene un nuevo directorio de importación sobre las celdas a través de las cuales el programa ya llama, de modo que ni el código ni los datos se mueven. `imports` enumera cada celda con su `origin`: las celdas `static` las enlazó el cargador a partir del directorio de la propia entrada, las celdas `runtime` las escribió el stub. La imagen queda fijada en su base observada: no se observaron reubicaciones del contenido generado, por lo que se elimina el directorio de reubicación y se establece `IMAGE_FILE_RELOCS_STRIPPED`. Para UPX se vuelve a designar el directorio TLS propio del programa, porque el empaquetado solo alcanza el manejador del stub.

La tabla de secciones original, la disposición del directorio de importación y la tabla de reubicación no se reconstruyen; un empaquetador no las restaura en memoria.

## Identificación

`packer.kind` nombra un protector solo a partir de evidencias presentes en el archivo. UPX requiere dos de `upx_section_names`, `upx_pack_header` (número mágico, formato, método y suma de comprobación) y `upx_entry_stub`. Una entrada no identificada se desempaqueta igualmente por observación.

## Límites

Solo se admiten ejecutables; las DLL no se ejecutan. La ejecución verificada admite una instrucción cada vez, del orden de 10^5 por segundo, por lo que un stub que necesita miles de millones de instrucciones supera cualquier presupuesto práctico. La ejecución se sigue por página de 4 KiB: el código escrito en una página que ya está ejecutando código de la misma generación no se informa como transferencia. El código del programa que se ejecuta antes de la entrada, como una devolución de llamada TLS que llama a una API no modelada, detiene la ejecución salvo que el stub declare su entrada. Los cargadores de VMProtect aún no son compatibles.

## Verificación

`NeverDUnpackTests` comprueba la identificación. `NeverDUnpackExecutionTests` desempaqueta muestras UPX incluidas en el repositorio (NRV2B, NRV2D, NRV2E, LZMA y un programa con biblioteca de ejecución de C) en Unicorn, KVM y WHP, compara cada sección con el original, ejecuta la imagen recuperada y exige bytes idénticos de todos los backends. `UnpackGeneratedTests.cpp` empaqueta un programa dentro de la prueba para x86-64 y ARM64 y comprueba, frente al archivo enlazado, un cargador que sale directamente, un cargador que primero llama al programa y un cargador de dos etapas. `NeverDUnpackPublicTests` cubre la ABI de C y la CLI. `unittests/unpack/fixtures/Makefile` regenera las muestras UPX.
