Per-module RVA exclusion lists (one hex RVA per line, # comments).
Applied automatically by translate.py in addition to DEFAULT_EXCLUDE.

hitmandlc.dlc / sound.dll: statically-linked CRT math region. Translating
it caused combat NaN/glitch symptoms (2026-07-03); kept original pending
domain-edge difftest exoneration of individual functions.
