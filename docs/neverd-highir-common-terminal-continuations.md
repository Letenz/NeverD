# highir common terminal continuations

## Ownership and behavior

HighIR can share equivalent terminal continuations after an optional prefix.
Matching checks types, operands, memory ordering and actual call targets; only
temporary names confined to the copies can differ. Reads stay after preceding
calls, and jump entries and exception wrappers prevent an unsafe merge.

## Reproduction and validation

`HighSourceExpressions.CommonTerminalTail*` executes original and folded
source at O0/O2 against a helper that changes the subsequently read memory.
Negative cases cover call targets, operands, ordered accesses, escaping names,
labels, unknown values and exception wrappers. Run `NeverDHighControlFlowTests`
and `NeverDWindowsEHSourceTests`.
