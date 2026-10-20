#!/usr/bin/env python3
"""Compare two builds on independently generated full-width MBA identities.

Keep each CLI beside its own shared SDK library. The benchmark pins library
lookup to that directory; copying only the executable would compare the same
engine twice. Timing includes parsing and proof, excludes process startup, and
is descriptive rather than a machine-dependent pass/fail threshold.
"""

import argparse
import ast
import hashlib
import itertools
import json
import os
import operator
from pathlib import Path
import platform
import random
import statistics
import subprocess
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def engine(path):
    path = Path(path).resolve()
    libraries = [
        p for p in path.parent.glob("*neverd*") if p.suffix in (".dylib", ".so", ".dll")
    ]
    if not libraries:
        raise ValueError(f"Put the matching shared SDK beside {path}")
    return {
        "path": str(path),
        "sha256": digest(path),
        "libraries": {p.name: digest(p) for p in libraries},
    }


def corpus(width, count):
    rng = random.Random(0x4D424100 + width)
    full = (1 << width) - 1
    rows = []
    for index in range(count):
        a, b, k, tail = (rng.getrandbits(width) for _ in range(4))
        k |= 1
        # Both complements and deliberately noncomplementary neighbours occur.
        opposite = full ^ a ^ (index % 4 == 3)
        right = "y" if index % 2 == 0 else f"(y^{b})"
        rows.append(f"({k}*((((x^{a})|{right})+(x^{opposite})))+{k})|{tail}")
    return rows


def weighted_corpus(width, count):
    rng = random.Random(0x4D424132 + width)
    full = (1 << width) - 1
    rows = []
    for index in range(count):
        carrier = ("(x^y)", "(x+y)", "(x*y)", "((x+y)^(x*y))")[(index // 6) % 4]
        size = (3, 4, 5, 4, 8, 16)[index % 6]
        if index % 6 < 3:
            masks = [rng.getrandbits(width) for _ in range(size)]
            terms = []
            positive, negative = [], []
            for subset in range(1, 1 << size):
                mask = full
                for bit in range(size):
                    if subset & (1 << bit):
                        mask &= masks[bit]
                # A nearby nonidentity must retain its changed high-bit term.
                if index % 4 == 3 and subset == 1:
                    mask ^= 1 << (width - 1)
                sign = "+" if subset.bit_count() % 2 else "-"
                terms.append(f"{sign}({carrier}&{mask})")
                (positive if sign == "+" else negative).append(f"({carrier}&{mask})")
            if index % 24 >= 12:
                # Compiler reassociation groups subtractions and hoists scales.
                grouped = f"({'+'.join(positive)})-({'+'.join(negative)})"
                rows.append(f"(3*({grouped}))+(x|y)")
                continue
        else:
            masks = [0] * size
            for bit in range(width):
                masks[rng.randrange(size)] |= 1 << bit
            scale = (37, -11, 182)[(index // 6) % 3]
            terms = [f"+({scale}*({carrier}&{mask}))" for mask in masks]
        rows.append("0" + "".join(terms))
    return rows


def bitwise_corpus(width, count):
    """Literal flips, forced bits and nested views of opaque arithmetic words."""
    rng = random.Random(0x4D424133 + width)
    full = (1 << width) - 1
    rows = []
    for index in range(count):
        carrier = ("(x^y)", "(x+y)", "(x*y)", "((x+y)^(x*y))")[(index // 6) % 4]
        kind = index % 6
        weight = (37, -11, 182, 1)[(index // 6) % 4]
        if kind in (0, 1, 4):
            masks = [0] * 16
            for bit in range(width):
                masks[rng.randrange(len(masks))] |= 1 << bit
            terms = []
            for mask in masks:
                key = rng.getrandbits(width)
                view = f"({carrier}^{key})"
                if kind == 4:
                    view = f"(~{view}|{rng.getrandbits(width)})"
                terms.append(f"({weight}*({view}&{mask}))")
                if kind == 1:
                    terms.extend((
                        f"({2*weight}*({carrier}&{key&mask}))",
                        str(-weight * (key & mask)),
                    ))
            rows.append("+".join(terms))
        else:
            positive, negative = [], []
            for part in range(8):
                key, forced = rng.getrandbits(width), rng.getrandbits(width)
                view = f"({carrier}^{key})"
                if kind == 3:
                    view = f"~{view}"
                    key ^= full
                compensation = key
                # A high-bit error must survive even if most terms collapse.
                if kind == 5 and part == 0:
                    compensation ^= 1 << (width - 2)
                positive.extend((
                    f"({view}|{forced})", f"({view}&{forced})",
                    f"(2*({carrier}&{compensation}))",
                ))
                negative.extend((str(key), str(forced)))
            rows.append(
                f"(-5*(({'+'.join(positive)})-({'+'.join(negative)})))+(x|y)"
            )
    return rows


def interleaved_corpus(width, count):
    """Several opaque words mixed inside shared scales and subtractions."""
    rng = random.Random(0x4D424134 + width)
    full = (1 << width) - 1
    families = (
        ("(x^y)", "(x*y)"),
        ("(x+y)", "(x*y)"),
        ("(x&y)", "(x^y)", "(x*y)"),
        ("(x+y)", "(x*y)", "((x+y)^(x*y))", "(x|y)"),
    )
    rows = []
    for index in range(count):
        carriers = families[(index // 6) % len(families)]
        pieces = (4, 8, 16)[(index % 6) // 2]
        if index % 2 == 0:
            groups = []
            for carrier in carriers:
                masks = [0] * pieces
                for bit in range(width):
                    masks[rng.randrange(pieces)] |= 1 << bit
                groups.append([
                    f"(({carrier}^{rng.getrandbits(width)})&{mask})"
                    for mask in masks
                ])
            rows.append("+".join(
                f"(37*({'+'.join(group[part] for group in groups)}))"
                for part in range(pieces)
            ))
        else:
            positive, negative = [], []
            for group, carrier in enumerate(carriers):
                masks = [rng.getrandbits(width) for _ in range(4)]
                for subset in range(1, 16):
                    mask = full
                    for bit in range(4):
                        if subset & (1 << bit):
                            mask &= masks[bit]
                    if index % 6 == 5 and group == 0 and subset == 1:
                        mask ^= 1 << (width - 1)
                    destination = positive if subset.bit_count() % 2 else negative
                    destination.append(f"({carrier}&{mask})")
            rows.append(
                f"(7*(({'+'.join(positive)})-({'+'.join(negative)})))+(x|y)"
            )
    return rows


def quadratic_corpus(width, count):
    """Expanded products with three to five inputs and unrelated partitions."""
    rng = random.Random(0x4D424135 + width)
    full = (1 << width) - 1
    families = (
        ("(x^y)", "(y|z)", ("z",), ("x",)),
        ("(x^y)", "(x|y)", ("z",), ("w",)),
        ("(x|z)", "(y^w)", ("w",), ("z",)),
        ("(x^y)", "(y|v)", ("z", "w"), ("x", "z")),
    )
    rows = []
    for index in range(count):
        left, right, axes_a, axes_b = families[(index // 6) % len(families)]
        kind = index % 6
        scale = (1, 7, -11, 182, 1 << (width - 1), 37)[kind]

        def partition(word, axes):
            return [
                "(" + "&".join(
                    [word] + [axis if mask & (1 << bit) else f"~{axis}"
                              for bit, axis in enumerate(axes)]
                ) + ")"
                for mask in range(1 << len(axes))
            ]

        terms = [f"({a}*{b})"
                 for a in partition(left, axes_a)
                 for b in partition(right, axes_b)]
        rng.shuffle(terms)
        expression = f"({scale}*({' + '.join(terms)}))"
        if kind in (1, 2, 3, 5):
            expression += "+(x^y)+2*(x&y)"
        if kind == 5:
            # A changed high bit remains an observable extra term.
            expression += f"+(x&{1 << (width - 1)})"
        if kind == 3:
            # A second, different degree must survive the quadratic recovery.
            expression += f"+({(rng.getrandbits(width) | 1) & full}*x*y*z)"
        rows.append(expression)
    return rows


def cubic_corpus(width, count):
    """Three Boolean factors, independently partitioned, with mixed degrees."""
    rng = random.Random(0x4D424136 + width)
    families = (
        ("(x^y)", "(y|z)", "(x|z)", ("z",), ("x",), ("y",)),
        ("(x^y)", "(x|y)", "(x|z)", ("z",), ("w",), ()),
        ("(x|z)", "(y^w)", "(x|w)", ("w",), ("z",), ()),
        ("(x^y)", "(y|v)", "(x|v)", ("z", "w"), ("x", "z"), ()),
    )
    rows = []
    for index in range(count):
        a, b, c, axes_a, axes_b, axes_c = families[(index // 6) % len(families)]
        kind = index % 6
        scale = (1, 7, -11, 182, 1 << (width - 2), 37)[kind]

        def partition(word, axes):
            return [
                "(" + "&".join(
                    [word] + [axis if mask & (1 << bit) else f"~{axis}"
                              for bit, axis in enumerate(axes)]
                ) + ")"
                for mask in range(1 << len(axes))
            ]

        terms = [f"({left}*{right}*{third})"
                 for left in partition(a, axes_a)
                 for right in partition(b, axes_b)
                 for third in partition(c, axes_c)]
        rng.shuffle(terms)
        expression = f"({scale}*({' + '.join(terms)}))"
        if kind in (1, 2, 3, 5):
            expression += "+(x^y)+2*(x&y)"
        if kind == 3:
            # Quadratic and cubic coefficients must both be retained.
            expression += f"+({rng.getrandbits(width) | 1}*x*y)"
        if kind == 5:
            # Nearby error survives every factor reconstruction.
            expression += f"+(x&{1 << (width - 1)})"
        rows.append(expression)
    return rows


def higher_corpus(width, count):
    """Fourth through eighth degrees, mixed products and nonlinear quotients."""
    rng = random.Random(0x4D424137 + width)
    words = ("(x^y)", "(y|z)", "(x|z)", "(x&y)", "(x^z)", "(y^z)",
             "(x|y)", "~(x&z)")
    axes = ("z", "x", "y", "w", "v", "z")
    rows = []
    for index in range(count):
        degree = (4, 5, 6, 8)[(index // 16) % 4]
        kind = index % 16
        scale = (1, 7, -11, 182, 1 << (width - 2), 37)[kind % 6]
        if 6 <= kind < 10:
            # Neither quadratic identity exposes a shared single factor.
            # Their distributed product requires a two-factor quotient.
            pairs = (("(x&y)", "(x|y)"), ("(x&~y)", "(~x&y)"))
            other = (("(z&w)", "(z|w)"), ("(z&~w)", "(~z&w)"))
            terms = ["(" + "*".join(a + b + words[4:degree]) + ")"
                     for a in pairs for b in other]
        else:
            pieces = (min(degree, kind - 9) if kind >= 14
                      else 2 + (kind % 6) // 2)
            choices = [
                (f"({word}&{axes[i]})", f"({word}&~{axes[i]})")
                if i < pieces else (word,)
                for i, word in enumerate(words[:degree])
            ]
            terms = ["(" + "*".join(fs) + ")"
                     for fs in itertools.product(*choices)]
        if kind == 9:
            # Alter one nonlinear coefficient; a partial identity must not
            # silently remove this difference.
            terms[0] = f"(3*{terms[0]})"
        rng.shuffle(terms)
        expression = f"({scale}*({' + '.join(terms)}))+(x^y)+2*(x&y)"
        if kind % 3 == 2:
            expression += "+13*x*y*z"
        if kind % 4 == 3:
            expression += f"+(x&{1 << (width - 1)})"
        if 10 <= kind < 14:
            factors = ("x", "y", "z", "(x|y)") + ("x",) * (degree - 4)
            if kind != 13:
                factors += ("y",)
            expression += "+19*" + "*".join(factors)
        rows.append(expression)
    return rows


def perturbed_corpus(width, count):
    """Sparse coefficient differences in fourth through twelfth degree sums."""
    rng = random.Random(0x4D424138 + width)
    words = ("(x^y)", "(y|z)", "(x|z)", "(x&y)", "(x^z)", "(y^z)",
             "(x|y)", "~(x&z)")
    axes = ("z", "x", "y", "w", "v")
    rows = []
    for index in range(count):
        degree = (4, 6, 8, 12)[(index // 16) % 4]
        kind = index % 16
        scale = (1, 37, -11, 182, 1 << (width - 2), -7)[kind % 6]
        delta = (2 * scale, -scale, 1 << (width - 1), 13)[kind % 4]
        if kind < 8:
            pairs = (("(x&y)", "(x|y)"), ("(x&~y)", "(~x&y)"))
            other = (("(z&w)", "(z|w)"), ("(z&~w)", "(~z&w)"))
            tail = tuple(words[i % len(words)] for i in range(4, degree))
            factors = [a + b + tail for a in pairs for b in other]
        else:
            pieces = 3 + (kind % 3)
            whole = tuple(words[i % len(words)] for i in range(degree))
            choices = [
                (f"({word}&{axes[i]})", f"({word}&~{axes[i]})")
                if i < pieces else (word,)
                for i, word in enumerate(whole)
            ]
            factors = list(itertools.product(*choices))
        # Some coefficients wrap to zero or change sign. Additional independent
        # differences ensure that recovering a nearby identity is insufficient.
        changed = {0: delta}
        if kind % 4 == 3:
            changed[len(factors) - 1] = -3 * delta
        terms = [f"({scale + changed.get(i, 0)}*{'*'.join(fs)})"
                 for i, fs in enumerate(factors)]
        rng.shuffle(terms)
        expression = "+".join(terms) + "+(x^y)+2*(x&y)"
        if kind % 3 == 2:
            expression += "+19*x*y*z+(x&" + str(1 << (width - 1)) + ")"
        if kind % 4 == 3:
            expression += "+23*" + "*".join(("x", "y") * (degree // 2))
        rows.append(expression)
    return rows


def check_semantics(expressions, results, width):
    """Independent modular evaluator; never execute parsed source as Python."""
    mask = (1 << width) - 1
    binary = {
        ast.Add: operator.add,
        ast.Sub: operator.sub,
        ast.Mult: operator.mul,
        ast.BitAnd: operator.and_,
        ast.BitOr: operator.or_,
        ast.BitXor: operator.xor,
    }
    unary = {ast.USub: operator.neg, ast.UAdd: operator.pos, ast.Invert: operator.invert}

    def evaluate(node, values):
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return node.value & mask
        if isinstance(node, ast.Name) and node.id in values:
            return values[node.id]
        if isinstance(node, ast.BinOp) and type(node.op) in binary:
            return binary[type(node.op)](
                evaluate(node.left, values), evaluate(node.right, values)
            ) & mask
        if isinstance(node, ast.UnaryOp) and type(node.op) in unary:
            return unary[type(node.op)](evaluate(node.operand, values)) & mask
        raise ValueError(f"Unsupported benchmark expression: {ast.dump(node)}")

    rng = random.Random(0x53454D00 + width)
    edges = (0, 1, mask, mask >> 1, 1 << (width - 1), mask // 3)
    names = sorted({
        node.id for expression in expressions
        for node in ast.walk(ast.parse(expression, mode="eval"))
        if isinstance(node, ast.Name)
    })
    assignments = [
        {name: (x if index % 2 == 0 else y)
         for index, name in enumerate(names)}
        for x in edges for y in edges
    ]
    for name in names:
        for base in (0, mask):
            for edge in edges:
                assignments.append({**dict.fromkeys(names, base), name: edge})
    assignments += [
        {name: rng.getrandbits(width) for name in names}
        for _ in range(128)
    ]
    if len(expressions) != len(results):
        raise AssertionError("Engine returned an incomplete corpus")
    for expression, result in zip(expressions, results):
        before = ast.parse(expression, mode="eval").body
        after = ast.parse(result["output"], mode="eval").body
        for values in assignments:
            if evaluate(before, values) != evaluate(after, values):
                raise AssertionError((width, expression, result["output"], values))


def run(engine_info, corpus_path, width):
    executable = engine_info["path"]
    environment = os.environ.copy()
    variable = (
        "DYLD_LIBRARY_PATH" if platform.system() == "Darwin" else "LD_LIBRARY_PATH"
    )
    environment[variable] = str(Path(executable).parent)
    command = [
        executable,
        "simplify",
        "--json",
        "--stats",
        f"--width={width}",
        "--max-work=65536",
        "-f",
        str(corpus_path),
    ]
    result = subprocess.run(
        command, env=environment, capture_output=True, text=True, timeout=120
    )
    # A bounded search may legitimately decline. Malformed queries may not.
    report = json.loads(result.stdout)
    if any(not row["ok"] for row in report["results"]):
        raise RuntimeError(result.stderr or result.stdout)
    return {"returncode": result.returncode, **report}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=7)
    parser.add_argument("--count", type=int, default=32)
    parser.add_argument(
        "--family",
        choices=("complement", "weighted", "bitwise", "interleaved", "quadratic",
                 "cubic", "higher", "perturbed"),
        default="complement",
    )
    args = parser.parse_args()
    if args.rounds < 1 or args.count < 1:
        parser.error("rounds and count must be positive")
    engines = {"baseline": engine(args.baseline), "candidate": engine(args.candidate)}
    report = {
        "schemaVersion": 1,
        "platform": platform.platform(),
        "engines": engines,
        "rounds": args.rounds,
        "family": args.family,
        "widths": [],
    }
    with tempfile.TemporaryDirectory(prefix="neverd-mask-bench-") as directory:
        path = Path(directory) / "expressions.txt"
        for width in (8, 16, 32, 64):
            generate = {
                "complement": corpus, "weighted": weighted_corpus,
                "bitwise": bitwise_corpus, "interleaved": interleaved_corpus,
                "quadratic": quadratic_corpus,
                "cubic": cubic_corpus,
                "higher": higher_corpus,
                "perturbed": perturbed_corpus,
            }[args.family]
            expressions = generate(width, args.count)
            path.write_text("\n".join(expressions) + "\n", encoding="utf-8")
            row = {
                "width": width,
                "corpusSha256": digest(path),
                "expressions": expressions,
                "runs": {name: [] for name in engines},
            }
            # Warm both processes, then alternate order to reduce thermal/load bias.
            for info in engines.values():
                warm = run(info, path, width)
                check_semantics(expressions, warm["results"], width)
            for iteration in range(args.rounds):
                for name in (
                    list(engines) if iteration % 2 == 0 else list(reversed(engines))
                ):
                    row["runs"][name].append(run(engines[name], path, width))
            row["medians"] = {
                name: {
                    key: statistics.median(r["summary"][key] for r in runs)
                    for key in (
                        "p50Micros",
                        "p95Micros",
                        "work",
                        "costAfter",
                        "simplified",
                    )
                }
                for name, runs in row["runs"].items()
            }
            report["widths"].append(row)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(
                json.dumps(report, indent=2) + "\n", encoding="utf-8"
            )
            print(width, json.dumps(row["medians"]), flush=True)


if __name__ == "__main__":
    main()
