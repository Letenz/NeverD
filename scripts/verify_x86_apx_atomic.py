#!/usr/bin/env python3
"""Qualify NeverD APX exports with the independent native instruction oracle.

First run NeverDX86APXAtomicSourceTests with NEVERD_APX_ATOMIC_EXPORT_DIR.
--oracle names NeverC's tests/verify-apx-atomic-completion.py. Controlled
completions qualify transport and ordinary memory order, not physical
atomicity, cache coherence or CPU fault behavior.
"""
import argparse
import importlib.util
import itertools
import json
from pathlib import Path
import sys


def qualify_pipeline(oracle, obj, case, windows, legacy=False):
    """Compare the recovered return with original CMP/LAHF/SETO semantics."""
    binary = oracle.lief.parse(str(obj))
    text = oracle.text_section(binary)
    code = bytes(text.content)
    symbol = next(s for s in binary.symbols if s.name.lstrip("_") == "apx_pipeline")
    start = symbol.value - getattr(text, "virtual_address", 0)
    native = oracle.decode(code)
    assert len(native) == 1, (obj, native)
    at, insn = next(iter(native.items()))
    assert insn.get("legacy", False) == legacy, (obj, "encoding family changed")
    assert tuple(insn[k] for k in ("op", "bits", "segment", "cc")) == (
        case["operation"], case["bits"], case["segment"], case["condition"])
    # LowIR has already wrapped the effective offset. Its native operation
    # deliberately uses addr64; it must not add FS/GS bases in ordinary IR.
    assert insn["address"] == 64
    patched = bytearray(code)
    patched[at:insn["end"]] = b"\x90" * (insn["end"] - at)
    uc = oracle.unicorn.Uc(oracle.unicorn.UC_ARCH_X86, oracle.unicorn.UC_MODE_64)
    span = (len(code) + 2 * oracle.PAGE - 1) // oracle.PAGE * oracle.PAGE
    stop = oracle.CODE + span - 1
    uc.mem_map(oracle.CODE, span)
    uc.mem_write(oracle.CODE, bytes(patched))
    uc.mem_map(oracle.STACK, 4 * oracle.PAGE)
    reg = lambda name: getattr(oracle.regs, "UC_X86_REG_" + name)
    def boundary(machine, address, size, user):
        if address in (oracle.CODE + at, stop):
            machine.emu_stop()
    uc.hook_add(oracle.unicorn.UC_HOOK_CODE, boundary)
    offset = 0x8765432100000208 if case["address_bits"] == 64 else 0x208
    compare, add = 0x0123456789abcdef, 0xfedcba9876543210
    mask = (1 << case["bits"]) - 1
    cmp = compare & mask
    olds = (0, mask, 1 << (case["bits"] - 1), (1 << (case["bits"] - 1)) - 1,
            cmp, (cmp - 1) & mask, (cmp + 1) & mask)
    saved_names = ("RBX", "RBP", "R12", "R13", "R14", "R15") + (("RSI", "RDI") if windows else ())
    for old in olds:
        for i, name in enumerate(oracle.GPRS):
            uc.reg_write(reg(name), 0x2345678900000000 + i)
        saved = {name: uc.reg_read(reg(name)) for name in saved_names}
        sp = oracle.STACK + 2 * oracle.PAGE - 8
        uc.reg_write(reg("RSP"), sp)
        uc.mem_write(sp, oracle.struct.pack("<Q", stop))
        uc.reg_write(reg("EFLAGS"), 0x202)
        uc.emu_start(oracle.CODE + start, stop, count=10000)
        assert uc.reg_read(reg("RIP")) == oracle.CODE + at, (obj, "missing atomic boundary")
        assert oracle.effective_offset(uc, insn) == offset
        assert uc.reg_read(reg(oracle.GPRS[insn["add"]])) & mask == add & mask
        if case["operation"] == "cmpccxadd":
            assert uc.reg_read(reg(oracle.GPRS[insn["compare"]])) & mask == cmp
            flags = oracle.flags_sub(old, cmp, case["bits"])
            uc.reg_write(reg(oracle.GPRS[insn["compare"]]), old)
            uc.reg_write(reg("EFLAGS"), (uc.reg_read(reg("EFLAGS")) & ~0x8d5) | flags)
        else:
            flags = oracle.flags_sub(compare, add, 64)
        uc.emu_start(oracle.CODE + insn["end"], stop, count=10000)
        assert uc.reg_read(reg("RIP")) == stop, (obj, "missing return")
        expected = ((flags & 0xd5) | 2) << 8 | ((flags >> 11) & 1)
        assert uc.reg_read(reg("RAX")) == expected, (obj, case, old, uc.reg_read(reg("RAX")), expected)
        assert all(uc.reg_read(reg(name)) == value for name, value in saved.items()), (obj, "C ABI corruption")
    return len(olds)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--oracle", type=Path, required=True)
    parser.add_argument("--llc", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--clang", type=Path, default=Path("clang"))
    parser.add_argument("--gcc", type=Path, default=Path("gcc"))
    parser.add_argument("--keep", type=Path, required=True)
    parser.add_argument("--pipeline", action="store_true")
    parser.add_argument("--pipeline-only", action="store_true",
                        help="qualify decoded pipeline exports without repeating wrapper compilation")
    parser.add_argument("--legacy", action="store_true")
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("apx_reference", args.oracle)
    oracle = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = oracle
    spec.loader.exec_module(oracle)
    oracle.verify_memory_decoder()
    args.keep.mkdir(parents=True, exist_ok=True)
    matrix = [c for c in oracle.cases() if c[2] == 64 and c[3] in (0, 0x64, 0x65, 0x100)]
    reports = []
    for name, triple in ([] if args.pipeline_only else zip(("elf", "coff", "macho"), oracle.TRIPLES)):
        obj = args.exports / ("atomic-" + name + ".o")
        reports.append({"route": "production-object", "triple": triple,
                        **oracle.qualify(obj, matrix, name == "coff", args.legacy)})
    for opt, triple in ([] if args.pipeline_only else itertools.product(("O0", "O2"), oracle.TRIPLES)):
        index = oracle.TRIPLES.index(triple)
        ir = args.exports / "atomic.ll"
        selected = ir
        if opt == "O2":
            selected = args.keep / ("atomic-" + str(index) + ".opt.ll")
            oracle.run([args.llc.parent / "opt", "-passes=default<O2>", "-S", ir, "-o", selected])
        obj = args.keep / f"llvm-{opt}-{index}.o"
        oracle.run([args.llc, "-mtriple=" + triple, "-" + opt,
                    "-verify-machineinstrs", "-filetype=obj", selected, "-o", obj])
        reports.append({"route": "llvm", "optimization": opt, "triple": triple,
                        **oracle.qualify(obj, matrix, index == 1, args.legacy)})
        for route, compiler in itertools.product(("high", "llvm"), (args.clang, args.compiler, args.gcc)):
            gcc = compiler == args.gcc
            if gcc and index:
                continue
            output = args.keep / f"{route}-{compiler.name}-{opt}-{index}.o"
            command = [compiler, "-" + opt, "-ffreestanding", "-fno-stack-protector",
                       "-fno-lto", "-fno-function-sections", "-c",
                       args.exports / f"atomic-{route}.c", "-o", output]
            if not gcc:
                command += ["--target=" + triple]
            if compiler == args.compiler:
                command += ["-fno-builtin-mimalloc", "-mllvm", "-verify-machineinstrs"]
            oracle.run(command)
            reports.append({"route": route, "compiler": str(compiler), "optimization": opt,
                            "triple": triple, **oracle.qualify(output, matrix, index == 1, args.legacy)})
        print({"optimization": opt, "triple": triple, "qualified_programs": len(reports)}, flush=True)
    pipeline, pipeline_checks, pipeline_objects = 0, 0, 0
    if not args.pipeline_only:
        (args.keep / "wrapper-qualification.json").write_text(json.dumps(reports, indent=2) + "\n")
    if args.pipeline or args.pipeline_only:
        formats = json.loads((args.exports / "formats.json").read_text())
        objects = sorted((args.exports / "pipeline").glob("*.o"))
        sources = sorted((args.exports / "pipeline").glob("*.c"))
        assert objects and len(sources) == 2 * len(objects), "missing decoded pipeline exports"
        assert {source.stem.rsplit("-", 1)[0] for source in sources} == {obj.stem for obj in objects}
        for obj in objects:
            triple = formats[obj.stem.split("-")[1]]
            case = json.loads(obj.with_suffix(".json").read_text())
            pipeline_checks += qualify_pipeline(oracle, obj, case, "windows" in triple, args.legacy)
            pipeline_objects += 1
        for source in sources:
            index, format_id, route = source.stem.split("-")
            triple = formats[format_id]
            case = json.loads(source.with_name(index + "-" + format_id + ".json").read_text())
            for opt, compiler in itertools.product(("O0", "O2"), (args.clang, args.compiler)):
                output = args.keep / "pipeline.o"
                command = [compiler, "--target=" + triple, "-" + opt, "-ffreestanding",
                           "-fno-function-sections", "-fno-stack-protector", "-fno-lto",
                           "-c", source, "-o", output]
                if compiler == args.compiler:
                    command += ["-fno-builtin-mimalloc", "-mllvm", "-verify-machineinstrs"]
                oracle.run(command)
                pipeline_checks += qualify_pipeline(oracle, output, case, "windows" in triple, args.legacy)
                pipeline += 1
            if pipeline % 336 == 0:
                print({"pipeline_source_compilations": pipeline, "pipeline_checks": pipeline_checks}, flush=True)
        print({"pipeline_source_compilations": pipeline}, flush=True)
    report = {"programs": reports, "pipeline_source_compilations": pipeline,
              "pipeline_production_objects": pipeline_objects,
              "pipeline_controlled_completions": pipeline_checks,
              "physical_atomicity_qualified": False}
    name = "pipeline-qualification.json" if args.pipeline_only else "qualification.json"
    (args.keep / name).write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()

