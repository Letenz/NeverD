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

Each directory keeps the existing recipe's relative filenames. The pkg tree
includes `outputs-canonical/` and `outputs-stripped/`.
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
These are local qualification receipts. The immutable archive still needs an
accessible URL and a successful workflow receipt before claiming remote release
qualification. Original manifests have not been relaxed or replaced.
