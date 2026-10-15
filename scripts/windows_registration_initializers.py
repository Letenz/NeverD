"""Ordinary scalar construction before the CRT owns an exception object."""
from pathlib import Path

if __package__:
    from .windows_registration_copies import (
        SOURCE as COPY_SOURCE, SHARED_DRIVER, profile as copy_profile,
        expected_copy as copy_trace, expected_lifetime as destruction_trace)
    from .check_windows_registration_nested_try import search_context, validate_search_context
    from .check_windows_registration_multiple_catch import observe as observe_catches
else:
    from windows_registration_copies import (
        SOURCE as COPY_SOURCE, SHARED_DRIVER, profile as copy_profile,
        expected_copy as copy_trace, expected_lifetime as destruction_trace)
    from check_windows_registration_nested_try import search_context, validate_search_context
    from check_windows_registration_multiple_catch import observe as observe_catches

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_initializer_driver.cpp"
TYPES = ROOT / "unittests/lift/eh/fixtures/registration_copy_types.h"
PROOF = ROOT / "unittests/lift/eh/RegistrationInitializerTests.cpp"
EDIT_PROOF = ROOT / "unittests/lift/eh/RegistrationInitializerTestUtils.cpp"
FORMS = {family + "-" + mode + "-llvm-fixed": "-" + mode.upper()
         for family in ("nested", "rethrow", "direct-nested", "direct-rethrow", "direct-secondary")
         for mode in ("o0", "o1")}
CASES = tuple(name + suffix for name in FORMS for suffix in ("", "-control"))


def expected_values(case):
    return ((39, 28, 39, 39, 18, 39, 1, 12) if search_context(case)["secondary_search"]
            else (17, 28, 39, 7, 18, 39, 1, 12))


def expected_copy(case):
    if not case.startswith("direct-secondary-"):
        return copy_trace(case)
    trace = 0
    for _ in range(4):
        trace = ((trace * 33) ^ 2201) & 0xffffffff
    return 4, trace


def expected_lifetime(case):
    if not case.startswith("direct-secondary-"):
        return destruction_trace(case)
    trace = 0
    for event in (318, 665, 669, 665) * 4:
        trace = ((trace * 33) ^ event) & 0xffffffff
    return 16, trace


def observe(path, case, route, receipt, launcher, env, timeout):
    validate_search_context(receipt, case)
    return observe_catches(path, case, route, receipt, launcher, env, timeout,
                           expected_values=expected_values(case),
                           expected_lifetime=expected_lifetime(case),
                           expected_copy=expected_copy(case))


def profile(validate_nested):
    result = copy_profile(validate_nested)
    result.source, result.types, result.proof = SOURCE, TYPES, PROOF
    result.forms, result.cases = FORMS, CASES
    result.dependencies = (COPY_SOURCE, SHARED_DRIVER, EDIT_PROOF)
    result.test = "RegistrationInitializer.InputPE32BindsSourceConstruction"
    result.expected_values = expected_values
    result.observe = observe
    return result
