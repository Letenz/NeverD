import copy
import unittest

from scripts.replay_windows_registration import source_cases, source_records


class RegistrationSourceReplayTests(unittest.TestCase):
    def test_older_compiler_profiles_do_not_claim_callback_state_execution(self):
        current, r4, legacy = (source_cases(p) for p in ("all", "r4", "seh3-eh4"))
        self.assertEqual(len(r4), 48)
        self.assertEqual(len(legacy), 39)
        r5 = source_cases("r5")
        self.assertEqual(len(r5), 64)
        eh4_jumps = {name for name in current
                     if name.startswith("eh4-") and "longjmp-" in name}
        self.assertEqual(len(current), 109)
        self.assertEqual(len(eh4_jumps), 40)
        for prefix in ("eh4-", "eh4-gs-"):
            for case in ("longjmp-callee", "longjmp-returning-callee",
                         "longjmp-scalar-callee"):
                self.assertEqual(current[prefix + case], current[case])
            for kind in ("callee", "returning-callee", "scalar-callee"):
                self.assertEqual(current[prefix + "longjmp-eh4-" + kind],
                                 current["longjmp-" + kind])
        self.assertEqual(set(current) - set(r5) - eh4_jumps, {
            "longjmp-shared-parent-call", "shared-parent-throwing-call",
            "longjmp-private-parent-call", "longjmp-private-parent-graph",
            "longjmp-multistate-capture"})
        self.assertEqual(set(current) - set(r4) - eh4_jumps, {
            "longjmp-state-activate-call", "longjmp-state-activate-unwind",
            "longjmp-state-retire", "longjmp-state-sentinel",
            "longjmp-state-byte", "longjmp-state-word",
            "longjmp-filter-correlated-state", "longjmp-filter-shared-state",
            "longjmp-filter-captured-cursor",
            "longjmp-filter-negative-disposition",
            "longjmp-filter-skipped-scope",
            "longjmp-filter-two-resume-mappings",
            "longjmp-filter-callback-activation", "longjmp-state-byte-high-bits",
            "longjmp-finally-incidental-result", "longjmp-deep-scope-chain",
            "longjmp-shared-parent-call", "shared-parent-throwing-call",
            "longjmp-private-parent-call", "longjmp-private-parent-graph",
            "longjmp-multistate-capture"})
        self.assertTrue(all(name.startswith("eh4-gs-") for name in set(r4) - set(legacy)))
        with self.assertRaises(ValueError):
            source_cases("unknown")

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
