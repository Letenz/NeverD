# Explicit binary modules and debug information

`NeverDModules` owns a format-independent catalogue of caller-supplied binary
and debug files. System files and privately built modules use the same interface.
The resource layer supports eager/lazy preparation, strict debug identity,
transactional guest mappings and resource repair at validated CPU stops.
The C SDK, Python binding and JSON-lines CLI expose the same persistent raw-buffer
execution owner. Its only registered environment is `buffer`; other environment
kinds fail explicitly. Windows driver and Ring3 adapters are separate consumers.

## Declare and prepare inputs

```json
{
  "base_directory": "D:/analysis-inputs",
  "module_defaults": {"load": "eager"},
  "modules": [
    {
      "id": "kernel",
      "image": "ntoskrnl.exe",
      "debug": {"kind": "pdb", "path": "ntkrnlmp.pdb"}
    },
    {
      "id": "support",
      "image": "support.sys",
      "load": "lazy"
    }
  ]
}
```

See the [configuration schema](../resources/modules/schema/modules.schema.json).
An explicit absolute base directory resolves relative paths. Absolute input
paths need no base. Registration performs no file access and permits files
that will be supplied later. Unknown fields, duplicate caller IDs, invalid
policies and excessive declarations fail before reading files.

```cpp
#include "neverd/modules/ModuleSet.h"

auto Modules = neverd::modules::ModuleSet::fromJSON(Configuration);
if (!Modules)
  return Modules.takeError();
if (auto E = (*Modules)->prepareEager())
  return E;

// A consumer explicitly asks for a lazy resource when it needs it.
auto Support = (*Modules)->prepare("support");
if (!Support)
  return Support.takeError();
```

`prepareEager()` prepares all eager declarations as one catalogue update. If
any input fails, none of that batch is published. A module's `load` overrides
the default; the default is `eager`. `lookupSymbol(id, name)` prepares that
module on demand. These operations prepare file resources; they do not map a
guest address space or execute an initialization entry point.

`reviseUnprepared(id, image, optionalDebug)` corrects a declaration whose input
has never published an observation. It replaces the image/debug declaration,
retains its ID and load policy, validates paths without file IO, and increments
the catalogue revision only on success. This permits retrying an omitted file
or incorrect companion while an execution retains its resource request.
Prepared inputs stay fixed; `attachDebug` remains the operation for adding
debug information to an existing observation.

`moduleOptionsFromFile(path)` reads an explicitly named, bounded regular JSON
configuration file. An explicit JSON `base_directory` selects its input origin;
otherwise relative module/debug paths use that file's absolute parent directory.
It does not open binary/debug inputs. Direct `moduleOptionsFromJSON` calls keep
their explicit-base conflict check; a file origin is a fallback rather than a
second caller assertion.

## C SDK and module CLI

Include [`NeverDCAPIModules.h`](../include/neverd/sdk/NeverDCAPIModules.h) or the
umbrella C header. `neverd_module_set_t` is a process-local opaque 64-bit identity,
with zero invalid and no reuse during the library's lifetime. It does not require
a decompiler session or CPU/driver emulation. All resource operations consume
the same `ModuleSet` implementation described above.

`neverd_module_set_create_v1` accepts an exact-size versioned structure containing
configuration JSON and an optional explicit absolute base. NULL options create
an empty catalogue. `neverd_module_set_from_file` is the named-configuration
convenience operation with file-origin handling. Create/register/inspect do not
prepare binary/debug files, including eager entries.

The resource operations are `add_json`, `revise_unprepared_json`, `prepare`,
`prepare_eager`, `attach_debug_json`, `inspect_json`, `lookup_symbol_json` and
`destroy`, all prefixed with `neverd_module_set_`. Add/revise accept a single
module object. Revision must omit `load` and retains the original policy; debug
attachment accepts a `kind`/`path` object. Lookup returns static original-image
addresses and a successful `found=false` result when no symbol matches.

Each operation clears its optional `const char **Error` output. Failure returns
zero/NULL and an owned UTF-8 diagnostic; JSON results are also owned. Free both
with `neverd_free_string`. Diagnostics remain within 64 KiB without splitting a
UTF-8 character, and JSON output stays within 64 MiB. Input/error-output storage
must be separate and borrowed inputs must remain readable during the call.
Concurrent operations on one source catalogue report busy rather than racing
mutable parser/query caches. Destroying a handle invalidates future lookups;
an operation that already acquired its resources can finish safely, and copied
reports remain valid after destruction.

```c
#include "neverd/sdk/NeverDCAPI.h"
#include <stdio.h>

int validate_modules(const char *configuration_path) {
  const char *error = NULL;
  neverd_module_set_t set = neverd_module_set_from_file(configuration_path, &error);
  if (!set) {
    fprintf(stderr, "%s\n", error ? error : "module configuration failed");
    neverd_free_string(error);
    return 1;
  }
  int ok = neverd_module_set_prepare_eager(set, &error);
  if (ok) {
    const char *report = neverd_module_set_inspect_json(set, &error);
    ok = report != NULL;
    if (report) {
      puts(report);
      neverd_free_string(report);
    }
  }
  if (error) {
    fprintf(stderr, "%s\n", error);
    neverd_free_string(error);
  }
  neverd_module_set_destroy(set, NULL);
  return ok ? 0 : 1;
}
```

The CLI calls these C functions, including configuration-file reading; it owns
no additional loader, identity or path-resolution policy:

```sh
neverd modules inspect /path/to/modules.json
neverd modules validate /path/to/modules.json
neverd modules lookup /path/to/modules.json --module=kernel --symbol=KdDebuggerDataBlock
```

`inspect` reports declarations without preparing inputs. `validate` prepares
eager inputs and leaves lazy entries unprepared. `lookup` prepares only the
queried source. Exit codes are 0 for success/found, 1 for invalid configuration or
operation failure, and 2 for a successful symbol query with no match. JSON goes
to stdout and diagnostics to stderr. The Python plugin ABI table also declares
the fixed-width handle, exact v1 layout, owned results and owned-error pointers;
the existing `HostAPI` binding can call them without a decompiler session.

## File identity and lifetime

Preparation reads bounded source bytes and retains private, read-only copies
for the existing path-based binary and debug loaders. This provides one input
snapshot for parsing, deferred symbol reads and future execution consumers
without creating another format parser. Replacing the caller's source files
does not change an already prepared observation. The private files are removed
when their last observation is released, including after the catalogue dies.
`preparedImagePath()` and `preparedDebugPath()` are borrowed, read-only paths
whose lifetime is the owning `ModuleImage`; consumers must not modify them.

`ModuleImage` exposes original bytes, the loader's immutable `BinaryImage`,
source SHA256 identities and the debug provider. It owns no guest base, process,
session, CPU or OS objects. `ModuleInstance` supplies execution-specific bindings.
Current operations require caller serialization, as do the underlying debug
provider caches. No host libraries are executed and no files are downloaded.

## Explicit debug files

`debug.kind` selects `pdb`, `dwarf` or `map`. No debug declaration means image
only: the catalogue does not search recorded build paths or neighboring files.
A declared companion is authoritative and an error never falls back to another
format. PDB authentication uses the shared PE/PDB identity loader, including its
linked DBI-age policy. Explicit split DWARF retains companion trust; MAP files
provide names without authenticated ABI or object-layout claims.

`attachDebug(id, input)` validates a new companion and publishes a new observation
sharing the original binary snapshot. Existing holders retain their previous
observation. A failed attachment changes neither the declaration nor its
prepared image, byte accounting or revision. Replacing an already declared
debug input is unsupported.

`lookupSymbol()` returns an original image address and symbol kind, not an
implicitly chosen guest address. Equal-name observations at different addresses
or with incompatible kinds fail as ambiguous. Symbol names and debug presence
do not authorize native execution or make runtime global values known.

## Inspection and bounds

`inspect()` and `moduleSetJSON()` never materialize lazy inputs. Reports include
resolved source paths, effective load policy, prepared status, source bytes,
hashes, format/architecture, original base, image CodeView identity when present,
and the debug provider's authenticated-semantic capabilities. `debug_loaded`
means the selected provider loaded successfully; `debug_identity_verified`
currently applies to matched PDB input. A parsed MAP or unauthenticated split
DWARF companion does not acquire image-identity verification.

Defaults: 256 modules, 256 MiB per image, 512 MiB per debug file, 1 GiB of retained
source bytes, and 1,000,000 symbol observations per query. Optional `limits`
fields can select finite bounds up to the schema ceilings. Source-byte accounting
covers retained file inputs; it is not a bound on all loader caches or guest RAM.
Configuration JSON is limited to 2 MiB; paths are limited to 4096 UTF-8 bytes.

## Execution module instances

`NeverDEmulationModules` provides `ModuleBindings`, `ModuleBinding`,
`ModuleInstance` and the shared resource request/resolver path. This component
depends on Modules, EmulationImage and EmulationRuntime; it has no Windows
dependency. The low-level CPU/runtime components do not depend on module parsing.

A binding names one resource, an instance ID, guest name, scope, address space
and optional base/extent. Declaration reads no file and is visible in `inspect()`
for eager and lazy instances alike. Guest names are exact identities within a
scope/address space; an OS adapter owns case rules and aliases. IDs are unique
within one bindings owner. The same source may have separate instances with
independent RAM at different bases or in different address spaces.

`materialize(batch, builder)` prepares all source observations and mapping plans
before publishing RAM, instance state and namespaced exports together.
`prepareEager(space, builder)` selects eager declarations in that address space
and uses the same transaction path. Lazy instances remain declarations until
explicitly requested and supplied. There is no default OS loader: the required
builder owns relocation, imports/forwarders, privilege, header visibility and
runtime-data knowledge. It must return prepared bytes and final permissions
without modifying the CPU, live memory or OS tables. This boundary does not
grant unsupported native code or disk-initialized kernel globals execution or
runtime-value authority.

An environment may `adopt(batch, validator)` to publish pinned observations over
mappings it already owns. The adapter must provide a pure proof of the existing
source/mapping identities and export ownership. The common layer checks extents,
conflicts, limits and export names, kinds and ordinals. Preexisting external
export addresses may remain opaque metadata; adoption grants no RAM backing or
access permissions. Guest materialization still requires backed forwarder
targets. Adoption requires fresh IDs, one
stopped address space and no pending resource request. It permits a pinned source
whose original catalogue or files no longer exist. Environment metadata patches,
if any, join the same RAM transaction. Without patches, a metadata publication
lease changes no RAM, mapping generation, physical capacity or reservation
witnesses. `prepareMappings` still rejects an empty batch. This internal adapter
entry is not a caller-supplied binding flag or an initialization request.

`lookupSymbol(instance, name)` projects the instance's pinned source symbol into
its guest extent. `lookupExport` selects a name or ordinal within that instance.
An export address is metadata and can denote opaque, inaccessible storage;
successful lookup supplies no read/execute permission. Missing instance resources
do not trigger implicit mapping from these query methods. The source observation
is pinned at materialization; later debug attachments preserve old observations
and do not silently replace an instance's execution inputs.

## Resource requests and publication

`requestModule`, `requestDependency` and `requestAddress` produce one pending
request per bindings owner. Address requests retain PC, address, size and access
kind. Only a declared, unmapped range covering the access supplies a module ID;
an unknown address remains an `UnmappedCandidate` without a guessed file name.
Fully mapped memory, including protection failures, cannot be reclassified as a
missing module. Requests record a question; they do not prove a faulted CPU can
resume.

`supply(token, batch, builder)` verifies owner/version/sequence before file IO.
The batch must satisfy the instance or exact dependency identity and target
address space. Address supply checks the whole access across new ranges and
existing pages, including final permissions and holes. OS dependency aliases
must be canonicalized before requesting. Failed supply retains the pending
request and original guest/module state; successfully cached file resources may
remain prepared. Successful supply consumes the token once. Cancellation also
invalidates it. Existing mapped instances cannot be replaced or unloaded.

`resolveModuleRequest` invokes a resolver once while stopped, outside CPU/model
callbacks. It may register omitted resources in the shared `ModuleSet` and return
bindings, request human input or report no match. Found bindings use the same
`supply` path. Resolver recursion and mutation of its bindings owner are rejected.
No callback starts execution, downloads files or searches host directories
implicitly. The execution owner must provide the stopped boundary. Low-level
resource pauses are available through `ExecutionSession` and `ExecutionRun`;
OS adapters register separately; this interface does not infer an OS model.

An optional `ModulePublicationPreparer` prepares environment metadata from the
candidate instance table. Its `ModulePublication` may expose guest RAM patches
for module lists/IAT-like fields and commits host metadata with a nonthrowing
swap. Preparation must have no visible effects. Its commit cannot allocate,
fail, invoke other callbacks, execute CPUs or mutate memory directly.

`AddressSpace::prepareMappings` owns the common physical reservation and
publication boundary. The transaction holds the physical owner's stopped lease,
copies initial bytes and backing patches, rejects overlaps (including physical
patch aliases), and publishes final rights without a temporary writable mapping.
Abandonment releases reservations while preserving existing bytes, aliases,
mapping generation and reservation witnesses. Committed patches retain physical
identity and invalidate affected reservation witnesses. CPU execution/mutation
is rejected until RAM and consumer metadata publication finish and the transaction
is released. Transactions must remain on their creating thread. Backend
projections synchronize through the existing mapping-generation check before
execution; a projection error remains terminal and does not claim guest rollback.
One batch targets one address space, without a cross-space atomicity guarantee.

`refreshPublication(space, builder)` runs the adapter publication over existing
instances without requiring a new binding. A typed request for a declared,
unmapped dependency may expand the transaction through the same builder and
finalizer as materialization. Empty candidate batches skip finalization. Guest
patches and instance/adapter metadata commit together; a refresh without guest
patches uses the metadata lease. Failed source/link/patch preparation publishes
nothing. Refresh rejects a pending request; that request must use `supply`.

An optional `ModuleMappingFinalizer` links the complete stopped batch after its
byte layouts have been validated. It sees the previous instance table and
`ModuleMappingCandidate` references, and may change only draft bytes/exports.
Adapters can return immutable `ModuleMappingPreparation` input from a builder;
the finalizer must consume it on success. Those references never become live
instance state and must not escape the callback. A finalizer may also publish immutable
`ModuleRuntimeData` for adapter-owned facts tied to its final bytes. The common
instance table retains those facts but does not interpret them or grant runtime
capabilities from their presence.

`ModuleDependencyError` identifies a missing provider and, optionally, an already
declared instance. A known declaration in the same address space expands the
same atomic batch; the failed drafts are discarded before retry. Unknown inputs
remain explicit resource questions. Batch instance/mapped-byte limits and final
layout/export validation still apply. `ModuleDependencyMatcher` delegates an
environment's provider aliases; omission retains exact generic identities.

`ModuleExport::Forwarded` distinguishes an environment-resolved external target
from an ordinary direct export. It requires finalization and an appropriately
permitted planned/existing RAM target, while direct exports keep their original
image-bound rule. Metadata cannot grant guest permissions, model behavior or a
function ABI. The Windows adapter owns PE forwarding policy; the common layer
does not parse PE identities or search for files.

## Validated CPU resource stops

`BackendHooks::ResourcePause` is an optional, pure admission policy. It receives
a provisional candidate within the backend's supported restart shapes and must
not perform file IO, resolve modules, mutate/execute CPUs or dispatch models.
Only a subsequent typed `ExecutionExitKind::ResourcePause` publishes the final
backend validation. Existing callers that omit the policy retain their current
fault behavior. An untyped `CPU.run()` reports a resource stop as an error rather
than implying successful completion.

`ResourcePause` records the fault/access, actual `ResumePC`, originating observed
instruction, restart kind, whether committed memory effects may exist, and the
complete required permission mask. Checked execution uses the architecture's
existing fault boundary, including instruction-prefix stores and completed REP
iterations. It does not abandon those effects through an OS exception transfer.
Direct/native opaque execution does not enable this resource policy yet.

The software Unicorn x64 adapter admits instruction-fetch boundaries, scalar
integer MOV/MOVZX/MOVSX/MOVSXD memory loads, and exact `REP MOVSB` byte iterations.
After Unicorn returns, it verifies the expected status and actual PC before
publishing a resource exit. Other software operand shapes, protection failures,
MMIO/model failures and terminal faults retain their existing behavior. Software
ARM64 rejects resource hooks; checked ARM64 uses its architectural preflight.
The adapter never clears `FirstFault` to resume a terminal CPU.

While a resource pause is pending, CPU mutation and snapshots are rejected.
The owner repairs memory through the address-space/module transaction while
stopped. Module address requests can carry `RequiredPermissions` from the CPU
event; supply must satisfy every bit, including privilege or combined operands.
`ExecutionSession::acknowledgeResourcePause()` checks that the access is now
permitted and retains a mandatory continuation PC. The next run must use the
recorded `ResumePC`; acknowledgement alone does not allow a context replacement
or a different PC. This preserves an already pushed CALL return address and
completed REP progress. An existing watch step-over survives repair, while a
new target watch still stops before its instruction.

`ExecutionEnvironment::prepareExecution` runs while stopped after eager
preparation and before an ordinary CPU action. It can finish adapter metadata or
return a typed module dependency. The run retains the original action and uses
the same quota/request/supply path. Repaired CPU continuations skip this hook.
Time exhausted during preparation stays terminal, including when the hook
returns another error; it cannot create a later resource event or CPU/model
exit callback after the exhausted admission.

`SessionModelHooks` adds a separate OS admission boundary. `Stop` yields an
uncharged `model_boundary`; `Emulate` charges one attempt before yielding, with
no machine effects. `Defer` yields `deferred_instruction` for stopped validation
of an input-dependent model operation. The session retains that exact attempt
until `admitDeferredInstruction()` accepts it once, checking PC, shared credits
and the original run's local deadline. Pending run/context/watch operations are
rejected; quota failure retains the pending attempt. Session watches and call
returns have priority, and CPU faults retain their typed outcome. This is model
admission, without a missing-module request or permission to repair a terminal
fault. OS adapters can use it for thunk/CFG/return boundaries and
modeled processor reads within its retained phase and invocation pump.

`ExecutionEnvironment::prepareCPUEntry` refreshes OS projections of committed
RAM immediately before every CPU entry, after eager mapping and ordinary
dependency preparation. Repaired CPU continuations also use this hook without
repeating `prepareExecution` or the retained scene action. It cannot execute the
CPU, change registers/context or acknowledge the retained stop. Projection
failure is terminal `environment_failure`; it cannot request another resource
or undo already committed modules. Shared time/instruction admission is checked
after success and error, and a cooperative pause arriving during projection is
observed before execution. Inspection and memory snapshots do not run the hook.

## Time policy across pauses

`ExecutionBudget::create()` retains the original absolute-deadline policy.
`createWithTimePolicy(..., ExecutionTimePolicy::ActiveWork)` selects cumulative
active work. `enterWork()` returns an owned scope; nested scopes charge their
union once and stopped intervals between scopes do not consume active time.
Instruction/event credits and fractional clock time persist across resumptions.
Old clock samples cannot replenish an observed active interval. Scopes borrow
their budget, which must outlive them.

`ExecutionSession::run()` owns one work scope automatically. `ExecutionRun` wraps
initialization, its execution loop, module supply and resolver calls in outer
scopes, so resource/model work shares the cumulative budget. It checks time
after environment/resolver callbacks return, including callback errors. This
does not promise a hard wall-clock bound on arbitrary host callbacks.

## Persistent execution C API

[`NeverDCAPIExecution.h`](../include/neverd/sdk/NeverDCAPIExecution.h) provides
15 execution exports independently of analysis sessions. The exact-size
`neverd_execution_options_v1` contains `struct_size`, a valid `module_set` handle
and required `configuration_json`. An execution retains the same source state
after the caller destroys its public module-set handle. Execution handles are
typed, non-reused uint64 identities; passing a module handle as an execution
handle, or using a released handle, fails explicitly.

The configuration has an explicit `environment` object, optional `run` controls
and optional `bindings`. The `buffer` kind accepts explicitly supplied CPU
instruction/data bytes without an OS loader; other environment kinds fail with an
unsupported-environment error; they never select a fallback. Ring3 execution remains a reserved integration point.

```json
{
  "environment": {
    "kind": "buffer",
    "cpu": {"backend": "unicorn", "architecture": "x86_64"},
    "memory_limit": 1048576,
    "entry": "0x10000",
    "stop_pc": "0x10001",
    "stack_pointer": "0x80f00",
    "regions": [
      {"address": "0x10000", "size": 4096, "permissions": "rx", "bytes_hex": "9090"},
      {"address": "0x11000", "size": 4096, "permissions": "rx", "bytes_hex": "4889c84801d0c3"},
      {"address": "0x80000", "size": 4096, "permissions": "rw"},
      {"address": "0xa0000", "size": 4096, "permissions": "rw"}
    ],
    "invocation": {"stack_base": "0xa0000", "stack_size": 4096, "return_pc": "0x10001"}
  },
  "run": {"pause_stages": ["stopped"]}
}
```

Regions must be nonempty, page aligned, disjoint RAM ranges within the explicit
memory limit. Their `permissions` contain unique `r/w/x/u` letters; `bytes_hex`
initializes a bounded prefix and the rest is zero filled. `cpu` uses the existing
execution-configuration parser and capability checks. Creation maps the private
initial RAM but executes no instructions and prepares no module files. Reaching
the executable `stop_pc` yields the `stopped` stage if selected, then completion
after explicit resume. An unhandled fault/transport stop does not count as reaching
that stage. A finite-call plan is optional and has `stack_base`, `stack_size` and
`return_pc`; its stack must be a dedicated writable, nonexecutable region outside
the suspended caller. The existing invocation engine still performs frame checks.

| Operations | Contract |
|---|---|
| `create_v1`, `destroy` | Retain/release one workload; zero destroy is a no-op, stale or wrong-kind identities fail |
| `run_json` | Drive the same `ExecutionRun` to its next event; a failed guest is a valid report, API/precondition failure is NULL |
| `state_json`, `request_pause` | May be called concurrently with run; state reads the atomic lifecycle and pause is cooperative |
| `resume` | Paused to Ready, without execution or quota replenishment |
| `inspect_json`, `instances_json` | Stopped snapshots of cumulative state and actual instance publication |
| `declare_binding_json`, `supply_json` | Declare one instance or supply the retained token through the existing transaction path |
| `invoke_json` | Install a bounded explicit-ABI call; later run events carry its result |
| `read_memory`, `write_memory` | Caller-owned bounded buffers; reads copy on success only, terminal runs reject writes |
| `lookup_symbol_json` | Query a mapped instance and return its bound guest address; found=false is success |
| `command_json` | Validate one command envelope and dispatch through these same primitives and the retained source catalogue |

All operation names above have the `neverd_execution_` prefix. Inputs are
borrowed during a call. Errors and JSON results are owned UTF-8 strings released
with `neverd_free_string`; optional error outputs are cleared on entry. The same
input and report bounds described below apply. Other concurrent operations,
including destroy during a call, return busy. Execution operations coordinate
with the source catalogue guard: another run or source operation cannot race
source preparation/query caches. Stopped users may add a missing source through
the module API, then supply bindings and explicitly run again. Source registration
alone changes no guest instance and resumes no CPU. `state_json`/`request_pause`
do not acquire the busy guard.

The buffer mapper only accepts fixed link-address images of the selected ISA,
without imports, relocations or rebase requests. It maps original file bytes
through `ImageMappingPlan` and the common module transaction, preserving segment
permissions; a nonzero entry in nonexecutable RAM is rejected. OS relocation/import/model policy belongs to each separately registered adapter.
Mapped symbols do not imply modeled API behavior. Python `HostAPI` has matching
ctypes handles, structure, owned-string and byte-buffer signatures.

The exports remain linkable with CPU emulation disabled and report that execution
is disabled. File-resource registration, preparation and symbol queries remain
available in that build. An isolated test target compiles the production
boundary/allocator without any emulation archive; the separate
`NeverDExecutionSDKDisabledTests` links the real feature-disabled SDK and
exercises every execution export, source lifetime and the public allocator.
The disabled CLI tests check startup diagnostics for requested environment kinds. See the complete
[feature-disabled build instructions](testing.md#explicit-module-inputs).

## Interactive execution CLI

Start one persistent workload with explicit configuration files:

```powershell
neverd execution E:\lab\execution.json --modules=E:\lab\modules.json
```

The CLI emits one `created` record containing its initial stopped snapshot, then
reads one JSON command per stdin line. `--modules` is optional; omission creates
an empty source catalogue without a relative-path origin. With a module file,
incremental source paths use that file's resolved origin, as in the module C API.
The execution configuration does not implicitly select a binary directory.

Commands require a nonempty, bounded UTF-8 string `id` and an `op`. Responses
carry that ID, `schema_version: 1`, `ok: true`, and a `result` object. IDs correlate
responses and do not replace the resource request's owner/version/sequence token.
An operation failure emits `ok: false` and an `error` string while retaining the
same workload for inspection or correction. A malformed transport may have a
null response ID. The CLI passes original command bytes to
`neverd_execution_command_json`; the SDK owns strict envelope admission, nested
input validation and dispatch. No frontend normalizes duplicate keys away or
chooses a resource recovery policy.

| `op` | Additional fields |
|---|---|
| `state`, `pause`, `resume`, `run`, `inspect`, `instances`, `modules`, `quit` | None |
| `declare` | `binding`: one module-instance object |
| `supply` | `supply`: the retained `token` and `bindings` batch |
| `invoke` | `invocation`: the finite-call object described below |
| `read` | `address`, `size` |
| `write` | `address`, `bytes_hex` |
| `lookup` | `instance_id`, `symbol` |
| `module_add`, `module_revise` | `module`: the same source input as the module C API |
| `module_debug` | `module_id`, `debug`: the explicit companion descriptor |

For example, send each dependent command after receiving the preceding response:

```json
{"id":"run-1","op":"run"}
{"id":"inspect-1","op":"inspect"}
{"id":"call-1","op":"invoke","invocation":{"entry":"0x11000","abi":"win64","arguments":[{"value":40},{"value":2}]}}
{"id":"call-run-1","op":"run"}
{"id":"quit-1","op":"quit"}
```

The configuration above provides a selected stage and a private call stack. Its
raw Win64 scalar function adds the two arguments and returns 42. At an
`awaiting_resource` event, add or correct the source, then send `supply` with the
event's exact token and explicit bindings, and finally send another `run`.
`module_add`/`module_revise` do no binary IO. An explicit `module_debug` may prepare
and validate its source/companion; already mapped instances retain their pinned
observation. These source operations do not resume a CPU or map an instance.

One `run` request executes on a CLI worker so stdin remains available for
`state` and `pause`. Responses may arrive out of order and must be matched by ID.
The CLI admits one pending run command; other concurrent operations follow the
SDK's busy rules. Wait for a run's event before sending dependent mutation,
resource-supply or resume commands. `quit` or EOF requests a cooperative pause of
an active run, joins the worker and releases both handles. No implicit resume or
retry occurs. A pipe that reaches EOF during a run therefore requests its pause.

Configuration and command lines use the public C input ceilings shared with
the engine in `ExecutionInputLimits.def`. Oversized/NUL-containing lines are
drained and rejected without dispatch; input/output records remain bounded.
Exit 0 denotes a successful transport session; determine the guest outcome from
`result.state`. A guest `failed` outcome remains a valid event. Startup/IO errors
exit 1; command errors remain recoverable in the session and produce exit 2 when
it ends. stdout contains JSON records and startup diagnostics go to stderr.

## Shared execution JSON protocol

The C++ [`ExecutionRunJSON`](../include/neverd/emulation/ExecutionRunJSON.h)
codec is implemented in `NeverDEmulationWorkload`. It is the common protocol
boundary used by the persistent C API and interactive CLI. Environment-specific
startup and binary/model policy remain in the selected factory.

`executionRunOptionsFromJSON` decodes control options. Bindings and environment
configuration are separate inputs; this decoder does not choose a loader,
address space or OS. Omitted controls use `ExecutionRunOptions` defaults.

```json
{
  "limits": {
    "instructions": "200000000",
    "events": "1000000",
    "timeout_microseconds": "10000000"
  },
  "time_policy": "active_work",
  "instruction_quantum": 100000,
  "pause_stages": ["after-entry"],
  "memory_transfer_limit": 1048576,
  "enable_resource_pauses": true,
  "pause_unknown_addresses": true
}
```

The decoder also accepts `actions_per_run` and `instance_limit`. Time policy
may be `active_work` or `absolute_deadline`; decoding it does not create or reset
a budget. Stage names must subsequently pass the selected environment's
inventory check. Binding arrays use `id`, `module_id`, `guest_name`, optional
`scope`/`address_space`, and optional `base`/`size`. The decoder returns owned
descriptions without touching a CPU, reading sources or creating address spaces.
`resolveExecutionBindings` uses an explicit stopped-environment lookup; an empty
space name asks that environment for its default. Unknown names and null lookup
results fail. The decoder never substitutes a host pointer or guesses a process.
`ModuleBindings` retains ownership of identity, extent, overlap and publication
admission.

`executionSupplyFromJSON` accepts exactly `token` and `bindings`. The token has
`owner`, `version`, and `sequence`; version zero is valid before the first
declaration, while owner/sequence must be nonzero. Decoding a token does not
validate its ownership or resume a CPU. The existing `ExecutionRun::supply`
path checks the live request and commits the resolved batch.

`guestInvocationFromJSON` requires an explicit entry and scalar ABI. Empty or
omitted `instance_id` selects an absolute guest address; a nonempty instance ID
makes `entry` an RVA. Scalar arguments use `value`; buffer pointers use `buffer`
and an optional `offset`. These representations cannot be mixed in one argument.

```json
{
  "instance_id": "helper-instance",
  "entry": "0x1234",
  "abi": "win64",
  "arguments": [{"buffer": 0}, {"value": 256}],
  "buffers": [{"size": 256, "input_hex": "0080ff01", "capture": true}],
  "limits": {"instructions": 1000000, "events": 10000, "timeout_microseconds": 1000000}
}
```

Supported explicit ABIs are `win64`, `sysv_amd64` and `aapcs64`; this codec adds
no OS adapter or application execution example. The engine still checks the
actual architecture, stage, private stack and environment capabilities before
installation. The codec bounds argument/buffer counts and the ABI-aligned
aggregate payload against the supplied transfer limit. It decodes only supplied
input bytes; zero fill and caller-state installation stay in `GuestInvocation`.

All inputs are at most 2 MiB, with bounded UTF-8 names and no NULs. Unknown or
duplicate object fields, unpaired Unicode surrogate escapes, fractional/exponent
numbers, negative values and uint64 overflow fail explicitly. Unsigned fields
accept JSON integers or decimal/`0x` strings. Strings are preferable for clients
whose JSON number representation loses integer precision. Binding extent and
runtime admission remain engine checks.

Event, snapshot and instance encoders return owned JSON with `schema_version: 1`.
Every uint64 value is a hexadecimal string, including token parts, counters and
addresses. Reports preserve state/reason/stage separately; CPU stop/deadline,
faults, service handoffs and validated resource restart facts stay distinct.
Absent optional facts are omitted. Address-space names require the environment's
explicit identifier callback. Invocation output is `bytes_hex`, with an empty
string for an uncaptured row; output does not invent a successful model or grant
execution permission. Transfer data is bounded at 64 MiB and encoded reports at
192 MiB. The shared ceilings in `ExecutionLimits.h` also govern the existing
engine admission checks, without changing their previous values.

## Persistent workload and environment

[`ExecutionRun`](../include/neverd/emulation/ExecutionRun.h) retains one CPU,
`ExecutionSession`, budget, module-instance table and
[`ExecutionEnvironment`](../include/neverd/emulation/ExecutionEnvironment.h).
The environment owns canonical OS objects, loader/scheduler continuations and
phase state. Its stopped `next` method returns a bounded action; the run alone
executes the CPU and then calls `afterCPUExit`. Service requests and modeled
guest exceptions remain the environment's responsibility. It must not replace
hooks, execute another CPU loop or acknowledge resource stops itself.

The run validates an environment's stage inventory and the caller's requested
`PauseStages` at creation. A `StageBoundary` pauses only when selected. The
inventory supports future application environments without requiring kernel
names, `DriverEntry`, Windows or a particular loader. Raw-buffer environments
exercise the generic layer. Future OS adapters can publish their stage inventory
through this same boundary; this change registers only the buffer environment.

`ExecutionRun::create` optionally adopts a shared budget with matching limits and
time policy. Existing instruction/event usage and active/deadline time are not
reset. `ExecutionEnvironment::modelHooks` supplies policy closures once before
initialization; the session remains the hook and accounting owner. Failed scene
actions may carry a validated `FailureReason` for guest/model/limit outcomes;
pause, resource or completion reasons cannot be published as failures, and a
nonfailed action cannot select a failure reason.

For a scene with model admission, the session decides instruction charging even
when the cumulative instruction credit is zero. This permits an already reached
uncharged model/return boundary to finish without granting another instruction.
Plain CPU scenes and finite invocation admission retain their existing limits.

The synchronous lifecycle is:

- `create` initializes the same environment once and returns `Ready`.
- `run` requires `Ready` and returns a structured `Paused`, `AwaitingResource`,
  `Completed` or `Failed` event. Finite control quanta also yield `Paused`.
- `resume` changes `Paused` to `Ready`; a subsequent `run` continues execution.
- `supply(token, bindings)` or a successful `resolve` changes
  `AwaitingResource` to `Ready`. Neither operation executes guest instructions.
- `Completed` and `Failed` reject resumption, supply and guest-memory mutation.

Before an ordinary CPU action, eager declarations are prepared and mapped.
Failed file preparation preserves that action as well as the resource request,
so correcting the input and supplying it does not repeat the environment's
already completed work. Source caches can advance independently; guest mapping
and metadata publication use the common module transaction.

During a supported CPU resource stop, the hook only checks environment policy
and a cached range inventory. Once the CPU exits, the run creates the versioned
request. A declared lazy instance can be prepared/supplied automatically through
the same path. An unknown address remains a candidate for caller input, without
guessing a filename. A hole inside a published module is rejected; the
environment must also decline known OS-object bounds/state failures.

Successful repair acknowledges the CPU stop and retains its mandatory
`ResumePC`. The next CPU action uses it before calling `next` again. This keeps
the actual partial instruction progress and avoids restoring an older scheduler
snapshot. Input errors and invalid tokens retain the request for retry;
unrecoverable environment/CPU failures and exhausted budgets terminate the run.
Instruction/event credits are never replenished, and a final successful
instruction may still reach a stage/completion without needing another credit.

`inspect`, bounded `readMemory` and bounded `writeMemory` operate at stopped
boundaries. Memory transfers use existing RAM permissions and do not dispatch
MMIO. Inspection remains available after termination. `state` and
`requestPause` are thread-safe while the owner lives; all other calls require
caller serialization and reject same-run callback reentry. Resolver callbacks
run outside the CPU and may register/correct caller-provided sources; source
addition alone never maps or resumes a running workload.

## Finite guest calls at a selected stage

`ExecutionRun::invoke(GuestInvocationRequest)` starts a call while paused at a
selected stage, or after a completed call at that same stage. It returns `Ready`
without executing instructions. `run`, resource supply, user pause and `resume`
drive the call through the retained session and environment. A second external
invoke while one is active is rejected; the environment still owns nested guest
callbacks required by modeled APIs.

The request specifies an explicit `IntegerCallingConvention`, an absolute guest
`Entry` or an `InstanceID` plus RVA, scalar arguments, bounded buffers and finite
instruction/event/time limits. Instance entries use that instance's address
space/base/extent. A declared lazy target can be supplied on its initial fetch
if the environment/CPU resource policies admit it. No file address or ABI is
guessed. `IntegerABI` supplies the existing non-variadic 64-bit scalar conventions;
floating-point/aggregate arguments, implicit extension and variadic calls are
unsupported. The environment also checks which conventions/targets it admits.

Each `GuestInvocationBuffer` has `Size`, initial `Input` bytes and a `Capture`
flag. Buffers occupy bounded, aligned payload storage in the environment's
private call stack; bytes beyond `Input` start at zero. A scalar argument has
`Value` and no `Buffer`; a buffer pointer has the buffer index in `Buffer` and
its byte offset in `Value`. Offsets through one-past and zero-sized buffers are
explicitly permitted. At most 64 arguments and 64 buffers are accepted, with
aggregate aligned payload bounded by the run's memory-transfer limit. Captured
output is copied into the result; returned buffer addresses describe guest RAM.

The environment's stopped `planInvocation` performs effect-free stage, target,
thread/IRQL/loader admission and supplies a private, already mapped RAM stack and
an executable return boundary. The stack must remain alive across every yield
and must not alias live caller storage. Generic preflight checks architecture,
layout, permissions, sizes, pointer offsets and obvious caller-stack overlap
before installing a frame. `beginInvocation` saves canonical caller/model state
and enters its own nested frame. `next` and `afterCPUExit` continue to handle
actual APIs, waits, exception transfers and callbacks using the same session.

`SessionContext` saves CPU registers and execution/write watches, including the
caller's watch step-over state. Hooks, RAM and budgets stay shared. A reserved
return watch remains present when the environment changes its ordinary watches;
it yields `InvocationReturn` before instruction admission and cannot be stepped
over. Pending resource/service/exception outcomes and terminal sessions reject
context capture/restoration. Live observers also reject context/control changes
and recursive session execution. Resource repair resumes the actual call
continuation before another environment action, retaining completed instruction effects.

At the reserved return, the run checks stack balance and captures outputs.
`finishInvocation` validates/restores canonical thread/model ownership while
leaving CPU/session restoration to the run. The run restores the caller context
and original stage and returns `Paused`/`InvocationComplete`, with a
`GuestInvocationResult` containing the scalar return, buffers and accounting.
Guest RAM side effects remain. Calling `resume` then `run` continues the parent
phase; another `invoke` can call again from the same stage.

Invocation limits cap the remaining shared allowance. Start/completion each
consume one event; admitted attempts, resource events, model events and supply
also remain cumulative. A local time cap is passed to each session run, and
planning, preparation and environment/resolver work count toward call time.
Planning errors cannot conceal exhausted time. Active-work policy excludes
stopped user waiting. A failure after installation, unbalanced return, failed
model restoration, exhausted limit or terminal guest fault terminates the owning
workload rather than restoring an earlier CPU snapshot and implying success.
Private call-stack/return-boundary failures are not missing-binary candidates.

The call engine is available through C++ and the persistent C API. OS adapters
can adopt its prepared frames through `ExecutionEnvironment`; API, callback and
scheduler policy stays with that adapter. Raw-buffer tests exercise the common
call boundary. Windows driver and Ring3 runtime validation belong to their
respective adapter changes.

## Verification

Build with `NEVERD_ENABLE_CPU_EMULATION=ON` and
`NEVERD_ENABLE_DRIVER_EMULATION=OFF` to exercise the independent CPU boundary.
`NeverDModuleTests` and `NeverDModuleCAPITests` cover file resources, PDB identity,
catalogue transactions and the pure C boundary. The fixture PE/PDB files are
compiled from repository source when Clang and lld-link are available.
`NeverDModuleBindingTests`, `NeverDResourcePauseTests`, `NeverDExecutionRunTests`,
`NeverDExecutionRunJSONTests` and `NeverDExecutionCAPITests` cover mapping rollback,
resumption, finite calls, state ownership and strict JSON admission.
`NeverDExecutionCAPIDisabledTests` and `NeverDExecutionSDKDisabledTests` cover
disabled-build diagnostics and exported ABI availability. The CLI tests use the
same public SDK. These checks do not establish an OS environment or Ring3 runtime.
