# Reversible lexical JavaScript renaming

`javascript-scope-rename-v1` is the explicit `scope_renaming` pass of the
shared source rewrite API. It replaces admitted function-local identifier
occurrences with deterministic ASCII names. The C++ parser and lexical binding
model remain authoritative. It does not recover original names, execute source,
preserve source reflection or establish whole-program equivalence.

## Admission

The source must parse with complete lexemes and an `ok` binding analysis.
Possible direct `eval`, `with`, uncertain Annex B declarations or conflicting
bindings refuse the complete candidate. Only explicit `parameter`, `var`, `let`
and `const` bindings with one simple identifier declaration are eligible.
Every resolved reference must be a read with its own exact identifier token.

Root/global/module/CommonJS wrapper bindings, function and class names,
imports/exports, implicit bindings, `arguments` and `eval` remain unchanged.
Destructuring, defaults, shorthand properties with shared identifier spans,
duplicate declarations and bindings with assignment/update/export references
are refused. A variable initialized with a function, arrow or class expression
is excluded because its identifier can determine an observable inferred name.

All bindings owned by a function with a non-simple parameter list are excluded,
including simple parameters and body variables in that function. For example,
`function f(a,b=0){var a;return a;}` copies the parameter value into a separate
same-named body variable. The lexical graph does not represent that implicit
copy, so matching graphs alone cannot justify renaming these bindings.
Eligible bindings in independently nested simple functions can still be selected.

Names are `neverd_arg_<binding ordinal>` or `neverd_local_<binding ordinal>`.
An underscore is appended until the name is absent from every decoded AST
identifier, including property keys and labels. The names indicate provenance
from this projection, not inferred domain meaning. Each admitted declaration
and every one of its resolved references are replaced together. Unselected
bytes, comments, literals, property keys and labels remain exact.

## Validation and publication

The candidate is reparsed and analyzed by the same lexical owner. Validation
compares the entire AST, allowing only recorded identifier-name substitutions;
scope kinds, parents, ownership and flags; binding kinds, mutability and
temporal-dead-zone flags; declarations; reference access and resolution; and
per-node scope ownership. Every changed token must have the candidate range
recorded in its receipt. Exact inverse application must reproduce the original
bytes and SHA-256 before publication.

Analysis metadata uses this distinct profile. `binding_verification` describes
the graph comparison. Value/effect analysis IDs and profiles are null and
`value_verification` is `not_applicable`; this pass does not claim primitive
SameValue evidence. `original_name_recovery`, `whole_program_equivalence` and
`source_reflection_preserved` are false. Records add opaque `binding_id` and
`identifier_role` (`declaration` or `reference`). Original/replacement names and
per-token hashes remain private. Accepted counts are identifier occurrences;
refused counts are explicit binding declarations considered but not selected.

The existing [rewrite API and local export](web-source-rewrite-profile.md)
retain ownership of revisions, caching, private source and disclosure. CLI,
worker and MCP expose the same pass; worker and MCP cannot export raw files.
Local export requires the exact pass/profile/revision-bound preview receipt.
It writes separate `original.js`, `candidate.js` and `receipt.json` files.
The original capture remains immutable. No derived source map is generated.

The limits are 1 MiB per original/candidate/inverse, 4,096 total records and
8,000,000 planning/application/validation steps. Identifier traversal, name
comparison/collision work and byte copies consume that budget. Parser and
binding limits also apply. Inverse validation has its own 8,000,000-step limit.
All three rewrite passes share two cached results per session. A failure
publishes no candidate or partial binding edits.

The two existing primitive passes retain `javascript-primitive-rewrite-v1`
and their exact IDs and serialized receipts. Binding/role fields are absent
for those passes. Fixed pre-rename export receipts provide a compatibility
oracle, including record IDs, export tokens, complete bytes and SHA-256.

References: [FunctionDeclarationInstantiation](https://tc39.es/ecma262/multipage/ordinary-and-exotic-objects-behaviours.html#sec-functiondeclarationinstantiation),
[variable declarations](https://tc39.es/ecma262/multipage/ecmascript-language-statements-and-declarations.html#sec-variable-statement),
and [Function.prototype.toString](https://tc39.es/ecma262/multipage/fundamental-objects.html#sec-function.prototype.tostring).
