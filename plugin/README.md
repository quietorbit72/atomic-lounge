# Atomic Lounge VST3 and standalone (Windows)

**Atomic Lounge (formerly Vintage Chonk)**: the synth as a VST3 instrument for any Windows DAW and as a
standalone app. Two engines: REGOLITH (the `dsp/` engine, called CHONK up to 1.1.0; 16 voices, desktop effect
sizes; the web panel, the same files as the Android tablet app, in a WebView2 window) and ORBIT
(poly-convolution, `dsp/orbit/`, its own panel `web/orbit/`). Product "Atomic Lounge", vendor "Atomic Lounge",
version 1.2.0, VST3 category Instrument|Synth, MIDI in, stereo out (mono offered), one stereo audio input
"Sidechain" (input bus 0, VST3 aux, mono accepted, active by default; used by ORBIT's INPUT). The build makes
`plugin\dist\Atomic Lounge.vst3` and `plugin\dist\Atomic Lounge.exe` (JUCE standalone wrapper: Options ->
Audio/MIDI Settings; the input starts muted). The VST3 class IDs (manufacturer `Cly1`, plugin `Mfm1`), the
parameter IDs and the state format are unchanged by the rename, so projects made with Vintage Chonk 1.0.0 /
1.1.0 find and load it as before.

## Build

```powershell
powershell -ExecutionPolicy Bypass -File plugin\fetch_deps.ps1              # JUCE + WebView2 SDK (pinned, checked)
powershell -ExecutionPolicy Bypass -File plugin\build.ps1                   # -> plugin\dist\Atomic Lounge.vst3
powershell -ExecutionPolicy Bypass -File plugin\build.ps1 -Test             # + host test (opens the editor ~10 s)
powershell -ExecutionPolicy Bypass -File plugin\build.ps1 -Test -NoScreenshot   # host test without a window
powershell -ExecutionPolicy Bypass -File plugin\build.ps1 -Dev -Test        # development build -> plugin\dist-dev
node plugin\tests\test_panel_desktop.mjs                                     # panel + desktop bridge in headless Chrome
powershell -ExecutionPolicy Bypass -File plugin\fetch_deps.ps1 -Validator   # Steinberg's validator.exe (VST3 SDK)
```

Needs: Visual Studio 2022 Build Tools (MSVC 14.44, Windows SDK 10.0.26100; its own CMake and Ninja are used),
git, and a `clang-cl.exe` (LLVM for Windows in `C:\Program Files\LLVM`, or set `MFE_CLANG_CL`): the engine's C
uses GCC/Clang vector extensions (the spring reverb tank) that MSVC's C compiler rejects, so `dsp/**/*.c` is
compiled by clang-cl (MSVC ABI and static runtime). Everything else is MSVC. node is optional (it regenerates
the factory bank header). `build.ps1` runs `fetch_deps.ps1` itself.

`fetch_deps.ps1` puts third-party code in `third_party/` (not part of this repository):

| What | Version | Check | Licence |
|---|---|---|---|
| JUCE | 8.0.15 (git tag) | commit 91ad83ae34a81e0833b1a2b0866f54846370ae53 | AGPLv3 (JUCE's open-source option) |
| Microsoft WebView2 SDK | NuGet Microsoft.Web.WebView2 1.0.4314.35 | .nupkg SHA-256 90778deb...44edc4, header and loader SHA-256 | BSD-3-Clause |
| Steinberg VST3 SDK (only for `-Validator`) | v3.8.1_build_84 + base, cmake, pluginterfaces, public.sdk | commit 3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96 + submodule commits | MIT |

**JUCE is used with one change**, `plugin/patches/juce-8.0.15-vst3-hidden-midi-cc-params.patch`, applied by
`fetch_deps.ps1` (CMake refuses an unpatched copy): JUCE's VST3 wrapper emulates MIDI controllers with
16 x 130 parameters (`JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS`, needed: mod wheel, sustain, pitch bend and
aftertouch reach a VST3 plugin only this way). Unpatched, DAWs list all 2,080 of them as automatable
parameters; the patch flags them `kIsHidden`, so a DAW shows the 22 parameters below plus Program and Bypass.
The modified file is `modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp` (AGPLv3).

The `.vst3` is self-contained (static runtime, static WebView2 loader); it needs the WebView2 runtime, which
Windows 11 has (Windows 10: Microsoft's Evergreen runtime).

## Install

Not installed by the build. Copy the bundle into the common VST3 folder from an **admin** PowerShell in the
repository folder:

```powershell
Copy-Item -Recurse -Force "plugin\dist\Atomic Lounge.vst3" "C:\Program Files\Common Files\VST3\"
```

then rescan plugins in the DAW (remove an older `Vintage Chonk.vst3` from that folder: same plugin, old name).
User presets are kept in `%APPDATA%\Atomic Lounge\presets\*.mfp`, ORBIT presets in `%APPDATA%\Atomic Lounge\orbit`,
dropped samples in `%APPDATA%\Atomic Lounge\samples`. While a new folder does not exist, the old
`%APPDATA%\Vintage Chonk\...` one is read instead; nothing is moved or deleted. The editor's WebView2 data is in
`%LOCALAPPDATA%\Atomic Lounge\WebView2\<host exe name>`, one folder per host program.

## How it works

| Part | File |
|---|---|
| Engine host: fixed 44.1 kHz engine, resampler, MIDI timing, patch triple buffer, panel note ring | `src/EngineRunner.*` |
| Processor: parameters, state, programs, tempo, the panel's patch diffing | `src/PluginProcessor.*`, `src/Params.h` |
| Editor: WebBrowserComponent (WebView2), panel files from binary data, events | `src/PluginEditor.*` |
| The panel's native bridge on the desktop + computer keyboard + browser keys off | `web/desktop_shim.js` |
| User presets `%APPDATA%\Atomic Lounge\presets\*.mfp` (old `Vintage Chonk` folder read while the new one is missing) | `src/PresetStore.*` |
| Factory bank as host programs (generated from the panel's `js/factory.js`) | `tools/gen_factory.mjs` -> `generated/factory_presets.h` |

**Audio.** The engine renders 128-frame blocks at 44.1 kHz on demand into a ring; a 64-tap Kaiser windowed-sinc
resampler (512 phases, linear between phases, cut-off 0.92 x Nyquist: flat to 18 kHz, -3.5 dB at 20 kHz, images
above 24.1 kHz at least 97 dB down) feeds the host rate; at exactly 44.1 kHz the engine output is copied. Any host
block size. MIDI is applied at the engine block nearest to the event (within +-64 engine samples = +-1.45 ms),
deterministically: the output depends only on the event times, never on the host's block size. Latency
reported to the host: 64 samples at 44.1 kHz (1.45 ms), 104 at 48 kHz (2.17 ms), 209 at 96 kHz (2.18 ms).
Host tempo -> `mfe_set_tempo` (synced LFOs and delays). Note on/off, sustain (CC 64), pitch bend, mod wheel
(CC 1), channel aftertouch, macros on CC 16-23, all-notes-off: as the engine (`mfe_midi`), omni. No allocation
or locks on the audio thread (tested).

**Host parameters** (22): Macro 1-8, Master Volume, Filter 1 Cutoff, Filter 1 Emphasis, Filter 2 Cutoff,
Mod Osc Ring / Phase / Wave, Oct Fuzz Amount, Tape Drive, Ensemble Mix, BBD Echo Mix, Oil Can Mix, Reverb
Amount, Glide. Each is one patch field; the mapping is exact both ways. Panel edits move them (with change
gestures); automation moves the sound at the next engine block and the panel at 30 Hz. The panel's sends
carry the last automation push they saw, so a stale send never drags automated values back.

**State.** The current `.mfp` (with the host values) + the panel's preset entry (factory/user, name, edited)
+ the program number, as XML in `getStateInformation` (the XML tag `VINTAGECHONK` is kept from the pre-release
name, so older projects still load). A reopened project plays and shows the same sound.
**Programs:** the 16 factory presets. (Re-selecting the current program is ignored: some hosts call it after a
project load.) The plugin's VST3 class IDs (manufacturer code `Cly1`, plugin code `Mfm1`) are unchanged by the
rename, so DAW projects made with earlier builds find it.

**Panel on the desktop.** Same files as the tablet. Mouse: drag a knob up/down, ctrl- or shift-drag = fine,
wheel (shift/ctrl = fine), double-click = default; the on-screen keyboard and wheels work with the mouse.
Computer keyboard: A W S E D F T G Y H U J K O L P = C4 to D#5, Z / X = octave. The window resizes with the
panel's 16:10 shape (default 1440 x 900, 720 x 450 to 3840 x 2400). IMPORT .NMSV opens a Windows file dialog.
The tablet-only "hold the title for Android settings" does nothing here.

## ORBIT mode (1.1.0)

The second engine, the poly-convolution ORBIT (`docs/ORBIT_MODE.md`, core `dsp/orbit/`, API `dsp/include/mfo.h`).
Development builds: `plugin\build.ps1 -Dev [-Test]` builds into `plugin\build-dev` and copies the bundle to
`plugin\dist-dev\Atomic Lounge.vst3` (the release `plugin\dist` is untouched).

| Part | File |
|---|---|
| Mode switch, cross-fade, latency, ORBIT state / panel ops | `src/PluginProcessor.*` |
| Engine host: worker-built assets, mailboxes + retire ring, MIDI-split render, EXT/macro feeds, drawn path, tape age | `src/OrbitRunner.*`, `src/TripleBuffer.h` |
| Settings, 42 host parameters, JSON / XML state with embedded audio, decoding, root key (YIN), factory + user presets | `src/OrbitModel.*` |
| The REGOLITH effects at 44.1 kHz on the host-rate ORBIT bus | `src/FxChain.*` |
| The panel (direction B "Streamline Lounge") | `web/orbit/orbit.html`, `orbit.css`, `orbit.js` |
| Preset level / render tool (development) | `tools/orbit_tool.cpp` (target `VintageChonkOrbitTool`) |

* **Mode** REGOLITH | ORBIT (CHONK | ORBIT up to 1.1.0; state value still `chonk`): the switch in either panel's header (REGOLITH panel: the foot bar, added by the desktop
  shim). Saved in the state (`mode` attribute). Switching cross-fades the engines (30 ms, equal power) and
  reports the new latency to the host. REGOLITH is the 1.0.0 engine, unchanged (tested bit-identical after a trip
  through ORBIT). The ORBIT engine is created the first time it is wanted.
* **Engine** at the host rate, 3 layers x 16 voices (synth layers: MFE_MAX_VOICES 16, as the tablet). Sources,
  impulses, synth prints, shaped IRs, BODY kernels and keytrack sets are built on a worker thread, handed over
  between render calls, and freed on the worker after the audio thread has let go. Keytrack sets (C2-C7, 1 s)
  are built only while a sample layer's KEYTRACK is on. Render calls split at MIDI offsets and keep to the
  core's 64-sample grid (block sizes on that grid render identically). Host tempo and transport feed the core,
  the effects and the modulation sources: MACRO1-4 (host parameters), mod wheel, aftertouch, and EXT1 1-bar
  sine, EXT2 16th-note accent stepper, EXT3 S&H per 8th, EXT4 pitch bend, EXT5 sustain, EXT6 note envelope,
  EXT7 beat ramp, EXT8 slow random walk. Sustain pedal held in the host.
* **Effects.** mfe_fx is Q15 at 44.1 kHz with every time constant fixed to that rate, so it is not rate-agnostic:
  it runs at 44.1 kHz between two Kaiser windowed-sinc resamplers in 16-frame blocks. Its delay is exact and
  reported: 16 samples at 44.1 kHz, 91 at 48 kHz, 179 at 96 kHz (about 1.9 ms). With every effect off the bus
  only passes the resamplers (float, no clipping). **Latency in ORBIT mode** = core STFT 256 + effects:
  347 samples at 48 kHz (7.2 ms).
* **Host parameters** (42, named "ORBIT ..."): puck X / Y, rate, wobble, spread, drift, tape age, output; per layer
  level, BODY, TONE, GRAIN, MOTION, PITCH, cutoff, resonance; the six effect amounts; macros 1-4. Every one of
  the 150 core parameters, the 32-slot mod matrix, path / drawn loop, effect switches and macro names are in the
  state.
* **Samples**: WAV / AIFF / FLAC through JUCE's AudioFormatManager (up to 120 s), root key detected by YIN on the
  steady part (editable). In the state by path **and** embedded (FLAC 24-bit, or WAV for odd rates) up to
  24 MB each; bigger files are kept by path only. Reopening: embedded audio first, then the path; if neither,
  the layer shows MISSING and the rest plays. Files dropped on the panel are read in the page, sent in chunks
  and kept in `%APPDATA%\Atomic Lounge\samples` (so they also have a path); LOAD FILE opens a Windows dialog.
* **Presets**: factory presets (only generated impulses and REGOLITH presets as synths or prints, and the live input in the INPUT presets); user presets
  `%APPDATA%\Atomic Lounge\orbit\*.orbit` (XML, samples embedded within the cap). Host programs: 0-15 the 16
  REGOLITH factory presets (as in 1.0.0), 16-23 the 1.1.0 ORBIT ones and 24 and up the 1.2.0 ones as "ORBIT - <name>"; choosing one switches the mode
  (the state keeps the CHONK program in `program`, as 1.0.0, and the ORBIT one in `orbitProgram`; a 1.0.0 state
  loads as CHONK, unchanged: tested with a real 1.0.0 state, `tests/fixtures/state_1.0.0_program7_macro2.bin`).
  Levels: each preset's output is set (with `tools/orbit_tool.cpp`) to about -19 dBFS RMS on its demo score,
  peaks at or below -1.5 dBFS also on a plain C chord (SPRING RAIN, peak-limited, about -23.5 dBFS RMS).
  Renders: `examples/orbit/presets/*.wav` (written by the host test, through the VST3, not normalised).

## 1.2.0 (development, 21 Nov 2026)

* **Rename**: Atomic Lounge (formerly Vintage Chonk); the CHONK mode is REGOLITH on every user-facing surface
  (mode switch REGOLITH | ORBIT, "REGOLITH PRINT", "REGOLITH preset"). Internal names, the state tag
  `VINTAGECHONK`, the `mode="chonk"` state value, parameter IDs and the class IDs are kept: a 1.1.0 project
  saved in CHONK mode loads as REGOLITH and renders bit-identically.
* **WebView2 fix** (`src/PluginEditor.*`): the user data folder used to be one folder for every process, so a
  running standalone owned the WebView2 browser process and a DAW's editor never got one (Ableton Live 10:
  sound, no panel). Now `%LOCALAPPDATA%\Atomic Lounge\WebView2\<host exe name>`, shared by all instances inside
  one host. If WebView2 environment or controller creation fails, the editor shows a plain JUCE message with
  the reason and the HRESULT and a Retry button instead of a blank window.
* **Sidechain input**: one stereo audio input bus "Sidechain" (input bus 0, VST3 type aux via
  `VST3ClientExtensions::getPluginHasMainInput() == false`, stereo or mono, default-active). Hosts feed the
  first aux input bus; Ableton Live and other DAWs route it as the plug-in sidechain ("Audio From"). An absent,
  disabled or silent input gives silence (non-finite samples are zeroed). REGOLITH ignores it.
* **ORBIT INPUT**: INPUT as a layer's SOURCE (GATE THRU, the default, or NOTE through the layer's amp envelope)
  or IMPULSE. BODY: true convolution with a continuously captured window of the input (100 / 250 / 500 /
  1000 ms, default 250 ms); a new kernel about every 6720 samples (140 ms at 48 kHz), built incrementally on the audio thread without
  allocation into the idle one of two convolvers that both hear the whole input history, then a 2048-sample
  cross-fade; kernels at unit energy with a floor, so silence gives silence. TONE / GRAIN / PITCH / MOTION: the
  input is the live modulator (as LIVE). INPUT meter and TRIM (-24..+24 dB).
* **Held orbit**: MOTION ON / OFF next to CIRCLE / FIG 8 / DRAWN; dragging a moving puck parks it (motion off),
  double-clicking the field restarts it; RATE at its minimum (free) = stop, no creep; WOBBLE moves only a moving
  puck; a held puck moves only with PUCK DRIFT (default 0). States from 1.1.0 keep the old behaviour (wobble
  moved a held puck). ORBIT Puck X / Y move the parked puck (smoothed); the panel follows automation.
* **New host parameters** (appended, so the 1.1.0 indices are unchanged): ORBIT Motion, ORBIT Puck Drift,
  ORBIT Input Trim, ORBIT L1-L3 Source Input, ORBIT L1-L3 Impulse Input, ORBIT L1-L3 Input Gate.
* **New factory presets** (programs 25 and up): INPUT - SPRING THROAT, INPUT - HALO FOLLOWER, and held-puck
  presets with a macro on the puck. Programs 1-24 are unchanged.
* **Standalone**: the audio device's input channels feed the sidechain; muted by default (JUCE's
  "Mute audio input") to avoid feedback.
* **Tests 1.2.0** (dev build, 21 Nov 2026): host test 161/0 (incl. 1.1.0 states bit-identical against the released
  1.1.0 bundle, all 2146 1.1.0 parameter ids kept, WebView2 per-host folder with two editors in one host + a second host +
  the fallback), panel test 64/0, Steinberg validator 47/0 (`plugin/validator_log_dev_1.2.0.txt`), ORBIT core 157/0 +
  live input 40/0 (`tests/orbit/test_orbit_input.c`), a host-side checker: input bus 0 fed, the output changes
  with the input for BODY, TONE, MOTION and SOURCE setups.

## Tests (22 Nov 2026, release 1.2.0)

* Release build: clean build of the public tree (`plugin\build.ps1 -Clean -Test`, log
  `plugin/clean_public_build_log_1.2.0.txt`); bundle and standalone exe copied to `plugin\dist`. Windows version
  resource of both: ProductName "Atomic Lounge", FileVersion 1.2.1, CompanyName "Atomic Lounge"; the VST3 factory URL is
  the source repository.
* `VintageChonkHostTest`: 161 passed, 0 failed with the released 1.1.0 bundle as the reference (`--ref`; 1.1.0
  states bit-identical, all 1.1.0 parameter ids kept); 159 passed, 0 failed without it (a fresh clone: the 1.1.0
  states are checked on the saved fixtures). 27 host programs (16 REGOLITH + 11 ORBIT), 78 visible parameters
  (22 + 54 + Bypass, Program).
* `test_panel_desktop.mjs`: 64 passed, 0 failed.
* Steinberg validator on the release bundle: 47 passed, 0 failed (`plugin/validator_log_release_1.2.0.txt`).
* `run_tests.ps1` of the public tree: ALL TESTS PASSED (engine 631 + 631, sequencer 65, ORBIT core 157, live input
  40, converter, panel 297) (`plugin/run_tests_log_1.2.0.txt`).
* The 11 ORBIT preset renders of the clean build are bit-identical to `examples/orbit/presets/*.flac`.

## Tests (20 Nov 2026, release 1.1.0)

* Release build: clean build of the public tree (`plugin\build.ps1 -Clean -Test`, log
  `plugin/clean_public_build_log_1.1.0.txt`); bundle and standalone exe copied to `plugin\dist`.
* `VintageChonkHostTest`: 122 passed, 0 failed (the 1.0.0 checks, version 1.1.0, plus ORBIT: the effects
  wrapper is an exact reported delay with everything off at 44.1 / 48 / 96 kHz and rings with everything on;
  core + effects latency; no allocation on the ORBIT audio thread across MIDI, panel notes and an impulse swap;
  block sizes on the 64 grid bit-identical, 1000 within 91 dB; CPU; YIN root key; 42 host parameters; mode switch
  through the state with the latency reported (104 -> 347 -> 104 at 48 kHz); CHONK after ORBIT bit-identical to
  a fresh instance; click-free cross-fade; state round trip with an embedded sample after the file is deleted;
  the missing-sample fallback; ORBIT cutoff / output / puck automation; 24 host programs (16 CHONK + 8 "ORBIT -
  <name>"), each ORBIT program switches the mode, loads, plays at or below -1 dBFS and survives the state; a real
  1.0.0 state (`tests/fixtures/state_1.0.0_program7_macro2.bin`, saved by the 1.0.0 bundle) loads as CHONK program 7
  with its macro, also into an instance playing ORBIT, and renders exactly as program 7 chosen by hand; the 8 factory
  presets through the VST3 at or below -1 dBFS and their WAVs; the ORBIT editor in the running plugin).
* Cross-version: the same 1.0.0 project state rendered by the 1.0.0 and the 1.1.0 binaries (pedalboard host):
  bit-identical.
* `test_panel_desktop.mjs`: 48 passed, 0 failed (24 CHONK + 24 ORBIT panel checks with real mouse / keyboard).
* Steinberg validator on the release bundle: 47 passed, 0 failed (`plugin/validator_log_release_1.1.0.txt`).
* `run_tests.ps1` of the public tree: ALL TESTS PASSED, with the ORBIT core tests 157/0 (`plugin/run_tests_log_1.1.0.txt`).
* CPU, 48 kHz / 256, three layers, five-note chords, effects on: 8.6-11.2 % of one core (11.4 % through the VST3).

## Tests (18 Nov 2026, release 1.0.0)

* `VintageChonkHostTest` (JUCE VST3 hosting of the built bundle): 77 checks: 44.1 / 48 / 96 kHz x block sizes
  32 / 64 / 441 / 512 / 1024 all bit-identical; at 44.1 kHz equal to the offline engine render 64 samples later;
  at 48 / 96 kHz 136 dB SNR against an exact offline resample; latency; name, vendor and version; state round
  trip (byte-identical, same sound); 16 programs; cutoff / volume automation and a mid-note sweep; host tempo
  moves a synced echo; the parameter list a DAW sees (24 visible: our 22 + Bypass + Program; 2,080 MIDI CC
  parameters hidden); mod wheel, pitch bend and sustain change the sound through the VST3 path; the one-time
  preset migration; zero heap allocations in the audio path; the editor opens, the panel reports to the
  plugin, a host program change reaches the panel and comes back, automation reaches it without an echo;
  screenshots `plugin/build/test_out/editor_1440x900.png`, `editor_960x600.png`.
* `test_panel_desktop.mjs` (headless Chrome, real mouse/keyboard input): 24 checks of the shim and mouse.
* Steinberg's VST3 validator (VST3 SDK v3.8.1_build_84, `validator.exe` on the bundle): 47 tests passed,
  0 failed.

## Licences

* Atomic Lounge code: AGPL-3.0-only in the public source tree (`LICENSE`); a distributed plugin binary is
  AGPLv3 because it links JUCE under AGPLv3, so it must come with (or offer) its complete source.
* JUCE 8: AGPLv3 (with the patch above), and the third-party code inside JUCE listed in `NOTICE`.
* Steinberg VST3 SDK (inside JUCE, and the validator): MIT. VST is a trademark of Steinberg Media Technologies GmbH.
* Microsoft WebView2 SDK (headers, `WebView2LoaderStatic.lib` linked in): BSD-3-Clause, Copyright (C) Microsoft
  Corporation.
