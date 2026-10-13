#include "Planning.h"

namespace tp
{
void attachSine (ArticulationPlan& p, const SineInstrument* inst, DynamicsSource userDynSource, PlanNotes& notes)
{
    if (inst == nullptr)
        return;
    const auto label = "#" + juce::String (p.art.index) + " " + p.art.name;
    const SineArticulation* byKs = p.keyswitch >= 0 ? inst->byKeyswitch (p.keyswitch) : nullptr;
    const SineArticulation* byTitle = inst->byTitle (p.art.name);
    const SineArticulation* s = nullptr;

    if (byKs != nullptr && (byTitle == nullptr || byTitle == byKs))
    {
        s = byKs;
        if (byTitle == nullptr)
            notes.warnings.add (label + ": keyswitch " + noteName (p.keyswitch) + " plays '" + byKs->title
                                + "' in SINE (different name - check the preset)");
    }
    else if (byTitle != nullptr && byTitle->keyswitch >= 0)
    {
        // the player has this articulation on another key: trust the player
        notes.log.add (label + ": keyswitch " + (p.keyswitch >= 0 ? noteName (p.keyswitch) : juce::String ("-")) + " -> "
                       + noteName (byTitle->keyswitch) + " (where SINE has '" + byTitle->title + "'"
                       + (byKs != nullptr ? "; " + noteName (p.keyswitch) + " plays '" + byKs->title + "'" : juce::String()) + ")");
        p.keyswitch = byTitle->keyswitch;
        s = byTitle;
    }
    if (s == nullptr)
    {
        notes.warnings.add (label + ": not found in SINE's articulation list (keyswitch "
                            + (p.keyswitch >= 0 ? noteName (p.keyswitch) : juce::String ("-")) + ")");
        return;
    }
    p.hasSine = true;
    p.sine = *s;

    if (! s->dynamics.isEmpty() && s->dynamics != p.art.dynamics)
    {
        notes.warnings.add (label + ": docs list " + juce::String (p.art.dynamics.size()) + " dynamic layers ("
                            + p.art.dynamics.joinIntoString (" ") + "), SINE has " + juce::String (s->dynamics.size()) + " ("
                            + s->dynamics.joinIntoString (" ") + ") - using SINE's");
        p.art.dynamics = s->dynamics;
    }
    if (userDynSource == DynamicsSource::autoDetect)
    {
        p.dynSource = s->modwheelDynamics ? DynamicsSource::cc1 : DynamicsSource::velocity;
        notes.log.add (label + ": dynamics -> " + toString (p.dynSource) + " (SINE: mod wheel control "
                       + (s->modwheelDynamics ? "on" : "off") + ")");
    }
    if (s->roundRobins != p.art.roundRobins)
        notes.warnings.add (label + ": docs say " + juce::String (p.art.roundRobins) + " round robins, SINE has "
                            + juce::String (s->roundRobins));
    if (s->randomRR)
        notes.log.add (label + ": SINE plays its round robins in random order");
    if (s->legato != p.art.legato)
        notes.warnings.add (label + ": legato " + (s->legato ? "on" : "off") + " in SINE, docs say "
                            + (p.art.legato ? "legato" : "not legato"));
}

LegatoChainShape legatoChainShape (const ArticulationPlan& p, const RunSettings& s)
{
    LegatoChainShape c;
    c.roundRobins = p.expectedRoundRobins() > 0 ? p.expectedRoundRobins() : juce::jmax (1, p.art.roundRobins);
    const int R = c.roundRobins;
    const int cycles = 1 + juce::jmax (0, s.legatoRRConfirm);
    if (R <= 1)
        c.transitions = 2; // up and down once
    else if (R % 2 == 0)
    {
        c.bothStarts = true;
        c.transitions = R * cycles;
    }
    else
        c.transitions = 2 * R * cycles;
    return c;
}

double legatoRenderUnits (const ArticulationPlan& p, const RunSettings& s, int notes, double gapMs)
{
    // reset pause + release tail (typically ~1 s) + the notes + the capture after the last one
    const double sec = (p.resetPauseMs (s.legatoResetPauseMs) + 1000.0 + notes * gapMs + s.legatoCaptureMs) / 1000.0;
    return sec / 1.2;
}

double legatoJobUnits (const ArticulationPlan& p, int note, const RunSettings& s)
{
    const auto shape = legatoChainShape (p, s);
    juce::Array<int> intervals = s.legatoIntervals;
    if (s.legatoUnison)
        intervals.insert (0, 0);
    double u = 0;
    for (auto& cell : p.legatoCells)
        for (auto i : intervals)
        {
            if (! p.playable.contains (note + i))
                continue;
            const int starts = shape.bothStarts && i != 0 ? 2 : 1;
            double chain = 0;
            // one render per chain length (verified by the next, longer one); the last is rendered until identical
            for (int k = 0; k <= shape.transitions; ++k)
                chain += (k == shape.transitions ? 2.0 : 1.0) * legatoRenderUnits (p, s, k + 1, cell.gapMs);
            // the single-note render is shared by every interval starting on the same note
            u += starts * chain;
        }
    return juce::jmax (1.0, u);
}

int legatoJobTransitions (const ArticulationPlan& p, int note, const RunSettings& s)
{
    const auto shape = legatoChainShape (p, s);
    juce::Array<int> intervals = s.legatoIntervals;
    if (s.legatoUnison)
        intervals.insert (0, 0);
    int n = 0;
    for (auto i : intervals)
        if (p.playable.contains (note + i))
            n += (shape.bothStarts && i != 0 ? 2 : 1) * shape.transitions;
    return n * (int) p.legatoCells.size();
}

int legatoZoneVelocity (int lo, int hi)
{
    if (lo <= 1 && hi >= 127)
        return 100;
    return juce::jlimit (1, 127, (lo + hi + 1) / 2);
}

std::vector<LegatoCell> buildLegatoCells (const ArticulationPlan& p, const RunSettings& s)
{
    std::vector<SineLegatoZone> zones;
    if (p.hasSine && ! p.sine.legatoZones.empty())
        zones = p.sine.legatoZones;
    else
    {
        SineLegatoZone z;
        z.type = "Legato";
        zones.push_back (z);
    }

    // note-on spacings are whole blocks (every note-on starts a processBlock call), so the gap that
    // decides the speed zone is the block-aligned one
    const double blockMs = s.blockSize * 1000.0 / s.sampleRate;
    auto aligned = [&] (double ms) { return juce::jmax (1.0, std::round (ms / blockMs)) * blockMs; };
    auto inSpeed = [] (const SineLegatoZone& z, double g) { return g >= z.speedLoMs && (z.speedHiMs < 0 || g < z.speedHiMs); };

    std::vector<std::pair<int, int>> velocityRanges;
    for (auto& z : zones)
        if (std::find (velocityRanges.begin(), velocityRanges.end(), std::make_pair (z.velocityLo, z.velocityHi)) == velocityRanges.end())
            velocityRanges.emplace_back (z.velocityLo, z.velocityHi);
    std::sort (velocityRanges.begin(), velocityRanges.end());

    std::vector<LegatoCell> cells;
    for (auto [lo, hi] : velocityRanges)
    {
        std::vector<double> gaps;
        for (auto g : s.legatoGapsMs)
            gaps.push_back (aligned (g));
        // every speed zone of this velocity range must be played at least once
        for (auto& z : zones)
        {
            if (z.velocityLo != lo || z.velocityHi != hi)
                continue;
            if (std::none_of (gaps.begin(), gaps.end(), [&] (double g) { return inSpeed (z, g); }))
            {
                const double want = z.speedHiMs < 0 ? z.speedLoMs + juce::jmax (200.0, z.speedLoMs) : 0.5 * (z.speedLoMs + z.speedHiMs);
                double g = aligned (juce::jmax (40.0, want));
                // alignment may push it out of a narrow zone: step inwards
                for (int i = 0; i < 64 && ! inSpeed (z, g); ++i)
                    g += g < z.speedLoMs ? blockMs : -blockMs;
                if (inSpeed (z, g))
                    gaps.push_back (g);
            }
        }
        std::sort (gaps.begin(), gaps.end());
        gaps.erase (std::unique (gaps.begin(), gaps.end(), [] (double a, double b) { return std::abs (a - b) < 1e-6; }), gaps.end());
        for (auto g : gaps)
        {
            LegatoCell c;
            c.velocity = legatoZoneVelocity (lo, hi);
            c.gapMs = g;
            c.type = "unassigned";
            for (auto& z : zones)
                if (z.velocityLo == lo && z.velocityHi == hi && inSpeed (z, g))
                {
                    c.type = z.type;
                    c.typeId = z.typeId;
                    break;
                }
            cells.push_back (c);
        }
    }
    return cells;
}

} // namespace tp
