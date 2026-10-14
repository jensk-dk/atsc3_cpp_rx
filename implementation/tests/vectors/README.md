# Test vectors

Hex/JSON vectors used by the unit tests: the standards' worked examples (A/322 pointer=130 →
`82 04`, A/331 Annex C TOI 0x80470003, …), bytes quoted in spec Appendix A/B, and references to
the schema example instances in `../../../specs/schemas` (never copied here). One file per vector,
named `<layer>-<what>.hex` or `.json`.

`mmt/` holds small binary vectors (real on-air MPU data units and ROUTE init segments, < 2 kB each) for the
`media` layer tests; `mmt/README.md` lists their provenance.
