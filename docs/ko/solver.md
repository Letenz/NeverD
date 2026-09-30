**언어**: [English](../solver.md) | [简体中文](../zh-CN/solver.md) | [繁體中文](../zh-TW/solver.md) | [日本語](../ja/solver.md) | [한국어](solver.md) | [Français](../fr/solver.md) | [Deutsch](../de/solver.md) | [Español](../es/solver.md) | [Italiano](../it/solver.md) | [Русский](../ru/solver.md) | [العربية](../ar/solver.md)

[← 문서 색인](README.md)

# 비트벡터 증명 백엔드

NeverD는 기본적으로 내장 비트벡터 solver를 사용합니다. 정확한 MBA 유도는 일반 solver와 독립적입니다. 표현식 합성은 동등성 증명 후에만 후보를 채택하며 반례나 미확정 질의는 원래 표현식을 유지합니다.

## 선택적 Z3 빌드

Z3를 활성화하면 CMake `FetchContent`가 고정된 4.13.3 소스 revision을 내려받아 NeverD와 정적 라이브러리로 빌드합니다. 시스템 Z3 설치는 필요하지 않습니다.

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_Z3=ON -DNEVERD_BUILD_SOLVER_BENCH=ON
cmake --build build-release --target neverd NeverDSolverTests \
  NeverDSymbolicTests neverd-solver-bench --parallel 4
```

기본 provider는 `NEVERD_Z3_PROVIDER=FETCH`입니다. 첫 configure에서 소스를 받고 이후에는 build 디렉터리의 `_deps`를 재사용합니다. Z3 CLI, 테스트, 예제, 문서, 언어 바인딩은 빌드하지 않습니다. Z3 Python generator는 기존 NeverD Python interpreter를 사용합니다. 설치된 라이브러리를 쓰려면 `-DNEVERD_Z3_PROVIDER=SYSTEM`, 필요하면 `-DZ3_ROOT=...`를 지정하세요. 개발 파일이 없으면 실패하며 provider를 조용히 바꾸지 않습니다. 오프라인 빌드는 `-DFETCHCONTENT_SOURCE_DIR_NEVERD_Z3=...`로 로컬 checkout을 제공할 수 있습니다.

기본 `NEVERD_ENABLE_Z3=OFF`에서는 Z3를 내려받거나 검색하거나 링크하지 않습니다. 사용할 수 없는 backend를 명시적으로 요청하면 대체 없이 실패합니다.

## 증명 게이트 표현식 합성

```sh
build-release/bin/neverd simplify --synthesize --solver=z3 \
  --solver-timeout-ms=1000 --json '(x >> 4) + ((x >> 2) >> 2)'
```

기본값은 `--solver=builtin`입니다. solver 선택에는 `--synthesize`가 필요하며 일반 MBA simplifier는 Z3를 호출하지 않습니다. Z3 timeout은 표현식 변환을 제외한 각 검사에 적용됩니다. 취소는 협력적이므로 엄격한 wall-clock 제한이 아닙니다. 0은 기본 1000 ms를 선택하고 `--exhaustive`는 제한을 제거합니다. SAT 충돌/전파/watch 방문 한도는 내장 backend에만 적용되며 Z3와 함께 지정하면 거부됩니다. Z3 검사는 proof-query 카운터를 증가시키고 내장 SAT 작업 카운터는 0으로 남습니다.

C API는 `neverd_synthesize_options`에 `solver_backend`, `solver_timeout_ms`를 추가합니다. 크기 제한 reader는 이전 caller를 위해 내장 backend를 유지합니다. `neverd_solver_backend_available()`은 빌드 지원 여부를 보고하고 Python도 `synthesize_expression(..., solver='z3', solver_timeout_ms=1000)`를 지원합니다. 현재 선택은 표현식 합성에만 적용됩니다. concolic 실행, 안전성 분석, 기존 IR 최적화 기본 solver 정책은 유지됩니다. 내부 사용자는 의미 simplifier의 증명 callback에 Z3 verifier를 전달할 수 있습니다.

## 독립 검사와 query 내보내기

Z3 활성화 시 `NeverDSolverTests`는 독립 표현식 평가와 backend 교차 검사를 포함합니다. 기준 표현식을 직접 구성하므로 NeverD 표현식 builder의 버그가 양쪽 테스트를 함께 단순화하지 못합니다. 비활성 빌드에서는 oracle 사례를 명시적으로 건너뛰지만 backend 비가용 계약은 계속 검사합니다.

```sh
build-release/bin/NeverDSolverTests --gtest_brief=1
build-release/bin/neverd-solver-bench tools/neverd-bench/solver-corpus.txt \
  --width=32 --repeat=5 --backend=both --timeout-ms=1000 \
  --max-conflicts=10000 --dump-dir=/tmp/neverd-queries
```

주석이 아닌 각 행은 `original ; candidate`입니다. 세미콜론이 없으면 MBA simplifier 결과를 후보로 씁니다. 도구는 verdict, model replay, 시간을 보고합니다(세션 생성·변환·해결 포함, 파싱·MBA 단순화·내보내기·정리 제외). 반복마다 새 solver를 사용합니다. SAT model은 표현식 evaluator에서 차이를 재현해야 합니다. 서로 반대인 확정 verdict나 잘못된 질의/model은 실행 실패이고, `unknown`은 기록되지만 동등성 증명이 아닙니다.

내보낸 SMT-LIB에는 원래 DAG, 영구 assertion, 마지막 query 가정이 들어 있으며 `z3 query-N.smt2`로 재생할 수 있습니다. 자원 제한과 solver 버전을 기록하세요. 두 backend의 예산 단위가 다르므로 동일 작업 비교가 아닌 제한된 workload 비교입니다. 비트벡터 증명은 표현식 언어의 total fixed-width 의미론을 사용합니다. 기계 예외, 메모리 효과, LLVM poison은 lifting/translation 경계의 책임이며 표현식 증명으로 인증되지 않습니다.
