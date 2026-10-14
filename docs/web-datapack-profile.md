# Captured Chromium and WebView2 resource packs

Two explicit C++ readers share one complete range and alias validator:

| Profile | Header | Resource entry | Alias entry | Qualified input |
| --- | --- | --- | --- | --- |
| `chromium-datapack-v5-v1` | 12 bytes; version/encoding u32, two counts u16 | ID u16, offset u32 | ID/index u16 | CEF 120.2.7 Windows x64 NuGet distribution |
| `msedge-webview2-datapack-v5-u32-v1` | 16 bytes; version/encoding/counts u32 | ID/offset u32 | ID/index u32 | Teams 26261.303.5165.1465, bundled WebView2 154.0.4258.37, macOS |

All integers are little endian. Both measured formats mark version 5; the
caller must choose a profile. The standard profile follows the independently
studied [Chromium 131.0.6778.85 format implementation](https://github.com/chromium/chromium/blob/131.0.6778.85/ui/base/resource/data_pack.cc).
The wide profile records the supplied WebView2 sample's actual layout; it
does not assert a public Microsoft format contract or compatibility with every
Edge/WebView release. No upstream implementation or client resource is copied
into NeverD or its self-authored fixtures.

Admission checks version/encoding/padding, bounded table sizes, unique sorted
resource and alias IDs, alias indexes, monotonic in-bounds offsets, and a zero-ID
end sentinel. The payload begins immediately after the complete index and ends
exactly at the captured file's end. Truncation, table overlap, gaps at the index,
trailing bytes, unsupported versions and mismatched hashes refuse atomically.
Equal offsets permit empty resources. Aliases retain independent IDs and share
the same canonical physical byte slice and digest.

The pack is limited to 128 MiB and 65,536 resource/alias records. A revision
retains at most four packs and 65,536 records in total. Pages contain 1–512
records. Hashes, numeric IDs, lengths and offsets are published; content and
resource names are withheld. An encoding declaration does not validate every
resource's text: a pack can contain binary images, fonts or other data. The
pack establishes no framework identity, native signature, runtime version,
resource route, filesystem namespace or source language.

Stored resource selections feed the existing source/HTML/map/native consumers
only after explicit selection. `gzip-rfc1952-v1` separately admits one complete
gzip stream and checks its CRC/size trailer. It rejects concatenated streams,
trailing bytes, corruption and truncated input. Native zlib omission is reported
as unavailable. Limits are 16 MiB stored input, 8 MiB output, 2 MiB decoder
allocation, 8,192 calls and a two-second deadline. A revision retains at most
16 decoded selections and 64 MiB of decoded bytes. Failed output is discarded.
A gzip magic hint is only a candidate; other encodings remain original opaque
bytes. Whole-pack compression, v4 packs and additional resource codecs are
outside these profiles.

Original resources retain their exact container offsets and nested storage
origins. Decoded resources retain a separate hash/identity, expanded coordinates
and the complete stored-resource origin. Source anchors never map decoded bytes
directly into a compressed frame. Neither operation fetches resources, loads an
application or executes source.

The SDK exposes `datapack_extract`, `datapack_records`, `datapack_decode` and
explicit local `datapack_export`. Worker/MCP expose only the first three. CLI
examples, with an explicitly selected resource index:

```sh
neverd web pak resources.pak chromium-datapack-v5-v1
neverd web pak-source resources.pak chromium-datapack-v5-v1 0 5 gzip module
neverd web pak-html resources.pak chromium-datapack-v5-v1 0 4 gzip
neverd web pak-export resources.pak chromium-datapack-v5-v1 0 NEW_DIRECTORY
neverd web archive-pak PACKAGE.nupkg chromium-datapack-v5-v1 zip 0 12
```

`archive-pak-source`, `archive-pak-html` and `archive-pak-export` select a member
from an explicitly captured ZIP/tar/tgz before the same operations. Source
selection uses the existing explicit large-source parser profile. Numeric
indexes are not resource IDs or guessed filenames. Local export writes fixed
`resource-N.bin` names; aliases reference the canonical file. Already selected
decoded bytes use separate `decoded-N.bin` names. Every file is reread against
its exact SHA-256 before atomic completion-manifest publication. Existing
destinations are refused. Export admits at most 32,768 canonical resources;
failures may retain an incomplete private directory with no completion manifest.

The [supplied descriptor](web-desktop-resource-profile.md) remains an independent
caller index. It can identify an exact `.pak` artifact for explicit extraction;
it does not discover or authenticate a runtime. These measured resource readers
extend actual package extraction beyond that descriptor.

## Fixed real inputs

The CEF input is the [maintained redistributable package](https://www.nuget.org/packages/cef.redist.x64/120.2.7),
downloaded from its fixed NuGet flat-container URL. Its README declares CEF
`120.2.7+g4bc6a59+chromium-120.0.6099.234`; this declaration and the distribution
hash qualify the observed bytes, without authenticating a live runtime.

| Input | Bytes | SHA-256 |
| --- | --- | --- |
| CEF NuGet original | 128,957,697 | `e7f2e0b6860d8cce3ab66d7cb50492b9e1ea601172c850e5c7595d2a45baff17` |
| CEF `resources.pak` | 8,187,343 | `5a18a11950d57afda243b033083fb83aac3ed6785bc85888a794d4f9da874543` |
| Local Teams WebView2 `resources.pak` | 51,851,676 | `b3b4fb23fa809ac33895d6f0a87d03fdb6a2f087d6a15824d39faf48644c7b8d` |

CEF contains 2,504 canonical resources and 16 aliases. Its resource 698 starts
at pack offset 24,660, stores 23,108 gzip bytes and decodes to 102,823 bytes with
SHA-256 `f017c5aec879c43e4b7753dbddbccbbaace776a39ac527f4ebb7b913ef41b703`.
The existing module parser admits 11,830 nodes. Resource 693 decodes to a
976-byte HTML input with one script candidate. Nested ZIP origins retain the
NuGet's original deflate frame separately from the resource's gzip stream.

The measured WebView2 pack contains 5,814 canonical resources and 633 aliases.
Resource 24,521 starts at offset 621,511 and contains 535 original JS bytes
with SHA-256 `f0a4be163e13efddbfc172e6d1e433f5ccce9385a5b3fdd3771b1133ea144d5f`;
the existing script parser admits 56 nodes and exact original byte anchors.
The installed app and its resources remain local and are not redistributed.

`WebDataPackReal.*` pins these originals, resources and consumer witnesses in
C++. The CEF CI lane downloads and checks its immutable original and requires
the real qualification case; local ordinary runs can explicitly omit these
large inputs. The [testing guide](testing.md) records commands and dependencies.
