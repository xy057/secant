#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "ResultStore.h"

namespace tp
{
// Shared, read-only description of a run plus the job cursor.
struct RunContext
{
    RunSettings settings;
    std::vector<ArticulationPlan> plans;
    std::vector<Job> jobs;
    std::atomic<size_t> nextJob { 0 };
    std::atomic<int> loadedWorkers { 0 }; // workers that finished waiting for the plugin to sound
    std::atomic<int64_t> doneMilliUnits { 0 }; // finished jobs' units x 1000 (progress / ETA)
    std::atomic<int64_t> legatoTransitionsDone { 0 }; // live count (a legato job reports its results only when it ends)
    // determinism check: the first worker to load runs it (it may switch `settings` to Never), the others
    // wait for its verdict before taking a job
    std::atomic<bool> determinismClaimed { false }, determinismDone { false };
    int64_t legatoTransitionsTotal = 0;
    double totalUnits = 0;
    ResultStore* store = nullptr;
};

// Owns one plugin instance (a clone of the user's configured instance) and
// renders jobs offline on its own thread. Each job = one note: every articulation (keyswitch)
// and dynamic layer of it, so the instance's round-robin counters see one coherent sequence.
class RenderWorker : public juce::Thread
{
public:
    RenderWorker (int id, std::unique_ptr<juce::AudioPluginInstance>, RunContext&);
    ~RenderWorker() override;

    void run() override;
    bool finished() const { return isDone.load(); }

    // Must be called on the message thread after the thread has stopped.
    std::unique_ptr<juce::AudioPluginInstance> releasePlugin() { return std::move (plugin); }

private:
    struct Capture
    {
        int64_t start = 0;
        juce::AudioBuffer<float> buffer;
    };

    struct PlayHead : juce::AudioPlayHead
    {
        int64_t* now = nullptr;
        int64_t origin = 0; // transport position 0; legato chains restart it so every repeat sees the same timeline
        double sampleRate = 48000.0;
        juce::Optional<PositionInfo> getPosition() const override;
    };

    // timeline
    void schedule (const juce::MidiMessage&, int64_t time);
    void advanceTo (int64_t end, Capture* capture);
    bool renderUntilSilent (double maxMs);
    void pace();

    // musical actions
    void sendKeyswitch (int note);
    void selectArticulation (int cc1);
    void sendResetKey(); // after a reset pause: the player's RR reset key, if it has one
    bool playHit (int note, int velocity, int cc1, Category, Capture& out);
    void playTimedHit (int note, int velocity, int64_t tOn, int64_t hold, int captureLen, Capture& out);
    void runSeries (const ArticulationPlan&, int note, int dynIndex, std::vector<Hit>& out, std::vector<Series>& seriesOut);
    void runSubtractedSeries (const ArticulationPlan&, int note, int dynIndex, int64_t interval, int64_t hold, int captureLen,
                              int maxHits, bool fast, std::vector<Hit>& out, std::vector<Series>& seriesOut);
    juce::AudioBuffer<float> renderSequence (int note, int velocity, int cc1, int count, int64_t interval, int64_t hold,
                                             int windowLen, int64_t& windowStartOut);
    DynamicsSource detectDynamicsSource (const ArticulationPlan&, int note);
    OnsetParams onsetParamsFor (const ArticulationPlan&, int note, int captureLen) const;
    // a series whose onsets a previous note's tail masked is measured again with tail subtraction
    // (needs replays from the reset state, so never with round robins that don't reset)
    bool remeasuresMaskedSeries() const { return ctx.settings.subtractTailsWhenMasked && ! ctx.settings.rrNeverResets; }
    int64_t alignUp (int64_t t) const;
    int64_t blockInterval (double ms) const;
    std::map<int, DynamicsSource> resolvedDynamics; // per plan, detected once per worker

    // legato: chains A B A B ... from the reset state; render(notes 0..k) vs render(notes 0..k-1)
    void runLegatoJob (int planIdx, int note, int dynIndex);
    juce::AudioBuffer<float> renderChain (const std::vector<int>& notes, int count, int velocity, int cc1, int64_t step, int captureLen);
    // renders until two consecutive renders are bit-identical (SINE: the first play of a streamed
    // transition differs, and now and then a note is dropped)
    juce::AudioBuffer<float> renderChainConverged (const std::vector<int>& notes, int count, int velocity, int cc1, int64_t step,
                                                   int captureLen, int& renders, bool& converged);
    int legatoPreSamples() const { return (int) msToSamples (100.0); }
    int64_t lastNoteOn = std::numeric_limits<int32_t>::min();
    int currentKeyswitch = -1;
    bool waitForLoad();
    bool awaitDeterminismVerdict();
    void checkDeterminism();
    void runJob (const Job&);

    double msToSamples (double ms) const { return ms * ctx.settings.sampleRate / 1000.0; }

    const int id;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    RunContext& ctx;
    PlayHead playHead;

    std::vector<std::pair<int64_t, juce::MidiMessage>> pending;
    juce::AudioBuffer<float> block;
    juce::MidiBuffer midi;
    int64_t now = 0;
    int mainChannels = 2;
    double wallStart = 0;
    double playerResetMs = 0; // the current articulation's own RR reset time (+ margin); 0 = none
    int64_t creditedMilliUnits = 0, currentJobMilliUnits = 0; // progress credited inside the running job
    std::atomic<bool> isDone { false };
};

} // namespace tp
