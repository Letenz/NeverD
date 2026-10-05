"""Native exclusive evidence must contain complete, authentic observations."""
import struct
import unittest

from scripts.check_aarch64_exclusives import (
    DEFINITION, validate_alignment_observations, validate_observations,
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

    def test_alignment_context_requires_real_matching_fault_state(self):
        fields = re.findall(r"NEVERD_EXCLUSIVE_ORACLE_FIELD\((\w+)\)", DEFINITION.read_text())
        row = dict.fromkeys(fields, 0)
        row.update(CaseIndex=1, Code=0x80000002, FaultPC=0x4000, ExceptionPC=0x4000,
                   ContextPC=0x4000, ContextLow=0x1234, ContextStatus=0x5678)
        def encode(values):
            return struct.pack("<" + "Q" * len(fields), *(values[field] for field in fields))
        cases = [("byte", 1), ("pair", 8)]
        valid = encode(row)
        self.assertEqual(validate_alignment_observations(valid, cases, fields, 0x1234, 0x5678),
                         [{"case": "pair", "record": row}])
        for data in [valid[:-8], valid + bytes(8)]:
            with self.assertRaises(ValueError):
                validate_alignment_observations(data, cases, fields, 0x1234, 0x5678)
        for field, value in [("CaseIndex", 0), ("Code", 0), ("ExceptionPC", 0),
                             ("ContextPC", 0x4004), ("ContextLow", 0), ("ContextStatus", 0),
                             ("ParameterCount", 16), ("Parameter14", 1)]:
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_alignment_observations(encode(row | {field: value}), cases, fields, 0x1234, 0x5678)
