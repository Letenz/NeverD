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
    fold,
    main,
    parse_line,
    settle_directory,
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


class FoldTests(unittest.TestCase):
    def test_identical_lines_fold(self) -> None:
        line = "AABBCCDD 00 0000 0004 :0000 f"
        result = fold([line, line, line])
        self.assertEqual(result.texts, [line])
        self.assertEqual(result.duplicates, 2)

    def test_same_bytes_under_different_names_are_all_dropped(self) -> None:
        result = fold(
            [
                "AABBCCDD 00 0000 0004 :0000 ??$size@H@vector@@QEBA_KXZ",
                "11223344 00 0000 0004 :0000 kept",
                "AABBCCDD 00 0000 0004 :0000 ??$size@I@vector@@QEBA_KXZ",
            ]
        )
        self.assertEqual(result.texts, ["11223344 00 0000 0004 :0000 kept"])
        self.assertEqual(result.conflicting_groups, 1)
        self.assertEqual(result.conflicting_lines, 2)
        self.assertEqual(result.ambiguous_keys, {"AABBCCDD 00 0000 0004"})

    def test_names_that_differ_only_in_spelling_are_still_different_names(self) -> None:
        # The loader compares names exactly, so the fold does too.
        result = fold(
            [
                "AABBCCDD 00 0000 0004 :0000 _Close_CFile__UEAAXXZ",
                "AABBCCDD 00 0000 0004 :0000 ?Close@CFile@@UEAAXXZ",
            ]
        )
        self.assertEqual(result.texts, [])
        self.assertEqual(result.conflicting_groups, 1)

    def test_output_is_sorted(self) -> None:
        result = fold(["BB 00 0000 0004 :0000 b", "AA 00 0000 0004 :0000 a"])
        self.assertEqual(result.texts, ["AA 00 0000 0004 :0000 a", "BB 00 0000 0004 :0000 b"])


class SettleDirectoryTests(unittest.TestCase):
    def test_bytes_one_file_found_ambiguous_are_dropped_from_the_others(self) -> None:
        # The debug CRT's wrapper is unique among the SDK's libraries, but the
        # toolset's libraries hold the same bytes under several names.
        toolset = fold(
            [
                "AABBCCDD 00 0000 0004 :0000 ??$_Allocate_at_least_helper@D@std@@YAXXZ",
                "AABBCCDD 00 0000 0004 :0000 ??$_Allocate_at_least_helper@H@std@@YAXXZ",
                "11223344 00 0000 0004 :0000 memcpy",
            ]
        )
        sdk = fold(
            [
                "AABBCCDD 00 0000 0004 :0000 ??$set_environment_variable@D@@YAXXZ",
                "55667788 00 0000 0004 :0000 _wsetenv",
            ]
        )
        results = {Path("vs2026.pat"): toolset, Path("winsdk.pat"): sdk}
        settle_directory(results)
        self.assertEqual(toolset.texts, ["11223344 00 0000 0004 :0000 memcpy"])
        self.assertEqual(sdk.texts, ["55667788 00 0000 0004 :0000 _wsetenv"])
        self.assertEqual(toolset.dropped_across_files, 0)
        self.assertEqual(sdk.dropped_across_files, 1)

    def test_bytes_two_files_name_differently_are_dropped_from_both(self) -> None:
        older = fold(["AABBCCDD 00 0000 0004 :0000 _fseeki64_nolock"])
        newer = fold(["AABBCCDD 00 0000 0004 :0000 _fseeki64"])
        settle_directory({Path("vs2017.pat"): older, Path("vs2026.pat"): newer})
        self.assertEqual(older.texts, [])
        self.assertEqual(newer.texts, [])
        self.assertEqual(older.dropped_across_files, 1)
        self.assertEqual(newer.dropped_across_files, 1)

    def test_a_claim_several_files_repeat_under_one_name_is_kept(self) -> None:
        line = "11223344 00 0000 0004 :0000 memcpy"
        older, newer = fold([line]), fold([line])
        settle_directory({Path("vs2022.pat"): older, Path("vs2026.pat"): newer})
        self.assertEqual(older.texts, [line])
        self.assertEqual(newer.texts, [line])


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

    def test_servicing_toolsets_share_a_file_that_is_rebuilt_from_them(self) -> None:
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
        self._asset(
            "winsdk-10.0.26100.0-arm64",
            {"kind": "winsdk", "arch": "arm64", "windows_sdk_version": "10.0.26100.0"},
            {"winsdk/ucrt/arm64/ucrt64.lib": b"u", "winsdk/um/arm64/kernel32.lib": b"k"},
        )
        existing = self.root / "sigs/pe/arm/64/vs2026.pat"
        existing.parent.mkdir(parents=True)
        existing.write_text("01020304 00 0000 0004 :0000 imported_from_elsewhere\n")

        report = self._run()

        # The earlier line is gone: the file holds what its assets say.
        # libcmt and chkstk make the same byte claim, as do the two folded
        # template instantiations, so both groups are ambiguous and dropped.
        self.assertEqual(
            existing.read_text().splitlines(),
            ["AA07CCDD 00 0000 0004 :0000 arm64_libcpmt"],
        )
        # ucrt64 makes the claim that was ambiguous in vs2026.pat, so the
        # directory drops it from winsdk.pat as well.
        self.assertEqual(
            (self.root / "sigs/pe/arm/64/winsdk.pat").read_text().splitlines(),
            ["AA08CCDD 00 0000 0004 :0000 arm64_kernel32"],
        )
        self.assertIn("ambiguous groups dropped", report)
        self.assertIn("1 dropped for bytes another file of pe/arm/64 names differently", report)

        provenance = json.loads(existing.with_suffix(".sources.json").read_text())
        self.assertEqual(provenance["file"], "pe/arm/64/vs2026.pat")
        self.assertEqual(provenance["generator"]["neverd_ref"], "abc123")
        self.assertEqual(provenance["lines"], 1)
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
