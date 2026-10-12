**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: 817e1308a48a9a751b9ef618b8094d4082fed99234c2b7381269f524196f5c80 -->

[← Documentation Index](README.md)

# Патчи из исходного кода и нативных объектов

NeverD компилирует замену выбранным компилятором и устанавливает её по явно указанному адресу входа исходной функции. Компилятор отвечает за семантику C/C++, NeverD — за привязку адресов, релокации, память, записи раскрутки стека и публикацию. Исправление бинарного кода и переписывание поднятого кода сохраняют свои конвейеры. Остальная программа не пересобирается.

<!-- i18n-section: compiler -->
## Выбор компилятора

Выберите `--compiler=clang`, `--compiler=gcc` или `--compiler=neverc`. Clang и обычный режим NeverC могут создавать LLVM IR; артефакт `object` сохраняет машинные инструкции производителя. CLI по умолчанию выбирает объекты для GCC. В Python SDK для GCC нужно явно указать `artifact="object"`. Незаметной подмены компилятора нет.

Замените адреса примеров подтверждёнными входами исходных функций. Объявление замены должно совпадать с исходным машинным ABI, включая соглашение о вызовах и размещение структур; адрес этого не доказывает. Для C++ нужны `--language=c++` и подходящий Clang++/G++. Используйте вход `extern "C"` или точное декорированное имя. Дополнительные функции разрешены. `--entry` и `--func` явно выбирают замену; прежний `--from-c` также учитывает адрес.

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
## Проверенные профили и ограничения

| Компилятор | Артефакт | Проверенный профиль выполнения ОС |
|---|---|---|
| Clang 22.1.6 | LLVM IR | ELF x64 C/C++; Mach-O x64/ARM64 C/C++; PE x64 C/C++ FH3 |
| Clang 22.1.6 | Нативный объект | ELF x64 C/C++ |
| GCC/G++ 13.3.0 | Нативный объект | ELF x64 C/C++ с совместимой средой Itanium |
| NeverC 3389.1.5 обычный режим | LLVM IR | ELF x64 C; Mach-O x64/ARM64 C |
| NeverC 3389.1.5 обычный режим | Нативный объект | ELF x64 C |

Это проверенные профили, а не гарантия для всех версий и параметров компилятора. `dyncode` и NeverC C++ не допускаются. GCC не создаёт LLVM IR. Нативные объекты COFF/Mach-O не допускаются; установка объектов требует ELF x64. Неизвестные релокации, TLS, конструкторы, LTO, символы common/IFUNC и неразрешённые импорты вызывают явную ошибку. JSON-квитанция содержит компилятор, аргументы, привязки и адреса установки; нативные объекты также содержат SHA-256.

Отдельная CPU-матрица проверяет четыре архитектуры (`x86`, `x64`, `ARM32`, `ARM64`) и три формата (`ELF`, `PE`, `Mach-O`): установку исходного кода либо явный отказ. Изолированное выполнение CPU не подтверждает загрузчик ОС, раскрутку C++ или произвольный ABI. Для таких выводов используйте профили выполнения выше.

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `REFUSED` | `CPU` |
| `PE` | `REFUSED` | `CPU` | `REFUSED` | `REFUSED` |
| `Mach-O` | `REFUSED` | `CPU` | `REFUSED` | `CPU` |

`CPU` означает установку и изолированное выполнение функции; `REFUSED` — явный отказ из-за отсутствующего контракта установки. ARM32 ELF требует регистрации EHABI; 32-битный Mach-O требует проверенных контрактов загрузчика, frontend и раскрутки; установка исходного кода PE сейчас требует x64.

CPU-матрица использует автономные вычисления, ветвления и вспомогательные вызовы. Она не подтверждает ссылки на данные ELF x86/ARM64 или импорты C++ ARM64; дополнительные пробы отклонили их неподдерживаемые контракты релокаций.

<!-- i18n-section: references -->
## Исходные ссылки и память

Привязки именуют внешние объявления: `function` требует подтверждённую исходную функцию или заглушку импорта; `data` — доступную для записи память; `const` — константное объявление IR. Данные объектов требуют ненулевой размер. Диапазоны не могут пересекать границы памяти или разрешать запись в RELRO. Вызов исходного тела заменяемой функции запрещён: этот профиль не сохраняет пролог в отдельном трамплине.

Новый код остаётся RX, изменяемое состояние — RW. Повторно используются только существующие импорты; слоты GOT/IAT отличаются от вызываемых заглушек. Mach-O требует свободного места для команд загрузки, например `-Wl,-headerpad,0x400`; PE — места для заголовков секций. Нехватка места вызывает ошибку. ELF `.data.rel.ro` и исходная защита CET/IBT требуют дополнительных контрактов и сейчас отклоняются.

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

Рост таблиц раскрутки Mach-O также требует свободного соседнего места в файле и виртуальном пространстве; резерв заголовка этого не обеспечивает. Проверенные образцы учитывают оба требования отдельно.

<!-- i18n-section: exceptions -->
## Исключения C++

ELF использует FDE/LSDA компилятора; Mach-O — compact unwind/DWARF. PE x64 допускает таблицы FH3 при наличии совместимой исходной среды MSVC и идентичности типов. Данные type-info могут требовать явных привязок. FH4, собственные personality, исходный SEH и активные CFG/EHCont/XFG отклоняются; исходные флаги защиты сохраняются.

Сценарии выполнения проверяют обычный возврат, внутренний catch, новое исключение в исходном вызывающем коде, исключение исходной вызываемой функции через замену, очистку RAII ровно один раз, повторный выброс, завершение noexcept и неизменённые функции. Исходная точка вызова уже должна допускать распространение исключений. Патч не восстанавливает catch, удалённые компилятором вокруг функции, доказанно не бросающей исключений.

<!-- i18n-section: publication -->
## SDK и публикация

Добавляемый C API — `neverd_patch_source` с `neverd_source_patch_options_v1`; `neverd_patch_source_report` возвращает квитанцию. Для объектного ввода передаётся длина в байтах. Прежний `neverd_patch_from_c` остаётся доступным.

Входные данные фиксируются. Кандидат создаётся рядом с целевым файлом, повторно загружается и проверяется для каждого запрошенного входа до исключительной публикации. Существующие файлы и символьные псевдонимы не перезаписываются. Одна отсутствующая привязка отменяет весь запрос. `--mode=inplace` отклоняется. Преобразования IR сеанса неприменимы к нативным объектам. Mach-O строго проверяет и обновляет подпись; подпись PE не удаляется незаметно.

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

<!-- i18n-section: verification -->
## Сборка и проверка

Выберите совместимый компилятор через `NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` (проверен Clang 22.1.6). Интегрированная сборка исправляет только копию LLVM в каталоге сборки, не меняя подмодуль или общий кеш. Ошибка x64 PLT в пакетах r3/r4 может привести к отказу IR-патча; нативные объекты обходят этот путь MC. `NEVERD_TEST_REQUIRE_FIXED_LLVM=1` требует точной исправленной генерации PLT.

`NeverDSourcePatchTests` содержит нативные случаи C/API/объектов/исключений; `NeverDSourcePatchMatrixTests` проверяет все комбинации в Unicorn. `SourcePatchPE` выполняет семь C++-сценариев при заданных `NEVERD_TEST_WINDOWS_CRT_LIB`, `NEVERD_TEST_WINDOWS_UCRT_LIB` и `NEVERD_TEST_WINDOWS_SDK_LIB`. Вне Windows также нужен `NEVERD_TEST_WINDOWS_RUNNER`. `NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` делает отсутствие входов ошибкой. Отдельный Windows workflow требует нативное выполнение и не принимает пропуски.

Linux на Mac разработчика проверяется через Docker с явным загрузчиком Linux; PE — через Wine 11. Mach-O ARM64 выполняется на самом Mac. Результаты нативного Windows CI проверяются отдельно: настроенное задание не означает успешный запуск. Исторические числа, исходные ошибки и ограничения эмуляторов приведены в [подробных контрактах и журнале проверки](../source-patching-details.md).

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

Копия LLVM также исправляет GOT-ссылки на данные Mach-O x64: таблица типизированного catch Clang отсчитывается от конца четырёхбайтового поля релокации. r3/r4 сдвигает её на четыре байта; проверка перед публикацией отклоняет такой результат. На Mac x64 работает через Rosetta 2, ARM64 — напрямую.
