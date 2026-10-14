"""PE32 identity-copy objects with independently checked destructor effects."""
from __future__ import annotations

if __package__:
    from .windows_registration_objects import ROOT, SOURCE as SHARED_DRIVER, profile as object_profile
    from .check_windows_registration_nested_try import observe as observe_nested, search_context
else:
    from windows_registration_objects import ROOT, SOURCE as SHARED_DRIVER, profile as object_profile
    from check_windows_registration_nested_try import observe as observe_nested, search_context

SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_destructor_driver.cpp"
TYPES = ROOT / "unittests/lift/eh/fixtures/registration_destructor_types.h"
PROOF = ROOT / "unittests/lift/eh/RegistrationThrowObjectTests.cpp"


def expected_lifetime(case):
    events = (318, 669, 665) if search_context(case)["rethrow_search"] else (322, 318, 665)
    trace = 0
    for event in events * 4:
        trace = ((trace * 33) ^ event) & 0xffffffff
    return 12, trace


def observe(path, case, route, receipt, launcher, env, timeout):
    return observe_nested(path, case, route, receipt, launcher, env, timeout,
                          expected_lifetime=expected_lifetime(case))


def profile(validate_nested):
    result = object_profile(validate_nested)
    result.source, result.types, result.proof = SOURCE, TYPES, PROOF
    result.dependencies = (SHARED_DRIVER,)
    result.test = "RegistrationThrowObject.InputPE32BindsDestruction"
    result.control_first = False
    result.definitions = lambda control: ["-DEXPECTED_DESTRUCTIONS=" + str(13 if control else 12)]
    result.observe = observe
    return result
