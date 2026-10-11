"""Re-lift destructor graphs after checking their transitive lifetime proof."""
if __package__:
    from .windows_registration_destructors_relift import profile as destructor_profile
    from .replay_windows_registration_nested_try import validate_capture
else:
    from windows_registration_destructors_relift import profile as destructor_profile
    from replay_windows_registration_nested_try import validate_capture


def validate_first(root, capture):
    if capture.get("profile") != "destructor-graphs":
        raise ValueError("generated destructor graphs require original transitive lifetime proofs")
    return validate_capture(root, capture)


def profile():
    result = destructor_profile()
    result.evidence = "relifted-destructor-graphs"
    result.validate_first = validate_first
    return result
