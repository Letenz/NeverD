import unittest
from pathlib import Path
from unittest.mock import patch

from scripts import check_windows_native_seh_resume as runner


def observation(values):
    return {"exit_code": 0, "stdout": "NATIVE_SEH_RESUME " +
            " ".join(map(str, values[:4])) +
            f" registers={values[4]} stack={values[5]} guards={values[6]} " +
            f"consistent={values[7]} iterations={values[8]} " +
            "runtime=C:\\runtime\\vcruntime140.dll\n"}


class NativeResumeEvidenceTests(unittest.TestCase):
    def test_coff_inputs_are_linked_without_source_language_override(self):
        directory = Path("build") / "directory with spaces"
        source = directory / "runtime.c"
        objects = [directory / name for name in ("body.obj", "bridge.obj", "probe.obj")]
        image = directory / "resume.exe"
        with patch.object(runner, "compile_step") as compile_step:
            runner.compile_and_link_c_runtime(
                "cl", source, objects, image, directory, {}, ["/O2", "/GS", "/MD", "/Z7"])
        self.assertEqual(compile_step.call_count, 2)
        compile_command, link_command = [call.args[0] for call in compile_step.call_args_list]
        self.assertIn("/TC", compile_command)
        self.assertIn("/c", compile_command)
        self.assertIn(str(source), compile_command)
        self.assertFalse(any(item in compile_command for item in map(str, objects)))
        self.assertNotIn(str(source), link_command)
        self.assertNotIn("/TC", link_command)
        self.assertNotIn("/TP", link_command)
        self.assertNotIn("/c", link_command)
        self.assertIn(str(directory / "runtime.obj"), link_command)
        for item in objects:
            self.assertEqual(link_command.count(str(item)), 1)
        self.assertIn("/link", link_command)

    def test_checks_normal_and_abandoned_suffixes_and_native_abi(self):
        self.assertEqual(runner.observe(observation(runner.EXPECTED)), runner.EXPECTED)
        for index in range(len(runner.EXPECTED)):
            changed = list(runner.EXPECTED)
            changed[index] += 1
            with self.subTest(index=index), self.assertRaises(ValueError):
                runner.observe(observation(changed))

    def test_missing_second_exception_observation_cannot_pass(self):
        first_only = (12347, 1237, 1367, 137, 1, 1, 1, 1, 128)
        with self.assertRaises(ValueError):
            runner.observe(observation(first_only))

    def test_stale_resume_token_cannot_skip_a_later_ordinary_suffix(self):
        stale = (1234782349, 12378239, 136782349, 1378239, 1, 1, 1, 1, 128)
        with self.assertRaises(ValueError):
            runner.observe(observation(stale))

    def test_incomplete_and_extra_output_cannot_pass(self):
        valid = observation(runner.EXPECTED)
        for changed in ({"exit_code": 1}, {"stdout": ""},
                        {"stdout": valid["stdout"] + "extra"},
                        {"stdout": valid["stdout"].replace("registers=1", "registers=0")}):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                runner.observe(valid | changed)


if __name__ == "__main__":
    unittest.main()
