# Secant

## Use
https://vi-control.net/community/threads/beta-secant-free-batch-negative-track-delay-finder.175872

## Build

```bash
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 16
open build/Secant_artefacts/Release/Secant.app
```

JUCE 8.0.10 is not tracked in this repo; clone it into `third_party/JUCE` before configuring:

```bash
git clone --depth 1 --branch 8.0.10 https://github.com/juce-framework/JUCE.git third_party/JUCE
```

Tagging `v*` builds macOS (universal) and Windows releases via GitHub Actions.


## License

Secant is licensed under the [GNU Affero General Public License v3.0](LICENSE).
