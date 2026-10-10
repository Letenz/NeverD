"""Check the exact machine-code boundary of the derived SavedESP oracle."""

import struct
import unittest

from scripts.windows_registration_snapshot import saved_stack_probe


class SnapshotImage:
    base = 0x400000

    def __init__(self, reference):
        self.data = bytearray(512)
        self.data[:6] = bytes.fromhex("55 8b ec 6a ff 68")
        self.data[0x18:0x22] = bytes.fromhex("51 83 ec 10 53 56 57 89 65 f0")
        inner, outer = ((0xe8, 0xe4) if reference else (0xec, 0xe8))
        self.data[0x45:0x5e] = (bytes.fromhex("c6 45 fc 01 8d 4d") + bytes([inner]) +
                                bytes.fromhex("e8 5f ff ff ff c6 45 fc 00 8d 4d") +
                                bytes([outer]) + bytes.fromhex("e8 53 ff ff ff 90"))
        self.store = 0x8f if reference else 0x7b
        self.data[self.store:self.store + 3] = bytes.fromhex("89 4d e0" if reference else "89 55 e0")
        target = 0xa7 if reference else 0x93
        self.data[self.store + 3:self.store + 9] = b"\xb8" + struct.pack("<I", 0x401000 + target) + b"\xc3"
        epilogue = 0xb8 if reference else 0xa4
        self.data[epilogue:epilogue + 17] = bytes.fromhex("8b 4d f4 64 89 0d 00 00 00 00 5f 5e 5b 8b e5 5d c3")
        self.fields = {0x1000 + self.store + 4}

    def entry(self, name):
        assert name == b"registration_cxx_probe"
        return 0x1000

    def raw(self, rva, size=4):
        assert 0x1000 <= rva <= 0x1200 - size
        return rva - 0x1000

    def relocation_fields(self):
        return self.fields


class RegistrationSnapshotTests(unittest.TestCase):
    def test_probe_reads_parent_object_through_restored_saved_memory(self):
        for reference in (False, True):
            with self.subTest(reference=reference):
                source = SnapshotImage(reference)
                result = saved_stack_probe(source, reference)
                self.assertEqual(len(source.data), len(result))
                self.assertEqual(result[source.store + 2], 0xf0)
                self.assertEqual(struct.unpack_from("<I", result, source.store + 4)[0], 0x401045)
                self.assertEqual(result[0x45:0x52], bytes.fromhex("c7 45 fc ff ff ff ff 8b 45 f0 8b 40") + bytes([20 if reference else 24]))
                allowed = set(range(0x45, 0x5e)) | {source.store + 2} | set(range(source.store + 4, source.store + 8))
                self.assertTrue(all(a == b or index in allowed
                                    for index, (a, b) in enumerate(zip(source.data, result))))

    def test_changed_code_or_relocation_cannot_be_used_as_an_oracle(self):
        for reference in (False, True):
            for mutation in range(8):
                with self.subTest(reference=reference, mutation=mutation):
                    source = SnapshotImage(reference)
                    if mutation < 6:
                        offsets = (0, 0x18, 0x45, source.store, source.store + 4,
                                   0xb8 if reference else 0xa4)
                        source.data[offsets[mutation]] ^= 1
                    elif mutation == 6:
                        source.fields.clear()
                    else:
                        source.fields.add(0x1042)
                    with self.assertRaises(ValueError):
                        saved_stack_probe(source, reference)


if __name__ == "__main__":
    unittest.main()
