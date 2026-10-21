#!/usr/bin/env python3
"""Generate Python request bounds from their owning native definitions."""

from __future__ import annotations

import argparse
import ast
import operator
from pathlib import Path
import re
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = Path("pluginsdk/python/neverd_plugin/_limits.py")
# Native definitions remain the authority. Bind only contracts Python checks;
# equal numeric values do not make independent request limits interchangeable.
BINDINGS = (
    ("include/neverd/sdk/Limits.h", "kMaxSourcePatchInputBytes", "MAX_SOURCE_PATCH_INPUT_BYTES"),
    ("include/neverd/sdk/Limits.h", "kMaxSourcePatchMappings", "MAX_SOURCE_PATCH_MAPPINGS"),
    ("include/neverd/sdk/Limits.h", "kMaxSourcePatchCompilerArguments", "MAX_SOURCE_PATCH_COMPILER_ARGUMENTS"),
    ("include/neverd/sdk/Limits.h", "kMaxSourcePatchBindings", "MAX_SOURCE_PATCH_BINDINGS"),
    ("include/neverd/backend/Limits.h", "kMaxSourcePatchDependencies", "MAX_SOURCE_PATCH_DEPENDENCIES"),
    ("include/neverd/sdk/Limits.h", "kMaxSourcePatchTimeoutSeconds", "MAX_SOURCE_PATCH_TIMEOUT_SECONDS"),
    ("include/neverd/sdk/Limits.h", "kDefaultSourcePatchTimeoutSeconds", "DEFAULT_SOURCE_PATCH_TIMEOUT_SECONDS"),
    ("include/neverd/pipeline/Limits.h", "BytecodeRecoveryInputLimit", "MAX_BYTECODE_RECOVERY_INPUT_BYTES"),
    ("include/neverd/sdk/NeverDCAPISymbolic.h", "NEVERD_LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1", "LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1"),
    ("include/neverd/sdk/Limits.h", "kDefaultSymbolicWidth", "DEFAULT_SYMBOLIC_WIDTH"),
    ("include/neverd/pass/Limits.h", "kDefaultInstructionSubstitutionRounds", "DEFAULT_INSTRUCTION_SUBSTITUTION_ROUNDS"),
    ("include/neverd/symbolic/SymExplore.h", "ExploreOptions::MaxPaths", "DEFAULT_EXPLORE_MAX_PATHS"),
    ("include/neverd/symbolic/SymExplore.h", "ExploreOptions::MaxSteps", "DEFAULT_EXPLORE_MAX_STEPS"),
    ("include/neverd/symbolic/SymExplore.h", "ExploreOptions::MaxBlockVisits", "DEFAULT_EXPLORE_MAX_BLOCK_VISITS"),
    ("include/neverd/concolic/LowIRConcolic.h", "LowIRConcolicOptions::MaxSteps", "DEFAULT_CONCOLIC_MAX_STEPS"),
    ("include/neverd/concolic/LowIRConcolic.h", "LowIRConcolicOptions::MaxBlockVisits", "DEFAULT_CONCOLIC_MAX_BLOCK_VISITS"),
    ("include/neverd/concolic/LowIRConcolic.h", "LowIRConcolicOptions::MaxLoopIterations", "DEFAULT_CONCOLIC_MAX_LOOP_ITERATIONS"),
    ("include/neverd/concolic/LowIRConcolic.h", "LowIRConcolicOptions::MaxFlipAttempts", "DEFAULT_CONCOLIC_MAX_FLIP_ATTEMPTS"),
    ("include/neverd/concolic/LowIRConcolic.h", "LowIRConcolicOptions::MaxCandidates", "DEFAULT_CONCOLIC_MAX_CANDIDATES"),
    ("include/neverd/concolic/Limits.h", "kLowIRConcolicDefaultLowIRConcolicSolverOptionsMaxConflicts", "DEFAULT_CONCOLIC_MAX_CONFLICTS"),
    ("include/neverd/concolic/Limits.h", "kLowIRConcolicDefaultLowIRConcolicSolverOptionsMaxPropagations", "DEFAULT_CONCOLIC_MAX_PROPAGATIONS"),
    ("include/neverd/concolic/Limits.h", "kLowIRConcolicDefaultLowIRConcolicSolverOptionsMaxWatchVisits", "DEFAULT_CONCOLIC_MAX_WATCH_VISITS"),
    ("include/neverd/solver/BitBlaster.h", "BlastLimits::MaxWidth", "DEFAULT_CONCOLIC_SOLVER_MAX_WIDTH"),
    ("include/neverd/solver/BitBlaster.h", "BlastLimits::MaxGates", "DEFAULT_CONCOLIC_SOLVER_MAX_GATES"),
)
_OPERATORS = {
    ast.Add: operator.add,
    ast.Sub: operator.sub,
    ast.Mult: operator.mul,
    ast.LShift: operator.lshift,
    ast.RShift: operator.rshift,
    ast.BitOr: operator.or_,
    ast.BitAnd: operator.and_,
}


def integer_expression(expression: str) -> int:
    """Accept scalar integer arithmetic, failing on an unaudited expression."""
    expression = re.sub(r"(?<=\d)'(?=[\da-fA-F])", "", expression)
    expression = re.sub(
        r"\b(?:std::)?(?:u?int(?:8|16|32|64)_t|size_t)\s*"
        r"(?:\{([^{}]+)\}|\(([^()]+)\))",
        lambda match: "(" + (match[1] or match[2]) + ")",
        expression,
    )
    expression = re.sub(r"\b(0[xX][\da-fA-F]+|0[bB][01]+|\d+)[uUlL]+\b", r"\1", expression)
    tree = ast.parse(expression.strip(), mode="eval")

    def value(node: ast.AST) -> int:
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return node.value
        if isinstance(node, ast.BinOp) and type(node.op) in _OPERATORS:
            return _OPERATORS[type(node.op)](value(node.left), value(node.right))
        raise ValueError(f"unsupported native policy expression: {expression!r}")

    result = value(tree.body)
    if result <= 0:
        raise ValueError(f"request bound must be positive: {expression!r}")
    return result


def scoped_definition(source: str, native: str) -> tuple[str, str]:
    """Read an explicitly selected options model, including nested defaults."""
    if "::" not in native:
        return source, native
    scope, field = native.split("::")
    match = re.search(rf"\bstruct\s+{re.escape(scope)}\s*\{{", source)
    if match is None:
        raise ValueError(f"missing options model: {scope}")
    start = match.end()
    depth = 1
    for position in range(start, len(source)):
        depth += (source[position] == "{") - (source[position] == "}")
        if depth == 0:
            return source[start:position], field
    raise ValueError(f"unterminated options model: {scope}")


def render(root: Path = ROOT) -> str:
    lines = [
        '"""Generated by scripts/generate_python_plugin_limits.py; do not edit."""',
        "",
        "from typing import Final",
        "",
    ]
    for path, native, python in BINDINGS:
        source = (root / path).read_text(encoding="utf-8")
        source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.DOTALL)
        source, identifier = scoped_definition(source, native)
        patterns = (
            rf"(?<![\w:.>])\b{re.escape(identifier)}\s*=\s*([^;]+);",
            rf"^\s*#\s*define\s+{re.escape(identifier)}\s+([^\n]+)$",
        )
        matches = [match for pattern in patterns for match in re.findall(pattern, source, re.MULTILINE)]
        if len(matches) != 1:
            raise ValueError(f"expected one definition of {native} in {path}")
        lines += [f"# {path}: {native}", f"{python}: Final[int] = {integer_expression(matches[0])}", ""]
    return "\n".join(lines)


def check(root: Path = ROOT) -> list[str]:
    try:
        expected = render(root)
        actual = (root / OUTPUT).read_text(encoding="utf-8")
    except (OSError, SyntaxError, ValueError) as error:
        return [f"cannot check generated Python limits: {error}"]
    if actual != expected:
        return [f"{OUTPUT} is stale; run python3 scripts/generate_python_plugin_limits.py"]
    return []


def main(arguments: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    options = parser.parse_args(arguments)
    if options.check:
        errors = check()
        for error in errors:
            print(error)
        return bool(errors)
    (ROOT / OUTPUT).write_text(render(), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
