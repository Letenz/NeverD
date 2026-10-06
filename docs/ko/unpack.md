**언어**: [English](../unpack.md) | [简体中文](../zh-CN/unpack.md) | [繁體中文](../zh-TW/unpack.md) | [日本語](../ja/unpack.md) | [한국어](unpack.md) | [Français](../fr/unpack.md) | [Deutsch](../de/unpack.md) | [Español](../es/unpack.md) | [Italiano](../it/unpack.md) | [Русский](../ru/unpack.md) | [العربية](../ar/unpack.md)

[← 문서 색인](README.md)

# 패킹된 실행 파일 언패킹

`neverd unpack`은 패킹된 실행 파일이 자신의 주소 공간 안에 다시 만들어 내는 프로그램을 복구합니다. 입력을 유한한 게스트 프로세스로 실행하고, 스텁이 자신이 생성한 코드로 제어를 넘기는 지점을 관찰하여, 그 시점의 이미지를 같은 컨테이너 형식의 새 파일로 기록합니다. 가상화 해제는 하지 않습니다. 보호 도구가 가상화한 함수는 가상화된 채로 남습니다. 빌드에는 `NEVERD_ENABLE_CPU_EMULATION=ON`이 필요합니다.

## 지원하는 입력

컨테이너 형식은 파일을 검증하고 재구성하는 방법을, 명령어 집합은 전이를 판정하는 방법을 정하며, 둘이 함께 게스트 프로세스 프로필을 정합니다. 이 표에 없는 입력은 어떤 코드도 실행되기 전에 이름과 함께 거부됩니다.

| 컨테이너 (`format`) | 명령어 집합 | 게스트 프로필 | 스텁 지식 |
| --- | --- | --- | --- |
| PE32+ (`pe64`) | x86-64 | [`windows-pe64-v1`](process-emulation.md) | UPX |
| PE32+ (`pe64`) | ARM64 | [`windows-pe64-v1`](process-emulation.md) | 없음(관찰만) |

## 사용법

```bash
neverd unpack packed.exe -o unpacked.exe
neverd unpack packed.exe -o unpacked.exe \
  --options='{"backend":"unicorn","instruction_limit":400000000,"transfer":2}'
```

이 명령은 JSON 보고서 하나를 출력합니다. 종료 코드 0은 이미지가 기록되었음을, 3은 진입점이 채택되기 전에 유한 실행이 끝났음을(`outcome`은 `no_entry`이며 아무것도 기록되지 않음), 1은 입력이나 옵션이 잘못되었거나 준비에 실패했음을 뜻합니다. 보고서에는 실제로 실행된 `format`, `architecture`, `profile`이 기재됩니다. C 진입점은 `neverd_unpack_json`이고 Python은 `Session.unpack`을 제공합니다. 옵션은 [프로세스 옵션](process-emulation.md)에 `transfer`를 더한 것입니다. 스텁에 더 많은 자원이 필요한 항목은 기본값이 다릅니다. 명령 100000000개, 600초, 512 MiB이며 `windows.defer_unmodeled`가 켜져 있습니다.

## 진입점을 정하는 방법

0세대는 게스트 로더가 매핑한 그대로의 이미지입니다. 그 이미지와 바이트가 다른 명령은 프로세스가 스스로 생성한 것입니다. 전이는 실행 중이던 코드보다 새로운 코드가 처음 실행되는 것을 말합니다. `transfers`에는 각 전이의 RVA, `generation`, 그리고 스택 포인터가 프로세스 진입 시의 값과 같은지 여부(`stack_balanced`)가 나열됩니다.

1. 진입 시의 스택에서 일어난 전이가 프로그램의 진입점입니다. 스텁이 받은 스택을 되돌려 놓았기 때문입니다. 이때 `entry_source`는 `transfer`입니다.
2. 더 깊은 스택에서 일어난 전이는 TLS 콜백처럼 스텁이 프로그램을 호출한 것입니다. 이는 보고되지만 채택되지 않습니다. 식별된 스텁이 마지막 점프 대상을 명시하는 경우, 이미지는 그 호출 시점(프로그램의 코드가 아직 전혀 실행되지 않은 시점)에서 재구성되며 명시된 주소가 진입점이 됩니다. 이때 `entry_source`는 `stub`입니다.
3. `transfer`는 나열된 전이를 위치로 명시적으로 선택합니다. 여러 단계로 푸는 보호 도구나 프로그램을 호출 방식으로 진입시키는 보호 도구를 위한 것입니다.

컴파일러 시작 코드의 형태로 진입점을 추측하지 않습니다. 실행이 먼저 멈추면 `no_entry`와 프로세스의 `stop_reason`이 보고됩니다.

## 재구성된 이미지

각 섹션은 RVA를 유지하며 관찰된 메모리를 담습니다. 각 섹션의 접근 권한은 전이 시점에 해당 페이지가 가졌던 권한입니다. 마지막 `.neverd` 섹션에는 프로그램이 원래 경유하여 호출하던 셀 위에 새 임포트 디렉터리가 놓이므로 코드와 데이터는 이동하지 않습니다. `imports`에는 각 셀과 그 `origin`이 나열됩니다. `static` 셀은 로더가 입력 자체의 디렉터리로부터 바인딩한 것이고, `runtime` 셀은 스텁이 기록한 것입니다. 이미지는 관찰된 베이스에 고정됩니다. 생성된 내용에 대한 재배치는 관찰되지 않았으므로 재배치 디렉터리를 제거하고 `IMAGE_FILE_RELOCS_STRIPPED`를 설정합니다. UPX의 경우 패킹된 디렉터리는 스텁의 처리기에만 도달하므로 프로그램 자체의 TLS 디렉터리를 다시 가리키게 합니다.

원래의 섹션 테이블, 임포트 디렉터리 배치, 재배치 테이블은 재구성되지 않습니다. 패커가 이를 메모리에 복원하지 않기 때문입니다.

## 식별

`packer.kind`는 파일 안의 증거만으로 보호 도구를 지목합니다. UPX는 `upx_section_names`, `upx_pack_header`(매직, 형식, 방식, 체크섬), `upx_entry_stub` 중 두 가지가 필요합니다. 식별되지 않은 입력도 관찰을 통해 언패킹됩니다.

## 제한

실행 파일만 지원하며 DLL은 실행하지 않습니다. 검사 실행은 한 번에 명령 하나씩, 초당 10^5개 수준으로 허용하므로 수십억 개의 명령이 필요한 스텁은 현실적인 예산을 넘어섭니다. 실행은 4 KiB 페이지 단위로 추적됩니다. 같은 세대의 코드가 이미 실행 중인 페이지에 기록된 코드는 전이로 보고되지 않습니다. 진입점 이전에 실행되는 프로그램 코드(모델링되지 않은 API를 호출하는 TLS 콜백 등)는 스텁이 진입점을 선언하지 않는 한 실행을 멈춥니다. VMProtect 로더는 아직 지원하지 않습니다.

## 검증

`NeverDUnpackTests`는 식별을 검사합니다. `NeverDUnpackExecutionTests`는 저장소에 포함된 UPX 픽스처(NRV2B, NRV2D, NRV2E, LZMA, 그리고 C 런타임 프로그램)를 Unicorn, KVM, WHP에서 언패킹하고, 모든 섹션을 원본과 비교하며, 복구된 이미지를 실행하고, 모든 백엔드가 동일한 바이트를 내놓을 것을 요구합니다. `UnpackGeneratedTests.cpp`는 테스트 안에서 x86-64와 ARM64용 프로그램을 패킹하고, 그대로 떠나는 로더, 먼저 프로그램을 호출하는 로더, 2단계 로더를 링크된 파일과 대조하여 검사합니다. `NeverDUnpackPublicTests`는 C ABI와 CLI를 다룹니다. `unittests/unpack/fixtures/Makefile`로 UPX 픽스처를 다시 생성합니다.
