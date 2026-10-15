"""Destructor graphs whose observable trace consumes a scalar child result."""
from pathlib import Path

if __package__:
    from .windows_registration_destructor_graphs import profile as graph_profile
else:
    from windows_registration_destructor_graphs import profile as graph_profile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_destructor_result_driver.cpp"
TYPES = ROOT / "unittests/lift/eh/fixtures/registration_destructor_result_types.h"


def profile(validate_nested):
    result = graph_profile(validate_nested)
    result.dependencies += (result.source,)
    result.source, result.types = SOURCE, TYPES
    return result
