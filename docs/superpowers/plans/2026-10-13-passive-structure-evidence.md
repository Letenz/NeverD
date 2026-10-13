# Passive request and observed JSON structure evidence

Implement the two #717 gaps established by the 2026-10-13 issue-body audit.
Use C++ and the existing NeverD frontend, committed HAR selection and public
report route. No target execution, network requests or external analysis tools.

## Evidence and ownership

- Extend PassiveReport.h with an optional structure descriptor. Publish
  capture-local opaque field identities, parent links, array positions, types
  and observed presence counts. Never publish keys, values or private URL
  components, or derive public field IDs by hashing private names.
- Keep HAR admission in report/Shapes.cpp; a separate observed-structure
  owner builds bounded trees and aggregates same-method/origin/path examples
  using the existing endpoint model. Request and response groups remain
  separate. Malformed, missing and non-JSON bodies do not imply absence.
- Add a source-structure owner consuming admitted request roles and transform
  relations. Preserve template segments/expressions, lexical URL component
  roles, header properties and direct body object/array structure. Unknown
  expressions, spreads and accessors stay explicit syntax limitations.
- Compose preserves distinct structural identities and Publication alone
  formats them. Session caching, C API, CLI, worker and MCP use the same model.
  Bump the named report profile to v2; API selection schema stays version 1.

## Verification

1. C++ unit cases distinguish the two previously indistinguishable nested JSON
   witnesses. Check repeated fields, missing versus null, mixed arrays,
   grouping isolation, parent links, unique IDs, budgets and privacy canaries.
2. Source cases inspect template holes, path/query component syntax, individual
   headers and nested JSON-stringify argument syntax. Check original byte
   anchors and conservative unknown boundaries.
3. Build affected Web library, CLI, SDK, worker and MCP tests before running
   them. Check parser-disabled observed reports and real Claude Code inputs.
4. Replay the audit fixtures through the actual CLI with an empty tool PATH.
   Update the profile, architecture and acceptance documents with measured
   results, then publish verified commits to dev.

## Limits and review

An observed absent field is not a server optionality rule. Grouping does not
infer routes, response schemas, runtime API identity or source execution.
Template components describe lexical spelling; interpolations may contain
delimiters at runtime. Structure has the existing finite report/JSON budgets;
exhaustion fails atomically. No new dependencies or public C ABI entry points
are needed. Existing report consumers must inspect the v2 profile and tolerate
additional record kinds. This ordinary implementation plan was self-reviewed
against the audit and the repository ownership rules.

## Implementation review

The implementation separates body admission, observed structure, request value
syntax, URL spelling and publication. Grouping consumes the existing captured
endpoint model and does not modify the source/HAR join. Identity construction
includes the report profile; aggregate fields use private dictionaries and
ordinal-based public IDs. Review added explicit private-byte accounting, charged
source key work, and regressions for object-order independence and shared record
limits. The first 17 targeted checks passed before transport qualification.
