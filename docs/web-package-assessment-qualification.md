# Actual offline package assessment qualification

On 2026-10-12, native NeverD on macOS arm64 Release composed the preserved
official Claude Code 2.1.296 npm wrapper, registry declaration and descriptor.
The [assessment profile](web-package-assessment-profile.md) uses the same owners
as the existing archive, original-integrity, Node behavior and installation
qualifications. No target package, script, JavaScript runtime or installer was
executed. No feed, signature key or metadata was fetched by the analyzer.

The capture is retained in the qualification evidence collection at
`offline-package-evidence/2026-10-12/assessment-capture`.
Input identities are independent of the host storage path.

| Input | SHA-256 |
| --- | --- |
| `a.package.tgz`, official wrapper 2.1.296 | `f6c375d51d4c22a7a850e185a0d7ddda85173f89dbbeb3262aa7971e3bd670b5` |
| `b.registry.json`, preserved registry declaration | `0fb79dc0c21d02fc14feeb7f9a86fa15ca3937598f9f3a6a03816bb15e7fa6e4` |
| `c.descriptor.json`, preserved metadata descriptor | `235325fac22b5b295b8ca999509d779be743693d1dfa36118af765aef324a628` |

This exact capture uses root indices 1/2/3 for the archive/registry/descriptor,
manifest member 3 and source members 0/1. Other captures must use their own
current inventory. The commands run with `PATH=/nonexistent` and `TMPDIR=/tmp`:

```console
neverd web archive-assessment ./assessment-capture tgz 1 3 package-json -- --registry 2 3 1 --source 0:commonjs --source 1:commonjs
neverd web archive-assessment ./assessment-capture tgz 1 3 package-json -- --registry 2 3 1 --source 0:commonjs --source 1:commonjs --markdown
```

Both formats produce assessment ID
`1d289c84ade51423dd9eb1def3b614ec9135d1f71ff50a151fcb4a3f8e6895a3`
and untouched review root/head
`1db319403407e527f3cd755a34fee9e5f9ee32daf9ff70450f1b0e6f8395b9c7`.
No dispositions were applied to this sample. There are 45 findings, 48,320
charged work units and 98,464 charged retained metadata bytes:

| Evidence class | Findings |
| --- | ---: |
| Explicit coverage | 9 |
| Root inventory declaration | 1 |
| Original archive integrity | 1 |
| Registry metadata and two unverified signature declarations | 3 |
| Two source selections and eight recognized syntax records | 10 |
| Two lifecycle declarations, 13 unresolved module requests and six static associations | 21 |

The original SHA-512 declaration matches selected archive bytes, and the
retained archive contains the selected root manifest. The finding therefore
uses `original_contains_declared_root_manifest`. It does not verify an installed
dependency tree, a signature, publisher identity, runtime activation or
reachability. Unsupplied capture/build timestamps remain absent. Omitted
advisory and comparison evidence remain explicit coverage gaps. Every finding
starts `unreviewed` with `disposition_source: not_reviewed`.

Reports are beside the capture. Their exact SHA-256 values are:

| Report | SHA-256 |
| --- | --- |
| `claude-code-2.1.296-assessment.jsonl` | `0114ae9b57c69ce3a0dfe66bb7b9b0b63d5aae4be9c8ba9d64e43b9d40e4e30c` |
| `claude-code-2.1.296-assessment.md` (31,988 bytes) | `9261c333120e41c7d9c35bcf4dc002343055a3ce3ac932e336d7d06bd20a2b73` |
| `claude-code-2.1.296-assessment-metadata.jsonl`, no source selections | `7a1476bbdc726a2a410277adc778bf6da83280d641c301d355744dade1906685` |

The metadata-only command is byte-identical with the optional JS frontend
enabled or omitted. Source-enabled reports require the frontend and do not
claim the two selected wrapper units cover all embedded sources.

The separately preserved public OSV capture from the
[advisory qualification](web-package-advisory-qualification.md) also composes
against explicit lodash 4.17.18 and 4.17.19 declarations. The former retains
`declared_version_match`; the latter retains
`version_not_in_admitted_selectors`. Both retain `safety_verdict: not_assessed`.
The corresponding reports are:

| Report | SHA-256 |
| --- | --- |
| `lodash-4.17.18-assessment.jsonl` | `1182ab22a1ad973821752e4f63771447f0949b327c6e8314887f08b849bee183` |
| `lodash-4.17.19-assessment.jsonl` | `2d4af3850059fd3251ec74f95945ea184c138114f9ace1038f3a6e1c905ce8c6` |

The [implementation ledger](web-analysis-implementation.md) records tests,
omission/restoration checks and limitations. This qualification establishes
these selected inputs and host only; it does not close issue #715 or the other
four epics. Previously qualified preservation/reparsing of all 2,345 shipped
Claude JS units remains separate evidence. There are no Claude source maps;
original TypeScript, erased types/names and opaque JSC bytecode are not recovered.
