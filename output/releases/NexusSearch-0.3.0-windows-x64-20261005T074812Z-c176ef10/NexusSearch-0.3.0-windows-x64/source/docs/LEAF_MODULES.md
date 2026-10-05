# Implemented leaf contracts

The headers are the API authority. Each module has a corresponding C test suite.
All result ownership and input bounds below apply before a higher-level index
or executor is added.

| Module | Contract and current boundary |
| --- | --- |
| `nx_file` | UTF-8 Windows paths; byte paths on POSIX. Mapping ownership is closed explicitly. Atomic replacement preserves existing mappings. Read/write errors are returned; a post-rename flush/close failure can mean the replacement already happened. Files must not be truncated in place while mapped. |
| `nx_crc32c` | Standard Castagnoli checksum, seedable streaming and checksum concatenation. Hardware/software paths are checked against a bitwise oracle. No performance guarantee. |
| `nx_utf8`, `nx_json` | See TEXT_JSON.md for analysis profile, binary strings, strict syntax, number retention and rollback. |
| `nx_bitmap` | Standard portable no-run Roaring cookie 12346. Arrays at cardinality <=4096, bitsets above it. Strict sorted keys/arrays, canonical offsets, bitset cardinality and exact section length validation; no allocation on open/read. Cookie 12347 with run containers returns UNSUPPORTED. Build accepts unordered/duplicate uint32 IDs. Set operations build fresh bytes transactionally. No run optimization or SIMD container algebra yet. |
| `nx_parse`, `nx_ast` | Schema-free NexusQL AST, including params, field-group expansion, vectors, date validation, modes, sorting/facets. Arena ownership; rollback on failure; explicit byte/list/depth/memory limits. Four-digit years are date scalars; numeric-only contexts force numeric parsing. Numeric fields will be resolved from raw lexemes by the binder. Printing/equality/dumps require valid parser-produced ASTs. |
| `nx_regex` | Boolean substring matching via bounded Thompson state simulation, independent of input backtracking complexity. Explicit matching work budget returns LIMIT rather than false. Byte-oriented dot/classes and ASCII insensitive mode. No captures, lookaround, backreferences, lazy quantifiers or Unicode property classes. Compile caps are in the header. |
| `nx_fuzzy` | Unicode scalar Levenshtein with a band <=64 edits, 16 KiB per input. Returns cap+1 beyond the cap. No transpositions or grapheme/case normalization. |
| `nx_vec` | Checked finite float32 inputs, double reductions, scalar oracle and runtime AVX2. Cosine zero norm returns 0. SQ8 quantizes independently per vector; it does not implement graph search or guaranteed ranking preservation. Buffers require normal C type alignment but no SIMD alignment. |

The parser's numeric conversion uses a C-runtime-owned locale object per parse,
freed on all exits, so decimal conversion never changes the process locale.
These locale objects are the exception to application heap accounting: their
internal libc allocations cannot be fault-injected by `nx_mem`.

Core repairs made during integration: physical rollback of oversized arena
chunks; checked arena chunk/total arithmetic; clearing empty reset's stale OOM
flag; C11 CRC table qualification; stable decimal boost round trips; equivalent
debug-allocator and sanitizer settings for static/shared CMake targets.

Primary references checked during this continuation:

- [Roaring portable format](https://github.com/RoaringBitmap/RoaringFormatSpec)
- [Thompson regex construction and simulation](https://swtch.com/~rsc/regexp/regexp1.html)
- [JSON syntax](https://www.rfc-editor.org/rfc/rfc8259)
- [Windows file rename information](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_rename_info)

These references support format/algorithm choices; they do not establish
NexusSearch's speed, durability under power loss, or full-platform correctness.
