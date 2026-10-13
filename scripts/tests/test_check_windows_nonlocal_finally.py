import struct
import unittest

from scripts import check_windows_nonlocal_finally as runner


def result(values):
    return {"exit_code": 0, "stdout": "NONLOCAL_FINALLY " +
            " ".join(map(str, values[:6])) +
            f" guards={values[6]} chain={values[7]} consistent={values[8]} "
            f"iterations={values[9]} " +
            "reenter=" + ",".join(map(str, values[10:14])) +
            f" counters={values[14]},{values[15]} " +
            f"immediate_guards={values[16]} immediate_chain={values[17]} " +
            f"reentries={values[18]}\n"}


class NonlocalFinallyEvidenceTests(unittest.TestCase):
    def test_checks_every_exit_and_runtime_guard(self):
        values = runner.EXPECTED + runner.RUNTIME_CHECKS
        self.assertEqual(runner.observe(result(values)), values)
        for index in range(len(values)):
            changed = list(values)
            changed[index] += 1
            with self.subTest(index=index), self.assertRaises(ValueError):
                runner.observe(result(changed))
        for changed in ({"exit_code": 1}, {"stdout": ""},
                        {"stdout": result(values)["stdout"] + "extra"}):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                runner.observe(result(values) | changed)

    def test_control_cannot_pass_as_original(self):
        control = result(runner.CONTROL + runner.RUNTIME_CHECKS)
        runner.observe(control, True)
        with self.assertRaises(ValueError):
            runner.observe(control)
        with self.assertRaises(ValueError):
            runner.observe(result(runner.EXPECTED + runner.RUNTIME_CHECKS), True)

    def test_legacy_observation_does_not_claim_reentry_coverage(self):
        old = {"exit_code": 0, "stdout": "NONLOCAL_FINALLY " +
               " ".join(map(str, runner.EXPECTED)) +
               " guards=1 chain=1 consistent=1 iterations=192\n"}
        with self.assertRaises(ValueError):
            runner.observe(old)

    def test_native_compiler_must_match_the_requested_target(self):
        for arch, machine, magic in (("x86", 0x14C, 0x10B), ("x64", 0x8664, 0x20B)):
            image = bytearray(128)
            image[:2] = b"MZ"
            struct.pack_into("<I", image, 60, 64)
            image[64:68] = b"PE\0\0"
            struct.pack_into("<H", image, 68, machine)
            struct.pack_into("<H", image, 88, magic)
            runner.require_architecture(image, arch)
            with self.assertRaises(ValueError):
                runner.require_architecture(image, "x64" if arch == "x86" else "x86")
            for offset in (0, 60, 64, 68, 88):
                changed = bytearray(image)
                changed[offset] ^= 0xFF
                with self.subTest(arch=arch, offset=offset), self.assertRaises(ValueError):
                    runner.require_architecture(changed, arch)
            with self.assertRaises(ValueError):
                runner.require_architecture(image[:89], arch)


if __name__ == "__main__":
    unittest.main()
