# Writing a Secant instrument preset

This guide is for an AI agent (or a person) writing a preset JSON for a sample library that has no
preset in `presets/` yet. A preset tells Secant which articulations an instrument has, which keys
select them, which notes they cover, their dynamic layers and their round robins. Secant then plays
every articulation × note × layer × round robin and measures its transient.

The loader is `Collection::load` in `Source/PresetModel.cpp`. When this guide and the code disagree,
the code is right.

## Where the facts come from

Use the vendor's documentation (articulation list, ranges, layers, RR counts) or a screenshot or
description of the player's articulation list. **Do not invent values.** When the source is missing
something or contradicts itself, use the most likely value and say what you did in that
articulation's `issues`. Never fix a docs error silently.

With SINE Player, Secant reads keyswitches, layers, RR counts, legato and playable range from the
player at run time, and they override the preset. Wrong values there only cause warnings. With any
other player (Kontakt, ...) the preset is all Secant knows, so every value has to be right.

## File layout

Save as `presets/<vendor>/<vendor>_<collection>.json`, lower case, words joined by `_`. Use UTF-8 and
2-space indentation.

```json
{
  "format": "secant.preset",
  "version": 1,
  "vendor": "Orchestral Tools",
  "collection": "Berlin Strings",
  "player": "SINE Player",
  "noteConvention": "C3=60",
  "source": "https://example.com/where-the-data-came-from",
  "keyswitchBase": "C0",
  "instruments": [
    {
      "name": "Violins 1",
      "group": "Violins 1",
      "articulations": [
        {
          "index": 1,
          "docNumber": 1,
          "name": "Sustains + Legato",
          "category": "long",
          "legato": true,
          "ranges": [ { "from": "G2", "to": "C7", "lo": 55, "hi": 108 } ],
          "dynamics": [ "p", "mf", "f" ],
          "roundRobins": 1,
          "issues": []
        },
        {
          "index": 2,
          "docNumber": 2,
          "name": "Spiccato",
          "category": "short",
          "legato": false,
          "ranges": [ { "from": "G2", "to": "C7", "lo": 55, "hi": 108 } ],
          "dynamics": [ "pp", "mf", "ff" ],
          "roundRobins": 7,
          "issues": []
        }
      ]
    }
  ]
}
```

### Top level

| field | required | meaning |
|---|---|---|
| `format` | **yes** | always `"secant.preset"`. Any other value makes the file fail to load |
| `version` | yes | `1` |
| `vendor` | yes | library maker |
| `collection` | yes | product name. The GUI shows it |
| `player` | yes | `"SINE Player"`, `"Kontakt"`, ... |
| `noteConvention` | yes | always `"C3=60"` (informational: the loader always uses C3 = 60) |
| `source` | yes | URL or description of where the data came from |
| `keyswitchBase` | if keyswitches are a grid | note of articulation `index` 1's keyswitch, e.g. `"C0"`. `""` if unknown |
| `instruments` | **yes** | at least one, or the file fails to load |

### Instrument

| field | required | meaning |
|---|---|---|
| `name` | yes | the patch name as the player shows it, e.g. `"Violins 1"` |
| `group` | no | section heading in the docs. When it differs from `name`, the GUI shows `group / name` |
| `articulations` | yes | in the player's own list order |

### Articulation

| field | required | meaning |
|---|---|---|
| `index` | **yes** | 1-based position in the player's articulation list. Keyswitch = `keyswitchBase + index − 1`. Count **every** slot the player has, including ones you leave out |
| `docNumber` | no | the number printed in the docs (may differ from `index`, or be `null`) |
| `name` | **yes** | exactly as the player shows it. With SINE it is the fallback match when the keyswitch doesn't fit |
| `category` | **yes** | `short`, `long`, `dynamic` or `phrase` (see below). An unknown value reads as `short` |
| `legato` | yes | `true` if playing overlapping notes makes a legato transition |
| `ranges` | **yes** | list of `{ "lo", "hi" }` MIDI note numbers, inclusive. `from`/`to` are optional note names for readers; **the loader only reads `lo`/`hi`**. `[]` = unplayable: the row starts unselected |
| `dynamics` | **yes** | layer names from soft to loud, e.g. `["pp","mf","ff"]`. One entry per recorded layer. Missing = `["mf"]` |
| `roundRobins` | yes | RR samples per note and layer. Minimum 1 |
| `keyswitch` | no | explicit keyswitch as a note name (`"C-1"`) or MIDI number. Use it when keyswitches are not a contiguous grid. Overrides `keyswitchBase + index − 1` |
| `dynamicsSource` | no | `"velocity"` or `"cc1"`: what switches the layers. Leave it out to auto-detect at run time (recommended unless the docs say so) |
| `issues` | yes | list of strings: every doubt, guess or docs contradiction about this articulation. `[]` if none |

Other fields are kept when the GUI saves the file (the Berlin presets carry `legatoTypes`,
`variations`, `transitions`, `tempoMeasured`, `dynamicCountDocs` from the docs), but Secant does not
use them.

## Notes: C3 = 60

Use the Yamaha / Orchestral Tools convention: **middle C (MIDI 60) is C3**, so MIDI 0 is C-2.

| name | C-2 | C-1 | C0 | C1 | C2 | C3 | C4 | C5 | C6 | C7 | G8 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| MIDI | 0 | 12 | 24 | 36 | 48 | 60 | 72 | 84 | 96 | 108 | 127 |

`midi = (octave + 2) × 12 + pitch class` (C=0, C#=1, D=2, … B=11). Sharps are written `#` and
flats `b` (`Bb2` = 46).

**Check the source's convention.** Kontakt, Cubase, Logic and Ableton also write middle C as
**C3**. Scientific pitch notation (Pro Tools, notation programs, many manuals) writes it as **C4**:
subtract one octave (12) from those note names. FL Studio writes it as C5. A sure test: the docs'
lowest note of a known instrument (violin G below middle C = G2 here). If you can't tell, say so in
`issues`.

## Category

The category sets how long each hit is captured and which onset method is the primary one.

| category | what it is | examples |
|---|---|---|
| `short` | one short, detached note per key press | staccato, spiccato, pizzicato, marcato short, "Spiccato Serial", single percussion hits |
| `long` | a sustained note (also the default) | sustain, tremolo, trill, flautando, harmonics long, marcato long, sul pont, rolls, "Playable Runs" |
| `dynamic` | a note with a built-in level change: its "attack end" is the end of the swell, so `first` is measured | crescendo, decrescendo, diminuendo, swell, fortepiano, sforzando-crescendo |
| `phrase` | one key press plays **several notes or a gesture** | repetitions, doubles / triples, double / triple tonguing, scale runs ("C maj"), glissandi, SFX |

Decision order (the same as `classify()` in `tools/ot_docs/parse_ot_docs.py`):

1. crescendo / decrescendo / diminuendo / swell / fortepiano → `dynamic`
2. "Playable Runs" / "Runs" (single notes made for fast lines, dynamics on CC1) → `long`
3. "Serial" (single short notes recorded in series) → `short`
4. repetitions, `maj` scale runs, doubles, triples, tonguing, glissando → `phrase`
5. staccato, spiccato, pizzicato, marcato, sforzando, martelé, portato short, hits, … that don't
   also say sustain / tremolo / trill / roll / long → `short`
6. everything else → `long`

`"Sustains + Legato"`, `"Legato"`, `"+ Leg"` → `long` with `legato: true`. Its first-note attack is
measured like any long note, and its transitions are measured as well.

## Keyswitches

- **Contiguous grid** (SINE, most Kontakt multis): set `keyswitchBase` to articulation 1's key and
  number `index` 1, 2, 3, … in the player's list order. If the player has an empty slot or an
  articulation you leave out, skip its `index` so the following ones still land on the right key.
- **No grid** (scattered keys, UACC, a different scheme per patch): set `keyswitch` on every articulation.
  `index` then only orders the rows.
- **No keyswitches** (one patch per articulation): one instrument per patch, with one articulation
  each. The user then unticks *Use keyswitches* in the GUI.

## Dynamics and round robins

- `dynamics` lists layers, **not** dynamic markings. If the docs say "3 layers (pp, mf, ff)", write
  `["pp","mf","ff"]`. If they give only a count, write placeholder names (`["L1","L2","L3"]`) and add
  an issue.
- If the count and the names in the docs disagree, use the names and record the difference in
  `issues`.
- `roundRobins` counts per note and per layer. If the docs say "RR: none" or say nothing, write `1`.
  If the round robins play in random order, write the count anyway and add an issue.

## Ranges

- Use the **playable** range of that articulation, not the instrument's written range.
- Gaps and single notes are separate entries:
  `[{"lo":36,"hi":60},{"lo":64,"hi":64}]`.
- Percussion: the mapped keys (one entry per key or block). One key can be a different
  instrument, so split instruments if their articulations differ.
- Never let a range include the keyswitch zone.

## Checklist before you finish

1. `format` is `secant.preset` and the JSON parses.
2. Every articulation has `index`, `name`, `category`, `ranges` with `lo` ≤ `hi` in 0–127,
   `dynamics` (≥ 1), `roundRobins` (≥ 1), `issues`.
3. Note names and `lo`/`hi` agree (C3 = 60), and you checked the source's octave convention.
4. `keyswitchBase + index − 1` (or `keyswitch`) gives the key the docs list for at least the
   first and last articulation. No keyswitch falls inside a playable range.
5. Every guess is written in `issues`.

Quick self-check (only reads the file):

```bash
python3 - presets/<vendor>/<file>.json <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["format"] == "secant.preset" and d["instruments"], "format / instruments"
for inst in d["instruments"]:
    seen = set()
    for a in inst["articulations"]:
        tag = f'{inst["name"]} #{a["index"]} {a["name"]}'
        assert a["category"] in ("short", "long", "dynamic", "phrase"), tag + ": category"
        assert all(0 <= r["lo"] <= r["hi"] <= 127 for r in a["ranges"]), tag + ": range"
        assert a["dynamics"] and a["roundRobins"] >= 1, tag + ": layers / RR"
        assert a["index"] not in seen, tag + ": duplicate index"
        seen.add(a["index"])
print("ok")
PY
```

Then load it in Secant (*Preset › Open…*), choose the instrument and check the table: Range, Dynamics,
RR and KS should match the docs. Edits made there are written back by *Save… › Save Preset…*. With
SINE Player loaded, *Save… › Save with SINE Settings…* writes the player's keyswitches, layers,
dyn src, RR counts, legato flags and playable ranges into the selected instrument, then saves.
Use it to correct a preset written from incomplete docs.
