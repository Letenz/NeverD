# Offline advisory qualification

On 2026-10-11 the native NeverD CLI compared captured official npm metadata for
`lodash@4.17.18` and `lodash@4.17.19` against one captured public OSV advisory.
Only metadata was downloaded for this qualification; no package code, install
script or JavaScript runtime ran. The CLI ran with
`PATH=/neverd-no-external-tools` on macOS arm64 Release with prebuilt LLVM
23.0.0 r4. Other hosts require separate qualification.

| Original evidence | Bytes | SHA-256 |
|---|---:|---|
| [OSV GHSA-p6mc-m468-83gw](https://api.osv.dev/v1/vulns/GHSA-p6mc-m468-83gw) | 4,949 | `a0ed80d486cf363f586b0f031e983c68a136f1f58d1a6e2cc3253dc11319dc75` |
| [npm lodash 4.17.18 metadata](https://registry.npmjs.org/lodash/4.17.18) | 2,454 | `626d0b763199d0981496ab75fe2f28cf084a3ec9b23a3f6ebecadee8af2a68ac` |
| [npm lodash 4.17.19 metadata](https://registry.npmjs.org/lodash/4.17.19) | 2,454 | `2d8acb8b50700415a4c328b261883769934c1f01f124c6ac0bfcfc827c419153` |

The caller-supplied descriptor records the OSV API URI and retrieval/as-of
observation `2026-10-11T14:39:34Z`; its SHA-256 is
`afcaca0598a91aaf0c2e7bf83eb544dadb71d507bf8db45dc325137b598428be`.
The npm metadata was captured by `2026-10-11T15:11:58Z`. These are local capture
observations, not authenticated publisher timestamps or a freshness guarantee.

The capture contains `a.advisory.json`, `b.descriptor.json`, `c.before.json` and
`d.after.json`; its root is artifact index 0. NeverD's reader retains eight
affected entries and separates the non-npm ecosystem entry. The two commands
select the same original advisory and descriptor:

```sh
neverd web package-advisories <local-corpus>/advisory-capture 3 package-json 1:2
neverd web package-advisories <local-corpus>/advisory-capture 4 package-json 1:2
```

The imported advisory ID is
`1f5104624082a1d4c3defb9d1f5d650a8abc4fb86e452fe7d9444ac3c82de5b1`.
For 4.17.18, the matcher reports one `declared_version_match`, 5,945 charged
steps, and result ID
`6c2a286874d0cfb3cc841221ffa9544115f91999a6b387d19ea58de489b1d794`.
For 4.17.19, its one row is `version_not_in_admitted_selectors`, with 5,817
charged steps and result ID
`082f3f05168203b1d944418c50ee0d99caf159e6f3b3f4b137f7b13fb7683f02`.

Each JSONL report has seven lines, including inventory and original advisory
anchors, and empty stderr. Report SHA-256 values are
`18a746c85195ad7f8e6d81d9611ae17f600478ed924898ab875397aa1ec6a988` and
`c889c25d69112eec2914ca6da0f93eb480a4e6ca500bfaf21358d08117cd889d`
respectively. Feed completeness, freshness, publisher authentication,
installed-content verification, reachability and exploitability remain
unverified. The second result does not assert that 4.17.19 is free of other
advisories or safe in any application.

Originals and reports are preserved under
`<local-recovery>/offline-package-evidence/2026-10-11/`. Replaying the preserved
capture with JavaScript parsing disabled produced the byte-identical 4.17.18
JSONL report; capture location and parser availability do not change this
metadata result.

`WebPackageAdvisories.OptionalPinnedPublicOsvRecordRetainsItsBoundary` takes
`NEVERD_OSV_NPM_ADVISORY` and verifies the exact original size/hash, eight
affected entries and the same version boundary. Tests never fetch inputs.
Independent C++ cases additionally cover prereleases, huge decimal components,
union selectors, shuffled events, unknown identities, contradictory times,
private publication, import/work ceilings and atomic cache refusal.
