import struct
import unittest

from scripts import check_windows_alignment as oracle


class WindowsAlignmentEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.values, _, self.operations, self.scenarios, self.fields = oracle.definitions()
        self.record = struct.Struct("<" + "Q" * len(self.fields))
        self.rows = []
        for op in range(len(self.operations)):
            for case in range(len(self.scenarios)):
                row = dict.fromkeys(self.fields, 0)
                row.update(Operation=op, Scenario=case, Code=0xc0000005, ParameterCount=2,
                           Parameter1=0xffffffffffffffff, FaultPC=0x12345000,
                           ExceptionPC=0x12345000, ContextPC=0x12345000,
                           Operand=0x10001, ContextOperand=0x10001,
                           VectorLow=self.values["VectorLow"], VectorHigh=self.values["VectorHigh"])
                self.rows.append(row)

    def data(self):
        return b"".join(self.record.pack(*(row[key] for key in self.fields)) for row in self.rows)

    def test_every_original_operation_and_scenario_is_recorded(self):
        observations = oracle.parse_observations(self.data())
        self.assertEqual(len(observations), 72)
        self.assertEqual({(o["operation"], o["scenario"]) for o in observations},
                         {(op, case) for op in self.operations for case in self.scenarios})

    def test_no_guessed_windows_status_is_used_as_an_oracle(self):
        self.rows[0].update(Code=0x80000002, ParameterCount=0, Parameter1=0)
        observed = oracle.parse_observations(self.data())
        self.assertEqual(observed[0]["record"]["Code"], 0x80000002)

    def test_truncated_and_extra_results_fail(self):
        data = self.data()
        for invalid in (b"", data[:-1], data[:-self.record.size], data + data[:self.record.size]):
            with self.subTest(size=len(invalid)), self.assertRaises(ValueError):
                oracle.parse_observations(invalid)

    def test_duplicate_and_reordered_results_fail(self):
        for field in ("Operation", "Scenario"):
            with self.subTest(field=field):
                self.rows[0][field] = 1
                with self.assertRaises(ValueError):
                    oracle.parse_observations(self.data())
                self.rows[0][field] = 0

    def test_fault_record_and_saved_pc_must_identify_the_original_site(self):
        for field in ("FaultPC", "ExceptionPC", "ContextPC"):
            old = self.rows[0][field]
            for value in (0, old + 1):
                with self.subTest(field=field, value=value):
                    self.rows[0][field] = value
                    with self.assertRaises(ValueError):
                        oracle.parse_observations(self.data())
            self.rows[0][field] = old

    def test_changed_input_or_vector_cannot_be_accepted(self):
        for field in ("ContextOperand", "VectorLow", "VectorHigh"):
            self.rows[0][field] ^= 1
            with self.subTest(field=field), self.assertRaises(ValueError):
                oracle.parse_observations(self.data())
            self.rows[0][field] ^= 1

    def test_parameter_counts_and_undefined_tails_are_checked(self):
        for field, value in (("Code", 0), ("ParameterCount", 16), ("Parameter2", 7)):
            old = self.rows[0][field]
            self.rows[0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                oracle.parse_observations(self.data())
            self.rows[0][field] = old


if __name__ == "__main__":
    unittest.main()
