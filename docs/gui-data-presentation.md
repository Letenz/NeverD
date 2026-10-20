# Arrays, alignment and enum operands

The disassembly **Edit** menu includes **Make array** (`*`) and **Make
alignment** (`L`). **Operand type → Enum operand** (`M`) selects an enumeration
declared in Local Types. The normal operand format reset (`#`) restores numeric
spelling; `U` undefines a complete array or a manually marked alignment span.
All three edits use project undo/redo, sidecars and database/replica state.

An array records element width, count and an optional local type name. The
complete span must be initialized, bounded to 64 MiB, and disjoint from function
code and other definitions. Untyped scalar elements are 1, 2, 4 or 8 bytes;
typed elements require the exact current type layout. Each element has an
indexed listing row, including when navigating into the middle of a large
array. Record rows show a bounded byte preview with the complete element size.
They do not expand a record into an unbounded number of member rows.

Saved numeric extent and element width remain authoritative for presentation
when a local type later changes. A missing or differently sized type produces
`stale element type` and raw bytes, without resizing the original definition.
Type edits invalidate both array layout and enum spelling caches.

Alignment is an explicit padding span ending at the requested power-of-two
boundary (2 bytes to one MiB). At an already aligned address it marks one full
alignment unit. It changes the listing's interpretation, never the input bytes.

Enum operands currently use the existing native x86/x64 operand formatter.
The SDK validates an actual decoded immediate, its logical width and the enum's
underlying width. Signed values retain their width; aliases choose the first
declared exact member deterministically. Flags mode combines named single bits
and includes every remaining bit numerically, for example
`QA_READ | QA_WRITE | 0x8`. Deleting the enum restores numeric display until
the type returns. Original operand identity survives classic operand folding.

Definitions are presentation state. They do not supply a new machine semantic
contract to decompilation, lifting or native rewriting. Structural state loading
does not eagerly bind type-dependent rows against a preceding type generation;
new edits validate against the current library, and render binding uses the
current library without changing saved extents.

Checks: `NeverDWorkerDataPresentation` uses a real ELF with code and a separate
data segment and covers pagination, records, enum aliases/signed values/unknown
bits, layout changes, code overlap, atomic refusals, undo/redo, reopen, replicas
and ordinary IR isolation. `NeverDWorkerFormat` covers logical-width plumbing
and folded operands. The Qt source/workbench test includes all three dialogs.

## CLI and saved project parity

These definitions use the same SDK validators and sidecars in the CLI:

```console
neverd items app.exe --array 140020000 --element-size 4 --count 16 --json
neverd items app.exe --array 140020000 --element-size 12 --count 4 --type MY_ROW --json
neverd items app.exe --alignment 140020043 --boundary 16 --json
neverd operands app.exe --addr 140001000 --operand 1 --enum MY_FLAGS --flags --json
```

`--type` names a previously declared local type; `neverd types app.exe
--declare types.h --save-declarations` imports and persists it. Without
`--save-declarations`, declarations remain local to that CLI invocation. Its current layout must match `--element-size`.
The SDK refuses invalid sizes, unavailable types, overlaps, uninitialized spans
and non-immediate enum operands before saving. Alignment padding ends at the
next boundary; an already aligned address defines one full boundary unit.
`--array`, `--alignment`, `--data`, `--string`, `--undefine` and `--clear` are
mutually exclusive. `--flags` composes single-bit enum names and keeps residual
numeric bits; omitting it chooses an exact member or the numeric value.

Reopening that binary in the GUI reads the same arrays, alignment extents,
local types and enum presentation. The native SDK/CLI disassembly keeps raw
`op_str` and adds `display_op_str` when an enum changes its display; text
output uses that same spelling. Unknown/deleted enum types fall back to the
original number. None of these display settings supplies analysis semantics.
