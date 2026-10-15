"""Re-lift checked scalar member arguments with independent provenance."""
if __package__:
    from .windows_registration_destructors_relift import profile as destructor_profile
    from .replay_windows_registration_nested_try import validate_capture
else:
    from windows_registration_destructors_relift import profile as destructor_profile
    from replay_windows_registration_nested_try import validate_capture


def validate_first(root, capture):
    if capture.get("profile") != "member-arguments":
        raise ValueError("generated member arguments require original scalar-word proofs")
    return validate_capture(root, capture)


def profile():
    result = destructor_profile()
    result.evidence = "relifted-member-arguments"
    result.validate_first = validate_first
    return result
