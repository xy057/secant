# Secant

Hosts a VST3 instrument (built for SINE Player / Orchestral Tools Berlin series) and measures the
**transient point** (the negative delay you need) of every articulation × note × dynamic layer ×
round robin, with sub-sample precision. Everything is exported as JSON.

## Build

```bash
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 16
open build/Secant_artefacts/Release/Secant.app
```

JUCE 8.0.10 is vendored in `third_party/JUCE`. Tests:

```bash
./build/OnsetAnalyzerTest_artefacts/Release/OnsetAnalyzerTest
./build/PipelineSmokeTest_artefacts/Release/PipelineSmokeTest --fake out.json 3
```

Probes for investigating a player (saved `.pluginstate` from a run):

```bash
./build/RRProbe_artefacts/Release/RRProbe "<plugin.vst3>" "<state>" <keyswitch> <note>
./build/LegatoProbe_artefacts/Release/LegatoProbe "<plugin.vst3>" "<state>" <keyswitch> <noteA> <noteB> [velocities] [gapsMs] [cc1]
```

## Workflow

1. **Plugin**: choose `SINE Player` → *Load* → the editor opens. Load the instrument you want to
   measure. Use **one mic position**, because every worker loads its own copy of the samples.
2. **Preset**: *Load Preset JSON…* → `presets/berlin/*.json` → choose the same instrument.
3. **Start**. With SINE Player the tool reads SINE's own configuration from the plugin state (see
   *What SINE's state provides*): keyswitches, dynamic layers, velocity vs. mod-wheel dynamics, round-robin
   counts and the legato zone grid. Where the preset (docs) disagrees, SINE wins and the run log / JSON
   `warnings` say so. The keyswitch base (articulation *n* = base + n − 1) is only the fallback for other
   players; double-click a Keyswitch cell to override one articulation.

The configured plugin state is cloned into *N* worker instances that render offline in parallel.
Each worker waits until its copy actually produces sound before it starts. Results are auto-saved
to `~/Documents/Secant/Results/`.

## How a hit is measured

For each hit the worker sends the keyswitch, then CC1, then note-on at an exact sample, and captures
10 ms before the note to check for bleed. It holds the note for the category's capture length,
sends note-off and all-sound-off, and waits for digital silence before the next hit.

All methods are stored for every hit, in ms relative to note-on with plugin latency subtracted:

| key | definition |
|---|---|
| `first` | first sample above −80 dBFS (sample-accurate) |
| `rel-40` … `rel-6` | Hilbert envelope first crosses N dB below its peak |
| `maxSlope` | steepest rise of the envelope |
| **`attackEnd`** | **end of attack / start of decay**: the envelope first comes within 1 dB of the attack peak |
| `attackPeak` | the attack's first maximum: either decay begins (−1 dB) or the level settles (< 1 dB rise over 40 ms) |
| `peak` | global envelope peak in the window (a late swell can land here, which is why `attackPeak` exists) |

The **primary** method is `attackEnd` for shorts, longs and phrases. That is the moment that should land
on the grid, so the **negative track delay = −attackEnd** (`suggestedNegativeDelayMs`,
`negativeDelayMsTable`). Crescendos and swells use `first`, because their "end of attack" is the end
of the swell. The attack envelope is smoothed over at least one full period of the played note,
which cancels harmonic beating that would otherwise look like an early decay.

**Round-robin discovery.** These SINE behaviours were measured with `RRProbe` on Berlin Symphonic
Strings, and the tool is built around them:

| SINE behaviour | How Secant handles it |
|---|---|
| RR counter is per instance and moves forward with every note | **one job = one note**: one worker plays every articulation and layer of it in order |
| gap between note-ons ≤ 1.2 s → cycles · ~1.5–2.5 s → restarts at the cycle's first sample · > 2.5 s → after-pause sample | each series starts after a **3 s reset pause**; hits are then spaced **1.0 s (short) / 1.2 s (long)**, capped at 1.2 s |
| the after-pause sample is usually a cycle recording started **3–4 samples (0.06–0.08 ms) early** | shift-tolerant matching (±4 samples, identical peak level), reported as `afterPauseIsCycleSample` / `afterPauseStartsEarlierMs` |
| all-sound-off (CC120) is ignored, and CC123 can make the next note drop | tails are never cut. CC123 is never sent |
| release tails reach the next onset (long notes, low short notes) | **exact tail subtraction**: render(notes 0..k) − render(notes 0..k−1) = note k alone (deterministic playback). Always used for long and dynamic articulations; used for short and phrase series when a tail masked an onset |
| the occasional note comes out silent, and the cycle restarts after it | the series is restarted (up to 2 retries, `retries` in the results) |
| some articulations pick RRs in random order | the series stops once no new sample appears, and is reported as `randomOrder` with its sample set |
| the same recording is sometimes replayed with a slightly different gain | same waveform (> 0.999) and same end of attack (±0.1 ms) count as the same recording, whatever the gain |
| different RRs of a sustained note can correlate > 0.995 | a waveform match also needs the same peak level (±0.1 dB) |
| some articulations switch dynamic layers on velocity, others on CC1 | **auto-detected** per articulation. A normalised waveform comparison ignores pure gain changes, so only a real layer change counts. Layers that turn out identical are flagged |

Repeated hits find the cycle, and *RR confirm* more hits (default 2) must line up with it.
Each distinct sample is measured once. Hits whose onset is masked by a tail are kept for identifying
samples but left out of the statistics (`tailContaminated`).

**Fast repetition** (short articulations): an extra 250 ms series, tail-subtracted. On Violins I it
plays the same RR set as normal spacing.

**RR prior.** SINE's state says how many round robins an articulation has. A hit that lands exactly one
cycle after an earlier hit is accepted as that recording under relaxed criteria (waveform > 0.98, level
within 1 dB, end of attack within 1 ms, or waveform > 0.99 and level within 0.3 dB): a choked voice,
a small level variation or an end-of-attack estimate flipping between two near-equal bumps no longer
splits one recording into two (`priorMatch` on the hit). Zero-lag and shifted comparisons are judged
separately, so a marginally better shifted alignment can't hide an exact repeat.

## Legato / portamento transitions

Legato articulations (preset `legato`, or legato on in SINE) get a second measurement: how long after
the note-on the **new pitch is established**. That is the negative delay for a legato note.

**What SINE's state provides.** Every legato articulation has a grid of *velocity zones* × *speed zones*,
each cell mapped to a legato type (Berlin Symphonic Strings: *Melodic Legato* for note-on spacing
≥ 120 ms, *Rapid Legato* below; Berlin Strings / First Chairs: velocity-triggered portamento etc.).
Speed zones are note-on to note-on spacing in ms (verified: the switch sits between 117.3 and 122.7 ms
block-aligned spacing). SINE's *adaptive legato* also scales the transition with the spacing, so every
transition is measured at several spacings (*Note spacing*: standard 100 / 250 / 500 / 1000 ms, full
sweep, or slow only), and every speed zone always gets at least one spacing inside it. The velocity sent
is the middle of each velocity zone (100 for an unrestricted zone); dynamic layers go on CC1.

**How it is rendered.** A legato *chain* A B A B … at the cell's spacing, from SINE's reset state, each
note-on overlapping the previous note by 10 ms. Transition *k* is measured on render(notes 0..k) against
render(notes 0..k−1): the two are identical up to note *k* (verified per transition, `referenceExact`),
so everything after it is note *k*'s transition. A chain gives both directions (A→B and B→A). With
round robins (Pattern legato: 3) SINE's counter is global and also cycles the transitions, so chains run
until every RR slot has played once in each direction: 2R transitions for an odd count; for an even count
R transitions, plus a second chain starting on the other note. Each transition needs its own render of
the chain up to it, so the work grows with the square of the chain length. That is why the chains are
as short as possible; *legatoRRConfirm* adds whole extra cycles. Distinct recordings are listed per
cell (`rr`). Progress moves after every chain render, not only when a legato job finishes, and the ETA line counts
legato transitions live (`legato N/M`). Every reset pause (note series, tail subtraction and legato chains) is at least SINE's own RR reset time
for that articulation (`resetAfterMS` + 250 ms). Berlin's runs use 4000 ms, so a shorter pause leaves
the RR counter running and no two renders are identical.

**RR reset** (Settings): "As set in SINE" (default) reads each articulation's reset time from the player,
so changing it in SINE (Round Robin › Reset after) is picked up by the next run. Choosing 1000–3000 ms
writes that time into the workers' copy of the state for every articulation (your open instance is
unchanged; `render.rrResetOverrideMs`) and shrinks the reset pauses to it. Note series give the same
results. In legato chains of short-sounding patches, a note spacing longer than the reset lets the counter
reset between notes, as SINE then plays it. Example: Berlin runs at a 1000 ms reset and 1000 ms spacing
reach 1–2 of their 6 transition recordings. Speed gain is small, because silence renders quickly.
`SineStateTool <state> [--json | --rr-reset <ms>]` shows or tests the patch.

"Playable Runs" and "Runs" are single notes with round robins. Their samples are short, but their
dynamics follow CC1, so they are measured as longs. Their "+ Legato" / "+ LEG" versions are regular
legato patches (measured like Sustains + Legato). "Spiccato Serial" and "Pizzicato Serial" are shorts. Only what one key press plays as several notes or a gesture is a
phrase: repetitions, doubles and triples, tonguing, octave scale runs, glissandi and SFX.

SINE behaviour this depends on (measured with `LegatoProbe`):

| SINE behaviour | handling |
|---|---|
| the first play of a streamed transition sample differs (cold cache), and now and then a note is dropped (a render that takes ~5× longer) | each chain length is rendered **once**: the next, longer render must repeat it bit for bit up to its new note, so that prefix comparison is the check (`referenceExact`). Only on a mismatch are both lengths re-rendered until two consecutive renders are **bit-identical**; the transition measured on the replaced render is then measured again (`renders`, `converged`). The last render of a chain has no longer one after it and is always rendered until identical |
| after a CC1 change, renders are not repeatable (and the first notes can come out silent) until the keyswitch is sent again | the keyswitch + CC1 are re-sent before **every** legato render |
| release samples of the previous note keep sounding under the new note | the analysis decides by dominance, not presence |
| at short spacings a transition may not have arrived when the next note-on comes | such transitions are reported with the reason *source not sounding at the note-on* and left out of tables / statistics |

**How it is measured** (`Source/LegatoAnalyzer.cpp`, ground-truth tests in `OnsetAnalyzerTest` and
`PipelineSmokeTest --fake`):

1. A pitch track (YIN, 0.5 ms hop) plus, per frame, how well the waveform fits the source note's period
   and the target note's period. Note bands are centred on the recording's measured intonation and widened
   for vibrato.
2. **Glide or jump?** The pitch track is fitted with a ramp model (source pitch → linear glide in cents →
   target pitch, averaged over each frame's analysis window, so the window's blur is undone; the start
   level is fitted too, for interrupted glides). A glide that explains the track clearly better than an
   instant change is a portamento: *start* / *arrive* = where the fitted glide leaves the source band /
   enters the target band, *mid* = its middle.
3. **Abrupt changes** (slurs, re-bows): one-period residuals at the two known periods are classified on a
   0.25 ms grid (a note counts where its period fits clearly better than the other's; octaves by
   dominance, with the boundary measured per recording; the pitch track vetoes contradictions). The change
   point is then located by least squares on per-sample residuals (source-period misfit before *J* +
   target-period misfit after *J*, gain-compensated when the level moves) — sample-precise. With noise
   between (a bow change), *start* = the source's end and *arrive* = the target's start, each by least
   squares against the noise level.

Accuracy on synthetic transitions with known truth (C0 … C5, ±1 … 12 semitones, both directions):

| transition | error |
|---|---|
| slur (abrupt pitch change, phase-continuous) | ≤ 0.07 ms |
| slur with a 6 dB bow-change dip (pipeline test) | ≤ 2.7 ms |
| re-bow with 40 ms of noise + the source's release at −12 dB underneath | ≤ 1.3 ms (arrive) |
| slur onto a ±25 cent vibrato | ≤ 0.9 ms |
| glide, C1 and up | ≤ 2.5 ms (≤ 1 ms from C2) |
| glide at C0 (33 Hz) | ≤ 0.45 period — a pitch needs ~2 periods (60 ms) to exist at all |

**Methods** (all exported, ms after the note-on): `arrive` (primary by default: the target pitch is
established for good), `mid`, `start` (the source pitch ends for good), `diverge` (first sample that
differs from the render without the transition), `diffAttackEnd` (end of attack of the difference
signal — the measure for unison / repeated-note legato). *Legato point* in the GUI picks the primary.

**JSON** (`articulations[].legato`): `zones`, `cells`, `summary` (type → spacing → method statistics),
`medianByInterval`, `suggestedNegativeDelayMs` (type → spacing), and

`negativeDelayMsTable`: `type → dynamic → spacing ms → source note → "+interval" → { first, rr[ ], median }`

(`first` = the first transition of that direction in its chain, closest to a phrase start; `rr` = the
distinct recordings in play order), plus every raw transition with all methods, `chainIndex`, `rrSlot`,
`renders`, `failure`. The top-level `player` object lists SINE's articulations as read from the state.

**`negativeDelayMsTable`**: `note → dynamic → { afterPause, cycle[ ], fastRepetition{ distinct[ ] } }`,
with negative delays in ms using the primary method (`attackEnd`).

### Accuracy

The envelope is the magnitude of the analytic signal (Hilbert transform). It is smoothed over a
fraction of the played note's fundamental period ("Envelope smoothing", default ½ period), which
cancels harmonic beating so the result doesn't depend on waveform phase. The test suite shows:

- pure tones ≥ 440 Hz: ≤ 0.02 ms absolute error and phase jitter (raw Hilbert)
- sub-sample shifts are tracked to < 0.1 sample
- harmonic-rich tones below ~150 Hz: envelope onsets are only defined to a fraction of the fundamental
  period. This is a physical limit, not a tool limit. Compare `first` and the median of many hits.

## Compact delays export

Every saved or exported results file `X.json` gets a sibling `X.delays.json` with only the
delays, for programs that apply them:

```json
{ "format": "secant.delays", "version": 1, "noteKeys": "MIDI note number (C3 = 60)",
  "articulations": [
    { "name": "Staccato", "keyswitch": 35, "dynamicsSource": "velocity", "roundRobins": 7,
      "dynamics": [ { "name": "p", "velocity": 32 }, ... ],
      "notes": { "55": { "p": [-22.42, -31.66, ...], "mf": [...] }, ... },
      "afterPause": { "55": { "p": -18.3 } },
      "legato": { "Fingered": { "velocity": [0, 99], "byNoteSpacingMs": { "250.7": -198.69 } } } } ] }
```

- `notes[note][dynamic]` lists one negative delay (ms, 0.01 ms precision) per round robin, in the
  order the player cycles through them.
- `afterPause` appears only where the first note of a phrase plays a sample outside that cycle.
- `legato` gives the median transition delay per legato type and note spacing.

To convert older results files:

```bash
build/DelayExport_artefacts/Release/DelayExport ~/Documents/Secant/Results/*.json
```

## Other players (Kontakt, ...)

Secant works with any VST3 instrument. When the loaded plugin is not SINE Player:

- **What is switched off.** The SINE-only parts don't apply: reading the player's articulation list,
  layers, RR counts and legato zones, and setting its RR reset time. The preset's data is used instead.
- **RR reset.** This is how long a pause it takes before the player goes back to its first round robin.
  For other players it switches to **Never**: the round robin never resets automatically and keeps
  cycling (sequential) or picks at random, however long the pause. That is common in Kontakt. If the
  player does go back to its first round robin after a pause, choose that time; Secant then waits that
  long before each series.
- **What Never changes:**
  - Series notes follow each other once the previous note is silent, so there is no tail to remove.
  - No fast-repetition series, because it needs replays from a reset state.
  - Dynamic layers are sent as velocity unless a row's Dyn src says CC1.
  - Legato is still measured. With round-robin transitions it can't replay identically, and those
    transitions are flagged.
- **Keyswitches.** By default an articulation's keyswitch is KS base + # − 1. Double-click a row's KS
  cell to set any note; it is saved with the preset (`"keyswitch": "C-1"`).

**Preset editor.**

| Control | What it does |
|---|---|
| **New** | Starts an empty preset |
| **Load…** | Loads a preset |
| **Save…** | Writes `secant.preset` JSON |
| Instrument **+ / −** | Adds or removes an instrument |
| Typing in the instrument box | Renames the selected instrument |
| Table **+ / −** | Adds an articulation after the selected row, or removes the selected rows (Cmd/Shift-click for several) |
| Double-click a cell | Edits it: Articulation (name), Range (`C2-C6, D7` or `36-96`), Dynamics (`pp mf ff`), RR, KS |
| Leg, Category, Dyn src | Edited in place |

Removing a row leaves the other rows' # (keyswitch slot) unchanged. An edited preset shows `*` until
it is saved.

## Presets

`presets/berlin/*.json` are generated from the Orchestral Tools help pages:

```bash
python3 tools/ot_docs/parse_ot_docs.py
```

The parser takes the saved pages in `tools/ot_docs/source/`. It uses the OT note convention
(C3 = 60), keeps the documentation's own numbering in `docNumber`, and uses the position in the
list (`index`) for keyswitches. Errors in the docs are never silently fixed. They are listed per
articulation in `issues`:
Flute 3 numbering skips 11, a corrupted Oboe 1 row, Timbales listing 8 layers but naming 6,
reversed cymbal ranges, ranges with a missing octave in the Violin scale runs, and more.
Legato articulations are measured twice: their first-note attack like any long note, and their transitions (see *Legato / portamento transitions*).
