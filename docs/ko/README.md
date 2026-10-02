**언어**: [English](../README.md) | [简体中文](../zh-CN/README.md) | [繁體中文](../zh-TW/README.md) | [日本語](../ja/README.md) | [한국어](README.md) | [Français](../fr/README.md) | [Deutsch](../de/README.md) | [Español](../es/README.md) | [Italiano](../it/README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

<!-- i18n-source: 4ad5761d6370d9ae8a26c9cf015210842c3c7817d1426b5df704408d1f0d1b7a -->

[← NeverD 프로젝트](project.md)

# NeverD 문서

프로젝트 개요·빌드·CLI는 저장소 README에 있습니다. 기여자용 설계와 테스트 자료를 여기에 모았습니다.

**모바일 지원(실험적 CLI):** `neverd mobile`은 [Android](android.md) APK, DEX, smali에서 Java를, [iOS](ios.md) IPA, `.app`, Mach-O에서 네이티브 C 및 지원되는 Objective-C/Swift 소스를 복원합니다. JSON 보고서는 복원 결과와 범위를 설명합니다. [모바일 개요](mobile.md)부터 읽고 플랫폼 가이드에서 명령과 제한을 확인하세요.

영어 가이드는 `docs/` 바로 아래에 있습니다. 번역은 `ar/`, `de/`, `es/`, `fr/`, `it/`, `ja/`, `ko/`, `ru/`, `zh-CN/`, `zh-TW/`로 나뉩니다. 각 언어 디렉터리는 문서 색인 `README.md`, 프로젝트 개요 `project.md`, 주제별 가이드, `CONTRIBUTING.md`, `ATTRIBUTION.md`, `roadmap.md`를 포함합니다. 공용 이미지는 `assets/`에 있습니다.

CPU 실행은 ISA 허용, 게스트 메모리, 백엔드 전송과 게스트 OS 정책을 분리합니다. `NEVERD_ENABLE_CPU_EMULATION`은 x64/ARM64 CPU 계층을 켜고 `NEVERD_ENABLE_DRIVER_EMULATION`은 제한된 x64 Windows WDM/KMDF 환경을 추가합니다. `linux-elf64-v1`은 지원되는 Linux ELF 프로세스를 실행합니다. [CPU 실행](cpu-execution.md), [게스트 프로세스 에뮬레이션](process-emulation.md), [Windows 드라이버 에뮬레이션](driver-emulation.md)를 참조하세요.

`driver-strict` / `checked-x64-v1`는 일치하는 Linux x64 host의 KVM과 Windows x64 host의 WHP를 지원합니다. `auto`는 해당 native transport를, cross-ISA는 Unicorn을 선택합니다. 명시적 Unicorn과 기존 V1 API는 portable software profile을 유지합니다. native 실행은 진입 전에 canonical address와 instruction effect를 검증하고, hardware가 없으면 fallback 없이 실패합니다. 지원되지 않는 instruction/OS behavior는 명시적 오류입니다. Windows x64 네이티브 CI는 Unicorn을 비활성화하고 필수 검사 359개를 모두 통과합니다. CPU 검사 131개, 내장 이미지 26개·WDK 이미지 46개·시나리오 사례 40개를 기본 및 재배치 주소에서 실행한 드라이버 결과 224개, SEH 경계 검사 4개를 포함합니다 ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). native ARM64 실기 증거는 아직 없으며, 임의 driver나 Android/Darwin 호환성을 의미하지 않습니다.

`checked-aarch64-v1`와 `checked-user-aarch64-v1`는 제한된 ARM64 FP32/FP64, 고정 폭 SIMD와 전체 FPCR/FPSR/벡터 상태를 제공합니다. ISA가 일치하는 Linux ARM64는 KVM, Windows ARM64는 WHP, 다른 ISA는 Unicorn을 사용합니다. 네이티브 ARM64 실기 검증은 남아 있으며 Windows 드라이버 로딩은 x64로 제한됩니다.

| 문서 | 설명 |
|------|------|
| [프로젝트 설명（한국어）](project.md) | 개요, 빠른 시작, 빌드, SDK, CLI |
| [기여 가이드](CONTRIBUTING.md) | 개발 환경, 빌드 프로필, 워크플로, 스타일, PR 요구 사항 |
| [아키텍처](architecture.md) | IR 경로, 구성 요소 경계, strict lifting, 지원 깊이, 수정 위치 |
| [테스트](testing.md) | 테스트 스위트, 생성 fixture, Unicorn 왕복, 증분 명령 |
| [데스크톱 워크벤치 (영문)](../gui.md) | 선택적 Qt Quick UI, 별도 작업 프로세스, C ABI, 주석과 MCP 작업 흐름 |
| [데스크톱 검증 기록 (영문)](../gui-qualification.md) | 측정된 GUI 증거, 패키징 범위와 남은 플랫폼 검증 |
| [인터프리터 소스 복원](interpreter-recovery.md) | 실험적 x64 인터프리터 특수화, HighC/LLVMC 출력, 실행 전제, 근거와 제한; 중첩 루프 증명 후보; 명시적 탐색 예산과 버전별 C API; 정확한 네이티브에서 LLVM으로의 증명 API |
| [Windows 예외 재구성](windows-exception-reconstruction.md) | SEH/C++ 지원 표, IR 계약, 네이티브 patch 규칙 및 PE 검증 |
| [CPU 실행](cpu-execution.md) | 구성, 기능 조회, 백엔드 가용성, 형식화된 결과 |
| [비트벡터 증명 백엔드](solver.md) | 선택적 Z3 증명, 증명 게이트 합성, 독립 검사, query 내보내기 |
| [게스트 프로세스 에뮬레이션](process-emulation.md) | Linux ELF 프로필, 시작, 서비스, 제한, 테스트 |
| [macOS/iOS 프로세스 환경](../darwin-emulation.md) | Mach-O 시작, 기기와 시뮬레이터 구분, Darwin 서비스와 페이지 규칙 |
| [macOS HVF](../macos-hvf.md) | 호스트와 동일한 ISA의 하드웨어 실행, 서명 권한, 패키징과 검증 |
| [Windows 드라이버 에뮬레이션](driver-emulation.md) | 제한된 x64 WDM/KMDF 수명 주기, 요청, 하드웨어 시나리오, SEH, PnP 하위 집합, 백엔드 선택 및 제한 |
| [메모리 안전성 감사와 헌트](memory-safety.md) | 힙 수명과 복사 오버플로 분석: 형식별 신원 계약, 싱크/소스 카탈로그, 판정, 예산, JSON 스키마 |
| [네이티브 플러그인](plugins.md) | 순수 C descriptor ABI, callback과 event, build/link workflow, discovery 및 호환성 규칙 |
| [Python 플러그인](python-plugins.md) | 플러그인 작성, 세션·이벤트 API, 격리, 테스트 및 배포 |
| [모바일 지원 개요](mobile.md) | Android / iOS 입력, 소스 출력, CLI 흐름 및 제한 |
| [Android Java 복구](android.md) | APK(multidex)·DEX·smali 파일/디렉터리 → Java. CLI 흐름, 실행 환경, 옵션, JSON 보고서, 오류 처리 및 검증 제한 |
| [iOS 소스 복원](ios.md) | IPA·`.app`·Mach-O → 네이티브 C 및 지원되는 Objective-C / Swift 소스. 입력 선택, 메서드 본문과 배치, CLI/export, JSON 커버리지 보고서, 제한 및 실행 검증 |
| [EVM 디컴파일](evm.md) | 입력, hardfork, 단계별 IR, C/LLVM host ABI, Solidity 복구 및 제한 |
| [Solana SBF 디컴파일](sbf.md) | SBF v0-v4, LLVM IR, C/Rust 출력, 검증 및 알려진 제한 사항 |
| [로드맵](roadmap.md) | 상태: native format, EVM, Solana SBF 구현 완료 |
| 다국어 문서 | 위의 언어 링크에서 각 언어의 색인과 프로젝트 개요를 열 수 있습니다 |

x64와 ARM64 네이티브 시작 검사는 독점 메모리 임대하에서 제한된 전체 상태 실행을 검증합니다. XSAVE 패킷과 ISA를 식별하는 페이지 테이블 캐시는 하나의 권한 계층이 관리하며 네이티브 ARM64 작업 증거는 아직 불완전합니다.

네이티브 x64 `FOP/FIP/FDP`는 호스트 저장·복원 규칙을 따르며 AMD는 비활성 x87 예외 메타데이터를 0으로 만들 수 있습니다. 시작 검사는 마스크되지 않은 대기 예외로 이 필드를 검증합니다.
