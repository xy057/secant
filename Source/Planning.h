#pragma once

#include "RunTypes.h"

// Turning the user's choices + the player's own configuration into what the workers play.
namespace tp
{
struct PlanNotes
{
    juce::StringArray log;      // what was taken from / corrected by the player's configuration
    juce::StringArray warnings; // disagreements between the preset (docs) and the player
};

// Matches the plan to the player's articulation (by keyswitch, else by title) and applies what
// the player knows better than the docs: the keyswitch (if the title sits on another key), the
// dynamic layers, whether layers follow velocity or the mod wheel, the round-robin count and the
// legato zones. `userDynSource` = the GUI choice (autoDetect = let the player decide).
void attachSine (ArticulationPlan&, const SineInstrument*, DynamicsSource userDynSource, PlanNotes&);

// The legato grid of one articulation: one velocity per velocity zone x the configured note
// spacings, plus a spacing inside every speed zone that none of them reaches.
std::vector<LegatoCell> buildLegatoCells (const ArticulationPlan&, const RunSettings&);

// Shape of the legato chains for one articulation. Chains alternate two notes from the reset state
// and the player's legato round-robin counter advances once per note, so K consecutive transitions
// meet K consecutive RR slots. One direction only uses every other transition: with an odd RR count a
// chain of 2R transitions still reaches every slot in both directions; with an even count it reaches
// half of them, so a second chain starts on the other note and both chains need only R transitions.
// Every transition k needs its own render of notes 0..k (the reference for k+1), so the cost grows
// with K^2 - keep K minimal.
struct LegatoChainShape
{
    int roundRobins = 1;
    int transitions = 2;   // K: transitions per chain
    bool bothStarts = false;
};
LegatoChainShape legatoChainShape (const ArticulationPlan&, const RunSettings&);

// Expected work of one chain render of `notes` notes at this spacing, in hit-sized units (~1.2 s of
// audio each), and of a whole legato job (one note x one layer); used for progress / ETA.
double legatoRenderUnits (const ArticulationPlan&, const RunSettings&, int notes, double gapMs);
double legatoJobUnits (const ArticulationPlan&, int note, const RunSettings&);
// Transitions one legato job measures (for the live progress count).
int legatoJobTransitions (const ArticulationPlan&, int note, const RunSettings&);

// Velocity sent for a legato cell's notes (middle of the zone; 100 for an unrestricted zone).
int legatoZoneVelocity (int lo, int hi);

} // namespace tp
