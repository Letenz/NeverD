# Reversible Boolean branch expressions

`javascript-boolean-branches-v1` is the explicit `boolean_branches` pass of
the shared C++ source rewrite API. It selects operands of `?:`, `&&` and `||`
when the existing finite-value owner proves the evaluated condition is a
Boolean and the effect owner proves that condition has no effects or contained
declarations. Unknown, coercive and throwing conditions remain unchanged.
The selected operand can contain calls, reads, writes or exceptions; those
operations remain in the candidate and are never executed by NeverD.

## Admission and emission

The entire source must parse with complete lexemes and complete lexical
bindings. Possible direct eval, with environments, Annex B uncertainty and
conflicting declarations prevent publication. A candidate expression cannot
contain any declaration occurrence or scope-owning node from the binding
model. Bounded summaries follow validated AST ownership edges rather than
assuming contiguous subtree indices. Embedded functions/classes are excluded,
including those in an unreachable branch.

Every candidate containing an `AwaitExpression` is refused, even if that await
would never execute. Module asynchronous evaluation depends on syntax-derived
`[[HasTLA]]`; matching surviving references alone does not preserve it.
This exclusion also applies inside async functions. Other independent
expressions in the same source remain eligible. See
[ECMAScript ParseModule](https://tc39.es/ecma262/multipage/ecmascript-language-scripts-and-modules.html#sec-parsemodule).

Each accepted expression becomes `(0,(<exact selected operand bytes>))`.
The outer sequence forces GetValue, retaining the value-result contract of
conditional and logical expressions. A selected property used as a callee must
not acquire a receiver; selected identifiers must not become special direct
arguments of typeof, delete or eval. Inner parentheses retain a chosen sequence
expression as one operand. See the specification's
[conditional operator](https://tc39.es/ecma262/multipage/ecmascript-language-expressions.html#sec-conditional-operator),
[binary logical operators](https://tc39.es/ecma262/multipage/ecmascript-language-expressions.html#sec-binary-logical-operators)
and [comma operator](https://tc39.es/ecma262/multipage/ecmascript-language-expressions.html#sec-comma-operator).

Comments/hashbangs inside a candidate range prevent its replacement. All
unselected bytes are preserved. The outermost admitted expression wins;
nested candidates are recorded as covered. Refused outer expressions can have
independently admitted descendants. This profile never transforms statements,
removes declarations, propagates variable values, evaluates target code or
claims general control-flow reconstruction.

## Validation and evidence

The candidate is reparsed. Each replacement must have exactly two outer
sequence operands: canonical zero and the original selected subtree. The
validator compares all retained node kinds, attributes, strict flags and child
roles, building an original-to-candidate node mapping. No undeclared candidate
nodes may remain. Scope and binding order and all declarations must survive;
owner nodes are compared through that mapping. Candidate references must
equal the ordered projection of surviving original references, including
decoded names, binding, scope, access and resolution. Exact inverse application
must reproduce the original bytes and SHA-256 before publication.

Records add opaque `condition_node_id`, nullable `retained_node_id` and
`selected_operand`. These fields are absent from the older profiles. Condition
and retained identities and indices participate in this pass's record identity.
Ordinary metadata contains no operand bytes or per-operand hashes.
`value_verification` is `constant_boolean_condition_only`, not a SameValue claim
about the retained expression. `binding_verification` is
`same_scopes_declarations_and_retained_references` and retained effects are
explicitly `preserved_not_assumed_absent`.

Limits remain 1 MiB per original/candidate/inverse, 4,096 records and 8,000,000
rewrite steps, plus the parser/binding/value/effect owners' limits. Inverse
validation has a separate 8,000,000-step ceiling. Exhaustion or any failed
structural check clears the entire candidate and all tentative records. All
four passes share two cached results. Existing primitive and rename profiles
retain their exact IDs and serialized export receipts through fixed goldens.

The [existing API/export transaction](web-source-rewrite-profile.md) owns
revisions and private output. CLI, worker and MCP expose the same explicit
pass. Only local SDK/CLI export can write the original, candidate and receipt,
after supplying the exact preview token. Captures remain immutable. No derived
map, original-name recovery, source-reflection or whole-program equivalence
claim is made.
