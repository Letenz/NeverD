import unittest

from scripts import check_windows_native_seh_bridge as runner


def observation(values):
    return {"exit_code": 0, "stdout": "NATIVE_SEH_BRIDGE " +
            " ".join(map(str, values[:8])) +
            f" registers={values[8]} stack={values[9]} guards={values[10]} " +
            f"callbacks={values[11]} codes={values[12]} consistent={values[13]} " +
            f"iterations={values[14]} runtime=C:\\runtime\\vcruntime140.dll " +
            "unwind=C:\\runtime\\ucrtbase.dll handler=C:\\runtime\\ucrtbase.dll\n"}


class NativeBridgeEvidenceTests(unittest.TestCase):
    def test_every_suffix_and_independent_observer_is_required(self):
        self.assertEqual(runner.observe(observation(runner.EXPECTED)), runner.EXPECTED)
        for index in range(len(runner.EXPECTED)):
            changed = list(runner.EXPECTED)
            changed[index] += 1
            with self.subTest(index=index), self.assertRaises(ValueError):
                runner.observe(observation(changed))

    def test_callback_or_code_failure_cannot_hide_behind_correct_traces(self):
        for index in (11, 12):
            changed = list(runner.EXPECTED)
            changed[index] = 0
            with self.subTest(index=index), self.assertRaises(ValueError):
                runner.observe(observation(changed))

    def test_incomplete_or_extra_runtime_output_is_rejected(self):
        valid = observation(runner.EXPECTED)
        for changed in ({"exit_code": 1}, {"stdout": ""},
                        {"stdout": valid["stdout"] + "extra"},
                        {"stdout": valid["stdout"].split(" unwind=")[0]}):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                runner.observe(valid | changed)

    def test_unrelated_crash_cannot_pass_callback_or_code_controls(self):
        for name, index in (("abnormal-byte-alias", 11),
                            ("unexpected-before-correct-code", 12)):
            changed = list(runner.EXPECTED)
            changed[index] = 0
            runner.observe_negative(name, observation(changed) | {"exit_code": 1})
            for result in (observation(runner.EXPECTED),
                           {"exit_code": -1, "stdout": ""},
                           observation(changed) | {"exit_code": -1}):
                with self.subTest(name=name, result=result), self.assertRaises(ValueError):
                    runner.observe_negative(name, result)

    def test_shifted_frame_requires_execution_and_rejects_timeouts(self):
        started = {"exit_code": -1073741784, "stdout": "",
                   "stderr": "NATIVE_SEH_BRIDGE_STARTED\n"}
        runner.observe_negative("shifted-target-frame", started)
        for changed in ({"stderr": ""}, {"exit_code": 0}, {"exit_code": 2},
                        {"error": "timeout after 30s"}, {"error": "launch failed"}):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                runner.observe_negative("shifted-target-frame", started | changed)


if __name__ == "__main__":
    unittest.main()
