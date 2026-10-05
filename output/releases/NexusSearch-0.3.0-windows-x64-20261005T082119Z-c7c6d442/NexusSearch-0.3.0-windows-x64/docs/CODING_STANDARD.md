# NexusSearch coding standard

Binding for every module. The compiler flags in `tools/nxbuild.sh` and `CMakeLists.txt`
(`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`) are the first reviewer.

## Language & portability
* **C11**, no compiler extensions in portable code. No VLAs, no `alloca`, no statement-expressions.
  GNU/MSVC specifics only behind the macros in `src/core/nx_config.h`.
* Targets: Windows (MinGW-w64 UCRT / MSVC / clang), Linux, macOS; x86-64 and arm64. 64-bit only, little-endian only.
* **Never use `long`** (32-bit on Windows). Use `size_t`, `int64_t`, `uint64_t`, `uint32_t`... in anything sized or persisted.
  `printf`: `%zu` for `size_t`, `%lld`/`%llu` with a cast for 64-bit ints, never `%ld`.
* First include of **every** `.c`/`.h` is `"core/nx_config.h"` (directly or via another nx header), before any system header.
* Baseline ISA is x86-64-v2 (SSE4.2 + POPCNT). **AVX2/FMA/BMI2 code lives in functions marked `NX_TARGET("avx2,fma")`
  and is selected at runtime** through `core/nx_simd.h` – a scalar fallback must always exist and must produce identical
  results (or documented-epsilon-identical for float reductions). Never compile whole files with `-mavx2`.
* No global mutable state except explicit, documented, thread-safe singletons (`nx_mem` counters, `nx_simd` dispatch table).
* No `printf` to stdout from library code. Diagnostics go through return codes (`nx_status`, `nx_error`).

## Memory
* All heap goes through `nx_malloc/nx_calloc/nx_realloc/nx_free` (`core/nx_mem.h`), short-lived objects through `nx_arena`.
  This is how leaks and overruns are detected (no ASan on our toolchain).
* **Every allocation can fail.** Functions that allocate return `nx_status` (or NULL) and must leave no leaks on failure.
  Each module's tests include an OOM sweep (`nx_test_oom_sweep`) over its main build/query path.
* Ownership is stated in the header comment of every function that takes or returns a pointer
  (`borrowed`, `owned – caller frees with X`, `valid until Y`).
* Size arithmetic on anything derived from input/files uses `nx_mul_overflow/nx_add_overflow`.

## Untrusted bytes
Segment files, model/image/3D headers, JSON, HTTP and queries are **hostile input**.
* Parse with `nx_cursor` (bounds-checked, sticky error) – never raw pointer arithmetic from file-supplied offsets/lengths.
* Every offset/length/count read from a file is validated against the containing section before use; counts are
  checked against remaining bytes *before* allocating (`count * elem_size <= remaining`).
* Recursion depth and total work are bounded (configurable limits). A corrupt/malicious file must yield
  `NX_ERR_CORRUPT`, never a crash, hang or unbounded allocation.
* Every parser ships a mutation-fuzz test (flip/truncate/splice bytes of valid inputs; assert no crash, no leak, clean error).

## Concurrency
* Immutable-after-build structures (segments, indexes, plans) are freely shared across threads with no locks.
* Mutable shared state is protected by `nx_mutex`/`nx_rwlock`, documented at the struct (which lock guards what).
* Work is parallelised with `nx_pool` / `nx_group` (work-helping; safe to nest). Never spawn raw threads in library code
  except inside `nx_pool`, the HTTP server accept loop and the background merger.
* Lock order is documented in `docs/ARCHITECTURE.md` (store → snapshot → pool). No callbacks invoked while holding a lock.

## Style
* `snake_case`, prefix `nx_` for exported symbols, `nx_<module>_` per module; file-local helpers `static`.
* Public header per module in `src/<area>/nx_<module>.h` with a block comment describing purpose, invariants, thread-safety
  and ownership. Header guards `NX_<MODULE>_H`.
* 4-space indent, 110-column soft limit, K&R braces. Comments explain *why*; avoid restating code.
* Magic numbers get a named constant; on-disk constants (magics, versions, section ids) live in one header per format.
* Prefer small `static` functions and flat control flow over deep nesting. No dead code, no commented-out code.

## Testing (mandatory, part of "done")
Each module ships `tests/test_<module>.c` using `tests/nx_test.h`:
1. **Unit tests** for each public function incl. edge cases (empty, 1 element, max sizes, duplicates, unicode).
2. **Differential/property tests** against a trivially-correct oracle (brute force / `qsort` / naive set) on random data,
   many iterations (`nx_test_iters`), seeds reproducible via `NX_TEST_SEED`.
3. **OOM sweep** over build + main query path (`nx_test_oom_sweep`).
4. **Corruption fuzz** for anything that reads serialized bytes.
5. **Concurrency stress** for anything shared (N threads, invariant checks) where applicable.
6. Leak/canary checks happen automatically per test (`nx_test_run`).
A module is not done until `tools/nxbuild.sh` builds with zero warnings and all its tests pass.

## Performance
* Hot paths: no allocation per item, no `qsort` with function-pointer callbacks per comparison where a typed sort suffices,
  no division in inner loops, prefetch where access is predictable. Measure with `bench/` microbenchmarks
  (`nx_now_ns`, warm-up, report p50/p95) and record the numbers in the module's header comment or `docs/BENCHMARKS.md`.
* Do not optimise blind: write the straightforward correct version first, keep it as the oracle, then specialise.
