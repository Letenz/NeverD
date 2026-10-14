"""Ordinary scalar member results borrowed from live CRT exception objects."""
from pathlib import Path

if __package__:
    from .windows_registration_destructor_results import profile as result_profile
else:
    from windows_registration_destructor_results import profile as result_profile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_reference_member_driver.cpp"
PROOF = ROOT / "unittests/lift/eh/RegistrationRuntimeMemberTests.cpp"


def profile(validate_nested):
    result = result_profile(validate_nested)
    result.dependencies += (result.source, result.proof)
    result.source, result.proof = SOURCE, PROOF
    result.test = "RegistrationRuntimeMember.InputPE32BindsReferenceReceiver"
    return result
