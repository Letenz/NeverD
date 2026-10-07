**언어**: [English](../emulation.md) | [简体中文](../zh-CN/emulation.md) | [繁體中文](../zh-TW/emulation.md) | [日本語](../ja/emulation.md) | [한국어](emulation.md) | [Français](../fr/emulation.md) | [Deutsch](../de/emulation.md) | [Español](../es/emulation.md) | [Italiano](../it/emulation.md) | [Русский](../ru/emulation.md) | [العربية](../ar/emulation.md)

<!-- i18n-source: a3e64122b77a690dd856d02f5b2af53973d3bf1affd973bea5f9735aa9dd6722 -->

[← 문서 색인](README.md)

# CPU 실행과 게스트 환경

<!-- i18n-section: backends -->

## CPU 백엔드와 워크로드

CPU 실행은 ISA 허용, 게스트 메모리, 백엔드 전송과 게스트 OS 정책을 분리합니다. `NEVERD_ENABLE_CPU_EMULATION`은 x64/ARM64 CPU 계층을 켜고 `NEVERD_ENABLE_DRIVER_EMULATION`은 제한된 x64 Windows WDM/KMDF 환경을 추가합니다. `linux-elf64-v1`은 지원되는 Linux ELF 프로세스를 실행합니다. [CPU 실행](cpu-execution.md), [게스트 프로세스 에뮬레이션](process-emulation.md), [Windows 드라이버 에뮬레이션](driver-emulation.md)를 참조하세요.

지원되는 네이티브 계약에서 게스트와 호스트 ISA가 같으면 `auto`는 Linux의 KVM, Windows의 WHP 또는 [macOS의 HVF](macos-hvf.md)를 선택합니다. 다른 ISA는 Unicorn을 사용하며 `software-cpu-v1`과 기존 V1 API는 소프트웨어 실행을 유지합니다. 명시한 백엔드를 사용할 수 없으면 대체 없이 실패합니다. 네이티브 실행은 진입 전에 허용 명령어, 주소와 효과를 검사합니다. 호스트 가상화는 게스트 OS를 결정하지 않으며 [Darwin 프로필](darwin-emulation.md)이 macOS, iOS와 iOS Simulator를 별도로 모델링합니다. HVF에는 `com.apple.security.hypervisor` 권한이 필요합니다.

`driver-strict` / `checked-x64-v1`은 제한된 x64 실행을 지원하며 Windows 드라이버 로딩도 x64로 제한됩니다. `checked-aarch64-v1`과 `checked-user-aarch64-v1`은 제한된 ARM64 FP32/FP64, 고정 폭 SIMD와 완전한 FPCR/FPSR/벡터 상태를 포함합니다. ARM64 HVF 네이티브 검증 결과는 Mac 가이드에 기록되어 있습니다. ARM64 KVM/WHP 워크로드 검증과 Intel HVF 전체 검증은 미완료입니다. CPU 실행 지원이 모든 드라이버나 앱과의 호환성을 뜻하지는 않습니다.

<!-- i18n-section: windows-processes -->

## Windows 프로세스와 모듈

`windows-pe64-v1`은 PEB/TEB, 정적·동적 TLS, `DllMain`, 이름 기반 Win32 API 및 명시적 비순환 DLL 그래프를 갖춘 제한된 Windows x64/ARM64 콘솔 프로세스를 지원합니다. 게스트 모듈은 이름/서수 코드·데이터 가져오기, DIR64 재배치, 전달 내보내기 및 실제 로더 목록 식별자를 지원합니다. `LoadLibraryA` / `LoadLibraryW`, `FreeLibrary`, `GetProcAddress`는 설정된 모듈 카탈로그를 사용합니다. CRT/GUI, ARM64 스택 프레임 기반 사용자 SEH, 스레드 및 일반 Windows 앱 호환성은 미완성이며 네이티브 ARM64 KVM/WHP 증거도 아직 없습니다.

`WindowsSystemModules`는 두 ISA에 대해 `ntdll.dll`, `kernelbase.dll`, `kernel32.dll`의 제한된 PE64 모델 이미지를 만듭니다. ASCII `GetModuleHandleA` / `GetModuleHandleW`, `LoadLibraryA` / `LoadLibraryW`, `GetProcAddress`는 매핑된 베이스를 공유하며 PEB/LDR과 `MEM_IMAGE`도 같은 이미지를 나타냅니다. 정적 가져오기, 이름 조회와 게스트 DLL 전달은 동일한 API 게이트와 내보내기 해석기를 사용합니다. 제공자는 고정 상주하고 게스트 초기화 콜백이 없으며 일반 게스트 DLL을 모두 해제한 뒤 진입점 반환을 막지 않습니다. 헤더 또는 내보내기 메타데이터가 바뀌면 조회를 중단합니다. 미지원 시스템 내보내기 이름과 0이 아닌 서수 조회는 명시적으로 중단하며 지원 이름의 대소문자 불일치와 빈 이름은 오류 127, NULL 조회는 87을 반환합니다. 생성 바이트와 주소는 모델 정책이며 Windows DLL 버전별 배치, 네이티브 서수와 제공자 간 별칭은 재구성하지 않습니다. `WindowsSystemTests.cpp`는 자체 x64/ARM64 EXE를 네이티브 Windows와 비교하고 초기 스레드 반환을 독립적으로 8회 관측합니다.

<!-- i18n-section: environment-memory -->

## 환경 변수와 메모리

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` 는 PEB 프로세스 매개변수의 실제 게스트 환경 블록을 공유합니다. 이름은 대소문자를 구분하지 않는 ASCII이며 값은 UTF-16입니다. 변경 전에 입력, 용량, 쓰기 가능한 메모리를 검증합니다. 스냅샷은 이후 변경과 독립적이며 해제하면 게스트 메모리를 회수합니다. 모델의 블록 한도는 64 KiB이고 문자열과 확장에는 크기 및 실행 기한 검사가 적용됩니다. 알 수 없는 포인터 소유권, 잘못된 블록, ANSI 코드 페이지, 확장 버퍼 중첩은 지원하지 않습니다. `WindowsEnvironmentTests.cpp`는 사용 가능한 백엔드에서 자체 x64/ARM64 픽스처를 비교하며 CI는 독립적인 네이티브 Windows 오라클을 필수로 실행합니다.

`WindowsProcessHeap`은 프로세스 힙의 할당, `HeapReAlloc`, 해제와 크기 조회를 통합 관리합니다. 크기 변경은 유지되는 데이터를 보존하며 `HEAP_ZERO_MEMORY`는 추가 바이트를 0으로 만들고 `HEAP_REALLOC_IN_PLACE_ONLY`는 이동을 금지합니다. 재할당 실패 시 기존 블록을 보존하고 NULL을 반환하며 `ERROR_NOT_ENOUGH_MEMORY`(8)를 설정하여 네이티브 관측과 일치합니다. 독립적인 페이지는 축소와 해제 시 용량을 반환하며 단계별 확장과 제한된 복사는 실행 기한을 확인합니다. 사용자 정의 힙, 예외 생성 플래그, 알 수 없는 소유권, 접근 불가능한 복사 또는 초기화 범위는 명시적으로 중단합니다. `WindowsHeapTests.cpp`는 두 ISA, 강제 이동, 예산 재사용, 실패 원자성을 검증하며 CI는 동일한 자체 EXE를 네이티브 Windows에서도 실행합니다.

Windows 가상 메모리는 `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery`와 현재 프로세스의 `FlushInstructionCache`를 지원합니다. OS 계층은 예약 영역을 소유하고 `AddressSpace`는 커밋된 페이지, 권한, 실제 저장 공간을 관리합니다. 테스트는 동적 코드 수정, 접근 오류, 메모리 한도 재사용을 검증합니다.

`WriteProcessMemory`는 현재 프로세스에 최대 4 KiB를 쓸 때 x64/ARM64에서 실측한 커밋된 페이지의 동작을 따릅니다. 각 영역의 보호 속성, 복사된 앞부분, 바이트 수와 LastError를 보존하며 `ERROR_NOACCESS`, `ERROR_PARTIAL_COPY`, RX 앞부분을 쓴 뒤 성공을 반환하는 동작도 재현합니다. `WindowsMemoryWriteTests.cpp`는 보호 속성의 25가지 조합을 모두 검사하고, `check_windows_memory_write.py`는 같은 자체 제작 실행 파일을 네이티브 Windows CI에서 검증합니다. 커밋되지 않은 대상 영역은 명시적으로 지원하지 않습니다.

<!-- i18n-section: vectored-exceptions -->

## 벡터 예외와 실행 계속

`WindowsProcessExceptions`는 같은 CPU와 프로세스 예산으로 `AddVectoredExceptionHandler`, `RemoveVectoredExceptionHandler`, `RaiseException`을 구현합니다. 순서가 있는 처리기는 등록·삭제, 중첩 예외, 모델 API 호출, DLL 로드와 프로세스 종료를 수행할 수 있습니다. x64/ARM64 데이터 접근 위반과 x64 정수 나눗셈 예외는 게스트의 `CONTEXT` 변경을 검증한 뒤 재개하며 범용 레지스터, SIMD 및 지원 FP 상태를 보존합니다. 소프트웨어 예외는 모델 공급자 내부의 실제 반환 명령으로 재개합니다. 보관된 등록은 128개, 중첩은 16프레임으로 제한합니다. 잘못된 처리 결과, 바뀐 예외 포인터, 미지원 필드와 한도 초과는 명시적으로 실패합니다. ARM64 스택 프레임 기반 SEH/언와인딩, 디버거 전달과 실행/가드 페이지 예외는 미지원입니다. `WindowsExceptionTests.cpp`는 자체 EXE/DLL을 네이티브 Windows와 비교하며 ARM64 KVM/WHP 실기기 증거는 아직 없습니다. 소프트웨어 예외 레코드에는 `EXCEPTION_SOFTWARE_ORIGINATE`(`0x80`)가 포함되며 호출자의 계속 불가 플래그와 별도로 처리됩니다. 원본 Windows 실행 파일은 소프트웨어 및 하드웨어 예외의 정확한 플래그 값을 검증합니다.

`AddVectoredContinueHandler`와 `RemoveVectoredContinueHandler`는 독립된 순서 목록을 관리하며 예외 처리기와 최대 128개 보존 등록 제한을 공유합니다. 벡터 예외 처리기가 실행 재개를 수락하면 계속 처리기는 같은 수정 가능한 예외 레코드와 `CONTEXT`를 봅니다. 중첩 예외와 DLL 알림을 포함한 콜백이 끝난 뒤 최종 컨텍스트를 검증합니다. 다른 종류의 처리기 API로 핸들을 제거할 수 없습니다. `WindowsContinuationTests.cpp`는 순서, 조기 종료, 등록 변경, 컨텍스트 복구, 중첩 전달, 로더 콜백과 프로세스 종료를 독자 EXE와 네이티브 Windows로 비교합니다. 검증한 Windows x64 벡터 경로에서는 `EXCEPTION_NONCONTINUABLE`이 설정되어도 재개할 수 있지만 스택 프레임 기반 SEH 동작의 근거는 아닙니다. 네이티브 ARM64 실행은 아직 검증하지 않았습니다.

<!-- i18n-section: caller-context -->

## 호출자 컨텍스트

`RtlCaptureContext`는 x64와 ARM64의 `kernel32.dll`, `ntdll.dll`에서 사용할 수 있습니다. 공유 `WindowsProcessContext`와 `IntegerABI`가 CPU 상태와 LastError를 바꾸지 않고 호출자의 PC/SP를 저장합니다. 네이티브 Windows 관찰로 x64 플래그 `0x10000f`, 사용하지 않는 home/디버그/벡터 영역의 보존, 기존 32비트 x87 주소 필드를 확인했습니다. ARM64는 LR을 PC에 저장하고 기록의 X0/LR을 0으로 만듭니다. 레지스터, SIMD, 부동소수점 제어는 게스트에서 가져오며 x64 선택자와 MXCSR 기능 마스크는 설정된 게스트 CPU를 따릅니다. 잘못되거나 정렬되지 않거나 일부에 접근할 수 없는 대상 기록은 쓰기 전에 실패합니다. `WindowsContextTests.cpp`는 직접 가져오기, 제공자 조회, VEH 콜백, 페이지 경계를 넘는 출력과 실패 원자성을 검증합니다. `scripts/check_windows_context.py`는 독자 실행 파일을 Windows x64/ARM64에서 실행하고 비어 있지 않은 x87 상태를 별도로 검증합니다. 이 ARM64 API 관찰은 네이티브 KVM/WHP 실행 증거가 아닙니다. 컨텍스트 복원, 스택 순회와 동적 함수 테이블은 여전히 별도 구현 과제입니다. `WindowsProcessServices.def`는 정확한 모듈 제한을 선언합니다. `kernelbase.dll`에서 이 심볼을 찾으면 네이티브 관찰과 동일하게 `ERROR_PROC_NOT_FOUND`(127)를 반환하며 존재하지 않는 내보내기를 추가하지 않습니다. [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

<!-- i18n-section: structured-exceptions -->

## 구조적 예외 처리

`WindowsProcessSEH`는 `os/windows/exception/`의 공통 `X64SEH`(드라이버 환경 없이도 사용하는 `NeverDEmulationWindowsException`)로 x64 `__C_specific_handler`와 UNWIND_INFO V1을 처리합니다. VEH 검색 후 필터, finally, 비지역 처리기 이동, 중첩/충돌 언와인딩과 재배치한 EXE/DLL 프레임을 지원하고 비휘발성 GPR/XMM을 보존합니다. 필터가 실행 재개를 선택하면 같은 `CONTEXT`로 VCH를 실행합니다. `WindowsSEHTests.cpp`는 독자적인 23개 시나리오를 네이티브 Windows와 비교하며 KVM/WHP/Unicorn은 같은 의미론을 사용합니다. 프로세스 예산 안에서 이미지 세대, 헤더, 언와인드/범위 바이트, 언어 처리기 코드 영역과 IAT 바인딩을 다시 검증합니다. 메타데이터 변경이나 보존된 이미지 언로드는 명시적으로 실패합니다. ARM64 프레임 SEH, C++ EH, 동적 함수 테이블, 일반 RtlUnwind/NtContinue 및 로더/VEH/VCH 콜백 경계를 넘는 언와인딩은 미지원입니다.

`EXCEPTION_NONCONTINUABLE`에 대해 x64 필터가 `EXCEPTION_CONTINUE_EXECUTION`을 반환하면 새 컨텍스트로 `STATUS_NONCONTINUABLE_EXCEPTION`(`0xc0000025`, 플래그 `0x81`, 연결 레코드 null)을 전달합니다. VEH를 다시 실행한 뒤 보존된 논리 스택을 다시 검색하며, 동일한 깊이 및 실행 예산 안에서 finally 순서와 EXE/DLL 프레임의 정체성을 유지합니다. 23개 네이티브 시나리오는 성공 실행 21개와 종료 2개를 포함합니다. 원래 `CONTEXT`를 복원해도 VEH/VCH가 이 2차 예외의 계속 실행을 수락하면 처리되지 않은 채 종료되며, 모델은 런타임 실패를 보고합니다. 소프트웨어 예외 주소는 저장된 PC와 같고 내부 디스패처 주소 및 레지스터 배치는 모델 정책입니다. [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

<!-- i18n-section: processor-state -->

## 명령어와 프로세서 상태

검사형 x64는 일반 RAM의 `MOVS/STOS/LODS`와 `CLD/STD`를 지원하며 요소별 재개, 취소와 페이지 경계를 검사합니다. CPU별로 다른 0회 반복 상위 비트와 STOS/LODS 장치 피연산자는 계약 범위 밖입니다.

검사형 x64는 일반 RAM의 `CMPS/SCAS`와 `REPE/REPNE`도 지원하며 산술 플래그, 조기 종료, 요소별 중지와 오류 복구를 처리합니다. 장치 비교는 아직 지원하지 않습니다.

x64와 ARM64 네이티브 시작 검사는 독점 메모리 임대하에서 제한된 전체 상태 실행을 검증합니다. XSAVE 패킷과 ISA를 식별하는 페이지 테이블 캐시는 하나의 권한 계층이 관리합니다.

네이티브 x64 `FOP/FIP/FDP`는 호스트 저장·복원 규칙을 따르며 AMD는 비활성 x87 예외 메타데이터를 0으로 만들 수 있습니다. 시작 검사는 마스크되지 않은 대기 예외로 이 필드를 검증합니다.
