#!/usr/bin/env python3
"""Constrain previously unrepresented byte collisions using unpruned sources.

This is an offline refinement of existing records, not a source-name recognizer.
It does not read evaluation labels or invent aliases. A conflicting complete
body needs all distinguishing references, or becomes an incoming-only query.
An incomplete conflicting record cannot supply either proof and is omitted.
"""

from __future__ import annotations

import argparse
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import sys

try:
    from . import build_msvc_signatures as builder
except ImportError:
    import build_msvc_signatures as builder


def describe(path: Path) -> dict:
    return {"file": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def library_name(path: Path) -> str:
    return re.sub(r"\.part(?:[2-9][0-9]*|1[0-9]+)$", "", path.stem)


def refine(existing: Path, evidence: list[Path], output: Path, machine: str) -> dict:
    if output.exists():
        raise builder.BuildError("output already exists; use a fresh directory")
    if machine not in {"x86", "x64", "arm", "arm64"}:
        raise builder.BuildError(f"unsupported machine {machine!r}")
    paths = builder.pattern_files(existing)
    if not paths or not evidence:
        raise builder.BuildError("existing patterns and unpruned evidence are required")
    source = {}
    for path in evidence:
        count = 0
        for number, text in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            row = builder.parse_line(text, evidence=True)
            if row is None:
                continue
            if text.split()[4] != "!evidence":
                raise builder.BuildError(f"{path}:{number}: expected unpruned !evidence")
            source[row.text] = row
            count += 1
        if not count:
            raise builder.BuildError(f"{path}: empty source evidence")

    documents = {path: path.read_text(encoding="utf-8").splitlines() for path in paths}
    known = defaultdict(list)
    for path, lines in documents.items():
        for text in lines:
            row = builder.parse_line(text)
            if row and not (row.require_caller or row.require_references or
                            row.is_vtable or row.is_load_config):
                known[library_name(path), row.key].append(row)
    by_key = defaultdict(list)
    for row in source.values():
        by_key[row.key].append(row)
    shapes = {key: builder.Shape.of(key) for key in by_key}
    ordered = sorted((shape.lead, key) for key, shape in shapes.items())
    leads = [lead for lead, _ in ordered]
    by_lead = defaultdict(list)
    by_crc = defaultdict(list)
    for key, shape in shapes.items():
        by_lead[shape.lead].append(key)
        if shape.crc_len:
            by_crc[shape.crc_len, shape.crc, len(shape.lead)].append(key)

    coverage = {}
    changed = {}
    additions = defaultdict(list)
    actions = []
    for path, lines in documents.items():
        kept = []
        library = library_name(path)
        for number, text in enumerate(lines, 1):
            row = builder.parse_line(text)
            if row is None or row.require_caller or row.is_vtable or row.is_load_config:
                kept.append(text)
                continue
            if row.key not in coverage:
                shape = builder.Shape.of(row.key)
                if shape.crc_len:
                    nearby = by_crc[shape.crc_len, shape.crc, len(shape.lead)]
                else:
                    prefix = shape.stated_prefix()
                    nearby = [key for _, key in ordered[
                        bisect_left(leads, prefix):bisect_right(leads, prefix + "~")]]
                    # A source can encode the rest in its tail, or end at a
                    # shorter RET before our padding. Its leading field is
                    # then a prefix of ours, outside the forward range.
                    for end in range(2, len(prefix), 2):
                        nearby.extend(by_lead[prefix[:end]])
                coverage[row.key] = [key for key in nearby
                                     if shape.covered_by(shapes[key]) or
                                     shape.padding_collision_with(shapes[key], machine=machine)]
            rivals = [other for key in coverage[row.key] for other in by_key[key]
                      if builder.another_routine(row.names, other.names)
                      and not any(set(loaded.names).intersection(other.names)
                                  and loaded.refs == other.refs
                                  and loaded.data_sites == other.data_sites
                                  and loaded.local_sites == other.local_sites
                                  for loaded in known[library, key])]
            if not rivals:
                kept.append(text)
                continue
            complete = builder.Shape.of(row.key).fully_verified()
            if complete and row.refs and all(builder.distinguished([row, other]) for other in rivals):
                if row.require_references:
                    kept.append(text)
                    continue
                action, token = "required", "!refs"
            elif not row.require_references and complete and all(name.startswith(":0000 ") for name in row.names):
                action, token = "caller", "!caller"
            else:
                action, token = "omitted", None
            if token:
                words = row.text.split()
                words.insert(4, token)
                native = " ".join(words)
                builder.parse_line(native)
                additions[path.with_suffix(".npat").name].append(native)
            actions.append({"file": path.name, "line": number, "pattern": row.text,
                            "action": action, "rival_count": len(rivals),
                            "rival_example": rivals[0].text})
        changed[path.name] = kept
    for name, rows in additions.items():
        lines = [line for line in changed.get(name, []) if line.strip() != "---"]
        seen = set(lines)
        for row in rows:
            if row not in seen:
                lines.append(row)
                seen.add(row)
        changed[name] = lines + ["---"]

    # Parse and plan the complete transaction before writing any output.
    output.mkdir(parents=True)
    for name, lines in changed.items():
        (output / name).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return {"schema_version": 1, "machine": machine,
            "scope": "Previously unrepresented source claims; same-library byte cohorts with identical typed references remain intact. Contradicted required claims are omitted.",
            "inputs": [describe(path) for path in paths],
            "evidence": [describe(path) for path in evidence],
            "outputs": [describe(path) for path in sorted(output.iterdir())],
            "counts": dict(Counter(action["action"] for action in actions)),
            "actions": actions}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--existing", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--machine", choices=("x86", "x64", "arm", "arm64"), required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.report.exists() or args.report.resolve().is_relative_to(args.output.resolve()):
            raise builder.BuildError("report must be a new file outside the output directory")
        result = refine(args.existing, args.evidence, args.output, args.machine)
        args.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    except (builder.BuildError, OSError, ValueError) as error:
        print(f"collision refinement: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result["counts"], sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
