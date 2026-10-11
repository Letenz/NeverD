"""Reconstruct a second PE32 generation while retaining copy construction."""
from __future__ import annotations

if __package__:
    from .windows_registration_destructors_relift import profile as destructor_profile
    from .windows_registration_objects_relift import entry_context
    from .check_windows_registration_nested_try import search_context
    from .check_windows_registration_multiple_catch import observe as observe_catches
    from .replay_windows_registration_nested_try import validate_capture
    from .windows_registration_copies import expected_copy, expected_lifetime
else:
    from windows_registration_destructors_relift import profile as destructor_profile
    from windows_registration_objects_relift import entry_context
    from check_windows_registration_nested_try import search_context
    from check_windows_registration_multiple_catch import observe as observe_catches
    from replay_windows_registration_nested_try import validate_capture
    from windows_registration_copies import expected_copy, expected_lifetime


def validate_first(root, capture):
    if capture.get("profile") != "copies":
        raise ValueError("generated copy evidence requires the original construction proofs")
    return validate_capture(root, capture)


def observe(path, case, route, receipt, launcher, env, timeout, *, source_section=".ndtext"):
    entry_context(case)
    expected = ((39, 28, 39, 39, 18, 39, 1, 12) if search_context(case)["rethrow_search"]
                else (17, 28, 39, 7, 18, 39, 1, 12))
    return observe_catches(path, case, route, receipt, launcher, env, timeout,
                           expected_values=expected, expected_lifetime=expected_lifetime(case),
                           expected_copy=expected_copy(case), source_section=source_section)


def profile():
    result = destructor_profile()
    result.evidence = "relifted-copy-reconstruction"
    result.validate_first = validate_first
    result.observe = observe
    return result
