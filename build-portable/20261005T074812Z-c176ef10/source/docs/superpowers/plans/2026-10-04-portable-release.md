# NexusSearch 0.3 portable release plan

Target: this Windows PC, portable local deployment. Preserve verified v0.2 backup.

- [ ] Prepared postings/trigram acceleration with exact scan parity and bounded memory.
- [ ] Filtered approximate-vector API with measured recall and exact fallback.
- [ ] Atomic batch updates with OS writer locks, rollback and interruption tests.
- [ ] Calendar/relative dates, numeric units, facets and bounded live watching.
- [ ] Live server refresh without exposing partially updated snapshots.
- [ ] Portable Windows package, launcher, dependency closure and relocated smoke tests.
- [ ] Compatibility checks available on this host plus repeatable external CI configuration.
- [ ] Fresh integration tests, browser QA, release manifest and external source backup.

Ruling: use a single coherently rebuilt immutable snapshot as the transactional
publication unit. This delivers bounded batch updates with correct global ranking
statistics; the older multi-file store remains a separate research implementation.
Ruling: production target is local Windows, as explicitly selected by the user.
No public service, cloud deployment, certificate or account setup is needed.
Ruling: no universal ANN recall or power-loss guarantees. Report measured recall,
exact fallbacks, filesystem assumptions and any unavailable platform validation.
