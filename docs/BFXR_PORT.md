# BFXR engine: porting bfxr2 to the FM-1

Goal: all 23 bfxr2 synths (https://github.com/increpare/bfxr2) in one Felucca engine, `BFXR`, with a SYNTH
selector. Each synth is rewritten from JavaScript into fixed-point C99 (the engines allow no floats, no
allocation and no library calls) and checked on a computer against the JavaScript output.

## Size budget (stock build, GitHub Actions run 42450725077)

| Resource | Used | Limit | Left |
| --- | --- | --- | --- |
| Flash: app image | 517,616 B | 647,100 B (`XIP` in `firmware/app.ld`) | ~126 KB |
| RAM: `.data` + `.bss` | 62,324 B | 73,728 B | ~11 KB |
| RAM: pool (engine memory) | 326,236 B | 368,640 B | ~42 KB |

The BFXR engine, its tables and all presets must fit in the flash left. Part state lives in
`ENG_MEM_PART` (12,888 B per part, shared by every engine); growing it costs RAM times the number of parts.

## Design

- One `engine_t` named BFXR. Eight parameters per synth through the `desc` hook (mode-dependent labels),
  the first one being SYNTH.
- Sound effects have a fixed length, so the engine owns its envelope (`ownenv` + `done`), as FM6 does.
- Port order: classic Bfxr first, then the other 22. Measure flash after each batch; trim presets before
  dropping a synth.
