# BFXR for the M-VAVE FM-1

BFXR turns the FM-1 into [bfxr.net](https://www.bfxr.net)'s sound-effect maker: coins, lasers, explosions, power-ups,
hits, jumps and blips, made the way bfxr.net makes them and played from the keys.

![The BFXR screen](docs/screen.png)

**[Install it](https://nickhelliot.github.io/Bfm-1xr/install/)** · **[Try it in your browser](https://nickhelliot.github.io/Bfm-1xr/emu/)**

---

## Install

1. Connect the FM-1 to your computer by USB and switch it on.
2. Open **https://nickhelliot.github.io/Bfm-1xr/install/** in Chrome or Edge. Allow MIDI access when the browser asks.
3. Press **Connect**, then **Install**. Keep the cable in and the tab open until it says it's done. The FM-1 restarts
   into BFXR: a cream screen with a maroon header.

If an install is interrupted, plug the FM-1 back in and press **Install** again: it carries on where it stopped.

To go back to M-VAVE's firmware or to another one, use the same page or the
[FM-1 Firmware Hub](https://thefirmwarehub.com). Installing firmware is at your own risk.

## The screen

- **Header:** BFXR, the kind of sound you last made, and **PLAYING** while a sound plays.
- **The list:** bfxr's 32 parameters, each with a bar and its value. The bar fills from the left for settings that
  go from 0 up, and from the middle for settings that go both ways (up or down, faster or slower).
- **K1 to K4:** the four highlighted rows are the ones the four knobs change.
- **Footer:** what you just did (the value you changed, SAVED, MUTATED), or a reminder of the main buttons and the
  octave.

## The controls

| Control | What it does |
| --- | --- |
| **PRESET** | Picks a kind of sound and makes a new one, then plays it: PICKUP/COIN, LASER/SHOOT, EXPLOSION, POWERUP, HIT/HURT, JUMP, BLIP/SELECT, RANDOMIZE. Each click is a fresh sound |
| **EDIT** | Another new sound of the same kind. Press it until you hear one you like |
| **FX** | Mutate: nudges the sound a little in a random direction (and sometimes changes the wave) |
| **SELECT** | Scrolls the list. The four highlighted rows move with it |
| **K1 to K4** | Change the four highlighted rows. Turn slowly for fine steps (1 % of the range), fast for bigger ones |
| **ALGO** | Changes the wave, from anywhere in the list |
| **PLAY** | Plays the sound |
| **Keys** | Play the sound at a pitch. **C4** (the 5th white key from the left) plays it as made; the others transpose it |
| **OCT− / OCT+** | Shift the keys and PLAY down or up an octave (−2 to +2) |
| **HOME** | Jumps back to the top of the list |
| **SAVE** | Keeps the sound: it's there again the next time you switch on. SAVE is lit while there's something unsaved |
| **MASTER** | Volume |

After you change a value, the sound plays once you stop turning, as bfxr.net plays when you let go of a slider.

**MIDI:** notes from USB play the sound (middle C at its own pitch), and the keys send notes out over USB.

## Making a sound

1. **Start from a kind.** Turn **PRESET** to the kind you want. Press **EDIT** until you hear something close.
2. **Nudge it.** Press **FX** a few times to try variations. If a variation is worse, press **EDIT** for a new start.
3. **Shape it.** Turn **SELECT** to the part you want to change and use **K1 to K4**. The useful places to start:
   - **Length:** ATTACK, SUSTAIN, DECAY.
   - **Pitch:** FREQUENCY, and FREQ SLIDE to make it rise or fall.
   - **Character:** the wave (**ALGO**), HARMONICS, LOWPASS, BIT CRUSH.
4. **Play it in tune.** Use the keys and **OCT− / OCT+** to hear it higher or lower.
5. **Keep it.** Press **SAVE**.

BFXR keeps one sound. To keep several, record each one (the FM-1 is a USB audio input: record it in any DAW) before
you make the next.

## The parameters

The same as bfxr.net's sliders, in the same order. Values are bfxr's, from 0 to 1 (or −1 to 1).

| On the screen | bfxr.net | What it does |
| --- | --- | --- |
| VOLUME | Sound Volume | How loud this sound is |
| WAVE | (the wave buttons) | The basic tone: SQUARE, SAW, SIN, WHITE (noise), TRIANGLE, RASP, TAN, WHISTLE, BREAKER, BITNOISE, FMSYN, VOICE |
| ATTACK | Attack Time | How long the sound takes to fade in |
| SUSTAIN | Sustain Time | How long it holds at full volume |
| PUNCH | Punch | A burst of extra volume at the start of the sustain, for more pop |
| DECAY | Decay Time | How long it takes to fade out |
| COMPRESSION | Compression | Squeezes loud and quiet parts together so the sound stands out over music |
| FREQUENCY | Frequency | The starting pitch |
| FREQ SLIDE | Frequency Slide | Slides the pitch up (positive) or down (negative) |
| DELTA SLIDE | Delta Slide | Speeds the slide up or slows it down; can turn it around |
| FREQ CUTOFF | Frequency Cutoff | Stops a falling sound at this pitch, so it doesn't sink into a rumble |
| VIBRATO DEPTH | Vibrato Depth | How much the pitch wobbles |
| VIBRATO SPEED | Vibrato Speed | How fast it wobbles |
| JUMP REPEAT | Pitch Jump Repeat Speed | How often the pitch jumps repeat (up to 50 a second): for arpeggios |
| JUMP 1 AMOUNT | Pitch Jump Amount 1 | A sudden jump in pitch, up or down |
| JUMP 1 ONSET | Pitch Jump Onset 1 | When the first jump happens |
| JUMP 2 AMOUNT | Pitch Jump Amount 2 | A second jump |
| JUMP 2 ONSET | Pitch Jump Onset 2 | When the second jump happens |
| HARMONICS | Harmonics | Layers copies of the wave at higher pitches: a fuller, richer tone |
| HARM FALLOFF | Harmonics Falloff | How quickly those higher copies get quieter |
| SQUARE DUTY | Square Duty | SQUARE wave only: the shape of the square, from hollow to thin |
| DUTY SWEEP | Duty Sweep | SQUARE wave only: changes that shape as the sound plays |
| REPEAT SPEED | Repeat Speed | Restarts the slides and sweeps over and over (up to 10 times a second) |
| FLANGER OFFSET | Flanger Offset | Mixes in a slightly delayed copy: a hollow, jet-like tone |
| FLANGER SWEEP | Flanger Sweep | Moves that delay as the sound plays |
| LOWPASS | Low-pass Filter Cutoff | Cuts the high frequencies: lower is duller. All the way down can make the sound silent |
| LOWPASS SWEEP | Low-pass Filter Cutoff Sweep | Opens or closes the low-pass as the sound plays |
| LOWPASS RESO | Low-pass Filter Resonance | Adds a ring at the low-pass's edge |
| HIGHPASS | High-pass Filter Cutoff | Cuts the low frequencies: higher is thinner |
| HIGHPASS SWEEP | High-pass Filter Cutoff Sweep | Moves the high-pass as the sound plays |
| BIT CRUSH | Bit Crush | Lowers the sample rate for a crunchy, retro sound |
| CRUSH SWEEP | Bit Crush Sweep | Changes the crush as the sound plays |

## Tips

- **I can't hear anything.** Check MASTER, then VOLUME, then LOWPASS (all the way down silences most sounds) and
  FREQ CUTOFF (it can stop a falling sound almost at once).
- **TAN is noisy, and with HARMONICS up it barely follows the keys.** That's how bfxr's TAN wave works (bfxr.net does
  the same). For a pitched TAN, keep HARMONICS low, raise HARM FALLOFF, or soften it with LOWPASS.
- **Lots of HARMONICS is the heaviest thing for the FM-1**, as it is for bfxr.net. If you ever hear clicks or gaps
  that bfxr.net doesn't make, report it with the settings you used.
- **A sound only plays once it's finished being edited:** turn a knob, stop, and it plays.

## If something goes wrong

- **It won't start, or keeps restarting:** hold **OCT−** while switching on. The FM-1 starts in its USB updater, with
  no sound, so you can install BFXR again or another firmware.
- **The installer can't find the FM-1:** use Chrome or Edge, allow MIDI access, and try another USB cable or port.
  Close other programs that use MIDI (DAWs, other installer tabs).

---

## How close it is to bfxr.net

The sound engine is bfxr2's classic Bfxr engine, ported line by line to C and played sample by sample (the FM-1 has
no room to hold a whole sound in memory). `tools/check_engine.c` plays 336 sounds made by bfxr2's own JavaScript,
with every wave and every kind of sound: 327 come out the same sample for sample, 7 more the same to the ear (a wave
cycle one sample early or late), and 2 slightly different (the TAN wave through a filter, where rounding decides the
result).

## Build it yourself

GitHub builds every push (Actions, "Build firmware") and publishes the installer and the emulator. The version is in
`app/VERSION`. To build on your own machine, on Linux x86-64 or with Docker, set up JieLi's toolchain and SDK as in
[fm1-base](https://github.com/charlesvestal/fm1-base), then:

    git clone --recursive -b claude/bfxr-firmware https://github.com/nickhelliot/Bfm-1xr
    ./build.sh                     # app/build/bfxr.fwsc
    ./build.sh --host              # the simulator: app/build/host/bfxr_host app/tests/basic.txt app/build/host
    python3 base/tools/fm1_install.py app/build/bfxr.fwsc

| Where | What |
| --- | --- |
| `app/bfxr.c` | The sound engine and the sound generators (from bfxr2) |
| `app/app.c` | The screen, the controls, saving |
| `app/tests/basic.txt` | The simulator's checks |
| `tools/check_engine.c`, `tools/gen_ref.js` | The comparison with bfxr2's JavaScript |
| `base/` | fm1-base, the platform (a git submodule) |

## Credits and licence

- Sound engine and generators: [bfxr2](https://github.com/increpare/bfxr2) by Stephen Lavelle (MIT), from Thomas
  Vian's SfxrSynth (Apache-2.0)
- Waves: [Adventure Kid Waveforms](https://www.adventurekid.se/akrt/waveforms/adventure-kid-waveforms/) (CC0)
- Platform: [fm1-base](https://github.com/charlesvestal/fm1-base) by Charles Vestal and
  [felucca-bsp](https://github.com/hugelton/felucca-bsp) by Leo Kuroshita (MPL-2.0), in `base/`

BFXR's own files are MIT ([LICENSE](LICENSE)). M-VAVE and FM-1 are trademarks of their owners; BFXR is not affiliated
with M-VAVE.
