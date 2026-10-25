#!/usr/bin/env python3
"""Add missing patterns after checking unpruned source-library evidence.

Inputs use NAME=PATH, where NAME is the loader's library stem (for example
vs2022 or winsdk). Repeat --evidence for every raw sigmaker output belonging
to that library, including other versions and libraries in the same target
directory. An already pruned database is not a substitute for raw evidence.
Existing packs are read-only; output contains additional numbered parts.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re

if __package__:
    from . import build_msvc_signatures as builder
else:
    import build_msvc_signatures as builder


def named_path(value: str) -> tuple[str, Path]:
    name, separator, path = value.partition("=")
    if not separator or not builder.LIBRARY_NAME.fullmatch(name) or not path:
        raise argparse.ArgumentTypeError("expected library=path")
    return name, Path(path)


def select(candidates: dict[str, builder.FoldResult],
           evidence: list[tuple[str, Path]], existing: Path
           ) -> dict[str, list[builder.PatternLine]]:
    """Retain novel claims with complete byte-collision evidence.

    Only source claims with the same key or whose bytes cover a candidate
    can affect its settlement. Retain those claims before folding: discarded
    ambiguities are essential evidence. This bounds memory by relevant claims,
    rather than by the size of the entire source collection.
    """
    keys = {line.key for result in candidates.values() for line in result.lines}
    heads = {" ".join(key.split()[:4]) for key in keys}
    by_crc = defaultdict(list)
    by_prefix = defaultdict(list)
    short_prefixes = defaultdict(list)
    for key in keys:
        shape = builder.Shape.of(key)
        if shape.crc_len and len(shape.lead) < 2 * shape.total:
            by_crc[shape.crc_len, shape.crc].append(shape)
        elif len(shape.stated_prefix()) >= 2 * builder.MIN_PREFIX_BYTES:
            by_prefix[shape.stated_prefix()[:2 * builder.MIN_PREFIX_BYTES]].append(shape)
        else:
            short_prefixes[shape.stated_prefix()].append(shape)

    def relevant(raw: str, allow_evidence: bool = False) -> builder.PatternLine | None:
        parts = raw.split(maxsplit=4)
        if len(parts) < 5 or parts[0].startswith(("#", ";")):
            return None
        rest = parts[4].split(maxsplit=1)
        if rest[0] == "!evidence":
            if not allow_evidence or len(rest) != 2:
                raise builder.BuildError("collision evidence belongs only in --evidence inputs")
            raw = " ".join([*parts[:4], rest[1]])
        nearby = by_crc.get((int(parts[1], 16), parts[2]), []) + by_prefix.get(
            parts[0][:2 * builder.MIN_PREFIX_BYTES], [])
        for prefix, shapes in short_prefixes.items():
            if parts[0].startswith(prefix):
                nearby += shapes
        if not nearby and " ".join(parts[:4]) not in heads:
            return None
        line = builder.parse_line(raw)
        if line is None:
            return None
        shape = builder.Shape.of(line.key)
        return line if line.key in keys or any(
            other != shape and other.covered_by(shape) for other in nearby) else None

    grouped: dict[str, dict[str, None]] = defaultdict(dict)
    source_claims = defaultdict(set)
    for name, path in evidence:
        with path.open(encoding="utf-8") as source:
            for raw in source:
                line = relevant(raw, allow_evidence=True)
                if line is not None:
                    grouped[name][line.text] = None
                    source_claims[name, line.key].update(line.names)
    for name, result in candidates.items():
        for line in result.lines:
            if not set(line.names) & source_claims[name, line.key]:
                raise builder.BuildError(f"{name}: candidate lacks raw source evidence: {line.names}")

    results = {}
    for name, lines in grouped.items():
        result = builder.fold(list(lines))
        result.lines = [line for line in result.lines if line.key in keys]
        results[Path(name + ".pat")] = result
    old = []
    owned = defaultdict(set)
    for path in builder.pattern_files(existing):
        with path.open(encoding="utf-8") as source:
            for raw in source:
                line = relevant(raw)
                if line is not None:
                    old.append(line)
                    owned[line.key].update(line.names)
    results[Path("__existing__")] = builder.FoldResult(
        lines=[line for line in old if line.key in keys], evidence=old)
    builder.settle_directory(results)

    selected = {}
    for name, result in candidates.items():
        wanted = defaultdict(set)
        for line in result.lines:
            if not line.require_references:
                wanted[line.key].update(line.names)
        selected[name] = [
            line for line in results.get(Path(name + ".pat"), builder.FoldResult(lines=[])).lines
            if set(line.names) & wanted[line.key]
            and not set(line.names) & owned[line.key]]

    # Required context does not need a rival pack to be loaded: an unknown
    # reference suppresses the candidate outright. Retain such a claim only
    # when its references distinguish it from EVERY overlapping source claim,
    # including ambiguous and covered claims that folding would discard.
    # This permits evidence from independently selected toolset packs without
    # turning a missing rival into a unique byte-only name.
    claims = {}
    for lines in grouped.values():
        for text in lines:
            line = builder.parse_line(text)
            claims[line.text] = line
    for line in old:
        claims[line.text] = line
    by_key = defaultdict(list)
    for line in claims.values():
        by_key[line.key].append(line)
    shapes = {key: builder.Shape.of(key) for key in by_key}
    for name, result in candidates.items():
        retained = {line.text for line in selected[name]}
        for line in result.lines:
            if not line.require_references or not line.refs or \
                    set(line.names) & owned[line.key] or line.text in retained:
                continue
            shape = builder.Shape.of(line.key)
            safe = True
            for key, counterclaims in by_key.items():
                if key != line.key and not shape.covered_by(shapes[key]):
                    continue
                if any(builder.another_routine(line.names, other.names)
                       and not builder.distinguished([line, other])
                       for other in counterclaims):
                    safe = False
                    break
            if safe:
                selected[name].append(line)
        selected[name].sort(key=lambda line: line.text)
    return selected


def digest(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=named_path, action="append", required=True)
    parser.add_argument("--evidence", type=named_path, action="append", required=True)
    parser.add_argument("--existing", type=Path, required=True,
                        help="existing patterns for this one format/architecture")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if not args.existing.is_dir():
            raise builder.BuildError("existing directory does not exist")
        raw = defaultdict(list)
        for name, path in args.candidate:
            raw[name].extend(path.read_text(encoding="utf-8").splitlines())
        selected = select({name: builder.fold(lines) for name, lines in raw.items()},
                          args.evidence, args.existing)
        args.output.mkdir(parents=True, exist_ok=True)
        written = []
        for name, lines in sorted(selected.items()):
            indices = [1]
            for path in builder.pattern_files(args.existing):
                match = re.fullmatch(re.escape(name) + r"\.part([2-9][0-9]*|1[0-9]+)", path.stem)
                if match:
                    indices.append(int(match[1]))
            if not lines:
                continue
            parts = builder.split_parts([line.text for line in lines], builder.MAX_FILE_BYTES)
            suffix = ".npat" if any(line.require_references for line in lines) else ".pat"
            for index, texts in enumerate(parts, start=max(indices) + 1):
                path = args.output / f"{name}.part{index}{suffix}"
                # Never replace an existing pack, even if output == existing.
                with path.open("x", encoding="utf-8") as target:
                    target.write("\n".join(texts) + "\n---\n")
                written.append({"file": path.name, "patterns": len(texts), "sha256": digest(path)})
        inputs = lambda paths: [{"library": name, "file": path.name, "sha256": digest(path)}
                                for name, path in paths]
        report = {"candidates": inputs(args.candidate), "evidence": inputs(args.evidence),
                  "existing": [{"file": path.name, "sha256": digest(path)}
                               for path in builder.pattern_files(args.existing)
                               if path.name not in {entry["file"] for entry in written}],
                  "output": written}
        with (args.output / "supplement.sources.json").open("x", encoding="utf-8") as target:
            target.write(json.dumps(report, indent=2) + "\n")
        print(json.dumps(written, indent=2))
        return 0
    except (builder.BuildError, OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
