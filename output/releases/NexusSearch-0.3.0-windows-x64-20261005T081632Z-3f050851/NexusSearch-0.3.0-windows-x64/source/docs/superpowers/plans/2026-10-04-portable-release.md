# NexusSearch 0.3 portable release plan

Target: this Windows PC, portable local deployment. Preserve verified v0.2 backup.

- [x] Prepared term/trigram search is integrated with exact scan parity, bounded memory, and a matched warm-query benchmark.
- [ ] Filtered approximate-vector API with measured recall and exact fallback. Experimental graph/quantization modules remain unpromoted.
- [~] Atomic batch updates use OS writer locks, final-table validation and atomic replacement; failure-injection rollback is tested, while hard process-interruption behavior remains unclaimed.
- [~] Calendar/relative date filtering is implemented for ISO-like text values. Field units, facets and bounded live watching remain roadmap items.
- [ ] Live server refresh after snapshot replacement. Restart remains the documented behavior.
- [ ] Portable Windows package, launcher, dependency closure and relocated smoke tests (final build pending).
- [~] Windows host checks plus a repeatable Linux/Windows CI workflow are configured; hosted jobs have not run.
- [~] Fresh integration/browser checks pass; final release manifest and external source backup remain pending.

Ruling: use a single coherently rebuilt immutable snapshot as the transactional
publication unit. This delivers bounded batch updates with correct global ranking
statistics; the older multi-file store remains a separate research implementation.
Ruling: production target is local Windows, as explicitly selected by the user.
No public service, cloud deployment, certificate or account setup is needed.
Ruling: no universal ANN recall or power-loss guarantees. Report measured recall,
exact fallbacks, filesystem assumptions and any unavailable platform validation.
