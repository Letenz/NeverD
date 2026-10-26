#!/usr/bin/env python3
"""Generate owned native matching diagnostics and evaluate retained tool results.

These small fixtures are development diagnostics, not held-out quality or
competitive performance qualification. Linker-map truth is never a tool input.
"""
from __future__ import annotations

import argparse
from contextlib import closing
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess
import sys
import time


CORPUS_SCHEMA = "neverd.benchmark.diff.corpus.v1"
RUN_SCHEMA = "neverd.benchmark.diff.run.v1"
RESULT_SCHEMA = "neverd.benchmark.diff.result.v1"
DEFAULT_TIMEOUT_SECONDS = 180.0
HASH_BLOCK_BYTES = 1024 * 1024
ADDRESS_BITS = 64


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def digest(path: Path, algorithm: str = "sha256") -> str:
    value = hashlib.new(algorithm)
    with path.open("rb") as source:
        for block in iter(lambda: source.read(HASH_BLOCK_BYTES), b""):
            value.update(block)
    return value.hexdigest()


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def save(path: Path, value: dict) -> None:
    with path.open("x", encoding="utf-8") as output:
        json.dump(value, output, indent=2, sort_keys=True)
        output.write("\n")


def artifact(path: Path, root: Path) -> dict:
    return {"path": os.path.relpath(path, root), "sha256": digest(path)}


def checked_artifact(record: dict, root: Path) -> Path:
    path = (root / record["path"]).resolve()
    require(digest(path) == record["sha256"], f"SHA-256 mismatch: {path}")
    return path


def address(value: object, base: int = 0) -> int:
    require(type(value) is int or isinstance(value, str), "address must be an integer or string")
    number = value if type(value) is int else int(value, base)
    require(0 <= number < 1 << ADDRESS_BITS, "address is outside uint64")
    return number


def truth_pairs(rows: list[dict]) -> set[tuple[int, int]]:
    """Admit complete one-to-one truth, including added and removed functions."""
    require(bool(rows), "ground truth must not be empty")
    ids, primary, secondary, pairs = set(), set(), set(), set()
    for row in rows:
        identity = row["id"]
        require(isinstance(identity, str) and identity and identity not in ids,
                "ground-truth identities must be unique and nonempty")
        ids.add(identity)
        left = None if row["primary"] is None else address(row["primary"])
        right = None if row["secondary"] is None else address(row["secondary"])
        require(left is not None or right is not None, "ground-truth row has no function")
        for value, seen in ((left, primary), (right, secondary)):
            if value is not None:
                require(value not in seen, "ground-truth address is assigned more than once")
                seen.add(value)
        if left is not None and right is not None:
            pairs.add((left, right))
    return pairs


def score(expected: set[tuple[int, int]], observed: list[tuple[int, int]]) -> dict:
    actual = set(observed)
    correct, wrong, missed = actual & expected, actual - expected, expected - actual
    encode = lambda pairs: [[hex(a), hex(b)] for a, b in sorted(pairs)]
    return {
        "expected_pairs": len(expected), "accepted_pairs": len(actual),
        "duplicate_evidence_rows": len(observed) - len(actual),
        "correct": len(correct), "wrong": len(wrong), "missed": len(missed),
        "precision": len(correct) / len(actual) if actual else None,
        "recall": len(correct) / len(expected) if expected else None,
        "wrong_pairs": encode(wrong), "missed_pairs": encode(missed),
        "exact_mapping": not wrong and not missed,
    }


def read_pairs(kind: str, path: Path, inputs: list[Path]) -> list[tuple[int, int]]:
    """Read selected function associations, never candidate or data matches."""
    if kind == "neverd":
        report = load(path)
        require(report["schema"] == "neverd.bindiff" and report["schema_version"] == 3,
                "unsupported NeverD report schema")
        require(report["state"] == "complete", "NeverD comparison did not complete")
        for side, source in zip(("primary", "secondary"), inputs):
            require(report[side]["sha256"] == digest(source),
                    f"NeverD {side} input identity does not match the corpus")
        return [(address(row["primary"]), address(row["secondary"]))
                for row in report["matches"]]
    if kind == "ghidra":
        report = load(path)
        require(report["schema"] == "neverd.baseline.ghidra-vt-smoke.v1",
                "unsupported Ghidra export schema")
        rows = [row for group in report["match_sets"] for row in group["matches"]
                if row["type"] == "Function" and row["status"] == "ACCEPTED"]
        require(all(row["source_space"] == row["destination_space"] == "ram" for row in rows),
                "Ghidra address-space mapping needs an explicit adapter")
        return [(address(row["source"], 10), address(row["destination"], 10)) for row in rows]
    with closing(sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True)) as database:
        if kind == "bindiff":
            identities = database.execute(
                "SELECT p.hash, s.hash FROM metadata m "
                "JOIN file p ON p.id=m.file1 JOIN file s ON s.id=m.file2").fetchall()
            require(len(identities) == 1, "BinDiff must contain one comparison")
            for recorded, source in zip(identities[0], inputs):
                require(isinstance(recorded, str) and len(recorded) in (40, 64),
                        "unsupported BinDiff input hash")
                require(recorded.lower() == digest(source, "sha1" if len(recorded) == 40 else "sha256"),
                        "BinDiff input identity does not match the corpus")
            # SQLite stores unsigned native addresses in signed INTEGER cells.
            return [(address(a & ((1 << ADDRESS_BITS) - 1)),
                     address(b & ((1 << ADDRESS_BITS) - 1)))
                    for a, b in database.execute("SELECT address1, address2 FROM function")]
        require(kind == "diaphora", "unsupported result format")
        rows = database.execute("SELECT type, address, address2 FROM results").fetchall()
        require(all(category in ("best", "partial", "unreliable", "multimatch")
                    for category, _, _ in rows), "unknown Diaphora result category")
        return [(address(a, 16), address(b, 16)) for category, a, b in rows
                if category in ("best", "partial")]


def evaluate(corpus_path: Path, run_path: Path) -> dict:
    corpus, run = load(corpus_path), load(run_path)
    require(corpus["schema"] == CORPUS_SCHEMA and run["schema"] == RUN_SCHEMA,
            "unsupported corpus or run schema")
    cases = [case for case in corpus["cases"] if case["id"] == run["case"]]
    require(len(cases) == 1, "run must identify exactly one corpus case")
    case = cases[0]
    require(case["truth_complete"] is True, "this evaluator requires complete function truth")
    expected = truth_pairs(case["truth"])
    inputs = [checked_artifact(case[side], corpus_path.parent) for side in ("primary", "secondary")]
    require(run["input_sha256"] == [digest(path) for path in inputs],
            "run input identities or comparison direction do not match the corpus")
    require(run["completed"] is True, "tool run did not complete")
    require(run["track"] in ("end-to-end", "common-input"), "unknown comparison track")
    require(bool(run["tool"]["name"]) and bool(run["tool"]["version"]), "tool version is required")
    result = checked_artifact(run["result"], run_path.parent)
    for item in run["artifacts"]:
        checked_artifact(item, run_path.parent)
    pairs = read_pairs(run["format"], result, inputs)
    # Detect mutations during evaluation as well as stale receipts.
    for side in ("primary", "secondary"):
        checked_artifact(case[side], corpus_path.parent)
    checked_artifact(run["result"], run_path.parent)
    return {
        "schema": RESULT_SCHEMA, "case": case["id"], "category": case["category"],
        "tool": run["tool"], "track": run["track"], "format": run["format"],
        "corpus_sha256": digest(corpus_path), "run_sha256": digest(run_path),
        "evaluator_sha256": digest(Path(__file__)),
        "result_sha256": run["result"]["sha256"], "input_sha256": run["input_sha256"],
        "input_binding": "native-and-receipt" if run["format"] in ("neverd", "bindiff") else "receipt",
        "metrics": score(expected, pairs),
        "scope": "Owned development diagnostics; no held-out or timing qualification.",
        "metric_note": "A wrong pair can also miss a true pair; wrong and missed are not disjoint function counts.",
    }


FUNCTIONS = {
    "rotate": "return (x << 13) | (x >> 51);",
    "multiply": "return (x * 37) ^ (x >> 11);",
    "fold": "return (x & 255) + ((x >> 8) & 255) + ((x >> 16) & 255);",
    "clamp": "return x < 500 ? x : 500;",
    "alternate": "return (x & 1) ? (x + 0x1234) : (x ^ 0x5678);",
    "bound": "return (x & 255) < BOUND ? x + 5 : x - 9;",
    "combine": "return (x ^ (x << 7)) + (x >> 17) + 31;",
    "dispatch": "return x == 0x7788 ? rotate(x) : multiply(x + 19);",
}


def reordered_source(reverse: bool) -> str:
    names = list(FUNCTIONS)
    ordered = list(reversed(names[:-1])) + names[-1:] if reverse else names
    text = "typedef unsigned long long u64;\n#define OWN __attribute__((noinline,used))\n"
    text += "\n".join(f"OWN u64 {name}(u64 x) {{ {FUNCTIONS[name]} }}" for name in ordered)
    text += "\nOWN u64 entry(u64 x) { return "
    text += " + ".join(f"{name}(x + {index})" for index, name in enumerate(names)) + "; }\n"
    return text


def linker_truth(path: Path, expected: dict[str, set[str]]) -> dict[str, int]:
    """Read only authored .text symbols from the retained LLD map."""
    owner = None
    result = {}
    for line in path.read_text().splitlines():
        fields = line.split(maxsplit=4)
        if len(fields) != 5 or not re.fullmatch(r"[0-9a-fA-F]+", fields[0]):
            continue
        token = fields[4]
        if ":(" in token:
            candidate, section = token.rsplit(":(", 1)
            owner = Path(candidate).name if section == ".text)" else None
        elif owner in expected and token in expected[owner]:
            key = owner + ":" + token
            require(key not in result, f"duplicate linker-map identity: {key}")
            result[key] = int(fields[0], 16)
    identities = {owner + ":" + name for owner, names in expected.items() for name in names}
    require(result.keys() == identities, f"linker map is missing authored functions: {identities - result.keys()}")
    require(len(set(result.values())) == len(result), "fixture contains folded function addresses")
    return result


def tool_record(path: Path) -> dict:
    return {"path": str(path), "sha256": digest(path), "version": subprocess.check_output(
        [str(path), "--version"], text=True, stderr=subprocess.STDOUT).strip()}


def generate(args: argparse.Namespace) -> int:
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    # Preserve argv[0]: LLVM's ld.lld symlink selects the ELF driver in lld.
    compiler, linker, strip = (path.absolute() for path in (args.clang, args.linker, args.strip))
    tools = {"clang": tool_record(compiler), "linker": tool_record(linker), "strip": tool_record(strip)}
    manifest = {"schema": CORPUS_SCHEMA, "origin": "NeverD-authored C; truth from retained LLD maps",
                "architecture": "x86-64", "format": "ELF", "tools": tools,
                "generator_sha256": digest(Path(__file__)), "cases": []}
    for name in ("stripped-reorder", "stripped-reorder-bound-change", "duplicate-symbol-reorder"):
        folder = output / name
        private, inputs = folder / "truth", folder / "inputs"
        private.mkdir(parents=True)
        inputs.mkdir()
        commands, truth = [], {}
        def execute(command: list[str]) -> None:
            commands.append(command)
            with (private / "build.log").open("a") as log:
                subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
        if name == "duplicate-symbol-reorder":
            sources = {
                "one": "static __attribute__((noinline)) int helper(int x) { return x * 7 + 11; }\nint one(int x) { return helper(x) + 5; }\n",
                "two": "static __attribute__((noinline)) int helper(int x) { return (x ^ 93) + 17; }\nint two(int x) { return helper(x) - 2; }\n",
                "entry": "int one(int); int two(int); int entry(int x) { return one(x) + two(x); }\n",
            }
            expected = {"one.o": {"one", "helper"}, "two.o": {"two", "helper"}, "entry.o": {"entry"}}
        else:
            sources = {"before": reordered_source(False), "after": reordered_source(True)}
            expected = {"unit.o": set(FUNCTIONS) | {"entry"}}
        for source_name, content in sources.items():
            (private / (source_name + ".c")).write_text(content)
        for side in ("before", "after"):
            object_dir = private / side
            object_dir.mkdir()
            objects = []
            names = ["two", "one", "entry"] if side == "after" else ["one", "two", "entry"]
            if name != "duplicate-symbol-reorder":
                names = [side]
            for source_name in names:
                obj = object_dir / (source_name + ".o" if name == "duplicate-symbol-reorder" else "unit.o")
                bound = "17" if side == "after" and name.endswith("bound-change") else "16"
                execute([str(compiler), "--target=x86_64-unknown-linux-gnu", "-O2", "-ffreestanding",
                         "-fno-stack-protector", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
                         "-DBOUND=" + bound, "-c", str(private / (source_name + ".c")), "-o", str(obj)])
                objects.append(str(obj))
            unstripped, map_path = private / (side + ".unstripped"), private / (side + ".map")
            execute([str(linker), "-e", "entry", "--build-id=none", "-Map=" + str(map_path),
                     *objects, "-o", str(unstripped)])
            truth[side] = linker_truth(map_path, expected)
            if name == "duplicate-symbol-reorder":
                shutil.copyfile(unstripped, inputs / (side + ".elf"))
            else:
                execute([str(strip), "--strip-all", "-o", str(inputs / (side + ".elf")), str(unstripped)])
        rows = [{"id": identity, "primary": hex(truth["before"][identity]),
                 "secondary": hex(truth["after"][identity])} for identity in sorted(truth["before"])]
        truth_pairs(rows)
        require(any(row["primary"] != row["secondary"] for row in rows), "fixture did not reorder functions")
        manifest["cases"].append({"id": name, "category": name, "truth_complete": True, "truth": rows,
                                  "primary": artifact(inputs / "before.elf", output),
                                  "secondary": artifact(inputs / "after.elf", output),
                                  "commands": commands,
                                  "evidence": [artifact(path, output) for path in sorted(private.rglob("*")) if path.is_file()]})
    save(output / "manifest.json", manifest)
    print(output / "manifest.json")
    return 0


def run_neverd(args: argparse.Namespace) -> int:
    corpus_path = args.corpus.resolve()
    corpus = load(corpus_path)
    require(corpus["schema"] == CORPUS_SCHEMA, "unsupported corpus schema")
    executable, library = args.neverd.absolute(), args.library.absolute()
    require(sys.platform.startswith("linux") and library.name == "libneverd.so",
            "run requires Linux and an explicit libneverd.so runtime")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LD_LIBRARY_PATH=str(library.parent), NEVERD_THREADS=str(args.threads))
    version = subprocess.check_output([str(executable), "--version"], env=env, text=True, stderr=subprocess.STDOUT).strip()
    runtime = [artifact(path, output) for path in (executable, library)]
    results = []
    for case in corpus["cases"]:
        truth_pairs(case["truth"])
        inputs = [checked_artifact(case[side], corpus_path.parent) for side in ("primary", "secondary")]
        folder = output / case["id"]
        folder.mkdir()
        report = folder / "result.nddiff"
        command = [str(executable), "diff", "--structural", "-a", str(inputs[0]), "-b", str(inputs[1]),
                   "--save", str(report), "--json"]
        start = time.monotonic()
        with (folder / "stdout.log").open("w") as stdout, (folder / "stderr.log").open("w") as stderr:
            try:
                exit_code = subprocess.run(command, env=env, stdout=stdout, stderr=stderr,
                                           timeout=args.timeout).returncode
                error = None
            except subprocess.TimeoutExpired:
                exit_code, error = None, "timeout"
        run = {"schema": RUN_SCHEMA, "case": case["id"], "format": "neverd", "track": "end-to-end",
               "runner_sha256": digest(Path(__file__)),
               "tool": {"name": "NeverD", "version": version}, "input_sha256": [item["sha256"] for item in (case["primary"], case["secondary"])],
               "command": command, "configuration": {"threads": args.threads}, "exit": exit_code,
               "wall_seconds": time.monotonic() - start, "completed": exit_code in (0, 2) and report.is_file(),
               "error": error, "artifacts": [artifact(path, folder) for path in (executable, library)],
               "result": artifact(report, folder) if report.exists() else None}
        run_path = folder / "run.json"
        save(run_path, run)
        try:
            result = evaluate(corpus_path, run_path)
        except (ValueError, KeyError, OSError) as failure:
            result = {"case": case["id"], "error": str(failure)}
        save(folder / "evaluation.json", result)
        results.append(result)
        print(case["id"], result.get("metrics", result.get("error")), flush=True)
    for item in runtime:
        checked_artifact(item, output)
    save(output / "results.json", {"schema": RESULT_SCHEMA, "cases": results, "runtime": runtime})
    return 0 if all(result.get("metrics", {}).get("exact_mapping", False) for result in results) else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    generate_parser = commands.add_parser("generate", help="build owned x64 ELF diagnostics with explicit tools")
    for flag in ("output-dir", "clang", "linker", "strip"):
        generate_parser.add_argument("--" + flag, required=True, type=Path)
    run_parser = commands.add_parser("run", help="run a frozen Linux NeverD CLI on every corpus case")
    for flag in ("corpus", "neverd", "library", "output-dir"):
        run_parser.add_argument("--" + flag, required=True, type=Path)
    run_parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS)
    run_parser.add_argument("--threads", type=int, default=1)
    evaluate_parser = commands.add_parser("evaluate", help="score a hash-bound retained tool result")
    for flag in ("corpus", "run", "output"):
        evaluate_parser.add_argument("--" + flag, required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "generate":
            return generate(args)
        if args.command == "run":
            require(math.isfinite(args.timeout) and args.timeout > 0 and args.threads > 0,
                    "timeout must be finite and positive; threads must be positive")
            return run_neverd(args)
        result = evaluate(args.corpus.resolve(), args.run.resolve())
        save(args.output, result)
        print(json.dumps(result["metrics"], sort_keys=True))
        return 0 if result["metrics"]["exact_mapping"] else 1
    except (ValueError, KeyError, TypeError, OSError, sqlite3.Error, subprocess.SubprocessError) as error:
        print(f"binary diff benchmark: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
