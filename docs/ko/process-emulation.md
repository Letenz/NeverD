**언어**: [English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← 문서 색인](README.md)

# 게스트 프로세스 에뮬레이션

`neverd emulate`는 명시적 게스트 OS 프로필로 이미지를 실행합니다. CPU 전송, 이미지 파싱, 프로세스 진입, OS 서비스는 각각 별도 소유 경계입니다. `NEVERD_ENABLE_CPU_EMULATION=ON`으로 활성화합니다. 드라이버 에뮬레이션에도 포함됩니다.

첫 프로필 `linux-elf64-v1`은 CPL3 또는 EL0에서 x64/AArch64 freestanding ELF `ET_EXEC`를 실행합니다. 실제 ELF 세그먼트를 적재하고 초기 스택을 만들며 명령 quantum으로 재개하고 명시적 Linux 시스템 호출 요청을 처리합니다. 이는 완전한 Linux 배포판이나 임의 libc 바이너리 실행을 보장하지 않는 독립 프로세스 모델입니다. 동적 링크, `PT_TLS`, 시그널, 스레드, FP/SIMD, 미지원 서비스는 명시적으로 실패합니다. Windows, Android, Darwin 및 기타 커널 작업은 별도입니다.

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

`schema_version`은 1입니다. 보고서에는 프로필, 아키텍처, 선택 백엔드와 이유, `stop_reason`, nullable `exit_status`, 진단, 진입/현재 PC, 카운터, 서비스 기록, 마지막 typed CPU exit가 포함됩니다. 주소, syscall 번호, 인자 레지스터, raw 반환 비트는 `0x` 없는 16진수 문자열입니다. `stdout_hex`/`stderr_hex`는 NUL과 잘못된 UTF-8을 보존합니다. syscall 결과 null은 모델링된 반환이 없다는 뜻(exit 또는 미지원 요청 등)이지 성공한 0이 아닙니다.

## Linux 프로필 의미론

OS 정책은 기존 ELF 로더가 디코딩한 프로그램 헤더를 사용합니다. ABI 태그, 세그먼트 정렬, 매핑된 프로그램 헤더 테이블, user 주소 범위를 검증합니다. 매핑 계획은 할당 전에 범위, 권한, 겹침, 예산을 확인하고 완전히 준비한 전용 주소 공간만 공개합니다. 파일 페이지 앞/뒤 바이트를 보존하고 BSS를 0으로 채우며 세그먼트 권한을 지키고 스택 guard gap을 예약합니다. 페이지가 겹치는 레이아웃과 모순 헤더는 추측하지 않고 거부합니다.

초기 스택은 정렬된 argc/argv/envp/auxv, PHDR/PHENT/PHNUM, entry, 페이지 크기 및 identity 값을 포함합니다. 모델 PID/TID/UID/GID는 1000입니다. 재현성을 위해 `AT_RANDOM`은 입력 SHA-256의 첫 16바이트입니다. 이는 암호학적 엔트로피가 아닌 결정적 모델 정책입니다. HWCAP/HWCAP2는 0이며 vDSO는 없습니다.

`write`, `exit`, `exit_group`, `getpid`, `gettid`를 구현하며 번호는 [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl)와 [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h)에서 다릅니다. 반환하는 x64 SYSCALL은 RCX/R11 clobber, RAX, 다음 PC를 반영합니다. ARM64는 번호에 x8, 결과에 x0을 사용합니다. 나머지는 `unsupported_service`로 중단하며 호스트 syscall을 실행하지 않습니다.

파일 디스크립터 1과 2는 가상 바이트 sink입니다. `write`는 읽기 가능한 user 페이지를 검증하고, 뒤쪽 페이지 접근이 막히면 읽을 수 있는 prefix를 반환하며 한 바이트도 읽지 못하면 게스트 `EFAULT`를 반환합니다. 잘못된 디스크립터는 `EBADF`; 유효한 디스크립터에 0바이트 write는 포인터를 읽지 않습니다. Linux pipe 원자성이나 파일 객체는 모델링하지 않습니다. 출력 한도를 넘을 쓰기는 게시 전에 중단됩니다.

## 검증

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate)Tests$' --output-on-failure
# shared-library/CLI 빌드:
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

두 ISA의 원본 ELF entry assembly/C를 컴파일합니다. 데이터/BSS, 시작 메타데이터, syscall 오류, 바이너리 출력, 권한 fault, 부분 쓰기, 미지원 서비스, quantum 간 예산 보존을 검사합니다. 사용할 수 없는 백엔드는 명시적 skip입니다. 공개 테스트는 C ABI/CLI와 보고서/종료 코드 일치를 확인합니다. 크로스 컴파일과 Unicorn ARM64는 네이티브 ARM64 KVM/WHP 증거가 아닙니다.
