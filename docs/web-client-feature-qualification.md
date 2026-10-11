# Captured Claude client source candidates

The optional C++ corpus case inspects all available JS units from the captured
Claude Code **2.1.296 macOS ARM64 npm package**. It uses the same immutable
archive/member/Bun owners as the
[cross-platform extraction qualification](web-claude-code-cross-platform-qualification.md),
then the embedded JavaScript parser, lexical bindings, generic navigation and
[client syntax profile](web-client-feature-profile.md). It neither executes
input code nor invokes another analyzer. Private source remains outside Git.

The original tarball SHA-256 is
`50fbf594d6526c9aae7d66b8b45a08d89ae6b863e2bc9777bfb277d6521a7458`;
its selected executable SHA-256 is
`c9b5341637becbd423ddffc5b254afb645682a3868cb708bbc6cc0e7bb419937`.
All 2,364 decoded units and 45,035,899 UTF-8 bytes are visited sequentially.
The interactive frontend admits 2,354 units. Ten are explicitly outside its
budgets: three source-byte refusals, three parser-work refusals and four syntax
inventory refusals. No parser deadline expires in the recorded receipt.
The larger recovery profile has already preserved/reparsed all 2,364 units;
these navigation limits do not mean source bytes were lost during extraction.

Every admitted unit finishes the bounded feature pattern inventory. There are
416 **syntax candidates**, not authenticated feature implementations:

| Domain | Candidate records | Unresolved shapes |
| --- | ---: | ---: |
| Tool | 3 | 2 |
| Configuration | 88 | 84 |
| Permission | 165 | 21 |
| MCP | 110 | 35 |
| Hook | 50 | 0 |

The candidates retain 43 callback **source** relationships and one containing
permission-named conditional test. Neither count establishes callback execution,
effective policy, configuration precedence, reachability or server behavior.
Hook literals can occur in logging or unrelated calls. Missing pattern matches
and the ten refused units remain coverage gaps. The receipt is
`/tmp/neverd-client-features-corpus-receipt.xml`; tests pin original bytes and
record the coverage from the current bounded run instead of turning a deadline
dependent result into a universal application claim.

Two additional actual CLI selections use the retained `original.bin`, with
`PATH=/nonexistent` and `TMPDIR=/tmp`:

```console
neverd web bun-navigate ./recovered/original.bin 1676 module
neverd web bun-navigate ./recovered/original.bin 392 module
```

These indices belong to this exact graph. Module 1676 produces one permission
setting syntax candidate. Module 392 produces no pattern match in all five
domains, preserving partial coverage rather than an absence/safety verdict.
The source IDs are respectively
`e2c3a1c6257549d03c6624b714ce7de91e64f7849e36af1065e51a946c2db6b8`
and `bd7b6d5dd9acdd6ae74ddef306b800e1cfa8b1e483cdbb5beb159190e81a2267`.
Their feature analysis IDs are respectively
`655bece27bdd86a66786b04af17666b9f2d273e1d4b3c117d34a620e5af8bc9f`
and `e73be8be5ae828384e8f57befe8562e41b72e6d4c03248075ec7d6bb411b45f4`.
The archive-member corpus and standalone CLI have different typed origins;
equal source bytes do not fabricate equal origin identities.

Self-authored C++ fixtures cover all five domains, static/escaped keys,
computed-name refusal, spread, getters/methods, duplicate fields, callback
reassignment/shadowing, dynamic scope, nested-function condition boundaries,
unrelated event literals, record exhaustion and foreign/unavailable models.
The [implementation ledger](web-analysis-implementation.md) records regression,
transport, omission and host limits. This increment does not close #714–#718.
