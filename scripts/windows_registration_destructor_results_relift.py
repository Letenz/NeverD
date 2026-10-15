"""Re-lift destructor graphs with source-observed scalar member results."""
if __package__:
    from .windows_registration_destructors_relift import profile as destructor_profile
    from .replay_windows_registration_nested_try import validate_capture
else:
    from windows_registration_destructors_relift import profile as destructor_profile
    from replay_windows_registration_nested_try import validate_capture


def validate_first(root, capture):
    if capture.get("profile") != "destructor-results":
        raise ValueError("generated destructor graphs require original transitive lifetime proofs")
    return validate_capture(root, capture)


def profile():
    result = destructor_profile()
    result.evidence = "relifted-destructor-results"
    result.validate_first = validate_first
    return result
