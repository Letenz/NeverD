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
includes `outputs-canonical/` and `outputs-stripped/`. Preserve all applicable
runtime/source redistribution licenses with the archive. The lane verifies
the entire archive before admitting inputs, then the owning C++ tests check
each recorded artifact hash. It builds the pinned in-process Zstandard 1.5.7
SDK for Bun maps and requires the qualified portable namespace and JS parser.
The macOS ARM64 analysis host reads ELF/Mach-O/PE inputs without executing them.
This does not establish Windows analysis-host support.

The old full-image directories recorded under `/tmp` are absent on the current
host. Preserved graph/blob fixtures continue to run, but they do not replace
full independent producer images. An immutable corpus archive still needs to
be restored/published before a successful full release qualification can be
claimed. An explicit absence probe selected all eleven cases and failed all
eleven, with zero successful skips. No corpus URL/hash or green release receipt
is invented.
