# Контракты выпущенных ядер Android GKI

NeverD отдаёт приоритет выпущенным веткам Android GKI 5.10–6.18 перед другими вариантами Linux. Ветка явно выбирается в запросе процесса:

```json
{"linux_kernel":{"gki":"android17-6.18"},"linux_files":{"files":[],"descriptor_limit":16}}
```

Контракт API 28 нативного профиля Android описывает импорты Bionic и не выбирает ядро. GKI управляет реализованными контрактами `pidfd_open`, векторного вывода и кодированных CPU-часов процессов. Он не сертифицирует и не запускает полное ядро, не выводит устройства, пространства имён, права или список процессов. Неподдерживаемые службы явно останавливаются. См. [официальную политику GKI](https://source.android.com/docs/core/architecture/kernel/gki-releases).

## Закреплённые исходники

`LinuxGKIKernels.def` использует эти официальные теги `r1`, проверенные 2026-10-07. Неизменяемые коммиты содержат `kernel/pid.c`, `include/uapi/linux/pidfd.h`, `arch/arm64/configs/gki_defconfig`, `kernel/fork.c`, `lib/iov_iter.c` и `fs/read_write.c`. Флаги взяты из UAPI и проверки вызовов, а не уровня Android API или ядра хоста. Закреплённые исходники CPU-часов приведены ниже.

| Запрошенная ветка | Тег выпуска | Закреплённый коммит | Допустимые флаги | Импорт iovec |
| --- | --- | --- | --- | --- |
| `android12-5.10` | `android12-5.10-2026-07_r1` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/pid.c) | `PIDFD_NONBLOCK` (`0x800`) | [Сначала копировать все метаданные](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/lib/iov_iter.c) |
| `android13-5.10` | `android13-5.10-2026-07_r1` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/pid.c) | `PIDFD_NONBLOCK` | [Сначала копировать все метаданные](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/lib/iov_iter.c) |
| `android13-5.15` | `android13-5.15-2026-09_r1` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/pid.c) | `PIDFD_NONBLOCK` | [Сначала копировать все метаданные](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/lib/iov_iter.c) |
| `android14-5.15` | `android14-5.15-2026-07_r1` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/pid.c) | `PIDFD_NONBLOCK` | [Сначала копировать все метаданные](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/lib/iov_iter.c) |
| `android14-6.1` | `android14-6.1-2026-09_r1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/pid.c) | `PIDFD_NONBLOCK` | [Сначала копировать все метаданные](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/lib/iov_iter.c) |
| `android15-6.6` | `android15-6.6-2026-07_r1` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/pid.c) | `PIDFD_NONBLOCK` | [Путь одного буфера](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/lib/iov_iter.c) |
| `android16-6.12` | `android16-6.12-2026-09_r1` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/pid.c) | `PIDFD_NONBLOCK` и `PIDFD_THREAD` (`0x80`) | [Путь одного буфера](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/lib/iov_iter.c) |
| `android17-6.18` | `android17-6.18-2026-09_r1` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/pid.c) | `PIDFD_NONBLOCK` и `PIDFD_THREAD` | [Путь одного буфера](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/lib/iov_iter.c) |

Эти ревизии задают контракт. Новые выпуски и переносы исправлений требуют проверки исходников и регрессий. GKI вместе с явным наблюдением отсутствия `pidfd_open` отклоняется до загрузки.

## Реализованная часть процессных дескрипторов

Трапы x64/AArch64 и Bionic `syscall` разделяют `LinuxServices` и таблицу дескрипторов нагрузки. Используются младшие 32 бита PID/флагов; неизвестные флаги и неположительные знаковые PID дают `EINVAL` до выделения дескриптора. Необязательный массив `tasks` задаёт фиксированный замкнутый каталог других живых гостевых задач:

```json
{"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true},{"id":3000,"group_leader":false}]},"linux_files":{"files":[],"descriptor_limit":16}}
```

Текущий лидер группы PID 1000 включён неявно даже при пустом массиве. Без каталога поиск внешних целей остаётся неподдерживаемым; допустимый положительный PID вне объявленного каталога даёт `ESRCH` до выделения. Живая задача, не являющаяся лидером, без `PIDFD_THREAD` даёт `EINVAL` в 5.10–6.12 и `ENOENT` в [`pidfd_prepare` версии 6.18](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/fork.c). Разрешённый флаг потока позволяет 6.12/6.18 открывать объявленные задачи, не являющиеся лидерами. Проверка флагов всегда предшествует поиску.

Каждая запись требует целого `id` в 1..2147483647 и логического `group_leader`; максимум 4096 записей. Повторы, лишние поля, PID 1000 как не-лидер и каталог без GKI отклоняются. Наблюдения приоритетов должны ссылаться на каталог или текущий процесс. Фиксированный каталог несовместим с кооперативными потоками Android (`thread_limit > 1`); создание, удаление, полномочия и преобразование пространств имён требуют отдельного управления временем жизни.

Требуется `linux_files`: обычные файлы и pidfd разделяют владельца и предел. Выделяется минимальный свободный номер; исчерпание даёт `EMFILE`, `close` освобождает номер, повторное закрытие даёт `EBADF`. Номер закрытого стандартного потока можно использовать повторно. pidfd, файловая система и процессы хоста не запрашиваются.

На допустимом pidfd `read`/`write` возвращают `EINVAL` до обращения к данным; `lseek` проверяет начало отсчёта и даёт `ESPIPE`. `writev` сначала импортирует метаданные и проверяет пользовательские диапазоны: `EFAULT` может предшествовать `EINVAL` отсутствующей записи. Данные не читаются и не захватываются. stdout/stderr используют тот же версионный импортёр; см. [порядок VFS](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/fs/read_write.c).

5.10/5.15/6.1 сначала копируют весь iovec: ранняя отрицательная длина с последующими недоступными метаданными даёт `EFAULT`. Исходные диапазоны проверяются до ограничения передачи, даже для одного вектора. 6.6/6.12/6.18 проверяют последовательно и дают здесь `EINVAL`; один буфер сначала ограничивается, несколько сохраняют проверку всех исходных диапазонов. Основа — `copy_iovec_from_user`, `__import_iovec`, `import_ubuf`. Без GKI остаётся прежняя политика одного буфера без предположений о версии.

Bionic преобразует отрицательные сырые ошибки в `-1` и локальный для потока `errno`; успех сохраняет `errno`. Метаданных для `fstat` нет. Опрос, уведомления о завершении, сигналы через pidfd, `pidfd_getfd`, `fcntl`, ioctl pidfs и ненаблюдаемые задачи не поддерживаются. Планирование и время жизни не выводятся из pidfd.

## Реализованные CPU-часы процессов

При явном выборе GKI `clock_gettime` принимает отрицательные кодированные идентификаторы PROF, VIRT и SCHED, интерпретируя младшие 32 бита со знаком. PID и вид определяют явный образец в `linux_time`. Текущий процесс задан неявно; остальные до задания образца должны быть объявлены живыми лидерами групп в замкнутом каталоге задач.

```json
{"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true}]},"linux_time":{"advance_on_idle":true,"clocks":[{"id":1,"seconds":10,"nanoseconds":0},{"id":2,"seconds":3,"nanoseconds":4},{"id":-16006,"seconds":7,"nanoseconds":9}]}}
```

`-16006` — SCHED процесса PID 2000. PROF и VIRT независимы. Текущие SCHED ID 2, -6 (PID ноль) и -8006 (PID 1000) разделяют один образец; алиасы PROF — -8/-8008, VIRT — -7/-8007. Дубли отклоняются даже при равных значениях. CPU-секунды неотрицательны, наносекунды нормализованы. Продвижение при простое меняет только настенные часы 0, 1 и 7; CPU-образцы остаются фиксированными. Расход CPU из выполнения инструкций не выводится.

Собственный TID текущей задачи также обозначает её группу, включая кооперативные потоки Android без внешнего каталога. Отсутствующий внешний PID или живой не-лидер в замкнутом каталоге даёт `EINVAL` до доступа к выходу. Неуказанный каталог или отсутствующий образец известной группы оставляет операцию неподдерживаемой до копирования. Неверный вид даёт `EINVAL`; допустимый образец может дать `EFAULT` при пользовательском копировании. Сырые traps сохраняют отрицательные ошибки; только Bionic меняет errno и возвращает -1.

Правила следуют `pid_for_clock`, `posix_cpu_clock_get`, диспетчеру и определениям ID каждой закреплённой версии:

| Запрошенная ветка | Исходник CPU-часов процессов |
| --- | --- |
| `android12-5.10` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/time/posix-cpu-timers.c) |
| `android13-5.10` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/time/posix-cpu-timers.c) |
| `android13-5.15` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/time/posix-cpu-timers.c) |
| `android14-5.15` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/time/posix-cpu-timers.c) |
| `android14-6.1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/time/posix-cpu-timers.c) |
| `android15-6.6` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/time/posix-cpu-timers.c) |
| `android16-6.12` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/time/posix-cpu-timers.c) |
| `android17-6.18` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/time/posix-cpu-timers.c) |

Распознавание FD-часов и CPU-маршрутизация также следуют закреплённому [диспетчеру 6.18](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/time/posix-timers.c) и [определениям ID](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/include/linux/posix-timers_types.h). FD-часы и кодированные CPU-часы отдельных потоков не поддерживаются. Каталог — фиксированное наблюдение гостя; права, пространства имён, жизнь процессов и учёт CPU требуют собственных контрактов.

## Проверка и дальнейшее покрытие

`LinuxPIDFDTests.cpp` выполняет независимые ELF x64/AArch64 O0/O2 для восьми веток и доступных бэкендов: флаги, общая таблица, пределы/повторное использование, порядок ошибок, сбои метаданных, ограничение и исходные диапазоны, пропущенные/замкнутые каталоги, не-лидеры и поиск до исчерпания FD. `AndroidSyscallTests.cpp` повторяет владение raw/Bionic, поиск и errno в шести O0/O2-профилях с обычными, Android packed и RELR-релокациями. Исходники и выполнение доказывают этот поднабор; нативная загрузка всех закреплённых GKI-образов не проверена. Расширять Linux следует по службам с сохранением версий, конфигураций и наблюдений.

CPU-тесты проверяют идентичность, порядок вывода, независимые виды, явные образцы и отделение от продвижения настенных часов. `AndroidTimeTests.cpp` проверяет именованный/сырой вывод и сторожевые значения; кооперативный syscall проверяет алиас текущего TID не-лидера.
