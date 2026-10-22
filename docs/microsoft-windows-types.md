# Local Microsoft Windows type libraries

Windows x86, x64 and ARM64 user and kernel libraries can be generated from Microsoft's
SDK/WDK, with the bundled ReactOS, mingw-w64 and phnt declarations supplementing
missing names. Microsoft's declarations win every name collision. Program debug
types and explicit user types still take precedence over either system source.

Generate the libraries outside the repository using an already-built NeverD:

```sh
python3 scripts/generate_microsoft_windows_types.py \
  --architecture all \
  --cache /path/to/package-cache --output /path/to/microsoft-windows \
  --neverd build-release/bin/neverd
cmake -S . -B build-release \
  -DNEVERD_WINDOWS_TYPES_DIRECTORY=/path/to/microsoft-windows
python3 scripts/build.py build-release --target neverd neverd_shared --parallel
```

The default `--architecture all` produces six libraries and a combined manifest.
Use `--architecture x86`, `x64` or `arm64` to replace only that target's two
libraries. The combined manifest requires all three targets, matching source
packages and compiler versions, and unchanged header and license hashes.

The generator verifies the official NuGet package hashes already owned by
`scripts/WDKDriverFixtures.def`. It preprocesses architecture-conditional headers
for `i686-pc-windows-msvc`, `x86_64-pc-windows-msvc` or
`aarch64-pc-windows-msvc`, with the corresponding target macro and data model.
The WDK package's `x64` host-tool label does not select AMD64 declarations. It
uses Microsoft's CRT headers, never the host CRT.
The selected kernel and user providers are listed in the generator. These are
not an inventory of every Windows export, every SDK header or every OS version.

The x86 kernel unit also reads the WDK's `WindowsDriver.Common.props` and
preserves its default `StdCall` compiler setting. Preprocessing cannot encode
that setting in declarations that omit an explicit convention. The generator
therefore supplies `#pragma neverd_default_calling_convention(__stdcall)` to
NeverD's parser and the matching compiler option to the independent Clang
check. Explicit conventions take precedence, variadic functions use `cdecl`,
and rendered callbacks and prototypes carry their conventions explicitly.
An empty pragma resets the source default; it never leaks into another parse.
User libraries and 64-bit targets retain their own defaults.

Generation rejects parser errors, missing declarations on round trip and named
record layout differences against Clang reading the original Microsoft headers.
The emitted records also compile with size and offset assertions. Fixed-width
`__ptr32` and `__ptr64` pointers survive C/C++ round trips; Rust, Go and Python
represent foreign-width addresses as integers. These checks establish type
layouts, not native ARM64 driver execution or ARM64EC/ARM64X qualification.
Microsoft required alignment survives intermediate records, unions and arrays,
including a later packed parent and a rendered declaration round trip.

The primary library and its compatibility supplement are embedded at build
time and parsed once on first use. There is no SDK directory scan during
analysis. `mergeHeaderTypes` supplies only missing names, leaves records with
incompatible by-value dependencies opaque, and omits prototypes whose named
dependencies or by-value size/alignment no longer agree. Official public PEB
and TEB definitions retain their reserved fields; private supplements do not
replace them with larger definitions.

The build validates each generated header's hash against `manifest.json` and
records the compatibility headers' hashes in `neverd-windows-types-inputs.json`.
The build receipt records that file's hash. Package, input header, compiler,
command and license information remains available in the manifest. Original
Microsoft headers and generated declarations are local inputs under Microsoft's
terms and must not be committed as redistributable compatibility headers.

Without `NEVERD_WINDOWS_TYPES_DIRECTORY`, builds use the bundled compatibility
libraries. A single-target manifest changes only its two libraries; a combined
manifest changes all six. Other targets retain their existing sources.

Focused checks:

```sh
python3 -m unittest discover -s scripts/tests -p 'test_generate*windows_types.py'
python3 scripts/build.py build-release --target NeverDTypesTests \
  NeverDSessionCAPITests NeverDAAPCS64CallContractTests --parallel
build-release/bin/NeverDTypesTests
build-release/bin/NeverDSessionCAPITests --gtest_filter='SessionCAPITest.*Imports*'
build-release/bin/NeverDAAPCS64CallContractTests
```

Run the type suite both with and without the optional official input. The
public ARM64 driver fixture exercises the NDIS import contracts through both
HighC and LLVMC. Export names alone never supply parameter types.
