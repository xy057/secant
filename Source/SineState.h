#pragma once

#include <juce_core/juce_core.h>
#include <vector>

// SINE Player keeps its whole instrument configuration as JSON inside the VST3 state
// (VST3PluginState XML -> IComponent, JUCE base64 -> "VstW"/"CcnK" header -> JSON).
// It is the authority on what the player actually does: articulation keyswitches, round-robin
// count and reset time, whether the mod wheel switches dynamic layers, and the legato zone grid
// (velocity zones x speed zones -> legato type). Read once from the run's saved state.

namespace tp
{
struct SineLegatoType
{
    int id = 0;
    juce::String title;        // "Melodic Legato", "Rapid Legato", "Portamento", ...
};

// One cell of SINE's legato grid: notes whose velocity and note-on spacing fall in these ranges
// play this legato type.
struct SineLegatoZone
{
    int velocityLo = 0, velocityHi = 127;     // inclusive
    double speedLoMs = 0, speedHiMs = -1;     // note-on to note-on gap; hi < 0 = unbounded
    int typeId = 0;
    juce::String type;
};

struct SineArticulation
{
    juce::String id, title;
    int keyswitch = -1;          // mainOptions.switchValue (+ instrument ksshift)
    bool enabled = true;
    int roundRobins = 1;         // enabled RR slots
    double rrResetMs = 0;
    bool randomRR = false;
    bool modwheelDynamics = false; // dynamic layers crossfade on the mod wheel (else velocity)
    juce::StringArray dynamics;  // layer titles
    int keyLow = -1, keyHigh = -1;
    bool legato = false;
    std::vector<SineLegatoType> legatoTypes;
    std::vector<SineLegatoZone> legatoZones;
};

struct SineInstrument
{
    juce::String title;
    int ksShift = 0;
    std::vector<SineArticulation> articulations;
    // adaptive legato (per mic position in SINE; the first enabled mic's values)
    bool hasAdaptive = false;
    double adaptiveAmplitudeLoMs = 0, adaptiveAmplitudeHiMs = 0, adaptiveOffsetLoMs = 0, adaptiveOffsetHiMs = 0,
           adaptiveTransitionMinMs = 0;

    const SineArticulation* byKeyswitch (int ks) const;
    const SineArticulation* byTitle (const juce::String&) const; // case / punctuation insensitive
};

struct SineState
{
    bool valid = false;
    juce::String samplerVersion, error;
    std::vector<SineInstrument> instruments;
    int selectedInstrument = 0;

    const SineInstrument* instrument() const
    {
        return valid && selectedInstrument >= 0 && selectedInstrument < (int) instruments.size() ? &instruments[(size_t) selectedInstrument]
                                                                                                    : nullptr;
    }
};

// Parses a plugin state as returned by AudioPluginInstance::getStateInformation (or a .pluginstate file).
SineState parseSineState (const juce::MemoryBlock& pluginState);

// The same state with every articulation's round-robin reset time (roundRobinOptions.resetAfterMS)
// set to `ms`. Empty (and `error` set) if the state is not a SINE state or cannot be patched safely.
juce::MemoryBlock withRoundRobinReset (const juce::MemoryBlock& pluginState, double ms, juce::String* error = nullptr);

// The raw JSON (for diagnostics / saving next to the results); empty if not a SINE state.
juce::String extractSineJson (const juce::MemoryBlock& pluginState);

} // namespace tp
