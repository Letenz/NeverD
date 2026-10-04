import struct
import unittest

from scripts import check_windows_simd as oracle


class WindowsSIMDEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.values, _, self.operations, self.fields = oracle.definitions()
        self.record = struct.Struct("<" + "Q" * len(self.fields))
        self.rows = []
        for op, (_, status) in enumerate(self.operations):
            for memory in range(2):
                for unmask in range(self.values["MaskCount"]):
                    for sticky in range(2):
                        for mode in range(self.values["ModeCount"]):
                            row = dict.fromkeys(self.fields, 0)
                            control = (self.values["DefaultMXCSR"] & ~(unmask << self.values["MaskShift"]))
                            control |= self.values["Sticky"] if sticky else 0
                            row.update(Operation=op, Memory=memory, Unmask=unmask, Sticky=sticky, Mode=mode,
                                       BeforeMXCSR=control, FaultPC=1, ResumePC=2)
                            if unmask & status:
                                row.update(Traps=1, Code=1, ExceptionPC=1, ContextPC=1,
                                           Continues=1, ContinueContextPC=2 if mode == 0 else 1)
                            self.rows.append(row)
        self.fault = next(row for row in self.rows if row["Traps"])

    def data(self):
        return b"".join(self.record.pack(*(row[key] for key in self.fields)) for row in self.rows)

    def test_complete_ordered_matrix_is_preserved(self):
        observations = oracle.parse_observations(self.data())
        self.assertEqual(len(observations), 6144)
        self.assertEqual([row["record"] for row in observations], self.rows)
        self.assertEqual({row["operation"] for row in observations},
                         {name for name, _ in self.operations})

    def test_observation_does_not_guess_status_or_mxcsr_conventions(self):
        self.fault.update(Code=2, ContextMXCSR=3, ContextFXMXCSR=4, AfterMXCSR=5)
        observations = oracle.parse_observations(self.data())
        self.assertEqual(observations[self.rows.index(self.fault)]["record"], self.fault)

    def test_missing_extra_or_partial_records_fail(self):
        data = self.data()
        for invalid in (b"", data[:-1], data[:-self.record.size], data + data[:self.record.size]):
            with self.subTest(size=len(invalid)), self.assertRaises(ValueError):
                oracle.parse_observations(invalid)

    def test_duplicate_or_reordered_coordinates_fail(self):
        for field in ("Operation", "Memory", "Unmask", "Sticky", "Mode"):
            self.rows[0][field] = 1
            with self.subTest(field=field), self.assertRaises(ValueError):
                oracle.parse_observations(self.data())
            self.rows[0][field] = 0

    def test_input_control_must_match_the_matrix_coordinates(self):
        self.rows[0]["BeforeMXCSR"] ^= 1
        with self.assertRaises(ValueError):
            oracle.parse_observations(self.data())

    def test_fault_requires_the_exact_original_pc(self):
        for field in ("FaultPC", "ExceptionPC", "ContextPC"):
            old = self.fault[field]
            for value in (0, old + 1):
                self.fault[field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    oracle.parse_observations(self.data())
            self.fault[field] = old

    def test_retry_requires_one_continuation_at_the_selected_pc(self):
        for row in (row for row in self.rows if row["Traps"]):
            old = row["ContinueContextPC"]
            row["ContinueContextPC"] = 0
            with self.subTest(mode=row["Mode"]), self.assertRaises(ValueError):
                oracle.parse_observations(self.data())
            row["ContinueContextPC"] = old
            if row["Mode"] == self.values["ModeCount"] - 1:
                break
        for count in (0, 2):
            self.fault["Continues"] = count
            with self.subTest(count=count), self.assertRaises(ValueError):
                oracle.parse_observations(self.data())
        self.fault["Continues"] = 1

    def test_primary_exception_cannot_be_lost_or_repeated(self):
        for count in (0, 2):
            self.fault["Traps"] = count
            with self.subTest(count=count), self.assertRaises(ValueError):
                oracle.parse_observations(self.data())

    def test_masked_case_cannot_invent_an_exception_record(self):
        for field in ("Traps", "Code", "ContextPC", "ContextMXCSR", "Parameter14", "HandlerMXCSR", "Continues"):
            self.rows[0][field] = 1
            with self.subTest(field=field), self.assertRaises(ValueError):
                oracle.parse_observations(self.data())
            self.rows[0][field] = 0

    def test_parameters_require_a_bounded_count_and_normalized_tail(self):
        for field, value in (("Code", 0), ("ParameterCount", 16), ("Parameter0", 1)):
            old = self.fault[field]
            self.fault[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                oracle.parse_observations(self.data())
            self.fault[field] = old


if __name__ == "__main__":
    unittest.main()
