# External bytecode profiles

`neverd-bytecode` recovers source from a caller-specified instruction language.
It accepts raw instruction bytes, a JSON encoding profile, and explicit function
ranges. The decoder lives in `lib/analysis/bytecode`; it lowers to ordinary LowIR
and uses the existing MedIR, HighC and LLVM-to-C backends. Profiles and input
containers are supplied separately. There is no built-in dialect, proprietary
opcode table, container signature or host-service registry.

The command-line tool, public C API and Python SDK use one recovery pipeline
in `lib/pipeline/BytecodeRecovery.cpp`. The decoder remains the authority for
encoding and CFG validation; the pipeline owns state lowering and source-route
selection. Plugins supply their own container reader and rules.

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

`--unaligned-pointers` requests direct scalar pointer accesses from either C
route. The emitted typedefs use Clang/GCC `aligned(1)` and `may_alias`
attributes, so an unaligned state buffer and overlapping integer/float views
retain their byte access semantics. These are host addresses under the same
contract above; the option does not translate guest virtual addresses or infer
original source types. The default keeps portable byte-copy accesses. Atomic,
segmented, partial-width and other unsupported carrier shapes retain their
existing exact access paths. The shared C emitter option is
`CEmitterOptions::UseUnalignedPointers`.

Normal fields, arrays and pointer expressions require proven object and type
information on either native architecture. ARM64 does not inherently require
memory helper calls. The explicit bytecode state ABI alone does not establish
original object types or alignment, so this API keeps portable copy helpers by
default; the optional compiler-specific spelling does not supply that proof.

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

`--with-context` selects a second, opaque parameter for every recovered function
and bound call:

```c
uint64_t callee(void *state, void *context);
```

The caller supplies the context when executing the generated C. Its pointer is
captured once on entry and passed unchanged through internal calls and external
bindings, including loops back to the entry. The callback owns the meaning of
its contents, which may change or alias state or guest memory; the lowering
assumes no disjointness. A null context is passed through without inspection.
Independent or reentrant invocations may use different contexts without a global
variable or a reserved state slot. Status propagation is unchanged. All bound
implementations must use the selected ABI; the tool neither provides their
bodies nor manages context lifetime. This runtime parameter is separate from
the decoder callback context used while recovering source.

Bindings supply names and signatures, not implementations. Register-indirect
calls must first be resolved by the producer or explicitly modeled through a
bound dispatcher with a defined state contract; guest integers are never cast
automatically to host function pointers. Native callbacks, suspension/resume
behavior, pointer relocation and original high-level argument recovery remain
the producer's responsibility.

## C and Python plugin API

Include `neverd/sdk/NeverDCAPI.h` and call:

```c
const char *report = neverd_bytecode_recover_json_v1(
    code, code_size, request_json, request_size);
/* Parse report, including its ok field; null is an allocation failure. */
neverd_free_string(report);
```

No loaded native image or session is required. Both input pointers are borrowed
only for this synchronous call. The JSON length is explicit and does not include
a terminating NUL. Each input is limited to 64 MiB. The strict request is:

```json
{
  "schemaVersion": 1,
  "profile": {"version": 1, "register_bytes": 8, "byte_order": "little", "encodings": []},
  "functions": [{"entry": 0, "end": 4, "name": "example"}],
  "bindings": [],
  "base": 0,
  "output": "highc",
  "optimize": false,
  "unaligned_pointers": false,
  "with_context": false
}
```

Replace the empty `encodings` placeholder with a nonempty profile such as the
example above. Only `schemaVersion`, `profile` and `functions` are mandatory.
`output` is `check`, `highc` (default) or `llvmc`; optimization requires `llvmc`.
`with_context` is a boolean, default false, selecting the source contract above.
Unknown fields and wrong field types fail. Function declarations, bindings and
whole-request instruction/operation budgets are validated by the same engine
used by the CLI, including binding declarations supplied in check mode.

Every non-null response is owned, including errors. Success contains
`schemaVersion: 1`, `ok: true`, `functions`, `blocks`, `decoded_instructions`,
`decoded_bytes`, `input_bytes`, `scope`, `with_context`, and `source`. The boolean
`with_context` reports the selected ABI even in check mode, without claiming
source conversion or execution was verified. Scope is `cfg` for check
mode, whose source is empty, or `state-c` for source emission. Failure contains
`ok: false` and `error`, without partial source. Counts cover reachable bytes;
unvisited input is still unclassified.

Python plugins can use the typed wrapper:

```python
from neverd_plugin import recover_bytecode

result = recover_bytecode(
    b"\xb4\x34\x12\xe7", profile,
    [{"entry": 0, "end": 4, "name": "example"}],
    output="llvmc", optimize=True,
)
print(result.source)
```

Here `profile` is the complete encoding example above. The wrapper owns and
releases native responses and raises `NeverDError` for recovery failures. For
an explicitly loaded library outside the plugin host, pass `api=HostAPI(library)`.
Both Python recovery functions accept `with_context=True`; their result records
the selected ABI. They do not accept or retain the future runtime context. An
older engine can still serve the default unary ABI, but must explicitly confirm
the context option before the wrapper returns a result requesting that ABI.
See the runnable [C plugin](../plugins/bytecode/bytecode_plugin.c) and
[Python plugin](../pluginsdk/python/examples/external_bytecode.py).

### Per-instruction decoder callbacks

When selecting an encoding needs computation or external context, C and Python
plugins can supply a synchronous decoder instead of a static encoding table.
The request uses `layout` in place of `profile`:

```json
{"schemaVersion":1,
 "layout":{"version":1,"register_bytes":8,"byte_order":"little"},
 "functions":[{"entry":0,"end":1,"name":"example"}],
 "output":"highc"}
```

Layout accepts `version`, `register_bytes`, `byte_order` and optional
`temporary_bytes`; `encodings` is forbidden. All other request, result, call
binding and state-ABI contracts remain the same. The CLI continues to accept
static profiles; dynamic callbacks are available through the library APIs.

```c
static void decode(void *context, const unsigned char *bytes, size_t size,
                   uint64_t pc, ND_BytecodeInstructionSinkV1 reply,
                   void *reply_context) {
  const unsigned key = *(const unsigned *)context;
  const char *json = size && (bytes[0] ^ (unsigned char)(pc + key)) == 0xe7
      ? "{\"size\":1,\"operations\":[{\"op\":\"RETURN\",\"inputs\":[]}]}"
      : "{\"error\":\"unknown instruction\"}";
  reply(reply_context, json, strlen(json));
}

/* code and request must describe this example language and its function range. */
const char *report = neverd_bytecode_recover_decoder_json_v1(
    code, code_size, request_json, request_size, decode, &key);
/* Inspect report's ok/error fields before using its source. */
neverd_free_string(report);
```

Include `<string.h>` for `strlen`. The callback receives the logical PC and
up to 4,096 borrowed bytes from that PC, truncated at the declared function end.
The window bound avoids copying a whole function for each instruction in Python.
It replies exactly
once, before returning, with `{size, operations}` using the existing operand
grammar and **no `match` field**, or with `{error: "diagnostic"}`. The sink
copies the explicit-length JSON before returning; NUL termination is unnecessary.
It returns 1 for accepted bytes and 0 for invalid, repeated or oversized replies.
Acceptance means copied, not semantically validated. A callback reply is limited
to 64 MiB, an instruction to 4,096 bytes and 4,096 operations. Missing replies,
any rejected reply, invalid JSON and unsupported semantics fail the recovery
without partial source. The sink, its context and the input pointer must not
escape the invocation. No exception may cross the C callback boundary.

The decoder and its context are borrowed for the recovery call, invoked on the
calling thread and never retained afterward. Separate calls have independent
reply state and can be nested. A caller sharing its own mutable plugin context
across threads is responsible for synchronizing that context. Results must
depend only on bytes, PC and stable caller context. CFG discovery order is not
execution order: rolling keys or other path-dependent decoding state must first
be resolved by the producer. The callback does not add a path-state interpreter.
Instruction/operation budgets bound recovered graphs; they cannot preempt
trusted plugin code inside a callback.

The Python wrapper copies the byte window and handles reply/result ownership:

```python
from neverd_plugin import recover_bytecode_with_decoder

key = 37

def decode(data: bytes, pc: int) -> dict:
    opcode = data[0] ^ ((pc + key) & 255)
    if opcode == 0xb4 and len(data) >= 3:
        return {"size": 3, "operations": [{
            "op": "COPY", "output": {"space": "reg", "size": 2, "value": {}},
            "inputs": [{"space": "const", "size": 2,
                        "value": {"addend": int.from_bytes(data[1:3], "little")}}]
        }]}
    if opcode == 0xe7:
        return {"size": 1, "operations": [{"op": "RETURN", "inputs": []}]}
    raise ValueError(f"unknown or truncated instruction at {pc:#x}")

result = recover_bytecode_with_decoder(
    bytes([0xb4 ^ key, 0x34, 0x12, 0xe7 ^ (3 + key)]),
    {"version": 1, "register_bytes": 8, "byte_order": "little"},
    [{"entry": 0, "end": 4, "name": "example"}], decode,
    output="llvmc", optimize=True,
)
```

The wrapper catches Python exceptions inside the trampoline, reports a decode
failure, releases the native response, then re-raises the original exception.
It never invokes that decoder again after an exception. Returning an explicit
error mapping instead produces `NeverDError`. Non-mappings, nonfinite JSON and
oversized replies fail. A closure may carry stable private context; neither the
callable nor the closure is registered globally or retained by recovery.

These APIs do not infer indirect targets, recover an original source ABI, or
implement unknown host services. A profile or callback is a supplied semantic
specification, not proof that it matches an arbitrary interpreter.
The current source route retains the CLI's AArch64 state carrier and floating
conversion policy. Engine-specific policies must be modeled explicitly in the
supplied operations. Input execution and host-service modeling are separate
from source recovery; native and Python plugins remain trusted host code.

The C++ entry points are `readBytecodeProfile`, `BytecodeDecoder::create`,
`readBytecodeLayout`, `readBytecodeEncoding`, `BytecodeDecoder::createExternal`,
`BytecodeDecoder::decode`, `BytecodeDecoder::function` and
`lowerBytecodeState`. The tool demonstrates source ABI binding, call recovery,
external LLVM declarations and both C routes. Default resource bounds include
64 MiB per input file, 65,536 encodings, 4,096 bytes per instruction, 65,536 bytes
per state/temporary bank, and per-function instruction/operation budgets.

Profiles are semantic specifications. Successful decoding, C compilation and
even execution on selected inputs do not prove equivalence to a separate native
interpreter. Native instruction boundaries and rewrite authorization are never
created by this path.
