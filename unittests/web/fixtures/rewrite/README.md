# Source rewrite compatibility receipts

These are actual raw local export receipts recorded before adding lexical
renaming, from local pre-rebase NeverD `15e92c7ff` on macOS arm64 Release
(the primitive implementation was published in `500c51062`). They contain only
analysis metadata from the self-authored `WebRewriteSDK` three-file fixture,
captured as one directory at revision 1 with script parse goal and artifact 1.
No target code was executed. Retain exact bytes; there is no final newline.

| Pass | Receipt SHA-256 |
|---|---|
| `string_recovery` | `ec2173260577fc3f4b8d8fb4602500c693fa91fd80e380dbd98878f39e2aa6f2` |
| `constant_folding` | `8e29fa15d36cece4f8cca5a7587cce4745f4689f52c4adcfe5663753e65e29fa` |

The SDK regression compares new export bytes to these originals; it separately
checks rewrite identity, record identities and the export authorization token.
Adding new pass evidence must not silently alter existing primitive receipts.

`scope-renaming-v1-receipt.json` was captured from published `8a1a303a0`, before
adding Boolean branch selection, using the exact self-authored `WebRenameSDK`
single-file fixture named `input.js`, script goal and revision 1. Its SHA-256
was `434de5241d88899e51247722e0595106718cdecb3b99821fab1c1253b199eabc`.
That `rewrite_steps` value counted comparator calls, which differ between C++
libraries (1352 with macOS libc++, 1386 with Linux libstdc++). Ordered searches
and sorts now charge deterministic comparison bounds, so the receipt was
re-exported from the same fixture: 1446 steps, SHA-256
`d2c8b50fdec7fa921b1d771bb8726d6aaa8945e24174b56a9a8752a324311e1b`, and every
other byte unchanged. The primitive receipts above were unaffected.
It likewise has no final newline; the SDK compares the complete raw export.
