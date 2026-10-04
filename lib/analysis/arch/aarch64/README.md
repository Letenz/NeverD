# AArch64 recovery adapter boundary

This directory reserves the native ARM64 recovery adapter. It contains no
implementation and does not enable ARM64 native interpreter recovery.
The shared scalar LLVM, symbolic and MBA engines already operate on explicit
bit widths. Reuse them; do not copy them here.

An adapter must supply authenticated instruction boundaries and undefined
outputs, X/W register views, SP versus XZR, NZCV, link-register call/return
semantics, and an explicit state/ABI contract. Extend provider dispatch only
after these contracts have focused semantic tests. Do not reuse x64 RFLAGS,
CET, PUSH/RET or string-transfer rules for ARM64.

Keep OS services in `lib/emulation/os`. Environment assumptions do not certify
an ABI. See the [architecture guide](../../../../docs/architecture.md), whose
language selector links the synchronized translations.
