"""Checked scalar stack arguments on live exception-object member calls."""
from pathlib import Path

if __package__:
    from .windows_registration_destructor_results import profile as result_profile
else:
    from windows_registration_destructor_results import profile as result_profile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_member_argument_driver.cpp"
TYPES = ROOT / "unittests/lift/eh/fixtures/registration_member_argument_types.h"
PROOF = ROOT / "unittests/lift/eh/RegistrationMemberArgumentTests.cpp"


def profile(validate_nested):
    result = result_profile(validate_nested)
    result.dependencies += (result.source, result.proof)
    result.source, result.types, result.proof = SOURCE, TYPES, PROOF
    result.test = "RegistrationMemberArguments.InputPE32BindsScalarStackWords"
    return result
