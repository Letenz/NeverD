"""Re-lift preserved ordinary constructors and their CRT lifetime callbacks."""
from __future__ import annotations

if __package__:
    from .windows_registration_copies_relift import profile as copy_profile, observe as observe_copy
    from .windows_registration_objects_relift import validate_installation as validate_objects
    from .replay_windows_registration_nested_try import validate_capture
    from .windows_registration_initializers import CASES
else:
    from windows_registration_copies_relift import profile as copy_profile, observe as observe_copy
    from windows_registration_objects_relift import validate_installation as validate_objects
    from replay_windows_registration_nested_try import validate_capture
    from windows_registration_initializers import CASES


def entry_context(case):
    if case not in CASES:
        raise ValueError("unknown generated initializer profile")
    return 0, 0, b"callback_parent"


def validate_first(root, capture):
    if capture.get("profile") != "initializers":
        raise ValueError("generated initializer evidence requires the original constructor proofs")
    return validate_capture(root, capture)


def validate_installation(original, product, receipt, first, case):
    entry_context(case)
    validate_objects(original, product, receipt, first, case)


def observe(path, case, route, receipt, launcher, env, timeout, *, source_section=".ndtext"):
    entry_context(case)
    return observe_copy(path, case, route, receipt, launcher, env, timeout,
                        source_section=source_section)


def profile():
    result = copy_profile()
    result.cases = CASES
    result.evidence = "relifted-initializer-reconstruction"
    result.entry_context = entry_context
    result.validate_first = validate_first
    result.validate_installation = validate_installation
    result.observe = observe
    return result
