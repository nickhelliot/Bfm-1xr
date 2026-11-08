# BFXR for the M-VAVE FM-1

Firmware that turns the FM-1 into [bfxr.net](https://www.bfxr.net)'s sound-effect maker: the same sounds, made the same
way, played from the keys.

![The screen](docs/screen.png)

| Control | What it does |
| --- | --- |
| PRESET | A kind of sound: PICKUP/COIN, LASER/SHOOT, EXPLOSION, POWERUP, HIT/HURT, JUMP, BLIP/SELECT, RANDOMIZE. Each turn makes a new one and plays it |
| EDIT | Another new sound of the same kind |
| FX | Mutate the sound a little |
| SELECT | Scroll through bfxr's 32 parameters; K1 to K4 change the four marked rows. A change plays the sound once the knobs rest |
| ALGO | The wave: SQUARE, SAW, SIN, WHITE, TRIANGLE, RASP, TAN, WHISTLE, BREAKER, BITNOISE, FMSYN, VOICE |
| PLAY | Play the sound |
| Keys | Play it at a pitch: C4 is the sound as made, the other keys transpose it. OCT- / OCT+ shift the octave |
| HOME | Back to the top of the list |
| SAVE | Keep the sound: it is back at the next power-on |
| MASTER | Volume |

USB MIDI in plays the sound too (middle C at its own pitch); the keys send USB MIDI out.

## How close it is to bfxr.net

The engine is bfxr2's classic Bfxr engine, ported line by line to C and played sample by sample (the FM-1 has no room
for a whole sound in memory). `tools/check_engine.c` plays 336 sounds made by bfxr2's own JavaScript (every wave, every
kind of sound) and compares: 328 match sample for sample, 6 more match in loudness and pitch (a period one sample
early or late), and 2 differ slightly: the TAN wave through a filter, where the result is decided by rounding.

## Build

GitHub builds every push (Actions, "Build firmware": the package is in the run's artifacts). To build it yourself,
on Linux x86-64 or with Docker: JieLi's toolchain and SDK as in [fm1-base](https://github.com/charlesvestal/fm1-base),
then

    git clone --recursive https://github.com/nickhelliot/Bfm-1xr
    ./build.sh                     # app/build/bfxr.fwsc
    ./build.sh --host              # the simulator: app/build/host/bfxr_host app/tests/basic.txt app/build/host

Install with fm1-base's installer page or `python3 base/tools/fm1_install.py app/build/bfxr.fwsc`. Unofficial firmware,
at your own risk; M-VAVE's updater puts the stock firmware back.

## Credits and licence

- Sound engine and generators: [bfxr2](https://github.com/increpare/bfxr2) by Stephen Lavelle (MIT), from Thomas
  Vian's SfxrSynth (Apache-2.0)
- Waves: [Adventure Kid Waveforms](https://www.adventurekid.se/akrt/waveforms/adventure-kid-waveforms/) (CC0)
- Platform: [fm1-base](https://github.com/charlesvestal/fm1-base) by Charles Vestal and
  [felucca-bsp](https://github.com/hugelton/felucca-bsp) by Leo Kuroshita (MPL-2.0, in `base/`)

This firmware's own files are MIT ([LICENSE](LICENSE)).
