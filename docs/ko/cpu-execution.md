**언어**: [English](../cpu-execution.md) | [简体中文](../zh-CN/cpu-execution.md) | [繁體中文](../zh-TW/cpu-execution.md) | [日本語](../ja/cpu-execution.md) | [한국어](cpu-execution.md) | [Français](../fr/cpu-execution.md) | [Deutsch](../de/cpu-execution.md) | [Español](../es/cpu-execution.md) | [Italiano](../it/cpu-execution.md) | [Русский](../ru/cpu-execution.md) | [العربية](../ar/cpu-execution.md)

[← 문서 색인](README.md)

# CPU 실행 및 기능 조회

CPU 실행은 게스트 OS, 이미지 로더, 호출 규약과 독립적입니다. `NEVERD_ENABLE_CPU_EMULATION`으로 단독 빌드할 수 있고, `NEVERD_ENABLE_DRIVER_EMULATION`은 Windows 드라이버 모델도 포함합니다. [아키텍처 안내서](architecture.md)는 소유권, 백엔드 선택, 플랫폼 제한을 설명합니다.

`NEVERD_ENABLE_SEMANTIC_TESTS`의 기본값은 `ON`이며 `unittests/semantic`의 테스트 그룹과 통합 실행 대상을 제어합니다. Unicorn 없이 네이티브 CPU 테스트를 빌드하려면 `BUILD_TESTING=ON`을 유지하고 `NEVERD_ENABLE_SEMANTIC_TESTS=OFF`와 `NEVERD_EMULATION_BACKEND_UNICORN=OFF`를 설정합니다. 적절한 SDK 헤더가 있는 Windows ARM64/MSVC를 포함하여 네이티브 KVM/WHP 테스트를 계속 빌드할 수 있습니다. Windows ARM64에서 Unicorn을 활성화하려면 ARM64 LLVM-MinGW 도구 모음이 필요합니다. 이 빌드 분리는 ARM64 네이티브 실행 검증을 의미하지 않습니다.

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

마스킹된 x64 명령 설명은 이식 가능한 기준입니다. KVM/WHP는 `driver-strict`, `checked-x64-v1`, `checked-user-x64-v1`에 `precise_simd_exceptions`를 추가합니다. 네이티브 시작 검증이 정확한 `#XM`과 두 재시도를 확인한 뒤에만 마스크 해제 MXCSR 쓰기, `LDMXCSR`, Windows `CONTEXT` 복원을 허용합니다. `ExecutionProfiles.def`가 선택을 관리하고 `supportsSIMDExceptions`가 결정된 인스턴스 기능을 제공합니다. checked Unicorn은 계속 마스크를 요구하며 ARM64 및 HVF의 예외 기능은 확장하지 않습니다.

네이티브 x64 KVM/WHP는 실제 `#XM`을 `X64SIMDException`을 통해 드라이버 C SEH로 전달합니다. 하드웨어 오류의 `CONTEXT`는 XMM0–15와 MXCSR를 보존합니다. 필터, 예외 언와인드 finally 콜백과 선택된 핸들러는 MXCSR `0x1f80`, DF 해제 상태에서 실행됩니다. 음수 필터는 XMM과 최상위 `CONTEXT.MxCsr`를 수정한 후 원래 명령을 재시도할 수 있습니다. 후자에는 게스트 CPU 마스크가 적용되며 `FltSave.MxCsr`는 커널 복원을 제어하지 않습니다. 모델 API 예외는 정수/제어 레코드를 유지하며 x87/AVX 컨텍스트 수정은 계속 거부됩니다.

checked x64는 제한된 legacy SSE/SSE2 이동·논리 연산, `MOVLHPS`/`MOVHLPS`, 마스크형 scalar `CVTTSS2SI`/`CVTTSD2SI`/`SUBSS`/`SUBSD`를 허용합니다. MXCSR는 누적 상태, 반올림, FTZ를 보존하며 이식 가능한 실행은 마스크되지 않은 예외를 거부합니다. KVM/WHP는 16개 XMM 레지스터 전체와 MXCSR를 동기화합니다. 목록에 없는 인코딩과 operand 조합은 허용되지 않습니다.

checked x64는 마스크된 legacy `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `MIN`, `MAX`의 `SS`, `SD`, `PS`, `PD` 형식도 허용합니다. `X64SSEInstructions.def`가 operand 너비, 정렬, 허용 규칙을 관리합니다. `MaskedSSEArithmeticMatchesIndependentHostExecution`은 독립 host CPU oracle로 register/RAM 형식, 네 반올림 모드, FTZ, signed zero, subnormal, NaN을 검증하며, `SSEMemoryObserverStopsBeforeResultAndStatusChanges`는 효과 반영 전 중단을 검증합니다. x87, AVX는 허용하지 않습니다.

이식 가능한 소프트웨어 프로필에서 Unicorn은 기존 `MINSS/MINSD/MINPS/MINPD`와 `MAXSS/MAXSD/MAXPS/MAXPD`가 선택한 값에 DAZ를 적용합니다. 선택한 비정규 입력은 부호 있는 0이 되며 NaN 페이로드와 기존 MXCSR 상태는 보존됩니다. `test_x86_sse_minmax_daz`는 레지스터, RAM, 동일 레지스터 형식에서 DAZ 켜기/끄기, 모든 반올림 모드, FTZ 및 누적 상태를 검사합니다.

KVM/WHP는 비공개 `FXSAVE64` 실행으로 `MXCSR_MASK`를 탐색하고 부호 있는 비정규 입력의 산술로 DAZ 기능을 검증합니다. checked Unicorn은 소프트웨어 마스크를 제공합니다. `supportedControlBits`는 CPU의 불변 마스크를 반환하며 FX/XSAVE, 스냅샷, Windows CONTEXT가 이를 공유합니다. checked `LDMXCSR/STMXCSR`는 정확히 m32의 전체 RAM 권한을 검사하고 오류나 관찰 콜백 취소 시 상태를 보존합니다. 예약 비트 로드는 #GP를 발생시키며 이식 가능한 실행은 마스크되지 않은 SIMD 예외를 거부합니다. `X64MXCSRTests.cpp`는 제어와 재시도를 검사하고 `X64DAZData`는 허용된 SSE 산술 28종을 원본 호스트 명령과 비교하여 DAZ, 반올림, FTZ를 검증합니다. HVF의 DAZ 지원은 확장하지 않습니다.

네이티브 x64 시작 검증은 전송 계층의 콜드 스타트 준비와 모든 탐색 단계에 하나의 `5 s` 예산을 사용합니다. 게스트 실행 예산은 독립적입니다. 초기화 중단은 명령 단계, `stop_requested`, `deadline_reached`를 보고하며 중단 타입과 원래 전송 진단을 유지합니다. 재시도하거나 검증이 끝나지 않은 CPU를 허용하지 않습니다.

`PAUSE`(`F3 90`)는 네이티브 `driver-strict`를 포함하여 KVM/WHP와 checked Unicorn의 공통 x64 머신 경계를 통해 실행됩니다. `X64PauseTests.cpp`는 전체 상태 보존, 실행 전 중지, 컨텍스트 복원, 잘못된 `LOCK` 거부, 스핀 루프의 시간 제한과 재개를 검증합니다. 이 프로세서 힌트는 게스트 스레드를 스케줄링하거나 특정 지연을 보장하지 않습니다.

`PUSHFQ`(`9C`)와 16비트 `PUSHF`(`66 9C`)는 `driver-strict`를 포함한 네이티브 KVM/WHP 및 checked Unicorn에서 실행됩니다. 공유 ISA 계층은 RAM 트랜잭션을 커밋하기 전에 스택 이미지에서 내부 단일 단계 실행용 TF를 제거합니다. 암시적 스택 주소에는 전체 RSP를 사용하며 접두사 순서가 피연산자 너비를 결정합니다. 전체 범위 권한 검사, 관찰자 중지, 오류 재시도는 원자성을 유지합니다. `X64PushFlagsTests.cpp`는 허용된 플래그 조합 256개와 인코딩 9개, 페이지 경계를 넘는 별칭, 사용자 권한, 취소 및 컨텍스트 복원을 검사합니다.

`POPFQ`(`9D`)와 16비트 `POPF`(`66 9D`)는 공유 x64 ISA 계층에서 플래그를 복원하며 KVM, WHP, checked Unicorn 및 `driver-strict`에 적용됩니다. 프로파일의 IOPL은 0으로 고정되므로 CPL0은 IF를 바꿀 수 있고 CPL3은 IF와 IOPL을 유지합니다. 예약 비트와 VM/VIF/VIP는 무시하며 RF는 지웁니다. 게스트 TF, NT, AC, ID 또는 CPL0 IOPL의 유효한 변경은 아직 지원하지 않으며 상태 공개 전에 명시적으로 실패합니다. 전체 스택을 읽은 뒤 FLAGS/RSP/RIP를 원자적으로 갱신하고, 전체 RSP와 유효한 접두사 순서로 피연산자를 결정합니다. 스택은 읽기 전용이거나 실행 메모리의 별칭일 수 있습니다. 공유 계층에서 완료하므로 전송 계층의 내부 TF가 지워지지 않습니다. `X64PopFlagsTests.cpp`는 독립적인 네이티브 CPL3 명령 비교와 모든 입력 비트, 오류, 관찰자, 후속 네이티브 실행을 검사합니다.

checked x64는 `CLC/STC/CMC`와 `LAHF/SAHF`를 허용합니다. 캐리 명령은 전송 계층에서 실행하고 AH 전송은 KVM, WHP, checked Unicorn 및 네이티브 `driver-strict`에서 하나의 ISA 구현을 공유합니다. LAHF는 다섯 상태 플래그와 고정 비트를 AH에 쓰며 SAHF는 CF/PF/AF/ZF/SF만 변경합니다. OF/IF/DF와 다른 레지스터는 유지됩니다. 모든 REX 값을 포함해 무시되는 접두사도 암시적 AH를 사용합니다. `X64StatusFlagsTests.cpp`는 원본 호스트 명령, 전체 상태, 취소와 재개를 검사합니다. 고정 버전 Unicorn 변환기도 이식 가능한 프로필에서 REX가 붙은 암시적 AH를 유지하고, 다섯 명령의 LOCK 형식을 상태 변경 전에 거부합니다.

checked x64는 KVM, WHP, Unicorn에서 16/32/64비트 대상과 imm8/CL 횟수를 사용하는 `SHLD/SHRD`를 실행합니다. 횟수에 아키텍처 마스크를 적용하고, 16비트 형식에서 16을 넘는 미정의 횟수는 효과 전에 거부합니다. RAM은 정확한 폭으로 읽기·쓰기를 검사하며 기존 트랜잭션에서 공개 전에 결과를 관찰합니다. 취소하면 CPU와 메모리를 되돌립니다. LOCK과 장치 피연산자는 지원하지 않습니다.

`X64ScalarShiftInstructions.def`는 `SHL/SHR/SAR`, `ROL/ROR`, `RCL/RCR`의 8/16/32/64비트 대상 효과를 정의합니다. 레지스터 및 RAM 형식은 암시적 1, imm8, CL 횟수를 지원하며 마스킹 후 0인 횟수와 SAL 별칭도 포함합니다. RAM 쓰기는 권한 검사, 결과 관찰 콜백, 공유 트랜잭션을 거칩니다. 중지 또는 콜백 오류가 발생하면 CPU와 메모리를 복원합니다. KVM, WHP, Unicorn 검사 모드는 이 경계를 공유합니다. LOCK 및 MMIO 형식은 아직 지원하지 않습니다.

`X64LoopInstructions.def`는 프로세서 실행 경로에서 `LOOP/LOOPE/LOOPNE`를 허용합니다. 주소 크기는 RCX 또는 0으로 확장되는 ECX를 선택하며 FLAGS는 유지됩니다. 대상 너비는 CPU 모델을 따릅니다. 긴 모드의 Intel은 `66H`를 무시하고 AMD는 16비트 재정의를 유지하며 REX.W가 우선합니다. 네이티브 KVM/WHP는 호스트 CPU를, Unicorn은 기본 Intel Haswell 모델을 사용합니다. 공유 허용 규칙은 디코더가 생략한 경우에도 LOCK과 REP를 부작용 전에 거부합니다.

`X64PackedIntegerInstructions.def`는 순환·포화 덧셈과 뺄셈, 비교, 곱셈, 평균, 최솟값·최댓값, 바이트 차이, 패킹·언패킹을 포함한 45개 legacy SSE2 packed integer 명령을 허용합니다. XMM과 정렬된 128비트 RAM 소스는 KVM, WHP, Unicorn의 기존 checked 경로를 공유합니다. FLAGS와 MXCSR은 변하지 않으며 결함이나 관찰자 취소 시 상태를 보존합니다. MMX, VEX/EVEX, 장치 피연산자는 제외됩니다.

`X64PackedShiftInstructions.def`는 legacy SSE2 패킹 시프트 열 종류를 허용합니다. 요소 시프트의 횟수는 imm8 또는 XMM/정렬된 m128이며 바이트 시프트는 imm8만 허용합니다. 가변 횟수는 부호 없는 하위 64비트를 사용하고 스칼라 횟수 마스킹을 하지 않으며 상위 64비트는 무시합니다. 횟수가 0이거나 범위를 넘어도 메모리 피연산자는 16바이트 전체를 읽어야 합니다. FLAGS와 MXCSR은 유지되며 MMX, VEX/EVEX, 장치 피연산자는 제외됩니다.

`X64VectorOperands.def`는 기존 SSE 이동, 연산, 시프트, 변환 및 마스크의 전체 피연산자 규칙을 정의합니다. `MOVMSKPS`, `MOVMSKPD`, `PMOVMSKB`는 XMM의 부호 비트를 r32/r64로 추출하고 대상의 나머지 비트를 0으로 만듭니다. KVM, WHP, checked Unicorn은 같은 허용 규칙을 사용하며 FLAGS, MXCSR 및 원본 레지스터를 보존합니다. 마스크의 메모리 피연산자, MMX 및 VEX/EVEX 형식은 지원하지 않습니다.

`X64ShuffleInstructions.def`는 `PSHUFD`, `PSHUFHW`, `PSHUFLW`, `SHUFPS`, `SHUFPD`를 추가합니다. `X64VectorOperands.def`는 XMM 대상, XMM 또는 정렬된 m128 원본, imm8의 완전한 세 피연산자 형식을 요구합니다. 원래 명령은 FLAGS와 MXCSR을 유지하며 비트 그대로 레인을 선택합니다. 메모리 형식은 16바이트 전체를 검사하고 정렬 오류는 데이터 관찰보다 먼저 발생합니다. KVM, WHP, checked Unicorn은 이 규칙을 공유합니다. 같은 목록은 피연산자가 정확히 두 개인 `UNPCKLPS`, `UNPCKHPS`, `UNPCKLPD`, `UNPCKHPD`도 허용하여 원래 대상과 원본의 비트 패턴을 교차 배치합니다. 하드웨어는 선택한 64비트만 가져올 수 있으며 checked RAM은 정렬된 m128 피연산자를 검사합니다.

`MOVLPS`, `MOVHPS`, `MOVLPD`, `MOVHPD`는 정렬 요구 없이 RAM의 정확히 8바이트를 전송합니다. `X64VectorInstructions.def`는 저장할 절반을 선언하고 `X64VectorOperands.def`는 XMM/m64 쌍을 요구합니다. 로드는 나머지 64비트를 보존하며 상위 절반 저장 관찰자는 상위 데이터를 받습니다. KVM, WHP, checked Unicorn은 전체 범위 권한 검사와 RAM 롤백을 공유합니다. 레지스터 전용 `MOVHLPS`/`MOVLHPS`는 고유 의미를 유지합니다.

`CVTSI2SS`와 `CVTSI2SD`는 MXCSR 반올림 규칙으로 부호 있는 32/64비트 정수를 변환하며 정밀도 상태를 유지합니다. 공유 `IntegerSource` 규칙은 XMM 목적지와 r32/r64 또는 m32/m64 소스만 허용합니다. 기존 형식은 목적지 상위 96/64비트를 보존하고 메모리 검사에는 정수 너비를 사용합니다. KVM, WHP, checked Unicorn은 원래 명령을 실행합니다. MMX, VEX/EVEX는 제외됩니다.

`CVTSS2SI`와 `CVTSD2SI`는 공유 `IntegerResult` 규칙에서 MXCSR 반올림에 따라 부호 있는 32/64비트 정수를 생성하고, `CVTTSS2SI`와 `CVTTSD2SI`는 항상 0 방향으로 버립니다. 예외가 마스크된 NaN·범위 초과 변환은 정수 부정값을 반환하고 무효 상태를 설정하며, 유효하지만 부정확한 결과는 정밀도 상태를 설정합니다. 기존 누적 상태, FLAGS, XMM 소스는 보존됩니다. r32 결과는 범용 레지스터 상위 절반을 지웁니다. RAM 읽기는 목적지 너비와 관계없이 부동소수점 소스 너비를 사용하며 FTZ는 비정규 입력을 버리지 않습니다. KVM, WHP, checked Unicorn에 공통으로 적용됩니다.

`COMISS`, `COMISD`, `UCOMISS`, `UCOMISD`는 공유 `Source` 규칙으로 스칼라 XMM 또는 m32/m64를 비교합니다. CF/PF/ZF를 설정하고 OF/SF/AF를 지우며 다른 FLAGS와 소스 레인을 보존합니다. COMIS는 모든 NaN에, UCOMIS는 signaling NaN에만 무효 상태를 설정하며 NaN 처리가 비정규 상태보다 우선합니다. MXCSR 누적 비트는 보존되고 반올림과 FTZ는 비교에 영향을 주지 않습니다. KVM, WHP, checked Unicorn은 정확한 메모리 검사를 공유합니다. 고정된 Unicorn 비교 함수는 기존 비정규 입력 분류를 재사용합니다.

`CMPSS`, `CMPSD`, `CMPPS`, `CMPPD`는 KVM, WHP, checked Unicorn에서 기존 8개 비교 조건을 실행합니다. 공유 `Source` 규칙은 디코딩된 별칭을 허용하고 예약 제어값은 거부합니다. 스칼라 형식은 상위 레인을 보존하고 m32/m64를 읽으며 벡터 형식은 정렬된 m128을 요구합니다. FLAGS와 기존 MXCSR 상태를 보존하고 활성 레인별 무효·비정규 상태를 누적합니다. Capstone이 명령 계열 ID와 SSE 조건을 소유하여 lifter 내부의 ID 보정을 대체합니다. Unicorn은 각 비교 함수에서 비정규 입력을 분류합니다.

`CVTSS2SD`, `CVTSD2SS`, `CVTPS2PD`, `CVTPD2PS`는 공유 `Source` 규칙으로 기존 SSE 정밀도를 변환합니다. 스칼라 결과는 대상 상위 64/96비트를 보존합니다. 패킹 확장은 m64를 읽어 배정밀도 두 개를 쓰고, 축소는 정렬된 m128을 읽어 단정밀도 두 개를 쓰며 상위 64비트를 지웁니다. KVM, WHP, checked Unicorn의 원본 명령 실행은 FLAGS를 보존하고 반올림 및 FTZ 제어에 따라 마스크된 MXCSR 상태를 누적합니다. Unicorn은 변환 함수에서 활성 비정규 입력을 각각 분류합니다. VEX/EVEX는 제외됩니다.

`CVTDQ2PS`와 `CVTDQ2PD`는 공유 `Source` 규칙으로 부호 있는 32비트 정수를 패킹 변환합니다. 단정밀도는 정렬된 m128과 MXCSR 반올림을 사용하고, 배정밀도는 비정렬도 허용하는 m64를 읽어 정확히 변환합니다. 대상 XMM 전체를 교체하고 FLAGS와 기존 MXCSR 상태를 보존하며 부정확한 단정밀도 결과는 정밀도 상태를 누적합니다. KVM, WHP, checked Unicorn은 원본 명령을 실행합니다. Unicorn은 두 패킹 확장 함수를 식별해 8바이트 읽기를 선택합니다. MMX, VEX/EVEX는 제외됩니다.

`CVTPS2DQ`와 `CVTPD2DQ`는 MXCSR 반올림을 사용하며 `CVTTPS2DQ`와 `CVTTPD2DQ`는 0 방향으로 절삭합니다. 공유 `Source` 규칙은 정렬된 m128 또는 XMM 입력을 요구합니다. NaN이나 범위 밖 레인은 signed32 indefinite와 무효 상태를 생성하고 다른 유효한 비정확 레인은 독립적으로 정밀도 상태를 누적합니다. 단정밀도 입력은 정수 네 개를, 배정밀도 입력은 두 개를 생성하고 대상 상위 64비트를 지웁니다. FLAGS와 기존 MXCSR 상태를 보존하며 FTZ는 비정규 입력을 버리지 않습니다. KVM, WHP, checked Unicorn은 원본 명령을 실행합니다. MMX, VEX/EVEX는 제외됩니다.

`X64AlignmentTests.cpp`는 허용된 aligned SSE 명령의 비정렬 피연산자가 데이터 관찰자, 권한 검사 또는 장치 콜백 전에 복구 가능하거나 종료되는 `#GP(0)`를 보고하는지 검증합니다. 오류는 공개 x64 레지스터 전체, PC와 RAM을 보존합니다. 주소 폭에 따른 순환 후 FS/GS 기준 주소를 더하고, 주소를 고치면 원래 명령을 재시도합니다. 직접 KVM/WHP 머신 테스트가 하드웨어 경계를 독립적으로 검증합니다. Windows ring3는 분류된 `operand_alignment` 오류를 전달하며, 다른 원인의 `#GP`는 아직 지원하지 않습니다.

thread pointer는 x64 FS/GS base와 ARM64 `TPIDR_EL0`의 정확한 `MRS`/`MSR` 인코딩을 포함합니다. 네이티브 전송 계층과 CPU snapshot은 메모리와 독립적으로 상태를 보존하지만 OS 스레드나 TLS 블록을 만들지는 않습니다. supervisor x64는 1/2/4바이트 정렬 scalar MMIO와 재시작 경계마다 MOVS 한 요소를 지원합니다. 장치 읽기는 부작용 없는 준비 preview 후 최대 한 번 commit해야 합니다. user 프로필은 장치 매핑을 거부하며 RMW, 넓은 MMIO, 포트 I/O도 계속 미지원입니다.

KVM/WHP는 활성 네이티브 진입을 취소하고 실행 자원을 회수하기 전에 취소 완료를 확인합니다. KVM은 전용 실행 스레드와 일시적으로 차단 해제하는 realtime signal을 사용하며 진입 중 해당 signal을 무시 상태로 두면 안 됩니다. 호출자의 signal mask/handler는 바꾸지 않습니다. 게스트 진행 상태가 불확실한 취소는 terminal failure이고 엄격한 wall-clock deadline은 보장하지 않습니다.

## x64 네이티브 동기 예외

checked x64의 `DIV`/`IDIV`는 실제 프로세서 결과와 `#DE`를 사용합니다. KVM은 비공개 supervisor IDT/IST, WHP는 명시적인 예외 비트맵을 사용하며 원래 컨텍스트와 제공된 오류 코드를 전송 오류와 구분합니다. OS는 복구 가능한 이벤트를 소비한 뒤 계속 실행할 컨텍스트를 설치합니다. Windows 드라이버는 0으로 나누기와 몫 오버플로를 `STATUS_INTEGER_DIVIDE_BY_ZERO`로 변환하고 실제 SEH filter, `__finally`, 재시도를 실행합니다. `NeverDX64ExceptionTests`는 Unicorn 없이 빌드되며 `DriverWDMCPUException`은 원본 WDK 사례를 검증합니다. 사용할 수 없는 ARM64 호스트는 명시적으로 건너뜁니다.

## 단계적으로 보관하는 RAM 효과

`RAMTransaction`은 물리 실행 임대 아래에서 명령이 선언한 쓰기 범위의 물리적 합집합만 보관합니다. 결과 관찰자 호출 전에 원래 RAM을 복원하며 취소, 전송 오류, 관찰자 예외는 부분 RAM이나 레지스터를 공개하지 않습니다. CPU 예외는 RAM 복원 후에도 아키텍처 예외 상태를 유지합니다. ARM64 단일·쌍 저장도 같은 계층을 사용합니다. x64는 8/16/32/64비트 `XCHG`, `XADD`, `CMPXCHG`를 실행하며 LOCK 또는 암시적 잠금 형식에는 자연 정렬을 요구합니다. `NeverDRAMTransactionTests`는 호스트 CPU와의 결과 비교, 복원, 별칭, 권한을 검증하며 사용할 수 없는 플랫폼은 명시적으로 건너뜁니다. 장치와 병렬 SMP는 제외되며 CPU 스냅샷은 이미 확정된 RAM을 복원하지 않습니다.

## 전체 x87 상태

`NeverDEmulationArch`는 ISA, 페이지 테이블과 FP 상태 배치를 소유하며 네이티브 및 Unicorn 전송이 공유합니다. x64 컨텍스트는 x87 제어, 상태, TOP, 물리 태그, 연산 코드, 명령/데이터 포인터와 8개의 80비트 레지스터를 보존합니다. `FP0`–`FP7`은 `RegisterValue`를 사용하고 스칼라 접근은 잘림을 거부합니다. `FPTag`는 물리 비어 있지 않음 비트맵입니다. `NeverDX64FPTests`는 모든 TOP, 정확한 연산의 호스트 FXSAVE/FXRSTOR 비교와 복원을 검사합니다. checked x87 명령 또는 모든 반올림 의미를 입증하지 않으며 없는 네이티브 호스트는 명시적으로 건너뜁니다.

`driver-strict`는 일치하는 Linux x64 host의 KVM과 Windows x64 host의 WHP를 지원합니다. `auto`는 해당 native transport를, cross-ISA는 Unicorn을 선택합니다. 명시적 Unicorn과 기존 V1 API는 portable software profile을 유지합니다. native 실행은 진입 전에 canonical address와 instruction effect를 검증하고, hardware가 없으면 fallback 없이 실패합니다. 지원되지 않는 instruction/OS behavior는 명시적 오류입니다. Windows x64 네이티브 CI는 Unicorn을 비활성화하고 필수 검사 359개를 모두 통과합니다. CPU 검사 131개, 내장 이미지 26개·WDK 이미지 46개·시나리오 사례 40개를 기본 및 재배치 주소에서 실행한 드라이버 결과 224개, SEH 경계 검사 4개를 포함합니다 ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). native ARM64 실기 증거는 아직 없으며, 임의 driver나 Android/Darwin 호환성을 의미하지 않습니다.

`executionCapabilities(Contract, ISA, Backend)`로 선택한 백엔드의 기능을 조회합니다. `NativeLegacyX64`는 네이티브 x64 드라이버 실행을 나타내며, `NeverDNativeDriverTests`는 기존 드라이버 모음을 검증합니다. 이 테스트는 Unicorn을 비활성화한 빌드에서도 실행할 수 있습니다.

Checked ARM64는 하나의 완전한 상태 커밋 경계를 사용합니다. `Registers.def`가 39개 스칼라 필드와 32개 128비트 벡터를 정의하며 `captureAArch64State`는 모든 읽기, 선언된 폭과 NZCV 정규화를 완료한 뒤 한 번에 게시합니다. Unicorn/KVM/WHP/HVF는 TPIDR_EL0, TPIDRRO_EL0, TPIDR_EL1, FPCR, FPSR를 포함한 같은 상태를 전송합니다. 네이티브 어댑터는 CPACR_EL1로 FP/SIMD를 활성화합니다. 읽기 실패나 진입 취소 시 호출자의 전체 상태가 보존됩니다.

ARM64 KVM/WHP/HVF 초기화는 전용 `AArch64MachineProbe.def` 프로그램을 실행합니다. NOP, 양의 무한대 방향으로 반올림하는 FP32 덧셈, 두 레인의 SIMD 덧셈입니다. 각 단계에서 39개 스칼라 필드와 32개 벡터를 모두 비교하여 TLS, NZCV, 결과 상위 비트 초기화, FPCR/FPSR 보존 및 누적 상태를 확인합니다. 감독자 전용 모니터 메모리와 하나의 전체 마감 시간을 사용합니다. 이 검사는 제한된 초기화만 검증합니다. Linux ARM64 KVM과 Windows ARM64 WHP의 워크로드 검증은 아직 남아 있습니다. macOS 네이티브 결과는 [HVF 가이드](macos-hvf.md)에 기록되어 있습니다. 이 프로그램에는 키가 비활성화된 A/B 반환 주소 서명 및 인증과 보호되지 않은 페이지의 네 가지 BTI 명령도 포함됩니다.

프로브는 `MRS CTR_EL0`를 두 번 실행하고 `DC CVAU`, `DSB ISH`, `IC IVAU`, `ISB`를 실행하여 캐시 구성의 안정성과 전체 상태를 확인합니다. Checked EL0/EL1은 원래 명령어, 이름이 있는 기본 DSB 옵션, ISB SY를 허용합니다. CTR은 선택한 가상 CPU에서 읽으며 전송 방식에 따라 다를 수 있습니다. 캐시 대상은 현재 권한으로 읽을 수 있는 일반 RAM이어야 하며 비정렬 주소와 별칭도 허용합니다. 다른 대상은 미지원으로 거부하고 캐시 유지 작업은 데이터 읽기/쓰기 관찰 이벤트를 만들지 않습니다. 투영은 명령 실행의 일관성을 유지하지만 개별 캐시 내용이나 병렬 하드웨어 SMP를 모델링하지 않습니다. `NeverDAArch64CacheTests`는 전체 상태, 읽기 전용 페이지 끝, 거부, 중지, 컨텍스트, 예산 및 페이지를 넘는 RW/RX 별칭을 통한 guest 코드 갱신을 검증합니다. 사용할 수 없는 KVM/WHP 호스트는 명시적으로 건너뜁니다.

x64 KVM/WHP/HVF 네이티브 초기화는 비공개 supervisor 페이지에서 `X64MachineProbe.def`를 실행합니다. 하나의 기한 안에 NOP, 양의 무한대 방향으로 반올림하는 FP32 덧셈, 두 레인 SIMD 덧셈, FS/GS 로드와 CS/SS/CR8 읽기를 수행하며 각 단계에서 전체 스칼라, XMM, 물리 x87 및 제어 상태를 비교합니다. x64와 ARM64 검사는 물리 메모리의 독점 실행 임대를 요구합니다. `MemoryProjection`은 캐시 식별 정보(ISA, 주소 공간, 매핑 세대, 권한, 모니터 구성)와 ISA별 확정된 페이지 테이블 루트 이력을 소유합니다. 비공개 바이트를 다시 쓰기 전에 캐시를 무효화하므로 실패한 재구축의 부분 테이블이나 호출자의 오래된 루트를 재사용할 수 없습니다. 이 검사는 제한된 초기화만 검증합니다. Linux ARM64 KVM과 Windows ARM64 WHP의 워크로드 검증은 아직 남아 있습니다. macOS 네이티브 결과는 [HVF 가이드](macos-hvf.md)에 기록되어 있습니다.

공유 XSAVE 디코더는 표준 형식과 압축 형식의 SSE 초기 상태를 구분합니다. XSTATE_BV[1]이 0이면 두 형식 모두 XMM을 초기화하지만 표준 형식은 MXCSR을 읽고 검증하며 압축 형식은 MXCSR을 초기화합니다. `X64XsaveCases.def`는 독립적인 데이터 배치와 직접 작성한 호스트 XRSTOR 프로그램을 제공합니다. `X64XsaveTests.cpp`는 거부 시 상태의 원자성을 확인하고 호출자의 FP/SSE 상태를 보존하면서 두 형식을 실제 호스트 실행과 비교합니다. 호스트 아키텍처나 필요한 명령 기능을 사용할 수 없으면 명시적으로 건너뜁니다.

`X64FPState.def`는 압축 AVX, AVX-512, CET_U/CET_S, AMX 전송 배치와 구성 요소의 64바이트 정렬을 선언합니다. 존재하는 확장 데이터는 모두 0인 초기 상태여야 하며, 없는 구성 요소의 데이터와 정렬 패딩은 상태를 정의하지 않습니다. 배치 비트가 오프셋을 결정하고 알 수 없는 배치, 초기 상태가 아닌 데이터, 잘못된 길이는 공개 전에 실패합니다. `CompactedOffsetsFollowLayoutRatherThanPresentBits`, `WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`, `InitialCETComponentsDoNotHideFPState`, `InitialWideComponentsDoNotHideFPState`가 872바이트와 10752바이트 WHP 패킷을 검증합니다. 이 전송 지원은 해당 확장 명령 실행을 허용하지 않습니다.

`WhpXsaveRegisters.def`는 이름이 있는 x87/SSE 제어 레지스터로 완전한 XSAVE 패킷을 보완합니다. 마지막 연산 코드와 명령/데이터 포인터를 명시적으로 쓰고 호스트에서 읽습니다. 패킷의 0 필드는 보완할 수 있지만, 0이 아닌 메타데이터 충돌이나 공통 제어값 불일치는 상태 공개 전에 실패합니다. `NamedMetadataRestoresOmittedPacketFields`는 전체 FP 데이터를 유지하면서 누락 필드를 검증합니다.

네이티브 `FOP/FIP/FDP`는 호스트 x87 저장·복원 규칙을 따릅니다. 마스크되지 않은 대기 예외가 없으면 AMD는 이 필드를 0으로 만들 수 있으며 스냅샷은 관측값을 유지합니다. `X64MachineProbe.def`와 정밀 NOP/컨텍스트 테스트는 일관된 대기 예외를 설정하여 모든 필드를 유효한 상태에서 차이를 숨기지 않고 비교합니다. 호스트 프로세스 FXRSTOR64/FXSAVE64 참조는 두 상태를 검사하며, 백엔드는 호스트 결과를 입력 메타데이터로 대체하지 않습니다.

공통 `encodeX64XsaveState` / `decodeX64XsaveState` 코덱은 표준·압축 FP/SSE 패킷, 물리 TOP 순환, 누락된 구성 요소의 초기 상태 및 원자적 검증을 소유합니다. WHP는 완전한 XSAVE API를 사용하며 `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState`를 우선하고 이전 XSAVE API를 호환 경로로 사용합니다. 이전 개별 x87 레지스터 인터페이스는 완전한 패킷을 대체할 수 없습니다. 초기 상태가 아닌 확장 구성 요소, 잘못된 헤더·제어 값 및 잘린 캡처는 명시적으로 실패합니다. WHP 매핑 실패는 진단을 위해 HRESULT, GPA 및 크기를 보존합니다.

`CheckedX64Instructions.def`는 기존 CPU 백엔드에서 8/16/32/64비트 부호 없는 `MUL`과 `CBW/CWDE/CDQE/CWD/CDQ/CQO`를 허용합니다. `NeverDX64IntegerTests`는 독립적인 `X64IntegerCases.def` 인코딩과 예상값을 사용하여 두 권한 수준에서 부분 레지스터 보존, 32비트 제로 확장, 곱의 상위·하위 결과, 정의된 CF/OF 및 부호 확장 시 플래그 보존을 검증합니다. 일반 RAM 곱셈은 전체 접근 범위의 권한 검사와 읽기 관찰 콜백을 유지하며, 오류나 관찰 콜백의 중지는 암시적 출력 레지스터와 PC를 보존합니다. 장치 피연산자는 지원하지 않습니다. checked Unicorn에서도 실행하며 사용할 수 없는 네이티브 백엔드는 명시적으로 건너뜁니다.

`X64BitInstructions.def`는 16/32/64비트 레지스터 및 일반 RAM의 `BT/BTS/BTR/BTC`를 허용합니다. 레지스터 비트 인덱스는 피연산자 너비의 부호 있는 값으로 전체 워드를 선택하며, 즉시값은 기준 워드 안에 머뭅니다. 주소 너비에 따른 절단은 FS/GS 기준 주소를 더하기 전에 적용됩니다. 프로세서가 CF와 쓰기 값을 제공하고, `RAMTransaction`은 관찰 콜백이 수락할 때까지 결과를 비공개로 유지합니다. 전체 범위 권한 검사는 독립 페이지 할당과 별칭을 포함하며, 중지·콜백 실패·페이지 접근 거부 시 원래 CPU와 RAM을 보존합니다. LOCK은 자연 정렬된 메모리 수정 형식만 허용하며 MMIO와 하드웨어 병렬 SMP는 지원하지 않습니다. `X64BitStringTests.cpp`는 독립 인코딩을 실제 x64 호스트 실행과 비교하고 음수 인덱스, 너비 절단, 페이지 경계 접근, 취소, 잘못된 LOCK 형식을 검사합니다. [Intel 명령어 참조](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)를 참고하세요.

`X64StringInstructions.def`는 일반 RAM의 8/16/32/64비트 `MOVS/STOS/LODS`를 관리하며, `CLD/STD`는 다른 플래그를 보존하면서 방향만 바꿉니다. REP의 각 요소는 관찰 전에 전체 피연산자를 검사하고 재개 경계에서 확정됩니다. 후속 오류가 발생해도 완료한 요소는 유지되며, 중지나 콜백 예외는 현재 요소를 변경하지 않습니다. FS/GS는 주소 폭을 자른 뒤 소스에만 더합니다. AL/AX 로드는 상위 비트를 보존하고 EAX 로드는 0으로 확장합니다. 32비트 주소 모드에서 반복 횟수가 0이면 카운터 상위 비트가 0이어야 하며 MOVS/STOS의 참여 주소 레지스터도 동일합니다. 그렇지 않으면 실제 CPU 구현마다 결과가 다릅니다. MOVS/STOS/LODS의 REPNE 형식과 STOS/LODS 장치 피연산자는 아직 지원하지 않습니다. `X64StringTransferTests.cpp`는 독립적인 호스트 명령으로 폭, 방향, 중첩, 0회 반복을 비교하고 권한, 별칭, 주소 순환, 오류와 재개도 검사합니다. 자체 WDK 리소스 드라이버는 `driver_resource_strings.def`로 STOS/LODS의 네 폭을 모두 실행합니다.

`X64StringInstructions.def`는 일반 RAM의 8/16/32/64비트 `CMPS/SCAS`와 `REPE/REPNE`도 관리합니다. 각 요소는 관찰 전에 전체 읽기 범위를 검사하고 여섯 산술 플래그를 갱신하며 첫 종료 조건에서 멈춥니다. 데이터 오류는 이번 연속 REP 시작 시점의 플래그를 복원하면서 완료된 포인터와 카운터 변경을 유지합니다. 공개 API로 재개하면 게시된 CPU 상태에서 다시 시작합니다. 중지와 관찰 예외는 현재 요소를 변경하지 않으며 조기 종료 후 다음 요소를 읽지 않습니다. FS/GS는 CMPS 소스에만 적용되고 SCAS는 누산기와 사용하지 않는 소스 레지스터를 유지합니다. 장치 피연산자와 모호한 32비트 0회 반복 상위 비트는 제외됩니다. `X64StringComparisonTests.cpp`는 독립 호스트 명령으로 플래그, 방향, 별칭, 주소 순환, 권한과 복구를 비교하고 Linux x64 신호로 실제 오류 시점의 레지스터를 검사합니다. 자체 WDK 리소스 드라이버는 `driver_resource_strings.def`로 네 폭의 두 조건 반복 형식을 실행합니다. [Intel 명령 참조](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)를 참고하세요. Linux 네이티브 검증은 첫 요소 실행 전후의 오류를 검사하며, Intel의 진입 flags 복원과 Hyper-V의 AMD EPYC 7763에서 관측한 마지막 비교 flags 유지를 구분합니다([네이티브 관측](https://github.com/NeverSight/NeverD/actions/runs/37202522130)). 알 수 없는 CPU 제조사는 명시적으로 실패합니다. checked 게스트는 모든 백엔드에서 진입 flags를 복원합니다.

`WhpResourceCache.h`는 논리 CPU 상태와 WHP 파티션을 분리합니다. 런타임은 활성 네이티브 파티션 하나를 유지하며 같은 CPU의 연속 단계에서 재사용합니다. CPU를 전환할 때 이전 파티션을 먼저 제거한 다음 매핑과 가상 프로세서를 다시 만들고 전체 상태를 복원합니다. 논리 CPU는 독립적인 `MemoryProjection` 뷰와 권위 있는 RAM을 유지합니다. 임대 획득은 취소와 현재 기한을 따르며 비활성 CPU를 제거해도 다른 CPU의 파티션은 제거되지 않습니다. x64는 호스트의 기본 XSAVE 기능 조합을 보존하고 `WHvGetPartitionProperty`로 실제 파티션을 검증하며 종속 기능을 지워 마스크를 축소하지 않습니다. 협력적 CPU 전환은 병렬 하드웨어 SMP를 제공하지 않습니다.

`CheckedBackend`는 CPU마다 명령어 인출 버퍼와 `cs_disasm_iter` 명령어 레코드를 유지합니다. 매 단계 실행 권한이 있는 바이트를 다시 읽고 디코딩하며, 코드 쓰기·별칭 변경·실행 재개 뒤에 이전 디코딩 결과를 재사용하지 않습니다. 실행 임대는 재사용 저장소에 접근하기 전에 재귀 실행을 거부합니다. 명령어마다 이루어지는 버퍼와 레코드 할당을 제거하면서 명령어 관찰, 시스템 서비스 가로채기, 정확한 오류 처리를 유지합니다. 고정 버전 Unicorn의 단일 단계는 간접 변환 조회를 포함해 후속 명령어 인출 전에 끝나며 내부 코드 쓰기 재시도를 완료된 명령어로 세지 않습니다.

`WhpX64Partition.h`는 실제 파티션별로 x64 WHP 레지스터 재사용을 관리합니다. 고정 패킷은 `WhpX64Registers.def`와 `X64HostRegisters.def`를 사용하며, 성공한 매 스텝에서 일반·제어·세그먼트 레지스터와 전체 FP/SSE 상태를 캡처합니다. 완료가 확인된 디버그 종료만 변경 없는 입력의 생략을 허용하며, 비교 시 예약 비트와 공용체 패딩은 무시합니다. CR3, CPL, TLS, 일반 또는 FP 입력 변경은 재설치하고 부분 실패·취소·예외는 재사용을 무효화합니다. 파티션을 다시 만들면 전체 상태부터 설치합니다. 중복 전송을 줄이며 명령 허용 범위를 넓히거나 전체 속도 향상을 주장하지 않습니다.

WHP는 `WhpXsaveRegisters.def`의 x87/SSE 메타데이터를 일반 레지스터와 동일한 `WHvGetVirtualProcessorRegisters` 호출로 읽습니다. 중지된 vCPU는 동일한 파티션 임대로 보호됩니다. 상태 게시 전에 전체 XSAVE 캡처와 모든 메타데이터 일관성 검사를 수행합니다. 스텝당 호스트 API 호출 하나를 줄이지만 처리량 향상을 측정한 결과는 아닙니다.

`CheckedAArch64Instructions.def`와 `AArch64InstructionEffects`는 EL0/EL1에서 제한된 기본 FP32/FP64 연산·비교·이동과 고정 폭 SIMD를 허용합니다. FPCR는 네 가지 반올림 모드, FZ, DN을 지원하고 FPSR는 누적 상태와 QC를 보존합니다. 미지원 제어·상태 비트는 변경 전에 거부합니다. FP16 연산, SVE/SME, 마스크되지 않은 예외, 선택적 확장과 목록 밖 형식은 명시적으로 실패합니다. Windows ARM64 드라이버 로딩이나 다른 OS 환경은 추가하지 않습니다.

`AArch64InstructionEffects`는 최대 128비트 피연산자의 스칼라·FP/SIMD 단일/쌍 RAM 범위를 소유합니다. 공유 주소 공간은 CPU 진입 전 모든 페이지를 검사하고 `RAMTransaction`은 선언된 전체 물리 쓰기만 커밋합니다. 128비트 쓰기는 실행 전에 두 개의 64비트 값으로 순서대로 관찰됩니다. 정지와 오류는 RAM, 벡터와 주소 갱신을 보존합니다. Xn/Vn 번호 중복은 유효하며 쌍 접근 범위의 주소 래핑은 거부됩니다. `NeverDAArch64MemoryTests`는 독립적인 `AArch64CrossPageCases.def`와 `AArch64VectorMemoryCases.def`를 사용합니다.

KVM x64/ARM64는 `KvmRunControl`을 통해 같은 전용 vCPU 작업 스레드에서 상태 준비, `KVM_RUN`, 상태 캡처를 수행합니다. `EINTR` 재시도에도 준비는 한 번이며 취소나 캡처 실패는 게시할 수 없습니다. `KvmAArch64Machine.cpp`의 주소 변환 유지와 전체 스칼라·벡터 전송도 하나의 단일 단계 기한을 공유합니다. 호출 스레드는 완료 확인 후 커밋하며 ISA 해석, RAM 트랜잭션, OS 정책과 관찰자를 담당합니다. 네이티브 ARM64 실기 증거는 아직 없습니다.

KVM은 `X64HostRegisters.def`와 `X64FPState.def`에 따라 일반 레지스터와 전체 FP/SSE 상태를 마지막으로 완료 확인한 디버그 종료 상태와 비교하고 변경된 입력을 다시 설치합니다. 호스트 쓰기와 컨텍스트 복원도 비교에 포함되며 예외, 취소와 실패는 재사용을 무효화합니다. 단일 단계 설정과 실제 일반/FP 상태 읽기는 명령마다 수행합니다.

x64 KVM은 `KVM_CAP_SYNC_REGS`를 조회하고 지원되는 `KVM_SYNC_X86_REGS`와 `KVM_SYNC_X86_SREGS`를 각각 사용합니다. 완료가 확인된 `KVM_RUN`은 공유 영역에 실제 레지스터를 반환하므로 연속으로 성공한 스텝에서 `KVM_GET_REGS`와 `KVM_GET_SREGS`를 생략할 수 있습니다. 미지원 집합이나 선택적 조회 실패는 ioctl 경로를 유지합니다. 변경된 입력은 여전히 `KVM_SET_GUEST_DEBUG` 전에 설치하며 공유 dirty 비트는 0으로 유지합니다. 예외, 취소, 캡처 실패는 재사용을 무효화합니다. 전체 FP/SSE 캡처는 필수지만 변경 없는 FP 입력의 XSAVE 재인코딩은 생략합니다. 이는 전송 호출 감소이며 전체 실행 속도 향상을 입증하지는 않습니다. [KVM API](https://docs.kernel.org/virt/kvm/api.html#kvm-cap-sync-regs).

하드웨어 실행 자체가 더 짧은 전체 지연 시간을 보장하지는 않습니다. 현재 네이티브 실행은 명령마다 허용 검사, 관찰, 상태 전송과 VM 종료를 수행합니다. 같은 원본 이미지와 시나리오를 동일한 명령·이벤트 예산으로 비교하고 시간과 함께 결과 일치를 보고해야 합니다. CLI 지연에는 시작과 로드도 포함합니다.

checked Unicorn은 `MachineRunControl`을 사용하며 ARM64 유지보수, 게스트 실행과 전체 상태 읽기를 한 단계 시간 한도에서 처리합니다. `UC_HOOK_CODE`는 명령 진입에서 빌린 정지 토큰과 기한을 확인합니다. 동기 엔진 호출은 반환 전에 hook 참조를 해제하지만 기계 단계는 상태 게시까지 제어를 유지합니다. Unicorn과 WHP는 전체 CPU 상태를 임시 저장하고 성공한 단계의 게시 직전에 같은 제어 조건을 확인합니다. WHP는 준비 전에 시간 한도를 한 번만 만듭니다. 확인된 x64 CPU 예외는 상태 읽기 중 도착한 정지 요청보다 우선합니다. 읽기가 취소되면 checked RAM 트랜잭션은 추측 쓰기를 버리며 비제한 소프트웨어 계약은 그대로입니다. `MachineInterruptedError`는 확인된 취소와 호스트 또는 상태 읽기 실패를 구분합니다. 공통 checked CPU는 `Stopped` 또는 `Deadline`을 반환하고 CPU/RAM을 유지하며 재시도를 허용합니다. 동시 정지 요청이 있어도 실제 실패는 `BackendFailure`로 남습니다.

`RunDeadline::invoke`는 중지되었거나 기한이 지난 WHP 실행을 호스트 호출 전에 거부하고, 취소 중에도 실제 호스트 결과를 보존하며, 빌린 중지 토큰을 해제하기 전에 인터럽트 콜백의 종료를 확인합니다. KVM과 WHP는 실행 임대를 보유한 호출 스레드에서 완전히 캡처된 비공개 상태를 검증한 다음 동시에 도착한 중지나 기한을 분류합니다. 실제 호스트·캡처 실패와 인증된 x64 CPU 예외가 우선합니다. 일반 성공 상태는 취소 확인이 끝날 때까지 비공개로 유지하며, 확인된 중단은 추측적 CPU/RAM 효과를 버리고 재시도를 허용합니다. 준비, 네이티브 실행, 캡처는 하나의 스텝 유예를 공유합니다. 협력적 취소를 제공하지만 엄격한 실제 시간 상한은 보장하지 않습니다.

## macOS HVF

`hvf` · Hypervisor.framework · Apple Silicon → ARM64 · Intel Mac → x86-64.

[Setup, signing and native hardware validation (English)](../macos-hvf.md)
