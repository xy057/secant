#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// Sub-sample onset estimation for a single rendered hit.
//
// The capture buffer starts `preSamples` before the note-on, so sample index
// `preSamples` is time 0. All results are milliseconds relative to the
// note-on (plugin latency already subtracted by the caller via latencySamples).
//
// Every method is computed for every hit; which one is "primary" is a
// reporting choice, so re-analysis never requires re-rendering.

namespace tp
{
enum class EnvelopeMode
{
    rms,     // centred moving RMS (rmsWindowMs) - cheap, but ripples with the carrier for low notes
    hilbert  // analytic-signal magnitude - instantaneous amplitude, independent of carrier phase
};

struct OnsetParams
{
    EnvelopeMode envelope = EnvelopeMode::hilbert;
    double sampleRate = 48000.0;
    int preSamples = 0;            // samples before note-on in the buffer
    int latencySamples = 0;        // plugin-reported latency, subtracted from all times
    double peakWindowMs = 500.0;   // search window for the reference peak (from note-on)
    double absThresholdDb = -80.0; // "first sound" threshold, dBFS
    double rmsWindowMs = 0.5;      // centred RMS window for the threshold envelope (rms mode)
    double hilbertSmoothMs = 0.1;  // light centred smoothing of the analytic magnitude (hilbert mode)
    double slopeWindowMs = 2.0;    // centred RMS window for the max-slope envelope
    // Pitch-adaptive smoothing: the threshold envelope window is at least
    // periodSmoothing x the fundamental period (harmonics beat at multiples of f0,
    // so averaging over whole periods makes the envelope independent of waveform phase).
    double fundamentalHz = 0.0;    // 0 = unknown (no pitch-adaptive smoothing)
    double periodSmoothing = 0.5;

    // End of attack / start of decay - the moment to put on the grid (negative track delay).
    // Walk the envelope up from the rel-20 crossing; the attack is over when it either
    // decays attackDecayDb below its running maximum, or rises less than attackPlateauRiseDb
    // over attackPlateauMs (a sustain that has settled). attackEnd = where the envelope first
    // comes within attackEndDb of that attack peak.
    double attackSmoothMs = 2.0;   // envelope window, but at least attackPeriods x the note's period
    double attackPeriods = 1.0;    // whole periods cancel harmonic beating, which would otherwise fake a "decay"
    double attackDecayDb = 1.0;
    double attackPlateauMs = 40.0;
    double attackPlateauRiseDb = 1.0;
    double attackEndDb = -1.0;
};

inline double midiToHz (int note) { return 440.0 * std::pow (2.0, (note - 69) / 12.0); }

// Relative thresholds (dB below the reference peak) reported for every hit.
inline constexpr float kRelThresholdsDb[] = { -40.0f, -30.0f, -20.0f, -12.0f, -6.0f };

struct OnsetResult
{
    bool valid = false;            // false => no sound above absThresholdDb in the window
    double firstMs = 0;            // first sample above absThresholdDb (sub-sample)
    double relMs[std::size (kRelThresholdsDb)] {}; // envelope crossing of each relative threshold
    double maxSlopeMs = 0;         // steepest rise of the 2 ms RMS envelope (attack centre)
    double peakMs = 0;             // reference peak of the RMS envelope
    double attackPeakMs = 0;       // first prominent maximum / plateau arrival (end of attack, start of decay)
    double attackEndMs = 0;        // envelope first within attackEndDb of the attack peak
    double attackPeakDb = -200;
    double peakDb = -200;          // reference peak level (RMS envelope, dBFS)
    double preRollDb = -200;       // loudest sample before note-on: > -90 dB means bleed from a previous hit

    // primary-method lookup by key: "first", "rel-40" .. "rel-6", "maxSlope", "peak"
    double get (const juce::String& method) const;
};

juce::StringArray onsetMethodKeys();

OnsetResult analyseOnset (const juce::AudioBuffer<float>& capture, const OnsetParams& params);

// Normalised cross-correlation (zero lag) of the first `numSamples` after note-on
// of two captures - used to verify that hit k and hit k+RR are the same sample.
double similarity (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int start, int numSamples);

// Best normalised correlation over relative shifts of up to +-maxLag samples (b shifted against a);
// `bestLag` > 0 means b plays later than a.
double similarityLagged (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int start, int numSamples,
                         int maxLag, int& bestLag);

} // namespace tp
