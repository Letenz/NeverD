#!/usr/bin/env python3
"""Generate local Windows type libraries from Microsoft's SDK/WDK.

Microsoft's original headers, licenses and the generated declarations stay in
the selected local directory. The SDK/WDK is a build-time input only. Configure
NeverD with NEVERD_WINDOWS_TYPES_DIRECTORY pointing at the output directory.
No ReactOS, MinGW or phnt headers are used by this generator.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

import build_wdk_driver_fixtures as wdk


VERSION = "10.0.26100.0"
KERNEL_HEADERS = (
    "ntifs", "ndis", "storport", "scsiwmi", "usb", "usbdi", "usbdlib",
    "wmilib", "wdmsec", "hidusage", "hidpi", "bcrypt", "fltkernel",
    "netiodef", "fwpmk", "fwpsk",
)
USER_HEADERS = (
    "winsock2", "ws2tcpip", "windows", "winternl", "wininet", "shellapi",
    "shlobj", "shlwapi", "psapi", "tlhelp32", "dbghelp", "bcrypt", "ncrypt",
    "iphlpapi", "setupapi", "wintrust", "softpub", "userenv", "wtsapi32",
    "lm", "sddl", "aclapi", "ntsecapi", "commctrl", "urlmon", "winevt",
    "evntrace", "dwmapi", "uxtheme", "avrt", "mmsystem", "wlanapi",
    "bluetoothapis", "winldap", "winber", "winusb", "hidsdi", "hidpi",
    "virtdisk", "vss", "winspool", "imm", "sensapi", "ras", "rasdlg",
    "raserror", "rtutils", "powrprof", "sspi", "authz", "dpapi", "dhcpcsdk",
    "fwpmu", "fltuser",
)
LIBRARIES = {
    "WindowsKernel": (KERNEL_HEADERS, ["km", "shared", "km/crt", "um"],
                           ["-D_KERNEL_MODE=1", "-DNDIS680"]),
    # WDK's CRT supplies the Microsoft C runtime declarations required by the
    # SDK. It is the same pinned Microsoft package, never a host CRT or MinGW.
    "WindowsUser": (USER_HEADERS, ["um", "shared", "km/crt"],
                         ["-DSECURITY_WIN32=1"]),
}
TARGETS = {
    "x64": ("X64", "x86_64-pc-windows-msvc", "LLP64", "_AMD64_"),
    "x86": ("X86", "i686-pc-windows-msvc", "ILP32", "_X86_"),
    "arm64": ("ARM64", "aarch64-pc-windows-msvc", "LLP64", "_ARM64_"),
}


def run(command: list[str], log: Path, output: Path | None = None) -> str:
    with log.open("w", encoding="utf-8") as errors:
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=errors, timeout=wdk.LIMITS["command_timeout"])
    if result.returncode:
        raise ValueError(f"command failed ({result.returncode}); see {log}")
    if output:
        output.write_text(result.stdout, encoding="utf-8")
    return result.stdout


def checked_types(neverd: str, source: Path, log: Path,
                  model: str = "LLP64") -> dict:
    result = json.loads(run([neverd, "types", "--data-model", model,
                             "--record-abi", "microsoft", "--declare",
                             str(source), "--keep-going", "--json"], log))
    if result["problems"]:
        log.write_text(json.dumps(result["problems"], indent=2) + "\n",
                       encoding="utf-8")
        raise ValueError(f"official declarations were refused; see {log}")
    return result


def record_layouts(text: str) -> dict:
    """Named record sizes, alignment and direct named fields from Clang."""
    records, current = {}, None
    for line in text.splitlines():
        if line.startswith("*** Dumping"):
            current = None
        heading = re.fullmatch(r"\s*0 \| (?:struct|union) (\w+)", line)
        if heading:
            current = {"fields": {}}
            records[heading[1]] = current
        elif current is not None:
            size = re.search(r"\[sizeof=(\d+), align=(\d+)\]", line)
            field = re.fullmatch(r"\s*(\d+(?::\d+-\d+)?) \|   (\S.*)", line)
            if size:
                current.update(size=int(size[1]), alignment=int(size[2]))
            elif field:
                name = re.search(r"\b([A-Za-z_]\w*)(?:\[[^]]*\])*$", field[2])
                if name:
                    current["fields"][name[1]] = field[1]
    return {name: record for name, record in records.items() if "size" in record}


def verify_layouts(original: str, generated: str) -> int:
    source, rendered = record_layouts(original), record_layouts(generated)
    shared = source.keys() & rendered.keys()
    if not shared:
        raise ValueError("Clang produced no shared named record layouts")
    for name in sorted(shared):
        a, b = source[name], rendered[name]
        fields = a["fields"].keys() & b["fields"].keys()
        if (a["size"] != b["size"] or a["alignment"] != b["alignment"] or
                any(a["fields"][field] != b["fields"][field] for field in fields)):
            raise ValueError(f"official record layout changed: {name}: {a} != {b}")
    return len(shared)


def include_headers(headers: tuple[str, ...]) -> str:
    lines = []
    for header in headers:
        if header == "ntsecapi":
            # winternl already supplied NTSTATUS, STRING and UNICODE_STRING.
            # Use ntsecapi's own guard against its alternative LSA aliases;
            # restore the macro before any subsequent provider is read.
            lines += ['#pragma push_macro("_NTDEF_")', '#define _NTDEF_ 1']
        lines.append(f"#include <{header}.h>")
        if header == "ntsecapi":
            lines.append('#pragma pop_macro("_NTDEF_")')
    return "\n".join(lines) + "\n"


def compiler_defaults(kit: Path, architecture: str, library: str) -> tuple[list[str], str]:
    """Keep the WDK's compiler default, which preprocessing alone discards."""
    if architecture != "x86" or library != "WindowsKernel":
        return [], ""
    source = kit / f"c/build/{VERSION}/WindowsDriver.Common.props"
    properties = ET.parse(source).getroot()
    conventions = [item.text for item in properties.findall(
        "./{*}ItemDefinitionGroup/{*}ClCompile/{*}CallingConvention")]
    if conventions != ["StdCall"]:
        raise ValueError(f"unrecognized WDK default calling convention in {source}")
    return ["-Xclang", "-fdefault-calling-conv=stdcall"], "__stdcall"


def combine_targets(output: Path, directories: list[Path]) -> None:
    """Publish all target libraries only after every input has been verified."""
    output.mkdir(parents=True, exist_ok=True)
    manifest = output / "manifest.json"
    manifest.unlink(missing_ok=True)
    targets, libraries, seen = [], [], set()
    common = None
    for directory in directories:
        target = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
        arch = target.get("architecture")
        if (target.get("schema") != 1 or target.get("provider") != "Microsoft SDK/WDK"
                or arch not in TARGETS or arch in seen):
            raise ValueError("invalid or duplicate Microsoft target manifest")
        seen.add(arch)
        expected = {base + TARGETS[arch][0] for base in LIBRARIES}
        actual = [library["name"] for library in target["libraries"]]
        if len(actual) != len(expected) or set(actual) != expected:
            raise ValueError(f"incomplete Microsoft target libraries: {arch}")
        identity = {key: target[key] for key in ["compiler", "packages", "licenses"]}
        if common is not None and identity != common:
            raise ValueError("Microsoft targets use different compilers, packages or licenses")
        common = identity
        for library in target["libraries"]:
            header = directory / (library["name"] + ".h")
            if wdk.digest(header) != library["sha256"]:
                raise ValueError(f"changed Microsoft target header: {header}")
            libraries.append(dict(library, architecture=arch))
        for notice in target["licenses"]:
            name = notice["file"]
            if Path(name).name != name or wdk.digest(directory / "licenses" / name) != notice["sha256"]:
                raise ValueError("changed Microsoft target license")
        targets.append((directory, target))
    if seen != set(TARGETS):
        raise ValueError("all x86, x64 and ARM64 Microsoft target libraries are required")
    for directory, target in targets:
        for library in target["libraries"]:
            name = library["name"] + ".h"
            shutil.copy2(directory / name, output / name)
    (output / "licenses").mkdir(exist_ok=True)
    for notice in common["licenses"]:
        shutil.copy2(targets[0][0] / "licenses" / notice["file"],
                     output / "licenses" / notice["file"])
    record = dict(common, provider="Microsoft SDK/WDK", schema=1,
                  architecture="all", libraries=libraries,
                  targets=[target for _, target in targets])
    manifest.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def generate(output: Path, cache: Path, neverd: str, clang: str,
             architecture: str = "arm64") -> None:
    if architecture == "all":
        output.mkdir(parents=True, exist_ok=True)
        (output / "manifest.json").unlink(missing_ok=True)
        directories = []
        for arch in TARGETS:
            directory = output / arch
            generate(directory, cache, neverd, clang, arch)
            directories.append(directory)
        combine_targets(output, directories)
        return
    suffix, triple, model, macro = TARGETS[architecture]
    output.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    manifest = output / "manifest.json"
    # A failed update must never certify earlier or partially replaced inputs.
    manifest.unlink(missing_ok=True)
    inventory = wdk.declarations()
    packages = [wdk.download(cache, *record) for record in inventory["DOWNLOAD"]]
    kit = output / "microsoft-kit"
    selected = [f"c/Include/{VERSION}/{part}/"
                for part in ("km", "shared", "um")]
    selected += ["LICENSE.txt", "Microsoft.Windows.WDK.x64.nuspec",
                 "Microsoft.Windows.SDK.CPP.nuspec",
                 f"c/build/{VERSION}/WindowsDriver.Common.props"]
    inputs = wdk.extract(packages, kit, selected)
    licenses = output / "licenses"
    licenses.mkdir(exist_ok=True)
    (licenses / "WDK-LICENSE.txt").write_bytes((kit / "LICENSE.txt").read_bytes())
    for record in inventory["LICENSE"]:
        source = wdk.download(cache, *record)
        (licenses / source.name).write_bytes(source.read_bytes())
    overlay = output / "headers-vfs.json"
    overlay.write_text(json.dumps({
        "version": 0, "case-sensitive": False,
        "roots": [wdk.overlay_directory(kit / "c/Include", True)],
    }, indent=2) + "\n", encoding="utf-8")
    include = kit / f"c/Include/{VERSION}"
    # The WDK package's x64 name identifies its host tools/libraries. Its
    # architecture-conditional headers are compiled for the requested target;
    # no host architecture definition or structure is substituted.
    common = [clang, "--target=" + triple, "-ffreestanding",
              "-fms-extensions", "-fms-compatibility", "-Wno-invalid-token-paste",
              "-Wno-ignored-pragma-intrinsic", "-Wno-ignored-attributes",
              "-Wno-nonportable-include-path", "-Wno-pragma-pack",
              "-Wno-microsoft-anon-tag", "-D" + macro + "=1",
              "-D_WIN32_WINNT=0x0A00", "-DNTDDI_VERSION=0x0A00000C",
              "-ivfsoverlay", str(overlay)]
    built = []
    for base, (headers, directories, defines) in LIBRARIES.items():
        name = base + suffix
        source = output / (name + ".c")
        source.write_text(include_headers(headers), encoding="utf-8")
        defaults, convention = compiler_defaults(kit, architecture, base)
        arguments = common + defines + defaults + [item for part in directories
                                        for item in ("-I", str(include / part))]
        preprocessed = output / (name + ".pre.c")
        command = arguments + ["-E", str(source)]
        run(command, output / (name + ".pre.log"), preprocessed)
        if convention:
            preprocessed.write_text(
                f"#pragma neverd_default_calling_convention({convention})\n" +
                preprocessed.read_text(encoding="utf-8"), encoding="utf-8")
        read = checked_types(neverd, preprocessed, output / (name + ".parse.log"), model)
        generated = output / (name + ".h")
        with tempfile.TemporaryDirectory(dir=output, prefix="render-") as scratch:
            temporary = Path(scratch) / generated.name
            render = [neverd, "types", "--data-model", model, "--record-abi",
                      "microsoft", "--declare", str(preprocessed), "--all",
                      "--prototypes", "--spelling", "windows", "--no-offsets",
                      "-o", str(temporary)]
            run(render, output / (name + ".render.log"))
            body = temporary.read_text(encoding="utf-8")
            temporary.write_text(
                "// Local declarations generated from Microsoft SDK/WDK.\n"
                "// Source packages, commands and hashes: manifest.json.\n"
                "// Original Microsoft notices: licenses/.\n" + body,
                encoding="utf-8")
            reread = checked_types(neverd, temporary, output / (name + ".roundtrip.log"), model)
            if (set(read["types"]) - set(reread["types"]) or
                    set(read["prototypes"]) - set(reread["prototypes"])):
                raise ValueError(f"declarations disappeared while rendering {name}")
            # Independently compare the actual Microsoft headers with the
            # emitted type declarations under the target compiler ABI. Function
            # prototypes are excluded: target CRT builtin spellings need not
            # equal the renderer's fixed-width aliases.
            layouts = output / (name + ".layouts.h")
            run([neverd, "types", "--data-model", model, "--record-abi",
                 "microsoft", "--declare", str(temporary), "--all", "--asserts",
                 "--spelling", "windows", "--no-offsets", "-o", str(layouts)],
                output / (name + ".layouts-render.log"))
            dump = ["-fsyntax-only", "-Xclang", "-fdump-record-layouts-complete"]
            original_layouts = run(arguments + dump + [str(source)],
                                   output / (name + ".official-clang.log"),
                                   output / (name + ".official-layouts.log"))
            generated_layouts = run(common + dump + ["-x", "c", str(layouts)],
                                    output / (name + ".generated-clang.log"),
                                    output / (name + ".generated-layouts.log"))
            count = verify_layouts(original_layouts, generated_layouts)
            temporary.replace(generated)
        built.append({"name": name, "sha256": wdk.digest(generated),
                      "types": len(read["types"]), "verified_records": count,
                      "default_calling_convention": convention,
                      "commands": [command, render]})
        print(f"Generated {name}: {body.splitlines()[0]}", flush=True)
    record = {
        "provider": "Microsoft SDK/WDK", "architecture": architecture,
        "schema": 1, "libraries": built,
        "compiler": subprocess.check_output([clang, "--version"], text=True),
        "packages": [{"file": name, "url": url, "sha256": digest}
                     for name, url, digest in inventory["DOWNLOAD"]],
        "inputs": inputs,
        "licenses": [{"file": path.name, "sha256": wdk.digest(path)}
                     for path in sorted(licenses.iterdir())],
    }
    manifest.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--neverd", required=True)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--architecture", choices=[*TARGETS, "all"], default="all")
    arguments = parser.parse_args()
    generate(arguments.output.resolve(), arguments.cache.resolve(),
             str(Path(arguments.neverd).resolve()), arguments.clang,
             arguments.architecture)


if __name__ == "__main__":
    main()
