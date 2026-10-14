# Required full producer corpus qualification

Ordinary local Web tests can explicitly skip unavailable large producer images.
Release qualification uses `NEVERD_REQUIRE_WEB_PRODUCER_CORPUS=1`, which converts
missing/empty inputs and unavailable native/source consumers into test failures.
Malformed policy values also fail. The release target is available with Web,
the JS parser and shared SDK enabled:

```sh
cmake --build build-release --target check-neverd-web-producer-corpus
```

The same inventory has the `NeverDWebProducerCorpus` CTest label. In required
mode, disappearing test registrations fail discovery; `--no-tests=error` also
prevents an empty selection from passing. Eleven C++ cases retain their existing
independent original-container hashes and semantic/source expectations:

| Required input environment | Qualification |
| --- | --- |
| `NEVERD_BUN_142_CORPUS` | Five full Linux x64 containers; native loader handoff |
| `NEVERD_BUN_142_CROSS_CORPUS` | Twenty full cross-target containers |
| Both Bun inputs | Twelve source-map selections across six platforms |
| `NEVERD_NODE_SEA_22150_CORPUS` | Official Node-produced snapshot remains opaque |
| `NEVERD_NODE_SEA_22150_IMAGES` | Six official native images retain their injected blob |
| `NEVERD_NEXE_4RC7_CORPUS` | Three full images; original sources/bootstrap/manifest consumers |
| `NEVERD_PKG_581_CORPUS` | Six full images, preprocessing witness and shared source consumers |

The individual recipes, producer/source pins, hashes, licenses and expected
filenames remain under `unittests/web/fixtures/{bun,sea,nexe,pkg}`. Required
qualification never invokes Node, Bun, nexe, pkg, a sample or an external decoder.
Fixture production is a separate trusted development activity, not a test
dependency or fallback for absent corpus files.

`.github/workflows/web-producer-qualification.yml` is an explicit/reusable
release lane. It requires an HTTPS archive URL and exact SHA-256 as inputs;
there is no optional-download or absent-corpus success path. Its tar archive
contains these directories at its root:

```text
bun-linux/   bun-cross/   sea-cases/   sea-images/   nexe/   pkg/
```

The combined `.github/workflows/web-release-qualification.yml` calls this lane
and the fixed CEF resource qualification at the same selected repository ref.
Its final `Web release qualification gate` requires both called jobs to report
`success`; failures, cancellation and skips cannot produce a successful gate.
The input preflight must also succeed, and rejects missing/invalid URL, SHA-256
or witness selections before scheduling either expensive reader build.
It is available both by explicit dispatch and as a reusable release prerequisite.
A publishing workflow must depend on this gate before claiming complete Web
release qualification. Ordinary CI and unrelated package publication are not
represented as substitutes for the combined gate.

Dispatch the combined gate after supplying the immutable archive location:

```sh
gh workflow run web-release-qualification.yml --ref dev \
  -f corpus_url=HTTPS_ARCHIVE_URL -f corpus_sha256=ARCHIVE_SHA256 \
  -f snapshot_witness=2026-10-13
```

Each directory keeps the existing recipe's relative filenames. The pkg tree
includes `outputs-canonical/` and `outputs-stripped/`.
Private archive endpoints can require a bearer token. Explicit dispatches read
the repository Secret `NEVERD_WEB_PRODUCER_CORPUS_AUTHORIZATION`; reusable calls
can pass the optional Secret of the same name. The combined gate forwards
that Secret only to the producer reader. The token is supplied in an HTTP
authorization header, never as a URL or workflow input, and does not replace
archive or per-artifact hash checks. Without a token, public pinned archives
retain the existing download behavior. Remove temporary access tokens and stop
temporary serving endpoints after the qualification completes.
Interrupted downloads receive at most two retries, including partial-file and
HTTP/2 stream errors. Curl resets its explicit output file before retrying;
no archive is unpacked until the complete-file SHA-256 passes.

The optional explicit `snapshot_witness` input selects `original` by default
or the independently recorded `2026-10-13` witness. The latter requires
`sea-cases/snapshot-2026-10-13.blob` and its separate fixed manifest; repeat
generation is not assumed deterministic. Original snapshot metadata and byte
pins remain unchanged. An unknown witness or mismatched byte hash fails.

Preserve all applicable runtime/source redistribution licenses with the archive.
The lane verifies
the entire archive before admitting inputs, then the owning C++ tests check
each recorded artifact hash. It builds the pinned in-process Zstandard 1.5.7
SDK for Bun maps and requires the qualified portable namespace and JS parser.
The macOS ARM64 analysis host reads ELF/Mach-O/PE inputs without executing them.
This does not establish Windows analysis-host support.

On 2026-10-13 the absent `/tmp` corpus was rebuilt under a persistent local
directory. All 25 Bun containers, six injected SEA images, three nexe images,
six canonical pkg images and the pkg preprocessing image match their original
complete-file hashes. Node's opaque snapshot uses the separately pinned
`2026-10-13` witness. Both nexe/pkg installations retain new dependency locks;
their complete output hashes match the old witnesses despite those lock changes.
All eleven required C++ cases pass with zero skips (25.73 seconds, Release,
macOS ARM64). A missing-input probe fails all eleven cases with zero skips.
These local receipts are complemented by the
[complete combined GitHub qualification](https://github.com/NeverSight/NeverD/actions/runs/38502323427)
on 2026-10-14 at `8c1f4ac03875cec8711568b692bc0c76843b40ed`.
Its eleven producer cases and fifteen fixed CEF/resource cases pass with zero
skips; the input preflight and final combined gate also succeed. The supplied
archive has SHA-256
`7372a4fb66605e2d62405ee1e5ef55da1fbde99bed8cd61c1f5c6790c1d53ce3`
and uses the explicit `2026-10-13` snapshot witness. Original manifests have
not been relaxed or replaced. This receipt qualifies the selected commit and
fixed inputs on the macOS ARM64 reader. Future invocations must supply a
reachable pinned archive; the qualification does not establish permanent
public fixture hosting or additional analysis-host platforms.
