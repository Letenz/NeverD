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

  * A file is rebuilt from its assets alone.  Lines it held before, from an
    earlier run or from another tool, are replaced, so give the script every
    asset of a directory at once.

  * When lines state the same bytes under different names, all of them are
    dropped.  The pattern cannot tell those routines apart -- MSVC folds
    identical template instantiations, for one -- and any single name chosen
    among them would be a guess presented as an identification.  The loader
    reads every file of a directory together, so this holds across the
    directory: bytes that one file's libraries give two names are dropped
    from every file of it, and so are bytes two files name differently.
    NeverD's matcher compares a line only as far as the line's own length,
    so a line whose bytes open a longer routine of another name is dropped
    as well: in a linked image it would name that routine too.  The counts
    are reported.

Every file written is read back through `neverd-sigmaker --verify`, the
loader's own parser: the loader rejects a whole directory for one bad line.

Usage:

    python3 scripts/signatures/build_msvc_signatures.py \\
        --sigmaker build/bin/neverd-sigmaker \\
        --assets release-assets/ \\
        --output signatures
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


@dataclass(frozen=True)
class Shape:
    """A line's byte assertions, in the parts NeverD's matcher reads."""

    lead: str
    crc_len: int
    crc: str
    total: int
    tail: str

    @classmethod
    def of(cls, key: str) -> "Shape":
        tokens = key.split()
        tail = tokens[4] if len(tokens) > 4 else ""
        return cls(tokens[0], int(tokens[1], 16), tokens[2], int(tokens[3], 16), tail)

    def opens(self, other: "Shape") -> bool:
        """Whether a longer routine's line states every byte this one states.

        Only what both lines state can be compared, so a byte that one line
        leaves to a relocation and the other states counts as a difference;
        the check errs toward keeping a line.
        """

        if other.total <= self.total:
            return False
        if len(self.lead) >= 2 * self.total:
            # The whole routine fits in the leading pattern.
            return other.lead.startswith(self.lead)
        return (
            other.lead == self.lead
            and other.crc_len == self.crc_len
            and other.crc == self.crc
            and other.tail.startswith(self.tail)
        )


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
class FoldResult:
    """One file's lines once identical lines fold and ambiguous claims drop."""

    lines: list[PatternLine]
    ambiguous_keys: set[str] = field(default_factory=set)
    # Every distinct claim the libraries made, the dropped ones included.
    evidence: list[PatternLine] = field(default_factory=list)
    duplicates: int = 0
    conflicting_lines: int = 0
    conflicting_groups: int = 0
    conflict_examples: list[list[str]] = field(default_factory=list)
    dropped_across_files: int = 0
    dropped_openings: int = 0

    @property
    def texts(self) -> list[str]:
        return [line.text for line in self.lines]


def fold(lines: list[str]) -> FoldResult:
    """Fold identical lines and drop every ambiguous byte claim.

    A group of lines with the same key but more than one set of names is
    ambiguous: the bytes cannot say which routine they are.  All of its
    lines are dropped, and the key is kept so that `settle_directory` can
    drop the same bytes from the other files of the directory.
    """

    groups: dict[str, dict[tuple[str, ...], PatternLine]] = {}
    seen = 0
    for raw in lines:
        parsed = parse_line(raw)
        if parsed is None:
            continue
        seen += 1
        groups.setdefault(parsed.key, {}).setdefault(parsed.names, parsed)

    result = FoldResult(lines=[])
    for key, variants in groups.items():
        result.evidence.extend(variants.values())
        if len(variants) > 1:
            result.ambiguous_keys.add(key)
            result.conflicting_groups += 1
            result.conflicting_lines += len(variants)
            if len(result.conflict_examples) < 20:
                result.conflict_examples.append(sorted(" ".join(n) for n in variants))
            continue
        (line,) = variants.values()
        result.lines.append(line)
    result.lines.sort(key=lambda line: line.text)
    result.duplicates = seen - sum(len(v) for v in groups.values())
    return result


def settle_directory(results: dict[Path, FoldResult]) -> None:
    """Drop lines whose bytes another file of the same directory contradicts.

    The loader applies every file of a directory together.  A line one file
    keeps is therefore only an identification if no library behind any file
    of the directory gives the same bytes another name: when another file
    states them under other names, or dropped them as ambiguous, the line
    is dropped too.  Lines that repeat a claim with the same names, such as
    a CRT routine that several toolsets share unchanged, are kept.
    """

    claims: dict[str, tuple[str, ...] | None] = {}
    for result in results.values():
        for key in result.ambiguous_keys:
            claims[key] = None
    for result in results.values():
        for line in result.lines:
            if line.key not in claims:
                claims[line.key] = line.names
            elif claims[line.key] != line.names:
                claims[line.key] = None
    for result in results.values():
        kept = [line for line in result.lines if claims[line.key] is not None]
        result.dropped_across_files = len(result.lines) - len(kept)
        result.lines = kept

    # A line that opens another agrees with it on its first 16 bytes, which
    # every line states or leaves to a relocation, so bucketing by them
    # loses nothing.
    openings: dict[str, list[tuple[Shape, tuple[str, ...]]]] = {}
    for result in results.values():
        for line in result.evidence:
            shape = Shape.of(line.key)
            openings.setdefault(shape.lead[:32], []).append((shape, line.names))
    for bucket in openings.values():
        bucket.sort(key=lambda item: -item[0].total)
    for result in results.values():
        kept = []
        for line in result.lines:
            shape = Shape.of(line.key)
            opened = False
            for other, names in openings.get(shape.lead[:32], ()):
                if other.total <= shape.total:
                    break
                if names != line.names and shape.opens(other):
                    opened = True
                    break
            if not opened:
                kept.append(line)
        result.dropped_openings = len(result.lines) - len(kept)
        result.lines = kept


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


def generate(
    args: argparse.Namespace, members: list[Asset], work_root: Path
) -> tuple[list[str], list[dict]]:
    """Every line neverd-sigmaker writes for one file's assets, and their sources."""

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
    return generated, sources


def write(
    args: argparse.Namespace, relative: Path, result: FoldResult, sources: list[dict]
) -> None:
    if not result.lines:
        raise BuildError(f"{relative}: nothing left to write")
    destination = args.output / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text("\n".join(result.texts) + "\n", encoding="utf-8")
    verify(args.sigmaker, destination)

    provenance = {
        "file": relative.as_posix(),
        "generator": {
            "neverd_ref": args.neverd_ref,
            "release": args.release,
            "tail": args.tail,
        },
        "sources": sorted(
            sources, key=lambda entry: (entry["asset"], entry["archive_sha256"])
        ),
        "lines": len(result.lines),
    }
    destination.with_suffix(".sources.json").write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        f"{relative}: {len(result.lines)} lines "
        f"({result.duplicates} duplicates folded, "
        f"{result.conflicting_lines} lines in {result.conflicting_groups} "
        f"ambiguous groups dropped, "
        f"{result.dropped_across_files} dropped for bytes another file of "
        f"{relative.parent.as_posix()} names differently, "
        f"{result.dropped_openings} for bytes that open a longer routine of "
        f"another name)",
        flush=True,
    )
    for example in result.conflict_examples[:5]:
        print(f"  ambiguous: {', '.join(example)[:200]}", flush=True)


def build(args: argparse.Namespace) -> int:
    assets = load_assets(args.assets, args.asset)
    by_directory: dict[Path, dict[Path, list[Asset]]] = {}
    for asset in assets:
        by_directory.setdefault(asset.directory, {}).setdefault(asset.output, []).append(asset)

    work_root = Path(tempfile.mkdtemp(prefix="neverd-msvc-sigs-", dir=args.work))
    try:
        for _, outputs in sorted(by_directory.items()):
            results: dict[Path, FoldResult] = {}
            sources: dict[Path, list[dict]] = {}
            for relative, members in sorted(outputs.items()):
                generated, sources[relative] = generate(args, members, work_root)
                results[relative] = fold(generated)
                del generated
            settle_directory(results)
            for relative, result in sorted(results.items()):
                write(args, relative, result, sources[relative])
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
