import unittest

from scripts.check_windows_registration_cookie import require_cookie_outcome
from scripts.check_windows_registration_frame import BANNER


class WindowsRegistrationCookieAdmissionTests(unittest.TestCase):
    def test_accepts_checked_execution_and_pre_dispatch_cookie_rejection(self):
        require_cookie_outcome({"exit_code": 0, "stdout": BANNER + "\n"}, False)
        require_cookie_outcome({"exit_code": 99, "stdout": ""}, True)

    def test_a_crash_cannot_substitute_for_cookie_rejection(self):
        for code in (None, 0, 1, -11, 0xc0000005):
            with self.subTest(code=code), self.assertRaises(ValueError):
                require_cookie_outcome({"exit_code": code, "stdout": ""}, True)

    def test_dispatch_after_corruption_is_not_success(self):
        with self.assertRaises(ValueError):
            require_cookie_outcome({"exit_code": 99, "stdout": BANNER}, True)

    def test_a_banner_cannot_hide_a_failed_valid_frame(self):
        with self.assertRaises(ValueError):
            require_cookie_outcome({"exit_code": 99, "stdout": BANNER}, False)

    def test_an_empty_success_is_not_callback_execution(self):
        with self.assertRaises(ValueError):
            require_cookie_outcome({"exit_code": 0, "stdout": ""}, False)


if __name__ == "__main__":
    unittest.main()
