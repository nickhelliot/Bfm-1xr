#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The preset synths' parameters and recipes, as C tables (app/synth_tables.h) and for the Library page
(web/library/synths.js), from tools/gen_synths.js's output.

  tools/gen_synth_tables.py SREF_DIR app/synth_tables.h web/library/synths.js SYNTH ...

Only the synths named are written (those ported to the FM-1), in that order. For each: its parameters (key, label for
the screen, default, range, choices, whether bfxr2 rounds it to a whole number) and its recipes (each parameter a
recipe sets: a range, a fixed value, or a list of choices to pick from), as bfxr2's PresetSynth.generate_recipe uses
them. Each synth's parameters get an enum, SYNTH_key, for its renderer."""
import json
import sys
from pathlib import Path

RANDOMIZE = {"Mixr": 1, "Birdr": 1, "Boomr": 1, "Fractr": 1, "Crittr": 2, "Transfxr": 3}   # bfxr2's own randomize: a random recipe (2: and
                                                                 # Crittr's cap on the length); else every slider at random
ROUNDED = {"Breathr": ["cycles"], "Pluckr": ["strings"], "Rustlr": ["folds"], "Zappr": ["arcs"], "Birdr": ["syllables"],
           "Choirr": ["voices"], "Crittr": ["calls"], "Fractr": ["fragments"], "Signlr": ["packets"],
           "Swarmr": ["count"], "Jinglr": ["noteCount", "octave", "instrumentSeed"]}
LABEL_MAX = 15


def cnum(v):
    s = repr(float(v))
    return (s if ("e" in s or "." in s) else s + ".0") + "f"


def screen(text):
    """a choice's name for the screen: upper case, in the font's ASCII"""
    t = text.upper().replace("\u266f", "#").replace("\u266d", "B").replace('"', "'")
    if any(ord(c) > 126 for c in t):
        raise SystemExit(f"{text!r}: not ASCII")
    return t


def label(text):
    t = screen(text).replace("SOUND VOLUME", "VOLUME")
    return t if len(t) <= LABEL_MAX else t[:LABEL_MAX].rstrip()


def transfxr_tables(sref, h, keys, rows):
    """Transfxr's buttons are bfxr2's preset families (PresetFamily.sample): each exemplar's values in the FM-1's
    order (16 bits: the range's fraction; a choice its value + 1), each family's variation, limits and pitch interval.
    Then TIMBRAL MORPH. Returns the recipes, with no values (synths.c makes them)."""
    data = json.loads((sref / "Transfxr" / "families.json").read_text())
    curves, fams = data["curves"], data["families"]
    def enc(k, v):
        r = rows[k]
        if r["choices"]:
            return int(v) + 1
        return max(0, min(65535, round((float(v) - r["lo"]) / (r["hi"] - r["lo"]) * 65535)))
    def flat(ex):
        out = []
        for k in keys:
            base = k.rsplit("_", 1)
            if k.endswith(("_start", "_end", "_curve")) and base[0] in ex and isinstance(ex[base[0]], dict):
                v = ex[base[0]][base[1]]
                out.append(enc(keys.index(k), curves.index(v) if base[1] == "curve" else v))
            else:
                out.append(enc(keys.index(k), ex.get(k, rows[keys.index(k)]["d"])))
        return out
    allex, info, lim = [], [], []
    for f in fams:
        first = len(allex) // len(keys)
        for ex in f["exemplars"]:
            allex += flat(ex)
        var = [f.get("variation", {}).get(n, d) for n, d in
               [("pitch", 0.07), ("tone", 0.1), ("vibrato", 0.1), ("level", 0.1), ("morph", 0.06)]]
        l0 = len(lim)
        for path, v in f.get("limits", {}).items():
            k = keys.index(path.replace(".", "_"))
            if isinstance(v, list):
                lim.append(f"{{{k}, 0, {cnum(v[0])}, {cnum(v[1])}}}")
            else:
                vv = curves.index(v) if isinstance(v, str) else v
                lim.append(f"{{{k}, 1, {cnum(vv)}, 0}}")
        iv = f.get("intervals", {}).get("pitch")
        info.append(f'    {{{first}, {len(f["exemplars"])}, {{{", ".join(cnum(x) for x in var)}}}, {l0}, {len(lim) - l0}, '
                    f'{1 if iv else 0}, {cnum(iv[0] if iv else 0)}, {cnum(iv[1] if iv else 0)}}},')
        if set(f.get("intervals", {})) - {"pitch"}:
            raise SystemExit("Transfxr: an interval other than pitch's")
    h.append(f"#define TX_NFAM {len(fams)}u")
    h.append(f"static const uint16_t TX_EX[{len(allex)}] = {{" + ",".join(str(x) for x in allex) + "};")
    h.append("typedef struct { uint8_t param, fixed; float a, b; } tx_limit_t;")
    h.append(f"static const tx_limit_t TX_LIM[{max(1, len(lim))}] = {{" + ", ".join(lim or ["{0, 0, 0, 0}"]) + "};")
    h.append("typedef struct { uint16_t first, n; float var[5]; uint16_t lim0, nlim; uint8_t has_iv; float iv0, iv1; } tx_family_t;")
    h.append(f"static const tx_family_t TX_FAM[{len(fams)}] = {{")
    h += info
    h.append("};")
    return [{"name": f["name"], "values": {}, "id": f["id"]} for f in fams] + \
           [{"name": "Timbral Morph", "values": {}, "id": "morph"}]


def footsteppr_tables(sref, h):
    """Footsteppr's terrains: bfxr2's Pure Data patches as its library compiles them (straight lines of pd_* calls on
    whole buffers), as a list of operations per terrain that synths.c runs a sample at a time. An operand is a node
    (0 ..) or a constant (-1 - its index); node 0 is the patch's output."""
    import re
    pd = json.loads((sref / "Footsteppr" / "pd.json").read_text())
    terrains = ["snow", "grass", "dirt", "gravel", "wood"]
    codes = {"pd_noise": "FP_NOISE", "pd_mul": "FP_MUL", "pd_div": "FP_DIV", "pd_add": "FP_ADD", "pd_clip": "FP_CLIP",
             "pd_osc": "FP_OSC", "pd_lop": "FP_LOP", "pd_hip": "FP_HIP", "pd_vcf": "FP_VCF", "pd_bp": "FP_BP",
             "pd_sqrt": "FP_SQRT"}
    consts, ops, starts, nodes = [], [], [], 0
    def const(v):
        v = float(v)
        if v not in consts:
            consts.append(v)
        return -1 - consts.index(v)
    for t in terrains:
        starts.append(len(ops))
        body = pd[t]
        top = 0
        temp = 200                               # (polyadd's sums: nodes past the patch's own)
        for m in re.finditer(r"const s_(\d+) = (.*?);\n", body):
            dst, expr = int(m.group(1)), m.group(2).strip()
            top = max(top, dst)
            if expr == "envelope_signal":
                ops.append(("FP_ENV", dst, 0, 0, 0))
                continue
            if re.fullmatch(r"s_\d+", expr):
                ops.append(("FP_COPY", dst, int(expr[2:]), 0, 0))
                continue
            fm = re.fullmatch(r"(pd_\w+)\((.*)\)", expr)
            fn, args = fm.group(1), fm.group(2)
            parts, depth, cur = [], 0, ""
            for ch in args:
                if ch == "," and depth == 0:
                    parts.append(cur.strip()); cur = ""; continue
                depth += ch == "("; depth -= ch == ")"; cur += ch
            if cur.strip():
                parts.append(cur.strip())
            opnd = []
            for a in parts:
                if a.startswith("s_"):
                    opnd.append(int(a[2:]))
                elif a.startswith("pd_c("):
                    opnd.append(const(a[5:-1]))
                elif a.startswith("pd_polyadd("):
                    srcs = [int(x.strip()[2:]) for x in a[11:-1].split(",")]
                    ops.append(("FP_COPY", temp, srcs[0], 0, 0))
                    for x in srcs[1:]:
                        ops.append(("FP_ADD", temp, temp, x, 0))
                    opnd.append(temp); temp += 1
                elif a == "NULL":
                    opnd.append(const(0))
                else:
                    raise SystemExit(f"Footsteppr {t}: {a}")
            opnd += [0] * (3 - len(opnd))
            ops.append((codes[fn], dst, *opnd[:3]))
        nodes = max(nodes, temp)
    starts.append(len(ops))
    h.append("enum { FP_ENV, FP_COPY, FP_NOISE, FP_MUL, FP_DIV, FP_ADD, FP_CLIP, FP_OSC, FP_LOP, FP_HIP, FP_VCF, FP_BP, FP_SQRT };")
    h.append("typedef struct { uint8_t op; int16_t dst, a, b, c; } fp_op_t;")
    h.append(f"#define FP_NODES {nodes}u")
    h.append(f"#define FP_NOPS {len(ops)}u")
    h.append(f"static const float FP_CONST[{len(consts)}] = {{" + ", ".join(cnum(c) for c in consts) + "};")
    h.append(f"static const fp_op_t FP_OPS[{len(ops)}] = {{" +
             ", ".join(f"{{{o}, {d}, {a}, {b}, {c}}}" for o, d, a, b, c in ops) + "};")
    h.append("static const uint16_t FP_START[6] = {" + ", ".join(str(x) for x in starts) + "};")


def mixr_rows(names):
    src = ["Bfxr"] + [n for n in names if n != "Mixr"]
    row = lambda key, d, lo, hi, lab, choices=None, rnd=False: dict(key=key, d=d, lo=lo, hi=hi, label=lab,
                                                                   choices=choices, values=None, round=rnd)
    rows = [row("masterVolume", 0.5, 0, 1, "VOLUME"), row("balance", 0.5, 0, 1, "BALANCE")]
    for x, syn, seed in (("A", "Machinr", 0.5), ("B", "Birdr", 0.25)):
        rows += [row("synth" + x, src.index(syn), 0, len(src) - 1, "SOUND " + x, src),
                 row("kind" + x, 0, 0, 30, "KIND " + x, None, True),      # 0: any of its recipes (app.c names them)
                 row("seed" + x, seed, 0, 1, "VARIATION " + x),
                 row("mut" + x, 0, 0, 20, "MUTATIONS " + x, None, True)]
    return rows


def main():
    sref, out_h, out_js, names = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4:]
    h = ["/* Generated by tools/gen_synth_tables.py from bfxr2's synths (MIT, Stephen Lavelle): do not edit.",
         " * Included by synths.c alone (it defines SYN_INFO and SYN_N). */",
         "#pragma once", '#include "synths.h"', ""]
    infos, js = [], {}
    for name in names:
        rows = []
        for line in (sref / name / "params.txt").read_text().splitlines():
            f = line.split("\t")
            key, d, lo, hi, lab = f[0], float(f[1]), float(f[2]), float(f[3]), f[4]
            choices = f[5][len("select:"):].split("|") if len(f) > 5 else None
            values = [int(v) for v in f[6][len("values:"):].split(",")] if len(f) > 6 else None
            if values and values == list(range(values[0], values[0] + len(values))) and values[0] == lo:
                values = None                    # (0, 1, 2 ...: the usual, no list)
            rows.append(dict(key=key, d=d, lo=lo, hi=hi, label=label(lab), choices=choices, values=values,
                             round=key in ROUNDED.get(name, [])))
        if name == "Mixr":                       # two sounds of the other synths, each as a recipe of it, a variation and
            rows = mixr_rows(names)              # mutations (bfxr2 keeps each one's parameters: too many for a slot)
        keys = [r["key"] for r in rows]
        if keys[0] != "masterVolume" or len(keys) > 24:
            raise SystemExit(f"{name}: the volume first, 24 parameters at most (app.c, synths.h)")
        up = name.upper()
        enum = f"enum {{ " + ", ".join(f"{up}_{k}" for k in keys) + f", {up}_NP }};"
        if name == "Mixr":                       # (in synth_ids.h, for app.c too)
            ids = ["/* Generated by tools/gen_synth_tables.py: do not edit. Mixr's parameters, for app.c and synths.c. */",
                   "#pragma once", enum, f"#define MIXR_ID {names.index('Mixr') + 1}u",
                   "#define MIXR_X_A MIXR_synthA      /* each of its sounds: synth, kind, variation, mutations */",
                   "#define MIXR_X_B MIXR_synthB"]
            (out_h.parent / "synth_ids.h").write_text("\n".join(ids) + "\n")
            enum = '#include "synth_ids.h"'
        h.append(enum)
        for r in rows:
            if r["values"]:
                h.append(f"static const int8_t {up}_V_{r['key']}[{len(r['values'])}] = {{" +
                         ", ".join(str(v) for v in r["values"]) + "};")
        h.append(f"static const syn_param_t {up}_P[{len(rows)}] = {{")
        for r in rows:
            ch = '"' + "|".join(screen(c) for c in r["choices"]) + '"' if r["choices"] else "0"
            vals = f"{up}_V_{r['key']}, {len(r['values'])}" if r["values"] else "0, 0"
            h.append(f'    {{"{r["key"]}", "{r["label"]}", {cnum(r["d"])}, {cnum(r["lo"])}, {cnum(r["hi"])}, {ch}, '
                     f'{1 if r["round"] else 0}, {vals}}},')
        h.append("};")
        recipes = json.loads((sref / name / "recipes.json").read_text())
        if name == "Mixr":                       # (a recipe: its pair of synths, then a balance)
            src = ["Bfxr"] + [n for n in names if n != "Mixr"]
            recipes = [dict(r, values={"synthA": src.index(r["pair"][0]), "synthB": src.index(r["pair"][1]),
                                       **r["values"]}) for r in recipes]
        if name == "Transfxr":
            recipes = transfxr_tables(sref, h, keys, rows)
        rnames = []
        for ri, rec in enumerate(recipes):
            vals = []
            for k, v in rec["values"].items():
                if k not in keys:
                    raise SystemExit(f"{name} {rec['id']}: {k} is not a parameter")
                i = keys.index(k)
                if isinstance(v, list) and rows[i]["choices"] is not None:
                    lst = ", ".join(cnum(x) for x in v)
                    h.append(f"static const float {up}_R{ri}_{k}[{len(v)}] = {{{lst}}};")
                    vals.append(f"{{{up}_{k}, SYN_RV_PICK, {len(v)}, 0, 0, {up}_R{ri}_{k}}}")
                elif isinstance(v, list):
                    vals.append(f"{{{up}_{k}, SYN_RV_RANGE, 0, {cnum(v[0])}, {cnum(v[1])}, 0}}")
                else:
                    vals.append(f"{{{up}_{k}, SYN_RV_FIXED, 0, {cnum(v)}, 0, 0}}")
            h.append(f"static const syn_rv_t {up}_R{ri}[{max(1, len(vals))}] = {{" + ", ".join(vals or ["{0}"]) + "};")
            rnames.append((rec["name"].upper(), f"{up}_R{ri}", len(vals)))
        h.append(f"static const syn_recipe_t {up}_RECIPES[{max(1, len(rnames))}] = {{")
        for nm, arr, n in rnames:
            h.append(f'    {{"{nm}", {arr}, {n}}},')
        if not rnames:
            h.append('    {"", 0, 0},')
        h.append("};")
        if name == "Footsteppr":
            footsteppr_tables(sref, h)
        h.append("")
        infos.append((name, up, len(rows), len(rnames)))
        ver = sref / name / "version.txt"
        js[name] = [ver.read_text().strip() if ver.exists() else "", keys]
    h.append("/* the synths, in this order on the FM-1 (after the classic Bfxr, synth 0) */")
    h.append(f"#define SYN_NPRESET {len(infos)}u")
    h.append("const uint8_t SYN_N = SYN_NPRESET;")
    h.append("const syn_info_t SYN_INFO[SYN_NPRESET] = {")
    for name, up, np, nr in infos:
        h.append(f'    {{"{name.upper()}", "{name}", {np}, {up}_P, {nr}, {up}_RECIPES, {RANDOMIZE.get(name, 0)}}},')
    h.append("};")
    out_h.write_text("\n".join(h) + "\n")
    out_js.write_text("// Generated by tools/gen_synth_tables.py: the FM-1's preset synths, in its order (synth 1, 2, ...; 0 is\n"
                      "// the classic Bfxr): [synth_type, its .bfxr version, its parameters in the order the slots hold them]\n"
                      "const PRESET_SYNTHS = " + json.dumps([[n, *js[n]] for n in names]) + ";\n")
    print(f"{len(infos)} synths -> {out_h}, {out_js}")


if __name__ == "__main__":
    main()
