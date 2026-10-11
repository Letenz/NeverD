# Primitive rewrite compatibility receipts

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
