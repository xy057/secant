#pragma once

#include <juce_core/juce_core.h>

// The compact export for programs that only need the delays: per articulation, per note, per
// dynamic layer, the negative track delay (ms) of every round robin, in the player's own RR order.
// Built from the full results JSON (ResultStore::toJson or a saved results file), so it can be
// produced again from any earlier run.
namespace tp
{
juce::var compactDelays (const juce::var& fullResults);

// "X.json" -> "X.delays.json"
juce::File delaysFileFor (const juce::File& resultsFile);

// Reads a full results file and writes its compact form next to it (or to `out`).
juce::Result writeCompactDelays (const juce::File& resultsFile, juce::File out = {});

} // namespace tp
