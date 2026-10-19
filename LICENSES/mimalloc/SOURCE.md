# mimalloc

Source: [NeverSight/mimalloc](https://github.com/NeverSight/mimalloc).
Every mainline build resolves the remote default branch (currently `main3`),
then uses that exact commit throughout compilation. The generated
`neverd-dependency-inputs.json` records the configure input commit and export SHA-256.
It is an input list; build/test receipts establish product qualification.

License: MIT. Original license and copyright notices remain in the fork.
The build copies the license files from the actual source snapshot, together
with a generated immutable source link, into the delivered notices.
NeverD applies no local source patches.

NeverSight modification (2026-10-19): the macOS minimum allocation and size-query
contract, including debug padding and fresh threads, with a system-libffi
regression. Merged into the default branch at
[d1b3a813](https://github.com/NeverSight/mimalloc/commit/d1b3a813bad6f107c12c5597eaf14ca24854cf5d).
Original Microsoft Research and Daan Leijen copyright notices are preserved.
