**언어**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: e6d2ea5c79823e0881de4fbdb33b27672ef1858ba61ddd1ad54e1477b2e20d85 -->

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

메모리 서비스는 `flags=0x1002`, 설명자 -1, 오프셋 0의 전용 익명 데이터 매핑을 지원합니다. 길이와 고정되지 않은 주소 힌트는 OS 페이지로 올림합니다. 점유된 힌트는 높은 주소부터 검색한 뒤 기본 배치로 돌아갑니다. 기존 raw mmap은 길이 0에서 할당 없이 0을 반환하며 `MAP_UNIX03`을 지원하며 길이 0은 EINVAL입니다. Unmap/protect는 정렬된 주소를 요구합니다. NONE/READ/WRITE를 지원하고 WRITE는 READ를 포함합니다. 물리 메모리는 OS 페이지별로 소유하므로 부분 해제는 예산을 반환하고 새 페이지는 0으로 채워집니다. 빈 구간이나 최대 권한을 넘는 protect가 실패하면 전체 범위를 변경하지 않습니다. 출처: [XNU VM 서비스](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

공유/고정/JIT 매핑, 실행 가능한 익명 메모리, Mach trap, 간접 syscall, 스레드, 신호, 호스트 파일/네트워크, dyld, Objective-C/Swift runtime, Foundation/UIKit은 지원하지 않으며 명시적으로 중지합니다. 전체 Apple OS나 iOS Simulator 애플리케이션이 아닙니다.

## 검증

직접 작성한 C 테스트 입력을 Clang과 `ld64.lld`로 만들며 Apple SDK나 독점 바이너리가 필요하지 않습니다. 플랫폼/ISA 다섯 조합, 잘못된 Mach-O 레코드, 4/16 KiB 페이지, 예산이 가득 찬 상태의 부분 해제를 검사합니다. `NeverDProcessPublicTests`가 C API/CLI를 비교하며 `NEVERD_TEST_LIBNEVERD`와 `NEVERD_TEST_DARWIN_FIXTURES`는 Python SDK의 동일한 다섯 조합을 활성화합니다.

## 명시적 파일 입력과 설명자

`darwin_files`는 세 프로필에 닫힌 읽기 전용 파일 목록을 제공합니다. 필수 `files` 항목에는 정규 절대 게스트 `path`와 16진수 `bytes_hex`가 있으며 선택적 `stdin_hex`는 유한 입력입니다. 입력 생략은 알 수 없는 상태로 비영 읽기를 중지하며 빈 문자열은 EOF입니다. 목록 생략 시 open을 중지하고 명시적 빈 목록의 없는 절대 경로는 ENOENT를 반환합니다. 호스트 파일이나 입력을 사용하지 않습니다.

`open`, `read`, `pread`, `lseek`, `close`, `dup`, `dup2`, `fcntl`을 추가합니다. read/write/open/close/fcntl/pread의 nocancel도 같은 구현을 사용합니다. O_RDONLY/O_CLOEXEC와 F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL을 지원합니다. 개별 open은 독립 위치를, dup은 공유 위치와 독립 close-on-exec 플래그를 가지며 pread는 위치를 바꾸지 않습니다. 0/1/2의 닫기와 교체는 이후 I/O에 적용되고 복제 출력은 원래 캡처 대상과 예산을 유지합니다.

최대 256개 파일, 경로/NUL/파일/입력 합계 16 MiB, 1024바이트 미만 경로와 255바이트 이하 구성 요소를 허용합니다. 배타적 상한 `descriptor_limit`은 3–4096, 기본 256이며 JSON은 64 KiB입니다. 잘못된 설정은 로드 전에 거부합니다. INT_MAX 초과 읽기는 FD 조회 전에 EINVAL이며 EOF는 목적지에 접근하지 않고 잘못된 목적지는 EFAULT입니다. 일부만 쓰기 가능한 버퍼는 복사와 위치 변경 전에 중지합니다. SET/CUR/END 실패는 위치를 보존합니다. 쓰기, 구형 stat, 희소 seek와 기타 fcntl은 미지원입니다. 파일을 경로 조상으로 쓰면 ENOTDIR입니다. 같은 오브젝트를 네이티브 macOS와 비교하고 C/CLI/Python으로 다섯 게스트 조합을 검사하며 iOS 실기기 검증은 아닙니다.

2026-10-05 Release 검증은 381개 중 177개 통과, 204개 건너뜀, 실패 0이며 ARM64 HVF 필수 51/51을 실행했습니다. 네이티브 macOS 7개 프로그램, 공개 C/CLI 및 보고서 35개, Python 다섯 게스트 조합과 검증 스크립트 66개도 통과했습니다. 집계는 겹칩니다. 새 파일 서비스의 Intel HVF/KVM/WHP 네이티브 증거는 없으며 Intel HVF는 미검증 상태로 Actions가 중지되어 있습니다. iOS SDK와 실기기 대조도 없습니다.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## 디렉터리와 상대 경로

선택적 `directories`는 정규 절대 `path`와 선택적 전체 `metadata`로 빈 디렉터리를 지정합니다. 루트와 조상은 암시적이며 메타데이터가 없는 경로를 만들지는 않습니다. mode는 `0x4000`과 권한, size는 [0, INT64_MAX]의 명시적 관측입니다. `working_directory`는 기존 정규 디렉터리여야 하며 생략한 CWD는 미지정입니다. 호스트에서 상속하지 않습니다. 지정 경로는 조상 메타데이터를 포함해 최대 256개, 경로/NUL/내용/입력/CWD 합계는 16 MiB입니다.

`openat` (463), `openat_nocancel` (464), `chdir` (12), `fchdir` (13), `fstatat64` (470)는 해석기를 공유합니다. 상대 경로는 디렉터리 FD 또는 `AT_FDCWD=-2`를 사용하고 절대 경로는 FD를 무시합니다. 반복 슬래시, `.`, `..`, 끝 슬래시도 조상을 검사하여 `/file/..`는 ENOTDIR, `/missing/..`는 ENOENT입니다. 실패나 원래 FD의 닫기·재사용·교체는 CWD를 바꾸지 않습니다. `F_GETPATH=50`은 복제 FD에도 정규 경로와 NUL을 복사하며 이후 바이트는 보존합니다.

디렉터리 read/pread는 길이 0에도 EISDIR이며 음수 pread 오프셋은 EINVAL이 우선합니다. SET/CUR은 커서를 공유하고 END는 명시 size가 필요하며 mmap은 EINVAL입니다. fstatat64는 0, `AT_SYMLINK_NOFOLLOW=0x20`, `AT_SYMLINK_NOFOLLOW_ANY=0x800`, `AT_FDONLY=0x400`(경로 무시)을 지원합니다. 잘못된 비트는 EINVAL, `AT_REALDEV=0x200`은 미지원입니다. 스트림 정체성은 미지정이며 권한은 접근 제어 모델이 아닙니다. 쓰기는 남은 작업입니다. 같은 `directories`를 네이티브와 다섯 게스트로 비교하고 stat은 실제 파일과 디렉터리를 비교합니다. Intel HVF Actions는 중지 상태입니다.

[XNU VFS](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/fcntl.h).

디렉터리 검증(2026-10-05, Release): 등록 467건, 통과 227, 건너뜀 240, 실패 0이며 ARM64 HVF 필수 60/60건을 실행했습니다. 네이티브 macOS 프로그램 10개, 공개 C/CLI/보고서 37건(건너뜀 없음), Python 다섯 게스트, 검증 스크립트 66건이 통과했습니다. 집계는 중복됩니다. 증거: `build-hvf-arm64/darwin-directory-verified-evidence/`. 다른 네이티브 백엔드와 iOS 실기기는 미검증입니다.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## 명시적 디렉터리 스냅샷

`getdirentries64` (344)는 기존 `directories` 항목의 선택적 불변 `contents`를 열거합니다. C++에서는 `DarwinFileOptions::DirectoryContents`를 사용합니다. `entries`는 `.`과 `..`, 모든 직접 자식을 명시한 순서대로 포함해야 합니다. 빈 디렉터리도 스냅샷이 없으면 미지정입니다. 경로나 stat 정보를 생성하거나 호스트를 조회하지 않습니다.

각 항목은 `name`, 0이 아닌 `inode`, `type`(0 미지정, 4 디렉터리, 8 일반 파일), `next_offset`, `seek_offset`이 필수입니다. 타입은 경로와, 같은 경로의 inode는 다른 스냅샷·메타데이터와 일치해야 합니다. `next_offset`은 디렉터리 내 유일한 0 초과 INT64_MAX 이하 값이며 증가할 필요는 없습니다. 0은 되감기입니다. 별도 관측값 `seek_offset`은 부호 없는 64비트 d_seekoff이며 중복 0도 허용합니다. 정수는 stat과 같은 무손실 십진 문자열 규칙을 사용합니다.

필수 `contents.minimum_buffer_size`는 EOF를 포함한 페이로드 최소값 1–128 MiB입니다. 항목별 선택적 `minimum_buffer_size`(기본 0)는 해당 위치에서 시작할 때 추가 제한입니다. 예제는 APFS 시작 점 항목 쌍에 64바이트, EOF에 1바이트가 필요한 관측을 담습니다. 다른 위치도 완전한 기록 하나가 필요합니다. LP64 기록은 8바이트 정렬이며 길이는 `roundUp(25 + nameBytes, 8)`입니다. 전체 4096항목 한도, 기록 바이트도 16 MiB 예산에 포함됩니다. 메타데이터/스냅샷만 명시한 조상 경로는 중복 없이 256경로 한도에 포함되며 JSON은 64 KiB입니다.

독립 open은 별도 커서, dup는 공유 커서를 사용합니다. 0 또는 제공한 값에서만 재개하며 알 수 없는 위치는 중단합니다. 들어가는 완전한 기록의 최대 접두부를 반환합니다. 길이 >=1024이면 원래 요청 끝 4바이트에 EOF(끝이면 1, 아니면 0)를 예약하며 기록 페이로드만 128 MiB로 제한합니다. 플래그 주소는 원래 부호 없는 연산과 래핑을 유지합니다. 데이터 복사, 커서 갱신, 읽기 전 위치 복사, 플래그 순서입니다. 뒤의 EFAULT는 앞선 효과를 유지하고 EOF는 빈 데이터 복사를 생략합니다. 개별 복사가 일부만 쓰기 가능하면 그 복사 전에 미지원으로 중단하며 이전 효과는 유지합니다.

동일한 `directory-entries`는 원본 macOS와 기록, dup/되감기, 작은 읽기, EOF 및 복사 순서를 비교합니다. 별도 테스트는 SDK 배치와 긴 이름을 포함한 원본 기록의 전체 바이트를 비교합니다. 고정 스냅샷 값은 되감기에도 유지되어 APFS의 동적 세대를 재현하지 않습니다. 이전 `getdirentries` (196), 쓰기, 다른 네이티브 백엔드와 iOS 실기기는 이 검증 밖입니다.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"directories":[{"path":"/empty"},{"path":"/","contents":{
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":73,"type":8,"next_offset":99,"seek_offset":0}]}}]}}
```

[XNU getdirentries64](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [dirent ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [extended flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent_private.h).

열거 검증(2026-10-05, Release): Darwin 498개 중 246개 통과, 사용 불가 백엔드 252개 건너뜀, 실패 0개입니다. ARM64 HVF 필수 63/63개를 실행했습니다. 네이티브 macOS 프로그램 11개, C/CLI/보고서 38개(건너뜀 없음), Python 5개 조합의 파일 작업 각 8개, 실행기 66개가 통과했습니다. 수치는 중복됩니다. 증거: `build-hvf-arm64/darwin-dirents-verified-evidence/`. Intel HVF Actions는 중지 상태이며 다른 네이티브 백엔드와 iOS 실기기는 미검증입니다.


## 전용 파일 매핑

`mmap`은 일반 카탈로그 파일의 `MAP_PRIVATE`를 지원합니다. `flags=0x2` 또는 `MAP_UNIX03`을 더한 `0x40002`를 쓰며 오프셋은 OS 페이지에 정렬해야 합니다. 짧은 길이를 요청해도 페이지의 원본 파일 바이트를 유지하고 EOF 마지막 페이지의 나머지는 0으로 채웁니다. 전용 쓰기는 해당 매핑만 변경하며 원본, 다른 매핑, 고정 메타데이터와 공유 커서를 바꾸지 않습니다. close 또는 설명자 재사용 후에도 매핑은 유지됩니다. 읽기 전용과 PROT_NONE도 초기 바이트를 보존하며 `mprotect`로 쓰기를 허용할 수 있습니다.

파일 끝 산술 오버플로, UNIX03의 길이 0 및 미정렬 오프셋은 FD 조회 전 EINVAL, 잘못된 FD는 예산 검사 전 EBADF입니다. 기존 길이 0도 FD를 검사한 뒤 할당 없이 0을 반환합니다. 기존 미정렬 오프셋, 스트림, 빈 파일 페이지, 완전히 EOF 밖인 페이지는 할당 전에 미지원으로 중지합니다. macOS는 EOF 밖 매핑을 허용하지만 접근 시 SIGBUS가 발생하므로 모델은 읽을 수 있는 0 페이지나 신호 전달을 만들지 않습니다. 공유, 고정, 실행 가능, JIT 매핑은 미지원입니다.

`DarwinFiles`는 설명자와 바이트를, `DarwinMemory`는 배치·권한·예산·롤백을 소유합니다. 데이터는 `darwin_files`만 사용합니다. 동일한 `file-mapping` 프로그램으로 전용 쓰기, close 이후 수명, 커서, 오류와 익명 페이지 재사용을 검증합니다. 별도 네이티브 비교는 0이 아닌 파일 오프셋, 전체 페이지와 실제 SIGBUS 경계를 검사합니다.

[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c)

### 전용 매핑 검증 (2026-10-05)

Release Darwin은 고유 등록 438건 중 210건 통과, 228건 건너뜀, 실패 0건입니다. ARM64 HVF 필수 57/57건을 실행했고 Unicorn은 다섯 게스트 조합을 검증했습니다. 네이티브 macOS 프로그램 9개, 비영 오프셋 전체 페이지 비교와 격리 자식 프로세스의 SIGBUS 검증이 통과했습니다. 공개 API/보고서 36건은 건너뜀 없이 통과했고 Python 다섯 조합의 `file-mapping`, 검증 스크립트 66건과 출처 회귀 38건도 통과했습니다. 집계는 중복됩니다. 증거: `build-hvf-arm64/darwin-mmap-verified-evidence/`. 새 Intel HVF/KVM/WHP 및 iOS 실기기 증거는 없으며 Intel HVF Actions는 중지 상태입니다.

## 명시적 파일 메타데이터

파일 항목에 `metadata`를 추가할 수 있으며 아래 필드는 모두 필수입니다. 십진 문자열은 정수의 전체 폭을 보존하고 JSON 숫자는 ±(2^53−1) 내 정확한 정수로 제한됩니다. device는 부호 있는 32비트, mode/link_count는 부호 없는 16비트, inode는 부호 없는 64비트, uid/gid/flags/generation은 부호 없는 32비트입니다. size는 파일 바이트 수와 같아야 하며 blocks는 부호 있는 64비트 상한 이하, block_size는 음수가 아닌 부호 있는 32비트입니다. 시간은 부호 있는 64비트 초와 0–999999999 나노초입니다.

`stat64` (338), `fstat64` (339), `lstat64` (340)는 ARM64/x64에서 동일한 144바이트 LP64 레코드를 반환합니다. open과 경로 해석을 공유하며 FD 복제와 닫기를 따릅니다. FD를 할당하거나 커서를 바꾸지 않고 rdev, 패딩, 예약 필드는 0입니다. 메타데이터는 호출자의 고정 관측값이므로 읽기가 시간을 갱신하지 않고 mode가 접근 허가를 바꾸지 않습니다. 미지정 메타데이터, 스트림 상태, 심볼릭 링크, 구형 stat, 확장 보안은 미지원입니다. 경로/FD 오류를 출력 포인터보다 먼저 처리하며 부분 쓰기 가능 출력은 변경 전에 중지합니다. 네이티브 테스트는 실제 파일의 모든 바이트와 SDK 배치를 비교하고 동일한 자체 프로그램으로 세 호출을 검증합니다.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","metadata":{
  "device":1,"inode":"18364758544493064720","mode":33188,"link_count":1,
  "uid":1000,"gid":1000,"size":10,"block_size":4096,"blocks":8,
  "flags":0,"generation":0,
  "access_time":{"seconds":-1,"nanoseconds":1},
  "modification_time":{"seconds":2,"nanoseconds":3},
  "change_time":{"seconds":4,"nanoseconds":5},
  "birth_time":{"seconds":6,"nanoseconds":7}
}}]}}
```

[XNU stat.h](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/sys/stat.h)

### 메타데이터 검증과 남은 작업 (2026-10-05)

stat64 추가 후 Release 검증은 고유 409건 중 193건 통과, 216건 건너뜀, 실패 0건입니다. ARM64 HVF 필수 54/54건을 실행했고 Unicorn은 다섯 게스트 조합을 검증했습니다. SDK 배치 및 실제 레코드 전체 비교, 네이티브 프로그램 8개, 공개 API/보고서 36건(건너뜀 없음), Python 다섯 조합, 검증 스크립트 66건도 통과했습니다. 집계는 중복됩니다. 네이티브 테스트의 출력 파일을 사례별로 분리해 짧은 출력에 이전 끝 바이트가 남는 문제를 수정했습니다. 추가 기능의 Intel HVF/KVM/WHP 및 iOS 실기기 증거는 없습니다.

다음은 공유 매핑과 EOF 페이지 오류, 제한된 쓰기(EOF 페이지, close 이후 수명, 오류 순서 검증), 명시적 시간/시스템 정보, 필수 Mach/스레드 서비스, Mach-O 의존성·재배치/바인딩·초기화/TLS 순입니다. 이후 실제 네이티브 프로그램으로 Objective-C/Swift와 Foundation/UIKit을 검증합니다. iOS 실기기는 SDK와 장치가 필요하며 Intel HVF는 미검증이고 Actions는 계속 중지합니다.



```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

독립 워크로드 검증은 ARM64 60개 또는 x64 40개 네이티브 사례를 모두 요구하며 각 플랫폼의 `LC_MAIN`과 `LC_UNIXTHREAD`를 포함합니다. 필수 항목 누락, 건너뛰기 또는 `ld64.lld` 부재는 실패입니다.

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
