// SPDX-License-Identifier: MIT
// The Library page's protocol code (web/library/index.html, between its "protocol" markers) against the FM-1's, in the
// host simulator: a bfxr2 sound written into a slot over SysEx, the list read back and decoded, the .bfxr file it makes
// opened by bfxr2's library, the .wav and the .zip checked.
//   node tools/library_test.mjs BFXR2_DIR      (after ./build.sh --host)
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {execFileSync} from 'node:child_process';

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const bfxrDir = process.argv[2];
if (!bfxrDir) { console.error('usage: node tools/library_test.mjs BFXR2_DIR'); process.exit(2); }
const page = fs.readFileSync(path.join(root, 'web/library/index.html'), 'utf8');
const code = fs.readFileSync(path.join(root, 'web/library/synths.js'), 'utf8') +
  page.slice(page.indexOf('// --- protocol ---'), page.indexOf('// --- end protocol ---'));
const P = new Function(code + '; return {PARAMS, NSLOTS, CURRENT, encodeSound, decodeSound, listRequest, writeRequest, ' +
                       'clearRequest, parseFrame, toBfxr, fromBfxr, soundObject, wavBytes, zipBytes, slotName, ' +
                       'PRESET_SYNTHS};')();
globalThis.window = globalThis;
(0, eval)(fs.readFileSync(path.join(bfxrDir, 'lib', 'bfxrlib.js'), 'utf8'));
const bfxr = globalThis.bfxr;

let fails = 0;
const check = (ok, what) => { console.log(`${ok ? '  ok' : 'FAIL'} ${what}`); if (!ok) fails++; };
const f32 = v => Math.fround(v);
const hex = b => b.map(x => x.toString(16).padStart(2, '0')).join(' ');

// 1. a bfxr2 sound, as the page reads a .bfxr file
const preset = bfxr.preset('Bfxr', 'laser_shoot', 4242);
const file = JSON.stringify({synth_type: 'Bfxr', version: '1.0.4', file_name: 'laser', params: preset.params});
const sound = P.fromBfxr(file), values = sound.values;
check(sound.synth === 0 && values.length === 32, 'a .bfxr file read: Bfxr, 32 parameters');
check(P.decodeSound(P.encodeSound(values)).every((v, k) => v === f32(values[k])), 'encode / decode: the floats exactly');
let refused = false;
try { P.fromBfxr(JSON.stringify({synth_type: 'Nosuchr', params: {}})); } catch (e) { refused = true; }
check(refused, 'a synth the FM-1 lacks: refused');
// a preset synth's sound: the FM-1's first, Whooshr
const wname = P.PRESET_SYNTHS[0][0], wpre = bfxr.preset(wname, bfxr.presets(wname)[1], 77);
const wsound = P.fromBfxr(JSON.stringify({synth_type: wname, version: '1.0.0', file_name: 'w', params: wpre.params}));
check(wsound.synth === 1 && wsound.values.length === P.PRESET_SYNTHS[0][2].length, `a ${wname} file read: synth 1`);

// 2. the FM-1: write it into slot 4 (index 3), list, clear slot 4 again in a second run
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'bfmxr-lib-'));
const host = path.join(root, 'app/build/host/bfmxr_host');
const log = path.join(tmp, 'sysex.bin');
const script = ['wait 2000', `sysex_log ${log}`, `sysex ${hex(P.writeRequest(3, sound))}`, 'wait 50',
                `sysex ${hex(P.writeRequest(5, wsound))}`, 'wait 50',
                `sysex ${hex(P.listRequest())}`, 'wait 300', `sysex ${hex(P.clearRequest(3))}`, 'wait 50',
                `sysex ${hex(P.clearRequest(5))}`, 'wait 50',
                `sysex ${hex(P.listRequest())}`, 'wait 300', 'sysex_log off'].join('\n') + '\n';
fs.writeFileSync(path.join(tmp, 'script.txt'), script);
execFileSync(host, [path.join(tmp, 'script.txt'), tmp], {stdio: 'pipe'});
const raw = fs.readFileSync(log), frames = [];
for (let i = 0; i < raw.length; i++) if (raw[i] === 0xF0) {
  const j = raw.indexOf(0xF7, i);
  frames.push(Array.from(raw.subarray(i, j + 1)));
  i = j;
}
const parsed = frames.map(f => P.parseFrame(f)).filter(Boolean);
const replies = parsed.filter(f => f.type === 'reply'), ends = parsed.filter(f => f.type === 'end');
check(replies.length === 4 && replies.every(r => (r.slot === 3 || r.slot === 5) && r.status === 0),
      'writes and clears answered: done');
check(ends.length === 2 && ends[0].version === 2 && ends[0].nslots === 16 && ends[0].nsynths === P.PRESET_SYNTHS.length,
      `two lists, each ended (version 2, 16 slots, ${P.PRESET_SYNTHS.length} synths)`);
const firstList = parsed.slice(parsed.indexOf(replies[1]) + 1, parsed.indexOf(ends[0]));
const secondList = parsed.slice(parsed.indexOf(replies[3]) + 1, parsed.indexOf(ends[1]));
check(firstList.length === 17 && secondList.length === 17, 'each list: 16 slots and the sound being edited');
const s4 = firstList.find(f => f.slot === 3);
check(s4 && s4.used && s4.synth === 0 && s4.values.every((v, k) => v === f32(values[k])), 'slot 4 read back as written');
const s6 = firstList.find(f => f.slot === 5);
check(s6 && s6.used && s6.synth === 1 && wsound.values.every((v, k) => s6.values[k] === f32(v)) &&
      s6.values.slice(wsound.values.length).every(v => v === 0), `slot 6 read back as written: ${wname}`);
// (as the FM-1 holds it: floats)
const wf = {...wpre, params: Object.fromEntries(Object.entries(wpre.params).map(([k, v]) => [k, typeof v === 'number' ? f32(v) : v]))};
const wout = P.toBfxr(s6, 'w'), wa = bfxr.render({...wf, renderSeed: 0.5}), wb = bfxr.render(JSON.parse(wout));
let wd = 0;
for (let i = 0; i < Math.min(wa.length, wb.length); i++) wd = Math.max(wd, Math.abs(wa[i] - wb[i]));
check(JSON.parse(wout).synth_type === wname && Math.abs(wa.length - wb.length) <= 1 && wd < 0.02,
      `its .bfxr renders as on the FM-1 (largest difference ${wd.toFixed(4)})`);
// Transfxr: its transitions nested again in the file, and it renders as on the FM-1
{
  const at = P.PRESET_SYNTHS.findIndex(x => x[0] === 'Transfxr');
  if (at >= 0) {
    const tp = bfxr.preset('Transfxr', bfxr.presets('Transfxr')[3], 9);
    const fr = v => typeof v === 'number' ? f32(v) : v && typeof v.start === 'number' ? {...v, start: f32(v.start), end: f32(v.end)} : v;
    const tf = {...tp, params: Object.fromEntries(Object.entries(tp.params).map(([k, v]) => [k, fr(v)]))};
    const ts = P.fromBfxr(JSON.stringify({synth_type: 'Transfxr', params: tf.params}));
    const to = JSON.parse(P.toBfxr({synth: ts.synth, values: ts.values.map(f32)}, 't'));
    const ta = bfxr.render({...tf, renderSeed: 0.5}), tb = bfxr.render(to);
    let td = 0;
    for (let i = 0; i < Math.min(ta.length, tb.length); i++) td = Math.max(td, Math.abs(ta[i] - tb[i]));
    check(typeof to.params.pitch === 'object' && to.params.pitch.curve === tf.params.pitch.curve && td < 0.02,
          `a Transfxr file: its transitions back in shape, rendering as on the FM-1 (largest difference ${td.toFixed(4)})`);
  }
}
// Jinglr: its melody written into the file, as bfxr2 composes it
{
  const at = P.PRESET_SYNTHS.findIndex(x => x[0] === 'Jinglr');
  if (at >= 0) {
    const jp = bfxr.preset('Jinglr', bfxr.presets('Jinglr')[2], 5);
    const jf = {...jp, params: Object.fromEntries(Object.entries(jp.params).map(([k, v]) => [k, typeof v === 'number' ? f32(v) : v]))};
    const js = P.fromBfxr(JSON.stringify({synth_type: 'Jinglr', params: jf.params}));
    const jo = JSON.parse(P.toBfxr({synth: js.synth, values: js.values.map(f32)}, 'j'));
    check(jo.params.phrase === jp.params.phrase, 'a Jinglr file: its melody as bfxr2 composed it');
  }
}
check(secondList.find(f => f.slot === 3).used === false && secondList.find(f => f.slot === 5).used === false,
      'slots 4 and 6 empty after the clears');
check(firstList.find(f => f.slot === P.CURRENT).used === true, 'the sound being edited is in the list');

// 3. the files: the .bfxr opens in bfxr2's library and sounds as the original; the .wav and the .zip are whole
const out = P.toBfxr(s4, P.slotName(3));
const a = bfxr.render({...preset, renderSeed: 0.5}), b = bfxr.render(out);
let maxd = 0;
for (let i = 0; i < Math.min(a.length, b.length); i++) maxd = Math.max(maxd, Math.abs(a[i] - b[i]));
check(Math.abs(a.length - b.length) <= 64 && maxd < 0.02, `the .bfxr renders as the original (length ${b.length} vs ${a.length}, ` +
      `largest difference ${maxd.toFixed(4)})`);
check(JSON.parse(out).synth_type === 'Bfxr' && P.slotName(3) === 'bfmxr-slot-04', 'the .bfxr file: Bfxr, named for its slot');
const wav = P.wavBytes(b, 44100);
check(String.fromCharCode(...wav.slice(0, 4)) === 'RIFF' && wav.length === 44 + 2 * b.length, 'the .wav: RIFF, 16-bit mono');
const zip = P.zipBytes([['a.bfxr', new TextEncoder().encode(out)], ['a.wav', wav]]);
fs.writeFileSync(path.join(tmp, 'test.zip'), zip);
let zipOk = true;
try {
  const r = execFileSync('python3', ['-c', 'import sys,zipfile; z=zipfile.ZipFile(sys.argv[1]); assert z.testzip() is None; ' +
                                    'print(",".join(z.namelist()))', path.join(tmp, 'test.zip')]).toString().trim();
  zipOk = r === 'a.bfxr,a.wav';
} catch (e) { zipOk = false; }
check(zipOk, 'the .zip: whole, both files in it');

console.log(fails ? `${fails} failed` : 'library: all passed');
process.exit(fails ? 1 : 0);
