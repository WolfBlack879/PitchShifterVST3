# Bass Pitch Shifter (VST3)

A minimal pitch-shifter plugin tuned for bass guitar and synth bass.

![UI](docs/screenshot.png)

| Knob   | Range            | What it does                                     |
|--------|------------------|--------------------------------------------------|
| Pitch  | −24 … +12 st     | Shift in semitones (−12 = octave down)           |
| Fine   | −100 … +100 ct   | Fine tune in cents                               |
| Mix    | 0 … 100 %        | Dry/wet blend (dry is latency-aligned, no combing) |
| Output | −24 … +12 dB     | Output level                                     |
| Studio / Live | switch    | Choose the engine (see below)                    |
| Mono / Stereo | switch    | Mono sums L+R and outputs the same signal on both sides (half the CPU, rock-solid centre). Stereo keeps the input's stereo image, with both sides shifted together. |

Double-click any knob to reset it.

## Two modes

| | **Live** (default) | **Studio** |
|---|---|---|
| Use it for | Playing live / monitoring through the plugin | Recording, re-amping, mixing |
| Delay | About half a period of the note: ~6 ms on E2, ~12 ms on low E, ~16 ms on low B (worst case one full period) | Fixed 3584 samples (~75 ms at 48 kHz), compensated by the DAW |
| Material | Single notes (chords will warble) | Single notes and chords |
| Dry signal in Mix | Not delayed, like an octave pedal | Delayed to stay phase-aligned |

Upward shifts in Live mode add a few ms more than downward shifts (the
crossfade needs some headroom). Live mode reports 0 latency to the host so it
doesn't hold back other tracks while you play.

Total delay you hear live = this plugin + your audio interface's round trip,
so also keep the buffer size small (64–128 samples) on stage.

## How it works

**Live** — `Source/LiveShifter.h` is a pitch-synchronous delay-line shifter:

- A read head moves through a short delay line at the pitch ratio. When it
  runs out of room, it hands over to a new head exactly one period of the
  note away, with a short crossfade. Because the jump is a whole period, the
  splice lines up with the waveform and is close to inaudible.
- The period comes from a YIN pitch detector on a low-passed, decimated copy
  of the input (25–700 Hz), and each splice is fine-tuned to sub-sample
  accuracy with a small cross-correlation search.

**Studio** — `Source/PitchShifter.h` is a phase vocoder with peak phase locking
(Laroche & Dolson, 1999):

- Long FFT (4096 samples at 44.1/48 kHz, scaled up at higher rates) with 8×
  overlap, so notes down to low B (~31 Hz) are resolved cleanly.
- Each spectral peak and its surrounding bins are moved as one block and
  rotated by a single phase, which keeps harmonics coherent and avoids the
  "phasey" sound of naive bin-by-bin shifting.
- The dry signal is delayed by the same latency so Mix blends in phase.


## Building

Requires CMake ≥ 3.22 and a C++17 compiler. JUCE 8 is downloaded automatically.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The plugin ends up in `build/BassPitchShifter_artefacts/Release/VST3/Bass Pitch Shifter.vst3`.
Copy it to your VST3 folder:

- **Windows:** `C:\Program Files\Common Files\VST3\`
- **macOS:** `~/Library/Audio/Plug-Ins/VST3/`
- **Linux:** `~/.vst3/`

On Linux you'll need: `libasound2-dev libx11-dev libxrandr-dev libxinerama-dev
libxcursor-dev libxcomposite-dev libfreetype6-dev libfontconfig1-dev`.

Prebuilt Windows/macOS/Linux builds are produced by GitHub Actions on every
push (Actions tab → latest run → Artifacts).

### DSP test

`build/DspTest_artefacts/Release/DspTest` runs bass sine waves through both
engines at several sample rates and intervals, checking output pitch (within
5 cents), level and distortion (residual below −30 dB). It also prints Live
mode's average and maximum delay, and checks that a harmonic-rich bass tone
changing note is tracked correctly.

### DISCLAIMER AND AI USE
This is a personal project to have a minimalist, easy to use vst3 Pitch Shifter for bass. I haven't tested it on any other instrument yet.
This was made using Claude Opus 5.5 because I needed something quite fast, and this was made in like... ten to twenty minutes or so.
You are free to use it however you like. How it is now is fine for me, though, if you ever want another feature implemented, you can make a pull request of develop it on your own. I am not getting any revenue for doing this, or creating this vst.
