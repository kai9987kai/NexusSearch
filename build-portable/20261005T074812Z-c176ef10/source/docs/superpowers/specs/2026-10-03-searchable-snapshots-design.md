# Searchable snapshots

Continue the existing architecture by delivering an actual local search workflow:
JSON Lines -> validated typed snapshot -> NexusQL -> ranked JSON results. Preserve
the C11/no-runtime-dependency requirement, existing syntax, exact integers,
bounded input handling, source backups and differential testing.

The first durable searchable snapshot has immutable typed columns, original JSON,
unique UTF-8 IDs, integrity-checked bytes and signed-integer bit-sliced filters.
Strings use the existing explicitly versioned text profile. Numeric arrays are
finite float32 vectors with a fixed dimension per field. Missing/null values
never satisfy a comparison, including !=; NOT negates over the entire document
universe. Incompatible types fail ingestion rather than coerce silently.

Queries bind the entire syntax tree before evaluation, including branches which
would short-circuit. Unknown fields, unsupported clauses and mismatched types
produce explicit errors. Supported initial search includes Boolean composition,
typed comparisons/ranges/any-of/exists, text matching, regex/fuzzy matching,
BM25 text ranking, exact filtered vector scoring, deterministic sorting and
pagination. Explain reports the selected execution paths. The scan reference
and indexed numeric path must agree on IDs and scores.

The CLI exposes build, search, explain and stats. Building writes a complete
validated snapshot through the existing atomic file replacement API. This is
a single immutable snapshot; it does not claim a WAL, incremental merges,
multiwriter transactions, complete date semantics, federation or WATCH. Those
features remain in the architecture roadmap and unsupported syntax fails closed.

Research selects exact/reference implementations before ANN or pruning. BM25
uses corpus-wide field statistics before filtering. No universal ANN cutoff or
performance superiority is claimed. Bounds cap input, rows, fields, query work
and vector dimensions. Each new format validates offsets, ordering, counts,
versions and checksums before exposing borrowed views. Every allocation path
has failure handling; tests exercise failures, corruption and random oracles.

Alternatives considered: immediately building all servers/UI would leave the
engine unvalidated; adopting an external database would abandon the existing
embeddable C design. The chosen workflow creates a usable, testable engine step
which later applications can share.
