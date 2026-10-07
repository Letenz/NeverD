import json
from pathlib import Path
import struct
import tempfile
import unittest

from scripts import audit_native_backend_build as audit


def elf(machine=183, kind=1):
    data = bytearray(64)
    data[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<HH", data, 16, kind, machine)
    return bytes(data)


def coff(machine=0xAA64):
    return struct.pack("<HHIIIHH", machine, 1, 0, 0, 0, 0, 0)


def big_object(machine=0xAA64):
    data = bytearray(56)
    struct.pack_into("<HHHH", data, 0, 0, 0xFFFF, 2, machine)
    data[12:28] = bytes.fromhex("c7a1bad1eebaa94baf20faf66aa4dcb8")
    return bytes(data)


class ObjectArchitectureTests(unittest.TestCase):
    def test_arm64_relocatable_objects_are_accepted(self):
        audit.validate_object(elf(), "kvm")
        audit.validate_object(coff(), "whp")
        audit.validate_object(big_object(), "whp")

    def test_x64_objects_cannot_establish_arm64_compilation(self):
        for data, backend in [(elf(62), "kvm"), (coff(0x8664), "whp"),
                              (big_object(0x8664), "whp")]:
            with self.subTest(backend=backend), self.assertRaises(ValueError):
                audit.validate_object(data, backend)

    def test_elf_images_and_wrong_byte_order_are_rejected(self):
        bad_endian = bytearray(elf())
        bad_endian[5] = 2
        for data in [elf(kind=2), elf(kind=3), bytes(bad_endian)]:
            with self.subTest(data=data[:20]), self.assertRaises(ValueError):
                audit.validate_object(data, "kvm")

    def test_truncated_headers_are_rejected(self):
        for data, backend in [(elf()[:-1], "kvm"), (coff()[:-1], "whp"),
                              (big_object()[:-1], "whp")]:
            with self.subTest(backend=backend), self.assertRaises(ValueError):
                audit.validate_object(data, backend)

    def test_anonymous_and_old_big_objects_are_rejected(self):
        bad_uuid = bytearray(big_object())
        bad_uuid[12] ^= 1
        old_version = bytearray(big_object())
        struct.pack_into("<H", old_version, 4, 1)
        for data in [bytes(bad_uuid), bytes(old_version)]:
            with self.assertRaises(ValueError):
                audit.validate_object(data, "whp")


class NativeRecipeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "build"
        self.build.mkdir()
        _, sources, _, self.texts = audit.definitions()
        self.rows = []
        for relative in sources["kvm"]:
            source = self.root / "lib/emulation" / relative
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("// independent source fixture\n")
            obj = self.build / "lib/emulation/CMakeFiles/NeverDEmulationNative.dir" / (relative + ".o")
            obj.parent.mkdir(parents=True, exist_ok=True)
            obj.write_bytes(elf())
            self.rows.append({"directory": str(self.build), "file": str(source),
                              "output": str(obj),
                              "command": "c++ -DNEVERD_EMULATION_KVM=1 -c " + str(source)})
        self.cache = {"NEVERD_ENABLE_CPU_EMULATION": "ON",
                      "NEVERD_EMULATION_BACKEND_UNICORN": "OFF",
                      "NEVERD_EMULATION_BACKEND_KVM": "ON"}

    def check(self, root=None):
        (self.build / "CMakeCache.txt").write_text("".join(
            key + ":BOOL=" + value + "\n" for key, value in self.cache.items()))
        (self.build / "compile_commands.json").write_text(json.dumps(self.rows))
        return audit.audit_objects(self.build, "kvm", root or self.root)

    def test_each_native_source_has_a_hashed_arm64_object(self):
        objects = self.check()
        self.assertEqual(len(objects), 3)
        self.assertTrue(all(len(obj["object_sha256"]) == 64 for obj in objects))

    def test_root_aliases_preserve_physical_source_identity(self):
        alias = self.root / ".." / self.root.name
        self.assertEqual(self.check(alias), self.check())

    def test_real_cache_comments_and_line_endings_preserve_profile_entries(self):
        expected = self.check()
        cache = self.build / self.texts["Cache"]
        entries = cache.read_text().splitlines()
        for ending in ("\n", "\r\n"):
            content = "# CMake cache\n\n" + "\n\n".join(
                "//Enable the selected component\n" + entry for entry in entries) + "\n"
            cache.write_bytes(content.replace("\n", ending).encode())
            with self.subTest(ending=ending):
                self.assertEqual(audit.audit_objects(self.build, "kvm", self.root), expected)

    def test_missing_and_duplicate_native_recipes_are_rejected(self):
        original = list(self.rows)
        for rows in [original[:-1], original + original[:1]]:
            self.rows = rows
            with self.assertRaises(ValueError):
                self.check()

    def test_fallback_build_and_lookalike_definition_are_rejected(self):
        for flag in ["", "-DNEVERD_EMULATION_KVM=0", "-DNEVERD_EMULATION_KVM_FAKE=1"]:
            self.rows[0]["command"] = "c++ " + flag + " -c input.cpp"
            with self.assertRaises(ValueError):
                self.check()

    def test_unicorn_and_disabled_native_profiles_are_rejected(self):
        for key, value in [("NEVERD_EMULATION_BACKEND_UNICORN", "ON"),
                           ("NEVERD_ENABLE_CPU_EMULATION", "OFF"),
                           ("NEVERD_EMULATION_BACKEND_KVM", "OFF")]:
            old = self.cache[key]
            self.cache[key] = value
            with self.assertRaises(ValueError):
                self.check()
            self.cache[key] = old

    def test_another_object_cannot_replace_the_native_recipe_output(self):
        self.rows[0]["output"] = str(self.build / "other.o")
        with self.assertRaises(ValueError):
            self.check()


if __name__ == "__main__":
    unittest.main()
