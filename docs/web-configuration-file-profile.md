# Configuration file and minified source navigation

`javascript-configuration-file-navigation-v1` adds an independent syntax
inventory to source navigation. It was measured against the retained Claude
Code 2.1.296 macOS ARM64 units, whose local function names are minified. It
recognizes direct file/layer literal sites and uses existing lexical declaration
identities to link calls to functions containing those sites. Function names
need not resemble `loadSettings`. This is inspection evidence; it does not
authenticate a configuration loader, open a configuration file or execute code.

## Fixed grammar

| Literal spelling | Published role |
| --- | --- |
| `settings.json` | `settings_file` |
| `settings.local.json` | `local_settings_file` |
| `managed-settings.json` | `managed_settings_file` |
| `managed-settings.d` | `managed_settings_directory` |
| `userSettings` | `user_layer` |
| `projectSettings` | `project_layer` |
| `localSettings` | `local_layer` |
| `flagSettings` | `flag_layer` |
| `policySettings` | `policy_layer` |

Only complete parser-decoded string literals match. File literals are admitted
as direct ordinary/optional call arguments, ordinary property values or direct
return arguments. Layer literals are admitted as direct call arguments,
switch-case tests or array elements. Templates, concatenations, arbitrary path
suffixes and explanatory text do not manufacture matches. A spelling occurrence
can be logging, validation or unrelated code; its role is a vocabulary candidate.

Sites retain exact original literal spans, containing function/call identities
and enclosing switch-case anchors. A nested function resets switch containment;
sites in its body do not imply execution of the containing case. Common case
anchors permit source inspection of file/layer associations without deriving
runtime paths, fallthrough or branch behavior.

An ordinary/optional call can link to a local function declaration or unique
function initializer through its existing lexical binding, or directly to an
inline function. Duplicate function candidates, conflicting bindings, dynamic
scope, imports, member dispatch and identifier alias chains do not acquire a
local function target. Reassignment does not erase the declaration relationship;
each published link explicitly says `lexical_source_only` and `not_proven` for
the runtime target. No inter-module propagation or call-return evaluation is
added.

Arrays containing at least two directly written layer literals retain every
element's source ordinal and anchor. Spreads, holes, calls and other unknown
elements stay unknown. The sequence is `array_element_source_order`; it does
not establish which layers are enabled, their iteration direction, merge
semantics, file-layer precedence or effective configuration. Conditionals and
separate configuration inputs remain reviewable through generic source
navigation with their original uncertainty.

## Ownership, bounds and publication

`features/configuration/Files` owns this grammar and its bounded analysis.
`SourceModel::indexSourceFunctions` owns function declaration/initializer
cardinality for both this analysis and existing client callback navigation.
Generic navigation, bindings and named client-feature identities remain their
existing independent evidence. `FilesPublication` publishes fixed roles and
identity/range metadata; paths, function names, values and per-value hashes
remain private.

Each selected source admits at most 1,024 total sites/call links/sequences,
4,096 sequence terms and 2,000,000 charged work steps. Explicit large-source
admission uses the shared larger work limit. Model validation, indexing,
literal comparisons, containment, array scans and call joins are charged.
Malformed/foreign models or exhaustion discard this entire candidate result;
generic navigation remains available. Existing source/node/cache and 512-row
page limits apply. These limits do not establish a whole-process RSS ceiling.

`SessionNavigation` computes and caches `configuration_files` with its other
navigation results. The existing records operation pages `config_files`, in
site/call/sequence order. Every row retains an original anchor and
`source_syntax_only` confidence. Reimport revokes the selection. CLI
`neverd web navigate FILE MODE` and its explicit large-source variant include
these pages; SDK, worker and MCP use the same existing operation and owner.
No C ABI entry point, raw-source disclosure, network access or execution option
is added. Parser omission removes this profile and navigation as before.

The C++ corpus qualification measures both interactive and explicit large
admission against all 2,364 original decoded units and records literal sites,
lexical call candidates and source sequences independently of the older
named-loader count. Source bytes remain local; fixture tests use self-authored
grammar and privacy canaries rather than copying client implementation.

The 2026-10-13 Release receipt covers every unit under explicit large admission:
973 literal sites, 592 local lexical call candidates, 69 layer arrays and 209
array terms, with zero unavailable inventories. Interactive admission retains
its ten refusals; its 2,354 admitted units yield 682 sites, 341 call candidates,
50 arrays and 156 terms. The old named-loader count remains zero, as expected
for a different grammar. The separately retained original `m00086.js` yields
30 sites, 19 call candidates and one two-term array through the CLI with an
unusable external-tool PATH and empty stderr. These are source relationships,
not 592 authenticated loaders or effective configuration layers.
