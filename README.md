# Secant

Measures the transient point (the negative track delay) of every articulation × note × dynamic layer ×
round robin of a VST3 instrument, with sub-sample precision. Built for SINE Player / Orchestral Tools
Berlin, works with any VST3 player. Results are JSON.

## Build

```bash
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 16
open build/Secant_artefacts/Release/Secant.app
```

JUCE 8.0.10 is vendored in `third_party/JUCE`.

## Use

1. **Plugin**: load SINE Player (or another VST3) and the instrument, with one mic position.
2. **Preset**: *Load…* a file from `presets/` and choose the same instrument.
3. **Start**.

Results go to `~/Documents/Secant/Results/`, each with a compact `*.delays.json` beside it.

## More

- [PRESET.md](PRESET.md): writing a preset for another library
- [docs/DETAILS.md](docs/DETAILS.md): measurement methods, SINE behaviour, legato, export format, other players, tests and probes
