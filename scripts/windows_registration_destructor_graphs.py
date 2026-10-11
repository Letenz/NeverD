"""Returning destructor graphs with observable catch and throw lifetimes."""
from pathlib import Path

if __package__:
    from .windows_registration_destructors import profile as destructor_profile
else:
    from windows_registration_destructors import profile as destructor_profile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_destructor_graph_driver.cpp"
TYPES = ROOT / "unittests/lift/eh/fixtures/registration_destructor_graph_types.h"
PROOF = ROOT / "unittests/lift/eh/RegistrationObjectGraphTests.cpp"


def profile(validate_nested):
    result = destructor_profile(validate_nested)
    result.dependencies += (result.proof,)
    result.source, result.types, result.proof = SOURCE, TYPES, PROOF
    result.test = "RegistrationObjectGraph.InputPE32BindsTransitiveDestruction"
    return result
