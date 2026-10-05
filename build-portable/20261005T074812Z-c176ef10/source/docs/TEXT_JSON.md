# Text and JSON profile 1

UTF-8 decoding rejects overlong encodings, surrogates, truncated sequences and
values above U+10FFFF. Embedded U+0000 is valid for these length-aware APIs.
NexusQL separately prohibits NUL bytes in query strings.

Normalization optionally converts U+FF01..U+FF5E to ASCII and U+3000 to space,
then folds ASCII A-Z to a-z. Other scalars are preserved. This is deliberately
not NFC, NFKC, Unicode case folding or accent folding. The existing Unicode
table generator is not used by this profile.

Tokens group ASCII letters, digits, underscore and non-ASCII scalars. Separators
include U+0080..U+009F, U+00A0, U+1680, U+180E, U+FEFF, U+2000..U+206F,
U+2E00..U+2E7F and U+3000..U+303F. ASCII punctuation is also a separator.
This simple policy can group emoji with adjacent letters and does not perform
language-specific segmentation. Tokens retain original byte spans and own
normalized copies in the supplied arena.

JSON follows RFC 8259 syntax with a stricter duplicate-key policy: decoded
duplicate object keys are rejected, including differently escaped spellings.
The parser keeps source order, source byte spans, and every decimal number's
original lexeme. Only integer lexemes within int64 are marked `exact_i64`;
fractions, exponents and larger integers retain lossless text. There is no
implicit conversion to binary floating point, so `1e400` remains representable.
Strings and keys can contain escaped NUL and supplementary Unicode scalars.

Limits cover input/output bytes, depth, nodes, cumulative decoded string bytes
and work. Parse failures restore the arena mark and clear the returned tree.
Serialization uses scratch output and appends only on success. Trees and slices
are immutable by contract. Serializer budgets stop cyclic caller-made trees;
lookup helpers require a valid parser-produced tree.

Primary syntax reference: [RFC 8259](https://www.rfc-editor.org/rfc/rfc8259).
