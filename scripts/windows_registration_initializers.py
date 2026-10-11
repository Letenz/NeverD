"""Ordinary scalar construction before the CRT owns an exception object."""
from pathlib import Path

if __package__:
    from .windows_registration_copies import SOURCE as COPY_SOURCE, SHARED_DRIVER, profile as copy_profile
else:
    from windows_registration_copies import SOURCE as COPY_SOURCE, SHARED_DRIVER, profile as copy_profile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_initializer_driver.cpp"
TYPES = ROOT / "unittests/lift/eh/fixtures/registration_copy_types.h"
PROOF = ROOT / "unittests/lift/eh/RegistrationInitializerTests.cpp"
FORMS = {family + "-" + mode + "-llvm-fixed": "-" + mode.upper()
         for family in ("nested", "rethrow") for mode in ("o0", "o1")}
CASES = tuple(name + suffix for name in FORMS for suffix in ("", "-control"))


def profile(validate_nested):
    result = copy_profile(validate_nested)
    result.source, result.types, result.proof = SOURCE, TYPES, PROOF
    result.forms, result.cases = FORMS, CASES
    result.dependencies = (COPY_SOURCE, SHARED_DRIVER)
    result.test = "RegistrationInitializer.InputPE32BindsSourceConstruction"
    return result
