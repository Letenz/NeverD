#!/usr/bin/env python3
"""Isolate the MSVC 19.44 CallRegisterEffects C1001 on disposable CI source."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def baseline_source(text, newline):
    if "// MSVC 19.44 crashes when these helper types are function-local." in text:
        first = text.index("// MSVC 19.44 crashes when these helper types are function-local.")
        last = text.index("/// The stores of", first)
        types = text[text.index("struct Slot {", first):last].rstrip()
        types = newline.join("  " + line for line in types.splitlines()) + newline
        comment = ("  // An address as a base register family, how often that family had been\n"
                   "  // written when the address was formed, and a displacement.\n").replace("\n", newline)
        text = text[:first] + text[last:]
        at = text.index("  std::array<unsigned, kX64GPRBytes / 8> Versions")
        text = text[:at] + comment + types + text[at:]
    mapped = "std::map<size_t, llvm::ArrayRef<uint64_t>>"
    span_mapped = "std::map<size_t, std::span<const uint64_t>>"
    if text.count(mapped) == 0 and text.count(span_mapped) == 3:
        text = text.replace(span_mapped, mapped).replace("#include <span>" + newline, "")
        rest = ("const llvm::ArrayRef<uint64_t> Rest =\n"
                "        llvm::ArrayRef(kLinuxX64SyscallArgumentRegisters).drop_front(*Count);").replace("\n", newline)
        text, count = re.subn(
            r"const std::span<const uint64_t> Rest =\s*std::span<const uint64_t>\(kLinuxX64SyscallArgumentRegisters\)\s*\.subspan\(\*Count\);",
            lambda _: rest, text)
        if count != 1:
            raise ValueError("Expected the exact standard-span register slice")
    return text


def variants(text, newline):
    text = baseline_source(text, newline)
    mapped = "std::map<size_t, llvm::ArrayRef<uint64_t>>"
    marker = mapped + newline + "unreadSyscallArguments"
    begin = text.index(marker)
    end = text.index("/// The scalar writes", begin)
    prefix = text[:text.index("namespace neverd {")]
    close = newline + "} // namespace" + newline + "} // namespace neverd" + newline
    save_begin = text.index("std::set<std::pair<size_t, size_t>>")
    save_end = text.index("/// The byte and word writes", save_begin)
    masked_begin = text.index("void findMaskedMerges")
    masked_end = text.index("/// The argument registers", masked_begin)

    def stub(start, stop, result):
        signature_end = text.index("{", start, stop)
        return text[start:signature_end] + "{ " + result + " }" + newline + newline

    body = text[save_begin:save_end]
    structures_begin = body.index("  struct Slot {")
    structures_end = body.index("  std::array<unsigned", structures_begin)
    loop_begin = body.index("  for (const size_t BI : Prologue)")
    loop_end = body.index("  if (!ReadsVectorCount)", loop_begin)
    prologue_begin = body.index("  if (Entry.Succs.size() == 2)")
    prologue_end = body.index("  // An address", prologue_begin)
    slot_begin = body.index("  auto SlotOf =")
    slot_end = body.index("  // The temporaries", slot_begin)
    entry_begin = body.index("  auto EntryValueAt =")
    entry_end = body.index("  const std::optional<unsigned> VectorCount", entry_begin)

    def whole(new_body):
        return text[:save_begin] + new_body + text[save_end:]

    moved_structures = whole(body[structures_begin:structures_end] +
                             body[:structures_begin] + body[structures_end:])
    renamed_slot = whole(body.replace("auto Slot = SlotOf(Address)", "auto AddressSlot = SlotOf(Address)").replace("Slot->", "AddressSlot->"))
    explicit_optional = whole(body.replace("if (const auto Family = V.isReg()", "if (const std::optional<unsigned> Family = V.isReg()"))
    prologue = body[prologue_begin:prologue_end]
    choice_body = prologue.replace("    if (auto A = IndexOf(Entry.Succs[0]), B = IndexOf(Entry.Succs[1]); A && B)",
        "    if (auto A = IndexOf(Entry.Succs[0]), B = IndexOf(Entry.Succs[1]); A && B) {")
    choice_body = choice_body.replace("      for (const auto &[Around, Join] :" + newline + "           {std::pair{*A, *B}, std::pair{*B, *A}}) {",
        "      const std::pair<size_t, size_t> Choices[]{{*A, *B}, {*B, *A}};" + newline +
        "      for (const auto &[Around, Join] : Choices) {") + "    }" + newline
    explicit_choices = whole(body[:prologue_begin] + choice_body + body[prologue_end:])
    constants = text[text.index("constexpr uint64_t kX64GPRBytes"):text.index("GPRFamilyMask familyBit")]
    isolated = prefix + newline + "namespace neverd { namespace {" + newline + constants + body + close
    return [
        ("original-o2", text, ""),
        ("isolated-save-area", isolated, ""),
        ("save-area-without-prologue", whole(body[:prologue_begin] + body[prologue_end:]), ""),
        ("save-area-without-scan", whole(body[:loop_begin] + body[loop_end:]), ""),
        ("save-area-without-final-selection", whole(body[:loop_end] + "  return Spills;" + newline + "}" + newline), ""),
        ("save-area-without-slot-lambda", whole(body[:slot_begin] + "  auto SlotOf = [&](const NdVar &) -> std::optional<Slot> { return std::nullopt; };" + newline + body[slot_end:]), ""),
        ("save-area-without-entry-value-lambda", whole(body[:entry_begin] + "  auto EntryValueAt = [&](const NdVar &) -> std::optional<unsigned> { return std::nullopt; };" + newline + body[entry_end:]), ""),
        ("save-area-namespace-structures", moved_structures, ""),
        ("save-area-renamed-shadow", renamed_slot, ""),
        ("save-area-explicit-optional", explicit_optional, ""),
        ("save-area-explicit-choices", explicit_choices, ""),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    args = parser.parse_args()
    if os.name != "nt":
        raise SystemExit("This probe requires the native Windows MSVC environment")
    build = args.build.resolve()
    target = "lib/ir/low/CMakeFiles/NeverDIRLow.dir/CallRegisterEffects.cpp.obj"
    commands = subprocess.check_output(
        ["ninja", "-C", str(build), "-t", "commands", target], text=True)
    command = commands.strip().splitlines()[-1]
    if "cl.exe" not in command or "CallRegisterEffects.cpp" not in command:
        raise ValueError("Expected the configured production MSVC command")
    source = Path("lib/ir/low/CallRegisterEffects.cpp").resolve()
    if str(source).replace("\\", "/").lower() not in command.replace("\\", "/").lower():
        raise ValueError("Compiler command does not consume the isolated source path")
    original = source.read_bytes()
    text = original.decode("utf-8")
    newline = "\r\n" if b"\r\n" in original else "\n"
    results = []
    try:
        for name, body, options in variants(text, newline):
            source.write_bytes(body.encode("utf-8"))
            compile_command, replacements = re.subn(
                r'/Fo(?:"[^"]*"|\S+)', "/Fo" + name + ".obj", command, count=1)
            if replacements != 1:
                raise ValueError("Expected one MSVC output object")
            process = subprocess.run(compile_command + options, cwd=build,
                                     shell=True, capture_output=True)
            output = process.stdout + process.stderr
            (build / (name + ".log")).write_bytes(output)
            row = {"variant": name, "returncode": process.returncode,
                   "c1001": b"C1001" in output,
                   "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                   "object_written": (build / (name + ".obj")).is_file()}
            results.append(row)
            print(json.dumps(row), flush=True)
    finally:
        source.write_bytes(original)
        (build / "compiler-probe.json").write_text(
            json.dumps(results, indent=2) + "\n", encoding="utf-8")
    if not results or not any(row["returncode"] == 0 and row["object_written"] for row in results):
        raise SystemExit("No successful compilation; inspect the independent variant logs")


if __name__ == "__main__":
    main()
