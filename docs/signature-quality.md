# Signature quality and performance

Signature growth is useful only when it adds correct identifications without
unacceptable false names, latency or memory cost. Record these separately;
pattern count and raw match count are not recognition rates. Keep stronger
existing packs when supplementing a library, and measure the supplement's
net contribution after selection, reference checks and ambiguity resolution.

## Strict identity and retained candidates

The default policy separates byte candidates from names permitted to drive
analysis. `matches()` and `buildCandidateNameMap()` retain byte, CRC, reference
and ambiguity results. `buildNameMap()` admits only exact spellings independently
stated by the image's function symbols/exports, plus the existing dedicated
exception-personality recognizer's metadata and whole-function checks. A symbol
for one alias does not certify a different alias. Analysis-generated names,
user labels, data/section symbols and a matching name at another address cannot
provide this evidence. Successful byte-pattern reloads/reapplication clear prior
decisions; failed loads leave the previous transaction intact.

This deliberately reduces automatic naming of stripped programs. An isolated
legacy byte pattern cannot recover erased C++ types or prove that the database
contains every function with those bytes. Even a completely confirmed callee
graph is not independent proof of the caller's original source identity.
Correct suggestions remain available for inspection; deferring a false name
is an abstention, not an increase in recognition coverage. The separately gated
library-feature recognizer retains its explicit identity verifier.

`neverd_sig_matches_json` retains `name`, `aliases` and `confirmed` as candidate
fields. Its additive `auto_name` field reports the address's admitted spelling,
or an empty string. `name_evidence` is `image_symbol`, `exception_personality`,
or `none`; a candidate without an admitted name reports
`candidate_reason=independent_identity_missing`. Every row at one address
reports the same decision, even if its own candidate disagrees. `confirmed`
alone never authorizes a name. The CLI marks candidates explicitly and the
worker and call graph no longer classify them as library functions or stop
traversal at them. C API match counts
continue to count candidates, not automatically named functions.

The probe evaluator reports automatic and preferred-candidate accuracy
separately. It reads `auto_name` directly for current engines, labels historical
responses as `legacy`, and rejects mixed or inconsistent admission responses.
Keep both correct and wrong candidate suggestions visible when reporting a
policy change. Map/truth files are evaluation inputs only, never supplied to
the tested `--no-debug` matching process.

For comparison, the [FLIRT design](https://docs.hex-rays.com/user-guide/signatures/flirt/ida-f.l.i.r.t.-technology-in-depth)
uses byte/CRC/reference discrimination and explicit collision handling, while
acknowledging future false positives. [Binary Ninja WARP](https://docs.binary.ninja/guide/warp.html)
separates automatic matches from possible functions for user selection; its
[matcher](https://github.com/Vector35/binaryninja-api/blob/dev/plugins/warp/src/matcher.rs)
accepts a unique nontrivial candidate and otherwise requires sufficient unique
constraint support. [Ghidra Function ID](https://github.com/NationalSecurityAgency/ghidra/blob/master/Ghidra/Features/FunctionID/src/main/doc/fid.xml)
uses scores, parent/child evidence and per-record mitigation, and documents the
false-positive/false-negative tradeoff. These are design references, not
comparative benchmark results. NeverD chooses the stronger identity boundary
because names also affect type reconstruction and semantic analysis.

## Acceptance evidence

| Dimension | Evidence required |
| --- | --- |
| Coverage | Correctly named library addresses divided by independently known library addresses; report results by platform, architecture, toolchain and optimization mode. Keep unknown and disputed addresses separate. |
| Accuracy | Compare final settled names and aliases with linker maps, retained symbols or independent compiler artifacts. Include unrelated programs, same-byte wrappers, changed constants, relocation targets and missing dependencies. Do not train exclusions on the evaluation set. |
| Time | Separate source reading, text parsing or validated cache mapping, index construction, matching, reference resolution and total analysis. Compare cold-process and repeated-use runs with the same worker count and input addresses. |
| Memory | Record process peak RSS and selected source bytes/modules. Include a large negative workload as well as positive matches. |
| Provenance | Retain engine/build identity, signature revision, input/truth hashes, selected pack hashes, options and every timing sample. State which stages and platforms were not measured. |

An added pack must not introduce a known false name. A matcher optimization
must retain its exact ordered matches unless changing recognition behavior is
the explicit, independently tested purpose. Existing false names must remain
visible as baseline defects; unchanged output is not proof of correctness.
Do not compensate for speed regressions by silently dropping uncertain target
profiles, weakening byte verification or choosing an arbitrary ambiguous name.

Use held-out programs and toolchain builds to evaluate new coverage. A probe
built from the same library used to generate patterns is a compatibility test,
not evidence of generalization to other builds. A fair comparison with another
tool needs the same stripped inputs and truth, documented function discovery,
enabled databases, options and analysis stages. An unconfigured database is
not a meaningful recognition-rate baseline.

## Matching work

The existing pipeline selects compatible packs before pattern I/O, builds a
sparse leading-byte index, verifies masks/CRC/tails, then checks references and
settles aliases. Parsed-file caching and one index per loaded database already
avoid repeated text parsing and per-segment index construction.

Packed batch matching now reuses a CRC for candidates at the same input address,
leading width and CRC length. Changing the input address or leading width
invalidates that reuse. Bounds and leading-byte checks run before a checksum
request; tail and reference checks retain their previous meaning. The cache
uses 544 bytes for values and validity bits plus bookkeeping per active entry,
independent of library size, with no heap allocation or cross-image state.
The value-form scanning APIs retain direct checksums because their external
callbacks can modify the input between candidates.

The optional `SignatureMatcher::ScanStatistics` overload reports entries
inside the supplied segment, candidate checks, CRC requests, actual CRC
computations and bytes checksummed. It resets on each call and sums worker
blocks deterministically. Ordinary callers retain the existing overload and
do not record counters. These counters measure work, not naming confidence.

`SignatureMatcherCRCReuse.*` checks every nonzero byte-sized CRC length,
wrong CRCs and tails, short inputs, nonstandard leading widths, modified
input buffers, mutating callbacks, value/packed forms and sequential/parallel
agreement. Its
growth case requires 1,024 candidates sharing a 63-byte CRC window to perform
one checksum over 63 bytes, while still rejecting all wrong CRCs.

## Reproduce measurements

Build Release targets `NeverDSignatureTests`, `neverd-signature-bench` and
`neverd` using the repository build workflow. Never use Debug timings.

```sh
build-release/bin/NeverDSignatureTests --gtest_filter='SignatureMatcher*'
build-release/bin/neverd-signature-bench program.exe --sig-base signatures \
  --entries function-vas.txt --threads 1 --repetitions 21 > matching.json
build-release/bin/neverd-signature-bench program.elf --sig-base signatures \
  --stride 16 --threads 1 --repetitions 21 > dense-scan.json
python signatures/scripts/evaluate_probes.py \
  --neverd build-release/bin/neverd --signatures signatures --auto \
  --report naming.json program-x64-probe.exe
```

`function-vas.txt` contains one hexadecimal VA per line. Entries are sorted
and deduplicated; a nonempty file is required. The stride alternative measures
dense candidate scanning, **not** discovered function coverage. The naming
evaluator requires the corresponding `.map` or `.truth.json`; use the normal
`neverd sigs --auto --json --no-debug` path for final naming accuracy.

The benchmark emits one JSON object with selected pack hashes, the loader's
input hash when available, a hash of the sorted entry VAs, raw ordered-hit
SHA-256, stage timings and work counters. `scan_cpu_ms` records process CPU
time, including worker threads; it is zero where the platform cannot report
CPU time. Address/module-index pairs and entry
VAs are hashed as little-endian 64-bit values. It discards one warmup scan and
records counters in a separate untimed scan whose results must agree. Peak
RSS is recorded on Unix/macOS and is null elsewhere. The benchmark parses text
without the persistent cache; it does not measure cache mapping, function
discovery, reference resolution or final names. Measure those separately with
the normal CLI/SDK. Peak RSS covers the whole benchmark process, including
source buffers and hashing, not just the matcher.

Retain each build's executable and JSON. Alternate old/new runs, repeat with
one worker and production worker counts, report medians and spread, and avoid
interpreting scheduler noise as a speedup. CPU affinity helps on machines
with mixed performance/efficiency cores. Compare hashes, module counts and
entry sets before comparing timing; when packs change, separately attribute
the change in work to added coverage. Use the deterministic unit-test work
bound in ordinary CI instead of a fragile universal millisecond threshold.

The [2026-10-24 measurement record](evidence/signature-matching-2026-10-24.json)
retains paired samples, corpus/truth hashes, build limitations and naming
baselines. Four MSVC probes reduced checksum computations by 58–84% and median
matching CPU time by about 8–26% with one/four workers in the final sweep.
Host load caused substantial timing variation during development, so these
local measurements do not guarantee a latency improvement for every input.
Text loading and reference resolution are not covered by those speedup numbers.

## Remaining recognition gaps

The current matcher does not provide a general instruction-normalized
function fingerprint database. Ghidra's pinned [function hasher](https://github.com/NationalSecurityAgency/ghidra/blob/164003036607e669226647bd0f7bedaf281e56a6/Ghidra/Features/FunctionID/src/main/java/ghidra/feature/fid/hash/MessageDigestFidHasher.java)
maintains differently normalized hashes, and its [program seeker](https://github.com/NationalSecurityAgency/ghidra/blob/164003036607e669226647bd0f7bedaf281e56a6/Ghidra/Features/FunctionID/src/main/java/ghidra/feature/fid/service/FidProgramSeeker.java)
reuses function hashes and considers caller/callee relationships. These are
research references, not copied code or evidence of comparative accuracy.
Any additional fingerprint stage in NeverD needs architecture-owned decoding,
explicit unsupported-input handling and independent collision validation;
a similarity hash alone must not establish a concrete function identity.
Prefer a bounded fallback for unresolved candidates that reuses existing
decoded instructions, rather than decoding every function again for each pack.

Exact bytes also do not prove a C++ template type or owning class when code
is shared or relocated data references have been masked. A local four-probe
MSVC MAP audit found 24 existing final-name discrepancies and library-address
coverage of about 62–71%. These are limited compatibility measurements, not
a broad product score. The CRC optimization preserves those results; it does
not repair identity ambiguity. Data-reference/type witnesses and held-out
negative examples are required before admitting more specific names.

Other unqualified areas include a common cross-tool held-out corpus, broad
kernel/mobile/legacy-library truth coverage, and persistent index startup
cost. The current file cache stores parsed data, not the decision index.
Investigate those costs and coverage gaps with the measurements above before
adding another database format or claiming a general recognition/performance
lead.

## Reference evidence withdrawal

Reference checking must withdraw evidence as well as discover it. A candidate
whose own calls contradict its identity cannot keep confirming callers through
its original byte hit. That invalidation is specific to the module and input
address: another viable version of the same named routine can still confirm a
reference. An ambiguous target's raw-pattern availability can change even when
its settled name stays absent.

Callers rejected using provisional evidence are reconsidered when that evidence
changes. Otherwise a short-lived name at one level can permanently discard a
correct alternative higher in the call graph. Checking still uses a worklist
of affected callers, rather than rescanning the database after every round.
Small proposal groups use inline storage. Changed addresses are sorted and
deduplicated in contiguous buffers, and a reusable queue with one byte per
match admits each dependent candidate at most once per round. Its length is
bounded by the match count, independent of how many references reach it.
An unchanged settled name does not requeue callers merely because an unused
raw pattern disappeared. Single-public-name hits also bypass temporary name
grouping allocations; multiple names retain the existing alias ordering and
intersection rules.
The bounds remain in `lib/sigs/ReferenceCheckLimits.def`. If propagation reaches
the round limit, all proposals at the unresolved addresses and their dependent
callers are suppressed; an unchecked alternative cannot win by elimination.
This conservative fallback runs only when work remains at the limit.

`SignatureDBReferences.*` covers withdrawn raw hits, recovery through multiple
call levels, a surviving same-name version, unresolved cycles with unrelated
functions, and 4,096 callers checked with one/four workers in different entry
orders. The synthetic regressions establish the evidence rules independently
of any particular library name or evaluation address.

The [reference-check measurement record](evidence/signature-references-2026-10-24.json)
compares eight MSVC compatibility probes. The VS2019 x86 debug probe gains
26 correctly named library functions (996 to 1,022 of 1,800); none of the eight
loses a correct name or acquires a new wrong name. Existing naming errors remain
visible. In the paired one/four-worker measurements, seven probes reduce median
CPU time for matching, reference checking and name settlement by about 0.8–4.2%.
The debug probe that gains names needs about 1.2–1.3 ms more CPU time in that
stage (about 8%). End-to-end wall timings varied substantially with host load;
the record retains that uncertainty and does not claim a universal speedup.
These results do not establish generalization to held-out programs or
comparative accuracy against another tool.

## Identity-admission measurements

The [2026-10-25 identity record](evidence/signature-identity-2026-10-25.json)
compares the strict policy with that earlier automatic-name policy on the same
eight stripped MSVC probes. All ordered byte/reference candidates remain equal.
The earlier 13,670 correct names and 66 wrong names remain preferred candidates;
none becomes an automatic name because these inputs provide no independent
identity evidence. Automatic coverage is therefore zero on this corpus, and
automatic precision is undefined. The change prevents those false names from
driving analysis; it does not recover more identities or repair the candidates.
Separate SDK cases retain exact export identity and the dedicated personality
recognizer, and reject data, analysis-generated and user labels as evidence for
unrelated signature names. A call-tree regression also verifies that candidates
cannot suppress traversal as library code.

Paired warm matching, reference checking and candidate settlement used the same
database, addresses and executable, with one/four workers. Median CPU changes
ranged from about -1.0% to +3.3%; both raw-match and candidate-name digests agreed
in every pair. Full-command measurements were less stable: a four-worker x86
CPU increase repeated, whereas the initial ARM64 increase did not. A separate
same-binary diagnostic localized the larger x86 increase to the unchanged
signature-load/discovery phase; name refresh became cheaper and matching did
not reproduce that increase. The record retains every trial, the extra JSON
cost and the phase controls. Concurrent builds prevent an end-to-end latency
guarantee; an idle-host qualification remains necessary. No pattern was removed
or verification weakened to obtain these timings.
