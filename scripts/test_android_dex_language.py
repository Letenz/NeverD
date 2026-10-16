#!/usr/bin/env python3
"""Independent JVM/D8 oracle for shared strings, catch/rethrow and dispatch."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
from test_android_dex_runtime import run


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build", type=Path, required=True)
    p.add_argument("--jdk", type=Path, required=True)
    p.add_argument("--d8", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--backend", choices=("unicorn", "hvf"), default="unicorn")
    args = p.parse_args()
    root = Path(__file__).resolve().parent.parent
    source = root / "scripts/tests/fixtures/dalvik-language"
    args.output.mkdir(parents=True, exist_ok=False)
    classes, dex_dir = args.output / "classes", args.output / "dex"
    classes.mkdir()
    dex_dir.mkdir()
    java = args.jdk / "bin/java"
    run(
        [
            args.jdk / "bin/javac",
            "--release",
            "8",
            "-g",
            "-d",
            classes,
            *sorted(source.glob("*.java")),
        ]
    )
    run(
        [
            java,
            "-cp",
            args.d8,
            "com.android.tools.r8.D8",
            "--debug",
            "--min-api",
            "28",
            "--output",
            dex_dir,
            *sorted(
                p for p in classes.rglob("*.class") if not p.stem.endswith("Oracle")
            ),
        ]
    )
    sequence = run([java, "-cp", classes, "runtime.language.SequenceOracle"]).stdout
    (args.output / "sequence.oracle.txt").write_text(sequence)
    dex = (dex_dir / "classes.dex").read_bytes().hex()
    (fixture,) = args.build.rglob("jni-O2-relr.so")
    probe = args.build / "bin/neverd-process-probe"
    records = []
    inputs = [
        *sorted(source.glob("*.java")),
        dex_dir / "classes.dex",
        probe,
        fixture,
        Path(__file__),
        root / "unittests/emulation/fixtures/android_jni.c",
    ]
    identities = {
        str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs
    }
    revision = run(["git", "-C", root, "rev-parse", "HEAD"]).stdout.strip()
    dirty = bool(run(["git", "-C", root, "status", "--porcelain"]).stdout)

    def execute(mode, seed, form, expected, name="run", stop="returned"):
        label = f"{name}-{mode}-{seed}-{form}"
        data = bytearray(512)
        for at, text in ((0, "runtime/language/Language"), (64, name), (128, "(II)J")):
            b = text.encode() + b"\0"
            data[at : at + len(b)] = b
        struct.pack_into("<QQ", data, 256, mode & 0xFFFFFFFF, seed & 0xFFFFFFFF)
        address = 0x65000000
        options = {
            "backend": args.backend,
            "instruction_limit": 300000,
            "timeout_microseconds": 10000000,
            "android": {
                "entry_symbol": "jni_dex_call",
                "initialize": False,
                "jni": True,
                "dex_images": [dex],
                "arguments": [
                    address,
                    address + 64,
                    address + 128,
                    address + 256,
                    1,
                    form,
                ],
                "memory": [{"address": address, "size": 4096, "bytes_hex": data.hex()}],
            },
        }
        request = args.output / f"{label}.request.json"
        report = args.output / f"{label}.report.json"
        request.write_text(json.dumps(options))
        run([probe, fixture, "android-aarch64-api28-v1", request, report])
        result = json.loads(report.read_text())
        if result["stop_reason"] != stop or (
            stop == "returned" and int(result["return_value"], 16) != expected
        ):
            raise AssertionError(
                (
                    label,
                    expected,
                    result["stop_reason"],
                    result.get("return_value"),
                    result.get("diagnostic"),
                )
            )
        records.append(
            {"case": label, "stop": stop, "value": result.get("return_value")}
        )

    for mode in range(22):
        for seed in (0, 1, -1, -2147483648, 2147483647):
            oracle = run(
                [java, "-cp", classes, "runtime.language.LanguageOracle", mode, seed]
            ).stdout
            (args.output / f"{mode}-{seed}.oracle.txt").write_text(oracle)
            for form in range(3):
                execute(mode, seed, form, int(oracle.strip(), 16))
    for name in ("unknown", "unsupportedCaught"):
        execute(0, 1, 2, None, name, "unsupported_service")
    for seed in (0, 1, -1, -2147483648, 2147483647):
        oracle = run(
            [java, "-cp", classes, "runtime.language.BridgeOracle", seed]
        ).stdout
        (args.output / f"bridge-{seed}.oracle.txt").write_text(oracle)
        expected = [int(line, 16) for line in oracle.splitlines()]
        for form in range(3):
            label = f"bridge-{seed}-{form}"
            address = 0x65000000
            options = {
                "backend": args.backend,
                "instruction_limit": 300000,
                "timeout_microseconds": 10000000,
                "android": {
                    "entry_symbol": "jni_dex_language",
                    "initialize": False,
                    "jni": True,
                    "dex_images": [dex],
                    "arguments": [seed & 0xFFFFFFFF, form, address],
                    "memory": [{"address": address, "size": 4096}],
                    "read_memory": [{"address": address, "size": 256}],
                },
            }
            request = args.output / f"{label}.request.json"
            report = args.output / f"{label}.report.json"
            request.write_text(json.dumps(options))
            run([probe, fixture, "android-aarch64-api28-v1", request, report])
            result = json.loads(report.read_text())
            if result["stop_reason"] != "returned" or int(result["return_value"], 16):
                raise AssertionError(
                    (
                        label,
                        result["stop_reason"],
                        result.get("return_value"),
                        result.get("diagnostic"),
                    )
                )
            actual = list(
                struct.unpack(
                    "<32Q", bytes.fromhex(result["android"]["memory"][0]["bytes_hex"])
                )
            )
            if actual != expected:
                raise AssertionError(
                    (
                        label,
                        [
                            (i, a, b)
                            for i, (a, b) in enumerate(zip(actual, expected))
                            if a != b
                        ],
                    )
                )
            records.append({"case": label, "stop": "returned", "words": 32})
    options["android"]["arguments"] = [1, 3, address]
    request = args.output / "nonvirtual-mismatch.request.json"
    report = args.output / "nonvirtual-mismatch.report.json"
    request.write_text(json.dumps(options))
    run([probe, fixture, "android-aarch64-api28-v1", request, report])
    result = json.loads(report.read_text())
    if result[
        "stop_reason"
    ] != "runtime_failure" or "nonvirtual intrinsic ID" not in result.get(
        "diagnostic", ""
    ):
        raise AssertionError(("nonvirtual-mismatch", result))
    records.append({"case": "nonvirtual-mismatch", "stop": "runtime_failure"})
    unchanged = all(
        hashlib.sha256(path.read_bytes()).hexdigest() == identities[str(path)]
        for path in inputs
    )
    if (
        not unchanged
        or revision != run(["git", "-C", root, "rev-parse", "HEAD"]).stdout.strip()
    ):
        raise AssertionError("inputs or source revision changed during execution")
    summary = {
        "backend": args.backend,
        "runs": len(records),
        "oracle_values": 810,
        "records": records,
        "inputs_sha256": identities,
        "inputs_unchanged": unchanged,
        "source_revision": revision,
        "source_dirty": dirty,
    }
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps({k: summary[k] for k in ("backend", "runs", "oracle_values")}))


if __name__ == "__main__":
    main()
