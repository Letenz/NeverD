import copy
import struct
import unittest

from scripts.check_windows_registration_cxx_rewrite import (
    BASES, IMAGE_LABELS, require_cxx_outcome, require_image_matrix,
    require_installation_identity, validate_compiled_image)
from scripts.check_windows_registration_rewrite import PE32


class WindowsRegistrationCxxRuntimeAdmissionTests(unittest.TestCase):
    def test_exact_value_and_reference_object_effects(self):
        for reference, caught in ((False, 7), (True, 18)):
            result = {"exit_code": 0, "stdout":
                      "neverd-registration-cxx: value=7 caller=00402030 "
                      "entry=00401000 chain=1 iterations=4 trace=213 "
                      f"caught={caught}\n"}
            accepted = require_cxx_outcome(result, 0x400000, 0x1000,
                                           0x2000, 0x2080, reference)
            self.assertEqual(accepted["caller_rva"], 0x2030)
            self.assertEqual(accepted["caught"], caught)

    def test_runtime_success_flags_cannot_hide_wrong_machine_effects(self):
        baseline = "neverd-registration-cxx: value=7 caller=00402030 " \
                   "entry=00401000 chain=1 iterations=4 trace=213 caught=18\n"
        for before, after in (("value=7", "value=9"), ("chain=1", "chain=0"),
                              ("iterations=4", "iterations=3"),
                              ("trace=213", "trace=123"), ("caught=18", "caught=7"),
                              ("caller=00402030", "caller=00401030"),
                              ("caller=00402030", "caller=00402080"),
                              ("entry=00401000", "entry=00402000")):
            with self.subTest(after=after), self.assertRaises(ValueError):
                require_cxx_outcome({"exit_code": 0, "stdout": baseline.replace(before, after)},
                                    0x400000, 0x1000, 0x2000, 0x2080, True)
        for code in (None, 1, -11, 0xc0000005):
            with self.subTest(code=code), self.assertRaises(ValueError):
                require_cxx_outcome({"exit_code": code, "stdout": baseline},
                                    0x400000, 0x1000, 0x2000, 0x2080, True)

    def test_empty_or_extra_output_cannot_substitute_for_execution(self):
        for output in ("", "PASS", "neverd-registration-cxx: value=7\n"):
            with self.subTest(output=output), self.assertRaises(ValueError):
                require_cxx_outcome({"exit_code": 0, "stdout": output},
                                    0x400000, 0x1000, 0x2000, 0x2080, False)

    def test_matrix_requires_every_public_and_cli_route_at_both_bases(self):
        records = [{"image": f"C:\\evidence\\{label}{suffix}.exe",
                    "generated": label != "original", "runtime_base": base}
                   for label in IMAGE_LABELS
                   for suffix, base in (("", BASES[0]), ("-rebased", BASES[1]))]
        require_image_matrix(records, 2)
        require_image_matrix(records[:4], 1)
        for index in range(len(records)):
            with self.subTest(index=index), self.assertRaises(ValueError):
                require_image_matrix(records[:index] + records[index + 1:], 2)
        for altered in (records[:4], records + records[:1],
                        records[:-1] + records[:1]):
            with self.assertRaises(ValueError):
                require_image_matrix(altered, 2)
        with self.assertRaises(ValueError):
            require_image_matrix(records, 3)

    @staticmethod
    def compiled_pe():
        data = bytearray(0xe00)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 0x3c, 0x80)
        data[0x80:0x84] = b"PE\0\0"
        struct.pack_into("<HH", data, 0x84, 0x14c, 3)
        struct.pack_into("<H", data, 0x94, 0xe0)
        optional = 0x98
        struct.pack_into("<H", data, optional, 0x10b)
        struct.pack_into("<I", data, optional + 28, 0x400000)
        for i, (name, rva, raw, size, flags) in enumerate((
                (b".text", 0x1000, 0x400, 0x200, 0x60000020),
                (b".ndtext", 0x2000, 0x600, 0x400, 0x60000020),
                (b".rdata", 0x3000, 0xa00, 0x400, 0x40000040))):
            offset = optional + 0xe0 + i * 40
            data[offset:offset + len(name)] = name
            struct.pack_into("<4I", data, offset + 8, size, rva, size, raw)
            struct.pack_into("<I", data, offset + 36, flags)
        struct.pack_into("<2I", data, optional + 96, 0x3000, 40)
        struct.pack_into("<2I", data, optional + 96 + 80, 0x3200, 64)
        struct.pack_into("<5I", data, 0xa00 + 20, 1, 1, 0x3080, 0x3084, 0x3088)
        struct.pack_into("<2I", data, 0xa80, 0x1000, 0x3040)
        name = b"registration_cxx_probe\0"
        data[0xa40:0xa40 + len(name)] = name
        data[0x400] = 0xe9
        struct.pack_into("<i", data, 0x401, 0x2000 - 0x1005)
        data[0x700] = 0xb8
        struct.pack_into("<I", data, 0x701, 0x402200)
        data[0x705] = 0xe9
        struct.pack_into("<i", data, 0x706, 0x1010 - 0x210a)
        struct.pack_into("<I", data, 0x800, 0x19930522)
        fields = [{"rva": 0x2101, "value": 0x402200}]
        for i in range(8):
            fields.append({"rva": 0x2320 + 4 * i, "value": 0x403000 + 4 * i})
            struct.pack_into("<I", data, 0x920 + 4 * i, fields[-1]["value"])
        struct.pack_into("<I", data, 0xc00, 192)
        struct.pack_into("<2I", data, 0xc40, 0x403300, 2)
        struct.pack_into("<2I", data, 0xd00, 0x1010, 0x2100)
        blocks = bytearray()
        for page, offsets in ((0x2000, [f["rva"] - 0x2000 for f in fields]),
                               (0x3000, [0x240])):
            size = (8 + 2 * len(offsets) + 3) & ~3
            block = bytearray(size)
            struct.pack_into("<2I", block, 0, page, size)
            for i, offset in enumerate(offsets):
                struct.pack_into("<H", block, 8 + 2 * i, 0x3000 | offset)
            blocks.extend(block)
        data[0xd80:0xd80 + len(blocks)] = blocks
        struct.pack_into("<2I", data, optional + 96 + 40, 0x3380, len(blocks))
        contract = {"schema": 1, "evidence": "checked-cxx-manual-installation",
                    "image_base": 0x400000, "source_entry_rva": 0x1000,
                    "generated_code_begin_rva": 0x2000, "generated_code_end_rva": 0x2080,
                    "registration_handler_rva": 0x2100, "func_info_rva": 0x2200,
                    "absolute_pointer_fields": fields}
        return data, contract

    def test_msvc_compatibility_directory_and_all_pointer_rebases(self):
        data, contract = self.compiled_pe()
        image = PE32(data)
        self.assertEqual(image.directory(10)[1], 64)
        self.assertEqual(image.u32(image.raw(0x3200)), 192)
        validate_compiled_image(image, contract)
        validate_compiled_image(PE32(image.rebase(0x18000000)), contract)

    def test_public_identity_closes_all_bytes_at_the_actual_base(self):
        data, contract = self.compiled_pe()
        manual = PE32(data)
        for base in BASES:
            installed = PE32(bytes(data) if base == BASES[0] else manual.rebase(base))
            require_installation_identity(installed, manual)
            changed = bytearray(installed.data)
            changed[0x415] ^= 1  # Preserved original helper outside indexed dispatch.
            validate_compiled_image(PE32(changed), contract)
            with self.assertRaises(ValueError):
                require_installation_identity(PE32(changed), manual)

    def test_incomplete_native_contracts_and_raw_installed_mutations_reject(self):
        for mutation in range(10):
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                data, contract = self.compiled_pe()
                contract = copy.deepcopy(contract)
                if mutation == 0:
                    data[0x401] ^= 1
                elif mutation == 1:
                    data[0x700] = 0xb9
                elif mutation == 2:
                    data[0x920] ^= 1
                elif mutation == 3:
                    struct.pack_into("<H", data, 0xd88, 0)
                elif mutation == 4:
                    contract["absolute_pointer_fields"][1] = contract["absolute_pointer_fields"][0]
                elif mutation == 5:
                    struct.pack_into("<I", data, 0xc44, 0)
                elif mutation == 6:
                    struct.pack_into("<I", data, 0xd04, 0x1020)
                elif mutation == 7:
                    struct.pack_into("<I", data, 0x98 + 0xe0 + 40 + 36, 0xe0000020)
                elif mutation == 8:
                    struct.pack_into("<i", data, 0x706, 0x2000 - 0x210a)
                else:
                    struct.pack_into("<I", data, 0xc00, 64)
                validate_compiled_image(PE32(data), contract)


if __name__ == "__main__":
    unittest.main()
