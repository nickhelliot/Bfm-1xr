# Checking the Bfxr port

`gen_ref.js` renders reference audio with the bfxr2 JavaScript library; `bfxr_ref.c` is a double-precision C copy
of the classic Bfxr engine that must match it. The fixed-point engine is then checked against `bfxr_ref.c`.

    git clone --depth 1 https://github.com/increpare/bfxr2 /tmp/bfxr2
    node tools/bfxr/gen_ref.js /tmp/bfxr2 /tmp/ref
    cc -O2 -ffp-contract=off -I /tmp/ref -o /tmp/bfxr_ref tools/bfxr/bfxr_ref.c -lm
    /tmp/bfxr_ref /tmp/ref

Last run: 336 cases (7 presets x 4 seeds x 12 wave types), same length as the JavaScript in all of them, largest
difference 6e-8 (float32 rounding).
