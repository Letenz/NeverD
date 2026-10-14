"""Re-lift preserved ordinary constructors and their CRT lifetime callbacks."""
from __future__ import annotations

if __package__:
    from .windows_registration_copies_relift import profile as copy_profile
    from .windows_registration_objects_relift import validate_object_installation
    from .replay_windows_registration_nested_try import validate_capture
    from .windows_registration_initializers import (
        CASES, expected_copy, expected_lifetime, expected_values)
    from .check_windows_registration_multiple_catch import observe as observe_catches
else:
    from windows_registration_copies_relift import profile as copy_profile
    from windows_registration_objects_relift import validate_object_installation
    from replay_windows_registration_nested_try import validate_capture
    from windows_registration_initializers import (
        CASES, expected_copy, expected_lifetime, expected_values)
    from check_windows_registration_multiple_catch import observe as observe_catches


def entry_context(case):
    if case not in CASES:
        raise ValueError("unknown generated initializer profile")
    return 0, 0, b"callback_parent"


def validate_first(root, capture):
    if capture.get("profile") != "initializers":
        raise ValueError("generated initializer evidence requires the original constructor proofs")
    return validate_capture(root, capture)


def validate_installation(original, product, receipt, first, case):
    _, _, export = entry_context(case)
    validate_object_installation(original, product, receipt, first, case, export)


def observe(path, case, route, receipt, launcher, env, timeout, *, source_section=".ndtext"):
    entry_context(case)
    return observe_catches(path, case, route, receipt, launcher, env, timeout,
                           expected_values=expected_values(case),
                           expected_lifetime=expected_lifetime(case),
                           expected_copy=expected_copy(case), source_section=source_section)


def profile():
    result = copy_profile()
    result.cases = CASES
    result.evidence = "relifted-initializer-reconstruction"
    result.entry_context = entry_context
    result.validate_first = validate_first
    result.validate_installation = validate_installation
    result.observe = observe
    return result
