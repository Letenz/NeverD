import struct
import unittest

from scripts import check_windows_memory_write as oracle


class WindowsMemoryWriteEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.values, _, self.protections, self.fields = oracle.definitions()
        self.record = struct.Struct("<" + "Q" * len(self.fields))
        self.rows = []
        for first, (_, first_rights) in enumerate(self.protections):
            for second, (_, second_rights) in enumerate(self.protections):
                self.rows.append(dict(First=first, Second=second, Result=0, Error=998,
                                      Written=self.values["WrittenSeed"],
                                      AfterFirst=first_rights, AfterSecond=second_rights,
                                      ByteFirst=self.values["InitialFirst"],
                                      ByteSecond=self.values["InitialSecond"]))

    def data(self):
        return b"".join(self.record.pack(*(row[key] for key in self.fields)) for row in self.rows)

    def test_complete_ordered_matrix_retains_actual_observations(self):
        self.assertEqual(oracle.parse_observations(self.data()), self.rows)

    def test_missing_extra_and_partial_records_fail(self):
        data = self.data()
        for invalid in (b"", data[:-1], data[:-self.record.size], data + data[:self.record.size]):
            with self.subTest(size=len(invalid)), self.assertRaises(ValueError):
                oracle.parse_observations(invalid)

    def test_duplicate_and_reordered_coordinates_fail(self):
        for key in ("First", "Second"):
            self.rows[0][key] = 1
            with self.subTest(key=key), self.assertRaises(ValueError):
                oracle.parse_observations(self.data())
            self.rows[0][key] = 0

    def test_boolean_error_and_byte_wire_widths_are_checked(self):
        for key, value in (("Result", 2), ("Error", 1 << 32),
                           ("ByteFirst", 1 << 8), ("ByteSecond", 1 << 8)):
            old = self.rows[0][key]
            self.rows[0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                oracle.parse_observations(self.data())
            self.rows[0][key] = old

    def test_unknown_output_protection_fails(self):
        for key in ("AfterFirst", "AfterSecond"):
            old = self.rows[0][key]
            self.rows[0][key] = 0
            with self.subTest(key=key), self.assertRaises(ValueError):
                oracle.parse_observations(self.data())
            self.rows[0][key] = old

    def test_probe_does_not_invent_write_failure_or_protection_conventions(self):
        self.rows[0].update(Result=1, Error=0, Written=2,
                            AfterFirst=self.protections[-1][1],
                            ByteFirst=self.values["UpdatedFirst"])
        self.assertEqual(oracle.parse_observations(self.data()), self.rows)


if __name__ == "__main__":
    unittest.main()
