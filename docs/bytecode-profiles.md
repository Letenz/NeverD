# External bytecode profiles

`neverd-bytecode` recovers source from a caller-specified instruction language.
It accepts raw instruction bytes, a JSON encoding profile, and explicit function
ranges. The decoder lives in `lib/analysis/bytecode`; it lowers to ordinary LowIR
and uses the existing MedIR, HighC and LLVM-to-C backends. Profiles and input
containers are supplied separately. There is no built-in dialect, proprietary
opcode table, container signature or host-service registry.

```sh
cmake --build build-release --target neverd-bytecode NeverDBytecodeAnalysisTests
build-release/bin/neverd-bytecode program.bin --profile language.json \
  --functions functions.json --check
build-release/bin/neverd-bytecode program.bin --profile language.json \
  --functions functions.json --bindings imports.json -o recovered.c
build-release/bin/neverd-bytecode program.bin --profile language.json \
  --functions functions.json --bindings imports.json --llvm -o recovered-llvm.c
build-release/bin/neverd-bytecode program.bin --profile language.json \
  --functions functions.json --bindings imports.json --llvm --optimize \
  -o recovered-optimized.c
```

`--base` sets the logical address of the first input byte (default zero).
`--check` validates reachable decoding and control flow; it does not claim
semantic equivalence or require call implementations. Source output uses an
atomic replacement after complete conversion. An unknown encoding, malformed
operand, ambiguous match, overlapping instruction, unresolved indirect branch
or unbound source call is an error. Unsupported behavior never becomes a NOP.

Straight-line instructions share a basic block; branch targets and call
continuations start new blocks. State lowering forwards exact register views
within each block, invalidating overlapping views on partial writes and all
views on potentially aliasing guest stores. It preserves state writes visible
to guest memory accesses and calls, without assuming a disjoint state buffer.
Whole state writes may be removed when overwritten before a memory observer;
different-width state loads also count as observers.

`--llvm --optimize` applies the existing thin LLVM optimization policy before
C emission, simplifying scalar expressions and control flow while retaining
function boundaries. It does not infer a new function ABI or drop observable
register state. The default LLVM route only promotes temporary storage.
`--optimize` without `--llvm`, or together with `--check`, is rejected. All
transformed modules must pass verification; unsupported C projection still
fails before replacing the output file.

## Encoding schema

The following independently constructed language writes a 16-bit immediate to
the low halfword of its only 64-bit register, then returns:

```json
{
  "version": 1,
  "register_bytes": 8,
  "temporary_bytes": 64,
  "byte_order": "little",
  "encodings": [
    {
      "size": 3,
      "match": [{"offset": 0, "value": "0xb4"}],
      "operations": [{
        "op": "COPY",
        "output": {"space": "reg", "size": 2, "value": {"addend": 0}},
        "inputs": [{"space": "const", "size": 2,
                    "value": {"offset": 1, "bytes": 2}}]
      }]
    },
    {
      "size": 1,
      "match": [{"offset": 0, "value": "0xe7"}],
      "operations": [{"op": "RETURN", "inputs": []}]
    }
  ]
}
```

Input bytes `b4 34 12 e7` with this function declaration produce
`uint64_t example(void *state)`:

```json
[{"entry": 0, "end": 4, "name": "example"}]
```

The caller supplies at least eight bytes of state. The function replaces bytes
zero and one with `34 12`, preserves the remaining bytes and returns zero.
Function ranges are half-open, disjoint logical intervals. Names must be unique
C identifiers; C keywords and reserved leading underscores are rejected.
Only reachable instruction bytes count toward reported coverage. Unvisited
bytes are not assumed to be padding or to belong to another function.

Every match tests `(instruction[offset] & mask) == value`; the default mask is
255. All tests in an encoding must hold. Multiple matching encodings are errors,
including a complete short match with a potentially matching truncated longer
encoding. Instruction and operand reads stay inside the declared encoding.

Operands have a `space` (`reg`, `temp` or `const`), byte `size` (1, 2, 4 or 8)
and `value`. The value expression is:

```text
index = (read(offset, bytes, byte_order) >> shift) & mask
selected = lookup[index] if lookup is present, otherwise index
value = (selected * scale + addend + (pc_relative ? pc : 0)) mod 2^bits
```

| Field | Default | Meaning |
|---|---|---|
| `offset` | 0 | Byte offset within this instruction |
| `bytes` | 0 | Read width; zero supplies zero without reading |
| `shift` | 0 | Unsigned right shift before masking |
| `mask` | All 64 bits | Field bit mask |
| `lookup` | Absent | Exact finite mapping; an invalid index fails |
| `scale` | 1 | Unsigned multiplier |
| `addend` | 0 | Unsigned additive constant |
| `bits` | 64 | Final modular width, from 1 to 64 |
| `pc_relative` | false | Include this instruction's logical address |

Integers can be unsigned JSON numbers or strings such as `"0xffff"`. Negative
offsets use modular bit patterns and an explicit `bits` width. Signed arithmetic
and sign extension are LowIR operations; field extraction is unsigned.
Register and temporary offsets cannot overflow or truncate into a valid bank.
Only constants can be PC relative. Unknown JSON keys are rejected.

## Semantic contract

Register operands identify byte ranges. Writes preserve bytes outside the exact
output range, including narrow and overlapping writes. Temporary operands are
typed instruction-local values; a read requires a preceding definition with the
same offset and width. Overlapping writes invalidate prior temporary views.
Use `SUBBYTES` and `CONCAT` to express slicing explicitly.

An operation names a supported scalar LowIR opcode and an `inputs` array, with
`output` only for operations producing a value. Arity and widths are checked.
Profiles must specify exceptional arithmetic behavior themselves: division by
zero, signed overflow and oversized shifts cannot be inferred from an opcode
name. For example, a language that masks shift counts must emit the mask.

`BRANCH` takes an 8-byte constant target; `COND_BR` takes a target and a one-byte
condition. `CALL` takes an 8-byte constant target and has no output in the raw
bytecode model. Control transfers must be the last operation of their encoding.
`RETURN` explicitly denotes a language-level return. A native link register or
stack convention is not inferred from that operation.

Source recovery binds registers to a caller-owned, little-endian state buffer.
Instruction-field byte order is independent. Guest LOAD/STORE addresses remain
runtime pointer values and must designate accessible host memory. Generated C
is an offline source projection, not an emulator with mapped guest addresses.
The input is never executed by the recovery tool.

Every bound callee has the same contract:

```c
uint64_t callee(void *state);
```

A callee may modify all state and guest memory. Zero continues at the following
bytecode instruction; a nonzero status returns immediately through every caller.
The state pointer is captured once on entry, even for a loop back to the first
instruction. Imported calls require explicit declarations:

```json
[{"address": 12288, "name": "example_callback"}]
```

Bindings supply names and signatures, not implementations. Register-indirect
calls must first be resolved by the producer or explicitly modeled through a
bound dispatcher with a defined state contract; guest integers are never cast
automatically to host function pointers. Native callbacks, suspension/resume
behavior, pointer relocation and original high-level argument recovery remain
the producer's responsibility.

The C++ entry points are `readBytecodeProfile`, `BytecodeDecoder::create`,
`BytecodeDecoder::decode`, `BytecodeDecoder::function` and
`lowerBytecodeState`. The tool demonstrates source ABI binding, call recovery,
external LLVM declarations and both C routes. Default resource bounds include
64 MiB per input file, 65,536 encodings, 4,096 bytes per instruction, 65,536 bytes
per state/temporary bank, and per-function instruction/operation budgets.

Profiles are semantic specifications. Successful decoding, C compilation and
even execution on selected inputs do not prove equivalence to a separate native
interpreter. Native instruction boundaries and rewrite authorization are never
created by this path.
