"""Native LSE evidence rejects missing records, partial effects and stale context."""
import copy
import struct
import unittest

from scripts.check_aarch64_atomics import contract, group_digests, required_records, validate_observations


class AtomicEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.values, self.fields, _, scenarios = contract()
        self.specification = (self.values, self.fields, [("Swap8Relaxed", "Swap", 1, 1)],
                              [scenarios[0], scenarios[3]])
        v = self.values
        before = [v["Operand"], v["InitialStatus"], v["Operand"], v["Initial"], 0x8000]
        self.rows = []
        for scenario in range(2):
            row = dict.fromkeys(self.fields, 0)
            row.update(Scenario=scenario, MemoryBase=0x8000, AtomicPC=0x4000,
                       ReturnFlags=v["InitialFlags"], After0=v["Initial"],
                       After1=v["Operand"], After2=v["Initial"], After3=v["Operand"])
            row.update({f"Return{i}": value for i, value in enumerate(before)})
            if scenario:
                row.update(Faults=1, Code=v["AccessViolation"], ParameterCount=2,
                           Parameter0=1, Parameter1=0x8000, ExceptionPC=0x4000, ContextPC=0x4000,
                           ContextFlags=v["InitialFlags"])
                row.update({f"Context{i}": value for i, value in enumerate(before)})
            else:
                row["Return2"] = v["Initial"] & 0xff
                row["After0"] = (v["Initial"] & ~0xff) | (v["Operand"] & 0xff)
            self.rows.append(row)

    def encode(self, rows):
        return b"".join(struct.pack("<" + "Q" * len(self.fields), *(row[f] for f in self.fields))
                        for row in rows)

    def test_complete_results_and_write_fault_are_accepted(self):
        self.assertEqual(len(validate_observations(self.encode(self.rows), self.specification)), 2)

    def test_unreadable_operand_reports_read_before_write(self):
        values, fields, cases, scenarios = self.specification
        scenarios = [scenarios[0], ("NoAccess", 0, values["PageNoAccess"], 0, 0)]
        specification = values, fields, cases, scenarios
        rows = copy.deepcopy(self.rows)
        with self.assertRaises(ValueError):
            validate_observations(self.encode(rows), specification)
        rows[1]["Parameter0"] = 0
        self.assertEqual(len(validate_observations(self.encode(rows), specification)), 2)

    def test_missing_extra_reordered_and_duplicate_records_are_rejected(self):
        for rows in [[], self.rows[:1], self.rows + self.rows[:1], self.rows[::-1],
                     [self.rows[0], self.rows[0]]]:
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                validate_observations(self.encode(rows), self.specification)
        with self.assertRaises(ValueError):
            validate_observations(self.encode(self.rows)[:-1], self.specification)

    def test_wrong_result_upper_bits_flags_and_adjacent_memory_are_rejected(self):
        for field in ["Return0", "Return2", "Return4", "ReturnFlags", "After0", "After1", "After3"]:
            rows = copy.deepcopy(self.rows)
            rows[0][field] ^= 0x100
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_observations(self.encode(rows), self.specification)

    def test_fault_cannot_publish_partial_register_or_memory_effects(self):
        for field in ["Context0", "Context1", "Context2", "Context3", "Context4", "ContextFlags",
                      "ContextPC", "ExceptionPC", "Return0", "Return2", "ReturnFlags", "After0"]:
            rows = copy.deepcopy(self.rows)
            rows[1][field] ^= 1
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_observations(self.encode(rows), self.specification)

    def test_invented_fault_metadata_and_wrong_access_kind_are_rejected(self):
        for index, field, value in [(0, "Code", 1), (0, "Parameter14", 1), (1, "Parameter0", 0),
                                     (1, "Parameter1", 0x9000), (1, "ParameterCount", 16),
                                     (1, "Faults", 2), (1, "Code", 0xc000001d)]:
            rows = copy.deepcopy(self.rows)
            rows[index][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_observations(self.encode(rows), self.specification)

    def test_complete_case_inventory_has_unique_ordered_scenarios(self):
        values, _, cases, scenarios = contract()
        required = list(required_records(cases, scenarios, values["Granule"]))
        self.assertEqual(len(cases), 168)
        self.assertEqual(len({name for name, *_ in cases}), 168)
        self.assertEqual(len(required), len({(index, scenario) for index, scenario, *_ in required}))
        self.assertEqual({index for index, *_ in required}, set(range(len(cases))))

    def test_digest_removes_only_placement_and_retains_real_state(self):
        expected = group_digests(validate_observations(self.encode(self.rows), self.specification))
        moved = copy.deepcopy(self.rows)
        for row in moved:
            for field in ["MemoryBase", "Return4"]:
                row[field] += 0x100000
            row["AtomicPC"] += 0x200000
            if row["Faults"]:
                for field in ["Context4", "Parameter1"]:
                    row[field] += 0x100000
                for field in ["ContextPC", "ExceptionPC"]:
                    row[field] += 0x200000
        observations = validate_observations(self.encode(moved), self.specification)
        self.assertEqual(group_digests(observations), expected)
        observations[0]["record"]["Return2"] ^= 1
        self.assertNotEqual(group_digests(observations), expected)


if __name__ == "__main__":
    unittest.main()
