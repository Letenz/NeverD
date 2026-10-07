# 출시된 Android GKI 커널 계약

NeverD는 다른 Linux 변형보다 출시된 Android GKI 5.10–6.18 분기를 우선합니다. 프로세스 요청에서 분기를 명시적으로 선택합니다.

```json
{"linux_kernel":{"gki":"android17-6.18"},"linux_files":{"files":[],"descriptor_limit":16}}
```

Android 네이티브 프로필의 API 28 계약은 Bionic 가져오기를 설명하며 커널 버전을 선택하지 않습니다. GKI 선택은 구현된 `pidfd_open`, 벡터 출력, 인코딩된 프로세스 CPU 시계만 제어합니다. 전체 커널 인증이나 부팅, 장치·네임스페이스·자격 증명·프로세스 목록 추론은 제공하지 않으며 미지원 서비스는 명시적으로 중단합니다. [공식 GKI 릴리스 정책](https://source.android.com/docs/core/architecture/kernel/gki-releases)을 참조하세요.

## 고정 소스 버전

`LinuxGKIKernels.def`는 다음 공식 `r1` 태그를 사용하며 2026-10-07에 확인했습니다. 고정 커밋의 근거 파일은 `kernel/pid.c`, `include/uapi/linux/pidfd.h`, `arch/arm64/configs/gki_defconfig`, `kernel/fork.c`, `lib/iov_iter.c`, `fs/read_write.c`입니다. 플래그는 UAPI와 호출 검증에서 가져오며 Android API 수준이나 호스트 커널에서 추론하지 않습니다. CPU 시계의 고정 소스는 아래 표에 있습니다.

| 요청 분기 | 공식 출시 태그 | 고정 소스 커밋 | 허용 플래그 | Iovec 가져오기 |
| --- | --- | --- | --- | --- |
| `android12-5.10` | `android12-5.10-2026-07_r1` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/pid.c) | `PIDFD_NONBLOCK` (`0x800`) | [모든 메타데이터를 먼저 복사](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/lib/iov_iter.c) |
| `android13-5.10` | `android13-5.10-2026-07_r1` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/pid.c) | `PIDFD_NONBLOCK` | [모든 메타데이터를 먼저 복사](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/lib/iov_iter.c) |
| `android13-5.15` | `android13-5.15-2026-09_r1` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/pid.c) | `PIDFD_NONBLOCK` | [모든 메타데이터를 먼저 복사](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/lib/iov_iter.c) |
| `android14-5.15` | `android14-5.15-2026-07_r1` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/pid.c) | `PIDFD_NONBLOCK` | [모든 메타데이터를 먼저 복사](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/lib/iov_iter.c) |
| `android14-6.1` | `android14-6.1-2026-09_r1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/pid.c) | `PIDFD_NONBLOCK` | [모든 메타데이터를 먼저 복사](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/lib/iov_iter.c) |
| `android15-6.6` | `android15-6.6-2026-07_r1` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/pid.c) | `PIDFD_NONBLOCK` | [단일 버퍼 경로](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/lib/iov_iter.c) |
| `android16-6.12` | `android16-6.12-2026-09_r1` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/pid.c) | `PIDFD_NONBLOCK` 및 `PIDFD_THREAD` (`0x80`) | [단일 버퍼 경로](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/lib/iov_iter.c) |
| `android17-6.18` | `android17-6.18-2026-09_r1` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/pid.c) | `PIDFD_NONBLOCK` 및 `PIDFD_THREAD` | [단일 버퍼 경로](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/lib/iov_iter.c) |

이 고정 버전이 계약을 정의합니다. 새 출시나 백포트에는 소스 확인과 회귀 검증이 필요합니다. GKI 선택과 `pidfd_open` 부재 관측의 조합은 모순이므로 로드 전에 거부합니다.

## 구현된 프로세스 설명자 범위

x64/AArch64 원시 트랩과 Bionic `syscall`은 `LinuxServices` 및 워크로드 소유 설명자 표를 공유합니다. PID/플래그의 하위 32비트를 사용하며 알 수 없는 플래그나 부호 있는 비양수 PID는 할당 전에 `EINVAL`을 반환합니다. 선택적인 `tasks` 배열은 다른 살아 있는 게스트 태스크의 고정된 폐쇄 목록입니다.

```json
{"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true},{"id":3000,"group_leader":false}]},"linux_files":{"files":[],"descriptor_limit":16}}
```

현재 그룹 리더 PID 1000은 빈 배열에도 암묵적으로 포함됩니다. 목록 생략 시 다른 대상 검색은 미지원이며, 선언된 목록 밖의 유효한 양수 PID는 할당 전에 `ESRCH`를 반환합니다. `PIDFD_THREAD` 없는 살아 있는 비리더는 고정 5.10–6.12에서 `EINVAL`, [6.18의 `pidfd_prepare`](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/fork.c)에서 `ENOENT`입니다. 허용 플래그를 쓰면 6.12/6.18에서 선언된 비리더를 열 수 있습니다. 플래그 검증이 대상 검색보다 먼저입니다.

각 항목은 1..2147483647 정수 `id`와 불리언 `group_leader`를 요구하며 최대 4096개입니다. 중복, 추가 필드, PID 1000 비리더 선언, GKI 없는 목록은 거부합니다. 우선순위 관측은 목록 태스크 또는 현재 프로세스를 지정해야 합니다. 고정 목록은 Android 협력 스레드 모드(`thread_limit > 1`)와 결합할 수 없습니다. 생성·회수·자격 증명·네임스페이스 변환에는 별도의 수명 소유권 구현이 필요합니다.

`linux_files`가 필요합니다. 일반 파일과 pidfd가 소유권과 한도를 공유하며 가장 작은 빈 번호를 할당합니다. 고갈 시 `EMFILE`, `close`는 해제, 이중 닫기는 `EBADF`입니다. 닫힌 표준 스트림 번호는 재사용할 수 있습니다. 호스트 pidfd·파일 시스템·프로세스 검색은 실행하지 않습니다.

유효한 pidfd에서 `read`/`write`는 데이터 접근 전에 `EINVAL`, `lseek`는 기준점 검증 뒤 `ESPIPE`를 반환합니다. `writev`는 먼저 메타데이터와 사용자 범위를 검증하므로 미지원 쓰기의 `EINVAL`보다 `EFAULT`가 먼저 나올 수 있습니다. 데이터를 읽거나 출력을 수집하지 않습니다. stdout/stderr도 같은 버전별 가져오기를 사용합니다. [VFS 순서](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/fs/read_write.c)를 참고하십시오.

5.10/5.15/6.1은 iovec 전체를 복사한 뒤 길이를 검사합니다. 앞의 음수 길이와 뒤의 접근 불가 메타데이터 조합은 `EFAULT`입니다. 단일 벡터도 전송 한도 적용 전에 원래 범위를 검사합니다. 6.6/6.12/6.18은 순차 검사하여 같은 경우 `EINVAL`을 반환합니다. 단일 버퍼는 먼저 제한하고, 여러 벡터는 모든 원래 범위를 검사합니다. 근거는 `copy_iovec_from_user`, `__import_iovec`, `import_ubuf`입니다. GKI 미선택 시 기존 단일 버퍼 정책을 유지하며 커널 버전을 추론하지 않습니다.

Bionic은 원시 음수 오류를 `-1`과 스레드 로컬 `errno`로 바꾸며 성공 시 `errno`를 보존합니다. `fstat`용 메타데이터는 없습니다. 폴링·종료 알림·pidfd 신호 전달·`pidfd_getfd`·`fcntl`·pidfs ioctl·미관측 태스크 검색은 미지원입니다. pidfd에서 스케줄링이나 프로세스 수명을 추론하지 않습니다.

## 구현된 프로세스 CPU 시계 범위

GKI를 명시적으로 선택하면 `clock_gettime`은 PROF, VIRT, SCHED의 음수 인코딩 프로세스 CPU 시계 ID를 허용하며 인자의 하위 32비트를 부호 있게 해석합니다. PID와 종류가 `linux_time`의 명시적 표본을 식별합니다. 현재 프로세스는 암묵적으로 존재하며 외부 프로세스 표본을 제공하려면 닫힌 작업 목록에 살아 있는 그룹 리더로 선언해야 합니다.

```json
{"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true}]},"linux_time":{"advance_on_idle":true,"clocks":[{"id":1,"seconds":10,"nanoseconds":0},{"id":2,"seconds":3,"nanoseconds":4},{"id":-16006,"seconds":7,"nanoseconds":9}]}}
```

`-16006`은 PID 2000의 SCHED입니다. PROF와 VIRT는 독립 관측입니다. 현재 프로세스의 SCHED ID 2, -6(PID 0), -8006(PID 1000)은 같은 표본을 공유합니다. PROF 별칭은 -8/-8008, VIRT는 -7/-8007입니다. 값이 같아도 중복 별칭은 거부합니다. CPU 초는 음수가 아니어야 하고 나노초는 정규화해야 합니다. 유휴 진행은 벽시계 ID 0, 1, 7만 바꾸며 CPU 표본은 고정됩니다. 명령 실행으로 CPU 사용량을 추론하지 않습니다.

현재 작업의 TID도 해당 프로세스 그룹을 식별하며 외부 목록이 없는 Android 협력 스레드에도 적용됩니다. 닫힌 목록에 없는 외부 PID나 살아 있는 비리더는 출력 접근 전에 `EINVAL`을 반환합니다. 목록 생략 시 외부 조회와 알려진 그룹의 표본 누락은 출력 전에 미지원으로 중단합니다. 잘못된 종류는 `EINVAL`, 유효한 표본의 사용자 복사는 `EFAULT`를 반환할 수 있습니다. 원시 트랩은 음수 오류를 유지하며 Bionic만 errno를 갱신하고 -1을 반환합니다.

대상과 종류 규칙은 각 고정 릴리스의 `pid_for_clock`, `posix_cpu_clock_get`, 시계 분배기와 ID 정의를 따릅니다.

| 요청 브랜치 | 프로세스 CPU 시계 소스 |
| --- | --- |
| `android12-5.10` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/time/posix-cpu-timers.c) |
| `android13-5.10` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/time/posix-cpu-timers.c) |
| `android13-5.15` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/time/posix-cpu-timers.c) |
| `android14-5.15` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/time/posix-cpu-timers.c) |
| `android14-6.1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/time/posix-cpu-timers.c) |
| `android15-6.6` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/time/posix-cpu-timers.c) |
| `android16-6.12` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/time/posix-cpu-timers.c) |
| `android17-6.18` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/time/posix-cpu-timers.c) |

FD 시계 판별과 CPU 라우팅도 고정된 [6.18 분배기](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/time/posix-timers.c) 및 [시계 ID 정의](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/include/linux/posix-timers_types.h)를 따릅니다. FD 기반 시계와 인코딩된 스레드별 CPU 시계는 미지원입니다. 목록은 고정 게스트 관측이며 권한, 네임스페이스, 프로세스 수명과 CPU 계측 확장은 각자의 명시적 계약이 필요합니다.

## 검증 및 남은 범위

`LinuxPIDFDTests.cpp`는 독립적인 x64/AArch64 O0/O2 ELF 호출자를 여덟 분기와 사용 가능한 백엔드에서 실행하여 플래그·공유 표·한도와 재사용·오류 순서·메타데이터 실패·길이 한도와 원래 범위·목록 생략과 폐쇄·비리더·FD 고갈 전 검색을 검사합니다. `AndroidSyscallTests.cpp`는 일반·Android packed·RELR의 여섯 O0/O2 구성에서 raw/Bionic 소유권·검색·errno를 반복 검증합니다. 소스와 모델 실행은 이 하위 계약의 증거이며 모든 고정 GKI 이미지를 부팅하는 네이티브 검증은 없습니다. 다른 Linux 확장도 서비스마다 버전·구성·관측 증거를 보존해야 합니다.

CPU 시계 테스트는 식별과 출력 순서, 독립 종류, 명시적 표본 검증, 벽시계 유휴 진행과의 분리를 확인합니다. `AndroidTimeTests.cpp`는 이름 기반/원시 출력과 경계 감시값을, 협력 syscall은 현재 비리더 TID 별칭을 확인합니다.
