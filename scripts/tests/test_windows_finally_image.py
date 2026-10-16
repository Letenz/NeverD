import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest

from scripts import windows_finally_image as image


def executable():
    data = bytearray(0xa00)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", data, 0x84, 0x14c, 3)
    struct.pack_into("<HH", data, 0x94, 0xe0, 0x102)
    struct.pack_into("<H", data, 0x98, 0x10b)
    struct.pack_into("<I", data, 0x98 + 16, 0x1000)
    struct.pack_into("<I", data, 0x98 + 28, 0x400000)
    struct.pack_into("<H", data, 0x98 + 70, 0x140)
    struct.pack_into("<II", data, 0x98 + 96, 0x2000, 0x80)
    for index, name in enumerate((b".text", b".rdata", b".ndtext")):
        offset = 0x178 + 40 * index
        data[offset:offset + len(name)] = name
        struct.pack_into("<IIII", data, offset + 8, 0x200,
                         0x1000 * (index + 1), 0x200, 0x400 + 0x200 * index)
        struct.pack_into("<I", data, offset + 36,
                         0x40000040 if index == 1 else 0x60000020)
    data[0x400:0x405] = b"\x90\x90\x90\x90\xc3"
    struct.pack_into("<IIIII", data, 0x600 + 20, 1, 1, 0x2040, 0x2044, 0x2048)
    struct.pack_into("<IIH", data, 0x640, 0x1000, 0x2060, 0)
    data[0x660:0x660 + len(image.EXPORT) + 1] = image.EXPORT + b"\0"
    return data


def observation(values, status=0):
    return {"exit_code": status, "stdout": " ".join(map(str, values)) + " \n"}


class FinallyImageTests(unittest.TestCase):
    def test_goto_requires_all_paths_guards_and_negative_control(self):
        values = (123457, 12357, 13567, 1357, 0, 1, 1, 1, 128, 0x18000000)
        self.assertEqual(image.observe(observation(values), values[-1], form="goto"), values)
        for index in range(len(values)):
            changed = list(values)
            changed[index] += 1
            with self.subTest(index=index), self.assertRaises(ValueError):
                image.observe(observation(changed), values[-1], form="goto")
        control = (128457, 12857, 18567, 1857, 0, 1, 1, 0, 128, values[-1])
        self.assertEqual(image.observe(observation(control, 1), values[-1], True, "goto"), control)
        for form, negative, result in (("goto", False, observation(control, 1)),
                                       ("goto", True, observation(values)),
                                       ("return", False, observation(values)),
                                       ("reenter", False, observation(values)),
                                       ("goto", False, observation(values, 1))):
            with self.subTest(form=form, negative=negative), self.assertRaises(ValueError):
                image.observe(result, values[-1], negative, form)

    def test_goto_keeps_its_selected_export_identity(self):
        source = executable()
        export = b"nonlocal_finally_goto"
        source[0x660:0x680] = (export + b"\0").ljust(32, b"\0")
        adapted = image.as_library(source, export)
        self.assertEqual(image.PE32(adapted).entry(export), 0x1000)
        with self.assertRaises(ValueError):
            image.as_library(source)
        product = bytearray(source)
        product[0x400] = 0xe9
        struct.pack_into("<i", product, 0x401, 0x3000 - 0x1005)
        self.assertEqual(image.generated_entry(source, product, export), 0x3000)
        with self.assertRaises(ValueError):
            image.generated_entry(source, product)

    def test_library_adapter_changes_only_documented_loader_fields(self):
        source = executable()
        adapted = image.as_library(source)
        allowed = {0x96, 0x97, *range(0xa8, 0xac), *range(0xd8, 0xdc), 0xde, 0xdf}
        self.assertEqual(len(source), len(adapted))
        self.assertTrue(all(a == b or index in allowed
                            for index, (a, b) in enumerate(zip(source, adapted))))
        parsed = image.PE32(adapted)
        self.assertEqual(parsed.u32(parsed.optional + 16), 0)
        self.assertTrue(parsed.u16(parsed.optional - 2) & 0x2000)
        self.assertEqual(parsed.u16(parsed.optional + 70), 0x100)
        self.assertEqual(parsed.entry(image.EXPORT), 0x1000)

    def test_tls_foreign_images_and_nonexecutable_exports_are_refused(self):
        for offset, code, value in ((0x98 + 96 + 9 * 8, "I", 0x2100),
                                    (0x96, "H", 0x2102), (0x84, "H", 0x1c4),
                                    (0x178 + 36, "I", 0x40000040)):
            data = executable()
            struct.pack_into("<" + code, data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                image.as_library(data)

    def test_replacement_must_reach_generated_code_from_the_same_export(self):
        source = executable()
        product = bytearray(source)
        product[0x400] = 0xe9
        struct.pack_into("<i", product, 0x401, 0x3000 - 0x1005)
        self.assertEqual(image.generated_entry(source, product), 0x3000)
        for offset, code, value in ((0x401, "i", -5), (0x640, "I", 0x3000),
                                    (0x98 + 28, "I", 0x18000000),
                                    (0x178 + 80 + 36, "I", 0xe0000020)):
            changed = bytearray(product)
            struct.pack_into("<" + code, changed, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                image.generated_entry(source, changed)
        with self.assertRaises(ValueError):
            image.generated_entry(source, source)

    def test_each_runtime_guard_and_the_actual_load_base_are_required(self):
        values = (123, 13, 0, 1, 1, 1, 64, 0x18000000)
        self.assertEqual(image.observe(observation(values), values[-1]), values)
        for index in range(len(values)):
            changed = list(values)
            changed[index] += 1
            with self.subTest(index=index), self.assertRaises(ValueError):
                image.observe(observation(changed), values[-1])
        for result in (observation(values, 1), {"exit_code": 0, "stdout": ""},
                       {"exit_code": 0, "stdout": observation(values)["stdout"] + "extra"}):
            with self.subTest(result=result), self.assertRaises(ValueError):
                image.observe(result, values[-1])
        control = (128, 18, 0, 1, 1, 0, 64, values[-1])
        self.assertEqual(image.observe(observation(control, 1), values[-1], True), control)
        with self.assertRaises(ValueError):
            image.observe(observation(control, 1), values[-1])
        with self.assertRaises(ValueError):
            image.observe(observation(values), values[-1], True)

    def test_captured_inputs_bind_source_profile_and_exact_image(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.c"
            source.write_text("int observed(void);\n")
            binary = root / "x86/Od-no-gs/original.exe"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(executable())
            case = {"case": "Od-no-gs", "passed": True, "negative_control": False,
                    "sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
            manifest = {"schema": 2, "passed": True, "toolset": "14.51",
                        "evidence": "original-msvc-nonlocal-finally", "architecture": "x86",
                        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                        "cases": [case]}
            path = root / "x86/nonlocal-finally.json"
            path.write_text(json.dumps(manifest))
            self.assertEqual(image.captured_input(root, source, "Od-no-gs"), binary)
            for key, value in (("passed", False), ("architecture", "x64"),
                               ("source_sha256", "0" * 64), ("cases", [case, case]),
                               ("cases", [case | {"negative_control": True}])):
                path.write_text(json.dumps(manifest | {key: value}))
                with self.subTest(key=key), self.assertRaises(ValueError):
                    image.captured_input(root, source, "Od-no-gs")
            path.write_text(json.dumps(manifest))
            binary.write_bytes(binary.read_bytes() + b"changed")
            with self.assertRaises(ValueError):
                image.captured_input(root, source, "Od-no-gs")


if __name__ == "__main__":
    unittest.main()
