# oneTBB

Source: [NeverSight/oneTBB](https://github.com/NeverSight/oneTBB).
Every mainline build resolves the remote default branch (currently `master`),
then uses that exact commit throughout compilation. The generated
`neverd-dependency-inputs.json` records the configure input commit and export SHA-256.
It is an input list; build/test receipts establish product qualification.

License: Apache-2.0. Original license and copyright notices remain in the fork.
The build copies the license files from the actual source snapshot, together
with a generated immutable source link, into the delivered notices.
NeverD applies no local source patches.

NeverSight modification (2026-10-19): explicit narrow Windows loader APIs and
Unicode regression coverage, merged into the default branch at
[00c4549a](https://github.com/NeverSight/oneTBB/commit/00c4549a8e549a60102e49954a7dea0aa3933cc0).
