"""Tests for the MSVC and Windows SDK signature build driver."""

from __future__ import annotations

import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import textwrap
import unittest
from contextlib import redirect_stdout
from pathlib import Path

from scripts.signatures.build_msvc_signatures import (
    Asset,
    BuildError,
    main,
    merge,
    merge_provenance,
    parse_line,
)


class ParseLineTests(unittest.TestCase):
    def test_key_is_everything_but_the_names(self) -> None:
        line = parse_line("4883EC28 00 0000 0010 :0000 ?Run@Task@@QEAAXXZ 4883C428C3")
        assert line is not None
        self.assertEqual(line.key, "4883EC28 00 0000 0010 4883C428C3")
        self.assertEqual(line.names, (":0000 ?Run@Task@@QEAAXXZ",))

    def test_every_public_name_is_kept_in_order(self) -> None:
        line = parse_line("AABB 00 0000 0020 :0000 first :0010 second")
        assert line is not None
        self.assertEqual(line.names, (":0000 first", ":0010 second"))
        self.assertEqual(line.key, "AABB 00 0000 0020")

    def test_separators_and_comments_are_not_lines(self) -> None:
        for text in ("", "---", "; comment", "# comment"):
            self.assertIsNone(parse_line(text))

    def test_line_without_a_name_is_an_error(self) -> None:
        with self.assertRaises(BuildError):
            parse_line("AABB 00 0000 0002 CCDD EEFF")


class MergeTests(unittest.TestCase):
    def test_identical_lines_fold(self) -> None:
        line = "AABBCCDD 00 0000 0004 :0000 f"
        result = merge([line], [line, line])
        self.assertEqual(result.lines, [line])
        self.assertEqual(result.duplicates, 2)
        self.assertEqual(result.kept_existing, 1)
        self.assertEqual(result.added, 0)

    def test_same_bytes_under_different_names_are_dropped_on_both_sides(self) -> None:
        existing = [
            "AABBCCDD 00 0000 0004 :0000 ??$size@H@vector@@QEBA_KXZ",
            "11223344 00 0000 0004 :0000 kept",
        ]
        generated = [
            "AABBCCDD 00 0000 0004 :0000 ??$size@I@vector@@QEBA_KXZ",
            "55667788 00 0000 0004 :0000 added",
        ]
        result = merge(existing, generated)
        self.assertEqual(
            result.lines,
            ["11223344 00 0000 0004 :0000 kept", "55667788 00 0000 0004 :0000 added"],
        )
        self.assertEqual(result.conflicting_groups, 1)
        self.assertEqual(result.conflicting_lines, 2)
        self.assertEqual(result.kept_existing, 1)
        self.assertEqual(result.added, 1)

    def test_names_that_differ_only_in_spelling_are_still_different_names(self) -> None:
        # The loader compares names exactly, so the merge does too.
        result = merge(
            ["AABBCCDD 00 0000 0004 :0000 _Close_CFile__UEAAXXZ"],
            ["AABBCCDD 00 0000 0004 :0000 ?Close@CFile@@UEAAXXZ"],
        )
        self.assertEqual(result.lines, [])
        self.assertEqual(result.conflicting_groups, 1)

    def test_output_is_sorted(self) -> None:
        result = merge([], ["BB 00 0000 0004 :0000 b", "AA 00 0000 0004 :0000 a"])
        self.assertEqual(result.lines, ["AA 00 0000 0004 :0000 a", "BB 00 0000 0004 :0000 b"])


class AssetTests(unittest.TestCase):
    def _asset(self, manifest: dict) -> Asset:
        return Asset(manifest["asset"], manifest, Path("unused.tar.zst"))

    def test_toolsets_are_filed_by_year_and_architecture(self) -> None:
        asset = self._asset(
            {
                "asset": "vs2026-14.50.35717-arm64",
                "kind": "toolset",
                "arch": "arm64",
                "visual_studio": {"year": 2026},
                "toolset_version": "14.50.35717",
                "archive": {"sha256": "ab"},
            }
        )
        self.assertEqual(asset.output, Path("pe/arm/64/vs2026.pat"))
        self.assertEqual(asset.machine, "arm64")
        self.assertEqual(asset.provenance()["toolset_version"], "14.50.35717")

    def test_sdks_share_one_file_per_architecture(self) -> None:
        for arch, expected in (
            ("x86", "pe/x86/32/winsdk.pat"),
            ("x64", "pe/x86/64/winsdk.pat"),
            ("arm", "pe/arm/32/winsdk.pat"),
        ):
            asset = self._asset(
                {"asset": f"winsdk-10.0.22621.0-{arch}", "kind": "winsdk", "arch": arch}
            )
            self.assertEqual(asset.output, Path(expected))

    def test_unknown_kind_is_an_error(self) -> None:
        asset = self._asset({"asset": "x", "kind": "driver-kit", "arch": "x64"})
        with self.assertRaises(BuildError):
            _ = asset.output


class ProvenanceTests(unittest.TestCase):
    def test_sources_accumulate_across_regenerations(self) -> None:
        previous = {"sources": [{"asset": "vs2026-14.50.1-x64", "archive_sha256": "aa"}]}
        update = {
            "file": "pe/x86/64/vs2026.pat",
            "sources": [
                {"asset": "vs2026-14.51.2-x64", "archive_sha256": "bb"},
                {"asset": "vs2026-14.50.1-x64", "archive_sha256": "aa"},
            ],
        }
        merged = merge_provenance(previous, update)
        self.assertEqual(
            [entry["asset"] for entry in merged["sources"]],
            ["vs2026-14.50.1-x64", "vs2026-14.51.2-x64"],
        )


FAKE_SIGMAKER = textwrap.dedent(
    """\
    #!{python}
    import sys
    from pathlib import Path

    args = sys.argv[1:]
    if args[0] == "--verify":
        text = Path(args[1]).read_text()
        sys.exit(0 if text.strip() else 1)
    output = Path(args[args.index("-o") + 1])
    machine = args[args.index("--machine") + 1]
    libraries = [a for a in args[: args.index("-o")]]
    lines = []
    for library in libraries:
        stem = Path(library).stem
        lines.append(f"AA{{len(stem):02X}}CCDD 00 0000 0004 :0000 {{machine}}_{{stem}}")
    lines.append("DEADBEEF 00 0000 0004 :0000 ??$fold@H@@YAXXZ")
    lines.append("DEADBEEF 00 0000 0004 :0000 ??$fold@I@@YAXXZ")
    output.write_text("\\n".join(lines) + "\\n")
    print(f"Generated {{len(lines)}} signatures")
    """
)


@unittest.skipUnless(os.name == "posix" and shutil.which("zstd"), "needs POSIX and zstd")
class BuildTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory()
        self.root = Path(self._temporary.name)
        self.sigmaker = self.root / "neverd-sigmaker"
        self.sigmaker.write_text(FAKE_SIGMAKER.format(python=sys.executable))
        self.sigmaker.chmod(0o755)
        self.assets = self.root / "assets"
        self.assets.mkdir()

    def tearDown(self) -> None:
        self._temporary.cleanup()

    def _asset(self, name: str, manifest: dict, files: dict[str, bytes]) -> None:
        tar_path = self.assets / f"{name}.tar"
        with tarfile.open(tar_path, "w") as archive:
            for member, data in files.items():
                info = tarfile.TarInfo(member)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
        subprocess.run(["zstd", "-q", "--rm", str(tar_path)], check=True)
        archive_path = self.assets / f"{name}.tar.zst"
        manifest = dict(manifest)
        manifest["asset"] = name
        manifest["archive"] = {
            "name": archive_path.name,
            "sha256": hashlib.sha256(archive_path.read_bytes()).hexdigest(),
        }
        manifest["files"] = [{"path": member} for member in files]
        (self.assets / f"{name}.json").write_text(json.dumps(manifest))

    def _run(self, *extra: str) -> str:
        out = io.StringIO()
        with redirect_stdout(out):
            status = main(
                [
                    "--sigmaker", str(self.sigmaker),
                    "--assets", str(self.assets),
                    "--output", str(self.root / "sigs"),
                    "--neverd-ref", "abc123",
                    "--release", "msvc-libs-test",
                    *extra,
                ]
            )
        self.assertEqual(status, 0, out.getvalue())
        return out.getvalue()

    def test_servicing_toolsets_share_a_file_and_existing_lines_survive(self) -> None:
        toolset = {"kind": "toolset", "arch": "arm64", "visual_studio": {"year": 2026}}
        self._asset(
            "vs2026-14.50.1-arm64",
            dict(toolset, toolset_version="14.50.1"),
            {"vc/lib/arm64/libcmt.lib": b"a", "vc/lib/arm64/notes.txt": b"n"},
        )
        self._asset(
            "vs2026-14.51.2-arm64",
            dict(toolset, toolset_version="14.51.2"),
            {"vc/lib/arm64/libcpmt.lib": b"b", "vc/lib/arm64/chkstk.obj": b"c"},
        )
        existing = self.root / "sigs/pe/arm/64/vs2026.pat"
        existing.parent.mkdir(parents=True)
        existing.write_text("01020304 00 0000 0004 :0000 older_servicing_build\n")

        report = self._run("--merge-existing")

        lines = existing.read_text().splitlines()
        # libcmt and chkstk make the same byte claim, as do the two folded
        # template instantiations: both groups are ambiguous and dropped.
        self.assertEqual(
            lines,
            [
                "01020304 00 0000 0004 :0000 older_servicing_build",
                "AA07CCDD 00 0000 0004 :0000 arm64_libcpmt",
            ],
        )
        self.assertIn("ambiguous groups dropped", report)

        provenance = json.loads(existing.with_suffix(".sources.json").read_text())
        self.assertEqual(provenance["file"], "pe/arm/64/vs2026.pat")
        self.assertEqual(provenance["generator"]["neverd_ref"], "abc123")
        self.assertEqual(
            [source["asset"] for source in provenance["sources"]],
            ["vs2026-14.50.1-arm64", "vs2026-14.51.2-arm64"],
        )

    def test_archive_that_disagrees_with_its_manifest_fails(self) -> None:
        self._asset(
            "winsdk-10.0.26100.0-x64",
            {"kind": "winsdk", "arch": "x64"},
            {"winsdk/10.0.26100.0/ucrt/x64/libucrt.lib": b"u"},
        )
        manifest_path = self.assets / "winsdk-10.0.26100.0-x64.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["files"].append({"path": "winsdk/10.0.26100.0/um/x64/missing.lib"})
        manifest_path.write_text(json.dumps(manifest))
        out = io.StringIO()
        with redirect_stdout(out):
            status = main(
                [
                    "--sigmaker", str(self.sigmaker),
                    "--assets", str(self.assets),
                    "--output", str(self.root / "sigs"),
                ]
            )
        self.assertEqual(status, 1)


if __name__ == "__main__":
    unittest.main()
