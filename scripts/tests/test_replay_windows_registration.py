import copy
import unittest

from scripts.replay_windows_registration import source_records


class RegistrationSourceReplayTests(unittest.TestCase):
    def evidence(self, schema=2):
        labels = ["original", "patched", "product-patched", "collision-patched"]
        if schema == 2:
            labels += ["cli-section-ir", "cli-section-exe-no-opt", "cli-section-exe-opt",
                       "cli-inplace-ir", "cli-inplace-exe-no-opt", "cli-inplace-exe-opt"]
        else:
            labels += ["cli-section", "cli-inplace"]
        return {"schema": schema, "case": "longjmp-retired-inner-scope",
                "passed": True, "expected_trace": 23,
                "observations": [
                    {"image": label + suffix + ".exe", "generated": label != "original",
                     "runtime_base": base}
                    for label in labels
                    for suffix, base in (("", 0x400000), ("-rebased", 0x18000000))]}

    def test_current_and_legacy_execution_sets_are_explicit(self):
        for schema, count in ((1, 12), (2, 20)):
            with self.subTest(schema=schema):
                self.assertEqual(len(source_records(self.evidence(schema),
                                                    "longjmp-retired-inner-scope", 23)), count)

    def test_changed_or_incomplete_evidence_is_not_native_coverage(self):
        for mutation in range(9):
            with self.subTest(mutation=mutation):
                evidence = copy.deepcopy(self.evidence())
                if mutation == 0:
                    evidence["observations"].pop()
                elif mutation == 1:
                    evidence["observations"][-1] = evidence["observations"][0]
                elif mutation == 2:
                    evidence["observations"][0]["generated"] = True
                elif mutation == 3:
                    evidence["observations"][0]["runtime_base"] = 0x18000000
                elif mutation == 4:
                    evidence["observations"][0]["image"] = "unknown.exe"
                elif mutation == 5:
                    evidence["passed"] = False
                elif mutation == 6:
                    evidence["expected_trace"] = 0
                elif mutation == 7:
                    evidence["case"] = "longjmp-finally"
                else:
                    evidence["schema"] = 3
                with self.assertRaises(ValueError):
                    source_records(evidence, "longjmp-retired-inner-scope", 23)


if __name__ == "__main__":
    unittest.main()
