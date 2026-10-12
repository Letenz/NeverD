**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: b504f377f29d587519311580a92c9cdaed9118057832a8afd81551fbd34802df -->

[← Documentation Index](README.md)

# 소스 및 네이티브 오브젝트 패치

NeverD는 선택한 컴파일러로 대체 함수를 컴파일한 뒤 명시된 원래 함수 진입점에 설치합니다. 컴파일러는 C/C++ 의미를, NeverD는 주소 바인딩, 재배치, 저장 공간, 언와인드 레코드와 출력을 담당합니다. 바이너리 수정과 리프팅 후 재작성은 각 파이프라인을 유지하며 나머지 프로그램은 다시 빌드하지 않습니다.

<!-- i18n-section: compiler -->
## 컴파일러 선택

`--compiler=clang`, `--compiler=gcc`, `--compiler=neverc` 중에서 선택합니다. Clang과 일반 모드 NeverC는 LLVM IR을 생성할 수 있으며 `object`는 컴파일러의 기계어를 보존합니다. CLI는 GCC에 오브젝트를 기본으로 선택합니다. Python SDK에서는 GCC에 `artifact="object"`를 명시해야 합니다. 다른 컴파일러로 조용히 대체하지 않습니다.

예시 주소를 확인된 원래 함수 진입점으로 바꾸세요. 대체 선언은 호출 규약과 구조체 배치를 포함한 원래 기계 ABI와 일치해야 하며 주소만으로 이를 증명할 수 없습니다. C++에는 `--language=c++`와 적절한 Clang++/G++ 경로가 필요합니다. `extern "C"` 진입점을 사용하거나 정확한 맹글링 이름을 전달하세요. 추가 보조 함수는 허용됩니다. `--entry`와 `--func`가 대체 대상을 명시하며 기존 `--from-c`도 지정 주소를 따릅니다.

```sh
# Clang C -> LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.clang

# NeverC ordinary C -> native object
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC -> native object (CLI default for GCC)
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/gcc \
  --patch-report -o app.gcc

# Independently compiled ELF x64 object
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

<!-- i18n-section: profiles -->
## 검증된 프로파일 및 제한

| 컴파일러 | 산출물 | 검증된 OS 실행 프로파일 |
|---|---|---|
| Clang 22.1.6 | LLVM IR | ELF x64 C/C++; Mach-O x64/ARM64 C/C++; PE x64 C/C++ FH3 |
| Clang 22.1.6 | 네이티브 오브젝트 | ELF x64 C/C++ |
| GCC/G++ 13.3.0 | 네이티브 오브젝트 | 호환되는 Itanium 런타임의 ELF x64 C/C++ |
| NeverC 3389.1.5 일반 모드 | LLVM IR | ELF x64 C; Mach-O x64/ARM64 C |
| NeverC 3389.1.5 일반 모드 | 네이티브 오브젝트 | ELF x64 C |

이 표는 검증된 설정이며 모든 컴파일러 버전이나 옵션의 호환성을 보장하지 않습니다. `dyncode`와 NeverC C++는 지원하지 않습니다. GCC는 LLVM IR을 생성할 수 없습니다. 네이티브 COFF/Mach-O 오브젝트는 지원하지 않으며 오브젝트 설치에는 ELF x64가 필요합니다. 알 수 없는 재배치, TLS, 생성자, LTO, common/IFUNC 심볼과 미해결 임포트는 명확히 실패합니다. JSON 기록은 컴파일러, 인수, 바인딩과 설치 주소를 포함하며 네이티브 오브젝트는 SHA-256도 기록합니다.

별도 CPU 매트릭스는 4개 아키텍처(`x86`, `x64`, `ARM32`, `ARM64`)와 3개 형식(`ELF`, `PE`, `Mach-O`)에서 소스 설치 또는 명시적 거부를 확인합니다. 격리된 CPU 실행은 OS 로더, C++ 언와인더 또는 임의 ABI의 검증이 아닙니다. 해당 주장은 위의 OS 실행 프로파일을 기준으로 판단해야 합니다.

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `REFUSED` | `CPU` |
| `PE` | `REFUSED` | `CPU` | `REFUSED` | `REFUSED` |
| `Mach-O` | `REFUSED` | `CPU` | `REFUSED` | `CPU` |

`CPU`는 소스 설치 및 격리 함수 실행, `REFUSED`는 설치 계약 부족으로 인한 명시적 거부를 뜻합니다. ARM32 ELF에는 EHABI 등록, 32비트 Mach-O에는 검증된 로더/프런트엔드/언와인드 계약이 필요합니다. PE 소스 설치는 현재 x64만 허용합니다.

CPU 매트릭스는 독립적인 산술, 분기 및 보조 함수 호출을 사용합니다. 결과는 ELF x86/ARM64 데이터 참조나 ARM64 C++ 임포트를 검증하지 않습니다. 추가 실행 탐색에서는 미지원 재배치 계약으로 설치를 명시적으로 거부했습니다.

<!-- i18n-section: references -->
## 원래 참조 및 저장 공간

바인딩은 외부 선언에 대응합니다. `function`에는 확인된 원래 함수/임포트 스텁, `data`에는 쓰기 가능한 저장 공간, `const`에는 const IR 선언이 필요합니다. 오브젝트 데이터 바인딩은 0이 아닌 범위가 필요합니다. 저장 경계를 넘거나 RELRO에 쓰기 권한을 부여할 수 없습니다. 대체되는 원래 함수 본체로의 콜백은 거부됩니다. 이 프로파일에는 원래 프롤로그를 저장하는 콜백 트램펄린이 없습니다.

새 코드는 RX, 변경 가능한 상태는 RW로 유지됩니다. 기존 임포트만 재사용하며 GOT/IAT 슬롯과 호출 가능한 스텁을 구분합니다. Mach-O에는 `-Wl,-headerpad,0x400` 같은 로드 명령 여유 공간, PE에는 섹션 헤더 여유 공간이 필요합니다. 공간이 부족하면 실패합니다. ELF `.data.rel.ro`와 원래 CET/IBT 보호에는 추가 계약이 필요하여 현재는 거부합니다.

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

Mach-O 언와인드 테이블 확장에는 인접한 파일 및 가상 공간의 여유도 필요합니다. 헤더 여유 공간만으로 확보되지 않습니다. 검증 입력은 두 요구 사항을 별도로 충족합니다.

<!-- i18n-section: exceptions -->
## C++ 예외

ELF는 컴파일러 FDE/LSDA, Mach-O는 compact unwind/DWARF를 사용합니다. PE x64는 컴파일러 FH3 테이블을 허용하며 원래 프로그램에 호환 MSVC 런타임과 타입 식별자가 필요합니다. type-info 데이터는 명시적 바인딩이 필요할 수 있습니다. FH4, 사용자 정의 personality, SEH 소스 및 활성 CFG/EHCont/XFG 모드는 거부하며 원래 보호 플래그는 유지합니다.

실행 사례는 정상 반환, 내부 catch, 원래 호출자의 새 예외 포착, 원래 피호출 함수의 예외 전파, 정확히 한 번의 RAII 정리, 재던지기, noexcept 종료와 변경하지 않은 함수를 검증합니다. 원래 호출 지점은 예외 전파를 이미 허용해야 합니다. 원래 컴파일러가 예외를 던지지 않는 함수 주위에서 제거한 catch 경로는 패치로 복원할 수 없습니다.

<!-- i18n-section: publication -->
## SDK 및 출력

추가 방식 C API는 `neverd_patch_source`와 `neverd_source_patch_options_v1`이며 `neverd_patch_source_report`가 기록을 반환합니다. 오브젝트 입력은 바이트 길이를 전달합니다. 기존 `neverd_patch_from_c`도 사용할 수 있습니다.

입력을 스냅샷으로 저장하고 목적지 옆에 후보를 생성한 다음 다시 로드하여 요청된 모든 진입점을 확인하고 배타적으로 출력합니다. 기존 파일이나 심볼릭 링크 별칭은 덮어쓰지 않습니다. 매핑 하나가 누락되어도 전체 요청이 실패합니다. `--mode=inplace`는 거부됩니다. 세션 IR 변환은 네이티브 오브젝트에 적용할 수 없습니다. Mach-O는 서명을 엄격히 확인하고 재서명하며 PE 서명을 조용히 제거하지 않습니다.

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

<!-- i18n-section: verification -->
## 빌드 및 검증

`NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE`로 호환 컴파일러를 선택하세요(Clang 22.1.6 검증). 통합 빌드는 빌드 디렉터리의 LLVM 복사본만 수정하고 서브모듈과 공유 사전 빌드 캐시는 유지합니다. r3/r4의 x64 PLT 버그는 IR 패치를 거부하게 할 수 있습니다. 네이티브 오브젝트는 해당 MC 경로를 거치지 않습니다. `NEVERD_TEST_REQUIRE_FIXED_LLVM=1`은 수정된 정확한 PLT 생성을 요구합니다.

`NeverDSourcePatchTests`는 네이티브 C/API/오브젝트/예외를, `NeverDSourcePatchMatrixTests`는 Unicorn으로 모든 조합을 검사합니다. `NEVERD_TEST_WINDOWS_CRT_LIB`, `NEVERD_TEST_WINDOWS_UCRT_LIB`, `NEVERD_TEST_WINDOWS_SDK_LIB`를 설정하면 `SourcePatchPE`가 C++ 시나리오 7개를 실행합니다. Windows 이외 호스트에는 `NEVERD_TEST_WINDOWS_RUNNER`도 필요합니다. `NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1`은 입력 누락을 실패로 처리합니다. Windows 전용 워크플로는 네이티브 실행을 요구하고 건너뛴 사례를 거부합니다.

개발 Mac에서 Linux 검증은 Docker와 명시적 Linux 로더를, PE 검증은 Wine 11을 사용합니다. Mach-O ARM64는 Mac 자체에서 실행합니다. Windows 네이티브 CI 결과는 별도로 확인해야 하며 작업 구성만으로 실행 성공을 의미하지 않습니다. 기록, 기존 실패 및 에뮬레이터 제한은 [상세 계약과 검증 기록](../source-patching-details.md)을 참조하세요.

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=ON \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDSourcePatchMatrixTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDSourcePatchMatrixTests)$' \
  --output-on-failure --parallel 4
```

LLVM 빌드 복사본은 Mach-O x64 GOT 데이터 참조도 수정합니다. Clang의 타입 catch 테이블은 4바이트 재배치 필드 끝을 기준으로 합니다. r3/r4는 4바이트 어긋나며 출력 전 검증에서 수정되지 않은 결과를 거부합니다. 개발 Mac의 x64 Mach-O 사례는 Rosetta 2로, ARM64 사례는 직접 실행합니다.

Windows x64 네이티브 검증 [96cf7cd59](https://github.com/NeverSight/NeverD/actions/runs/38200205814)에서 PE/객체/trampoline 전용 테스트 15개와 대상 행렬 테스트 48개가 건너뛰기 없이 모두 통과했습니다. FH3 테스트는 C++ 실행 시나리오 7가지를 검증했습니다. 이 실행은 Clang 22.1.6과 네이티브 MSVC 및 Windows SDK를 사용했습니다.
