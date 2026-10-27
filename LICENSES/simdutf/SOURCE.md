# simdutf

Source: [NeverSight/simdutf](https://github.com/NeverSight/simdutf).
Every mainline build resolves the remote default branch (currently `master`),
then uses that exact commit throughout compilation. The generated
`neverd-dependency-inputs.json` records the configure input commit and export SHA-256.
It is an input list; build/test receipts establish product qualification.

License: Apache-2.0 OR MIT. Original license and copyright notices remain in the fork.
The build copies the license files from the actual source snapshot, together
with a generated immutable source link, into the delivered notices.
NeverD applies no local source patches.
