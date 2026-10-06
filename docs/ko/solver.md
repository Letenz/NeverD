**언어**: [English](../solver.md) | [简体中文](../zh-CN/solver.md) | [繁體中文](../zh-TW/solver.md) | [日本語](../ja/solver.md) | [한국어](solver.md) | [Français](../fr/solver.md) | [Deutsch](../de/solver.md) | [Español](../es/solver.md) | [Italiano](../it/solver.md) | [Русский](../ru/solver.md) | [العربية](../ar/solver.md)

[← 문서 색인](README.md)

# 비트벡터 증명 백엔드

NeverD는 기본적으로 내장 비트벡터 solver를 사용합니다. 정확한 MBA 유도는 일반 solver와 독립적입니다. 표현식 합성은 동등성 증명 후에만 후보를 채택하며 반례나 미확정 질의는 원래 표현식을 유지합니다.

<!-- i18n-section: builtin-comparisons -->

## 내장 비교 회로

8비트를 넘는 비교는 상위 절반을 먼저 비교하고, 상위 절반이 같으면 하위 절반의 결과를 사용합니다. 작은 조각은 뺄셈 캐리를 사용합니다. 부분 레지스터 갱신이 하위 비트만 바꾸면 공유 인코더가 상위 접두부의 게이트를 재사용할 수 있습니다. 부호 있는 비교는 두 부호 비트를 계속 반전합니다. 표현식 의미와 기존 자원 제한은 그대로이며, 예산이 소진되면 `Unknown`을 반환합니다.

`BitBlaster.WidePredicatesAgreeWithTheEvaluator`는 8~256비트 중 선택한 폭에서 부호 있는 조건과 부호 없는 조건을 표현식 평가기와 비교합니다. 홀수 폭, 경계의 인접 값, 64비트 이상의 유효 비트를 포함하며 잘못된 출력 값도 배제합니다. 부분 카운터 테스트는 전체 질의, 반례 모델, 게이트 예산 소진을 검사합니다. 비교 결과를 저장하는 네이티브 루프 회귀 테스트는 모든 레지스터와 플래그 관측을 유지하며 비교와 갱신의 두 순서를 증명하고, 변경된 원본 루프 본문을 거부합니다.

<!-- i18n-section: pristine-encoding -->

## 탐색 전 인코딩 복사

`BitVectorSolver::cloneEncoding()`은 SAT 탐색을 시도하지 않은 완전한 인코딩을 복사합니다. 탐색 후 또는 인코딩 실패 시 null을 반환합니다. 복사본은 변경 가능한 절, 루트 전파, 게이트와 비트 매핑을 독립적으로 소유하며 변수 순서, 게이트 계산과 솔버 설정을 유지합니다. 컨텍스트는 두 솔버보다 오래 유지되어야 하며 원본 솔버는 독립적으로 수정하거나 제거할 수 있습니다.

<!-- i18n-section: z3-build -->

## 선택적 Z3 빌드

Z3를 활성화하면 CMake `FetchContent`가 고정된 4.13.3 소스 revision을 내려받아 NeverD와 정적 라이브러리로 빌드합니다. 시스템 Z3 설치는 필요하지 않습니다.

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_Z3=ON -DNEVERD_BUILD_SOLVER_BENCH=ON
cmake --build build-release --target neverd NeverDSolverTests \
  NeverDSymbolicTests neverd-solver-bench --parallel 4
```

기본 provider는 `NEVERD_Z3_PROVIDER=FETCH`입니다. 첫 configure에서 소스를 받고 이후에는 build 디렉터리의 `_deps`를 재사용합니다. Z3 CLI, 테스트, 예제, 문서, 언어 바인딩은 빌드하지 않습니다. Z3 Python generator는 기존 NeverD Python interpreter를 사용합니다. 설치된 라이브러리를 쓰려면 `-DNEVERD_Z3_PROVIDER=SYSTEM`, 필요하면 `-DZ3_ROOT=/path/to/prefix`를 지정하세요. 개발 파일이 없으면 실패하며 provider를 조용히 바꾸지 않습니다. 오프라인 빌드는 `-DFETCHCONTENT_SOURCE_DIR_NEVERD_Z3=/path/to/z3`로 로컬 checkout을 제공할 수 있습니다.

기본 `NEVERD_ENABLE_Z3=OFF`에서는 Z3를 내려받거나 검색하거나 링크하지 않습니다. 사용할 수 없는 backend를 명시적으로 요청하면 대체 없이 실패합니다.

<!-- i18n-section: synthesis -->

## 증명 게이트 표현식 합성

```sh
build-release/bin/neverd simplify --synthesize --solver=z3 \
  --solver-timeout-ms=1000 --json '(x >> 4) + ((x >> 2) >> 2)'
```

기본값은 `--solver=builtin`입니다. solver 선택에는 `--synthesize`가 필요하며 일반 MBA simplifier는 Z3를 호출하지 않습니다. Z3 timeout은 표현식 변환을 제외한 각 검사에 적용됩니다. 취소는 협력적이므로 엄격한 wall-clock 제한이 아닙니다. 0은 기본 1000 ms를 선택하고 `--exhaustive`는 제한을 제거합니다. SAT 충돌/전파/watch 방문 한도는 내장 backend에만 적용되며 Z3와 함께 지정하면 거부됩니다. Z3 검사는 proof-query 카운터를 증가시키고 내장 SAT 작업 카운터는 0으로 남습니다.

C API는 `neverd_synthesize_options`에 `solver_backend`, `solver_timeout_ms`를 추가합니다. 크기 제한 reader는 이전 caller를 위해 내장 backend를 유지합니다. `neverd_solver_backend_available()`은 빌드 지원 여부를 보고하고 Python도 `synthesize_expression(..., solver='z3', solver_timeout_ms=1000)`를 지원합니다. 현재 선택은 표현식 합성에만 적용됩니다. concolic 실행, 안전성 분석, 기존 IR 최적화 기본 solver 정책은 유지됩니다. 내부 사용자는 의미 simplifier의 증명 callback에 Z3 verifier를 전달할 수 있습니다.

```python
from neverd_plugin import synthesize_expression

result = synthesize_expression(
    '(x >> 4) + ((x >> 2) >> 2)', solver='z3', solver_timeout_ms=1000
)
```

<!-- i18n-section: checks -->

## 독립 검사와 query 내보내기

Z3 활성화 시 `NeverDSolverTests`는 독립 표현식 평가와 backend 교차 검사를 포함합니다. 기준 표현식을 직접 구성하므로 NeverD 표현식 builder의 버그가 양쪽 테스트를 함께 단순화하지 못합니다. 비활성 빌드에서는 oracle 사례를 명시적으로 건너뛰지만 backend 비가용 계약은 계속 검사합니다.

```sh
build-release/bin/NeverDSolverTests --gtest_brief=1
build-release/bin/neverd-solver-bench \
  tools/neverd-bench/solver-corpus.txt --width=32 --repeat=5 \
  --backend=both --timeout-ms=1000 --max-conflicts=10000 \
  --dump-dir=/tmp/neverd-queries > /tmp/neverd-solver-results.json
```

주석이 아닌 각 행은 `original ; candidate`입니다. 세미콜론이 없으면 MBA simplifier 결과를 후보로 씁니다. 도구는 verdict, model replay, 시간을 보고합니다(세션 생성·변환·해결 포함, 파싱·MBA 단순화·내보내기·정리 제외). 반복마다 새 solver를 사용합니다. SAT model은 표현식 evaluator에서 차이를 재현해야 합니다. 서로 반대인 확정 verdict나 잘못된 질의/model은 실행 실패이고, `unknown`은 기록되지만 동등성 증명이 아닙니다.

내보낸 SMT-LIB에는 원래 DAG, 영구 assertion, 마지막 query 가정이 들어 있으며 `z3 query-N.smt2`로 재생할 수 있습니다. 자원 제한과 solver 버전을 기록하세요. 두 backend의 예산 단위가 다르므로 동일 작업 비교가 아닌 제한된 workload 비교입니다. 비트벡터 증명은 표현식 언어의 total fixed-width 의미론을 사용합니다. 기계 예외, 메모리 효과, LLVM poison은 lifting/translation 경계의 책임이며 표현식 증명으로 인증되지 않습니다.
