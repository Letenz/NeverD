"""Re-lift a constructor graph only after authenticating its original proof."""
if __package__:
    from .windows_registration_initializers_relift import profile as initializer_profile
    from .replay_windows_registration_nested_try import validate_capture
else:
    from windows_registration_initializers_relift import profile as initializer_profile
    from replay_windows_registration_nested_try import validate_capture


def validate_first(root, capture):
    if capture.get("profile") != "delegating":
        raise ValueError("generated constructor graphs require the original transitive proofs")
    return validate_capture(root, capture)


def profile():
    result = initializer_profile()
    result.evidence = "relifted-delegating-construction"
    result.validate_first = validate_first
    return result
