#!/usr/bin/env python3
"""
Parse Orchestral Tools "Collection Notes" help pages (saved HTML in ./source)
into instrument-preset JSON files consumed by the Secant app.

Hierarchy in the output:
    collection -> instruments[] (one SINE instrument = one keyswitch set)
               -> articulations[] (index = position in SINE's articulation list)

Doc conventions (OT uses C3 = MIDI 60, so A-1 = 21 = lowest piano key):
    | 05. Marcato long
    | R: G2-Eb6 | L: 4 (pp, mp, f, ff) | RR:2 | S: Legato, Runs

Nothing is silently "fixed": anything odd in the source is kept as-is where
possible and recorded in the articulation's `issues` list.

Usage:  python3 parse_ot_docs.py            (writes ../../presets/berlin/*.json)
"""
from __future__ import annotations
import html
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "source"
OUT = HERE.parent.parent / "presets" / "berlin"

BASE_URL = "https://orchestraltools.helpscoutdocs.com/article/"

# file stem -> (output file, collection name)
COLLECTIONS = {
    "411-berlin-strings-notes": ("berlin_strings.json", "Berlin Strings"),
    "368-berlin-strings-first-chairs-notes": ("berlin_strings_first_chairs.json", "Berlin Strings | First Chairs"),
    "399-berlin-symphonic-strings-notes": ("berlin_symphonic_strings.json", "Berlin Symphonic Strings"),
    "410-berlin-woodwinds-notes": ("berlin_woodwinds.json", "Berlin Woodwinds"),
    "353-berlin-woodwinds-additional-instruments-notes": ("berlin_woodwinds_additional.json", "Berlin Woodwinds | Additional Instruments"),
    "412-berlin-brass-notes": ("berlin_brass.json", "Berlin Brass"),
    "516-berlin-brass-additional-instruments": ("berlin_brass_additional.json", "Berlin Brass | Additional Instruments"),
    "413-berlin-percussion-notes": ("berlin_percussion.json", "Berlin Percussion"),
}

DYN_ORDER = ["ppp", "pp", "p", "mp", "mf", "f", "ff", "fff"]

# --------------------------------------------------------------------------- text

def html_to_lines(raw: str) -> list[str]:
    m = re.search(r'id="fullArticle"(.*?)<div class="articleFoot', raw, re.S)
    s = m.group(1) if m else raw
    s = re.sub(r"<(br|/p|/li|/tr|/h\d|/div)[^>]*>", "\n", s)
    s = re.sub(r"<t[dh][^>]*>", " | ", s)
    s = re.sub(r"<h(\d)[^>]*>", r"\n#H\1 ", s)
    s = re.sub(r"<[^>]+>", "", s)
    s = html.unescape(s).replace("\xa0", " ")
    lines = [l.strip() for l in s.split("\n")]
    lines = [l for l in lines if l and l.strip("| ")]
    if "Still need help?" in lines:
        lines = lines[: lines.index("Still need help?")]
    return lines

# --------------------------------------------------------------------------- notes

NOTE_RE = re.compile(r"([A-Ga-g])([#b]?)(-?\d)")
PC = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def note_to_midi(name: str) -> int | None:
    m = re.fullmatch(r"([A-Ga-g])([#b]?)(-?\d)", name.strip())
    if not m:
        return None
    letter, acc, octv = m.groups()
    v = PC[letter.upper()] + (1 if acc == "#" else -1 if acc == "b" else 0)
    return (int(octv) + 2) * 12 + v  # OT / Yamaha convention: C3 = 60


def parse_ranges(text: str, issues: list[str]):
    """'C1-D1, C2, F2-G2' -> [{'from':'C1','to':'D1','lo':24,'hi':26}, ...]"""
    out = []
    for part in [p.strip() for p in text.split(",") if p.strip()]:
        notes = NOTE_RE.findall(part)
        names = ["".join(n) for n in notes]
        if len(names) == 1 and re.fullmatch(r"\s*" + re.escape(names[0]) + r"\s*", part):
            lo = hi = note_to_midi(names[0])
            out.append({"from": names[0], "to": names[0], "lo": lo, "hi": hi})
        elif len(names) == 2:
            lo, hi = note_to_midi(names[0]), note_to_midi(names[1])
            if lo > hi:
                issues.append(f"range '{part}' is reversed in the docs; using {names[1]}-{names[0]}")
                lo, hi = hi, lo
            out.append({"from": names[0], "to": names[1], "lo": lo, "hi": hi})
        else:
            issues.append(f"could not parse range part '{part}'")
    return out


# --------------------------------------------------------------------------- articulation fields

def parse_fields(line: str, issues: list[str]) -> dict:
    """Parse '| R: G2-D6 | L: 3 (pp, mf, ff) | RR: 4 | S: ...' (also the
    pipe-less 'R:D3 - E5 | L:2 (mp, ff) RR:3' variant)."""
    body = line.lstrip("| ").strip()
    f: dict = {}

    m = re.search(r"R:\s*(.*?)\s*(?=\|\s*L:|L:|\|\s*RR|RR:|$)", body)
    if m:
        f["ranges"] = parse_ranges(m.group(1).strip(" |"), issues)

    m = re.search(r"L:\s*(\d+)\s*\(([^)]*)\)?", body)
    if m:
        count = int(m.group(1))
        names = [d for d in re.split(r"[,.\s]+", m.group(2)) if d]
        f["dynamics"] = names
        f["dynamicCountDocs"] = count
        if count != len(names):
            issues.append(f"docs say {count} dynamic layers but list {len(names)} ({', '.join(names)})")
    else:
        issues.append("no dynamic layer info in docs; assuming 1 layer")

    m = re.search(r"RR:\s*(\d+)", body)
    f["roundRobins"] = int(m.group(1)) if m else 1

    m = re.search(r"\bS:\s*([^|]+)", body)
    if m:
        f["legatoTypes"] = [s.strip() for s in m.group(1).split(",") if s.strip()]
    m = re.search(r"\bT:\s*([^|]+)", body)
    if m:
        f["transitions"] = m.group(1).strip()
    m = re.search(r"\bV:\s*([^|]+)", body)
    if m:
        f["variations"] = [s.strip() for s in m.group(1).split(",") if s.strip()]
    if re.search(r"\|\s*TM\b", body):
        f["tempoMeasured"] = True
    return f


# --------------------------------------------------------------------------- classification

def classify(name: str) -> str:
    """Category drives render length and which onset method is 'primary'."""
    n = name.lower()
    if re.search(r"crescendo|decrescendo|diminuendo|swell|\bdynamics\b|fortepiano", n):
        return "dynamic"
    # "Playable Runs" (and Berlin Brass Additional's plain "Runs") are single notes recorded for fast
    # lines, one sample per key with round robins - not a recorded phrase. The notes are short, but
    # their dynamics follow CC1 like a sustain, so they are measured as longs; "+ Legato" / "+ LEG"
    # is a regular legato patch (legato transitions measured like Sustains + Legato).
    if re.search(r"playable runs|^runs\b", n):
        return "long"
    # "... Serial" (spiccato / pizzicato recorded in series): single short notes with many RR.
    # Recorded phrases are what one key press plays as several notes or a gesture: repetitions,
    # doubles / triples, double / triple tongue, octave scale runs ("C maj"), glissandi, SFX.
    if re.search(r"\bserial\b", n):
        return "short"
    if re.search(r"repetition|\bmaj\b|doubles|triples|triplets|tongue|glissand|scratches|thunder", n):
        return "phrase"
    if re.search(r"staccat|spiccato|pizz|short|marcato|single hits|hits|sforzando|martele|stopped|muted|"
                 r"portato|accented|bold|metal|glass|bamboo|blocks|anvil|claves|castanets|shakers|guiro|"
                 r"body percussion|bells?\b|cymbals|horns?\b|drumsticks|vibraslap|call|cuckoo|staccatissimo", n) \
            and not re.search(r"sustain|tremolo|trill|roll|long", n):
        return "short"
    return "long"


def is_legato(name: str) -> bool:
    n = name.lower()
    return bool(re.search(r"legato|\+\s*leg\b|\bleg\b", n))


# --------------------------------------------------------------------------- page walker

ART_NUM_RE = re.compile(r"^(\d{1,2})\s*\.?\s*(\D.*)$")       # 05. Marcato long | 17 Repetitions | 05.E maj
INST_RE = re.compile(r"^(\d{2})\.\s+(.+)$")                   # 01. Trumpet 1
GROUP_PAREN_RE = re.compile(r"^(\d+)\)\s*(.+)$")              # 1) Piccolo Trumpet
DATA_RE = re.compile(r"^\|?\s*R:")
# OT copy/paste glitch: '| R: ... | S: Legato, Runs 16. Trills Single' (header glued to stale data)
GLUED_HEADER_RE = re.compile(r"\s(\d{2})\.\s+([A-Z][^|]*)$")


def walk(lines: list[str], collection: str):
    """Rule: the line directly above an `R:` data line is an articulation header.
    Any other non-cell line is an instrument / group header."""
    instruments = []
    group = None
    inst = None

    def new_inst(name, grp):
        nonlocal inst
        inst = {"name": name.strip(), "group": grp, "articulations": []}
        instruments.append(inst)

    def is_data(j):
        return 0 <= j < len(lines) and bool(DATA_RE.match(lines[j]))

    def header_follows(j):
        """line j is an articulation header (or the glued-header glitch)"""
        return is_data(j + 1) or (is_data(j) and GLUED_HEADER_RE.search(lines[j]) and is_data(j + 1))

    started = False
    pending = None  # (docNumber or None, name, [issues])
    for i, line in enumerate(lines):
        if line.startswith("#H1"):
            started = True
            continue
        if not started:
            continue

        if line.startswith("#H4") or line.startswith("#H5"):
            title = re.sub(r"^\d+\.\s*", "", line[3:].strip())
            if not title or title.startswith("Berlin"):   # changelog headers / empty
                continue
            group = title
            inst = None
            if header_follows(i + 1):                      # First Chairs / BWW-AI style
                new_inst(title, group)
            continue

        if is_data(i):
            glued = GLUED_HEADER_RE.search(line)
            if glued and is_data(i + 1):
                # the data on this line belongs to nothing - it is the next row's header
                pending = (int(glued.group(1)), glued.group(2).strip(),
                           [f"docs row is corrupted ('{line.strip('| ')}'); header recovered, stale data ignored"])
                continue
            if pending is None or inst is None:
                print(f"[{collection}] orphan data line: {line}", file=sys.stderr)
                continue
            doc_no, name, issues = pending
            fields = parse_fields(line, issues)
            pos = len(inst["articulations"]) + 1
            if doc_no is None:
                issues.append("articulation is unnumbered in the docs")
            elif doc_no != pos:
                issues.append(f"docs number this articulation {doc_no:02d} but it is #{pos} in the list")
            art = {"index": pos, "docNumber": doc_no, "name": name,
                   "category": classify(name), "legato": is_legato(name)}
            art.update(fields)
            if "dynamics" not in art:
                art["dynamics"] = ["mf"]
                art["dynamicCountDocs"] = None
            art["issues"] = issues
            inst["articulations"].append(art)
            pending = None
            continue

        cell = line.lstrip("| ").strip()
        if is_data(i + 1):
            if inst is None:
                # a bare section title directly followed by data can't happen; treat as instrument
                new_inst(group or cell, group)
            m = ART_NUM_RE.match(cell)
            pending = (int(m.group(1)), m.group(2).strip(), []) if m else (None, cell, [])
            continue

        if line.startswith("|"):
            continue  # stray table cells ('| +', '| -', empty)

        m = INST_RE.match(line)
        if m:
            new_inst(m.group(2), group)
            continue
        m = GROUP_PAREN_RE.match(line)
        if m:
            group = m.group(2).strip()
            new_inst(group, group)
            continue
        # bare section title (Symphonic Strings: 'Violins I'; Percussion: 'Grand Piano')
        if len(line) < 40 and not line.startswith("-") and not line.endswith("."):
            group = line
            inst = None
            if header_follows(i + 1):
                new_inst(line, group)

    return [x for x in instruments if x["articulations"]]


def check_names(instruments):
    for x in instruments:
        for a in x["articulations"]:
            for d in a["dynamics"]:
                if d not in DYN_ORDER:
                    a["issues"].append(f"unknown dynamic marking '{d}'")
            if not a.get("ranges"):
                a["issues"].append("no playable range")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    total_art = total_issue = 0
    for stem, (out_name, coll) in COLLECTIONS.items():
        raw = (SRC / f"{stem}.html").read_text(encoding="utf-8")
        instruments = walk(html_to_lines(raw), coll)
        check_names(instruments)
        doc = {
            "format": "secant.preset",
            "version": 1,
            "vendor": "Orchestral Tools",
            "collection": coll,
            "player": "SINE Player",
            "noteConvention": "C3=60",
            "source": BASE_URL + stem,
            # SINE's default keyswitch for articulation #1 (verified in SINE Player 1.4.3 with Berlin
            # Woodwinds); articulation n uses keyswitchBase + n - 1, legato slots included
            "keyswitchBase": "C0",
            "instruments": instruments,
        }
        (OUT / out_name).write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        n_art = sum(len(x["articulations"]) for x in instruments)
        n_iss = sum(len(a["issues"]) for x in instruments for a in x["articulations"]) + \
            sum(len(x.get("issues", [])) for x in instruments)
        total_art += n_art
        total_issue += n_iss
        print(f"{out_name:40s} {len(instruments):3d} instruments {n_art:4d} articulations {n_iss:3d} issues")
    print(f"total: {total_art} articulations, {total_issue} issues")


if __name__ == "__main__":
    main()
