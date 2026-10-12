# Reversible primitive JavaScript rewrites

`javascript-primitive-rewrite-v1` applies one explicitly selected pass to an
immutable captured source. The C++ source frontend, binding analysis, primitive
evaluator and effect analysis own the admission decisions. No input is executed
and no external JavaScript runtime, script or analysis tool is used.

This profile verifies local primitive substitutions. It does not establish
whole-program equivalence, preserve `Function.prototype.toString()` or other
source observation, recover original variable names, or certify transformed
files for execution. Captured originals remain unchanged. Structural source
views and byte-preserving readable recovery are separate operations.

## Passes and preconditions

| Selected pass | Eligible result | Candidate syntax |
| --- | --- | --- |
| `string_recovery` | Exact primitive string | Unary, binary or untagged template expression |
| `constant_folding` | Other supported primitive | Unary, binary or untagged template expression |

The original must parse under the explicitly selected `script`, `module` or
`commonjs` goal and retain a complete, bounded lexeme inventory. A parsed AST
with an exhausted lexer is insufficient. Possible direct `eval` or a `with`
object environment refuses the whole pass. Unknown values, object coercion,
calls, getters, possible exceptions, immediate/deferred effects and contained
declarations do not authorize replacement. All descendants of a tagged template
are excluded in this version. Comments inside a candidate range refuse that
replacement; comments and all other unselected bytes remain exact.

Outermost eligible expressions are selected first. Their descendants receive
`covered` records. A refused expression can still contain independently eligible
children. Another pass's value kind is `not_selected`. Canonical closed
expressions are left unchanged, avoiding repeated wrapping of negative zero,
infinity and `void 0`.

Replacements are parenthesized closed expressions. UTF-16 code units, including
lone surrogates and line separators, are escaped exactly. Number output
distinguishes negative zero; NaN and infinities use arithmetic expressions
without global lookups. BigInts retain signed canonical decimal values.

The complete candidate is reparsed. Outside selected subtrees, retained node
kinds, strict flags, attributes, child fields and ordinals must agree with the
original. Each replacement must evaluate to the same primitive using the shared
evaluator. SameValue distinguishes signed zero and treats NaNs as equal without
requiring the same NaN payload. This also catches automatic-semicolon changes:
`foo()\n1+2;` cannot become `foo()\n(3);` merely because both texts parse.
An outside-tree mismatch refuses the whole result; the writer does not insert
unrecorded semicolons. Exact inverse application must reproduce the original
whole-source SHA-256 before a candidate is retained.

The limits are 1 MiB per original/candidate/inverse, 4,096 candidate records,
8,000,000 rewrite planning/application/validation steps and two cached results
per session. Inverse verification has a separate 8,000,000-step allowance.
Parser, binding, value and effect limits also apply. Reimport clears the cache.
Budget failure publishes no candidate bytes or partial patch list.

## Metadata and local disclosure

`source_rewrite_analyze` takes a source ID and explicit pass.
`source_rewrite_records` pages at most 512 accepted/refused/covered/unselected
records. Responses include source and analysis identities, fixed reasons,
source/candidate ranges, whole-source hashes and verification qualifications.
Original/replacement literal bytes and per-patch hashes are private. A receipt
is local evaluation evidence, not a signature authenticating a producer.

`source_rewrite_export` is a separate local C API operation. It requires the
exact `export_receipt` from analysis, binding the source ID, parse goal, pass,
analysis/profile identities, whole input/candidate hashes and session revision.
A receipt for another pass or revision fails. This operation is omitted from
MCP and the shared worker transport. MCP and worker use the same metadata API.

The POSIX local writer creates a new private directory with three fixed files:

- `original.js`: exact original bytes reconstructed and hash-verified by undo.
- `candidate.js`: the verified derived candidate.
- `receipt.json`: metadata, qualifications and byte ranges for the inverse.

The inverse recipe copies each accepted original range back over its paired
candidate range, in original order, preserving all gaps. The core
`undoSourceRewrite` checks the candidate hash, selected pass identity, ordered
nonoverlapping ranges, private before/after bytes and final original hash.
There is no implicit mutable active source to reset.

Files are exclusive, no-follow, bounded writes with read-back hash checks.
Failure rollback only removes fixed files still matching the entries created
by that transaction, using retained directory handles. A replaced path or
foreign entry is preserved. Created file handles remain open through the
transaction so an unlinked inode cannot be reused as a foreign cleanup target.
An observed rename of the output directory can
leave an empty moved directory. This is not isolation from a concurrent
same-user process changing files after verification. Windows local export is
unavailable in this profile; source analysis itself remains native C++.

The CLI first prints analysis metadata and its receipt:

```text
neverd web rewrite <root> <source-type> <pass> [artifact-index]
neverd web rewrite-export <root> <source-type> <pass> <artifact-index> <preview-receipt> <output-dir>
neverd web bun-rewrite <binary> <module-index> <source-type> <pass>
neverd web bun-rewrite-export <binary> <module-index> <source-type> <pass> <preview-receipt> <output-dir>
```

The Bun route uses NeverD's extractor and immutable decoded module bytes. It
does not read a substitute hand-reconstructed module. Existing source-map
directives remain byte-preserved evidence; no map for the derived candidate is
generated or implied.

## Required later work

Issue #714 remains open in scope. The separate
[lexical rename profile](web-scope-rename-profile.md) adds `scope_renaming` while
preserving these two primitive passes and their receipts exactly. The separate
[Boolean branch profile](web-boolean-branches-profile.md) selects proven
Boolean expression operands with retained binding and GetValue validation.
Statement transforms and broader control-flow deobfuscation remain outside
these profiles. The other four JS epics retain their documented gaps.

Language references: [SameValue](https://tc39.es/ecma262/multipage/abstract-operations.html#sec-samevalue),
[directive prologues](https://tc39.es/ecma262/multipage/ecmascript-language-source-code.html#sec-directive-prologues-and-the-use-strict-directive),
and [Function.prototype.toString](https://tc39.es/ecma262/multipage/fundamental-objects.html#sec-function.prototype.tostring).
