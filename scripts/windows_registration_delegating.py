"""Bounded constructor graphs in private helpers, parents and catch callbacks."""
from pathlib import Path

if __package__:
    from .windows_registration_initializers import profile as initializer_profile
else:
    from windows_registration_initializers import profile as initializer_profile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_delegating_driver.cpp"
PROOF = ROOT / "unittests/lift/eh/RegistrationInitializerGraphTests.cpp"


def profile(validate_nested):
    result = initializer_profile(validate_nested)
    result.dependencies += (result.source, result.proof)
    result.source, result.proof = SOURCE, PROOF
    result.test = "RegistrationInitializerGraph.InputPE32BindsTransitiveConstruction"
    return result
