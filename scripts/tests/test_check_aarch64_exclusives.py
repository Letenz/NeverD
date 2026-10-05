"""Native exclusive evidence must contain complete, authentic observations."""
import struct
import unittest

from scripts.check_aarch64_exclusives import (
    DEFINITION, oracle_values, validate_alignment_observations, validate_observations,
)
import re


class ExclusiveEvidenceTests(unittest.TestCase):
    def test_complete_original_checks_are_accepted(self):
        self.assertEqual(validate_observations(struct.pack("<II", 1, 1), ["byte", "pair"]),
                         [{"case": "byte", "passed": True}, {"case": "pair", "passed": True}])

    def test_missing_extra_failed_or_duplicate_checks_are_not_passes(self):
        for data, names in [(b"", []), (b"", ["byte"]), (struct.pack("<II", 1, 1), ["byte"]),
                            (struct.pack("<II", 1, 0), ["byte", "pair"]),
                            (struct.pack("<II", 1, 2), ["byte", "pair"]),
                            (struct.pack("<II", 1, 1), ["byte", "byte"])]:
            with self.subTest(data=data, names=names), self.assertRaises(ValueError):
                validate_observations(data, names)

    def alignment_records(self):
        fields = re.findall(r"NEVERD_EXCLUSIVE_ORACLE_FIELD\((\w+)\)", DEFINITION.read_text())
        initial, updated, status = 0x0102030405060708, 0x1817161514131211, 0xfedcba9876543210
        original = struct.pack("<QQQQ", initial, updated, initial, updated)
        rows = []
        for mode in range(4):
            for scenario in range(5):
                split = scenario >= 3
                row = dict.fromkeys(fields, 0)
                loaded = (0x1817 if mode == 3 else 0x0818) if split else (
                    0x0708 if mode == 3 else 0x0607)
                row.update(CaseIndex=1, Offset=1, Mode=mode, MemoryScenario=scenario,
                           MemoryBase=0x8ff0 if split else 0x8000, LoadPC=0x4000, StorePC=0x4020,
                           LoadedLow=initial if mode == 1 else loaded,
                           LoadedHigh=updated, ReturnLow=updated, ReturnHigh=initial, ReturnStatus=1)
                after = bytearray(original)
                if mode == 0:
                    row.update(ReturnLow=loaded, ReturnHigh=updated, ReturnStatus=status)
                if mode == 2 and scenario == 0:
                    row["ReturnStatus"] = 0
                    after[1:3] = updated.to_bytes(8, "little")[:2]
                if mode != 1 and (scenario == 2 or (scenario == 4 and mode != 3)):
                    row.update(Faults=1, Code=0xc0000005, FaultPC=0x4000, ExceptionPC=0x4000,
                               ContextPC=0x4000, ContextLow=initial, ContextHigh=updated,
                               ContextStatus=status, LoadedLow=initial, ReturnLow=initial,
                               ReturnHigh=updated, ReturnStatus=status,
                               ContextAddress=row["MemoryBase"] + (14 if split else 0) + (mode != 3))
                row.update({f"After{i}": word for i, word in enumerate(struct.unpack("<QQQQ", after))})
                rows.append(row)
        return fields, rows, initial, updated, status

    def encode(self, fields, rows):
        return b"".join(struct.pack("<" + "Q" * len(fields), *(row[field] for field in fields))
                        for row in rows)

    def test_transparent_fixup_and_conditional_store_outcomes_are_observed(self):
        fields, rows, initial, updated, status = self.alignment_records()
        cases = [("byte", 1, 1), ("half", 2, 1)]
        observed = validate_alignment_observations(self.encode(fields, rows), cases, fields,
                                                   initial, updated, status)
        self.assertEqual(observed, [{"case": "half", "record": row} for row in rows])

    def test_fault_context_is_distinct_from_transparent_fixup(self):
        fields, rows, initial, updated, status = self.alignment_records()
        selected = oracle_values()["LoadStore"] * oracle_values()["MemoryScenarioCount"]
        rows[selected].update(Faults=1, FaultStage=1, Code=0x80000002, FaultPC=0x4020,
                       ExceptionPC=0x4020, ContextPC=0x4020, ContextLow=updated,
                       ContextHigh=initial, ContextStatus=status, ReturnStatus=status,
                       ContextAddress=0x8001,
                       After0=initial, After1=updated, After2=initial, After3=updated)
        cases = [("byte", 1, 1), ("half", 2, 1)]
        self.assertEqual(len(validate_alignment_observations(self.encode(fields, rows), cases,
                                                             fields, initial, updated, status)), 20)
        for field, value in [("FaultPC", 0), ("Code", 0), ("ContextPC", 0x4000),
                             ("ContextLow", 0), ("ContextHigh", 0), ("ContextStatus", 0),
                             ("ContextAddress", 0x8000),
                             ("ParameterCount", 16), ("Parameter14", 1)]:
            changed = list(rows)
            changed[selected] = rows[selected] | {field: value}
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_alignment_observations(self.encode(fields, changed), cases, fields,
                                                 initial, updated, status)

    def test_missing_inconsistent_or_wrong_memory_observations_fail(self):
        fields, rows, initial, updated, status = self.alignment_records()
        cases = [("byte", 1, 1), ("half", 2, 1)]
        encoded = self.encode(fields, rows)
        selected = oracle_values()["LoadStore"] * oracle_values()["MemoryScenarioCount"]
        for data in [encoded[:-8], encoded + bytes(8)]:
            with self.assertRaises(ValueError):
                validate_alignment_observations(data, cases, fields, initial, updated, status)
        for field, value in [("CaseIndex", 0), ("Offset", 0), ("Mode", 0), ("LoadPC", 0),
                             ("MemoryScenario", 1), ("MemoryBase", 0),
                             ("StorePC", 0x4000), ("ReturnStatus", 2), ("LoadedLow", 0),
                             ("ReturnHigh", 0), ("After0", 0), ("Code", 0x80000002),
                             ("Faults", 2), ("FaultStage", 1)]:
            changed = list(rows)
            changed[selected] = rows[selected] | {field: value}
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_alignment_observations(self.encode(fields, changed), cases, fields,
                                                 initial, updated, status)

    def test_fault_prefix_cannot_write_the_inaccessible_page_or_escape_the_operand(self):
        fields, rows, initial, updated, status = self.alignment_records()
        values = oracle_values()
        selected = values["LoadStore"] * values["MemoryScenarioCount"] + values["SplitReadOnlyMemory"]
        rows[selected].update(Faults=1, FaultStage=1, Code=0xc0000005, FaultPC=0x4020,
                              ExceptionPC=0x4020, ContextPC=0x4020, ContextLow=updated,
                              ContextHigh=initial, ContextStatus=status, ContextAddress=0x8fff,
                              ReturnStatus=status, After1=0x1117161514131211)
        cases = [("byte", 1, 1), ("half", 2, 1)]
        validate_alignment_observations(self.encode(fields, rows), cases, fields, initial, updated, status)
        for field, value in [("After0", 0), ("After1", 0), ("After2", 0x0102030405060712)]:
            changed = list(rows)
            changed[selected] = rows[selected] | {field: value}
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_alignment_observations(self.encode(fields, changed), cases, fields,
                                                 initial, updated, status)
