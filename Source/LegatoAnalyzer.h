#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

// Legato / portamento transition timing.
//
// A legato transition is measured on a render where note A is held and note B starts while A is
// still down. The plugin then plays a transition recording (slur, portamento, run, ...) that ends on
// B's sustain. What matters for a negative track delay is when the *new pitch* is established.
//
// Because source and target are known, every analysis frame is tested directly for periodicity in
// A's period and in B's period (YIN d', a few tens of cents of tolerance for vibrato and scoops).
// That is robust where a free pitch tracker is not: the noisy middle of a slur, octave intervals
// (an A-periodic signal is not B-periodic when B is the higher note) and A's own release sample
// still sounding under B (it is quieter, so B's period wins).
//   start  = the source pitch ends for good,   arrive = the target pitch is established for good,
//   mid    = the glide crosses the log-frequency midpoint (portamento), else halfway start..arrive.
// Two sample-exact references need no pitch: the moment the render departs from a render of A alone
// (the transition audio starts), and the end of attack of the difference signal (the new
// material's attack) - the only usable measure for unison / re-articulated transitions.
//
// All times are ms relative to B's note-on (plugin latency subtracted by the caller).

namespace tp
{
struct PitchFrame
{
    double timeMs = 0;     // centre of the analysis window, rel. note-on
    double hz = 0;         // 0 = unvoiced
    double aperiodicity = 1; // YIN d'(tau) at the chosen dip: ~0 for a clean periodic tone
    double rmsDb = -200;
    // YIN d' at two caller-given periods (best value within +-probeCents): how well the frame is
    // periodic in the source / target note's period, whatever the overall pitch pick says
    double probe[2] { 1.0, 1.0 };
};

struct PitchTrackParams
{
    double sampleRate = 48000.0;
    double minHz = 30.0, maxHz = 4000.0; // search range (caller narrows it around the notes involved)
    double hopMs = 0.5;
    double windowPeriods = 2.5;      // window length in periods of minHz
    double yinThreshold = 0.2;       // first dip below this wins (YIN absolute threshold)
    double voicedMaxAperiodicity = 0.35;
    double silenceDb = -70.0;
    double probeHz[2] { 0.0, 0.0 };  // 0 = not probed
    double probeCents = 40.0;
};

// Mono (channel-summed) YIN over [start, start+numSamples); frame times are relative to `zeroSample`.
std::vector<PitchFrame> trackPitch (const juce::AudioBuffer<float>& buffer, int start, int numSamples, int zeroSample,
                                    const PitchTrackParams&);

struct LegatoParams
{
    double sampleRate = 48000.0;
    int latencySamples = 0;
    double divergeDb = -50.0;       // A+B render departs from A-alone by more than this (re. B's steady level)
    double periodicDprime = 0.25;   // a frame "is" note X when YIN d' at X's period is below this
    double octaveRejectDprime = 0.45; // octaves: fallback boundary (normally measured per recording, see analyseLegato)
    double noteCents = 40.0;        // tolerance around each note's period (capped at 1/3 of the interval)
    double minRunMs = 3.0;          // shorter runs of a note state are ignored (noise, octave slips)
    double arriveConfirmMs = 40.0;  // after arrival, the target must hold most of this window...
    double arriveConfirmShare = 0.6; // ...as a share of the voiced frames
    double stableFromMs = 450.0;    // steady-state window (tuning / vibrato) starts here...
    double stableToMs = 900.0;      // ...and ends here (clipped to the capture)
};

struct LegatoResult
{
    bool valid = false;           // pitch measures found (false for unison or unvoiced material)
    bool diffValid = false;       // difference-signal measures found
    double divergeMs = 0;         // first audible difference from "A held alone": the transition starts
    double startMs = 0;           // the source pitch ends for good (last frame periodic in the source note)
    double midMs = 0;             // pitch crosses the log-frequency midpoint (glides); else halfway start..arrive
    double arriveMs = 0;          // the target pitch is established for good: the new note has arrived
    bool midFromPitch = false;    // midMs from a measured glide (false: no pitch inside the transition)
    double diffAttackEndMs = 0;   // end of attack of (A+B) - (A alone): the new material's attack
    double targetCentsError = 0;  // steady-state pitch of B, cents re. equal temperament (tuning check)
    double vibratoCents = 0;      // steady-state pitch spread (median absolute deviation, cents)
    double steadyDb = -200;       // steady-state level of B
    double voicedFraction = 0;    // share of frames periodic in either note between note-on and arrival + 100 ms
    juce::String failure;         // why `valid` is false

    double get (const juce::String& method) const;
};

juce::StringArray legatoMethodKeys(); // "arrive", "mid", "start", "diverge", "diffAttackEnd"

// withB: render with A held and B played at sample `noteOnSample` (A released shortly after);
// aloneA: the identical render without B. Both buffers are aligned and equally long.
LegatoResult analyseLegato (const juce::AudioBuffer<float>& withB, const juce::AudioBuffer<float>& aloneA, int noteOnSample,
                            int sourceNote, int targetNote, const LegatoParams&);

} // namespace tp
