# Secant

Measures the transient point (the negative track delay) of every articulation × note × dynamic layer ×
round robin of a VST3 instrument, with sub-sample precision. Works with any VST3 player; with SINE Player
(Orchestral Tools) it also reads the player's own configuration. Results are JSON.

## Build

```bash
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 16
open build/Secant_artefacts/Release/Secant.app
```

JUCE 8.0.10 is vendored in `third_party/JUCE`.

## Use

1. **Plugin**: load the VST3 player (SINE Player, Kontakt, ...) and the instrument, with one mic position.
2. **Preset**: *Open…* a file from `presets/` and choose the same instrument.
3. **Start**.

Results go to `~/Documents/Secant/Results/`, each with a compact `*.delays.json` beside it.

## More

- [PRESET.md](PRESET.md): writing a preset for another library
- [docs/DETAILS.md](docs/DETAILS.md): measurement methods, player behaviour (SINE), legato, export format, other players, tests and probes
