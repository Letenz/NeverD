**언어**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 813c9673241230afbb295a950aab1e14478b4bd4fe9de2d2f2e27b6fbe34f588 -->

[← 문서 목록](README.md)

# macOS 및 iOS 게스트 프로세스 환경

`lib/emulation/os/darwin/`은 호스트 CPU 전송 계층과 별개로 제한된 독립 Mach-O 프로세스를 모델링합니다. `NEVERD_ENABLE_CPU_EMULATION`을 켜면 되며 Windows 드라이버 에뮬레이션은 필요하지 않습니다. `macos/`와 `ios/`가 명시적 플랫폼 프로필을 정의합니다.

| 프로필 | Mach-O 플랫폼 | 게스트 ISA | OS 페이지 |
| --- | --- | --- | --- |
| `macos-macho64-v1` | macOS | x86-64, 기본 ARM64 | x64 4 KiB; ARM64 16 KiB |
| `ios-macho64-v1` | iOS 기기 | 기본 ARM64 | 16 KiB |
| `ios-simulator-macho64-v1` | iOS Simulator | x86-64, 기본 ARM64 | x64 4 KiB; ARM64 16 KiB |

기기용 바이너리는 시뮬레이터 이미지가 아니며 호스트에서 게스트 플랫폼을 추론하지 않습니다. macOS에서 ISA가 같으면 [HVF](macos-hvf.md)를 쓸 수 있고, 다르면 `auto`가 Unicorn을 선택합니다. CPU 매핑 단위는 4 KiB로 유지합니다. [C, Python, CLI API](process-emulation.md)는 옵션, 제한 및 보고서를 공유합니다.

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

## 이미지와 시작

`MachOExecutionImage`는 분석의 재배치 패치 없이 원래 바이트를 보존합니다. 플랫폼과 진입점이 명확한 thin little-endian `MH_EXECUTE`만 허용합니다. 유니버설 이미지는 필요한 아키텍처 슬라이스를 명시적으로 추출해야 합니다.

메타데이터와 끝부분 바이트를 포함한 전체 파일이 파싱이나 복사 전에 `memory_limit` 안에 들어가야 합니다. 로더는 일반 파일에서 크기가 제한된 전용 스냅샷을 읽고 NUL 경로, 짧은 읽기, 크기 변경을 거부합니다. 살아 있는 파일 매핑은 유지하지 않습니다. 파일과 게스트 메모리는 같은 값의 별도 한도를 가지며 호스트 파일 I/O는 엄격한 실시간 기한을 보장하지 않습니다.

세그먼트는 현재/최대 권한과 0 채움을 유지합니다. `__PAGEZERO`는 큰 물리 메모리 할당 없이 주소를 예약합니다. 파일/VM 범위, OS 페이지 정렬, 반올림한 겹침, 헤더 소유권, 실행 가능한 진입점과 예산을 검사합니다. 헤더 세그먼트는 읽기 및 실행 가능해야 하며 보호 페이지와 전용 복귀 게이트는 예약됩니다. 마지막 파일 페이지는 페이지 경계 또는 EOF까지 원본 바이트를 유지하고 후속 완전한 VM 페이지는 0으로 채웁니다. 근거는 [XNU 로더](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c)입니다.

`LC_MAIN`은 `argc`, `argv`, `envp`, apple 벡터를 정수 인수 네 개로 받고 반환값의 하위 8비트를 종료 상태로 사용합니다. `/usr/lib/dyld`는 가져오기가 없는 이 진입 전달에서만 허용하며 호스트 dyld는 실행하지 않습니다. 0이 아닌 `stacksize`는 거부합니다. 예산은 호출자의 명시적 `stack_size`가 결정합니다.

`LC_UNIXTHREAD`는 PC만 설정된 완전한 네이티브 64비트 일반 레지스터 레코드 하나를 요구합니다. 초기 스택에는 argc, 종료된 argv/envp, `executable_path=<input filename>`을 포함하는 종료된 apple 벡터가 있습니다. 사용자 SP/플래그, 다른 레지스터, 추가 flavor, 충돌하는 진입점은 거부합니다. 호스트 환경이나 Linux 보조 벡터를 상속하지 않습니다. [dyld 구조](https://github.com/apple-oss-distributions/dyld/blob/main/doc/dyld4.md)를 참고하세요.

외부 dylib, 가져오기, rebases/chained fixups, 생성자/소멸자, TLS 섹션, arm64e/PAC, 미지원 CPU 하위 유형, 암호화 및 모델링하지 않은 로드 명령은 실행 전에 실패합니다. fixup 없는 PIE는 선호 주소를 사용하며 ASLR이 아닙니다. 서명 blob은 메타데이터이며 AMFI나 entitlement 정책을 구현하지 않습니다.

## Darwin 서비스

ARM64는 X16, X0–X5와 `svc #0x80`을 사용하고 x64는 BSD 클래스 `0x02000000`, RAX, RDI/RSI/RDX/R10/R8/R9를 사용합니다. 성공 시 carry를 지우고 오류 시 carry와 양수 errno를 반환합니다. ARM64는 X1을 지우고 x64는 성공 시 RDX를 지우며 오류 시 보존합니다. SYSCALL의 레지스터 변경은 명시적입니다. 보고서의 `result`와 `error=true`는 BSD 오류를 나타내며 반환하지 않거나 미지원인 요청에는 두 필드가 없습니다. XNU [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c)와 [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c)의 규칙을 따르며 Apple 구현 코드를 포함하지 않습니다.

지원 서비스는 `exit`, `write`, `getpid`, `getppid`, `getuid`, `geteuid`, `getgid`, `getegid`, `mmap`, `mprotect`, `munmap`입니다. PID/UID/GID는 1000, PPID는 1입니다. 설명자 1과 2는 NUL과 비 UTF8을 포함한 바이트를 캡처하고 닫혔거나 읽기 전용인 설명자는 EBADF를 반환합니다. 부분 복사된 바이트는 유지하지만 이후 오류는 EFAULT로 남습니다. 길이가 `INT_MAX`를 넘으면 설명자, 포인터, 예산을 검사하기 전에 EINVAL을 반환합니다. 근거는 [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c)입니다.

메모리 서비스는 `flags=0x1002`, 설명자 -1, 오프셋 0의 전용 익명 데이터 매핑을 지원합니다. 길이와 고정되지 않은 주소 힌트는 OS 페이지로 올림합니다. 점유된 힌트는 높은 주소부터 검색한 뒤 기본 배치로 돌아갑니다. 기존 raw mmap은 길이 0에서 할당 없이 0을 반환하며 `MAP_UNIX03`은 제외됩니다. Unmap/protect는 정렬된 주소를 요구합니다. NONE/READ/WRITE를 지원하고 WRITE는 READ를 포함합니다. 물리 메모리는 OS 페이지별로 소유하므로 부분 해제는 예산을 반환하고 새 페이지는 0으로 채워집니다. 빈 구간이나 최대 권한을 넘는 protect가 실패하면 전체 범위를 변경하지 않습니다. 출처: [XNU VM 서비스](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

파일/공유/고정/JIT 매핑, 실행 가능한 익명 메모리, Mach trap, 간접 syscall, 스레드, 신호, 호스트 파일/네트워크, dyld, Objective-C/Swift runtime, Foundation/UIKit은 지원하지 않으며 명시적으로 중지합니다. 전체 Apple OS나 iOS Simulator 애플리케이션이 아닙니다.

## 검증

직접 작성한 C 테스트 입력을 Clang과 `ld64.lld`로 만들며 Apple SDK나 독점 바이너리가 필요하지 않습니다. 플랫폼/ISA 다섯 조합, 잘못된 Mach-O 레코드, 4/16 KiB 페이지, 예산이 가득 찬 상태의 부분 해제를 검사합니다. `NeverDProcessPublicTests`가 C API/CLI를 비교하며 `NEVERD_TEST_LIBNEVERD`와 `NEVERD_TEST_DARWIN_FIXTURES`는 Python SDK의 동일한 다섯 조합을 활성화합니다.

## 명시적 파일 입력과 설명자

`darwin_files`는 세 프로필에 닫힌 읽기 전용 파일 목록을 제공합니다. 필수 `files` 항목에는 정규 절대 게스트 `path`와 16진수 `bytes_hex`가 있으며 선택적 `stdin_hex`는 유한 입력입니다. 입력 생략은 알 수 없는 상태로 비영 읽기를 중지하며 빈 문자열은 EOF입니다. 목록 생략 시 open을 중지하고 명시적 빈 목록은 ENOENT를 반환합니다. 호스트 파일이나 입력을 사용하지 않습니다.

`open`, `read`, `pread`, `lseek`, `close`, `dup`, `dup2`, `fcntl`을 추가합니다. read/write/open/close/fcntl/pread의 nocancel도 같은 구현을 사용합니다. O_RDONLY/O_CLOEXEC와 F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL을 지원합니다. 개별 open은 독립 위치를, dup은 공유 위치와 독립 close-on-exec 플래그를 가지며 pread는 위치를 바꾸지 않습니다. 0/1/2의 닫기와 교체는 이후 I/O에 적용되고 복제 출력은 원래 캡처 대상과 예산을 유지합니다.

최대 256개 파일, 경로/NUL/파일/입력 합계 16 MiB, 1024바이트 미만 경로와 255바이트 이하 구성 요소를 허용합니다. 배타적 상한 `descriptor_limit`은 3–4096, 기본 256이며 JSON은 64 KiB입니다. 잘못된 설정은 로드 전에 거부합니다. INT_MAX 초과 읽기는 FD 조회 전에 EINVAL이며 EOF는 목적지에 접근하지 않고 잘못된 목적지는 EFAULT입니다. 일부만 쓰기 가능한 버퍼는 복사와 위치 변경 전에 중지합니다. SET/CUR/END 실패는 위치를 보존합니다. 상대 경로, 디렉터리 열기, 쓰기, stat, 파일 매핑, 희소 seek와 기타 fcntl은 미지원입니다. 파일을 경로 조상으로 쓰면 ENOTDIR입니다. 같은 오브젝트를 네이티브 macOS와 비교하고 C/CLI/Python으로 다섯 게스트 조합을 검사하며 iOS 실기기 검증은 아닙니다.

2026-10-05 Release 검증은 381개 중 177개 통과, 204개 건너뜀, 실패 0이며 ARM64 HVF 필수 51/51을 실행했습니다. 네이티브 macOS 7개 프로그램, 공개 C/CLI 및 보고서 35개, Python 다섯 게스트 조합과 검증 스크립트 66개도 통과했습니다. 집계는 겹칩니다. 새 파일 서비스의 Intel HVF/KVM/WHP 네이티브 증거는 없으며 Intel HVF는 미검증 상태로 Actions가 중지되어 있습니다. iOS SDK와 실기기 대조도 없습니다.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

독립 워크로드 검증은 ARM64 51개 또는 x64 34개 네이티브 사례를 모두 요구하며 각 플랫폼의 `LC_MAIN`과 `LC_UNIXTHREAD`를 포함합니다. 필수 항목 누락, 건너뛰기 또는 `ld64.lld` 부재는 실패입니다.

```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

Linux에서는 `kvm`, Windows에서는 `whp`를 사용합니다. [Darwin 워크플로](../../.github/workflows/darwin-native.yml)는 Unicorn 없이 두 x64 전송 계층을 검사하고 개별 재실행도 지원합니다. [커널 참조](../../.github/workflows/darwin-kernel-reference.yml)는 NeverD/LLVM 없이 두 macOS ISA에서 프로그램을 직접 실행합니다. `DarwinNativeCases.def`가 모드, 종료 상태, 예상 바이트를 정의합니다. 실제 dyld 진입을 위해 libSystem을 링크하는 것은 호스트 참조뿐입니다. ISA 불일치, Rosetta, 시간 초과 또는 결과 차이는 실패이며 iOS 실기기 커널 증거가 아닙니다.

## 증거와 남은 범위

2026-10-03 결과입니다. 겹치는 행은 합산하지 않습니다.

| 전송 계층 | 소스 | 통과 | 실패 | 건너뜀 | 네이티브 워크로드 |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 HVF | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel HVF | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| x64 KVM | `36e11ca8a` | 51 | 0 | 235 | 26/26 |
| x64 WHP | `36e11ca8a` | 51 | 0 | 235 | 26/26 |

[Intel 실행](https://github.com/NeverSight/NeverD/actions/runs/37106013999)은 CTest 식별자 286개와 프로세스 32개를 원본 XML과 대조했습니다. 건너뛴 234개는 비활성 Unicorn 65개, ARM64 게스트 39개, 다른 호스트 플랫폼 130개입니다. 산출물 `11267489438`의 SHA-256은 `cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`로 검증했습니다. [KVM/WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703)도 독립 확인했습니다. [커널 참조](https://github.com/NeverSight/NeverD/actions/runs/37064795867)는 ISA마다 4/4 프로그램을 통과했으며 종료 상태 37, 정확한 출력, 빈 stderr를 확인했습니다.

Unicorn을 켠 C API/CLI는 138개 통과, 156개 건너뜀, 실패 0개입니다. Python은 다섯 조합을 다룹니다. 패키지 엔진은 ARM64 CLI 보고서 18개와 일치하고 Mach-O 186개 서명을 검증했습니다. HVF/Unicorn OFF는 38개 통과, 231개 건너뜀이며 Hypervisor.framework를 링크하지 않습니다. 이는 통합 증거로 추가 네이티브 실행 수에 포함하지 않습니다. 전체 Intel CPU는 아직 검증되지 않았습니다. [HVF](macos-hvf.md)와 [상세 기록](../darwin-emulation.md#hosted-native-verification-2026-10-03)을 참고하세요.
