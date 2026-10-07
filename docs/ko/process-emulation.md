**언어**: [English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← 문서 색인](README.md)

# 게스트 프로세스 에뮬레이션

`neverd emulate`는 명시적 게스트 OS 프로필로 이미지를 실행합니다. CPU 전송, 이미지 파싱, 프로세스 진입, OS 서비스는 각각 별도 소유 경계입니다. `NEVERD_ENABLE_CPU_EMULATION=ON`으로 활성화합니다. 드라이버 에뮬레이션에도 포함됩니다.

첫 프로필 `linux-elf64-v1`은 CPL3 또는 EL0에서 x64/AArch64 ELF `ET_EXEC`와 자체 재배치 static PIE `ET_DYN`을 실행합니다. 실제 ELF 세그먼트를 적재하고 초기 스택을 만들며 명령 quantum으로 재개하고 명시적 Linux 시스템 호출 요청을 처리합니다. 이는 완전한 Linux 배포판이나 임의 libc 바이너리 실행을 보장하지 않는 독립 프로세스 모델입니다. 동적 링크, 시그널, 스레드, 파일 시스템, 미지원 서비스는 명시적으로 실패합니다.

<!-- i18n-section: cli-sdk -->

## CLI 및 SDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

호환 Linux 호스트는 KVM, 호환 Windows 호스트는 WHP를 선택합니다. 그 외 호스트/게스트 ISA 조합은 Unicorn을 사용합니다. 선택한 백엔드를 사용할 수 없으면 조용한 대체 없이 오류입니다. Windows에서도 ELF 게스트는 Linux 프로세스 모델을 사용합니다. 명령 목록과 제한은 [CPU 실행](cpu-execution.md)을 참조하세요.

CLI는 JSON 보고서 하나를 출력합니다. 게스트 상태 0은 종료 코드 0, 다른 상태는 2, 불완전 실행(오류/한도 포함)은 3, 잘못된 설정/API는 1입니다. 실제 게스트 상태는 `exit_status`에 있습니다. 추가 C 진입점 [`neverd_emulate_process_json`](../../include/neverd/sdk/NeverDCAPIProcess.h)은 session, 비어 있지 않은 입력 경로, 명시 프로필, 선택적 옵션 JSON을 받습니다. 결과는 `neverd_free_string`으로 해제합니다. NULL은 설정 실패이며 `neverd_last_error`에 원인이 있습니다. 게스트 fault나 자원 중지도 보고서를 반환합니다. session의 분석용 로드 이미지는 필요하지 않고 변경되지 않습니다.

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

<!-- i18n-section: options-results -->

## 옵션과 결과

옵션은 최대 64 KiB JSON 객체입니다. 알 수 없는/null 필드, 잘못된 유형, 문자열 안의 NUL, 양수가 아닌 한도는 거부됩니다.

| 옵션 | 기본값 | 계약 |
|---|---|---|
| `backend` | `auto` | `auto`, `unicorn`, `kvm`, `whp` |
| `arguments` | 입력 파일명 | argv[0] 포함 완전한 argv; 비우면 기본값 |
| `environment` | `[]` | 명시적 게스트 문자열; 호스트 환경을 상속하지 않음 |
| `instruction_limit` | 100000 | 공유되는 허용 명령 시도 수 |
| `event_limit` | 10000 | OS 서비스 처리 전에 차감되는 syscall 이벤트 수 |
| `timeout_microseconds` | 5000000 | 프로세스 설정 후 시작하는 단조 deadline |
| `memory_limit` | 67108864 | 물리/매핑 메모리 예산 |
| `stack_size` | 1048576 | 예산 내 페이지 정렬 스택 |
| `output_limit` | 1048576 | stdout/stderr 합산 캡처 바이트 |
| `instruction_quantum` | 1024 | runtime으로 양보하기 전 admission 간격 |
| `linux_priority` | 미설정 | 태스크별 명시적 nice 값과 원시 Linux 우선순위 서비스의 호출 권한 |
| `linux_kernel` | 미지정 | 출시된 GKI 브랜치 또는 관측된 게스트 커널 인터페이스 부재를 명시 |

`schema_version`은 1입니다. 보고서에는 프로필, 아키텍처, 선택 백엔드와 이유, `stop_reason`, nullable `exit_status`, 진단, 진입/현재 PC, 카운터, 서비스 기록, 마지막 typed CPU exit가 포함됩니다. 주소, syscall 번호, 인자 레지스터, raw 반환 비트는 `0x` 없는 16진수 문자열입니다. `stdout_hex`/`stderr_hex`는 NUL과 잘못된 UTF-8을 보존합니다. syscall 결과 null은 모델링된 반환이 없다는 뜻(exit 또는 미지원 요청 등)이지 성공한 0이 아닙니다.

<!-- i18n-section: linux-semantics -->

## Linux 프로필 의미론

`writev`는 x64/ARM64와 Android Bionic에서 같은 출력 대상을 공유합니다. 출력 전에 게스트 `iovec`를 최대 1024개 가져오고, 음수 길이를 `EINVAL`로 거부하며, 사용자 주소 범위를 검증한 뒤 Linux의 페이지 정렬 전송 상한을 적용합니다. 잘못된 설명자는 벡터 접근 전에 `EBADF`, 읽을 수 없는 메타데이터는 출력 없이 `EFAULT`를 반환합니다. 이후 데이터 오류는 복사된 접두부를 유지합니다. 출력 예산은 게시 전에 두 스트림과 전체 벡터에 적용됩니다. `write`와 `writev`는 설명자의 하위 32비트를 사용하고 벡터 수도 Linux의 32비트 가져오기 규칙을 따릅니다. Bionic만 원시 음수 오류를 `-1`과 `errno`로 변환합니다. `LinuxOutputNativeTests`는 직접 작성한 열 사례를 호스트 Linux에서 일반 파일로 출력하여 검증하고, 모델의 x64/ARM64 사례는 예산도 검사합니다. [Linux 벡터 가져오기 계약](https://github.com/torvalds/linux/blob/v6.12/lib/iov_iter.c)을 참고하세요.

OS 정책은 기존 ELF 로더가 디코딩한 프로그램 헤더를 사용합니다. ABI 태그, 세그먼트 정렬, 매핑된 프로그램 헤더 테이블, user 주소 범위를 검증합니다. 매핑 계획은 할당 전에 범위, 권한, 겹침, 예산을 확인하고 완전히 준비한 전용 주소 공간만 공개합니다. 파일 페이지 앞/뒤 바이트를 보존하고 BSS를 0으로 채우며 세그먼트 권한을 지키고 스택 guard gap을 예약합니다. 페이지가 겹치는 레이아웃과 모순 헤더는 추측하지 않고 거부합니다.

Static PIE는 최소 `0x40000000`의 결정적 load bias를 사용하고 더 큰 `PT_LOAD` 정렬 요구에 따라 높입니다. 모든 매핑 세그먼트, entry PC, `AT_PHDR`/`AT_ENTRY`가 같은 bias를 사용하며 원래 program-header 값은 유지되고 interpreter가 없으므로 `AT_BASE`는 0입니다. 매핑 원본은 분석 fixup이 적용되지 않은 파일 바이트입니다. guest 시작 코드가 직접 relocation과 초기화를 수행해야 합니다. loader는 section header 없이 원본 파일의 제한된 record에서 `PT_DYNAMIC`을 decode합니다. 존재 시 readable/terminated 상태이며 최대 4096개 항목이어야 합니다. `PT_INTERP`와 외부 dependency/filter/audit tag는 거부합니다. dynamic linker, symbol resolver, constructor runner는 제공하지 않습니다.

초기 스택은 정렬된 argc/argv/envp/auxv, PHDR/PHENT/PHNUM, entry, 페이지 크기 및 identity 값을 포함합니다. 모델 PID/TID/UID/GID는 1000입니다. 재현성을 위해 `AT_RANDOM`은 입력 SHA-256의 첫 16바이트입니다. 이는 암호학적 엔트로피가 아닌 결정적 모델 정책입니다. HWCAP/HWCAP2는 0이며 vDSO는 없습니다.

`write`, `writev`, `exit`, `exit_group`, `getpid`, `gettid`, `mmap`, `mprotect`, `munmap`, `brk`를 구현하며 번호는 [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl)와 [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h)에서 다릅니다. 반환하는 x64 SYSCALL은 RCX/R11 clobber, RAX, 다음 PC를 반영합니다. ARM64는 번호에 x8, 결과에 x0을 사용합니다. 나머지는 `unsupported_service`로 중단하며 호스트 syscall을 실행하지 않습니다.

Static TLS template `PT_TLS`는 loader 소유 사실로 검증합니다. template 하나, 제한된 file/memory 범위, 일치하는 정렬, 읽을 수 있는 초기화 바이트가 필요합니다. guest startup이 TLS block을 할당·초기화하고 thread pointer를 설치합니다. Linux 모델은 libc별 TCB/DTV를 만들지 않습니다. 이에 따라 freestanding 프로그램의 컴파일러 생성 local-exec TLS를 지원합니다. dynamic TLS와 OS 스레드는 별도 작업입니다.

x64 `arch_prctl`은 `ARCH_SET_FS`, `ARCH_GET_FS`, `ARCH_SET_GS`, `ARCH_GET_GS`를 지원합니다. Set은 매핑되지 않은 user 범위 base도 받아들이지만 이후 역참조는 권한을 검사합니다. kernel 범위 base는 guest `EPERM`, 잘못된 Get 대상은 CPU fault 없이 `EFAULT`를 반환합니다. 나머지 operation은 명시적으로 실패합니다. ARM64 시작은 `MSR`로 `TPIDR_EL0`를 설정합니다. `MRS`, FS/GS 메모리 접근, context 복원은 quantum 및 backend 진입 사이에서 thread pointer를 보존합니다. 이는 thread scheduler를 구현하지 않습니다.

파일 디스크립터 1과 2는 가상 바이트 sink입니다. `write`는 읽기 가능한 user 페이지를 검증하고, 뒤쪽 페이지 접근이 막히면 읽을 수 있는 prefix를 반환하며 한 바이트도 읽지 못하면 게스트 `EFAULT`를 반환합니다. 잘못된 디스크립터는 `EBADF`; 0바이트 쓰기도 사용자 주소 범위를 검사하지만 페이지 매핑이나 데이터 읽기를 요구하지 않습니다. Linux pipe 원자성이나 파일 객체는 모델링하지 않습니다. 출력 한도를 넘을 쓰기는 게시 전에 중단됩니다.

익명 메모리 서비스는 이미지 및 스택과 같은 프로세스 주소 공간과 물리 메모리 예산을 사용합니다. `mmap`은 정확히 `MAP_PRIVATE | MAP_ANONYMOUS`와 일반 `PROT_NONE`, `PROT_READ`, `PROT_READ | PROT_WRITE`, `PROT_READ | PROT_EXEC` 또는 읽기 가능한 RWX 권한을 허용합니다. 비어 있고 페이지 정렬된 힌트를 우선하며, 그렇지 않으면 `0x100000000`부터, 이어 최소 사용자 주소부터 빈 영역을 찾고 스택 보호 영역을 보존합니다. 이 결정적 배치는 Linux ASLR을 모방하지 않습니다. 새 페이지는 개별 소유하며 0으로 채우므로 부분 해제로 고정되지 않은 페이지를 회수할 수 있습니다. CPU 투영이나 유지된 backing view는 자신의 수명이 끝날 때까지 폐기된 할당을 유지할 수 있습니다.

길이는 페이지 단위로 올림합니다. `munmap`은 빈 영역과 반복 해제를 허용하며, `mprotect`는 빈 영역 앞의 매핑을 변경한 후 `ENOMEM`을 반환합니다. `PROT_NONE`은 할당과 바이트를 보존하면서 게스트 접근을 거부합니다. 원시 `brk`는 성공 시 요청한 바이트 경계, 실패 시 이전 경계를 반환하며 libc의 0/-1 규약을 사용하지 않습니다. 초기 break는 페이지 정렬된 이미지 끝입니다. 확장은 다른 매핑과 예산을 준수하며 축소는 남은 부분 페이지의 바이트를 보존합니다. 지원 범위의 규칙과 오류 우선순위는 Linux [매핑](https://github.com/torvalds/linux/blob/v6.8/mm/mmap.c) 및 [보호](https://github.com/torvalds/linux/blob/v6.8/mm/mprotect.c)를 따릅니다.

파일/공유/고정 매핑, 아래 방향 확장, 대형 페이지, 메모리 잠금, 보호 키, 실행 전용/쓰기 전용 정책 및 다른 플래그는 명시적으로 지원하지 않습니다. 효과를 게시하거나 반환값을 만들기 전에 중단합니다. 지원 범위 안의 일반 범위/길이/정렬 오류는 게스트 오류를 반환하고 실행을 계속합니다. 게스트 포인터나 매핑 요청을 호스트 OS에 전달하지 않습니다.

선택적인 `linux_kernel` 입력은 명확히 관측한 커널 인터페이스 부재를 기록합니다. 예를 들어 `pidfd_open` 구현이 없는 fixture는 다음을 사용합니다.

```json
{"linux_kernel":{"unavailable_syscalls":["pidfd_open"]}}
```

선택한 raw 호출은 커널 진입점이 없는 경우처럼 인자 검증 전에 -ENOSYS를 반환하며, descriptor를 생성하거나 게스트 메모리를 변경하지 않습니다. Bionic `syscall` wrapper는 일반적인 -1/errno 변환을 유지합니다. 입력을 생략하거나 빈 목록을 주면 이 인터페이스의 기존 미지원 경계가 유지되며, 다른 알 수 없는 호출을 ENOSYS로 변환하지 않습니다. 현재 `pidfd_open`만 허용하며 알 수 없는 이름, 중복, 잘못된 타입은 거부합니다. 입력으로 커널 버전, 호스트 가용성 또는 작동하는 pidfd 구현을 추정하지 않습니다. [커널의 누락 호출 구현](https://github.com/torvalds/linux/blob/master/kernel/sys_ni.c)을 참조하십시오.

명시적인 `linux_kernel.gki`는 5.10부터 6.18까지 출시된 Android common 커널 브랜치를 선택합니다. 현재 구현 범위는 살아 있는 모델 프로세스에 대한 `pidfd_open`이며, 버전별 플래그와 `linux_files`와 동일한 descriptor 테이블을 사용합니다. Bionic과 raw trap은 소유권과 오류 순서를 공유합니다. GKI와 `pidfd_open` 부재 관측은 함께 선택할 수 없습니다. Android API 수준은 커널을 선택하지 않습니다. 고정된 8개 소스 버전, descriptor 동작, 테스트 및 남은 범위는 [출시된 GKI 계약](../android-gki-kernels.md)을 참조하십시오.

선택 입력 `linux_priority`는 호출자와 같은 UID를 가진 테스트 태스크의 nice 상태를 선언합니다. Linux ELF64와 Android의 원시 `setpriority` 및 `getpriority`는 이 상태를 공유하며 호스트 우선순위는 변경하지 않습니다.

```json
{"linux_priority":{"tasks":[{"id":1000,"nice":0}],
                   "cap_sys_nice":false,"rlimit_nice":0}}
```

태스크 ID는 서로 다른 양수의 부호 있는 32비트 값이며, 초기 nice는 -20..19, `rlimit_nice`는 0..40입니다. `cap_sys_nice`와 `rlimit_nice`의 기본값은 false와 0이지만 태스크 상태는 항상 명시해야 합니다. 입력이 없거나 태스크가 목록에 없으면(상태를 선언하지 않은 새 스레드 포함) 미지원으로 중단하며 상속이나 소유자를 추측하지 않습니다. PRIO_PGRP/PRIO_USER 선택도 미지원으로 중단합니다. PRIO_PROCESS에서 who=0은 현재 게스트 태스크를, 그 외 값은 지정 태스크를 선택합니다. 잘못된 선택자는 원시 -EINVAL을 반환합니다. 설정 요청은 부호 있는 32비트 nice를 -20..19로 제한합니다. nice 값을 낮추려면 CAP_SYS_NICE 또는 충분한 RLIMIT_NICE가 필요하며, 거부 시 상태를 바꾸지 않고 원시 -EACCES를 반환합니다. 원시 조회는 `20 - nice`, 즉 커널의 40..1 인코딩이며 libc가 변환한 결과가 아닙니다.

[Linux setpriority/getpriority](https://man7.org/linux/man-pages/man2/setpriority.2.html).

`linux_signals`는 프로세스 전체의 초기 시그널 동작을 지정합니다. 누락된 항목은 알 수 없는 값이며 `SIG_DFL`을 뜻하지 않습니다. 명시적인 빈 목록은 이전 동작을 조회하지 않고 새 동작을 설정할 수 있습니다. 다섯 필드가 모두 필수이며, JSON의 정확한 정수 범위를 벗어나는 부호 없는 64비트 값은 십진 문자열을 사용합니다.

```json
{"linux_signals":{"actions":[
  {"signal":11,"handler":0,"flags":0,"restorer":0,"mask":0}
]}}
```

시그널 번호는 1–64입니다. `rt_sigaction`과 Bionic은 상태를 공유하며 구조체 배치와 오류 순서는 각 인터페이스를 따릅니다. 대기 시그널, 전달, 핸들러 호출 및 스레드별 마스크는 구현하지 않으며 호스트 핸들러도 사용하지 않습니다. [전체 계약](../process-emulation.md#linux-profile-semantics)을 참조하세요.

<a id="windows-pe64-profile"></a>

<!-- i18n-section: linux-clocks -->

## 명시적인 게스트 시계

선택 항목인 `linux_time`은 Linux 시스템 호출과 Android Bionic에 고정된 시계 값을 제공합니다. 호스트 시계를 읽거나 명령 실행에 따라 시간을 진행시키거나 기본 시각을 추측하지 않습니다.

```json
{"linux_time":{"clocks":[
  {"id":0,"seconds":"4294967297","nanoseconds":987654321},
  {"id":1,"seconds":123,"nanoseconds":456789}],
  "timezone":{"minutes_west":-60,"dst_time":0}}}
```

정적 시계 ID 0–9와 11을 지원합니다. 각 시계는 독립적이며 생략한 값은 알 수 없는 상태로 유지됩니다. 중복되거나 알 수 없는 ID는 거부합니다. 초는 부호 있는 64비트 정수이며 나노초 범위는 `[0, 1000000000)`입니다. JSON 정수는 `±9007199254740991` 이내여야 하고 십진 문자열은 전체 64비트 범위를 보존합니다. 시간대 필드는 부호 있는 32비트 정수입니다. C++에서는 `ProcessOptions::LinuxTime`을 사용하며 다른 OS 프로필은 이 옵션을 거부합니다.

`clock_gettime`, `gettimeofday`, x64의 `time`이 같은 입력을 사용합니다. 입력 누락, 동적 시계 또는 모델링되지 않은 부분 쓰기는 명시적으로 중단되며 이미 완료된 쓰기는 유지됩니다. 시간 조정, 대기 및 실제 장치 시계는 지원하지 않습니다. 쓰기 순서, 오류 코드 및 포인터 동작은[전체 시계 계약](../process-emulation.md#explicit-guest-clocks)을 참조하세요.

<a id="explicit-memory-files"></a>

<!-- i18n-section: linux-files -->

## 명시적 메모리 파일

`linux_files`는 Linux ELF64와 Android에 닫힌 읽기 전용 파일 목록을 제공합니다. 필수 `files`는 비어 있어도 되며 각 항목은 정규 절대 경로 `path`와 바이너리 `bytes_hex`를 필수로 포함합니다. 호스트 파일이나 암시적 `/proc` 내용을 읽지 않습니다. 옵션이 없으면 중단하며 목록에 없는 경로는 `ENOENT`를 반환합니다.

```json
{"linux_files":{"files":[
  {"path":"/fixture/data","bytes_hex":"00ff410a805a"}],
  "descriptor_limit":256}}
```

각 open은 독립된 위치를 가지며 Bionic, `syscall`, 원시 트랩과 guest 스레드는 설명자 표를 공유합니다. close 후 가장 작은 빈 번호를 재사용합니다. 읽기 전용 `open/openat`, `read/close`, 일반 `lseek`, `O_CLOEXEC`와 아키텍처별 `O_LARGEFILE`을 지원합니다. Bionic만 errno를 변환합니다. 읽기 오류는 복사한 접두부를 보존합니다. stdin에는 기본 내용이 없으며 상대 경로, 디렉터리, 쓰기와 링크는 지원하지 않습니다.

C++: `ProcessOptions::LinuxFiles`. `descriptor_limit`: 3–4096 (256); `files` ≤ 256; `path` < 4096 bytes; component ≤ 255 bytes; data + paths + NUL ≤ 16 MiB; JSON ≤ 64 KiB. [Contract](../process-emulation.md#explicit-memory-files).

길이가 0인 읽기는 사용자 주소 범위의 끝에서 시작할 수 있습니다. 원래 주소 범위를 검증한 뒤 파일 위치와 원래 요청 길이의 합이 `INT64_MAX`를 초과하면 EOF에서도 `EINVAL`을 반환하며 커서는 바뀌지 않습니다.

각 항목에는 완전한 `metadata`를 추가할 수 있습니다. C++의 `LinuxFileOptions::Metadata`는 이미 존재하는 파일 경로를 키로 사용합니다. `fstat`/`fstat64`와 syscall은 고정 관측값을 공유하며 호스트 속성을 읽거나 내용 길이로 size를 추론하거나 커서를 이동하지 않습니다. 일반 파일과 모든 필수 필드만 허용하며 전체 폭 정수는 십진 문자열을 사용합니다. x64/AArch64는 144/128바이트를 쓰고 rdev와 패딩은 0입니다. 잘못된 설명자는 `EBADF`, 전체가 쓰기 불가능한 출력은 `EFAULT`입니다. 메타데이터 누락, 알 수 없는 표준 스트림, 일부만 쓰기 가능한 출력은 바이트를 바꾸지 않고 중단합니다. 필드와 범위는 연결된 계약을 참조하십시오.

```json
{"linux_files":{"files":[{"path":"/fixture/virtual","bytes_hex":"616263",
  "metadata":{"device":1,"inode":"18446744073709551615","mode":33060,
    "link_count":1,"uid":1000,"gid":1000,"size":0,"block_size":4096,"blocks":0,
    "access_time":{"seconds":0,"nanoseconds":0},
    "modification_time":{"seconds":0,"nanoseconds":0},
    "change_time":{"seconds":0,"nanoseconds":0}}}]}}
```

<!-- i18n-section: windows-pe64 -->

## Windows PE64 프로필

`windows-pe64-v1`은 PEB/TEB, 정적·동적 TLS, `DllMain`, 이름 기반 Win32 API 및 명시적 비순환 DLL 그래프를 갖춘 제한된 Windows x64/ARM64 콘솔 프로세스를 지원합니다. 게스트 모듈은 이름/서수 코드·데이터 가져오기, DIR64 재배치, 전달 내보내기 및 실제 로더 목록 식별자를 지원합니다. `LoadLibraryA` / `LoadLibraryW`, `FreeLibrary`, `GetProcAddress`는 설정된 모듈 카탈로그를 사용합니다. CRT/GUI, ARM64 스택 프레임 기반 사용자 SEH, 스레드 및 일반 Windows 앱 호환성은 미완성이며 네이티브 ARM64 KVM/WHP 증거도 아직 없습니다.

Windows 가상 메모리는 `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery`와 현재 프로세스의 `FlushInstructionCache`를 지원합니다. OS 계층은 예약 영역을 소유하고 `AddressSpace`는 커밋된 페이지, 권한, 실제 저장 공간을 관리합니다. 테스트는 동적 코드 수정, 접근 오류, 메모리 한도 재사용을 검증합니다.

전용 할당은 `MEM_RESERVE`, `MEM_COMMIT`, `MEM_DECOMMIT`, `MEM_RELEASE`, `MEM_TOP_DOWN`을 지원하며 예약 정렬은 64 KiB, 페이지는 4 KiB입니다. 예약만으로는 게스트 RAM을 사용하지 않습니다. 재커밋은 데이터를 보존하며 권한을 갱신하고 커밋 해제는 각 페이지의 저장 공간을 반환합니다. 전체 범위 검증과 사전 할당으로 일반적인 실패 시 일부만 변경되는 일을 방지합니다. 조회는 48바이트 x64/ARM64 메모리 정보 구조를 반환하고 동일한 할당 안에서만 이후 영역을 합칩니다. 초기 이미지, 환경, 힙 영역, API 진입점, 스택 경계도 배치에 반영하며 스택 할당 식별은 TEB와 일치합니다. 성공한 `VirtualProtect`가 이전 권한의 출력 위치를 읽기 전용으로 바꾸면 새 권한은 유지되고 출력 내용은 바뀌지 않으며 호출은 성공을 반환합니다. 커밋되지 않은 페이지를 포함한 범위의 권한 변경은 `ERROR_INVALID_ADDRESS`를 반환하고 이전 권한 출력에 `PAGE_NOACCESS`를 기록하며 페이지 권한은 변경하지 않습니다.

지원 권한은 `PAGE_NOACCESS`, `PAGE_READONLY`, `PAGE_READWRITE`, `PAGE_EXECUTE_READ`, `PAGE_EXECUTE_READWRITE`입니다. 가드 페이지, 실행 전용, 쓰기 시 복사 정책, 캐시 수식자, 대형 페이지, reset/write-watch/자리 표시자 및 모델이 소유한 런타임 매핑 변경은 명시적으로 거부합니다. 전용 가상 할당만 커밋 해제하거나 해제할 수 있습니다.

[VirtualAlloc](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc), [VirtualFree](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualfree), [VirtualProtect](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect), [VirtualQuery](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualquery), [MEMORY_BASIC_INFORMATION](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-memory_basic_information).

```bash
neverd emulate guest.exe --profile=windows-pe64-v1 \
  --options='{"backend":"auto","arguments":["guest.exe","argument"],"environment":["MODE=test"]}'
```

단일 스레드 PE32+ EXE는 선호 기준 주소를 유지하며 선택적 진입점과 정적 TLS를 가진 명시적 DLL을 허용합니다. `WindowsProcessOptions::Modules` 또는 JSON `windows.modules`의 `name`, `path`로 최대 64개 게스트 기본 이름과 호스트 입력 경로를 지정합니다. 호스트 DLL을 검색하거나 실행하지 않습니다. ASCII 이름은 대소문자를 구분하지 않으며 중복과 시스템 API 제공자 재정의를 거부하고 도달 가능한 파일만 읽습니다. 이름/서수 함수·데이터 가져오기는 실제 내보내기에 연결됩니다. 빈 서수, 없는 심볼, 순환, bound/delay import와 미지원 load configuration/CFG는 실패합니다. 이동 가능한 DLL 충돌에는 DIR64를 적용하며 고정 주소 충돌과 링크 메타데이터를 덮는 재배치는 해당 이미지를 공개하기 전에 거부합니다.

`readPEProgramExports`는 원본 내보내기와 읽기 범위를, `WindowsProcessModules`는 그래프와 프로세스 공통 제공자/이름 API 게이트를 소유합니다. `VirtualMemory`가 모든 이미지를 먼저 예약하고 `AddressSpace`가 페이지와 권한을 관리합니다. PEB/LDR에는 실제 이미지만 있으며 초기화 목록은 로더 등록 순서입니다. 등록 순서와 의존성에 따른 attach 호출 순서를 별도로 유지합니다. `GetModuleHandleW`는 NULL 또는 ASCII 기본 이름을 받으며 대소문자를 무시하고 확장자가 없으면 `.dll`을 붙입니다. 경로, 비 ASCII 조회, 끝의 점 규칙은 미지원입니다. 없는 이름은 오류 126, 성공은 LastError를 유지합니다. API 모델은 설치된 DLL이 아닙니다.

입력 총 바이트와 이미지 전체 범위는 각각 `memory_limit`로 제한하고 런타임 매핑도 이미지 예산에 포함합니다. 준비 단계는 65,536개 레코드, 64 MiB 메타데이터 읽기, 이름 길이와 전체 작업 기한을 공유합니다. 호스트 I/O의 강제 시간 보장은 없습니다. 독자 EXE→DLL→DLL은 재배치, 서수, 공유 데이터, API 포인터, `MEM_IMAGE`, 목록, EXE TLS attach/detach를 확인합니다. `NeverDWindowsProcessTests`는 원본 네이티브 Windows 대조, `NeverDPEProgramExportsTests`는 잘못된 메타데이터와 예산, `NeverDProcessPublicTests`는 C ABI/CLI 일치를 검증합니다. 사용할 수 없는 백엔드는 명시적으로 건너뜁니다.

`WindowsProcessLifetime`은 같은 CPU와 실행 예산에서 의존 순서대로 DLL TLS 콜백과 `DllMain`, 이어서 EXE TLS와 진입점을 실행합니다. 모듈마다 독립 TLS 인덱스와 정렬된 블록을 할당하고 재배치·연결된 이미지에서 공용 64 KiB 영역으로 복사합니다. TLS 예약 인수는 0이며 시작/프로세스 종료 `DllMain`은 불투명한 비 NULL 값을 받습니다. 명시적 프로세스 종료는 초기화를 완료한 DLL을 로더 목록의 역순으로 분리한 뒤 EXE TLS를 호출하며 EXE 초기화 전에도 같습니다. 시작 `DllMain(FALSE)`는 분리 통지 없이 `0xc0000142`로 종료합니다. 오류와 예산 소진은 가짜 정리를 수행하지 않습니다. 게스트 DLL이 있는 PE 진입점 반환은 미지원 스레드 종료가 필요하므로 명시적으로 중단합니다. 0이 아닌 `SizeOfZeroFill`은 미지원이며 실제 TLS 템플릿의 0으로 초기화된 바이트는 지원합니다. 진입점 없는 DLL은 TLS attach를 받지만 프로세스 detach 통지는 받지 않습니다.

`WindowsProcessExports`는 정적 가져오기와 `GetProcAddress`에 같은 이름/서수 해석을 사용하여 코드, 데이터, 별칭, 연쇄 전달을 처리합니다. 실제로 참조하는 시작 전달만 카탈로그 모듈과 초기화 의존성을 추가하며 사용하지 않는 전달은 파일을 읽지 않습니다. 이름은 대소문자를 구분하며 없는 이름은 NULL/오류 127, 직접 조회한 없는 서수는 빈 슬롯을 포함해 NULL/오류 182, NULL 조회 인수는 오류 87을 반환하고 성공은 LastError를 보존합니다. 알 수 없는 모듈 핸들은 미지원입니다. 제한된 API 목록에서 정확한 제공자/이름별 진입점을 한 번 예약합니다. 각 이미지의 현재 PE 헤더와 내보내기 메타데이터를 검사하고 변경 또는 읽을 수 없는 바이트를 거부합니다. 체인은 최대 64개이며 준비 단계의 남은 메타데이터 예산과 실행 기한을 공유합니다. 빈 슬롯으로 전달하면 대상 이미지 기준 주소를 반환하고 LastError를 보존합니다. 서수 0으로 전달하면 오류 87을 반환합니다. 기준 주소는 데이터 주소이며 이미지 헤더 실행 권한을 부여하지 않습니다. 런타임 전달은 설정 카탈로그의 모듈을 로드하고 초기화를 마친 후 조회 결과를 반환할 수 있습니다. 실행 중 내보내기 테이블 변경은 여전히 지원하지 않습니다.

`WindowsProcessLoader`는 `windows.modules`의 ASCII DLL 기본 이름을 로드하며 명시적 참조, 공유 의존성과 시작 모듈 유지를 관리합니다. 전달 조회를 반복해도 참조가 추가되지 않습니다. 다시 로드할 때 카탈로그 슬롯에 새 상주 세대를 부여합니다. TLS와 `DllMain`은 같은 CPU에서 중단된 API 프레임 아래에서 실행되며 레지스터 복원은 게스트 메모리 쓰기를 보존하고 현재 반환 주소를 사용합니다. 동적 attach/detach 예약 포인터는 0입니다. 명시적 로드 중 attach 실패는 정리 후 오류 1114를 반환하되 성공한 독립 중첩 로드는 유지합니다. 언로드는 이미지 매핑과 TLS를 해제하고 재로드는 원본 내용을 복원합니다. 모델 밖에서 로더 목록이나 TLS 포인터를 바꾸면 명시적으로 실패합니다. 파일·이미지·메타데이터 작업 예산은 실패와 재로드에도 누적됩니다. 시스템 제공자는 매핑된 PE 베이스를 모듈 핸들로 사용합니다. 파일 시스템 검색, 비 ASCII 경로, `LoadLibraryEx` 플래그, 순환 가져오기, 초기화 또는 언로드 중인 같은 모듈의 재진입 상태 전환은 지원하지 않습니다.

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` 는 PEB 프로세스 매개변수의 실제 게스트 환경 블록을 공유합니다. 이름은 대소문자를 구분하지 않는 ASCII이며 값은 UTF-16입니다. 변경 전에 입력, 용량, 쓰기 가능한 메모리를 검증합니다. 스냅샷은 이후 변경과 독립적이며 해제하면 게스트 메모리를 회수합니다. 모델의 블록 한도는 64 KiB이고 문자열과 확장에는 크기 및 실행 기한 검사가 적용됩니다. 알 수 없는 포인터 소유권, 잘못된 블록, ANSI 코드 페이지, 확장 버퍼 중첩은 지원하지 않습니다. `WindowsEnvironmentTests.cpp`는 사용 가능한 백엔드에서 자체 x64/ARM64 픽스처를 비교하며 CI는 독립적인 네이티브 Windows 오라클을 필수로 실행합니다.

`WindowsProcessHeap`은 프로세스 힙의 할당, [`HeapReAlloc`](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heaprealloc), 해제와 크기 조회를 통합 관리합니다. 크기 변경은 유지되는 데이터를 보존하며 `HEAP_ZERO_MEMORY`는 추가 바이트를 0으로 만들고 `HEAP_REALLOC_IN_PLACE_ONLY`는 이동을 금지합니다. 재할당 실패 시 기존 블록을 보존하고 NULL을 반환하며 `ERROR_NOT_ENOUGH_MEMORY`(8)를 설정하여 네이티브 관측과 일치합니다. 독립적인 페이지는 축소와 해제 시 용량을 반환하며 단계별 확장과 제한된 복사는 실행 기한을 확인합니다. 사용자 정의 힙, 예외 생성 플래그, 알 수 없는 소유권, 접근 불가능한 복사 또는 초기화 범위는 명시적으로 중단합니다. `WindowsHeapTests.cpp`는 두 ISA, 강제 이동, 예산 재사용, 실패 원자성을 검증하며 CI는 동일한 자체 EXE를 네이티브 Windows에서도 실행합니다.

`WindowsSystemModules`는 두 ISA에 대해 `ntdll.dll`, `kernelbase.dll`, `kernel32.dll`의 제한된 PE64 모델 이미지를 만듭니다. ASCII `GetModuleHandleA` / [`GetModuleHandleW`](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulehandlew), `LoadLibraryA` / `LoadLibraryW`, [`GetProcAddress`](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress)는 매핑된 베이스를 공유하며 PEB/LDR과 `MEM_IMAGE`도 같은 이미지를 나타냅니다. 정적 가져오기, 이름 조회와 게스트 DLL 전달은 동일한 API 게이트와 내보내기 해석기를 사용합니다. 제공자는 고정 상주하고 게스트 초기화 콜백이 없으며 일반 게스트 DLL을 모두 해제한 뒤 진입점 반환을 막지 않습니다. 헤더 또는 내보내기 메타데이터가 바뀌면 조회를 중단합니다. 미지원 시스템 내보내기 이름과 0이 아닌 서수 조회는 명시적으로 중단하며 지원 이름의 대소문자 불일치와 빈 이름은 오류 127, NULL 조회는 87을 반환합니다. 생성 바이트와 주소는 모델 정책이며 Windows DLL 버전별 배치, 네이티브 서수와 제공자 간 별칭은 재구성하지 않습니다. `WindowsSystemTests.cpp`는 자체 x64/ARM64 EXE를 네이티브 Windows와 비교하고 초기 스레드 반환을 독립적으로 8회 관측합니다.

`WindowsProcessExceptions`는 같은 CPU와 프로세스 예산으로 `AddVectoredExceptionHandler`, `RemoveVectoredExceptionHandler`, `RaiseException`을 구현합니다. 순서가 있는 처리기는 등록·삭제, 중첩 예외, 모델 API 호출, DLL 로드와 프로세스 종료를 수행할 수 있습니다. x64/ARM64 데이터 접근 위반과 x64 정수 나눗셈 예외는 게스트의 `CONTEXT` 변경을 검증한 뒤 재개하며 범용 레지스터, SIMD 및 지원 FP 상태를 보존합니다. 소프트웨어 예외는 모델 공급자 내부의 실제 반환 명령으로 재개합니다. 보관된 등록은 128개, 중첩은 16프레임으로 제한합니다. 잘못된 처리 결과, 바뀐 예외 포인터, 미지원 필드와 한도 초과는 명시적으로 실패합니다. ARM64 스택 프레임 기반 SEH/언와인딩, 디버거 전달과 실행/가드 페이지 예외는 미지원입니다. `WindowsExceptionTests.cpp`는 자체 EXE/DLL을 네이티브 Windows와 비교하며 ARM64 KVM/WHP 실기기 증거는 아직 없습니다. [AddVectoredExceptionHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-addvectoredexceptionhandler), [RemoveVectoredExceptionHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-removevectoredexceptionhandler), [RaiseException](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-raiseexception), [CONTEXT x64](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-context), [ARM64_NT_CONTEXT](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-arm64_nt_context). 소프트웨어 예외 레코드에는 `EXCEPTION_SOFTWARE_ORIGINATE`(`0x80`)가 포함되며 호출자의 계속 불가 플래그와 별도로 처리됩니다. 원본 Windows 실행 파일은 소프트웨어 및 하드웨어 예외의 정확한 플래그 값을 검증합니다. [EXCEPTION_RECORD](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_record).

`WindowsProcessContext`는 각 디스패치 프레임의 발생 원인을 유지합니다. 지원하는 x64 데이터 접근·나눗셈 오류는 `CONTEXT.EFlags`에 RF(`0x10000`)를 표시하며, 소프트웨어 접근 위반 코드를 포함한 `RaiseException`은 현재 컨텍스트를 유지합니다. 발생 원인은 VEH/VCH와 SEH 검색·해제 과정에서도 유지됩니다. 유효한 계속 실행은 RF 없이 논리 CPU 플래그를 복원하며, 게스트의 RF 변경은 상태 반영 전에 거부됩니다. 이 제한된 프로필은 명령어 중단점이나 게스트가 제어하는 RF를 모델링하지 않습니다. `WindowsExceptionTests.cpp`는 저장 기록, 복원, 거부 시 CPU·RAM 불변성을 확인합니다.

Windows ring3는 독립적인 네이티브 관측에 따라 checked x64의 `operand_alignment` 오류를 매개변수 `[read, UINT64_MAX]`의 `STATUS_ACCESS_VIOLATION`으로 변환합니다. 저장 명령도 동일합니다. CPU 계층이 원인을 제공하며 Windows는 벡터 13으로 추측하거나 명령을 다시 디코딩하지 않습니다. `WindowsAlignmentProcessTests.cpp`는 원본 PE 명령으로 오류 시나리오 72개와 주소 수정 후 재시도 9개(`72 + 9`)를 실행하며 PC, RF, XMM, RAM을 검사합니다. 미분류 또는 일관되지 않은 오류는 거부합니다. 프로세스와 드라이버 오류 보고서는 nullable `cause`와 16진수 `error_code`를 보존하여 누락과 0을 구분합니다. 이 전달은 checked x64 사용자 프로파일에 적용됩니다.

`AddVectoredContinueHandler`와 `RemoveVectoredContinueHandler`는 독립된 순서 목록을 관리하며 예외 처리기와 최대 128개 보존 등록 제한을 공유합니다. 벡터 예외 처리기가 실행 재개를 수락하면 계속 처리기는 같은 수정 가능한 예외 레코드와 `CONTEXT`를 봅니다. 중첩 예외와 DLL 알림을 포함한 콜백이 끝난 뒤 최종 컨텍스트를 검증합니다. 다른 종류의 처리기 API로 핸들을 제거할 수 없습니다. `WindowsContinuationTests.cpp`는 순서, 조기 종료, 등록 변경, 컨텍스트 복구, 중첩 전달, 로더 콜백과 프로세스 종료를 독자 EXE와 네이티브 Windows로 비교합니다. 검증한 Windows x64 벡터 경로에서는 `EXCEPTION_NONCONTINUABLE`이 설정되어도 재개할 수 있지만 스택 프레임 기반 SEH 동작의 근거는 아닙니다. 네이티브 ARM64 실행은 아직 검증하지 않았습니다. [AddVectoredContinueHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-addvectoredcontinuehandler), [RemoveVectoredContinueHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-removevectoredcontinuehandler).

`RtlCaptureContext`는 x64와 ARM64의 `kernel32.dll`, `ntdll.dll`에서 사용할 수 있습니다. 공유 `WindowsProcessContext`와 `IntegerABI`가 CPU 상태와 LastError를 바꾸지 않고 호출자의 PC/SP를 저장합니다. 네이티브 Windows 관찰로 x64 플래그 `0x10000f`, 사용하지 않는 home/디버그/벡터 영역의 보존, 기존 32비트 x87 주소 필드를 확인했습니다. ARM64는 LR을 PC에 저장하고 기록의 X0/LR을 0으로 만듭니다. 레지스터, SIMD, 부동소수점 제어는 게스트에서 가져오며 x64 선택자와 MXCSR 기능 마스크는 설정된 게스트 CPU를 따릅니다. 잘못되거나 정렬되지 않거나 일부에 접근할 수 없는 대상 기록은 쓰기 전에 실패합니다. `WindowsContextTests.cpp`는 직접 가져오기, 제공자 조회, VEH 콜백, 페이지 경계를 넘는 출력과 실패 원자성을 검증합니다. `scripts/check_windows_context.py`는 독자 실행 파일을 Windows x64/ARM64에서 실행하고 비어 있지 않은 x87 상태를 별도로 검증합니다. 이 ARM64 API 관찰은 네이티브 KVM/WHP 실행 증거가 아닙니다. 컨텍스트 복원, 스택 순회와 동적 함수 테이블은 여전히 별도 구현 과제입니다. `WindowsProcessServices.def`는 정확한 모듈 제한을 선언합니다. `kernelbase.dll`에서 이 심볼을 찾으면 네이티브 관찰과 동일하게 `ERROR_PROC_NOT_FOUND`(127)를 반환하며 존재하지 않는 내보내기를 추가하지 않습니다. [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

`WindowsProcessSEH`는 `os/windows/exception/`의 공통 `X64SEH`(드라이버 환경 없이도 사용하는 `NeverDEmulationWindowsException`)로 x64 `__C_specific_handler`와 UNWIND_INFO V1을 처리합니다. VEH 검색 후 필터, finally, 비지역 처리기 이동, 중첩/충돌 언와인딩과 재배치한 EXE/DLL 프레임을 지원하고 비휘발성 GPR/XMM을 보존합니다. 필터가 실행 재개를 선택하면 같은 `CONTEXT`로 VCH를 실행합니다. `WindowsSEHTests.cpp`는 독자적인 23개 시나리오를 네이티브 Windows와 비교하며 KVM/WHP/Unicorn은 같은 의미론을 사용합니다. 프로세스 예산 안에서 이미지 세대, 헤더, 언와인드/범위 바이트, 언어 처리기 코드 영역과 IAT 바인딩을 다시 검증합니다. 메타데이터 변경이나 보존된 이미지 언로드는 명시적으로 실패합니다. ARM64 프레임 SEH, C++ EH, 동적 함수 테이블, 일반 RtlUnwind/NtContinue 및 로더/VEH/VCH 콜백 경계를 넘는 언와인딩은 미지원입니다.

`EXCEPTION_NONCONTINUABLE`에 대해 x64 필터가 `EXCEPTION_CONTINUE_EXECUTION`을 반환하면 새 컨텍스트로 `STATUS_NONCONTINUABLE_EXCEPTION`(`0xc0000025`, 플래그 `0x81`, 연결 레코드 null)을 전달합니다. VEH를 다시 실행한 뒤 보존된 논리 스택을 다시 검색하며, 동일한 깊이 및 실행 예산 안에서 finally 순서와 EXE/DLL 프레임의 정체성을 유지합니다. 23개 네이티브 시나리오는 성공 실행 21개와 종료 2개를 포함합니다. 원래 `CONTEXT`를 복원해도 VEH/VCH가 이 2차 예외의 계속 실행을 수락하면 처리되지 않은 채 종료되며, 모델은 런타임 실패를 보고합니다. 소프트웨어 예외 주소는 저장된 PC와 같고 내부 디스패처 주소 및 레지스터 배치는 모델 정책입니다. [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

동적 해제 콜백 전에 모듈은 초기화 목록에서 빠지지만, 매핑·이름 조회·로드/메모리 목록 소속은 콜백 중에도 유지됩니다. 진입점 반환의 네이티브 비교는 시스템 작업 스레드와 별개로 초기 스레드를 관찰합니다.

`WindowsDynamicTests.cpp`는 원본 x64/ARM64 DLL과 EXE를 독립 네이티브 Windows 관측과 비교하여 참조 수, 공유 의존성, 중첩 로드, attach 실패 정리, 전달 조회, 프로세스 종료, 진입점 없는 DLL, 재로드 시 새 TLS를 확인합니다. 추가 회귀는 변경된 로더 메타데이터와 해제된 코드 포인터를 거부하고 누적 준비 예산과 중단 API의 미완료 결과를 보장합니다. Windows CI는 원본 기준 실행과 WHP 사례를 필수로 요구합니다. 교차 컴파일과 Unicorn ARM64는 네이티브 ARM64 실행 증거가 아닙니다.

`GetProcAddress` 전달 체인의 어느 위치에서든 라이브러리가 없으면 오류 127을 반환하며, 명시적 `LoadLibrary`로 카탈로그에 없는 모듈을 요청하면 126을 반환합니다. 네이티브 비교와 사용 가능한 각 백엔드는 선언된 41개 로더 시나리오 전체를 검증합니다. Windows에서는 DLL 변형마다 모든 DLL을 해제한 후의 진입점 반환을 16회 확인합니다. `GetProcAddress` 전달 대상 초기화 실패도 정리 후 127을 반환합니다. 프로세스 detach 콜백은 종료를 호출한 쪽의 스택 내용을 보존합니다.

`WindowsExportTests.cpp`는 원본 x64/ARM64 DLL과 EXE로 전달 코드/데이터/서수 호출, 별칭, 초기화 중 조회, 재배치, 대소문자별 누락, LastError, 순환 및 비상주 대상, 잘못된 포인터와 성공한 조회 이후 메타데이터 변경을 확인합니다. 같은 EXE를 독립 네이티브 Windows 기준으로 실행하며 네이티브 CI는 WHP 사례를 필수로 요구합니다. C ABI/CLI 테스트는 전체 보고서를 비교합니다. 네이티브 ARM64 하드웨어 증거는 아직 없습니다. 내보내기 표가 있는 EXE와 없는 EXE로 두 의존 그래프, PEB 목록 순서, detach 순서, 이름/서수/NULL 오류 코드를 확인합니다.

`WindowsLifetimeTests.cpp`는 고정된 추적을 독립 네이티브 Windows 프로세스 및 KVM/WHP/Unicorn 실행과 비교합니다. 정상 종료, 진입점 반환, 두 DLL의 초기화 실패, 네 곳의 조기 종료와 진입점 없는 DLL을 포함합니다. 콜백 오류, 공용 예산, 재배치 TLS 필드와 TLS 총용량도 확인합니다. 네이티브 진입점 반환 프로브는 초기 스레드 핸들을 보존하고 종료 코드와 정확한 스레드/프로세스 통지 순서를 64회 검증합니다. 남은 자식 스레드는 관찰 후 종료하며 프로세스 종료 코드를 진입점 반환값으로 취급하지 않습니다.

```json
{"windows":{"modules":[{"name":"middle.dll","path":"inputs/middle.dll"},{"name":"leaf.dll","path":"inputs/leaf.dll"}]}}
```

x64 GS와 ARM64 x18은 TEB를 가리키며 스택 경계, self, PID/TID, PEB, 프로세스 매개변수, LastError와 TLS를 제공합니다. UTF-8을 엄격히 UTF-16으로 변환하고 argv는 Microsoft CRT 규칙으로 인용합니다. 환경 이름은 ASCII이며 대소문자 무시 중복을 거부합니다. 값은 Unicode가 가능하며 정렬된 환경은 이중 NUL로 끝납니다. 호스트 환경과 파일 시스템은 상속하지 않습니다. 정적 TLS는 템플릿/BSS/32비트 인덱스를 초기화하고 동적 TLS는 별도 TEB 슬롯을 사용합니다. 시작·종료는 변경된 콜백 배열을 순서대로 읽으며 기한과 예산을 공유합니다. 정상 프로세스 종료는 종료 콜백을 실행합니다. 진입점 반환은 상주 게스트 DLL이 없을 때만 지원하며 프로세스 종료 정리 중 두 번째 `ExitProcess`는 지원하지 않습니다.

정확한 API는 `WindowsProcessServices.def`에 있습니다. `ExitProcess`, `RtlExitUserProcess`, 표준 출력 핸들과 동기 `WriteFile`, LastError, 프로세스/스레드 ID와 의사 핸들, `GetCommandLineW` / `HeapReAlloc`, 힙 할당/해제/크기, 동적 TLS, `LoadLibraryA` / `LoadLibraryW` / `FreeLibrary` / `GetModuleHandleA` / `GetModuleHandleW` / `GetProcAddress`를 지원합니다. `kernel32.dll`, `kernelbase.dll`, `ntdll.dll`의 정확한 이름만 해석합니다. 직접 syscall과 위조 콜백 게이트는 API를 선택하지 못합니다. 힙 소유권과 회수, 이진 출력, API 오류와 미지원 비동기 I/O·사용자 예외를 구분합니다. 포인터 별칭도 완료 수 초기 0과 실제 반환 주소 변경을 반영합니다.

`windows.native_calls`는 모듈/함수명, 선언된 스칼라 인수, nullable 결과를 기록하며 NT syscall 번호를 만들지 않습니다. `NeverDWindowsProcessTests`는 실제 PE, 컴파일러 TLS, 콜백 변경, 힙, 별칭, 잘못된 메타데이터, 권한과 예산을 검증합니다. `NeverDProcessPublicTests`는 CLI/C ABI를 검증합니다. Windows CI는 같은 EXE를 직접 실행해 독립 비교하고 WHP 검사도 필수입니다. 네이티브 ARM64 실행 증거에는 해당 머신이 필요합니다.

비어 있지 않은 입력 버퍼를 읽을 수 없으면 `WriteFile`은 `ERROR_INVALID_USER_BUFFER`(1784)를 반환하고, 기록한 바이트 수를 0으로 설정하며 아무 바이트도 출력하지 않습니다.

`windows.defer_unmodeled`는 모델이 구현하지 않은 로더 사실을 가진 이미지를 적재하고, 실행이 그중 하나에 의존할 때에만 멈춥니다. API 목록 밖의 익스포트와 카탈로그 밖의 모듈은 불투명 진입점에 바인딩됩니다. 각 식별자는 하나의 주소로 해석되며, 이를 실행하면 `module!export`를 지목하면서 `unsupported_service`로 멈춥니다. 모델이 해석하지 않는 디렉터리는 해석되지 않은 채로 남고, 파일이 뒷받침하지 않는 메타데이터는 적재 시 읽지 않으며, 그러한 이미지를 지나는 프레임 기반 예외 디스패치는 멈춥니다. `observeProcess`는 `ProcessObserver`를 추가합니다. 이는 프로세스 시작 시점과 각 실행 감시에서 멈춘 프로세스를 읽지만 게스트 상태를 바꿀 수 없으며, 이것이 실행을 끝내면 `observer`가 보고됩니다. [언패킹](unpack.md)은 이 두 가지 위에 만들어졌습니다.

[PE/COFF](https://learn.microsoft.com/windows/win32/debug/pe-format), [ARM64 ABI](https://learn.microsoft.com/cpp/build/arm64-windows-abi-conventions), [WriteFile](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-writefile), [TLS](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue), [Wine 10.0 loader](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/ntdll/loader.c). [GetProcAddress](https://learn.microsoft.com/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress).

<!-- i18n-section: verification -->

## 검증

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# shared-library/CLI 빌드:
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

테스트는 두 ISA의 독립 ELF 진입 어셈블리와 C를 컴파일해 data/BSS, 실제 시작 메타데이터, 시스템 호출 오류, 이진 출력, 권한 오류, 부분 쓰기, 미지원 서비스와 실행 구간 간 예산을 확인합니다. TLS는 독립 정렬 블록, BSS 초기화, 스레드 포인터 설치와 전환 후 보존을 검사하며 x64는 `arch_prctl` 오류 후 이전 베이스 보존도 검사합니다. 사용할 수 없는 백엔드는 건너뜀을 명시합니다. 공개 테스트는 공유 C ABI/CLI의 보고서와 종료 코드를 비교합니다. 정적 PIE는 자체 데이터/함수 포인터 재배치 전에 auxv와 원래 0인 RELA 슬롯을 검사합니다. 매핑 테스트는 분석 바이트를 선택할 때의 fixup 보존을, 동적 테이블은 section 부재 및 잘못된/의존 입력을 검사합니다. 익명 메모리 테스트는 두 ISA의 할당, 보호, 빈 영역, 재매핑, 힙 확장/축소 및 처리 가능한 호출 오류를 다룹니다. 실제 게스트 쓰기로 일반/부분 보호 변경 후 오류를 검사합니다. x64는 RW/RX 전환 사이에 같은 주소의 코드를 다시 쓰고 두 버전을 호출하며, 같은 ELF의 Linux 네이티브 실행을 독립 결과/오류 기준으로 사용합니다. 순수 메모리 테스트는 예산 소진, 회수, RAM을 유지하지 않는 권위 있는 매핑 스냅샷을 검사합니다. 교차 컴파일과 Unicorn ARM64 결과는 네이티브 ARM64 KVM/WHP 증거가 아닙니다.
