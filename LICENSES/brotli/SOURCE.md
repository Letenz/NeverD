# Brotli decoder SDK

The optional in-process decoder is the unmodified static Brotli **1.2.0** SDK
from [google/brotli v1.2.0](https://github.com/google/brotli/tree/v1.2.0).
It is not bundled, fetched, invoked as an executable or used to run targets.
CMake requires the exact pkg-config version and static decoder/common archives;
the wrapper checks the runtime version before admitting output.

`LICENSE` preserves the Brotli Authors' MIT notice. NeverD's C++ wrapper adds
private allocation accounting, bounded stream calls/output, a shared deadline
and exact input-consumption checks. No encoder enters the product.
