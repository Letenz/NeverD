# Captured npm signature metadata qualification

This case exercises the native NeverD CLI against preserved official Claude
Code registry records. It qualifies the metadata reader and publication contract
in [the attestation profile](web-package-attestation-profile.md); it does not
authenticate signatures, publishers, provenance, transparency or SLSA levels.

## Inputs and reproduction

The capture is preserved at
`<local-recovery>/offline-package-evidence/2026-10-11/attestation-capture`.
The parent directory retains the reports. Sorted artifact indices are:

| Index | Capture member | Original SHA-256 |
| ---: | --- | --- |
| 1 | `a.registry.json`, Claude Code 2.1.295 | `5b08bcfe0bd28ab796d40388a360e8f5b5d5db672376106887972e0715109578` |
| 2 | `b.descriptor.json` | `28047bf9ec968d1e2d0d1ec500b36ea7d5bc2c687f99c0fe3224bccd2dcdbeac` |
| 3 | `c.registry296.json`, Claude Code 2.1.296 | `0fb79dc0c21d02fc14feeb7f9a86fa15ca3937598f9f3a6a03816bb15e7fa6e4` |
| 4 | `d.descriptor296.json` | `235325fac22b5b295b8ca999509d779be743693d1dfa36118af765aef324a628` |

The registry records are respectively 3,503 and 3,531 original bytes, copied
from the existing version-specific evidence directories. Their original capture
timestamps are not established by this increment. The descriptors intentionally
supply only `schema_version: 1` and a source label; no retrieval or as-of time
is invented. Both therefore report `incomplete_source_or_times` and
`freshness_assessed: false`.

The macOS arm64 Release CLI ran these commands with an unusable executable
search path. Neither command executed a package nor fetched a key or URL:

```sh
env PATH=/neverd-no-external-tools build-web-dev/bin/neverd web package-attestation \
  <local-corpus>/attestation-capture \
  1 2 registry-dist
env PATH=/neverd-no-external-tools build-web-dev/bin/neverd web package-attestation \
  <local-corpus>/attestation-capture \
  3 4 registry-dist
```

## Observed result

Each command succeeds with four JSONL records: summary, signatures, subjects
and digests. Both registry captures declare name/version and SRI metadata,
have two signatures and supply no attestation reference or in-toto subjects.
The signature encodings are admitted standard Base64; decoded lengths are
72/72 bytes for 2.1.295 and 71/72 bytes for 2.1.296. Array positions remain
distinct evidence records. Encoding admission makes no claim about DER shape,
algorithm selection or cryptographic validity.

Reports expose original artifact/hash anchors, fixed states and counts. They
do not expose signature bytes, key IDs, package names, source labels or URLs.
Both retain `signature_verification: not_performed`,
`publisher_authenticated: false`, `trust_chain_verified: false`,
`transparency_verified: false`, `slsa_level_verified: false` and
`safety_verdict: not_assessed`. The SRI declaration remains
`declared_unverified`; these commands request no original-payload comparison.

| Preserved report in the parent directory | SHA-256 |
| --- | --- |
| `neverd-attestation-2.1.295.jsonl` | `b4b13a8581eea8188bf7a9fc7f068ed5da38c0eb19230e78aaf084e0fc45202a` |
| `neverd-attestation-2.1.296.jsonl` | `940abf5b60fdb849f37b946a7c8efb59d62071a12e0a1242291995dc4793d25e` |

The existing captured advisory command was also replayed after extracting the
shared descriptor publisher. Its report remains byte-identical to the prior
qualification, SHA-256
`18a746c85195ad7f8e6d81d9611ae17f600478ed924898ab875397aa1ec6a988`.
Replaying the 2.1.296 attestation command with JavaScript parsing disabled also
produces the byte-identical report. Metadata decoding does not depend on the
JavaScript parser.

The in-toto, DSSE, SLSA and selected subject-digest paths are covered separately
by bounded C++ fixtures. They are not presented as observed properties of these
two registry captures. This increment advances #715; the five JS epics remain
incomplete.
