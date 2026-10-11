# Controlled pkg 5.8.1 corpus

The manifest preserves four self-authored, MIT fixture inputs and independent
digests for six complete producer outputs. Native runtimes, producer source,
bootstrap code and complete outputs are retained locally rather than committed.
This is an extraction oracle, not permission to execute a sample or a claim
about V8 cache equivalence, runtime activation or signature validity.

Producer creation is a separate development activity. Ordinary C++ tests and
NeverD analysis never invoke a script, producer, downloader or another analyzer.
The manual C++ `neverd-web-nexe-fixture-inputs` writer accepts this same fixed
four-file manifest; no new scripting implementation is needed.

Reproduction on the qualified macOS arm64 host:

1. Prepare a fresh private directory with subdirectories `producer` and `cache`.
   Use a canonical physical path, especially `/private/tmp` instead of the macOS
   `/tmp` symlink, to avoid extra producer symlink declarations.
2. In isolated setup, install exact `pkg@5.8.1` with npm lifecycle scripts
   disabled (`--ignore-scripts --no-audit --no-fund`). Preserve the resulting
   dependency lock and its hash. The qualified setup used Node 24.1.0 / npm 11.3.0
   and the lock hash recorded in `manifest.json`.
3. Download the six base URLs in the manifest into
   `cache/v3.4/fetched-v18.5.0-TARGET`. Verify every SHA-256 against both the
   manifest and pinned upstream `pkg-fetch/lib/expected.ts`. Never run a base.
4. Build the manual C++ writer, then pass this manifest and a new `inputs`
   directory. Preserve those original bytes before packaging.
5. From the canonical `inputs` directory, run the installed producer with
   `PKG_CACHE_PATH` set to the isolated cache and these options:

   ```text
   --no-bytecode --no-native-build --public --public-packages '*'
   --targets node18-linux-x64,node18-linux-arm64,node18-macos-x64,node18-macos-arm64,node18-win-x64,node18-win-arm64
   --out-path ../outputs-canonical package.json
   ```

   The producer may use the host signing utility while creating macOS output;
   it never needs to run a fixture with bytecode generation disabled. Signing
   metadata, host file modes, toolchain changes or altered virtual paths can
   change container hashes. Fresh outputs need new qualification; tests do not
   silently accept them as the pinned originals.
6. Keep full outputs under `outputs-canonical`, originals under `inputs`, and
   set `NEVERD_PKG_581_CORPUS` to their parent for the optional C++ qualification.
   The test checks pinned container hashes before interpreting members, then
   compares each captured member to its separate original bytes/hash. Omitted
   corpus inputs cause an explicit skip, not a producer download.

The original local evidence root is `/tmp/neverd-pkg-qualification-5.8.1`.
Its earlier `outputs` directory intentionally preserves a diagnostic corpus
with `/tmp -> /private/tmp` virtual symlink declarations; those are not the
canonical manifest artifacts. No fixture target was executed.

The supplementary `stripped-manifest.json` keeps BOM/shebang originals and
separate expected stored text/digests. Use the same C++ writer with that manifest
and a fresh `stripped-inputs` directory. From its canonical physical path, reuse
the pinned producer/cache and flags, selecting only `node18-linux-x64` with
`--output ../outputs-stripped/neverd-pkg-stripped-linux-x64`. The optional C++
test pins that full container, checks the independent original and stored
bytes/hashes, and verifies that the retained stat sizes refer to originals
(78 and 22 bytes) while selected source sizes refer to storage (56 and 19).
