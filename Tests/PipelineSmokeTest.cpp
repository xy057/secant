// End-to-end test of the render pipeline without the GUI, exactly as the app runs it:
// master state -> cloned RenderWorkers (parallel) -> RR cycle discovery -> analysis -> JSON.
//
//   PipelineSmokeTest --fake out.json            built-in deterministic RR sampler, with assertions
//     FAKE_MODE=humanize                          ...with a random level per note: the determinism check must switch to Never
//     FAKE_MODE=resetkey                          ...whose round robins reset only on a key (never after a pause)
//     FAKE_MODE=noreset                           ...the same sampler, but Secant isn't told the key: must switch to Never
//   PipelineSmokeTest <plugin.vst3> out.json [N] any VST3 instrument (default patch), no assertions
//   PipelineSmokeTest <plugin.vst3> out.json N <state.pluginstate> <keyswitch> <notes,comma> <dyn,comma> [category]
//                                            real instrument from a saved Secant plugin state

#include <juce_audio_processors/juce_audio_processors.h>
#include "../Source/Planning.h"
#include "../Source/RenderWorker.h"

using namespace tp;

namespace
{
// ---------------------------------------------------------------------------------------------
// A tiny sampler whose round robins are known exactly. Keyswitches C-2.. select articulations.
// Every RR is a distinct waveform (pitch / phase offset) starting after a known delay:
//   delayMs = base + rrStepMs * rr + 0.05 * (note - 60) - (velocity >= 64 ? 0.3 : 0)
struct FakeArticulation
{
    juce::String name;
    int rrCount;        // actual number of samples
    int docsRR;         // what the "docs" claim
    bool randomRR;      // random selection instead of a cycle
    double baseMs, rrStepMs;
    bool legato = false; // monophonic legato: a note-on while a note is held becomes a transition
};

const FakeArticulation kFakeArts[] = {
    { "Cycle 4 (docs 4)", 4, 4, false, 2.0, 0.7 },
    { "Cycle 3 (docs say 2)", 3, 2, false, 5.0, 1.1 },
    { "No RR", 1, 1, false, 3.0, 0.0 },
    { "Random 3 (non-cycling)", 3, 3, true, 4.0, 0.9 },
    { "Legato (fake)", 1, 1, false, 2.0, 0.0, true },
};
constexpr int kLegatoArt = 4;
constexpr double kRiseMs = 3.0;
constexpr int kResetKey = 10; // FAKE_MODE=resetkey: this key resets every round-robin counter

enum class FakeMode { normal, humanize, resetKey };
FakeMode fakeMode = FakeMode::normal;

// Fake legato transitions (ground truth):
//   velocity < 64             -> "Portamento": log-frequency glide from 20 ms, 150 ms long
//   velocity >= 64, gap >= 120 -> "Slur": pitch jumps at 60 + 2 x |interval| ms (with a bow-change dip)
//   velocity >= 64, gap < 120  -> "Fast": pitch jumps at 25 ms
constexpr double kGlideStartMs = 20.0, kGlideMs = 150.0, kFastJumpMs = 25.0, kSpeedZoneMs = 120.0;
double slurJumpMs (int semis) { return 60.0 + 2.0 * std::abs (semis); }

double expectedDelayMs (const FakeArticulation& a, int rr, int note, int velocity)
{
    return a.baseMs + a.rrStepMs * rr + 0.05 * (note - 60) - (velocity >= 64 ? 0.3 : 0.0);
}

class FakeRRSampler : public juce::AudioPluginInstance
{
public:
    FakeRRSampler() : AudioPluginInstance (BusesProperties().withOutput ("Out", juce::AudioChannelSet::stereo())) {}

    void fillInPluginDescription (juce::PluginDescription& d) const override
    {
        d.name = "FakeRRSampler";
        d.isInstrument = true;
    }
    const juce::String getName() const override { return "FakeRRSampler"; }
    void prepareToPlay (double sr, int) override { sampleRate = sr; }
    void releaseResources() override {}
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& m) override { m.setSize (4); m.fillWith (0); }
    void setStateInformation (const void*, int) override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        buffer.clear();
        int pos = 0;
        for (const auto meta : midi)
        {
            render (buffer, pos, meta.samplePosition);
            pos = meta.samplePosition;
            handle (meta.getMessage());
        }
        render (buffer, pos, buffer.getNumSamples());
        clock += buffer.getNumSamples();
    }

private:
    struct Voice
    {
        int note, rr, velocity, art;
        int64_t start;
        int64_t releaseAt = -1;
        float gain = 1.0f; // FAKE_MODE=humanize: random level per note
        // legato voices: pitch path and continuous phase
        double fromHz = 0, toHz = 0;
        int64_t glideStart = 0, glideEnd = 0, lastOn = 0;
        double phase = 0;
        bool dip = false;

        double hzAt (int64_t t) const
        {
            if (t < glideStart)
                return fromHz;
            if (t >= glideEnd)
                return toHz;
            return fromHz * std::pow (toHz / fromHz, (double) (t - glideStart) / (double) (glideEnd - glideStart));
        }
    };

    void handle (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())
        {
            if (fakeMode == FakeMode::resetKey && m.getNoteNumber() == kResetKey)
            {
                for (auto& c : counters)
                    c.clear();
                return;
            }
            if (m.getNoteNumber() < (int) std::size (kFakeArts)) // keyswitch
            {
                art = m.getNoteNumber();
                return;
            }
            const auto& a = kFakeArts[art];
            if (a.legato)
            {
                const auto ms = [this] (double v) { return (int64_t) std::llround (v * sampleRate / 1000.0); };
                for (auto& v : voices)
                    if (v.art == art && v.releaseAt < 0)
                    {
                        // legato transition of the held voice
                        const int semis = m.getNoteNumber() - v.note;
                        const auto gap = cursor - v.lastOn;
                        v.fromHz = v.hzAt (cursor);
                        v.toHz = midiToHz (m.getNoteNumber());
                        v.note = m.getNoteNumber();
                        v.lastOn = cursor;
                        if (m.getVelocity() < 64)
                        {
                            v.glideStart = cursor + ms (kGlideStartMs);
                            v.glideEnd = v.glideStart + ms (kGlideMs);
                            v.dip = false;
                        }
                        else
                        {
                            v.glideStart = v.glideEnd = cursor + ms (gap < ms (kSpeedZoneMs) ? kFastJumpMs : slurJumpMs (semis));
                            v.dip = true;
                        }
                        return;
                    }
                Voice v { m.getNoteNumber(), 0, m.getVelocity(), art, cursor };
                v.fromHz = v.toHz = midiToHz (m.getNoteNumber());
                v.glideStart = v.glideEnd = v.lastOn = cursor;
                voices.push_back (v);
                return;
            }
            int rr = 0;
            if (a.randomRR)
                rr = random.nextInt (a.rrCount);
            else
            {
                // per-key RR counter that resets after 2.5 s without a note, like SINE (resetkey: only on the key)
                const auto it = lastOn[art].find (m.getNoteNumber());
                if (fakeMode != FakeMode::resetKey && it != lastOn[art].end() && cursor - it->second > (int64_t) (2.5 * sampleRate))
                    counters[art][m.getNoteNumber()] = 0;
                lastOn[art][m.getNoteNumber()] = cursor;
                rr = counters[art][m.getNoteNumber()]++ % a.rrCount;
            }
            voices.push_back ({ m.getNoteNumber(), rr, m.getVelocity(), art, cursor });
            if (fakeMode == FakeMode::humanize)
                voices.back().gain = 1.0f + 0.1f * (random.nextFloat() - 0.5f);
        }
        else if (m.isNoteOff())
        {
            for (auto& v : voices)
                if (v.note == m.getNoteNumber() && v.releaseAt < 0)
                    v.releaseAt = cursor;
        }
        else if (m.isAllSoundOff())
            voices.clear();
    }

    void render (juce::AudioBuffer<float>& b, int from, int to)
    {
        for (int n = from; n < to; ++n)
        {
            const int64_t t = clock + n;
            float sum = 0;
            for (auto& v : voices)
            {
                const auto& a = kFakeArts[v.art];
                if (a.legato)
                {
                    // sustained, three harmonics, continuous phase; slurs dip 6 dB around the jump (bow change)
                    const double tOn = (double) (t - v.start) * 1000.0 / sampleRate - a.baseMs;
                    if (tOn <= 0)
                        continue;
                    double env = tOn < kRiseMs ? 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * tOn / kRiseMs) : 1.0;
                    if (v.dip)
                    {
                        const double dj = (double) (t - v.glideStart) * 1000.0 / sampleRate;
                        if (std::abs (dj) < 15.0)
                            env *= 1.0 - 0.25 * (1.0 + std::cos (juce::MathConstants<double>::pi * dj / 15.0));
                    }
                    if (v.releaseAt >= 0)
                        env *= std::exp (-(double) (t - v.releaseAt) / (0.01 * sampleRate));
                    v.phase += 2.0 * juce::MathConstants<double>::pi * v.hzAt (t) / sampleRate;
                    sum += (float) (0.3 * (v.velocity / 127.0) * env
                                    * (std::sin (v.phase) + 0.5 * std::sin (2.0 * v.phase) + 0.25 * std::sin (3.0 * v.phase)) / 1.75);
                    continue;
                }
                const double tMs = (double) (t - v.start) * 1000.0 / sampleRate - expectedDelayMs (a, v.rr, v.note, v.velocity);
                if (tMs <= 0)
                    continue;
                double env = tMs < kRiseMs ? 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * tMs / kRiseMs)
                                           : std::exp (-(tMs - kRiseMs) / 200.0);
                if (v.releaseAt >= 0)
                    env *= std::exp (-(double) (t - v.releaseAt) / (0.01 * sampleRate));
                const double f = midiToHz (v.note) * (1.0 + 0.003 * v.rr);
                sum += (float) (0.3 * v.gain * (v.velocity / 127.0) * env
                                * std::sin (2.0 * juce::MathConstants<double>::pi * f * tMs / 1000.0 + 1.3 * v.rr));
            }
            for (int c = 0; c < b.getNumChannels(); ++c)
                b.setSample (c, n, sum);
            cursor = t + 1;
        }
        voices.erase (std::remove_if (voices.begin(), voices.end(),
                                      [&] (auto& v) { return v.releaseAt >= 0 && (clock + to - v.releaseAt) > (int64_t) (0.3 * sampleRate); }),
                      voices.end());
    }

    double sampleRate = 48000.0;
    int64_t clock = 0, cursor = 0;
    int art = 0;
    std::map<int, int> counters[std::size (kFakeArts)];
    std::map<int, int64_t> lastOn[std::size (kFakeArts)];
    std::vector<Voice> voices;
    inline static std::atomic<int> instances { 0 };
    juce::Random random { 1234 + instances++ }; // deterministic, but different per instance
};

int failures = 0;
bool expectNever = false; // fake modes the determinism check must catch
void check (bool ok, const juce::String& what)
{
    std::printf ("  %-78s %s\n", what.toRawUTF8(), ok ? "ok" : "FAIL");
    if (! ok)
        ++failures;
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 3)
    {
        std::puts ("usage: PipelineSmokeTest (--fake | <plugin.vst3>) <out.json> [workers]");
        return 2;
    }
    const bool fake = juce::String (argv[1]) == "--fake";
    const juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
    const int numWorkers = argc > 3 ? juce::String (argv[3]).getIntValue() : 3;
    const bool withState = ! fake && argc > 7;
    juce::MemoryBlock savedState;
    if (withState && ! juce::File (argv[4]).loadFileAsData (savedState))
    {
        std::puts ("cannot read state file");
        return 1;
    }

    RunContext ctx;
    auto& s = ctx.settings;
    // NEVER=1: like the app's "RR reset: Never" (round robins that never reset automatically)
    s.rrNeverResets = juce::SystemStats::getEnvironmentVariable ("NEVER", "0") == "1";
    // RR_RESET=<ms>: like the app's "RR reset" choice - patch the player state, shrink the pauses
    if (const double rrReset = juce::SystemStats::getEnvironmentVariable ("RR_RESET", "0").getDoubleValue(); rrReset > 0 && withState)
    {
        juce::String err;
        auto patched = withRoundRobinReset (savedState, rrReset, &err);
        if (patched.isEmpty())
        {
            std::printf ("RR reset patch failed: %s\n", err.toRawUTF8());
            return 1;
        }
        savedState = patched;
        s.rrResetOverrideMs = rrReset;
        s.rrResetPauseMs = rrReset + 250.0;
        double adaptiveMs = 0;
        if (auto* si = parseSineState (savedState).instrument(); si != nullptr && si->hasAdaptive)
            adaptiveMs = juce::jmax (si->adaptiveAmplitudeHiMs, si->adaptiveOffsetHiMs, si->adaptiveTransitionMinMs);
        s.legatoResetPauseMs = juce::jmax (s.rrResetPauseMs, adaptiveMs + 250.0);
        std::printf ("RR reset %g ms: pauses %g / legato %g ms\n", rrReset, s.rrResetPauseMs, s.legatoResetPauseMs);
    }
    if (fake)
    {
        const auto mode = juce::SystemStats::getEnvironmentVariable ("FAKE_MODE", "normal");
        fakeMode = mode == "humanize" ? FakeMode::humanize
                   : mode == "resetkey" || mode == "noreset" ? FakeMode::resetKey
                                                             : FakeMode::normal;
        expectNever = mode == "humanize" || mode == "noreset";
        if (mode == "resetkey")
        {
            // like the app's "RR reset: Reset key"
            s.rrResetKey = kResetKey;
            s.rrResetPauseMs = 500.0;
            s.legatoResetPauseMs = 1000.0;
        }
    }
    s.checkDeterminism = ! s.rrNeverResets;
    s.numWorkers = numWorkers;
    s.settleSec = 0.1;
    s.loadTimeoutSec = 20;
    s.onset.sampleRate = s.sampleRate;

    juce::AudioPluginFormatManager fm;
    fm.addFormat (new juce::VST3PluginFormat());
    juce::PluginDescription desc;
    auto makeInstance = [&] () -> std::unique_ptr<juce::AudioPluginInstance> {
        if (fake)
            return std::make_unique<FakeRRSampler>();
        juce::String err;
        return fm.createPluginInstance (desc, s.sampleRate, s.blockSize, err);
    };
    if (! fake)
    {
        juce::OwnedArray<juce::PluginDescription> descs;
        fm.getFormat (0)->findAllTypesForFile (descs, argv[1]);
        if (descs.isEmpty())
            return 1;
        desc = *descs[0];
        if (! withState)
        {
            s.maxHitsPerSeries = 6;
            s.tailMaxMs = 500;
        }
        else
        {
            s.settleSec = 1.0;
            s.loadTimeoutSec = 120;
        }
    }

    // plans: fake -> one per fake articulation (keyswitch = its index); plugin -> one plain articulation
    const int numArts = fake ? (int) std::size (kFakeArts) : 1;
    for (int i = 0; i < numArts; ++i)
    {
        ArticulationPlan plan;
        plan.art.index = i + 1;
        plan.art.name = fake ? kFakeArts[i].name : juce::String (withState ? "Saved state" : "Default patch");
        plan.art.category = Category::shortNote;
        plan.art.ranges.add ({ 48, 72 });
        plan.art.dynamics = { "p", "f" };
        plan.art.roundRobins = fake ? kFakeArts[i].docsRR : 1;
        plan.keyswitch = fake ? i : -1;
        plan.category = Category::shortNote;
        plan.dynSource = DynamicsSource::velocity;
        plan.notes = { 48, 60, 72 };
        if (withState)
        {
            plan.keyswitch = parseNoteName (argv[5]);
            plan.notes.clear();
            for (auto& n : juce::StringArray::fromTokens (argv[6], ",", ""))
                plan.notes.add (parseNoteName (n));
            plan.art.dynamics = juce::StringArray::fromTokens (argv[7], ",", "");
            if (argc > 8)
            {
                plan.category = categoryFromString (argv[8]);
                plan.art.category = plan.category;
                plan.dynSource = DynamicsSource::autoDetect;
            }
            // the player's own configuration, exactly as the app applies it
            const auto sineState = parseSineState (savedState);
            if (auto* inst = sineState.instrument())
                if (auto* sa = inst->byKeyswitch (plan.keyswitch))
                {
                    plan.art.name = sa->title;
                    PlanNotes pn;
                    attachSine (plan, inst, DynamicsSource::autoDetect, pn);
                    for (auto& l : pn.log)
                        std::printf ("  SINE: %s\n", l.toRawUTF8());
                    for (auto& w : pn.warnings)
                        std::printf ("  SINE warning: %s\n", w.toRawUTF8());
                    if (sa->keyLow >= 0)
                        for (int n = sa->keyLow; n <= sa->keyHigh; ++n)
                            plan.playable.add (n);
                    // LEGATO=1: measure legato transitions (intervals / spacings from LEGATO_INTERVALS / LEGATO_GAPS)
                    if (sa->legato && juce::SystemStats::getEnvironmentVariable ("LEGATO", "0") == "1")
                    {
                        plan.art.legato = true;
                        plan.measureLegato = true;
                        s.legatoIntervals.clear();
                        for (auto& t : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("LEGATO_INTERVALS", "1,2,3,4,5,6,7,8,9,10,11,12"), ",", ""))
                            s.legatoIntervals.add (t.getIntValue());
                        s.legatoGapsMs.clear();
                        for (auto& t : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("LEGATO_GAPS", "100,250,500,1000"), ",", ""))
                            s.legatoGapsMs.push_back (t.getDoubleValue());
                        plan.legatoCells = buildLegatoCells (plan, s);
                        for (auto& c : plan.legatoCells)
                            std::printf ("  legato cell: %s @ %.1f ms, velocity %d\n", c.type.toRawUTF8(), c.gapMs, c.velocity);
                    }
                }
        }
        if (fake && kFakeArts[i].legato)
        {
            // monophonic legato patch with a SINE-like zone grid (velocity zones x speed zones)
            plan.art.name = kFakeArts[i].name;
            plan.art.legato = true;
            plan.art.category = plan.category = Category::longNote;
            plan.art.dynamics = { "mf" };
            plan.dynSource = DynamicsSource::cc1;
            plan.hasSine = true;
            plan.sine.title = plan.art.name;
            plan.sine.legato = true;
            plan.sine.roundRobins = 1;
            plan.sine.modwheelDynamics = true;
            plan.sine.legatoZones = { { 0, 63, 0.0, kSpeedZoneMs, 1, "Portamento" },
                                      { 0, 63, kSpeedZoneMs, -1.0, 1, "Portamento" },
                                      { 64, 127, 0.0, kSpeedZoneMs, 2, "Fast" },
                                      { 64, 127, kSpeedZoneMs, -1.0, 3, "Slur" } };
            for (int n = 48; n <= 84; ++n)
                plan.playable.add (n);
            plan.measureLegato = true;
            s.legatoIntervals = { 1, 5, 12 };
            s.legatoGapsMs = { 100.0, 250.0, 1000.0 };
            plan.legatoCells = buildLegatoCells (plan, s);
        }
        ctx.plans.push_back (plan);
    }
    if (juce::SystemStats::getEnvironmentVariable ("LEGATO_ONLY", "0") != "1")
        for (auto n : ctx.plans.front().notes)
            ctx.jobs.push_back ({ n });
    for (int pi = 0; pi < (int) ctx.plans.size(); ++pi)
        if (ctx.plans[(size_t) pi].measureLegato)
            for (auto n : ctx.plans[(size_t) pi].notes)
                for (int d = 0; d < ctx.plans[(size_t) pi].art.dynamics.size(); ++d)
                    ctx.jobs.push_back ({ n, pi, d });

    auto master = makeInstance();
    juce::MemoryBlock state;
    if (withState)
        state = savedState;
    else
        master->getStateInformation (state);

    Instrument inst;
    inst.name = fake ? "FakeRRSampler" : desc.name;
    Collection coll;
    coll.name = "Smoke Test";
    ResultStore store;
    store.reset (ctx.plans, s, inst, coll);
    ResultStore::PluginInfo info;
    info.name = inst.name;
    store.setPluginInfo (info);
    ctx.store = &store;

    std::vector<std::unique_ptr<RenderWorker>> workers;
    for (int w = 0; w < numWorkers; ++w)
    {
        auto p = makeInstance();
        p->enableAllBuses();
        p->setStateInformation (state.getData(), (int) state.getSize());
        p->setNonRealtime (true);
        p->prepareToPlay (s.sampleRate, s.blockSize);
        workers.push_back (std::make_unique<RenderWorker> (w, std::move (p), ctx));
        if (withState) // like the app: bring instances up one at a time
        {
            workers.back()->startThread();
            while (ctx.loadedWorkers.load() <= w && ! workers.back()->finished())
                juce::Thread::sleep (50);
        }
    }

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    for (auto& w : workers)
        if (! w->isThreadRunning() && ! w->finished())
            w->startThread();
    while (! std::all_of (workers.begin(), workers.end(), [] (auto& w) { return w->finished(); }))
        juce::Thread::sleep (20);
    workers.clear();
    for (auto& l : store.drainLog())
        std::printf ("  %s\n", l.toRawUTF8());
    std::printf ("%lld / %lld jobs, %lld hits in %.2f s\n", (long long) store.doneJobs(), (long long) store.totalJobs(),
                 (long long) store.hitsRendered(), (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0);
    auto r = store.writeJson (out);
    std::printf ("%s\n", r.wasOk() ? ("wrote " + out.getFullPathName()).toRawUTF8() : r.getErrorMessage().toRawUTF8());
    if (! fake)
        return store.doneJobs() == store.totalJobs() && r.wasOk() ? 0 : 1;

    // ------------------------------------------------------------------ assertions (fake sampler)
    const auto json = store.toJson();
    const auto det = json["determinismCheck"];
    std::printf ("\n[determinism check]  passed %s, renders %d\n", det["passed"].toString().toRawUTF8(), (int) det["renders"]);
    if (expectNever)
    {
        check (det.isObject() && ! (bool) det["passed"] && (bool) det["forcedNever"], "the determinism check fails");
        check ((bool) json["render"]["rrNeverResets"], "the run switched to RR reset Never");
        bool warned = false;
        for (auto& w : *json["warnings"].getArray())
            warned |= w.toString().contains ("doesn't replay identically");
        check (warned, "warning raised");
        bool fast = false;
        for (auto& h : *json["articulations"][0]["hits"].getArray())
            fast |= (bool) h["fastRepetition"] || (bool) h["tailSubtracted"];
        check (! fast, "nothing replayed from the reset state (no fast repetition / tail subtraction)");
        std::printf ("\n%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
        return failures == 0 ? 0 : 1;
    }
    if (s.checkDeterminism)
        check (det.isObject() && (bool) det["passed"] && ! (bool) json["render"]["rrNeverResets"],
               "determinism check passed, the run keeps its RR reset");
    for (int i = 0; i < numArts; ++i)
    {
        const auto& fa = kFakeArts[i];
        if (fa.legato)
            continue;
        const auto a = json["articulations"][i];
        const auto rr = a["summary"]["roundRobin"];
        std::printf ("\n[%s]  detected %d, docs %d, confirmed %d, unconfirmed %d, never repeated %d\n", fa.name.toRawUTF8(),
                     (int) rr["detected"], (int) rr["docs"], (int) rr["cyclesConfirmed"], (int) rr["cyclesUnconfirmed"],
                     (int) rr["cyclesNeverRepeated"]);
        if (fa.randomRR)
        {
            check ((int) a["summary"]["distinctSamples"] <= 3 * 6, "random RR: never more distinct samples than exist");
            continue;
        }
        check ((int) rr["detected"] == fa.rrCount, "detected RR == actual " + juce::String (fa.rrCount));
        check ((int) rr["cyclesConfirmed"] == 6, "all 6 series (3 notes x 2 layers) confirmed");
        check ((int) a["summary"]["hitsPlayed"] == 6 * (fa.rrCount + 1 + s.confirmHits), "hits per series = RR + repeat + confirmations");

        // Slots are numbered in play order, which need not start at the sampler's RR 0 (the load
        // probe already advanced one key's counter) - so compare the *set* of delays per series.
        auto* hits = a["hits"].getArray();
        double worstRel = 0, worstRelC4 = 0, worstAbs = 0;
        for (int note : { 48, 60, 72 })
            for (int d = 0; d < 2; ++d)
            {
                std::vector<double> got, want;
                for (auto& h : *hits)
                    if ((int) h["note"] == note && (int) h["dynIndex"] == d && (int) h["duplicateOf"] < 0 && ! (bool) h["fastRepetition"])
                    {
                        got.push_back ((double) h["onsetMs"]["rel-20"]);
                        want.push_back (expectedDelayMs (fa, (int) want.size(), note, (int) h["velocity"]));
                    }
                std::sort (got.begin(), got.end());
                std::sort (want.begin(), want.end());
                for (size_t k = 0; k < got.size() && k < want.size(); ++k)
                {
                    // relative: same waveform shape per RR, only shifted -> differences must be exact
                    const double e = std::abs ((got[k] - got[0]) - (want[k] - want[0]));
                    worstRel = juce::jmax (worstRel, e);
                    if (note == 72)
                        worstRelC4 = juce::jmax (worstRelC4, e);
                    // absolute (C4, 523 Hz): rel-20 sits ~0.61 ms into the 3 ms raised-cosine rise
                    if (note == 72)
                        worstAbs = juce::jmax (worstAbs, std::abs (got[k] - (want[k] + 3.0 / juce::MathConstants<double>::pi * std::acos (0.8))));
                }
            }
        check (worstRelC4 < 0.02, "C4: RR-to-RR delay differences exact (worst " + juce::String (worstRelC4, 4) + " ms)");
        // C2/C3 pure tones with a 3 ms attack sit inside the smoothing blur -> sample-start accuracy
        check (worstRel < 0.15, "all notes: RR-to-RR differences within 0.15 ms (worst " + juce::String (worstRel, 4) + " ms)");
        check (worstAbs < 0.3, "absolute onset at C4 within 0.3 ms (worst " + juce::String (worstAbs, 4) + " ms)");

        // primary = end of attack: delay + where the 3 ms raised-cosine rise reaches -1 dB
        check (a["primaryMethod"].toString() == "attackEnd", "primary method is attackEnd");
        {
            const double endOffset = kRiseMs / juce::MathConstants<double>::pi * std::acos (1.0 - 2.0 * std::pow (10.0, -1.0 / 20.0));
            double worstEnd = 0;
            std::vector<double> got, want;
            for (auto& h : *hits)
                if ((int) h["note"] == 72 && (int) h["dynIndex"] == 1 && (int) h["duplicateOf"] < 0 && ! (bool) h["fastRepetition"])
                {
                    got.push_back ((double) h["onsetMs"]["attackEnd"]);
                    want.push_back (expectedDelayMs (fa, (int) want.size(), 72, (int) h["velocity"]) + endOffset);
                }
            std::sort (got.begin(), got.end());
            std::sort (want.begin(), want.end());
            for (size_t k = 0; k < got.size() && k < want.size(); ++k)
                worstEnd = juce::jmax (worstEnd, std::abs (got[k] - want[k]));
            check (! got.empty() && worstEnd < 0.4, "C4 attackEnd within 0.4 ms of the true end of attack (worst "
                                                        + juce::String (worstEnd, 4) + " ms)");
        }

        // Never: no fast-repetition series (it replays from a reset state)
        if (s.rrNeverResets)
            check (! a["negativeDelayMsTable"]["C4"]["f"]["fastRepetition"].isObject(), "Never: no fast-repetition series");
        // fast repetition: tail-subtracted, so the clean notes must match the main series' samples
        else
        {
            const auto fr = a["negativeDelayMsTable"]["C4"]["f"]["fastRepetition"];
            const auto main = a["negativeDelayMsTable"]["C4"]["f"]["cycle"];
            bool ok = fr.isObject() && fr["distinct"].size() == fa.rrCount;
            double worstFast = 0;
            if (ok)
            {
                std::vector<double> x, y;
                for (auto& v : *fr["distinct"].getArray()) x.push_back ((double) v);
                for (auto& v : *main.getArray()) y.push_back ((double) v);
                std::sort (x.begin(), x.end());
                std::sort (y.begin(), y.end());
                for (size_t k = 0; k < x.size() && k < y.size(); ++k)
                    worstFast = juce::jmax (worstFast, std::abs (x[k] - y[k]));
            }
            check (ok && worstFast < 0.05, "fast repetition (250 ms, tail-subtracted) finds the same " + juce::String (fa.rrCount)
                                               + " samples with the same delays (worst " + juce::String (worstFast, 4) + " ms)");
        }

        const auto cell = a["negativeDelayMsTable"]["C3"]["f"];
        check (cell["cycle"].isArray() && cell["cycle"].size() == fa.rrCount && ! cell["afterPause"].isVoid(),
               "table C3/f has afterPause + one cycle value per RR");
    }
    // ------------------------------------------------------------------ legato (fake ground truth)
    {
        const auto L = json["articulations"][kLegatoArt]["legato"];
        std::printf ("\n[Legato (fake)]  %d transitions, %d failed\n", (int) L["transitionCount"], (int) L["failed"]);
        check (L.isObject() && L["transitions"].isArray(), "legato results exported");
        auto* tr = L["transitions"].getArray();
        // 3 sources x 3 intervals (all targets playable) x 6 cells (2 velocity zones x 3 spacings) x 2 directions
        check (tr != nullptr && tr->size() == 3 * 3 * 6 * 2, "every pair, cell and direction measured ("
                                                               + juce::String (tr != nullptr ? tr->size() : 0) + " transitions)");
        std::map<juce::String, double> worstArrive, worstMid;
        int wrongType = 0, unusable = 0, notExact = 0;
        if (tr != nullptr)
            for (auto& t : *tr)
            {
                const int semis = (int) t["interval"];
                const int vel = (int) t["velocity"];
                const double gap = (double) t["gapMs"];
                const juce::String want = vel < 64 ? "Portamento" : gap < kSpeedZoneMs ? "Fast" : "Slur";
                wrongType += t["type"].toString() != want ? 1 : 0;
                if (! (bool) t["valid"])
                {
                    ++unusable;
                    std::printf ("    unusable: %s -> %s %s gap %.1f: %s\n", t["source"].toString().toRawUTF8(), t["target"].toString().toRawUTF8(),
                                 want.toRawUTF8(), gap, t["failure"].toString().toRawUTF8());
                    continue;
                }
                notExact += ((bool) t.getProperty ("referenceExact", true) && (bool) t.getProperty ("converged", true)) ? 0 : 1;
                const double I = 100.0 * std::abs (semis);
                const double band = juce::jmin (40.0, I / 3.0); // the analyzer's tolerance around each note
                double arrive = 0;
                if (want == "Portamento")
                {
                    // the glide starts from wherever the previous glide (towards this transition's source) had
                    // got to when this note-on came: a short gap interrupts it
                    double fromCents = I;
                    if ((int) t["chainIndex"] > 1 && gap < kGlideStartMs + kGlideMs)
                        fromCents = I * juce::jlimit (0.0, 1.0, (gap - kGlideStartMs) / kGlideMs);
                    arrive = fromCents > band ? kGlideStartMs + kGlideMs * (1.0 - band / fromCents) : 0.0; // enters the target band
                    if (fromCents >= I - 1.0)
                    {
                        const double mid = kGlideStartMs + 0.5 * kGlideMs;
                        worstMid[want] = juce::jmax (worstMid[want], std::abs ((double) t["ms"]["mid"] - mid));
                    }
                }
                else
                    arrive = want == "Fast" ? kFastJumpMs : slurJumpMs (semis);
                worstArrive[want] = juce::jmax (worstArrive[want], std::abs ((double) t["ms"]["arrive"] - arrive));
            }
        check (wrongType == 0, "legato type of every transition matches its velocity / speed zone");
        check (unusable == 0, "every transition measured");
        check (notExact == 0, "every render converged and every reference was exact");
        for (auto& [type, e] : worstArrive)
            check (e < 3.0, type + ": arrival within 3 ms of the truth (worst " + juce::String (e, 3) + " ms; slurs carry a 6 dB bow-change dip)");
        for (auto& [type, e] : worstMid)
            check (e < 3.0, type + ": glide midpoint within 3 ms (worst " + juce::String (e, 3) + " ms)");
        // table: type -> dynamic -> gap -> source -> interval, both directions
        const auto slur = L["negativeDelayMsTable"]["Slur"]["mf"];
        bool both = false;
        if (auto* g = slur.getDynamicObject())
            for (auto& gp : g->getProperties())
                both |= gp.value["C3"]["+12"].isObject() && gp.value["C4"]["-12"].isObject()
                        && std::abs ((double) gp.value["C3"]["+12"]["first"] + slurJumpMs (12)) < 3.0;
        check (both, "table has C3 +12 and C4 -12 (both directions) with the slur's negative delay");
    }

    bool mismatchWarned = false;
    for (auto& w : *json["warnings"].getArray())
        mismatchWarned |= w.toString().contains ("detected 3 round robins, docs say 2");
    check (mismatchWarned, "warning raised for docs RR mismatch");
    bool randomWarned = false;
    for (auto& w : *json["warnings"].getArray())
        randomWarned |= w.toString().startsWith ("Random 3")
                        && (w.toString().contains ("stable cycle") || w.toString().contains ("random order"));
    check (randomWarned, "warning raised for non-cycling (random) RR");

    std::printf ("\n%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
