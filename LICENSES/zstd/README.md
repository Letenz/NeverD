# Optional native Zstandard decoder

The Web backend can link the static native Zstandard 1.5.7 SDK when LLVM
Support was built without Zstd. The selected CMake target is
`zstd::libzstd_static`; the header and linked library version must both be
1.5.7. NeverD does not invoke the `zstd` executable or fetch/install a decoder.

Upstream release: [facebook/zstd v1.5.7](https://github.com/facebook/zstd/tree/v1.5.7).
The unmodified [BSD license](LICENSE) is preserved from that SDK. No Zstandard
implementation source is copied into NeverD's source tree. Distributed static
links include this notice beside the binaries, in the SDK and in installed
licenses.

`lib/web/compression/Zstd.cpp` is NeverD's independent C++ API adapter.
The existing owning format reader retains all frame, window, decoded-size
and content validation; selecting a library does not broaden those limits.
