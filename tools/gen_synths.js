#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// bfxr2's preset synths (everything but the classic Bfxr), for porting them to the FM-1:
//   node tools/gen_synths.js BFXR2_DIR OUT_DIR [SYNTH ...]
// For each synth: OUT_DIR/SYNTH/params.txt (its parameters in param_info order: name, default, min, max, and
// "select" for a choice), OUT_DIR/SYNTH/recipes.json (its recipes: id, name and the range of every parameter they
// set), and for every recipe and three seeds a case: case_NNNN.txt (the parameter values in params.txt's order, as floats like the FM-1's) and
// case_NNNN.f32 (bfxr2's output, mono float at 44.1 kHz).
'use strict';
const fs = require('fs');
const path = require('path');

const [, , bfxrDir, outDir, ...only] = process.argv;
if (!bfxrDir || !outDir) { console.error('usage: gen_synths.js BFXR2_DIR OUT_DIR [SYNTH ...]'); process.exit(2); }
globalThis.window = globalThis;
const src = fs.readFileSync(path.join(bfxrDir, 'lib', 'bfxrlib.js'), 'utf8');
const at = src.indexOf('\n', src.indexOf('const BFXR_SYNTHS = {'));   // (the library keeps its synths to itself)
if (at < 0) { console.error('bfxrlib.js: BFXR_SYNTHS not found'); process.exit(2); }
(0, eval)(src.slice(0, at) + '\nglobalThis.__BFXR_SYNTHS = BFXR_SYNTHS;' + src.slice(at));
const bfxr = globalThis.bfxr, SYNTHS = globalThis.__BFXR_SYNTHS;

for (const name of bfxr.synths()) {
  if (name === 'Bfxr' || (only.length && !only.includes(name))) continue;
  const engine = new SYNTHS[name]();
  const dir = path.join(outDir, name);
  fs.mkdirSync(dir, {recursive: true});
  const rows = engine.param_info.map(row => {
    if (Array.isArray(row)) {                    // a slider: [label, tip, key, default, min, max]
      const [label, tip, key, def, min, max] = row;
      if (Array.isArray(def) || typeof key !== 'string') return null;
      return {label, key, def, min, max, choices: null};
    }
    if (row && row.type === 'BUTTONSELECT')     // a choice: values [[label, tip, value], ...]
      return {label: row.display_name || row.name, key: row.name, def: row.default_value,
              min: Math.min(...row.values.map(v => v[2])), max: Math.max(...row.values.map(v => v[2])),
              choices: row.values.map(v => v[0]), values: row.values.map(v => v[2])};
    return null;
  }).filter(Boolean);
  fs.writeFileSync(path.join(dir, 'params.txt'), rows.map(r => `${r.key}\t${r.def}\t${r.min}\t${r.max}\t${r.label}` +
    (r.choices ? `\tselect:${r.choices.join('|')}\tvalues:${r.values.join(',')}` : '')).join('\n') + '\n');
  fs.writeFileSync(path.join(dir, 'recipes.json'), JSON.stringify(engine.recipes || [], null, 1));
  fs.writeFileSync(path.join(dir, 'version.txt'), String(engine.version || '') + '\n');   // (a .bfxr file's "version")
  let n = 0;
  for (const id of bfxr.presets(name)) {
    for (const seed of [11, 'b', 4242]) {
      const s = bfxr.preset(name, id, seed);
      for (const k of Object.keys(s.params))     // the FM-1 keeps its parameters as floats
        if (typeof s.params[k] === 'number') s.params[k] = Math.fround(s.params[k]);
      let pcm;
      try { pcm = bfxr.render(s); } catch (e) { console.error(name, id, e.message); continue; }
      const vals = rows.map(r => s.params[r.key]);
      if (vals.some(v => typeof v !== 'number')) { console.error(name, id, 'a parameter is not a number'); continue; }
      const cid = String(n++).padStart(4, '0');
      fs.writeFileSync(path.join(dir, `case_${cid}.txt`), vals.map(v => v.toPrecision(17)).join('\n') + '\n');
      fs.writeFileSync(path.join(dir, `case_${cid}.f32`), Buffer.from(new Float32Array(pcm).buffer));
    }
  }
  console.log(`${name}: ${rows.length} parameters, ${(engine.recipes || []).length} recipes, ${n} cases`);
}
