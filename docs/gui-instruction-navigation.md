# Immediate search and physical register navigation

The Search menu offers decoded immediate search (`Alt+I`, repeat `Ctrl+I`).
Enter a decimal integer or a `0x` hexadecimal integer; negatives match signed
logical operand values. Leading zeros keep decimal meaning (`010` is ten).
A sign-extended `-1` in a 32-bit operand matches `-1`
or `0xffffffff`, not `0xffffffffffffffff`. Instruction matches retain every
matching operand index and occupy one pagination row. The direction follows
the existing Search direction setting; wraparound is a frontend operation.

Search scans initialized executable ranges in bounded forward-decoded chunks,
including for reverse results. It does not assert reachability. Failed decoding
or unknown instruction modes leave explicit gaps and resume only at known
function or image entries. The UI reports partial coverage. This is not a byte
pattern search, and memory displacements are not immediate operands.
Newly discovered or removed function entries rebuild the search index; name
and extent changes alone preserve cached decoding and matching progress.

In disassembly, select an instruction and a register token. `Shift+Alt+Up`
lists reaching definitions before that instruction; `Shift+Alt+Down` lists
uses of the register value after it. Multiple reaching definitions, partial
register writes, branches and loops are retained. If no register token is
selected, a picker offers the decoded instruction's retained register operands.
These queries stay within the original function; calls and opaque operations
are explicit boundaries rather than inferred ABI clobbers.

`Shift+Alt+V` shows a static value **before** the selected instruction. Only
values agreed on by every ordinary incoming path and supported integer
transfers are reported. Entry/exception values, memory loads, differing paths,
calls, missing undefined-output coverage and architectural undefined outputs
remain unknown. This is not a live debugger register value. Unsupported native
lifting fails clearly. Native x86, x64, ARM and AArch64 use the same query core;
constant display is limited to registers of at most eight bytes.

The SDK owns decoding, matching, paging and register dataflow:
`neverd_search_page` accepts `kind: "immediate"` and returns `building`,
`decoded_bytes`, `coverage`, `undecoded_ranges`, and the ordinary page cursor.
While `building` is true, repeat the same request without changing its cursor.
`neverd_register_flow_json` accepts `kind` (`registers`, `definitions`, `uses`,
`value`) and a decoded `register` name. Both are read-only; they never rewrite
the retained IR or native emission. Limits fail explicitly instead of claiming
complete results.

The CLI consumes those same SDK queries:

```console
neverd search app.exe --immediate 0x1234 --max-results 100 --json
neverd search app.exe --immediate -1 --from 140001000 --backward --json
neverd disasm app.exe --func 140001000 --instruction 140001020 --register-flow registers --json
neverd disasm app.exe --func 140001000 --instruction 140001020 --register-flow definitions --register rax --json
neverd disasm app.exe --func 140001000 --instruction 140001020 --register-flow uses --register rax --json
neverd disasm app.exe --func 140001000 --instruction 140001020 --register-flow value --register rax --json
```

The immediate CLI completes bounded index work before printing a page. Its
`next_address` remains exclusive, as in the GUI/worker transport. JSON register
queries preserve unknown/boundary diagnostics in every frontend. Tests cover
branch disagreement, partial writes and zero extension, temporary lifetimes,
independent roots, calls, BSF undefined results, signed immediates, multiple
operands per instruction, reverse paging, long scans and CLI/worker agreement.
