# Captured package attestation metadata

`offline-package-attestation-metadata-v1` reads explicitly selected captured
JSON with a captured `offline-package-metadata-descriptor-v1` source/time
descriptor. It does not execute a package, follow a URI, fetch a key, invoke a
crypto process or authenticate a signature. All implementation is native C++.

Three input kinds have distinct admission rules:

| Kind | Admitted declarations | Unverified boundary |
| --- | --- | --- |
| `registry-dist` | npm name/version, optional agreeing `_id`, original SRI classification, `dist.signatures` and optional attestation URI presence | Registry signature encoding is not signature, key or publisher verification; the URI is never followed |
| `intoto-statement` | in-toto Statement v1 subjects and digest maps; selected SLSA provenance v1 metadata | Statement structure, builder presence and declared build times do not establish a SLSA level or authenticate provenance |
| `dsse-intoto` | DSSE envelope fields, exact decoded payload bytes and signature encoding; only `application/vnd.in-toto+json` enters the statement reader | No DSSE signing preamble, signature algorithm, certificate, trust-root or transparency verification |

Absent declarations, conflicting registry `_id`, unsupported statement or
predicate types, malformed digest strings, empty signatures and unsupported
Base64 alphabets have distinct states. Incorrect JSON shapes and exceeded
budgets fail with fixed errors before caching. Unknown fields are ignored;
this is a named metadata subset, not full schema validation. A statement with
missing subjects, predicate type or any subject digest map stays incomplete.
Unknown digest algorithms remain visible and cannot be selected for comparison.

DSSE admits one unmixed standard or URL Base64 alphabet, optional padding and
zero residual bits, with no whitespace coercion. Registry signatures admit
only the standard alphabet. The decoded payload is retained exactly, including
JSON whitespace; no reserialization replaces those bytes. Empty, malformed or
unsupported payloads do not produce invented statement records. Signature
encoding admission says nothing about DER or algorithm validity. Repeated key
IDs retain separate original signature ordinals.

The SLSA subset retains private builder/build-type declarations, parameter
counts and name-overlap state, resolved-dependency count and declared
started/finished times. Dependency digests are not compared. Timestamp
admission and source/as-of chronology use the shared descriptor owner and no
host clock. Invalid, incomplete, conflicting or newer-than-as-of times stay
explicit. Parameter values, builder URIs and dependency declarations are not
published verbatim.

## Explicit original-byte comparison

The caller selects one retained attestation, one digest ID and one captured
original artifact. Only exact `sha256`/`sha512` algorithms and the corresponding
64/128 hexadecimal characters are admitted; either hex case is accepted.
`packages/attestations/Digest` validates subject/digest anchors and delegates
the original-byte check to `PackageIntegrity`, using its existing hash and
input-budget rules. It does not implement a second hashing pipeline.

The receipt retains the original artifact/hash, declaration artifact/hash,
attestation/subject/digest IDs, integrity profile/result ID, byte domain,
algorithm and match/mismatch. Only the selected digest is checked, including
when other digests or subjects disagree. A matching name does not establish a
join; a matching digest does not authenticate the statement or publisher and
does not bind a package inventory. Unsupported selections refuse explicitly.

Signature, trust-chain, transparency and SLSA-level verification remain
unperformed/false on every summary and receipt. `analysis_status` is `partial`
and `safety_verdict` is `not_assessed` regardless of byte equality.

## Owners, identity and limits

`packages/attestations/{Reader,Registry,Statement,Provenance,Encoding,Digest}`
own admission and comparison. `Publication` owns the metadata representation.
`packages/evidence/Publication` shares descriptor/time publication with OSV.
`SessionPackageAttestations` owns revisions, atomic imports and selected
digest receipts; all CLI/worker/MCP consumers call the same four C APIs.

Import identity binds the profile, explicit kind, original artifact/hash and
descriptor ID. Signatures and subjects bind original array ordinals. Digest
maps are ordered by algorithm under the work budget; a global canonical digest
index binds each record ID. IDs do not fingerprint private algorithm names,
key IDs, signatures or other short values. Names, versions, URIs, digest text,
signature bytes and payload contents stay private behind original anchors.
Only the fixed SHA algorithm vocabulary and admitted timestamps are public.

| Boundary | Ceiling |
| --- | ---: |
| Input JSON / depth / nodes | 1 MiB / 32 / 32,768 |
| Decoded DSSE payload | 512 KiB |
| Signatures / decoded bytes each | 64 / 16 KiB |
| Subjects / total digest declarations | 64 / 256 |
| Retained attestation imports / digest receipts | 4 / 4 |
| Shared advisory + attestation descriptors | 20 |
| Shared retained parsed metadata, descriptor copies and anchors | 8 MiB |
| Charged work per reader | 8,000,000 units |
| Page limit | 1..512 |

Uncomposed imports consume the same ceilings. Exact repeat selections reuse
the prior result even at capacity. Descriptor/import insertion and serialization
complete atomically; failure leaves byte counters and existing results intact.
A successful project import revokes all metadata and comparison selections.

## Interfaces

The C APIs are `neverd_web_package_attestation_import_json`,
`neverd_web_package_attestation_records_json`,
`neverd_web_package_attestation_digest_match_json` and
`neverd_web_package_attestation_digest_result_json`. Records select `signatures`,
`subjects` or `digests`. Matching returns `package_attestation_digest_match_id`,
which the result API can read again. Worker/MCP names use the same suffixes.
Their closed schemas expose no URL, key path, raw payload or verification flag.

```console
neverd web package-attestation <capture> <metadata-index> <descriptor-index> registry-dist
neverd web package-attestation <capture> <metadata-index> <descriptor-index> intoto-statement <original-index> <digest-index>
```

CLI indices come from the current artifact inventory and canonical digest
page. It buffers successful analysis before printing four JSONL records
(summary and three complete pages), with a fifth receipt when comparison is
selected. No raw fields are printed. Metadata analysis works with JavaScript
disabled; complete Web omission preserves API stubs and capability negotiation.

Primary format references:
[npm registry signatures](https://docs.npmjs.com/about-registry-signatures/),
[in-toto Statement v1](https://github.com/in-toto/attestation/blob/main/spec/v1/statement.md),
[DSSE envelope](https://github.com/secure-systems-lab/dsse/blob/master/envelope.md),
[SLSA provenance v1](https://slsa.dev/spec/v1.0/provenance).

The [captured npm qualification](web-package-attestation-qualification.md)
records the original Claude Code metadata hashes and actual CLI observations.
