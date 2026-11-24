# Atomic Lounge: ORBIT mode (poly-convolution engine)

ORBIT is the second engine of Atomic Lounge (from 1.1.0), next to the REGOLITH synth. It is an original poly-convolution and
cross-synthesis design: up to three layers, each a played **source** combined with an **impulse** along five
separate dimensions, blended on a two-dimensional field by a puck that moves or stays parked. Poly-convolution and spectral
cross-synthesis are general signal-processing techniques; the engine, the panel, the generated impulses and
the presets were all written or generated for this project.

User documentation: the ORBIT chapters of `docs/guide/Atomic-Lounge-User-Guide.pdf`. This note is the
technical overview.

## 1. Signal flow

```
 MIDI --> LAYER 1..3:  SOURCE (sample | REGOLITH synth | input)  -->  CONVOLVE MATRIX  <--  IMPULSE (sample | synth print | live layer | input)
                                                          BODY, TONE, GRAIN, MOTION, PITCH
                                                                 |
                                                       FILTER (ladder / SVF) --> AMP ENV --> layer out
          ORBIT BLENDER: three layer nodes + DRY on a 2-D field; the puck (X/Y) blends them and can follow a
          path (CIRCLE, FIG 8, DRAWN; MOTION ON / OFF) with rate, wobble, spread, sync and direction, or stay parked
          MASTER: drift, tape age, the REGOLITH effects chain (OCT FUZZ, TAPE, ENSEMBLE, BBD ECHO, OIL CAN,
          TWIN TANK / SPRING), macros 1-4, output
```

### Sources (what is played)
* **SAMPLE**: WAV, AIFF or FLAC, root key detected (YIN) and editable; start, loop in/out, cross-fade,
  reverse, stretch, tape mode (wow, flutter, hiss), velocity to start. Or a **print**: a REGOLITH preset
  rendered to a sample at a chosen note and length.
* **SYNTH**: a full REGOLITH voice playing any factory or user preset.
* **INPUT**: the plugin's stereo sidechain input (another track routed in by the DAW), played whenever audio
  arrives (GATE THRU) or only while notes are held, through the layer's envelope (GATE NOTE).

### Impulses (what the source is combined with)
* A sample, a generated factory impulse (spring tank, plate, bowed metal, glass, wood box, formant voice,
  metal hit, tape hiss), a synth print, or **LIVE**: another layer's running output (true cross-synthesis
  between two playing sources), or **INPUT**: the sidechain input. As the BODY impulse the input is captured
  continuously into a window of 100 ms to 1 s (250 ms by default), refreshed about every 140 ms with a smooth
  cross-fade; TONE / GRAIN / MOTION / PITCH use it as the live modulator.
* Shaping: start, length (up to 6 s), pitch (fixed, or KEYTRACK per voice), damp, decay tilt, age, reverse,
  gain.

### The convolve matrix
Each layer has five amounts (0-100 %), each a modulation destination:

| Dimension | What crosses over from the impulse | DSP |
|---|---|---|
| **BODY** | the whole impulse: resonance, space, smear (true convolution) | uniform partitioned FFT convolution |
| **TONE** | its spectral envelope only (formants, body colour); the source keeps its waveform | STFT, cepstrally smoothed impulse magnitude imposed on the source |
| **GRAIN** | its fine structure and phase (texture, noise, roughness); the source keeps its EQ | STFT, impulse phase and fine structure under the source's envelope |
| **MOTION** | its loudness contour and transients (rhythm, swell, chop) | envelope follower / transient map applied to the source |
| **PITCH** | its partials re-tuned onto the played note (harmonic morph) | STFT, impulse spectrum warped onto the source's harmonic comb |

BODY 100 % and the rest 0 is classic convolution; TONE alone is a living "EQ match"; GRAIN and MOTION
together make cross-synthesis textures.

## 2. Implementation

* Core: `dsp/orbit/` (portable C99, float), API `dsp/include/mfo.h`, tests `tests/orbit/test_orbit.c`
  (FFT against a naive DFT, partitioned against direct convolution, transparency of each dimension at 0,
  stability, the blender, keytrack and synth sources) and `tests/orbit/test_orbit_input.c` (live input,
  parked puck), benchmark `tests/orbit/bench_orbit.c`, demo renders
  `tests/orbit/orbit_demos.c`.
* BODY: zero-latency partitioned convolution (direct taps 0-63, FFT partitions 64 / 512 / 4096), impulses
  up to 6 s; KEYTRACK uses per-voice convolvers with the impulse resampled for every note, built off the
  audio thread.
* TONE / GRAIN / PITCH: low-latency STFT (1024-point analysis, asymmetric windows, hop 128).
* Plugin host: `plugin/src/OrbitRunner.*` (worker-thread asset building, lock-free hand-over, no allocation
  on the audio thread), `plugin/src/OrbitModel.*` (settings, 54 host parameters, state with embedded
  samples, factory presets), `plugin/src/FxChain.*` (the effects chain at 44.1 kHz between two resamplers),
  panel `plugin/web/orbit/`.

## 3. Latency and CPU

* ORBIT reports its latency to the host: STFT 256 samples plus the effects wrapper, 347 samples at 48 kHz
  (about 7.2 ms). REGOLITH keeps its 1.0.0 latency (104 samples at 48 kHz). The live input as a source has
  exactly that reported latency; as a BODY impulse new input starts to fade in about 97 ms after it arrives.
* Three layers, 16 voices, 4 s impulses, all five dimensions, 48 kHz / 256: about 12-13 % of one core in the
  core benchmark; the factory presets measure about 9-12 % through the VST3.

## 4. Sound content

Only material made for this project: the generated impulses (`dsp/orbit/mfo_impgen.c`), REGOLITH factory
presets as synths or prints, and whatever audio the user loads. No third-party samples are included.
