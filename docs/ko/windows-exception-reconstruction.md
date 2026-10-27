**언어**: [English](../windows-exception-reconstruction.md) | [简体中文](../zh-CN/windows-exception-reconstruction.md) | [繁體中文](../zh-TW/windows-exception-reconstruction.md) | [日本語](../ja/windows-exception-reconstruction.md) | [한국어](windows-exception-reconstruction.md) | [Français](../fr/windows-exception-reconstruction.md) | [Deutsch](../de/windows-exception-reconstruction.md) | [Español](../es/windows-exception-reconstruction.md) | [Italiano](../it/windows-exception-reconstruction.md) | [Русский](../ru/windows-exception-reconstruction.md) | [العربية](../ar/windows-exception-reconstruction.md)

# Windows 예외 재구성

[← 문서 색인](README.md)

NeverD는 Windows 테이블 기반 예외 정보를 로드, lift, 디컴파일, 바이너리 재작성 전 과정에
전달합니다. 예외 metadata는 함수 실행 계약의 일부입니다. 생성 코드, runtime-function
record, 언어 테이블, guard table의 일관성을 증명할 수 없는 재작성은 거부됩니다.

이 문서는 세 가지 지원 수준을 구분합니다.

- **분석**: 네이티브 표현을 검사된 정규화 record로 decode하여 IR pipeline에 제공.
- **디컴파일**: reducible protected region은 명시적 HighIR 예외 node로 만들고, 나머지는
  handler나 state transition을 잃지 않는 결정적 네이티브 주석으로 유지.
- **네이티브 재구성**: patch mode가 LLVM으로 완전한 대체 예외 계약을 생성하고 최종 PE에 설치.

분석 지원이 네이티브 재구성 지원을 의미하지는 않습니다.

## 지원 표

| 네이티브 형식 | Lift/분석 | 고수준 출력 | Patch mode |
|---------------|-----------|-------------|------------|
| x64 unwind v1/v2 | 완전하고 검사된 unwind record, operation, chain, handler data, provenance | frame/unwind 요약 및 해당 시 구조화 언어 region | 완전한 primary record 지원. 생성 `.pdata`/`.xdata`가 대체 closure를 갱신 |
| x64 unwind v3/APX | 전용 v3 payload, epilog, operation accounting | 명시적 v3 주석 | 분석 전용. 대상 함수 변경 거부 |
| ARM32/ARM64 packed unwind | function range, packed field, primary/fragment identity | frame/unwind 요약 | 완전한 비언어 primary record이고 독립 addressable fragment가 없을 때만 |
| ARM32/ARM64 unpacked unwind | 검사된 xdata header/code extent, handler association, fragment | frame/unwind 요약 | 완전한 비언어 primary record이고 독립 addressable fragment가 없을 때만 |
| `__C_specific_handler` | scope range, filter, finally target, handler, continuation target | reducible region은 `__try`/`__except`/`__finally`, 나머지는 주석 | 완전하고 표현 가능한 scope graph의 네이티브 x64 재구성 |
| `__CxxFrameHandler3` | unwind/try map, catch, catch-object/frame offset, continuation, IP-to-state map | reducible state interval을 명시적 C++ HighIR 및 C 호환 형식 주석으로 변환 | 아래의 좁은 verifier-clean subset에 대해 네이티브 x64 재구성 |
| `__CxxFrameHandler4` | action kind/object offset를 포함한 bounded variable-length decode | FH4 provenance를 가진 공통 HighIR graph | 분석 전용. 대상 함수 변경 거부 |
| `__GSHandlerCheck_SEH/EH/EH4` | wrapped personality와 검사된 GS cookie provenance | base language graph와 wrapper 주석 | 분석 전용. downgrade 없이 대상 함수 변경 거부 |
| x86 registration-chain EH | 검증된 SEH 체인 및 C++ 그래프 | callback 식별자를 유지하는 HighIR 영역 | 증명된 부분 집합의 PE32 재구성. 자세한 계약 참조 |

스칼라 참조 `catch` 내부의 `try`는 살아 있는 외부 예외 객체를 유지하고 전용 스택을 복원할 수 있습니다. 공유 callback과 일반적인 객체 수명은 아직 네이티브 재구성 범위에 포함되지 않습니다. [PE32 C++](../windows-exception-reconstruction.md).

Malformed record를 완전한 일반 record로 취급하지 않습니다. partial decode는 조사에 쓸 수 있지만
네이티브 metadata 생성을 허가하지 않습니다. ARM xdata header가 bounded executable fragment
range를 증명해도 뒤 unwind body가 손상되면 range는 disassembly에 남지만 record는 malformed로
표시되고 patchable function으로 승격되지 않습니다.

## 정규화 모델

`ExceptionInfo`는 `BinaryImage`가 소유합니다. 각 `ExceptionFunction`은 다음을 가집니다.

- 검사된 half-open code range.
- primary, chained, fragment identity.
- native unwind encoding과 정확한 runtime/unwind provenance.
- 정규화 unwind operation/epilog 및 미해석 operation의 opaque operand bytes.
- 정확한 personality identity와 handler data.
- 선택적 SEH scope, C++ state map, GS cookie data.
- `Complete`, `Partial`, `Malformed` status와 결정적 diagnostic.

loader는 모델을 통해 raw file pointer를 노출하지 않습니다. native RVA는 진단과 patch 교체용으로
보존하고 IR consumer는 검증된 VA/range만 사용합니다.

image-wide index는 chained/fragment record overlap을 허용하고 address를 덮는 가장 구체적인
function을 반환합니다. 손상된 directory/range/pointer/count/state transition/compressed integer,
chain cycle, decode-budget exhaustion은 해당 parse status를 낮춥니다.

language-table limit는 개별 테이블과 함수 전체 정규화 graph에 모두 적용합니다. 여러 try-map
entry가 같은 handler map을 재사용해도 총 예산을 넘지 않습니다. 동일 `FuncInfo`와 personality를
공유하는 FH3 record는 bounded function group으로 decode하므로 부모 IP-to-state map은 자신의
catch funclet을 참조할 수 있지만 관련 없는 runtime function address는 거부합니다.

좁은 정수 인수는 검증된 ECX/EDX의 8·16·32비트 폭을 유지하며 물리 스택 슬롯은 32비트입니다.
부호 및 0 확장은 원본 명령을 따릅니다. FS 등록 전의 인수 로드와 로컬 저장은 ESP, EBP와
등록 필드를 변경할 수 없습니다.

생성된 PE32 C++ 진입점은 완전한 소스 증명이 성공하면 다시 로드, 리프트 및 재구성할 수 있습니다.
loader는 실제 FS:[0] 쓰기, 정렬 프레임, SafeSEH 포인터 역할을 이미지 바이트에서 검증합니다.
설치기는 이전에 생성된 실행 섹션의 인증된 진입점도 수정하지만, 가상 및 파일 저장 범위가 유일하고
분석한 바이트와 일치해야 합니다. cdecl/stdcall/thiscall/fastcall 검체는 Microsoft x86 CRT에서
두 세대의 재구성, 두 patch 모드와 강제 재배치를 검증합니다. 각 세대는 LowIR와 LLVM을 새로
증명하며 이전 증명 기록만으로 재작성을 허용하지 않습니다. 생성된 ESI cleanup relay도 지역 프레임 기준점과 저장된 진입 EBP가 인증된 부모와 일치하면
두 번째 재구성을 지원합니다. 캐시된 바이트 증명은 각 호출의 부모 프레임에 다시 연결하며,
LowIR가 객체 접근과 저장된 프레임 수명을 증명합니다. 실제 MSVC 값/참조 검체는 두 세대,
두 patch 모드와 강제 재배치에서 소멸 순서와 참조 쓰기를 검증합니다. 일반적인 객체 수명과
형식 변환은 아직 지원하지 않습니다.

PE32의 단순 복사 예외 객체도 값·참조 catch, 직접 throw 및 재던지기를 지원합니다.
불변 ThrowInfo에는 단일 타입만 있어야 하며 주소 조정, 복사 생성자 또는 전달
콜백이 없어야 합니다. 모든 바이트의 초기화와 비공개 프레임 포인터의 비탈출을 증명합니다.
O0/O1의 8·12바이트 구조체로 필드 변경, catch 분기, 두 CLI 모드와 Microsoft CRT
재배치를 검사합니다. PHI 복사 블록은 원래 CFG 간선과 콜백에 속하며 독립 검증은 추가
호출, 별칭 메모리와 변경된 대상을 거부합니다. RTTI는 멤버 배치를 제공하지 않으므로
C++ 구문 검사는 실제 테스트 타입 선언을 사용하고 출력은 원래 오프셋과 알 수 없는
집합 타입의 throw 호출을 유지합니다.

선택적 소멸자는 별도 LowIR 증명이 필요합니다. 반환하는 리프 함수로서 비공개 스택을
사용하고 ECX 접근이 객체 범위 안에 있어야 합니다. 코드와 메모리 효과도 보존할 호출
계약에 포함됩니다. O0/O1 테스트는 재던지기와 두 차례 재구성에서 소멸 횟수, 순서,
객체 내용을 검사합니다.

비단순 런타임 복사 생성자도 독립 LowIR 증명을 통해 지원합니다. ECX의 대상과 스택
인자의 원본 포인터를 별도 주소 영역으로 추적합니다. 원본은 읽기 전용이며 범위 내 접근,
모든 반환 경로의 대상 전체 초기화와 `RET 4`를 요구합니다. 포인터 탈출, CRT 호출자
주소 관찰과 증명되지 않은 호출을 거부합니다. O0/O1 테스트는 두 세대에서 실행마다
네 번의 복사와 열두 번의 소멸을 확인합니다. 알 수 없는 집합 타입은 원래 throw 호출과
초기화를 유지하며 RTTI로 `throw Type()`을 추측하지 않습니다. 일반 생성자 호출 그래프,
타입 변환, 가상 기반 클래스 복사와 일반 소멸자 호출 그래프는 아직 지원하지 않습니다.

## IR 계약

예외 metadata는 일반 CFG 의미를 바꾸지 않고 모든 표현으로 전달됩니다.

- LowIR는 protected-range endpoint, state transition, filter, handler, cleanup action,
  continuation target에서 block을 분할.
- exceptional successor/predecessor를 일반 edge와 분리하여 dominator/structuring pass가 runtime
  dispatch edge를 machine branch로 오인하지 않게 함.
- MedIR는 정규화 function descriptor와 안정적 exceptional edge를 보존.
- HighIR는 별도 `SEHTry`/`CxxTry` statement 사용. clause descriptor는 native target VA,
  type descriptor, adjective, catch-object/parent-frame offset, cleanup action kind/object
  offset, state, continuation VA를 보존.

HighIR structurer는 interval-conservative합니다. 완전한 protected range 안에 모든 address가
있는 연속 statement slice만 이동하고 nested region은 안쪽부터 처리합니다. crossing region,
partial graph, address-less ambiguous boundary, out-of-line funclet은 원래 control flow에 남고
function의 unstructured-EH count를 증가시킵니다.

C backend는 reducible single-clause SEH region에 MSVC SEH syntax를 출력합니다. HighC는 C
backend이므로 C++ catch/cleanup state를 결정적 C-compatible comment로 출력하고 compilable
C++라고 주장하지 않습니다. out-of-line native funclet은 정확한 address를 보존합니다.

## LLVM metadata schema

emitted function과 연결된 모든 분석된 예외 function은 native WinEH lowering 대상이 아니어도
lossless LLVM metadata를 받습니다.

- function attachment: `neverd.windows.eh`
- native-lowering marker: `neverd.windows.eh.native`
- module table: `neverd.windows.eh.functions`
- current schema version: `3`

fixed record는 parse status, encoding, code range, native runtime/unwind RVA, runtime-record kind/
chain provenance, packed-unwind word, frame description, canonical/resolved personality name,
handler data, native unwind bytes, operation(native slot count 포함)/epilog, SEH scope, C++ header/
map, GS data, diagnostic, regeneration flag를 보존합니다. patch validation은 정확한 schema version과
loaded image에 대한 완전한 range match를 요구하며 예외 계약이 있는 auto-named lifted function은
attachment를 생략할 수 없습니다.

native x64 SEH lowering은 LLVM WinEH를 사용하고 전체 scope graph가 표현 가능할 때만
verifier-clean `invoke`/funclet control flow를 생성합니다. native FH3 조건은 다음과 같습니다.

- x64 COFF, unwind v1/v2, complete metadata, 유효한 synchronous FH3 state graph.
- `noexcept`, asynchronous, separated-funclet, GS-wrapper, FH4, unknown flag semantics 없음.
- protected interval은 nested 또는 disjoint이며 crossing하지 않음.
- destructor/unwind action, catch-object construction, parent-frame dependency 없음.
- handler는 lifted function 내부의 predecessor-free, call-free 일반 block.
- unwind 가능한 모든 protected operation은 LLVM `invoke`로 표현.

하나라도 맞지 않으면 LLVM은 lossless metadata와 함께 분석 가능하지만 patch planning은 native
language-table replacement를 거부합니다. PE entry point, TLS callback, CRT callback root는
preservation boundary이며 일반 ABI rewrite candidate가 아닙니다.

## Patch transaction

지원 rewrite는 하나의 PE transaction으로 처리합니다.

1. loaded exception graph와 LLVM metadata attachment에 대해 각 touched function 검증.
2. section identity/alignment/allocation, code/data trait, semantic symbol-index reference를 보존해
   replacement code compile. local Windows personality는 codegen 전에 externalize하여 emitted
   xdata를 입증된 original executable handler에 binding.
3. untouched runtime-function entry를 보존하고 touched primary function이 대체하는 native
   closure와 관련 chained record를 제거.
4. generated code/xdata를 relocate하고 generated/retained pdata를 merge/sort하며 overlap 거부.
   같은 personality class의 generated runtime-function record가 redirected language-EH entry를
   덮는지 증명한 뒤 단일 replacement exception directory 설치.
5. input CFG instrumentation mode 보존. `.gfids` reference와 redirected entry를 Guard CF table에,
   `.gehcont` reference를 generated executable VA로 해결해 Guard EH continuation table에 merge하고
   guard flag를 유지한 채 load-config 갱신. unresolved helper는 중단. CFW, return-flow guard,
   retpoline, XFG는 별도 codegen contract가 필요하므로 analysis-only.
6. 디스크 기록 전 완성 byte image를 다시 parse.

LLVM fork extension은 일반적입니다. final-image writer가 section trait와 semantic symbol-index
reference를 보존하며 PE/MSVC decode, policy, directory merge, load-config update, final validation은
NeverD에 남습니다.

original Guard CF/EH continuation entry는 original entry trampoline이 유효한 indirect target이므로
보존됩니다. generated target은 emitted code 안을 가리키고 결과 table은 strict RVA sort여야 합니다.

## 최종 image validation

다음 조건을 모두 만족하지 않는 patched PE는 거부합니다.

- LLVM이 bytes를 COFF object로 수용하고 PE machine/class/section table, optional-header directory
  bounds, image base/extent가 일치.
- section raw/virtual extent가 범위 내이고 section range overlap 없음.
- exception directory가 file-backed이며 image 내부.
- runtime-function entry가 sorted, nonempty, non-overlapping, fully executable.
- x64 unwind RVA/header/code array/version/flag/handler target이 유효하고 chain은 bounded/acyclic.
- final import/export/COFF symbol을 memory에서 재구성해 알려진 SEH/FH3 personality와 scope/state
  table을 완성 bytes에서 다시 parse.
- ARM runtime entry/xdata version/range가 유효하고 지원됨.
- guard flag가 table을 표시할 때 load-config에 Guard CF/EH continuation field 존재.
- guard pointer/count/stride가 PE image와 file 범위 안이며 entry가 strict-sorted executable target.

실패하면 patch를 중단하고 best-effort image를 기록하지 않습니다.

## 집중 검증

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

guarded x64 fixture는 `/guard:cf`와 `/guard:ehcont`로 cross-assemble/link합니다. 통합 테스트는
SEH scope/guard table, structured HighC, patched image reload, table count/order/executable target을
검증합니다.

별도 linked x64 FH3 fixture는 같은 transaction으로 지원 C++ closure를 검사하며 original fixed
table, HighC state annotation, personality binding, 재구성 try/catch graph, reload 후 IP-to-state
map을 검증합니다. parser 변경 시 공통 모델을 쓰는 ARM format case도 실행합니다.

## 네이티브 지원 확장

새 네이티브 재구성 지원에는 같은 변경 안에서 다음이 필요합니다.

- complete bounded parser와 normalized-model invariant.
- HighIR/LLVM metadata round-trip coverage.
- 새로 허용하는 모든 graph shape의 verifier-clean native IR.
- 필요한 emitted-section/semantic-reference retention.
- 정확한 architecture/personality/version의 linked PE fixture.
- exception-directory/load-config/final-image structural validation.
- 가장 가까운 unsupported shape의 explicit rejection test.

decode 가능하다는 이유만으로 allow-list를 넓히지 않습니다. 최종 linked image에서 runtime
exception behavior가 보존되는 것이 수용 기준입니다.

선택된 handler의 cleanup은 부모 프레임의 동일한 capture 또는 다른 유효 capture로 새 스칼라 값을 사용해 점프할 수 있습니다(0은 1로 정규화). 각 대상과 값을 프레임 효과와 함께 유지하며, 실제 대상의 저장된 레지스터, ESP 및 unwind 경계를 복원합니다. 중단된 중첩 finally 호출도 종료합니다. MedIR는 각 대체를 정확한 callback 호출 위치와 capture에 연결하고, 비순환 전이 그래프의 모든 경로가 완결되도록 요구합니다. 남은 cleanup 전체에서 두 버퍼의 유효성을 확인하며, 겹치는 쓰기, 만료된 버퍼, 증명되지 않은 순환과 프레임 주소 유출은 거부합니다.

중단된 finally의 RaiseException이 호출한 검색 filter도 동일한 capture나 다른 유효 capture로 점프하거나 다른 분기에서 반환할 수 있습니다. 반환과 점프의 프레임 효과를 분리하며, 점프는 filter와 중단된 모든 중첩 finally를 종료하고 자신의 대상과 값을 유지합니다.

finally 콜백과 검색 필터의 유한 루프는 레지스터 카운터, 부모 프레임 셀, 전용 스택을 포함한 전체 프레임을 실행 단계마다 보존합니다. 전체 상태의 반복, 종료 증명 부족, 예산 소진은 거부합니다. PHI 복사 블록은 정확한 원본 간선과 같은 콜백에 속해야 하며, 설치기는 복원된 스칼라 슬롯의 escape 인덱스와 모든 별칭의 비원자적 전용 접근을 검사합니다.

검증된 고정 프레임 SEH3/EH4 finally는 SavedESP를 복원하고 부모 함수에 속함이 확인된
계속 지점으로 직접 분기할 수 있습니다. 네이티브 생성은 전용 SjLj 컨텍스트로 생성 코드의
스택과 FS:[0]을 복원하며 현재 논리 레지스터와 callback의 프레임 쓰기를 유지한 채 정확한
원본 scope 경계로 진입합니다. 설치기는 별칭, 캡처, 전이, 분기를 독립 검증하며 원본 버퍼로
컨텍스트를 대체할 수 없습니다. 런타임 탈출이 CRT의 임시 unwind 노드를 건너뛰면 다음 호출이나
예외 가능 연산 전에 원본이 살아 있는 registration을 다시 연결하거나 반환 시 제거해야 합니다.
체인 복원을 증명하지 못하거나 목적지가 검증된 부모 CFG에 속하지 않으면 재구성을 거부합니다.

finally 분기에서만 도달하는 계속 지점도 처리합니다. 단, 비순환 꼬리 부분의 모든 디코딩된
경로가 filter 또는 finally 진입점을 통과하지 않고 이미 확인된 부모 코드에 합류해야 합니다.
이 물리적 소유권 증명은 런타임 handler의 메모리 접근 권한을 부여하지 않습니다. SavedESP
전달과 도달 상태는 별도로 검증합니다. 부모 코드에 연결되지 않은 반환이나 순환만으로는
부모 소유권을 얻을 수 없습니다.


일반 finally 호출은 부모 프레임의 전용 슬롯을 통해 EAX/ECX/EDX/EBX/ESI/EDI를
명시적으로 전달하며, 실제 near return 시점의 값도 돌려받습니다. 런타임 진입점은
자신의 레지스터 계약을 유지하고 이전 일반 호출의 입력을 재사용하지 않습니다.
검증된 일반 finally 호출만 참조하는 scope도 소스 try level에서 활성화되지 않더라도
유지됩니다.

실제 MSVC x86 `/Od` 및 `/O2`의 `nonlocal_finally_return` 재구성은 `/GS-`와 `/GS`, 최적화 전후 소스 LLVM, 두 패치 모드 및 두 강제 기준 주소로 검증합니다. 외부 SEH 관찰자는 정상·예외 반환, 스택 보호 값과 체인 복원을 확인합니다. GS 경로는 스칼라 도우미가 호출자 프레임을 관찰하지 못함을 증명한 뒤 원래 예외 코드 정규화 래퍼를 유지합니다.

`/O2`의 직접 부모 반환에는 인접한 실제 호출, 저장된 ESP 복원, 원래 체인 제거 및 전체 부모 에필로그 검증이 필요합니다. LowIR이 소스 효과를 증명하고 MedIR이 남은 연산을 연결하며 COFF 설치기가 계약을 독립적으로 재검증합니다. 사용되지 않는 합성 레지스터 전달은 부모의 실제 매개변수를 늘리지 않으며 관찰되는 값의 의존성은 유지합니다. 검증된 MSVC의 비지역 `goto` 형태는 아래의 전체 펼침 계약을 사용합니다.

검증된 `_local_unwind2` 및 `_local_unwind4` 호출은 정확한 소스 연산, 인증된 임포트 슬롯, 실제 부모 프레임 인수, 부호 없는 펼침 경계와 반환 상태 쌍을 보존합니다. EH4는 cookie 주소와 프레임 cookie 초기화도 증명해야 합니다. 임포트 저장소 변경, 조건부 호출 또는 소스 식별 불일치는 증명을 무효화합니다. MedIR는 스택 인수 두 개 또는 세 개를 정확히 복원하며 우연히 살아 있는 레지스터를 인수로 만들지 않습니다. 네이티브 대체는 실제 부모 프레임 브리지, SavedESP 복원 및 소스 연산을 독립적으로 연결한 뒤 EH3 `-1` 또는 EH4 `-2`까지의 전체 펼침을 지원합니다. 실제 MSVC `nonlocal_finally_goto`는 `/Od`, `/O2`, `/GS-`, `/GS`, 최적화 전후 LLVM, 두 패치 모드 및 두 강제 기준 주소에서 실행 검증합니다. 부분 펼침 경계와 활성 외부 scope 안으로의 재개에는 추가 네이티브 계약이 필요합니다.
