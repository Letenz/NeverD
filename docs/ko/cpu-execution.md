**언어**: [English](../cpu-execution.md) | [简体中文](../zh-CN/cpu-execution.md) | [繁體中文](../zh-TW/cpu-execution.md) | [日本語](../ja/cpu-execution.md) | [한국어](cpu-execution.md) | [Français](../fr/cpu-execution.md) | [Deutsch](../de/cpu-execution.md) | [Español](../es/cpu-execution.md) | [Italiano](../it/cpu-execution.md) | [Русский](../ru/cpu-execution.md) | [العربية](../ar/cpu-execution.md)

[← 문서 색인](README.md)

# CPU 실행 및 기능 조회

CPU 실행은 게스트 OS, 이미지 로더, 호출 규약과 독립적입니다. `NEVERD_ENABLE_CPU_EMULATION`으로 단독 빌드할 수 있고, `NEVERD_ENABLE_DRIVER_EMULATION`은 Windows 드라이버 모델도 포함합니다. [아키텍처 안내서](architecture.md)는 소유권, 백엔드 선택, 플랫폼 제한을 설명합니다.

## 구성

공개 [`ExecutionConfiguration`](../../include/neverd/emulation/ExecutionConfiguration.h)은 CPU 팩토리와 기능 보고서에서 함께 사용됩니다. CPU 할당이나 주소 공간 연결 전에 요구사항을 검증합니다. 생략한 값은 계약의 고정 프로필을 사용하고, 명시한 미지원 값은 실패합니다.

| JSON 필드 | 기본값 | 의미 |
|---|---|---|
| `backend` | `auto` | `auto`, `unicorn`, `kvm`, `whp` |
| `contract` | `software-cpu-v1` | 버전이 지정된 실행 의미론 |
| `architecture` | `x86_64` | `x86_64` 또는 `aarch64` |
| `privilege` | 계약 프로필 | `flat`, `supervisor`, `user`; 계약과 일치해야 함 |
| `virtual_address_bits` | 계약 프로필 | checked 프로필은 48비트, flat 프로필은 64비트 직접 매핑 이름공간 |
| `page_size` | 4096 | 게스트 매핑 단위. 다른 값은 거부 |
| `required_features` | `[]` | [`ExecutionConfiguration.def`](../../include/neverd/emulation/ExecutionConfiguration.def)의 필수 기능 이름 |

`driver-strict`는 x64, `software-cpu-v1`은 x64와 ARM64를 허용합니다. `checked-x64-v1` 및 `checked-aarch64-v1`은 지정된 아키텍처와 supervisor 권한을 요구합니다. `checked-user-x64-v1`과 `checked-user-aarch64-v1`은 계약에 맞는 제한 명령 목록을 각각 CPL3/EL0에서 실행하며 MMU 격리와 명시적 서비스 요청 종료를 사용합니다. Unicorn 및 호스트와 일치하는 KVM/WHP를 지원하고, `auto`는 기존 호스트 선택을 따릅니다. flat 프로필은 아키텍처 수준 사용자/supervisor MMU 격리를 보장하지 않습니다. checked ARM64와 x64 프로필은 아래의 제한된 FP/SIMD 명령군을 허용합니다. supervisor x64는 제한된 MMIO와 prepared-read 문자열 전송을 추가하고, user 프로필은 장치 매핑을 거부합니다. 모든 checked 프로필에서 포트 I/O와 병렬 CPU 요구사항은 계속 거부됩니다. `service_traps`는 user 프로필만 알립니다.

사용자 실행은 매핑된 **모든** 페이지에 `UserAccessible`과 적절한 `Read`, `Write`, `Execute` 권한을 요구합니다. 기존 매핑은 기본적으로 supervisor용이며, 물리 바이트를 공유하는 별칭도 권한은 독립적입니다. `UserAccessible`만으로는 접근할 수 없습니다. 신뢰된 호스트 연산과 supervisor CPU는 RWX를 사용합니다. 예:

```cpp
Configuration.Contract = ExecutionContract::CheckedUserX64;
Configuration.Privilege = ExecutionPrivilege::User;
auto CPU = llvm::cantFail(createExecutionBackend(Configuration, Space)).CPU;
llvm::cantFail(CPU->map(Code, 4096, Read | Write | Execute | UserAccessible));
```

권한은 계약으로 고정됩니다. 컨텍스트 복원이나 주소 공간 연결로 바뀌지 않으며 x64 세그먼트 선택자로 승격할 수 없습니다. 복구 가능한 데이터 접근 fault는 소유자가 처리할 때까지 원래 명령과 레지스터를 보존합니다. `canAccess`는 요청한 권한만 정확히 검사하며 사용자 가시성 조회에는 `UserAccessible`을 포함합니다. 페이지 테이블은 CPU 전용 투영이며 변경 가능한 게스트 페이지 테이블이나 권한 변경 API를 노출하지 않습니다. ARM64에서는 사용자 페이지가 EL1에서 실행 불가합니다.

알 수 없는 필드/null 값, 잘못된 이름·숫자 폭, 중복된 필수 기능, 미지원 조합은 실패합니다. 입력은 64 KiB로 제한되고 기존 CPU 팩토리 및 드라이버 C 옵션은 호환성을 유지합니다.

## 워크로드를 실행하지 않고 조회

```bash
neverd cpu-capabilities
neverd cpu-capabilities \
  --configuration='{"contract":"checked-aarch64-v1","architecture":"aarch64"}' \
  --probe-host
```

스키마 버전은 1입니다. 보고서는 프로필 기본값 적용 전 `requested_configuration`, 정규화된 `configuration`, 정적 의미 지원 `capabilities`, 어댑터 빌드/ABI 지원 `build`, 그리고 `--probe-host` 요청 때만 설정되는 `host`(미요청 시 null)를 구분합니다. 호스트 probe는 전용 RAM에서 임시 CPU를 초기화할 뿐이며 임의의 워크로드 호환성을 증명하지 않습니다. 사용 가능성은 변할 수 있고 미지원 백엔드로 조용히 대체하지 않습니다. 유효한 보고서(백엔드 미사용 가능 포함)는 CLI 0, 잘못된 설정/질의는 1을 반환합니다.

## SDK 및 C++ 경계

[`neverd_cpu_capabilities_json`](../../include/neverd/sdk/NeverDCAPICPU.h)은 기존 세션, 선택적 설정 JSON, 값이 0 또는 1인 `ProbeHost`를 받으며 로드된 바이너리는 필요하지 않습니다. 결과는 `neverd_free_string`으로 해제하고, NULL이면 `neverd_last_error`를 확인합니다. CPU 비활성화 빌드도 동일 함수를 내보내고 비활성 상태를 명시합니다. Python 플러그인은 `session.cpu_capabilities(...)`를 사용합니다. C++은 `executionCapabilities`, `resolveExecutionConfiguration`, `queryExecutionBackendBuild`, `probeExecutionBackend`를 분리해 사용할 수 있습니다. `createExecutionBackend`는 기존 공간에 CPU를 연결하거나 전용 RAM과 기본 공간을 생성합니다.

## CPU 결과와 예산

`CPU.runUntilExit(PC, TimeoutMicroseconds)`는 형식화된 [`ExecutionExit`](../../include/neverd/emulation/ExecutionExit.h)를 반환합니다. 실행 전 설정 오류는 `llvm::Error`이고 실행이 시작되면 중지, deadline, 서비스 요청, 복구 가능한 fault, 게스트 fault/trap, 미지원 연산, 장치/백엔드 오류, 설명되지 않은 엔진 중지를 명시합니다. CPU/장치/백엔드 fault는 동시 중지/deadline보다 우선하며 독립 사실과 세부 정보를 보존합니다. 타임아웃은 양수이고 기간 및 절대 deadline으로 표현 가능해야 합니다. 아니면 CPU 상태를 바꾸기 전에 실패합니다. 모든 실행에는 유한 예산이 필요하며 0은 무제한도 유효한 즉시 timeout도 아닙니다. 제어는 협력적이며 hard wall-clock 제한이 아닙니다. 결과가 복구 가능한 fault를 소비하지 않습니다. OS 소유자가 fault를 가져와 검증된 예외 전송을 설정한 뒤 재개해야 합니다. 기존 `run`, `fault`, `timedOut`은 유지되지만 `run`만 재정의한 외부 CPU 구현은 새 형식 경계를 거부합니다.

## 서비스 요청

checked user x64는 접두사 없는 정확한 `SYSCALL` 인코딩만 가로채고 checked user ARM64는 `SVC #imm16`을 가로챕니다. `SYSENTER`, `INT`, `HVC`, `BRK` 및 다른 메커니즘은 미지원입니다. 명령 관찰자가 먼저 실행됩니다. 중지나 fault가 없으면 CPU는 명령 실행이나 백엔드 진입 **전에** `ExecutionExitKind::ServiceRequest`를 반환하고 종류, 원래 `PC`, 순차 `NextPC`, SVC immediate를 제공합니다. 레지스터, 플래그, 스택, 권한은 바뀌지 않습니다. x64 RCX/R11 SYSCALL clobber와 ARM64 예외 벡터 진입도 아직 발생하지 않습니다. SVC immediate는 범용 서비스 번호가 아닙니다.

요청이 보류된 동안 실행, CPU 변경, 주소 공간 연결, 컨텍스트 저장/복원을 막습니다. CPU가 정지한 상태에서 OS 소유자가 `takeServiceRequest()`로 정확히 한 번 가져갑니다. OS 소유자는 ABI 해석, 서비스 처리, 결과 레지스터 및 다음 PC/예외 전송을 명시적으로 수행해야 합니다. 미지원 서비스는 이 경계에서 실패합니다. 원래 PC를 다시 실행하면 새 요청이 생성되며 NOP나 성공 결과를 암묵적으로 만들지 않습니다. 서비스 이벤트는 동시 중지/deadline보다 우선하지만 게스트/백엔드 오류는 더 높은 우선순위입니다. CPU 중지, 소프트웨어 HLT, deadline, trap은 워크로드 성공을 뜻하지 않습니다. 별도의 [Linux 프로세스 프로필](process-emulation.md)은 OS 서비스를 직접 모델링하며 Windows, Android, Darwin 동작을 입증하지 않습니다. Windows 및 ARM64 native 실행은 실기 런타임 검증이 필요합니다.

## x64 확장과 네이티브 CPU 상태

checked x64는 제한된 legacy SSE/SSE2 이동·논리 연산, `MOVLHPS`/`MOVHLPS`, 마스크형 scalar `CVTTSS2SI`/`CVTTSD2SI`/`SUBSS`/`SUBSD`를 허용합니다. MXCSR는 누적 상태, 반올림, FTZ를 보존하며 DAZ와 마스크되지 않은 예외는 거부합니다. KVM/WHP는 16개 XMM 레지스터 전체와 MXCSR를 동기화합니다. 목록에 없는 인코딩과 operand 조합은 허용되지 않습니다.

checked x64는 마스크된 legacy `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `MIN`, `MAX`의 `SS`, `SD`, `PS`, `PD` 형식도 허용합니다. `X64SSEInstructions.def`가 operand 너비, 정렬, 허용 규칙을 관리합니다. `MaskedSSEArithmeticMatchesIndependentHostExecution`은 독립 host CPU oracle로 register/RAM 형식, 네 반올림 모드, FTZ, signed zero, subnormal, NaN을 검증하며, `SSEMemoryObserverStopsBeforeResultAndStatusChanges`는 효과 반영 전 중단을 검증합니다. DAZ, 마스크되지 않은 예외, x87, AVX는 허용하지 않습니다.

thread pointer는 x64 FS/GS base와 ARM64 `TPIDR_EL0`의 정확한 `MRS`/`MSR` 인코딩을 포함합니다. 네이티브 전송 계층과 CPU snapshot은 메모리와 독립적으로 상태를 보존하지만 OS 스레드나 TLS 블록을 만들지는 않습니다. supervisor x64는 1/2/4바이트 정렬 scalar MMIO와 재시작 경계마다 MOVS 한 요소를 지원합니다. 장치 읽기는 부작용 없는 준비 preview 후 최대 한 번 commit해야 합니다. user 프로필은 장치 매핑을 거부하며 RMW, 넓은 MMIO, 포트 I/O도 계속 미지원입니다.

KVM/WHP는 활성 네이티브 진입을 취소하고 실행 자원을 회수하기 전에 취소 완료를 확인합니다. KVM은 전용 실행 스레드와 일시적으로 차단 해제하는 realtime signal을 사용하며 진입 중 해당 signal을 무시 상태로 두면 안 됩니다. 호출자의 signal mask/handler는 바꾸지 않습니다. 게스트 진행 상태가 불확실한 취소는 terminal failure이고 엄격한 wall-clock deadline은 보장하지 않습니다.

## x64 네이티브 동기 예외

checked x64의 `DIV`/`IDIV`는 실제 프로세서 결과와 `#DE`를 사용합니다. KVM은 비공개 supervisor IDT/IST, WHP는 명시적인 예외 비트맵을 사용하며 원래 컨텍스트와 제공된 오류 코드를 전송 오류와 구분합니다. OS는 복구 가능한 이벤트를 소비한 뒤 계속 실행할 컨텍스트를 설치합니다. Windows 드라이버는 0으로 나누기와 몫 오버플로를 `STATUS_INTEGER_DIVIDE_BY_ZERO`로 변환하고 실제 SEH filter, `__finally`, 재시도를 실행합니다. `NeverDX64ExceptionTests`는 Unicorn 없이 빌드되며 `DriverWDMCPUException`은 원본 WDK 사례를 검증합니다. 사용할 수 없는 WHP/ARM64 호스트는 명시적으로 건너뜁니다.

## 단계적으로 보관하는 RAM 효과

`RAMTransaction`은 물리 실행 임대 아래에서 명령이 선언한 쓰기 범위의 물리적 합집합만 보관합니다. 결과 관찰자 호출 전에 원래 RAM을 복원하며 취소, 전송 오류, 관찰자 예외는 부분 RAM이나 레지스터를 공개하지 않습니다. CPU 예외는 RAM 복원 후에도 아키텍처 예외 상태를 유지합니다. ARM64 단일·쌍 저장도 같은 계층을 사용합니다. x64는 8/16/32/64비트 `XCHG`, `XADD`, `CMPXCHG`를 실행하며 LOCK 또는 암시적 잠금 형식에는 자연 정렬을 요구합니다. `NeverDRAMTransactionTests`는 호스트 CPU와의 결과 비교, 복원, 별칭, 권한을 검증하며 사용할 수 없는 플랫폼은 명시적으로 건너뜁니다. 장치와 병렬 SMP는 제외되며 CPU 스냅샷은 이미 확정된 RAM을 복원하지 않습니다.

## 전체 x87 상태

`NeverDEmulationArch`는 ISA, 페이지 테이블과 FP 상태 배치를 소유하며 네이티브 및 Unicorn 전송이 공유합니다. x64 컨텍스트는 x87 제어, 상태, TOP, 물리 태그, 연산 코드, 명령/데이터 포인터와 8개의 80비트 레지스터를 보존합니다. `FP0`–`FP7`은 `RegisterValue`를 사용하고 스칼라 접근은 잘림을 거부합니다. `FPTag`는 물리 비어 있지 않음 비트맵입니다. `NeverDX64FPTests`는 모든 TOP, 정확한 연산의 호스트 FXSAVE/FXRSTOR 비교와 복원을 검사합니다. checked x87 명령 또는 모든 반올림 의미를 입증하지 않으며 없는 네이티브 호스트는 명시적으로 건너뜁니다.

`driver-strict`는 일치하는 Linux x64 host의 KVM과 Windows x64 host의 WHP를 지원합니다. `auto`는 해당 native transport를, cross-ISA는 Unicorn을 선택합니다. 명시적 Unicorn과 기존 V1 API는 portable software profile을 유지합니다. native 실행은 진입 전에 canonical address와 instruction effect를 검증하고, hardware가 없으면 fallback 없이 실패합니다. 지원되지 않는 instruction/OS behavior는 명시적 오류입니다. native ARM64/WHP 실기 증거는 아직 없으며, 임의 driver나 Android/Darwin 호환성을 의미하지 않습니다.

`executionCapabilities(Contract, ISA, Backend)`로 선택한 백엔드의 기능을 조회합니다. `NativeLegacyX64`는 네이티브 x64 드라이버 실행을 나타내며, `NeverDNativeDriverTests`는 기존 드라이버 모음을 검증합니다. 이 테스트는 Unicorn을 비활성화한 빌드에서도 실행할 수 있습니다.

Checked ARM64는 하나의 완전한 상태 커밋 경계를 사용합니다. `Registers.def`가 39개 스칼라 필드와 32개 128비트 벡터를 정의하며 `captureAArch64State`는 모든 읽기, 선언된 폭과 NZCV 정규화를 완료한 뒤 한 번에 게시합니다. Unicorn/KVM/WHP는 TPIDR_EL0, TPIDRRO_EL0, TPIDR_EL1, FPCR, FPSR를 포함한 같은 상태를 전송합니다. 네이티브 어댑터는 CPACR_EL1로 FP/SIMD를 활성화합니다. 읽기 실패나 진입 취소 시 호출자의 전체 상태가 보존됩니다.

ARM64 KVM/WHP 초기화는 전용 `AArch64MachineProbe.def` 프로그램을 실행합니다. NOP, 양의 무한대 방향으로 반올림하는 FP32 덧셈, 두 레인의 SIMD 덧셈입니다. 각 단계에서 39개 스칼라 필드와 32개 벡터를 모두 비교하여 TLS, NZCV, 결과 상위 비트 초기화, FPCR/FPSR 보존 및 누적 상태를 확인합니다. 감독자 전용 모니터 메모리와 하나의 전체 마감 시간을 사용합니다. 성공은 이 제한된 초기화 프로그램만 검증하며 독립적인 native ARM64 워크로드 검증은 아직 필요합니다.

x64 KVM/WHP 네이티브 초기화는 비공개 supervisor 페이지에서 `X64MachineProbe.def`를 실행합니다. 하나의 기한 안에 NOP, 양의 무한대 방향으로 반올림하는 FP32 덧셈, 두 레인 SIMD 덧셈, FS/GS 로드와 CS/SS/CR8 읽기를 수행하며 각 단계에서 전체 스칼라, XMM, 물리 x87 및 제어 상태를 비교합니다. x64와 ARM64 검사는 물리 메모리의 독점 실행 임대를 요구합니다. `MemoryProjection`은 캐시 식별 정보(ISA, 주소 공간, 매핑 세대, 권한, 모니터 구성)와 ISA별 확정된 페이지 테이블 루트 이력을 소유합니다. 비공개 바이트를 다시 쓰기 전에 캐시를 무효화하므로 실패한 재구축의 부분 테이블이나 호출자의 오래된 루트를 재사용할 수 없습니다. 이 검사는 제한된 초기화만 증명하며, WHP와 ARM64의 독립적인 네이티브 작업 검증은 아직 남아 있습니다.

공유 XSAVE 디코더는 표준 형식과 압축 형식의 SSE 초기 상태를 구분합니다. XSTATE_BV[1]이 0이면 두 형식 모두 XMM을 초기화하지만 표준 형식은 MXCSR을 읽고 검증하며 압축 형식은 MXCSR을 초기화합니다. `X64XsaveCases.def`는 독립적인 데이터 배치와 직접 작성한 호스트 XRSTOR 프로그램을 제공합니다. `X64XsaveTests.cpp`는 거부 시 상태의 원자성을 확인하고 호출자의 FP/SSE 상태를 보존하면서 두 형식을 실제 호스트 실행과 비교합니다. 호스트 아키텍처나 필요한 명령 기능을 사용할 수 없으면 명시적으로 건너뜁니다.

`X64FPState.def`는 제한된 압축 AVX, CET_U, CET_S 배치를 선언합니다. 존재 비트가 설정된 구성 요소는 전체 데이터가 아키텍처의 0으로 채워진 초기 상태와 일치할 때만 허용됩니다. 앞선 구성 요소가 없어도 오프셋은 배치 비트로 결정됩니다. 초기 상태가 아닌 바이트, 알 수 없는 배치, 잘린 데이터는 FP/SSE 상태를 공개하기 전에 실패합니다. `CompactedOffsetsFollowLayoutRatherThanPresentBits`와 `InitialCETComponentsDoNotHideFPState`는 872바이트 WHP 패킷을 검증합니다. 초기 전송 메타데이터만 허용하며 AVX나 CET 실행은 허용하지 않습니다.

공통 `encodeX64XsaveState` / `decodeX64XsaveState` 코덱은 표준·압축 FP/SSE 패킷, 물리 TOP 순환, 누락된 구성 요소의 초기 상태 및 원자적 검증을 소유합니다. WHP는 완전한 XSAVE API를 사용하며 `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState`를 우선하고 이전 XSAVE API를 호환 경로로 사용합니다. 이전 개별 x87 레지스터 인터페이스는 완전한 패킷을 대체할 수 없습니다. 초기 상태가 아닌 확장 구성 요소, 잘못된 헤더·제어 값 및 잘린 캡처는 명시적으로 실패합니다. WHP 매핑 실패는 진단을 위해 HRESULT, GPA 및 크기를 보존하며 Windows 네이티브 검증이 계속 필요합니다.

`CheckedX64Instructions.def`는 기존 CPU 백엔드에서 8/16/32/64비트 부호 없는 `MUL`과 `CBW/CWDE/CDQE/CWD/CDQ/CQO`를 허용합니다. `NeverDX64IntegerTests`는 독립적인 `X64IntegerCases.def` 인코딩과 예상값을 사용하여 두 권한 수준에서 부분 레지스터 보존, 32비트 제로 확장, 곱의 상위·하위 결과, 정의된 CF/OF 및 부호 확장 시 플래그 보존을 검증합니다. 일반 RAM 곱셈은 전체 접근 범위의 권한 검사와 읽기 관찰 콜백을 유지하며, 오류나 관찰 콜백의 중지는 암시적 출력 레지스터와 PC를 보존합니다. 장치 피연산자는 지원하지 않습니다. checked Unicorn에서도 실행하며 사용할 수 없는 네이티브 백엔드는 명시적으로 건너뜁니다.

`WhpResourceCache.h`는 논리 CPU 상태와 WHP 파티션을 분리합니다. 런타임은 활성 네이티브 파티션 하나를 유지하며 같은 CPU의 연속 단계에서 재사용합니다. CPU를 전환할 때 이전 파티션을 먼저 제거한 다음 매핑과 가상 프로세서를 다시 만들고 전체 상태를 복원합니다. 논리 CPU는 독립적인 `MemoryProjection` 뷰와 권위 있는 RAM을 유지합니다. 임대 획득은 취소와 현재 기한을 따르며 비활성 CPU를 제거해도 다른 CPU의 파티션은 제거되지 않습니다. x64는 호스트의 기본 XSAVE 기능 조합을 보존하고 `WHvGetPartitionProperty`로 실제 파티션을 검증하며 종속 기능을 지워 마스크를 축소하지 않습니다. 협력적 CPU 전환은 병렬 하드웨어 SMP를 제공하지 않습니다.

`CheckedAArch64Instructions.def`와 `AArch64InstructionEffects`는 EL0/EL1에서 제한된 기본 FP32/FP64 연산·비교·이동과 고정 폭 SIMD를 허용합니다. FPCR는 네 가지 반올림 모드, FZ, DN을 지원하고 FPSR는 누적 상태와 QC를 보존합니다. 미지원 제어·상태 비트는 변경 전에 거부합니다. FP16 연산, SVE/SME, 마스크되지 않은 예외, 선택적 확장과 목록 밖 형식은 명시적으로 실패합니다. Windows ARM64 드라이버 로딩이나 다른 OS 환경은 추가하지 않습니다.

`AArch64InstructionEffects`는 최대 128비트 피연산자의 스칼라·FP/SIMD 단일/쌍 RAM 범위를 소유합니다. 공유 주소 공간은 CPU 진입 전 모든 페이지를 검사하고 `RAMTransaction`은 선언된 전체 물리 쓰기만 커밋합니다. 128비트 쓰기는 실행 전에 두 개의 64비트 값으로 순서대로 관찰됩니다. 정지와 오류는 RAM, 벡터와 주소 갱신을 보존합니다. Xn/Vn 번호 중복은 유효하며 쌍 접근 범위의 주소 래핑은 거부됩니다. `NeverDAArch64MemoryTests`는 독립적인 `AArch64CrossPageCases.def`와 `AArch64VectorMemoryCases.def`를 사용합니다.

KVM x64/ARM64는 `KvmRunControl`을 통해 같은 전용 vCPU 작업 스레드에서 상태 준비, `KVM_RUN`, 상태 캡처를 수행합니다. `EINTR` 재시도에도 준비는 한 번이며 취소나 캡처 실패는 게시할 수 없습니다. `KvmAArch64Machine.cpp`의 주소 변환 유지와 전체 스칼라·벡터 전송도 하나의 단일 단계 기한을 공유합니다. 호출 스레드는 완료 확인 후 커밋하며 ISA 해석, RAM 트랜잭션, OS 정책과 관찰자를 담당합니다. 네이티브 ARM64 실기 증거는 아직 없습니다.

KVM은 `X64HostRegisters.def`와 `X64FPState.def`에 따라 일반 레지스터와 전체 FP/SSE 상태를 마지막으로 완료 확인한 디버그 종료 상태와 비교하고 변경된 입력을 다시 설치합니다. 호스트 쓰기와 컨텍스트 복원도 비교에 포함되며 예외, 취소와 실패는 재사용을 무효화합니다. 단일 단계 설정과 실제 일반/FP 상태 읽기는 명령마다 수행합니다.

하드웨어 실행 자체가 더 짧은 전체 지연 시간을 보장하지는 않습니다. 현재 네이티브 실행은 명령마다 허용 검사, 관찰, 상태 전송과 VM 종료를 수행합니다. 같은 원본 이미지와 시나리오를 동일한 명령·이벤트 예산으로 비교하고 시간과 함께 결과 일치를 보고해야 합니다. CLI 지연에는 시작과 로드도 포함합니다.

checked Unicorn은 `MachineRunControl`을 사용하며 ARM64 유지보수, 게스트 실행과 전체 상태 읽기를 한 단계 시간 한도에서 처리합니다. `UC_HOOK_CODE`는 명령 진입에서 빌린 정지 토큰과 기한을 확인합니다. 동기 엔진 호출은 반환 전에 hook 참조를 해제하지만 기계 단계는 상태 게시까지 제어를 유지합니다. Unicorn과 WHP는 전체 CPU 상태를 임시 저장하고 성공한 단계의 게시 직전에 같은 제어 조건을 확인합니다. WHP는 준비 전에 시간 한도를 한 번만 만듭니다. 확인된 x64 CPU 예외는 상태 읽기 중 도착한 정지 요청보다 우선합니다. 읽기가 취소되면 checked RAM 트랜잭션은 추측 쓰기를 버리며 비제한 소프트웨어 계약은 그대로입니다. `MachineInterruptedError`는 확인된 취소와 호스트 또는 상태 읽기 실패를 구분합니다. 공통 checked CPU는 `Stopped` 또는 `Deadline`을 반환하고 CPU/RAM을 유지하며 재시도를 허용합니다. 동시 정지 요청이 있어도 실제 실패는 `BackendFailure`로 남습니다.

`RunDeadline::invoke`는 중지되었거나 기한이 지난 WHP 실행을 호스트 호출 전에 거부하고, 취소 중에도 실제 호스트 결과를 보존하며, 빌린 중지 토큰을 해제하기 전에 인터럽트 콜백의 종료를 확인합니다. KVM과 WHP는 실행 임대를 보유한 호출 스레드에서 완전히 캡처된 비공개 상태를 검증한 다음 동시에 도착한 중지나 기한을 분류합니다. 실제 호스트·캡처 실패와 인증된 x64 CPU 예외가 우선합니다. 일반 성공 상태는 취소 확인이 끝날 때까지 비공개로 유지하며, 확인된 중단은 추측적 CPU/RAM 효과를 버리고 재시도를 허용합니다. 준비, 네이티브 실행, 캡처는 하나의 스텝 유예를 공유합니다. 협력적 취소를 제공하지만 엄격한 실제 시간 상한은 보장하지 않습니다.
