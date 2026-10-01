# Third-Party Notices

## Z3

The optional solver backend (`NEVERD_ENABLE_Z3`) uses Z3 under the MIT license.
The default FetchContent provider builds the unmodified 4.13.3 sources at
revision `54d30f26f72ce62f5dcb5a5258f632f84858714f`; a system-library provider is
also available. The original copyright and license are preserved in
[LICENSES/Z3.txt](LICENSES/Z3.txt) and staged under `licenses/z3` beside enabled
binaries and in the SDK. See the [upstream source](https://github.com/Z3Prover/z3/tree/54d30f26f72ce62f5dcb5a5258f632f84858714f).

## Unicorn Engine

The optional Windows driver emulator (`NEVERD_ENABLE_DRIVER_EMULATION`) links
the repository's pinned `third_party/unicorn` CPU engine. Semantic tests also
use this dependency. Unicorn is based on QEMU; its README identifies the
engine's license as GPLv2. Component-specific notices and licenses remain in
the original source files.

See [Unicorn's license](third_party/unicorn/COPYING),
[authors](third_party/unicorn/AUTHORS.TXT),
[credits](third_party/unicorn/CREDITS.TXT),
[third-party notices](third_party/unicorn/THIRD_PARTY_NOTICES.md), and
[QEMU's licensing description](third_party/unicorn/qemu/LICENSE).
Enabled builds stage these notices and the dependency's GPL/LGPL texts under
`licenses/unicorn` beside the binaries and in the staged SDK. NeverD's project
license does not replace these dependency notices.

The pinned NeverSight fork includes the original 2026-09-30 exception-delivery
fix by NeverD contributors: x86 interrupt hooks acknowledge the in-flight
exception before resumption, preventing unrelated later faults from becoming
double faults. The change and original regression tests are preserved in
[commit cf40fa2c](https://github.com/NeverSight/unicorn/commit/cf40fa2c3ccdd90ab7cbb4ff060d9e07921aa3ae).
The modified QEMU file retains its original LGPL notice.

The fork also includes the original 2026-09-30 SSE denormal-status fix by NeverD
contributors. The x86 arithmetic helpers report MXCSR.DE for subnormal inputs
without changing the shared SoftFloat model for other architectures, and retain
NaN, divide-by-zero and negative-square-root priority. Original regressions and
the dated modification notice are preserved in
[commit 16c0b3fd](https://github.com/NeverSight/unicorn/commit/16c0b3fd9486b598287ad79553768ed44805e217).
The modified SSE helper retains its original LGPL notice. The instruction
semantics were checked against the
[Intel floating-point reference](https://www.intel.com/content/www/us/en/developer/articles/technical/floating-point-reference-sheet-for-intel-architecture.html)
and independently executed host instructions; no reference implementation was
copied into NeverD.

The fork includes the original 2026-09-30 pre-entry cancellation fix by NeverD
contributors. The TCG entry boundary honors an engine stop even when CPU startup
has reset its exit flag. Original x64, ARM32 and ARM64 regressions control only
thread scheduling and public engine APIs, and check zero guest effects and an
independent subsequent run. The original QEMU notice and dated modification
notice are preserved in
[commit 9cbcf76a](https://github.com/NeverSight/unicorn/commit/9cbcf76a1eb74d200dca6a9d6cee3200e47e9743).

## Swift runtime ABI declarations

`lib/loader/Swift/SwiftRuntimeDeclarations.inc` derives fixed C and Swift ABI declarations
from Swift's `include/swift/Runtime/RuntimeFunctions.def`, revision
`9215272a4725957dfabdd14e1ca76c0dfa4a3003`.

Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors.
The applicable Apache 2.0 license with Runtime Library Exception is included
in [LICENSES/Swift-Runtime-ABI.txt](LICENSES/Swift-Runtime-ABI.txt).
NeverD's generator extracts pointer, size, 32-bit integer and explicitly
zero-extended Boolean result carriers, together with explicit
nonreturning declarations, as recorded in the generated file. No Swift
runtime implementation is included. Two-word Swift results retain their
declared members. The metadata response layout comes from
[`IRGenModule.cpp:295-298`](https://github.com/swiftlang/swift/blob/9215272a4725957dfabdd14e1ca76c0dfa4a3003/lib/IRGen/IRGenModule.cpp#L295)
and [`Metadata.h:99-113`](https://github.com/swiftlang/swift/blob/9215272a4725957dfabdd14e1ca76c0dfa4a3003/include/swift/ABI/Metadata.h#L99)
at the same revision. The fixed Swift subset excludes the custom
parameter attributes assigned to `swift_willThrow` by
[`IRGenModule.cpp`](https://github.com/swiftlang/swift/blob/9215272a4725957dfabdd14e1ca76c0dfa4a3003/lib/IRGen/IRGenModule.cpp#L1127).

## richprint @comp.id table

`lib/loader/COFF/RichCompIds.inc` derives the tool kind, Visual Studio year
and description of each @comp.id record from richprint's
[`comp_id.txt`](https://github.com/dishather/richprint/blob/49f2dc93504db4a669c968fa80eb9b34591cb557/comp_id.txt),
and `lib/loader/COFF/RichHeader.cpp` decodes the Rich header as
[`richprint.cpp`](https://github.com/dishather/richprint/blob/49f2dc93504db4a669c968fa80eb9b34591cb557/richprint.cpp)
does, at revision `49f2dc93504db4a669c968fa80eb9b34591cb557`.
Copyright (c) 2015-2024 dishather. The original BSD 2-Clause license is
preserved in [LICENSES/richprint.txt](LICENSES/richprint.txt).
`scripts/generate_rich_comp_ids.py` regenerates the table from that file; the
decoder adds a check of the header's checksum. Adapted on 2026-09-28.

## FPREM anti-emulation regression

The FPREM regression in
`unittests/semantic/x86/X86_X87TranscendentalRTTests.cpp` adapts the operand
construction and status-check sequence from
[`fprem-anti-emu.asm`](https://github.com/gmh5225/fprem-anti-emulation/blob/f49ff009e062af2a23c5c5eec91649520ca605f0/fprem-anti-emu.asm).
Copyright (c) 2026 Packmad. The original MIT license is preserved in
[LICENSES/FPREM-Anti-Emulation.txt](LICENSES/FPREM-Anti-Emulation.txt).
The sequence was adapted into a callable C inline-assembly round-trip test
on 2026-09-26.

## zlib

Mobile ZIP extraction links zlib for DEFLATE and CRC-32. CMake uses an installed
library when available, or builds the unchanged, hash-pinned zlib 1.3.2 source
archive as a static dependency. Its copyright and license remain in the source
distribution. See the [zlib license](https://zlib.net/zlib_license.html) and
[source releases](https://zlib.net/fossils/).

## Intel x86 approximation reference implementations

The following files incorporate publicly released x86 approximation reference
implementations:

- `lib/ir/low/X86ApproxReference.c`
- `lib/ir/low/X86ApproxReference28.c`

Copyright (c) 2015, Intel Corporation

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.
- Neither the name of Intel Corporation nor the names of its contributors may
  be used to endorse or promote products derived from this software without
  specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The original x64 FP-state transport and test implementations were checked
against the [Intel architecture manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
[Linux KVM API](https://docs.kernel.org/virt/kvm/api.html),
[WHP register ABI](https://learn.microsoft.com/en-us/virtualization/api/hypervisor-platform/funcs/whvvirtualprocessordatatypes),
and the pinned Unicorn register ABI. The implementation retains full 80-bit
lanes, physical abridged tags and FXSAVE64 logical stack rotation. No code from
QEMU or Wine was copied into this implementation.
