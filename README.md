# Gitto FX

Audio effect plugins by JG Productions. Original DSP, one shared interface style.

| Plugin | What it is | Highlights |
| --- | --- | --- |
| Gitto FX EQ | 8-band parametric and dynamic EQ | 8 shapes, cuts to 48 dB/oct, per-band mid/side, dynamic bands, analog-matched curves, spectrum analyzer |
| Gitto FX Compressor | 6-style compressor | Clean, Punch, Glue, Opto, Vari-Mu, FET; lookahead, hold, range, external sidechain, mix |
| Gitto FX Limiter | True-peak limiter | 5 algorithms, 8x inter-sample peak detection, LUFS metering, dither |
| Gitto FX Reverb | Algorithmic reverb | 12 modes, 3 colours, frequency-dependent decay, modulation, freeze |
| Gitto FX Delay | Character delay | 8 echo styles, single, dual, ping-pong and rhythm modes, tempo sync, groove, ducking |
| Gitto FX Chorus | Chorus | 4 chorus types, 4 preset modes each plus manual, tone and warmth |
| Gitto FX Multiband | 4-band dynamics | Compress or expand upward per band, stereo/mid/side per band, lookahead, phase-matched mix |
| Gitto FX Smooth | Resonance suppressor | Finds and turns down whatever sticks out of the spectrum, moment by moment; delta listen |
| Gitto FX Opto Comp | Optical levelling amplifier | Two knobs, programme-dependent two-stage release, compress and limit modes |
| Gitto FX FET Comp | FET peak limiter | Fixed threshold driven by Input, 4/8/12/20 and All ratios, attack down to 0.02 ms |
| Gitto FX Bus Comp | Mix bus compressor | Stepped ratio, attack and release with Auto, sidechain high-pass, mix |
| Gitto FX De-Esser | De-esser | Level-independent vocal mode, wide or split band, lookahead, listen |
| Gitto FX Channel Strip | Console channel | Drive, filters, 4-band EQ with two characters, compressor, gate/expander, switchable order |
| Gitto FX Saturator | Saturation | 5 styles, 4x oversampled, tone shaping before and after, level-matched auto gain |
| Gitto FX Distortion | 4-band distortion | 12 styles per band with drive, dynamics, feedback, tone and mix; 4x oversampled |
| Gitto FX Tape | Tape machine | 3 speeds, 3 formulas, bias, head bump, wow, flutter, hiss |
| Gitto FX Plate | Plate reverb | 6 plate types, decay 0.3 to 20 s, damping, modulation |
| Gitto FX Space | Ambient reverb and echo | 8 modes from smeared delays to endless washes, tempo sync, freeze |
| Gitto FX Tune | Pitch correction | Key and scale, retune speed, humanize, formant shift, transpose (single voices only) |
| Gitto FX Shaper | Rhythmic shaper | Drawable curve driving volume, filter, pan and width; tempo sync or triggered by audio |

Formats: VST3 and Audio Unit on macOS (Apple silicon and Intel, macOS 10.13+), VST3 on Windows and Linux.

## Getting installable plugins

Every push to `main` builds the plugins on GitHub. Open the **Actions** tab, pick the latest
"Build Gitto FX" run, and download **GittoFX-macOS** from the Artifacts section. Unzip it and follow
`READ-ME-FIRST.txt`.

Pushing a tag such as `v0.1.0` also attaches the zip to a GitHub Release.

A full build of all twenty plugins takes a while (each one compiles its own copy of JUCE). On a
public repository GitHub's Mac build machines are free; on a private one they count against the
monthly allowance at ten times the normal rate, so a private repository only gets a few builds a
month on the free plan.

## Building on your own machine

Needs CMake 3.22+ and a C++17 compiler (Xcode on macOS, Visual Studio 2022 on Windows).
JUCE 8.0.9 is downloaded automatically.

```bash
# macOS, universal binary, installed straight into ~/Library/Audio/Plug-Ins
cmake -B build -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" -DGITTO_COPY_AFTER_BUILD=ON
cmake --build build --config Release --parallel
```

## Layout

```
dsp/        Plain C++17 signal processing, no framework dependency
shared/     Gitto.h: look and feel, parameter helpers, processor and editor base classes
plugins/    One folder per plugin: parameters, presets, display, layout
tests/      dsp_tests.cpp and dsp_tests2.cpp (offline checks), Preview.cpp (screenshots and offline rendering)
installer/  macOS install script that ships in the download
```

Adding a plugin: write its DSP class in `dsp/`, add `plugins/<Name>/Plugin.cpp` following an
existing one, and add a `gitto_add_plugin(...)` line to `CMakeLists.txt`.

## Tests

```bash
c++ -std=c++17 -O2 tests/dsp_tests.cpp -o dsp_tests && ./dsp_tests
c++ -std=c++17 -O2 tests/dsp_tests2.cpp -o dsp_tests2 && ./dsp_tests2
```

Covers filter accuracy against analog curves, compressor ratios and timing, the limiter's true-peak
ceiling against an independent 16x meter, loudness meter calibration, reverb decay times, delay
timing, and stability of every mode at extreme settings. The second file covers the other fourteen:
gain-reduction amounts and timing for the classic compressors, flat band-splitting, aliasing of the
oversampled saturators, plate decay times, pitch-correction accuracy in cents, and more.

## Levels and presets

Presets are set up on tracks peaking around -6 dBFS and are level-matched there: switching a
preset in should change the sound, not the volume (limiter presets and the ones named for loudness
are the exceptions). On much hotter or quieter material the threshold-based ones will do more or
less than intended; move Threshold or Input to taste. FET Comp's fixed threshold sits at -12 dBFS
with Input at 0 dB.

## Licensing notes

The plugin code in this repository belongs to JG Productions. It is built with
[JUCE](https://juce.com), which is dual-licensed: free under AGPLv3 (the source of anything you
distribute must then be public) or under a commercial JUCE licence. JUCE's free Starter tier covers
revenue up to its published limit; check JUCE's current terms before selling binaries.

The VST3 format comes from Steinberg's VST3 SDK (bundled with JUCE), which is licensed under GPLv3
or Steinberg's own VST3 licence. Selling closed-source VST3 plugins means signing Steinberg's free
VST3 licence agreement first. None of this matters for using the plugins in your own sessions.

"VST" is a trademark of Steinberg Media Technologies GmbH. Product names mentioned in the
specifications are trademarks of their owners and are used only for comparison; Gitto FX shares no
code, artwork or presets with them.
