**Языки**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: b3b4285b341fef4afed8cfe49f7fe8396536b9e63f924d14402ec7d0d3eb0b01 -->

[← Оглавление документации](README.md)

# Гостевые процессные окружения macOS и iOS

`lib/emulation/os/darwin/` моделирует автономные процессы Mach-O с явными ограничениями отдельно от CPU-транспорта хоста. Включите `NEVERD_ENABLE_CPU_EMULATION`; эмуляция драйверов Windows не нужна. `macos/` и `ios/` задают явные платформенные профили.

| Профиль | Платформа Mach-O | ISA гостя | Страница ОС |
| --- | --- | --- | --- |
| `macos-macho64-v1` | macOS | x86-64, базовая ARM64 | 4 KiB x64; 16 KiB ARM64 |
| `ios-macho64-v1` | устройство iOS | базовая ARM64 | 16 KiB |
| `ios-simulator-macho64-v1` | iOS Simulator | x86-64, базовая ARM64 | 4 KiB x64; 16 KiB ARM64 |

Образ устройства не является образом симулятора; платформа гостя не выводится из хоста. При совпадении ISA в macOS доступен [HVF](macos-hvf.md), иначе `auto` выбирает Unicorn. Гранулярность CPU остаётся 4 KiB. [API C, Python и CLI](process-emulation.md) разделяют параметры, ограничения и отчёты.

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

## Образ и запуск

`MachOExecutionImage` сохраняет исходные байты без релокационных исправлений анализа. Допускаются только тонкие little-endian образы `MH_EXECUTE` с однозначной платформой и точкой входа. Из универсального образа нужный срез следует извлечь явно.

Весь файл, включая метаданные и конечные байты, должен укладываться в `memory_limit` до разбора или копирования. Загрузчик читает ограниченный приватный снимок обычного файла; пути с NUL, короткие чтения и изменения размера отклоняются. Живое файловое отображение не удерживается. Файл и отображённая память гостя имеют отдельные пределы одинакового размера; файловый ввод-вывод хоста не имеет жёсткого временного ограничения.

Сегменты сохраняют текущие/максимальные права и нулевое заполнение. `__PAGEZERO` резервирует адреса без выделения всей области. Проверяются диапазоны файла/VM, выравнивание ОС, округлённые пересечения, принадлежность заголовка, исполняемый вход и бюджет. Сегмент заголовка должен быть читаемым и исполняемым; защитные страницы и приватный шлюз возврата зарезервированы. Последняя файловая страница сохраняет байты до её границы или EOF, последующие полные VM-страницы обнуляются: [загрузчик XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c).

`LC_MAIN` получает `argc`, `argv`, `envp` и вектор apple как четыре целочисленных аргумента. Возврат формирует младшие восемь бит кода завершения. `/usr/lib/dyld` допускается только для этого входа без импортов; dyld хоста не исполняется. Ненулевой `stacksize` отклоняется: бюджет задаёт параметр вызывающего кода `stack_size`.

`LC_UNIXTHREAD` требует ровно одну полную запись нативных общих 64-битных регистров с заполненным только PC. Стек содержит argc, завершённые argv/envp и завершённый вектор apple с `executable_path=<input filename>`. Собственные SP/флаги, другие регистры, дополнительные flavors и конфликтующие входы отклоняются. Окружение хоста и вспомогательный Linux-вектор не наследуются. См. [архитектуру dyld](https://github.com/apple-oss-distributions/dyld/blob/main/doc/dyld4.md).

Внешние dylib, импорты, rebases/chained fixups, конструкторы/деструкторы, TLS-секции, arm64e/PAC, неподдерживаемые подтипы CPU, шифрование и немоделируемые команды приводят к отказу до выполнения. PIE без fixups использует предпочтительные адреса без ASLR. Подписи являются метаданными, а не реализацией AMFI или политик entitlements.

## Службы Darwin

ARM64 использует X16, X0–X5 и `svc #0x80`; x64 — BSD-класс `0x02000000`, RAX и RDI/RSI/RDX/R10/R8/R9. Успех очищает carry, ошибка устанавливает его и возвращает положительный errno. ARM64 очищает X1; x64 очищает RDX при успехе и сохраняет при ошибке. Изменяемые SYSCALL регистры заданы явно. Отчёт обозначает BSD-ошибку через `result` и `error=true`; у невозвращающих или неподдерживаемых запросов этих полей нет. Правила основаны на XNU [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c) и [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c), без включения кода Apple.

Поддерживаются `exit`, `write`, `getpid`, `getppid`, `getuid`, `geteuid`, `getgid`, `getegid`, `mmap`, `mprotect`, `munmap`. PID/UID/GID равны 1000, PPID равен 1. Дескрипторы 1 и 2 принимают байты, включая NUL и не-UTF8; остальные возвращают EBADF. Частичная копия сохраняет прочитанные байты, но последующая ошибка остаётся EFAULT. Длина свыше `INT_MAX` даёт EINVAL до проверки дескриптора, указателя и бюджета: [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).

Разрешены приватные анонимные отображения данных: `flags=0x1002`, дескриптор -1 и смещение ноль. Длины и нефиксированные подсказки адреса округляются вверх до страницы ОС. Занятая подсказка запускает поиск вверх, затем возврат к стандартному размещению. Исторический сырой mmap нулевой длины возвращает ноль без выделения; `MAP_UNIX03` исключён. Unmap/protect требуют выровненный адрес. Поддерживаются NONE/READ/WRITE, причём WRITE подразумевает READ. Каждая страница ОС владеет физической памятью: частичный unmap освобождает бюджет, новые страницы обнуляются. Protect через дыру или сверх максимальных прав сохраняет весь диапазон неизменным. Источник: [VM-службы XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

Файловые/общие/фиксированные/JIT-отображения, исполняемая анонимная память, Mach traps, косвенные syscalls, потоки, сигналы, файлы/сеть, dyld, среды Objective-C/Swift и Foundation/UIKit исключены и явно останавливают выполнение. Это не полная ОС Apple и не приложение iOS Simulator.

## Проверка

Собственные C-образцы собираются Clang и `ld64.lld` без Apple SDK и проприетарных файлов. Они покрывают пять сочетаний платформы/ISA, некорректные записи Mach-O, страницы 4/16 KiB и частичное освобождение при полном бюджете. `NeverDProcessPublicTests` сравнивает C API/CLI; `NEVERD_TEST_LIBNEVERD` и `NEVERD_TEST_DARWIN_FIXTURES` включают те же пять сочетаний в Python.

```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

Отдельная проверка требует все 39 нативных ARM64 или 26 x64 случаев, включая `LC_MAIN` и `LC_UNIXTHREAD` на каждой платформе. Отсутствующие/пропущенные обязательные случаи или отсутствие `ld64.lld` означают провал.

```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

Для Linux выберите `kvm`, для Windows — `whp`. [Workflow Darwin](../../.github/workflows/darwin-native.yml) проверяет оба x64-транспорта без Unicorn и допускает отдельные повторные запуски. [Эталон ядра](../../.github/workflows/darwin-kernel-reference.yml) запускает программы непосредственно на обеих ISA macOS без NeverD/LLVM. `DarwinNativeCases.def` определяет режимы, коды выхода и ожидаемые байты. Только эталон хоста связывается с libSystem для настоящего входа dyld. Неверная ISA, Rosetta, тайм-аут или несовпадение вызывают ошибку; это не доказательство для ядра устройства iOS.

## Доказательства и оставшийся объём

Результаты на 2026-10-03; пересекающиеся строки не суммируются:

| Транспорт | Ревизия | Успешно | Ошибки | Пропущено | Нативные нагрузки |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 HVF | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel HVF | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| x64 KVM | `36e11ca8a` | 51 | 0 | 235 | 26/26 |
| x64 WHP | `36e11ca8a` | 51 | 0 | 235 | 26/26 |

[Запуск Intel](https://github.com/NeverSight/NeverD/actions/runs/37106013999) сверяет 286 CTest-идентификаторов и 32 процесса с исходным XML. Пропущены 65 отключённых Unicorn-случаев, 39 ARM64-гостей и 130 других хост-платформ — всего 234. У артефакта `11267489438` проверен SHA-256 `cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`. Независимо проверены и [KVM/WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703). [Эталон ядра](https://github.com/NeverSight/NeverD/actions/runs/37064795867) прошёл 4/4 программы на каждой ISA со статусом 37, точным stdout и пустым stderr.

C API/CLI с Unicorn: 138 успехов, 156 пропусков, ошибок нет. Python покрывает пять сочетаний; упакованный движок совпал с 18 ARM64-отчётами CLI, подписи 186 Mach-O-образов проверены. HVF/Unicorn OFF прошёл 38 проверок, пропустил 231 и не связался с Hypervisor.framework. Это интеграционные доказательства, не дополнительные нативные выполнения. Полный CPU Intel ещё не принят; см. [HVF](macos-hvf.md) и [подробный журнал](../darwin-emulation.md#hosted-native-verification-2026-10-03).
