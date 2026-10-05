# Layer 2 and segment/query interface contract

This is the next implementation sequence, not an implemented engine API.

1. Schema and columns: explicit missing-value masks, exact int64 values, finite
   float64 values, typed bool/date/vector fields, analyzer profile/version in
   serialized metadata. `_id` is unique UTF-8 text. Duplicate keys, nonfinite
   vectors, dimension mismatch, and conflicting field types fail ingestion.
2. Term dictionary: deterministic sorted normalized byte terms, exact lookup
   and prefix enumeration with explicit expansion caps. Preserve original
   bytes separately for `=` semantics. Build flat bytes; open borrowed views.
3. Numeric bit-sliced indexes: signed int64 order transformed by sign-bit XOR;
   missing rows never satisfy comparisons. Differential oracle is a direct
   array scan using exact int64 comparisons, including INT64_MIN/MAX.
4. Lexical/trigram/vector indexes: full BM25 baseline before pruning; literal
   trigram candidates must always be verified against stored content; exact
   filtered vector search before HNSW or approximate quantization selection.

## Segments

An immutable segment owns either a mapping or one heap byte buffer. Every
section reader borrows a bounded slice of those same bytes. A section directory
must validate version, non-overlap, offset/length overflow and CRC before any
view is made available. Heap and mapped readers use the same open functions.
Document ordinals are dense uint32; external `_id` values are never ordinals.

Section types initially required: document IDs, original JSON, schema/analyzer
metadata, field values/presence, term dictionaries/postings, numeric indexes and
per-field statistics. Unknown required sections fail open. Optional newer
sections can be skipped only when their directory flags explicitly permit it.
Do not serialize C structs, raw pointers or native padding.

## Query execution and its oracle

The existing `nx_query_parse` produces `nx_stmt`. The next binder resolves
schema fields/operators and four-digit date/numeric ambiguity, units and dates,
fixing `now` once per request. Unsupported clauses produce TYPE/UNSUPPORTED;
they must never silently widen or narrow results.

Both the naive evaluator and indexed executor consume the same bound query
and immutable segment snapshot. The oracle scans typed columns/original text
without using indexes. The optimized path uses bitmap membership/set operations,
but identical filters, missing-value rules and sorting. Test exact survivor IDs,
not just counts. Rank comparisons use stable `_id` tie breaks and explicit
floating tolerances; scores need snapshot-wide N/df/average length.

`EXPLAIN` describes selected operators without executing. `EXPLAIN ANALYZE`
adds measured work/rows. Parsing WATCH/SOURCE syntax does not implement those
features: execution must reject them until their lifecycle exists.

Durability, WAL recovery, concurrent snapshot publication, merges, all ingestors,
servers/UI/bindings and a final adversarial review follow after this layer.
