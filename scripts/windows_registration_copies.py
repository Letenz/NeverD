"""PE32 nontrivial copies with independently checked source and destination."""
from __future__ import annotations

if __package__:
    from .windows_registration_destructors import (
        ROOT, SHARED_DRIVER, expected_lifetime, profile as destructor_profile)
    from .check_windows_registration_nested_try import observe as observe_nested, search_context
else:
    from windows_registration_destructors import (
        ROOT, SHARED_DRIVER, expected_lifetime, profile as destructor_profile)
    from check_windows_registration_nested_try import observe as observe_nested, search_context

SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_copy_driver.cpp"
TYPES = ROOT / "unittests/lift/eh/fixtures/registration_copy_types.h"
PROOF = ROOT / "unittests/lift/eh/RegistrationCopyTests.cpp"


def expected_copy(case):
    event = 2201 if search_context(case)["rethrow_search"] else 1086
    trace = 0
    for _ in range(4):
        trace = ((trace * 33) ^ event) & 0xffffffff
    return 4, trace


def observe(path, case, route, receipt, launcher, env, timeout):
    return observe_nested(path, case, route, receipt, launcher, env, timeout,
                          expected_lifetime=expected_lifetime(case),
                          expected_copy=expected_copy(case))


def profile(validate_nested):
    result = destructor_profile(validate_nested)
    result.source, result.types, result.proof = SOURCE, TYPES, PROOF
    result.test = "RegistrationCopy.InputPE32BindsRuntimeConstruction"
    result.definitions = lambda control: ["-DEXPECTED_COPIES=" + str(5 if control else 4)]
    result.observe = observe
    return result
