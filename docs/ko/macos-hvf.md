**언어**: [English](../macos-hvf.md) | [简体中文](../zh-CN/macos-hvf.md) | [繁體中文](../zh-TW/macos-hvf.md) | [日本語](../ja/macos-hvf.md) | [한국어](macos-hvf.md) | [Français](../fr/macos-hvf.md) | [Deutsch](../de/macos-hvf.md) | [Español](../es/macos-hvf.md) | [Italiano](../it/macos-hvf.md) | [Русский](../ru/macos-hvf.md) | [العربية](../ar/macos-hvf.md)

<!-- i18n-source: 4fe3725ff9e09f256ae2f0f1ff627f7530bc5c57e8a057aba3f161e8650ed6f1 -->

[← 문서 목록](README.md)

# macOS 네이티브 CPU 실행(HVF)

NeverD는 macOS에서 KVM/WHP에 대응하는 [Hypervisor.framework](https://developer.apple.com/documentation/hypervisor)를 사용합니다. `--backend hvf`는 명시적 선택입니다. `auto`는 네이티브 실행을 허용하는 계약에서 호스트와 게스트 ISA가 일치할 때 HVF를 선택합니다. Apple Silicon에서는 ARM64, Intel Mac에서는 x86-64를 실행합니다. `software-cpu-v1`과 서로 다른 ISA 사이의 자동 선택은 Unicorn을 사용합니다. Rosetta로 변환된 실행 파일은 거부합니다.

macOS 11 이상과 하드웨어 가상화가 필요합니다. 다른 의존성의 최소 OS 버전은 별개입니다. 선택한 네이티브 백엔드를 사용할 수 없으면 원인을 보고하며 다른 백엔드로 조용히 전환하지 않습니다. [Virtualization.framework](https://developer.apple.com/documentation/virtualization)는 전체 VM용 API입니다. NeverD에는 Hypervisor.framework의 vCPU, 레지스터, 메모리 매핑 및 예외 제어가 필요합니다.

## 빌드와 서명

`NEVERD_ENABLE_CPU_EMULATION=ON` 또는 드라이버 에뮬레이션을 활성화합니다. `NEVERD_EMULATION_BACKEND_HVF`는 기본값이 `ON`이며 macOS에서만 프레임워크를 링크합니다. `OFF`에서도 `hvf` 이름을 인식하지만 기능 API는 `build_disabled`를 반환합니다.

**프로세스 실행 파일**에 [`com.apple.security.hypervisor`](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.hypervisor)가 필요합니다. `libneverd.dylib`만 서명해서는 충분하지 않습니다. CMake는 `resources/macos/neverd-hypervisor.entitlements`로 CLI, worker, 테스트를 서명합니다. `NEVERD_HVF_SIGN_IDENTITY`의 기본값은 임시 서명용 `-`이며 기존 서명 ID도 지정할 수 있습니다. 패키징은 Mach-O 의존성 수정 후 권한을 다시 적용하고 검증합니다.

라이브러리를 포함하는 애플리케이션은 자체 실행 파일을 서명해야 합니다. NeverD는 설치된 Python을 다시 서명하지 않습니다. `cpu-capabilities --configuration=JSON --probe-host`는 실제 프로세스를 검사합니다. 독립 worker도 기본적으로 서명하며, HVF가 필요하지 않을 때만 `NEVERD_WORKER_SIGN_HVF=OFF`를 사용합니다.

## 소유권과 실행

`backends/hvf/HvfExecutor`는 프로세스당 VM 하나와 vCPU 하나를 소유하고 전용 스레드에서 생성, 사용, 삭제합니다. 논리 CPU는 실행기를 공유하고 네이티브 진입을 직렬화합니다. CPU를 전환하기 전에 이전 소유자의 두 물리 영역을 해제합니다. 비활성 CPU를 삭제해도 다른 CPU의 매핑은 바뀌지 않습니다. RAM을 해제하기 전에 바인딩을 동기적으로 분리하며 부분 등록 실패는 롤백합니다. 매핑 해제가 실패하면 메모리보다 VM을 먼저 종료합니다. 복구할 수 없는 정리 실패는 프로세스를 중지합니다.

호스트 매핑은 Apple Silicon의 16 KiB를 포함한 호스트 페이지 크기를 따릅니다. 아키텍처 페이지 테이블과 게스트 CPU 예산은 4 KiB로 유지합니다. 명령 허용 판단, 권한, CPU 상태, 메모리 트랜잭션 및 OS 서비스는 각 계층이 담당합니다. 네이티브 worker는 게스트 관찰자를 호출하거나 호출자 코어 메모리 잠금을 획득하지 않습니다.

ARM64는 고정된 TLB/I-cache 유지 관리 명령 다섯 개를 단일 단계로 실행한 다음 허용된 게스트 명령을 실행합니다. `PSTATE.D`는 EL2로 전달되는 디버그 예외를 가리지 못합니다. 스칼라, TLS, FP/SIMD 상태를 모두 캡처합니다. Intel은 VMCS 제어를 협상하고 monitor trap, TLB 무효화, 완전한 XSAVE 패킷을 사용합니다. RIP/RFLAGS는 vCPU 재생성 후에도 VMCS를 통해 직접 전송합니다. CR0/CR4는 프레임워크 마스크와 하드웨어 고정 비트를 따릅니다. 인증된 CR8 읽기 종료는 ISA 계층이 완료하며 다른 제어 레지스터 접근은 실패합니다. 각 vCPU는 전용 관리 대상 `IA32_KERNEL_GS_BASE`를 초기화합니다. 게스트 MSR 접근은 가로채며 지원하지 않는 MSR/SWAPGS는 허용하지 않습니다.

대기열은 원래 중지 토큰과 기한을 유지합니다. 준비, 유지 관리, 진입, 캡처는 하나의 시간 예산을 공유합니다. `RunDeadline`은 인터럽트 확인이 끝난 후 반환합니다. 취소 시 vCPU를 재생성해 늦게 도착하는 인터럽트를 격리합니다. 무관한 Intel 호스트 인터럽트는 같은 취소 세대에서 재시도합니다. 캡처 오류와 인증된 예외는 동시 중지보다 우선하며 일반적인 취소 상태는 공개하지 않습니다. 협력적 취소이며 엄격한 실시간 보장은 아닙니다.

## 검증 방법

Release와 CMake, Ninja, Python 3, Clang, `ld.lld`, `lld-link`, `ld64.lld`, `codesign`을 사용합니다. LLVM 링커는 ELF, PE, Mach-O 테스트 입력을 생성합니다. 필수 네이티브 입력이 없으면 검증에 실패합니다.

```sh
cmake -S . -B build-hvf -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_BUILD_SHARED=OFF -DNEVERD_ENABLE_PYTHON_PLUGINS=OFF \
  -DNEVERD_ENABLE_CPU_EMULATION=ON \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=OFF \
  -DNEVERD_EMULATION_BACKEND_UNICORN=OFF
python3 scripts/run_native_cpu_ci.py --build build-hvf \
  --evidence build-hvf/native-evidence --require-hvf
```

Apple Silicon에서는 `-DNEVERD_LLVM_PREBUILT=ON`을 추가할 수 있습니다. Intel은 고정된 LLVM 리비전을 소스에서 빌드합니다. [HVF 워크플로](../../.github/workflows/hvf.yml)는 `self-hosted, macOS, ARM64/X64, hvf`와 `macos-15-intel`의 `hosted-intel`을 지원합니다. 먼저 실제 VM/vCPU 생성과 삭제를 확인합니다. `validation=probe`는 명령 실행의 증거가 아니며, `transport`는 전송 계층만, `darwin`은 일치하는 모든 Darwin 작업을, `full`은 전체 CPU와 Darwin 검증을 모두 요구합니다.

전송 계층의 필수 항목은 ARM64 12개, Intel 10개이며 전체 CPU 검증은 각각 16개, 14개입니다. 전체 상태, 특권, 권한, 페이지 경계, 별칭, CPU 전환, 롤백, 취소 및 재시도를 확인합니다. Intel은 큰 빌드 전에 CR8을 검사합니다. 산출물에는 목록, 소스 리비전, 호스트, 결과 및 각 재실행을 보관합니다. [GitHub](https://docs.github.com/en/actions/concepts/runners/github-hosted-runners)는 중첩 가상화를 실험적 기능으로 분류하므로 반복 가능한 검증을 위해 전용 네이티브 Mac 경로를 유지하는 것이 좋습니다.

호스팅 Intel의 `--execution-methods`는 전체 CTest 목록의 모든 매개변수, 플래그, 환경 및 작업 디렉터리를 유지하여 GoogleTest 메서드를 직렬 실행합니다. 알 수 없는 속성은 거부합니다. 메서드 전체 기한은 최대 120초이며 내부 매개변수마다 별도 기한을 적용하지 않습니다. 이후 제한된 시간 안에 프로세스 그룹을 회수합니다. 원본 XML, 이름 대응, 종료 상태를 보존합니다. 시간 초과나 불완전한 XML은 부분 실패를 만들고, 필수 네이티브 항목 누락이나 건너뛰기는 통과할 수 없습니다. 자체 runner는 CTest의 사례별 프로세스와 기한을 유지합니다.

## 결과와 제한

2026-10-03 기록입니다. 행의 범위가 겹치므로 합산하지 않습니다.

| 범위 | 소스 | 통과 | 실패 | 건너뜀 | 필수 네이티브 |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 CPU 전체 20개 대상 | `defc93928` | 849 | 0 | 5,993 | 16/16 |
| ARM64 Darwin | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel Darwin | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| 전체 Intel FP 대상 | `3e01cda5c` | 35 | 0 | 36 | HVF 12개 |

ARM64는 등록 항목 6,842개를 대조했고 Intel은 같은 20개 대상에 6,840개가 있습니다. Intel 예외, 나눗셈, 상태 전환의 네이티브 253개도 모두 통과했습니다. [Intel Darwin 실행](https://github.com/NeverSight/NeverD/actions/runs/37106013999)은 식별자 286개와 프로세스 32개를 대조하고 전송 계층 10개, 복구 100회, CR8도 통과했습니다. 수집기와 감사 검사 124개가 통과했습니다. CLI, SDK, worker, Mach-O 186개 서명을 통합 검증했으며 해당 패키지 의존성은 macOS 15.0을 요구합니다.

[Intel 전체 CPU 검증](https://github.com/NeverSight/NeverD/actions/runs/37106679688)은 미완료입니다. 2026-10-03 08:24 UTC 기준으로 설정한 기한이 지나도 최종 결과나 CPU 산출물이 없었습니다. 빌드와 사전 검사는 전체 결과를 대신하지 않습니다. macOS 커널 대조는 iOS 실기기 커널의 증거가 아닙니다.

동일 계약의 작은 ARM64 벤치마크는 Unicorn 73.9 ms, HVF 95.1 ms로 약 29 % 더 오래 걸렸습니다. 속도 향상은 입증되지 않았습니다. 일반 명령당 네이티브 진입 여섯 번이 필요하며 호스트 고부하 측정은 안정적 성능의 근거가 아닙니다. [자세한 증거](../macos-hvf.md#implementation-validation-2026-10-02-to-2026-10-03)와 [제한된 Darwin 계약](darwin-emulation.md)을 참고하세요.

## Intel 전체 목록의 분할 검증

이전 전체 실행 `37106679688`은 2026-10-03 08:35 UTC에 끝났고 GitHub는 실행기 통신 단절을 기록했습니다. 실행 `37116327329`의 작업 0, 1도 통신이 끊겼으며 CPU XML을 남기지 못했습니다. 이 사실만으로 실패한 게스트 명령을 특정할 수 없습니다. 호스팅 Intel의 `full`은 네 작업을 사용하며 최대 두 작업을 동시에 실행합니다. 각 작업이 네 배치를 순서대로 실행하여 총 열여섯 샤드를 구성하고, 번호는 `job + 4 × batch`로 정합니다. 기존 작업별 테스트 집합을 유지합니다. 각 배치는 먼저 대상 스무 개의 전체 CTest 목록을 빌드하고 검사합니다. `--hvf-shard INDEX/COUNT`는 실행 속성이 달라도 메서드 전체와 모든 매개변수를 같은 샤드에 유지합니다.

CPU 실행 전에 `scripts/prepare_hvf_batches.py`가 전체·선택 목록과 메서드 계획을 저장하고 워크플로가 진단 자료로 별도 업로드합니다. 로컬 composite action은 네 배치를 순서대로 실행하며 배치마다 원본 XML, 식별자 매핑, 프로세스 상태와 필요한 환경 변수 허용 목록을 즉시 업로드합니다. 외부의 30분 제한은 네 배치와 모든 업로드를 함께 포함합니다. 배치 하나가 실패하면 후속 실행을 중단합니다. 실패나 시간 초과 후에도 실행기와 통신할 수 있으면 별도의 2분 진단 업로드를 수행합니다. 실행기가 단절되면 이전에 업로드한 증거만 남습니다. 계획이나 미완료 진단 묶음은 통과한 샤드로 인정하지 않습니다.

별도 Linux 작업의 `scripts/audit_hvf_shards.py`는 체크아웃한 소스에서 대상과 필수 네이티브 항목을 다시 도출합니다. 워크플로는 현재 시도의 CPU 아티팩트만 내려받으며, 감사는 같은 깨끗한 커밋의 열여섯 샤드, 올바른 macOS 호스트 ISA, 일치하는 정규화 실행 계약을 요구합니다. 샤드는 겹치지 않고 합집합이 전체 목록과 정확히 같아야 하며 모든 자식 프로세스와 필수 항목이 성공해야 합니다. 누락, 필터 변경, 요약 불일치, 불완전한 XML, 필수 항목 건너뜀은 실패입니다. 각 네이티브 작업은 전송, 복구, CR8 및 독립 Darwin 검사도 유지합니다. 자체 호스팅은 분할하지 않은 CTest를 사용합니다. 배치 저장 자체가 Intel 검증 완료를 의미하지는 않습니다. 재시도에서는 모든 네이티브 작업을 다시 실행하며 이전 시도의 아티팩트를 합치지 않습니다.

## 최신 로컬 네이티브 검증

2026-10-03의 깨끗한 소스 `353dcd75f`에서 ARM64 열여섯 샤드가 통합 감사를 통과했고 독립 Darwin 검사도 통과했습니다. 두 행은 중복되므로 합산하지 않습니다.

| 범위 | 등록 | 통과 | 실패 | 건너뜀 | 필수 네이티브 |
| --- | ---: | ---: | ---: | ---: | ---: |
| CPU, 열여섯 샤드 | 6,908 | 857 | 0 | 6,051 | 16/16 |
| Darwin | 286 | 65 | 0 | 221 | 39/39 |

`build-hvf-native/hvf-batches-353dcd75f/aggregate.json` · `build-hvf-native/hvf-batches-353dcd75f/darwin/summary.json`
