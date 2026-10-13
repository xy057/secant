#pragma once

#include "LegatoAnalyzer.h"
#include "OnsetAnalyzer.h"
#include "PresetModel.h"
#include "SineState.h"
#include <map>

namespace tp
{
// Per-category timing. Captures are long enough to contain the reference peak;
// "phrase" (runs / repetitions) only looks at the first note.
struct CategoryTiming
{
    double captureMs;    // analysed length after note-on
    double peakWindowMs; // reference-peak search window
    juce::String primaryMethod;
    double holdMs;       // note length; short holds keep release tails away from the next onset
    double intervalMs;   // note-on spacing inside a series - must stay below the plugin's RR reset time
};

struct RunSettings
{
    double sampleRate = 48000.0;
    int blockSize = 256;
    int numWorkers = 4;
    int noteStep = 1;            // 1 = every note in range
    // Round-robin discovery: keep playing the same note/layer until a hit repeats an
    // earlier one (= the RR cycle closed), then optionally one more hit to confirm.
    bool confirmCycle = true;
    int confirmHits = 2;         // consecutive hits that must line up with the cycle after the first repeat
    int maxHitsPerSeries = 48;   // safety cap for instruments that never repeat
    double rrMatchThreshold = 0.995;
    // Fingerprint matches, for hits whose waveform is disturbed by the previous note's tail:
    //  (a) similarity > rrLooseSimilarity, peak within rrLoosePeakDb and attack end within rrLooseOnsetMs, or
    //  (b) similarity > rrPeakMatchSimilarity and peak level within rrPeakMatchDb (identical recording,
    //      attack-end estimate disturbed by the tail - slow attacks)
    double rrLooseSimilarity = 0.97;
    double rrLoosePeakDb = 0.1;
    double rrLooseOnsetMs = 0.3;
    double rrPeakMatchSimilarity = 0.98;
    double rrPeakMatchDb = 0.05;
    int rrSeriesRetries = 2;            // restart a series when a note comes out silent mid-series
    int rrMaxLagSamples = 4;            // shift tolerance when comparing hits (SINE: after-pause start is 3-4 samples early)
    double rrLaggedPeakDb = 0.01;       // a shifted match must have an essentially identical peak level
    double tailContaminationDb = -20.0; // tail before the onset within this of the hit's peak -> flagged, excluded from stats
    double rrGainInvariantSimilarity = 0.999; // same recording at a different gain (SINE level variation)...
    double rrGainInvariantOnsetMs = 0.1;      // ...if the end of attack also agrees
    double tailUncertainDb = -40.0;      // tail/residue above this -> waveform comparison relaxed (fingerprint only)
    double rrTailUncertainSimilarity = 0.95;
    bool nonRealtime = true;
    double maxSpeed = 0.0;       // x realtime; 0 = unlimited
    bool sendAllSoundOff = true; // CC120 after each hit to cut release tails
    double prerollMs = 60.0;     // CC1 -> note-on gap
    double preCaptureMs = 10.0;  // captured audio before note-on (bleed check)
    double keyswitchSettleMs = 150.0;
    double cc1SettleMs = 500.0;
    bool warmupHit = false;      // superseded by the reset pause (the after-pause hit is now measured)
    // Round-robin timing (SINE: RR counter per instance, advances per note, resets after ~2.5 s of no notes).
    // Note spacing inside a series is per category (CategoryTiming::intervalMs).
    double rrResetPauseMs = 3000.0; // silence before each series: the counter starts from its reset state
    // > 0: the player's round-robin reset time for every articulation is set to this (patched into the
    // state the workers load) and the reset pauses shrink to it. Part of how the instrument plays: slow
    // legato chains of short-sounding patches reset between notes. 0 = keep the player's own times.
    double rrResetOverrideMs = 0.0;
    // the round robins never reset automatically - they keep cycling (sequential) or are random, whatever
    // the pause (common in Kontakt): nothing may rely on
    // replaying from a reset state - series notes are spaced until the previous one is silent instead of
    // tail subtraction, no fast-repetition series, dynamics source not probed (velocity unless set)
    bool rrNeverResets = false;
    double rrMaxIntervalMs = 1200.0; // hard cap on in-series spacing (SINE restarts its cycle at ~1.5 s)
    // Fast repetition (short articulations): the plugin plays extra variants when notes repeat quickly.
    // Measured with tail subtraction: render(notes 0..k) - render(notes 0..k-1) = clean note k.
    bool fastRepetition = true;
    bool subtractTailsForLongNotes = true; // long / dynamic series rendered with exact tail removal
    bool subtractTailsWhenMasked = true;   // short / phrase series re-measured with tail removal if a tail masked an onset
    double fastRepIntervalMs = 250.0;
    int fastRepMaxHits = 24;
    double tailMaxMs = 4000.0;
    double silenceDb = -96.0;
    double loadTimeoutSec = 240.0;
    double settleSec = 3.0;
    double similarityMs = 120.0; // window for RR similarity
    // RR prior from the player's own configuration (SINE state): a hit that lands exactly one cycle
    // after an earlier hit is accepted as the same recording under relaxed criteria (the waveform is
    // disturbed by a choked voice / gain variation, but the plugin itself says the cycle has this length)
    bool useRRPrior = true;
    double rrPriorSimilarity = 0.98;
    double rrPriorPeakDb = 1.0;     // the same recording replayed at a slightly different level (seen: first hit of a fast series, +0.4 dB)
    double rrPriorOnsetMs = 1.0;

    // ---- legato / portamento transitions
    // Every transition is rendered as a legato chain A B A B ... from the plugin's reset state:
    // transition k = render(notes 0..k) vs render(notes 0..k-1) (identical up to note k, so the
    // difference and the pitch change are exactly note k's transition). Chains give both directions;
    // with round robins they run until every RR slot has played in both directions twice.
    bool measureLegato = true;
    juce::Array<int> legatoIntervals { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 }; // semitones up from each source note
    bool legatoUnison = false;            // repeated-note legato (measured on the difference signal)
    std::vector<double> legatoGapsMs { 100.0, 250.0, 500.0, 1000.0 }; // note-on spacing (SINE: speed zones + adaptive legato)
    juce::String legatoPrimary = "arrive"; // arrive / mid / start / diverge / diffAttackEnd
    double legatoCaptureMs = 1000.0;      // analysed audio after each transition's note-on
    double legatoOverlapMs = 10.0;        // previous note released this long after the next note-on
    int legatoMaxRenders = 6;             // per chain step: render until two consecutive renders are identical
    int legatoRRConfirm = 0;              // extra RR cycles per chain (cost grows with the square of the chain length)
    double legatoResetPauseMs = 3000.0;   // silence before each chain (RR counter + adaptive legato reset)
    LegatoParams legato;
    OnsetParams onset;
    std::map<Category, CategoryTiming> timing {
        // attackEnd = end of attack / start of decay: the point that should land on the grid
        // Spacing must stay below SINE's ~1.5 s "restart the cycle" threshold (measured with RRProbe:
        // <= 1.2 s cycles, 1.5-2.5 s always the cycle's first sample, > 2.5 s the after-pause sample).
        //                       capture peakWin primary       hold  interval
        { Category::shortNote, { 450.0, 400.0, "attackEnd", 300.0, 1000.0 } },
        { Category::longNote, { 800.0, 700.0, "attackEnd", 600.0, 1200.0 } },
        { Category::dynamic, { 1100.0, 1000.0, "first", 900.0, 1200.0 } }, // crescendo/swell: "attack end" would be the swell's end
        { Category::phrase, { 400.0, 120.0, "attackEnd", 400.0, 1000.0 } },
    };
};

// One cell of the legato measurement grid: a velocity (selects SINE's velocity zone) and a
// note-on spacing (selects the speed zone; adaptive legato also scales the transition with it).
struct LegatoCell
{
    int velocity = 100;
    double gapMs = 1000.0;
    juce::String type;   // legato type this cell plays (SINE zone), "Legato" if unknown
    int typeId = 0;
};

// One articulation to test, after the user's GUI choices are applied.
struct ArticulationPlan
{
    int articulationPos = 0;       // index into Instrument::articulations
    Articulation art;
    int keyswitch = -1;            // MIDI note, -1 = none
    DynamicsSource dynSource = DynamicsSource::velocity;
    Category category = Category::shortNote;
    juce::Array<int> notes;        // after noteStep thinning

    bool hasSine = false;          // the player's own configuration for this articulation (SINE state)
    SineArticulation sine;
    bool measureLegato = false;    // legato transitions are measured for this articulation
    std::vector<LegatoCell> legatoCells;
    juce::Array<int> playable;     // every playable note (legato targets)

    // round robins according to the player (0 = unknown / random)
    int expectedRoundRobins() const { return hasSine && ! sine.randomRR ? sine.roundRobins : 0; }
    // silence that returns this articulation to its reset state: the configured pause, but never less
    // than the player's own round-robin reset time (SINE: resetAfterMS, e.g. 4000 ms for Berlin runs)
    double resetPauseMs (double configured) const { return hasSine ? juce::jmax (configured, sine.rrResetMs + 250.0) : configured; }

    int velocityFor (int layer) const
    {
        const int n = art.dynamics.size();
        return juce::jlimit (1, 127, (int) std::round ((layer + 0.5) * 127.0 / n));
    }
    // velocity mode: CC1 parked high; cc1 mode: the "pure" point of each layer of an n-layer crossfade
    int cc1For (int layer) const
    {
        const int n = art.dynamics.size();
        if (dynSource == DynamicsSource::velocity)
            return 100;
        if (n == 1)
            return 100;
        return juce::jlimit (0, 127, (int) std::round (layer * 127.0 / (n - 1)));
    }
    int velocityToSend (int layer) const { return dynSource == DynamicsSource::velocity ? velocityFor (layer) : 100; }
    // expected hits per note if the docs' RR count is right (progress estimate only)
    int expectedHitsPerNote (int confirmHits) const { return art.dynamics.size() * (art.roundRobins + 1 + confirmHits); }
};

// One job = one note: a single worker (= one plugin instance with its own RR counters) plays
// every articulation and dynamic layer of that note in sequence.
// Legato jobs (legatoPlan >= 0) are one source note x one dynamic layer of one articulation; every
// chain in them starts from the reset state, so they can run on any worker.
struct Job
{
    int note = 0;
    int legatoPlan = -1;
    int legatoDyn = 0;
    double units = 1.0; // expected work (~ hit-sized renders), for progress / ETA
};

struct Hit
{
    int note = 0;
    int dynIndex = 0;
    int seq = 0;          // play order within its (note, dyn) series
    int rr = 0;           // distinct-sample slot: originals are numbered 0..U-1 in play order, repeats inherit
    int duplicateOf = -1; // seq of the earlier identical hit, -1 = a distinct sample
    double matchSimilarity = std::numeric_limits<double>::quiet_NaN(); // similarity to duplicateOf
    bool looseMatch = false;  // matched via peak level + onset + similarity > rrLooseSimilarity
    int matchLagSamples = 0;  // start offset against the matched hit (> 0: this hit starts later)
    double bestSimilarity = std::numeric_limits<double>::quiet_NaN(); // originals: closest earlier original
    int bestSimilarityTo = -1;
    int velocity = 0;
    int cc1 = 0;
    OnsetResult onset;

    bool priorMatch = false;  // matched one cycle back under the player's own RR count (relaxed criteria)
    bool afterPause = false;  // first hit after the reset pause (the sample a phrase starts with)
    bool fastRepetition = false; // hit of the fast-repetition series (tail-subtracted)
    double tailDb = -200.0;      // previous note's tail level before this onset, relative to this hit's peak
    bool tailContaminated = false;
    bool tailSubtracted = false; // earlier notes' tails removed exactly (render difference)
    bool unresolved = false;     // masked hit that matched nothing: excluded from samples and statistics (duplicateOf = -2)

    bool isOriginal() const { return duplicateOf == -1; }
};

// Outcome of the RR search for one (note, dynamic layer).
struct Series
{
    int note = 0;
    int dynIndex = 0;
    int hitsPlayed = 0;
    int uniqueSamples = 0; // distinct samples heard
    int period = 0;        // cycle length, 0 = no repeat found within maxHitsPerSeries
    int cycleStart = 0;    // seq where the cycle begins (>0 if the first hits never came back)
    bool confirmed = false;
    bool silent = false;
    bool fastRepetition = false;
    double intervalMs = 0.0;
    int retries = 0; // restarts because a note came out silent
    bool randomOrder = false; // no stable cycle, but the set of samples stopped growing (random RR)
};

// One measured legato transition.
struct LegatoHit
{
    int source = 0, target = 0;  // MIDI notes
    int dynIndex = 0;
    int velocity = 0, cc1 = 0;
    double gapMs = 0;            // note-on spacing of the chain (block-aligned)
    juce::String type;           // legato type of the cell (SINE zone)
    int cell = 0;                // index into ArticulationPlan::legatoCells
    int chainIndex = 0;          // transition k of its chain (1 = from a freshly attacked note)
    int chainLength = 0;
    int rrSlot = 0;              // note index mod the player's RR count (its global counter starts at 0 after the reset pause)
    int renders = 0;             // renders needed until two consecutive ones were identical
    bool converged = true;
    bool referenceExact = true;  // render(0..k) and render(0..k-1) identical before note k (exact reference)
    LegatoResult result;
};

} // namespace tp
