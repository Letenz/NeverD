#!/usr/bin/env python3
"""Build the pe/ signature files from collected MSVC and Windows SDK libraries.

The inputs are the release assets the signatures repository's
`msvc-libraries.yml` workflow publishes: one zstd-compressed tar per
toolset-or-SDK and architecture, each with a JSON manifest naming what it
holds.  Every asset becomes lines in one file:

    toolset assets  ->  pe/<x86|arm>/<32|64>/vs<year>.pat
    winsdk assets   ->  pe/<x86|arm>/<32|64>/winsdk.pat

so a year's servicing toolsets (VS 2026's 14.50 and its current default, say)
land in the same file, and so do all Windows SDK versions.

Four rules decide what a file holds:

  * Lines come from `neverd-sigmaker` with `--machine` set to the asset's
    architecture, so an object for another target that shares a library
    directory can never be filed under the wrong one, and with a tail long
    enough to state every byte of every function.  A match is then agreement
    over the whole routine, not over its prologue.

  * Names are the linkage names the libraries' symbol tables spell.  Nothing
    here renames anything.

  * A line already in the output file is kept (`--merge-existing`), so
    regenerating from a newer toolset adds to a file instead of forgetting
    the servicing builds it was made from before.

  * When lines state the same bytes under different names, all of them are
    dropped.  The pattern cannot tell those routines apart -- MSVC folds
    identical template instantiations, for one -- and any single name chosen
    among them would be a guess presented as an identification.  The count
    is reported.

Every file written is read back through `neverd-sigmaker --verify`, the
loader's own parser: the loader rejects a whole directory for one bad line.

Usage:

    python3 scripts/signatures/build_msvc_signatures.py \\
        --sigmaker build/bin/neverd-sigmaker \\
        --assets release-assets/ \\
        --output signatures \\
        --merge-existing
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

# Larger than any function, so the signature maker's tail reaches the end of
# every function it describes.  It clamps to the function size.
FULL_COVERAGE_TAIL = 65535

# The collector's architecture names, the directory the loader searches for
# that architecture, and the COFF machine neverd-sigmaker keeps.
ARCHITECTURES = {
    "x86": (Path("pe/x86/32"), "x86"),
    "x64": (Path("pe/x86/64"), "x64"),
    "arm": (Path("pe/arm/32"), "arm"),
    "arm64": (Path("pe/arm/64"), "arm64"),
}

LIBRARY_SUFFIXES = (".lib", ".obj")


class BuildError(RuntimeError):
    """An input is missing, malformed, or produced something unusable."""


# --- assets ------------------------------------------------------------------


@dataclass(frozen=True)
class Asset:
    name: str
    manifest: dict
    archive: Path

    @property
    def arch(self) -> str:
        return self.manifest["arch"]

    @property
    def directory(self) -> Path:
        return ARCHITECTURES[self.arch][0]

    @property
    def machine(self) -> str:
        return ARCHITECTURES[self.arch][1]

    @property
    def library(self) -> str:
        kind = self.manifest["kind"]
        if kind == "toolset":
            return f"vs{int(self.manifest['visual_studio']['year'])}"
        if kind == "winsdk":
            return "winsdk"
        raise BuildError(f"{self.name}: unknown asset kind {kind!r}")

    @property
    def output(self) -> Path:
        return self.directory / f"{self.library}.pat"

    def provenance(self) -> dict:
        entry = {
            "asset": self.name,
            "archive_sha256": self.manifest["archive"]["sha256"],
            "kind": self.manifest["kind"],
            "arch": self.arch,
        }
        for key in ("toolset_version", "compiler_version", "windows_sdk_version"):
            if self.manifest.get(key):
                entry[key] = self.manifest[key]
        return entry


def load_assets(directory: Path, patterns: list[str]) -> list[Asset]:
    assets = []
    for manifest_path in sorted(directory.glob("*.json")):
        if manifest_path.name == "assets.json":
            continue
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        name = manifest.get("asset")
        if name != manifest_path.stem:
            raise BuildError(f"{manifest_path.name}: manifest names asset {name!r}")
        if patterns and not any(fnmatch.fnmatchcase(name, p) for p in patterns):
            continue
        if manifest.get("arch") not in ARCHITECTURES:
            raise BuildError(f"{name}: unsupported architecture {manifest.get('arch')!r}")
        archive = directory / manifest["archive"]["name"]
        if not archive.is_file():
            raise BuildError(f"{name}: archive {archive.name} is missing")
        assets.append(Asset(name, manifest, archive))
    if not assets:
        raise BuildError(f"no assets in {directory} match {patterns or ['*']}")
    return assets


def extract(asset: Asset, destination: Path) -> list[Path]:
    """Unpack an asset and return its libraries in a stable order."""

    destination.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["tar", "--zstd", "-xf", str(asset.archive), "-C", str(destination)],
        check=True,
    )
    recorded = {entry["path"] for entry in asset.manifest["files"]}
    found = {
        path.relative_to(destination).as_posix()
        for path in destination.rglob("*")
        if path.is_file()
    }
    if recorded != found:
        missing = sorted(recorded - found)[:5]
        extra = sorted(found - recorded)[:5]
        raise BuildError(f"{asset.name}: archive disagrees with manifest: "
                         f"missing {missing}, unexpected {extra}")
    return sorted(
        (destination / path for path in found if path.lower().endswith(LIBRARY_SUFFIXES)),
        key=lambda path: path.as_posix().lower(),
    )


# --- pattern lines -------------------------------------------------------------


@dataclass(frozen=True)
class PatternLine:
    """One .pat line split into what it asserts and what it names."""

    text: str
    key: str
    names: tuple[str, ...]


def parse_line(text: str) -> PatternLine | None:
    """Split a line into its byte assertions and its names.

    The key is everything but the names, so two lines with equal keys make
    the same claim about a routine's bytes.
    """

    stripped = text.strip()
    if not stripped or stripped.startswith((";", "#")) or stripped == "---":
        return None
    tokens = stripped.split()
    if len(tokens) < 6:
        raise BuildError(f"malformed pattern line: {stripped[:120]}")
    names: list[str] = []
    rest = tokens[:4]
    index = 4
    while index < len(tokens):
        token = tokens[index]
        if token.startswith(":") and index + 1 < len(tokens):
            names.append(f"{token} {tokens[index + 1]}")
            index += 2
            continue
        rest.append(token)
        index += 1
    if not names:
        raise BuildError(f"pattern line names nothing: {stripped[:120]}")
    return PatternLine(stripped, " ".join(rest), tuple(names))


@dataclass
class MergeResult:
    lines: list[str]
    duplicates: int = 0
    conflicting_lines: int = 0
    conflicting_groups: int = 0
    kept_existing: int = 0
    added: int = 0
    conflict_examples: list[list[str]] = field(default_factory=list)


def merge(existing: list[str], generated: list[str]) -> MergeResult:
    """Union two sets of lines, dropping every ambiguous byte claim.

    A group of lines with the same key but more than one set of names is
    ambiguous: the bytes cannot say which routine they are.  All of its
    lines are dropped, whichever side they came from.
    """

    groups: dict[str, dict[tuple[str, ...], str]] = {}
    origin_existing: set[str] = set()
    seen = 0
    for source, lines in (("existing", existing), ("generated", generated)):
        for raw in lines:
            parsed = parse_line(raw)
            if parsed is None:
                continue
            seen += 1
            variants = groups.setdefault(parsed.key, {})
            if parsed.names not in variants:
                variants[parsed.names] = parsed.text
                if source == "existing":
                    origin_existing.add(parsed.text)

    result = MergeResult(lines=[])
    for key, variants in groups.items():
        if len(variants) > 1:
            result.conflicting_groups += 1
            result.conflicting_lines += len(variants)
            if len(result.conflict_examples) < 20:
                result.conflict_examples.append(sorted(" ".join(n) for n in variants))
            continue
        (text,) = variants.values()
        result.lines.append(text)
        if text in origin_existing:
            result.kept_existing += 1
        else:
            result.added += 1
    result.lines.sort()
    result.duplicates = seen - sum(len(v) for v in groups.values())
    return result


# --- generation -------------------------------------------------------------------


def run_sigmaker(
    sigmaker: Path, libraries: list[Path], machine: str, tail: int, output: Path
) -> str:
    if not libraries:
        raise BuildError("no libraries to read")
    command = [
        str(sigmaker),
        *map(str, libraries),
        "-o",
        str(output),
        "--machine",
        machine,
        "--tail",
        str(tail),
    ]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.stderr.strip():
        print(result.stderr.strip(), file=sys.stderr)
    if result.returncode != 0:
        raise BuildError(f"neverd-sigmaker failed ({result.returncode})")
    return result.stdout.strip()


def verify(sigmaker: Path, path: Path) -> None:
    result = subprocess.run(
        [str(sigmaker), "--verify", str(path)], capture_output=True, text=True
    )
    if result.returncode != 0:
        raise BuildError(
            f"{path} does not load: {(result.stderr or result.stdout).strip()}"
        )


def merge_provenance(previous: dict | None, update: dict) -> dict:
    """Keep the sources a file was built from across regenerations."""

    sources = {}
    for entry in (previous or {}).get("sources", []) + update["sources"]:
        sources[(entry["asset"], entry["archive_sha256"])] = entry
    merged = dict(update)
    merged["sources"] = sorted(
        sources.values(), key=lambda entry: (entry["asset"], entry["archive_sha256"])
    )
    return merged


def build(args: argparse.Namespace) -> int:
    assets = load_assets(args.assets, args.asset)
    by_output: dict[Path, list[Asset]] = {}
    for asset in assets:
        by_output.setdefault(asset.output, []).append(asset)

    work_root = Path(tempfile.mkdtemp(prefix="neverd-msvc-sigs-", dir=args.work))
    try:
        for relative, members in sorted(by_output.items()):
            generated: list[str] = []
            sources = []
            for asset in members:
                libraries = extract(asset, work_root / asset.name)
                pat = work_root / f"{asset.name}.pat"
                summary = run_sigmaker(args.sigmaker, libraries, asset.machine, args.tail, pat)
                lines = pat.read_text(encoding="utf-8").splitlines()
                print(f"{asset.name}: {len(libraries)} libraries; {summary}", flush=True)
                if not lines:
                    raise BuildError(f"{asset.name} produced no signatures")
                generated.extend(lines)
                entry = asset.provenance()
                entry["sigmaker"] = summary
                sources.append(entry)
                shutil.rmtree(work_root / asset.name)
                pat.unlink()

            destination = args.output / relative
            existing: list[str] = []
            if args.merge_existing and destination.is_file():
                existing = destination.read_text(encoding="utf-8").splitlines()
            result = merge(existing, generated)
            if not result.lines:
                raise BuildError(f"{relative}: nothing left to write")
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text("\n".join(result.lines) + "\n", encoding="utf-8")
            verify(args.sigmaker, destination)

            provenance_path = destination.with_suffix(".sources.json")
            previous = None
            if args.merge_existing and provenance_path.is_file():
                previous = json.loads(provenance_path.read_text(encoding="utf-8"))
            provenance = merge_provenance(
                previous,
                {
                    "file": relative.as_posix(),
                    "generator": {
                        "neverd_ref": args.neverd_ref,
                        "release": args.release,
                        "tail": args.tail,
                    },
                    "sources": sources,
                    "lines": len(result.lines),
                },
            )
            provenance_path.write_text(
                json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8"
            )
            print(
                f"{relative}: {len(result.lines)} lines "
                f"({result.added} new, {result.kept_existing} kept, "
                f"{result.duplicates} duplicates folded, "
                f"{result.conflicting_lines} lines in {result.conflicting_groups} "
                f"ambiguous groups dropped)",
                flush=True,
            )
            for example in result.conflict_examples[:5]:
                print(f"  ambiguous: {', '.join(example)[:200]}", flush=True)
    finally:
        shutil.rmtree(work_root, ignore_errors=True)
    return 0


def parse_arguments(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--sigmaker", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True,
                        help="directory holding <asset>.tar.zst and <asset>.json")
    parser.add_argument("--asset", action="append", default=[],
                        help="only assets whose name matches this glob (repeatable)")
    parser.add_argument("--output", type=Path, required=True,
                        help="signature tree root; files land under pe/")
    parser.add_argument("--merge-existing", action="store_true",
                        help="keep the lines already in each output file")
    parser.add_argument("--tail", type=int, default=FULL_COVERAGE_TAIL)
    parser.add_argument("--neverd-ref", default=None,
                        help="NeverD revision of the signature maker, for provenance")
    parser.add_argument("--release", default=None,
                        help="release tag the assets came from, for provenance")
    parser.add_argument("--work", type=Path, default=None,
                        help="scratch directory for unpacked libraries")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_arguments(argv)
    if not args.sigmaker.is_file():
        print(f"error: {args.sigmaker} does not exist", file=sys.stderr)
        return 1
    try:
        return build(args)
    except BuildError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
