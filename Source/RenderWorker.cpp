#include "RenderWorker.h"
#include "Planning.h"

namespace tp
{
RenderWorker::RenderWorker (int workerId, std::unique_ptr<juce::AudioPluginInstance> p, RunContext& c)
    : juce::Thread ("RenderWorker " + juce::String (workerId)), id (workerId), plugin (std::move (p)), ctx (c)
{
    // channels we analyse = the main output bus (mic mix); extra multi-outs are rendered but ignored
    if (auto* bus = plugin->getBus (false, 0))
        mainChannels = juce::jmax (1, bus->getNumberOfChannels());
    const int bufferChannels = juce::jmax (plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels(), mainChannels);
    block.setSize (bufferChannels, ctx.settings.blockSize);
    playHead.now = &now;
    playHead.sampleRate = ctx.settings.sampleRate;
    plugin->setPlayHead (&playHead);
}

RenderWorker::~RenderWorker()
{
    stopThread (10000);
    if (plugin != nullptr)
        plugin->setPlayHead (nullptr);
}

juce::Optional<juce::AudioPlayHead::PositionInfo> RenderWorker::PlayHead::getPosition() const
{
    // Tempo-synced patches (runs, repetitions) need a tempo; report a steady 120 bpm transport.
    PositionInfo info;
    const int64_t t = *now - origin;
    info.setBpm (120.0);
    info.setTimeSignature (TimeSignature { 4, 4 });
    info.setIsPlaying (true);
    info.setTimeInSamples (t);
    info.setTimeInSeconds ((double) t / sampleRate);
    info.setPpqPosition ((double) t / sampleRate * 2.0);
    info.setPpqPositionOfLastBarStart (0.0);
    return info;
}

// ---------------------------------------------------------------------------- timeline

void RenderWorker::schedule (const juce::MidiMessage& m, int64_t time)
{
    if (m.isNoteOn())
        lastNoteOn = juce::jmax (lastNoteOn, juce::jmax (time, now));
    pending.emplace_back (juce::jmax (time, now), m);
    std::stable_sort (pending.begin(), pending.end(), [] (auto& a, auto& b) { return a.first < b.first; });
}

void RenderWorker::pace()
{
    if (ctx.settings.maxSpeed <= 0.0)
        return;
    const double audioSec = (double) now / ctx.settings.sampleRate;
    const double wantWall = wallStart + audioSec / ctx.settings.maxSpeed;
    const double wall = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    if (wantWall > wall)
        juce::Thread::sleep ((int) ((wantWall - wall) * 1000.0));
}

void RenderWorker::advanceTo (int64_t end, Capture* capture)
{
    while (now < end && ! threadShouldExit())
    {
        const int n = (int) juce::jmin<int64_t> (ctx.settings.blockSize, end - now);
        midi.clear();
        size_t used = 0;
        for (; used < pending.size() && pending[used].first < now + n; ++used)
            midi.addEvent (pending[used].second, (int) (pending[used].first - now));
        pending.erase (pending.begin(), pending.begin() + (long) used);

        juce::AudioBuffer<float> view (block.getArrayOfWritePointers(), block.getNumChannels(), n);
        view.clear();
        plugin->processBlock (view, midi);

        if (capture != nullptr)
        {
            auto& cb = capture->buffer;
            const int64_t cStart = capture->start, cEnd = capture->start + cb.getNumSamples();
            const int64_t s = juce::jmax (now, cStart), e = juce::jmin (now + n, cEnd);
            if (e > s)
                for (int ch = 0; ch < juce::jmin (mainChannels, cb.getNumChannels()); ++ch)
                    cb.copyFrom (ch, (int) (s - cStart), view, ch, (int) (s - now), (int) (e - s));
        }
        now += n;
        pace();
    }
}

bool RenderWorker::renderUntilSilent (double maxMs)
{
    const auto thr = juce::Decibels::decibelsToGain ((float) ctx.settings.silenceDb);
    const int64_t needQuiet = (int64_t) msToSamples (50.0);
    const int64_t limit = now + (int64_t) msToSamples (maxMs);
    int64_t quiet = 0;
    while (now < limit && ! threadShouldExit())
    {
        Capture c;
        c.start = now;
        c.buffer.setSize (mainChannels, ctx.settings.blockSize);
        c.buffer.clear();
        advanceTo (now + ctx.settings.blockSize, &c);
        float mag = 0;
        for (int ch = 0; ch < c.buffer.getNumChannels(); ++ch)
            mag = juce::jmax (mag, c.buffer.getMagnitude (ch, 0, c.buffer.getNumSamples()));
        quiet = mag < thr ? quiet + ctx.settings.blockSize : 0;
        if (quiet >= needQuiet)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------- musical actions

// Keyswitch (again) + CC1. Re-sending the keyswitch resets SINE's per-articulation playback state: after a
// CC1 change without it, renders were not repeatable and the first notes could come out silent.
void RenderWorker::selectArticulation (int cc1)
{
    if (currentKeyswitch >= 0)
    {
        schedule (juce::MidiMessage::noteOn (1, currentKeyswitch, (juce::uint8) 100), now);
        schedule (juce::MidiMessage::noteOff (1, currentKeyswitch), now + (int64_t) msToSamples (30.0));
    }
    schedule (juce::MidiMessage::controllerEvent (1, 1, cc1), now + (int64_t) msToSamples (40.0));
    advanceTo (now + (int64_t) msToSamples (50.0), nullptr);
}

void RenderWorker::sendKeyswitch (int note)
{
    currentKeyswitch = note;
    if (note < 0)
        return;
    schedule (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), now);
    schedule (juce::MidiMessage::noteOff (1, note), now + (int64_t) msToSamples (40.0));
    advanceTo (now + (int64_t) msToSamples (ctx.settings.keyswitchSettleMs), nullptr);
}

// Players that reset their round robins on a key rather than after a pause: the key ends every reset
// pause. Ends on a block boundary, so what follows sits on the same grid as without a key.
void RenderWorker::sendResetKey()
{
    const int key = ctx.settings.rrResetKey;
    if (key < 0)
        return;
    schedule (juce::MidiMessage::noteOn (1, key, (juce::uint8) 100), now);
    schedule (juce::MidiMessage::noteOff (1, key), now + (int64_t) msToSamples (40.0));
    advanceTo (alignUp (now + (int64_t) msToSamples (ctx.settings.keyswitchSettleMs)), nullptr);
}

bool RenderWorker::playHit (int note, int velocity, int cc1, Category cat, Capture& out)
{
    const auto& s = ctx.settings;
    const auto& timing = s.timing.at (cat);
    const int64_t tOn = now + (int64_t) msToSamples (s.prerollMs);
    const int64_t hold = (int64_t) msToSamples (timing.captureMs);
    const int pre = (int) msToSamples (s.preCaptureMs);

    schedule (juce::MidiMessage::controllerEvent (1, 1, cc1), now);
    schedule (juce::MidiMessage::noteOn (1, note, (juce::uint8) velocity), tOn);
    schedule (juce::MidiMessage::noteOff (1, note), tOn + hold);

    out.start = tOn - pre;
    out.buffer.setSize (mainChannels, pre + (int) hold + plugin->getLatencySamples(), false, false, true);
    out.buffer.clear();
    // End a block exactly at tOn so the note-on lands at sample offset 0 of a processBlock call.
    // Some samplers quantise intra-block event positions (SINE: a few samples), which made the
    // first hit of a series start 3 samples earlier than the rest.
    advanceTo (tOn, &out);
    advanceTo (out.start + out.buffer.getNumSamples(), &out);

    if (s.sendAllSoundOff)
        schedule (juce::MidiMessage::allSoundOff (1), now);
    return renderUntilSilent (s.tailMaxMs);
}

bool RenderWorker::waitForLoad()
{
    // SINE loads samples asynchronously after setStateInformation. Probe with a
    // mid-range note until it actually sounds, then let it settle.
    if (ctx.jobs.empty())
        return true;
    const auto& plan = ctx.plans.front();
    const int note = plan.notes[plan.notes.size() / 2];
    const int layer = plan.art.dynamics.size() - 1;

    const double startSec = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    OnsetParams op = ctx.settings.onset;
    op.preSamples = (int) msToSamples (ctx.settings.preCaptureMs);
    op.latencySamples = plugin->getLatencySamples();
    op.peakWindowMs = ctx.settings.timing.at (plan.category).peakWindowMs;
    op.fundamentalHz = midiToHz (note);

    while (! threadShouldExit())
    {
        sendKeyswitch (plan.keyswitch);
        Capture c;
        playHit (note, plan.velocityToSend (layer), plan.cc1For (layer), plan.category, c);
        if (analyseOnset (c.buffer, op).valid)
        {
            ctx.store->log ("worker " + juce::String (id) + ": instrument is sounding, settling "
                            + juce::String (ctx.settings.settleSec, 1) + " s");
            wait ((int) (ctx.settings.settleSec * 1000.0));
            return true;
        }
        if (juce::Time::getMillisecondCounterHiRes() / 1000.0 - startSec > ctx.settings.loadTimeoutSec)
        {
            ctx.store->log ("worker " + juce::String (id) + ": instrument never produced sound (timeout). "
                            "Check the keyswitch base and that the instrument is loaded.");
            return false;
        }
        wait (1000); // real time, so the plugin's loader threads can make progress
    }
    return false;
}

void RenderWorker::playTimedHit (int note, int velocity, int64_t tOn, int64_t hold, int captureLen, Capture& out)
{
    const int pre = (int) msToSamples (ctx.settings.preCaptureMs);
    schedule (juce::MidiMessage::noteOn (1, note, (juce::uint8) velocity), tOn);
    schedule (juce::MidiMessage::noteOff (1, note), tOn + hold);
    out.start = tOn - pre;
    out.buffer.setSize (mainChannels, pre + captureLen + plugin->getLatencySamples(), false, false, true);
    out.buffer.clear();
    // a block ends exactly at tOn, so the note-on lands at offset 0 of a processBlock call
    advanceTo (tOn, &out);
    advanceTo (out.start + out.buffer.getNumSamples(), &out);
}

int64_t RenderWorker::alignUp (int64_t t) const
{
    const int64_t bs = ctx.settings.blockSize;
    return ((t + bs - 1) / bs) * bs;
}

// note spacing as a whole number of blocks: every note-on then starts a processBlock call
int64_t RenderWorker::blockInterval (double ms) const
{
    const int64_t bs = ctx.settings.blockSize;
    return juce::jmax<int64_t> (bs, (int64_t) std::round (msToSamples (ms) / (double) bs) * bs);
}

namespace
{
    // Collects the hits of one series and decides when the round-robin cycle has closed.
    struct SeriesBuilder
    {
        SeriesBuilder (const RunSettings& settings, int start, int len) : s (settings), simStart (start), simLen (len) {}

        const RunSettings& s;
        int simStart, simLen;
        std::vector<juce::AudioBuffer<float>> caps;
        std::vector<Hit> hits;
        Series info;
        std::vector<int> resolved; // indices of identifiable hits, in play order
        int lastNewSample = 0, distinctSoFar = 0;
        int hypothesis = 0, lined = 0;
        int expectedPeriod = 0; // the player's RR count (0 = unknown): enables the cycle-position prior
        // the count is only the preset's claim (not the player's): the prior waits until a hit has matched,
        // under the normal criteria, the hit exactly one such cycle earlier - a wrong count never gets there
        bool priorNeedsEvidence = false;
        bool periodSeen = false;

        int originalOf (int k) const { return hits[(size_t) k].duplicateOf < 0 ? k : hits[(size_t) k].duplicateOf; }

        enum Status { more, done, retry };
        bool allowRetry = true;

        // `retry`: a note came out silent mid-series - SINE then restarts its cycle, so start over
        Status add (Hit h, juce::AudioBuffer<float> buf)
        {
            const int k = (int) hits.size();
            if (k > 0 && ! h.onset.valid && allowRetry)
                return retry;
            int loose = -1, looseLag = 0;
            double looseSim = 0;
            for (int j = 0; j < k; ++j)
            {
                // compare with every identified hit (not just each sample's first): the previous note's
                // tail differs from hit to hit, so another member of the class may match better
                const auto& o = hits[(size_t) j];
                if (o.unresolved)
                    continue;
                const int cls = originalOf (j);
                // shift-tolerant: the after-pause hit is often the same recording started a few samples earlier.
                // Zero lag is judged on its own as well: a shifted alignment can score marginally higher on a
                // sustained tone, and shifted matches are held to a much tighter peak tolerance
                int lag = 0;
                const double simLag = similarityLagged (caps[(size_t) j], buf, simStart, simLen, s.rrMaxLagSamples, lag);
                const double sim0 = lag == 0 ? simLag : similarity (caps[(size_t) j], buf, simStart, simLen);
                const double simBest = juce::jmax (sim0, simLag);
                if (std::isnan (h.bestSimilarity) || simBest > h.bestSimilarity)
                {
                    h.bestSimilarity = simBest;
                    h.bestSimilarityTo = j;
                }
                const std::pair<double, int> candidates[] { { sim0, 0 }, { simLag, lag } };
                bool matched = false;
                for (int ci = 0; ci < (lag != 0 ? 2 : 1) && ! matched; ++ci)
                {
                    const auto [sim, cLag] = candidates[ci];
                    // a repeat of the same recording has the same peak level (to ~0.01 dB); different RRs of
                    // sustained notes can still correlate > 0.995 over the window, so similarity alone isn't enough
                    const double dPeakAll = (h.onset.valid && o.onset.valid) ? std::abs (h.onset.peakDb - o.onset.peakDb) : 0.0;
                    // shifted alignments only count for an essentially identical recording (same peak level):
                    // on sustained tones a shift can otherwise line up the carrier of a different take
                    // ...but the same recording replayed with a small random gain variation keeps its waveform
                    // (correlation is gain-invariant) and its end of attack
                    const bool sameShapeOtherGain = cLag == 0 && sim > s.rrGainInvariantSimilarity && h.onset.valid && o.onset.valid
                                                    && std::abs (h.onset.attackEndMs - o.onset.attackEndMs) < s.rrGainInvariantOnsetMs;
                    const bool samePeakLevel = dPeakAll < (cLag == 0 ? s.rrLoosePeakDb : s.rrLaggedPeakDb) || sameShapeOtherGain;
                    if (sim > s.rrMatchThreshold && samePeakLevel)
                    {
                        h.duplicateOf = cls;
                        h.matchSimilarity = sim;
                        h.matchLagSamples = cLag;
                        matched = true;
                        break;
                    }
                    if (loose < 0 && h.onset.valid && o.onset.valid)
                    {
                        const double dPeak = std::abs (h.onset.peakDb - o.onset.peakDb);
                        const double peakTol = cLag == 0 ? 1.0 : s.rrLaggedPeakDb / s.rrPeakMatchDb; // tighten for shifted matches
                        const bool fingerprint = sim > s.rrLooseSimilarity && dPeak < s.rrLoosePeakDb * juce::jmin (1.0, peakTol * 5.0)
                                                 && std::abs (h.onset.attackEndMs - o.onset.attackEndMs) < s.rrLooseOnsetMs;
                        const bool samePeak = sim > s.rrPeakMatchSimilarity && dPeak < s.rrPeakMatchDb * peakTol;
                        // residue left by a retriggered voice (or a loud tail): the waveform is disturbed, the peak isn't
                        const bool tailUncertain = juce::jmax (h.tailDb, o.tailDb) > s.tailUncertainDb
                                                   && sim > s.rrTailUncertainSimilarity && dPeak < s.rrLoosePeakDb && cLag == 0;
                        if (fingerprint || samePeak || tailUncertain)
                        {
                            loose = cls;
                            looseSim = sim;
                            looseLag = cLag;
                        }
                    }
                }
                if (matched)
                    break;
            }
            if (h.duplicateOf < 0 && loose >= 0)
            {
                h.duplicateOf = loose;
                h.matchSimilarity = looseSim;
                h.matchLagSamples = looseLag;
                h.looseMatch = true;
            }
            // the player's own RR count: the hit one (or two) cycles back should be this recording
            if (h.duplicateOf < 0 && expectedPeriod > 0 && h.onset.valid && (periodSeen || ! priorNeedsEvidence))
            {
                const int r = (int) resolved.size();
                for (int back = expectedPeriod; back <= 2 * expectedPeriod && r - back >= 0; back += expectedPeriod)
                {
                    const int j = resolved[(size_t) (r - back)];
                    const auto& o = hits[(size_t) j];
                    if (! o.onset.valid)
                        continue;
                    int lag = 0;
                    const double sim = similarityLagged (caps[(size_t) j], buf, simStart, simLen, s.rrMaxLagSamples, lag);
                    const double dPeak = std::abs (h.onset.peakDb - o.onset.peakDb);
                    // the end-of-attack estimate can flip between two near-equal bumps of the same recording,
                    // so a very close waveform + level match stands on its own
                    if (sim > s.rrPriorSimilarity && dPeak < s.rrPriorPeakDb
                        && (std::abs (h.onset.attackEndMs - o.onset.attackEndMs) < s.rrPriorOnsetMs || (sim > 0.99 && dPeak < 0.3)))
                    {
                        h.duplicateOf = originalOf (j);
                        h.matchSimilarity = sim;
                        h.matchLagSamples = lag;
                        h.looseMatch = true;
                        h.priorMatch = true;
                        break;
                    }
                }
            }
            // A silent hit after the first (only kept on the last retry) is unresolved: neither a new
            // sample nor a break of the cycle. Tail-masked hits stay identifiable - a tail is deterministic
            // (same predecessor -> same tail), so repeats still compare; only their onset is unreliable.
            if (h.duplicateOf < 0 && k > 0 && ! h.onset.valid)
            {
                h.unresolved = true;
                h.duplicateOf = -2;
            }
            hits.push_back (h);
            caps.push_back (std::move (buf));
            if (h.unresolved)
                return more;

            if (k == 0 && ! h.onset.valid)
            {
                info.silent = true;
                return done;
            }

            // cycle detection runs on the resolved hits only (positions in `resolved`)
            resolved.push_back (k);
            const int r = (int) resolved.size() - 1;
            if (! h.isOriginal() && ! h.priorMatch && expectedPeriod > 0 && r >= expectedPeriod
                && originalOf (resolved[(size_t) (r - expectedPeriod)]) == originalOf (k))
                periodSeen = true;
            if (h.isOriginal())
            {
                hypothesis = 0;
                lined = 0;
                lastNewSample = r;
                ++distinctSoFar;
                return more;
            }
            // random round robin: no stable cycle, but no new sample for a long stretch either
            if (r - lastNewSample >= juce::jmax (8, 3 * distinctSoFar) && ! info.confirmed)
            {
                info.randomOrder = true;
                info.period = 0;
                return done;
            }
            if (hypothesis > 0 && originalOf (resolved[(size_t) r]) == originalOf (resolved[(size_t) (r - hypothesis)]))
            {
                if (++lined >= juce::jmax (1, s.confirmHits))
                {
                    info.confirmed = true;
                    return done;
                }
                return more;
            }
            const int origPos = (int) (std::find (resolved.begin(), resolved.end(), h.duplicateOf) - resolved.begin());
            hypothesis = r - origPos;
            lined = 0;
            info.period = hypothesis;
            info.cycleStart = h.duplicateOf;
            return s.confirmCycle ? more : done;
        }

        void finish (std::vector<Hit>& out, std::vector<Series>& seriesOut)
        {
            int unique = 0;
            for (auto& h : hits)
                h.rr = h.unresolved ? -1 : h.isOriginal() ? unique++ : hits[(size_t) h.duplicateOf].rr;
            info.uniqueSamples = unique;
            info.hitsPlayed = (int) hits.size();
            if (! info.confirmed && s.confirmCycle)
                info.period = hypothesis;
            out.insert (out.end(), hits.begin(), hits.end());
            seriesOut.push_back (info);
        }
    };
} // namespace

// Which control switches this articulation's dynamic layers? Three probes, each from the reset
// state (so the same sample plays): a normalised waveform comparison ignores pure gain changes,
// so only a real layer change (a different sample) counts.
DynamicsSource RenderWorker::detectDynamicsSource (const ArticulationPlan& plan, int note)
{
    const auto& s = ctx.settings;
    const int captureLen = (int) msToSamples (300.0);
    auto probe = [&] (int vel, int cc1) {
        schedule (juce::MidiMessage::controllerEvent (1, 1, cc1), now);
        renderUntilSilent (s.tailMaxMs);
        advanceTo (alignUp (now + (int64_t) msToSamples (plan.resetPauseMs (s.rrResetPauseMs))), nullptr);
        sendResetKey();
        Capture c;
        playTimedHit (note, vel, now, (int64_t) msToSamples (250.0), captureLen, c);
        return c.buffer;
    };
    const auto ref = probe (110, 110), lowVel = probe (30, 110), lowCc = probe (110, 30);
    const int start = (int) msToSamples (s.preCaptureMs) + plugin->getLatencySamples();
    const int len = (int) msToSamples (s.similarityMs);
    const double simVel = similarity (ref, lowVel, start, len), simCc = similarity (ref, lowCc, start, len);
    auto level = [] (const juce::AudioBuffer<float>& b) { return juce::Decibels::gainToDecibels (b.getMagnitude (0, 0, b.getNumSamples()), -200.0f); };
    const bool velSwitches = simVel < 0.98, ccSwitches = simCc < 0.98;

    DynamicsSource result = DynamicsSource::velocity;
    juce::String why;
    if (ccSwitches && ! velSwitches)
        result = DynamicsSource::cc1, why = "CC1 changes the sample, velocity does not";
    else if (velSwitches && ! ccSwitches)
        why = "velocity changes the sample, CC1 does not";
    else if (velSwitches && ccSwitches)
    {
        // long notes crossfade layers on CC1 (velocity only shapes the attack); shorts switch on velocity
        if (plan.category != Category::shortNote)
            result = DynamicsSource::cc1;
        why = "both change the sample - using " + toString (result) + " for a " + toString (plan.category) + " articulation";
    }
    else if (std::abs (level (ref) - level (lowCc)) > std::abs (level (ref) - level (lowVel)))
        result = DynamicsSource::cc1, why = "single layer; CC1 changes the level";
    else
        why = "single layer";
    ctx.store->log ("worker " + juce::String (id) + ": " + plan.art.name + " dynamics -> " + toString (result) + " (" + why
                    + ", similarity vel " + juce::String (simVel, 3) + " / cc1 " + juce::String (simCc, 3) + ")");
    return result;
}

void RenderWorker::runJob (const Job& job)
{
    if (job.legatoPlan >= 0)
    {
        runLegatoJob (job.legatoPlan, job.note, job.legatoDyn);
        return;
    }
    // every articulation of this note, on this instance, in order: the instance's RR counters
    // only ever see this worker's sequence
    for (int pi = 0; pi < (int) ctx.plans.size() && ! threadShouldExit(); ++pi)
    {
        if (! ctx.plans[(size_t) pi].notes.contains (job.note))
            continue;
        auto plan = ctx.plans[(size_t) pi]; // local copy: the dynamics source may be resolved here
        sendKeyswitch (plan.keyswitch);
        if (plan.dynSource == DynamicsSource::autoDetect && ctx.settings.rrNeverResets)
        {
            // the probes compare renders from the reset state - without a reset they would hit different RRs
            plan.dynSource = DynamicsSource::velocity;
            if (resolvedDynamics.emplace (pi, plan.dynSource).second)
                ctx.store->setResolvedDynamics (pi, plan.dynSource);
        }
        if (plan.dynSource == DynamicsSource::autoDetect)
        {
            auto known = resolvedDynamics.find (pi);
            if (known == resolvedDynamics.end())
            {
                const auto& notes = plan.notes;
                known = resolvedDynamics.emplace (pi, detectDynamicsSource (plan, notes[notes.size() / 2])).first;
                ctx.store->setResolvedDynamics (pi, known->second);
            }
            plan.dynSource = known->second;
        }
        std::vector<Hit> out;
        std::vector<Series> seriesOut;
        for (int d = 0; d < plan.art.dynamics.size() && ! threadShouldExit(); ++d)
        {
            const auto& s = ctx.settings;
            const auto& timing = s.timing.at (plan.category);
            if (s.subtractTailsForLongNotes && ! s.rrNeverResets && plan.category != Category::shortNote && plan.category != Category::phrase)
            {
                // sustained notes: release tails are loud at any spacing below the RR restart time -> remove them exactly
                runSubtractedSeries (plan, job.note, d, blockInterval (juce::jmin (timing.intervalMs, s.rrMaxIntervalMs)),
                                     (int64_t) msToSamples (timing.holdMs), (int) msToSamples (timing.captureMs),
                                     s.maxHitsPerSeries, false, out, seriesOut);
            }
            else
            {
                const auto hitsBefore = out.size(), seriesBefore = seriesOut.size();
                runSeries (plan, job.note, d, out, seriesOut);
                const bool masked = std::any_of (out.begin() + (long) hitsBefore, out.end(), [] (const Hit& h) { return h.tailContaminated; });
                if (masked && s.subtractTailsWhenMasked && ! s.rrNeverResets && ! threadShouldExit())
                {
                    // low notes: the previous note's tail masks the onset at this spacing -> measure exactly instead
                    out.resize (hitsBefore);
                    seriesOut.resize (seriesBefore);
                    runSubtractedSeries (plan, job.note, d, blockInterval (juce::jmin (timing.intervalMs, s.rrMaxIntervalMs)),
                                         (int64_t) msToSamples (timing.holdMs), (int) msToSamples (timing.captureMs),
                                         s.maxHitsPerSeries, false, out, seriesOut);
                }
            }
            if (s.fastRepetition && ! s.rrNeverResets && plan.category == Category::shortNote && ! threadShouldExit())
            {
                const auto fastInterval = blockInterval (s.fastRepIntervalMs);
                runSubtractedSeries (plan, job.note, d, fastInterval, (int64_t) (0.8 * (double) fastInterval),
                                     (int) msToSamples (timing.captureMs), s.fastRepMaxHits, true, out, seriesOut);
            }
        }
        if (! threadShouldExit())
            ctx.store->addJob (pi, std::move (out), std::move (seriesOut));
    }
}

OnsetParams RenderWorker::onsetParamsFor (const ArticulationPlan& plan, int note, int captureLen) const
{
    const auto& s = ctx.settings;
    OnsetParams op = s.onset;
    op.preSamples = (int) msToSamples (s.preCaptureMs);
    op.latencySamples = plugin->getLatencySamples();
    op.fundamentalHz = midiToHz (note);
    op.peakWindowMs = juce::jmin (s.timing.at (plan.category).peakWindowMs, captureLen * 1000.0 / s.sampleRate);
    return op;
}

// One (articulation, note, dynamic layer): reset pause, then hits at the category's fixed spacing
// until a hit repeats an earlier one (the RR cycle closed) and confirmHits more line up with it.
void RenderWorker::runSeries (const ArticulationPlan& plan, int note, int d, std::vector<Hit>& out, std::vector<Series>& seriesOut)
{
    const auto& s = ctx.settings;
    const auto& timing = s.timing.at (plan.category);
    const int pre = (int) msToSamples (s.preCaptureMs);
    // spacing must stay below the reset time, or every hit would be the after-pause sample
    // (round robins that never reset automatically: notes follow each other once the previous one is silent - no tails to
    // remove, and nothing depends on the spacing)
    const bool untilSilent = s.rrNeverResets;
    const int64_t interval = blockInterval (juce::jmin (timing.intervalMs, s.rrMaxIntervalMs));
    const int captureLen = untilSilent ? (int) msToSamples (timing.captureMs)
                                       : (int) juce::jmin<int64_t> ((int64_t) msToSamples (timing.captureMs), interval - pre - (int64_t) msToSamples (20.0));
    const int64_t hold = juce::jmin<int64_t> ((int64_t) msToSamples (timing.holdMs), captureLen);
    const auto op = onsetParamsFor (plan, note, captureLen);

    for (int attempt = 0; attempt <= s.rrSeriesRetries && ! threadShouldExit(); ++attempt)
    {
        // keyswitch + layer's CC1, then the reset pause (also lets CC smoothing and earlier tails finish)
        selectArticulation (plan.cc1For (d));
        renderUntilSilent (s.tailMaxMs); // earlier tails first: the pause may be as short as the RR reset time
        advanceTo (now + (int64_t) msToSamples (plan.resetPauseMs (s.rrResetPauseMs)), nullptr);
        sendResetKey();
        const int64_t t0 = alignUp (now + (int64_t) msToSamples (s.prerollMs));
        // transport restarts at the first note-on: tempo-synced patches (repetitions, runs) then start every
        // note on a beat (1000 ms spacing = 2 beats at 120 bpm) instead of wherever the transport happens to be
        playHead.origin = t0;

        SeriesBuilder sb (s, op.preSamples + op.latencySamples, (int) msToSamples (s.similarityMs));
        sb.info.note = note;
        sb.info.dynIndex = d;
        sb.info.intervalMs = (double) interval * 1000.0 / s.sampleRate;
        sb.info.retries = attempt;
        sb.allowRetry = attempt < s.rrSeriesRetries;
        sb.expectedPeriod = s.useRRPrior ? plan.priorRoundRobins (s.useRRPriorFromPreset) : 0;
        sb.priorNeedsEvidence = ! plan.hasSine;
        bool restart = false, maskedOnset = false;
        int64_t nextOn = t0;

        for (int k = 0; k < s.maxHitsPerSeries && ! threadShouldExit(); ++k)
        {
            const int64_t tOn = untilSilent ? nextOn : t0 + k * interval;
            if (untilSilent)
                playHead.origin = tOn; // every note on beat 0, like the fixed spacing's beat grid
            Hit h;
            h.note = note;
            h.dynIndex = d;
            h.seq = k;
            h.afterPause = k == 0;
            h.velocity = plan.velocityToSend (d);
            h.cc1 = plan.cc1For (d);
            Capture c;
            playTimedHit (note, h.velocity, tOn, hold, captureLen, c);
            if (untilSilent)
            {
                advanceTo (juce::jmax (now, tOn + hold), nullptr);
                renderUntilSilent (s.tailMaxMs);
                nextOn = alignUp (now + (int64_t) msToSamples (s.prerollMs));
            }
            h.onset = analyseOnset (c.buffer, op);
            if (h.onset.valid)
            {
                h.tailDb = h.onset.preRollDb - h.onset.peakDb;
                h.tailContaminated = h.tailDb > s.tailContaminationDb;
            }
            else if (k > 0 && s.subtractTailsWhenMasked
                     && c.buffer.getMagnitude (0, c.buffer.getNumSamples()) > juce::Decibels::decibelsToGain (-70.0f))
            {
                // not silent: the previous note's tail is so loud that this onset can't rise above it
                // (repetition phrases). Re-measure the whole series with exact tail subtraction.
                h.tailContaminated = true;
                maskedOnset = true;
                out.push_back (h);
                break;
            }
            const auto status = sb.add (h, std::move (c.buffer));
            restart = status == SeriesBuilder::retry;
            if (status != SeriesBuilder::more)
                break;
        }
        // let the last note's hold finish before anything else is sent
        advanceTo (juce::jmax (now, t0 + (int64_t) sb.hits.size() * interval), nullptr);
        if (maskedOnset)
            return; // the caller sees a tail-contaminated hit and re-runs the series with subtraction
        if (restart)
        {
            ctx.store->log ("worker " + juce::String (id) + ": " + plan.art.name + " " + noteName (note) + " "
                            + plan.art.dynamics[d] + ": a note came out silent - restarting the series");
            continue;
        }
        sb.finish (out, seriesOut);
        return;
    }
}

// Renders `count` notes at `interval` after a reset pause, returning the audio from note
// (count-1)'s capture start to the end of where note `count`'s capture would be. Used in pairs:
// render(k+1)[window k] - render(k)[window k] = note k alone (the tails cancel exactly, because
// the plugin renders deterministically from its reset state).
juce::AudioBuffer<float> RenderWorker::renderSequence (int note, int velocity, int cc1, int count, int64_t interval,
                                                       int64_t hold, int windowLen, int64_t& windowStartOut)
{
    const auto& s = ctx.settings;
    const int pre = (int) msToSamples (s.preCaptureMs);
    selectArticulation (cc1);
    renderUntilSilent (s.tailMaxMs);
    advanceTo (alignUp (now + (int64_t) msToSamples (juce::jmax (s.rrResetPauseMs + 1000.0, playerResetMs))), nullptr);
    sendResetKey();
    const int64_t t0 = now;
    playHead.origin = t0; // first note-on on beat 0 (see runSeries)
    for (int i = 0; i < count; ++i)
    {
        schedule (juce::MidiMessage::noteOn (1, note, (juce::uint8) velocity), t0 + i * interval);
        schedule (juce::MidiMessage::noteOff (1, note), t0 + i * interval + hold);
    }
    Capture big;
    big.start = t0 + juce::jmax (0, count - 1) * interval - pre;
    windowStartOut = big.start;
    big.buffer.setSize (mainChannels, (int) (interval + windowLen), false, false, true);
    big.buffer.clear();
    advanceTo (big.start + big.buffer.getNumSamples(), &big);
    return big.buffer; // pending note-offs are delivered at the start of the next render's pause
}

// Series with exact tail removal: render(k+1)[window k] - render(k)[window k] = note k alone.
// Used for fast repetition (notes overlap) and for long / dynamic articulations (loud release tails).
void RenderWorker::runSubtractedSeries (const ArticulationPlan& plan, int note, int d, int64_t interval, int64_t hold,
                                        int captureLen, int maxHits, bool fast, std::vector<Hit>& out, std::vector<Series>& seriesOut)
{
    playerResetMs = plan.resetPauseMs (0.0);
    const auto& s = ctx.settings;
    const int pre = (int) msToSamples (s.preCaptureMs);
    const int windowLen = pre + captureLen + plugin->getLatencySamples();
    const auto op = onsetParamsFor (plan, note, captureLen);

    const int vel = plan.velocityToSend (d), cc = plan.cc1For (d);
    for (int attempt = 0; attempt <= s.rrSeriesRetries && ! threadShouldExit(); ++attempt)
    {
        SeriesBuilder sb (s, op.preSamples + op.latencySamples, (int) msToSamples (s.similarityMs));
        sb.info.note = note;
        sb.info.dynIndex = d;
        sb.info.fastRepetition = fast;
        sb.info.intervalMs = (double) interval * 1000.0 / s.sampleRate;
        sb.info.retries = attempt;
        sb.allowRetry = attempt < s.rrSeriesRetries;
        sb.expectedPeriod = s.useRRPrior ? plan.priorRoundRobins (s.useRRPriorFromPreset) : 0;
        sb.priorNeedsEvidence = ! plan.hasSine;
        bool restart = false;

        juce::AudioBuffer<float> tailOfPrevious; // render(k)[window k], empty for k = 0
        for (int k = 0; k < maxHits && ! threadShouldExit(); ++k)
        {
            int64_t ws = 0;
            auto r = renderSequence (note, vel, cc, k + 1, interval, hold, windowLen, ws);
            // window k (note k present, nothing after it) and window k+1 (tail only) of this render
            juce::AudioBuffer<float> withNote (mainChannels, windowLen), nextTail (mainChannels, windowLen);
            for (int c = 0; c < mainChannels; ++c)
            {
                withNote.copyFrom (c, 0, r, c, 0, windowLen);
                nextTail.copyFrom (c, 0, r, c, (int) interval, windowLen);
                if (tailOfPrevious.getNumSamples() == windowLen)
                    withNote.addFrom (c, 0, tailOfPrevious, c, 0, windowLen, -1.0f); // subtract the earlier notes' tails
            }
            tailOfPrevious = std::move (nextTail);

            Hit h;
            h.note = note;
            h.dynIndex = d;
            h.seq = k;
            h.afterPause = k == 0;
            h.fastRepetition = fast;
            h.tailSubtracted = k > 0;
            h.velocity = vel;
            h.cc1 = cc;
            h.onset = analyseOnset (withNote, op);
            if (h.onset.valid)
            {
                h.tailDb = h.onset.preRollDb - h.onset.peakDb; // residual before the onset after subtraction
                h.tailContaminated = h.tailDb > s.tailContaminationDb;
            }
            const auto status = sb.add (h, std::move (withNote));
            restart = status == SeriesBuilder::retry;
            if (status != SeriesBuilder::more)
                break;
        }
        if (restart)
        {
            ctx.store->log ("worker " + juce::String (id) + ": " + plan.art.name + " " + noteName (note) + " "
                            + plan.art.dynamics[d] + (fast ? " (fast)" : "") + ": a note came out silent - restarting the series");
            continue;
        }
        sb.finish (out, seriesOut);
        return;
    }
}

// ---------------------------------------------------------------------------- legato

// A legato chain from the reset state: notes[0..count-1] at `step` spacing, each note-on overlapping
// the previous note (released legatoOverlapMs later), the last note held to the end of the capture.
// Returns the audio from (t0 - pre) to (t0 + count * step + captureLen + latency): long enough for
// this render to serve as the "without note `count`" reference of the next longer chain.
juce::AudioBuffer<float> RenderWorker::renderChain (const std::vector<int>& notes, int count, int velocity, int cc1, int64_t step,
                                                    int captureLen)
{
    const auto& s = ctx.settings;
    const int pre = legatoPreSamples();
    const int latency = plugin->getLatencySamples();
    // Keyswitch before every render: it resets SINE's per-articulation playback state. Without it, renders
    // after a CC1 change were not repeatable (and the first notes after the change could come out silent).
    selectArticulation (cc1);
    // previous chain's release tail, then the reset time counted from the last note-on
    renderUntilSilent (s.tailMaxMs);
    // (renderUntilSilent ran after the last note-off, so `now` is past it: the player's RR reset time,
    // counted from there, returns its round-robin counter to the start)
    advanceTo (alignUp (juce::jmax (juce::jmax (now + (int64_t) msToSamples (s.cc1SettleMs), now + (int64_t) msToSamples (playerResetMs)),
                                    lastNoteOn + (int64_t) msToSamples (s.legatoResetPauseMs))),
               nullptr);
    sendResetKey();
    // the transport restarts here for every render: tempo / beat-position dependent behaviour (SINE's
    // legato speed, synced patches) then sees exactly the same timeline each time
    const int64_t t0 = alignUp (now + pre);
    playHead.origin = t0;
    const auto overlap = (int64_t) msToSamples (s.legatoOverlapMs);
    for (int i = 0; i < count; ++i)
    {
        schedule (juce::MidiMessage::noteOn (1, notes[(size_t) i], (juce::uint8) velocity), t0 + i * step);
        if (i > 0 && notes[(size_t) i] != notes[(size_t) i - 1])
            schedule (juce::MidiMessage::noteOff (1, notes[(size_t) i - 1]), t0 + i * step + overlap);
    }
    Capture cap;
    cap.start = t0 - pre;
    cap.buffer.setSize (mainChannels, (int) (pre + count * step + captureLen + latency), false, false, true);
    cap.buffer.clear();
    advanceTo (cap.start + cap.buffer.getNumSamples(), &cap);
    schedule (juce::MidiMessage::noteOff (1, notes[(size_t) count - 1]), now);
    return std::move (cap.buffer);
}

juce::AudioBuffer<float> RenderWorker::renderChainConverged (const std::vector<int>& notes, int count, int velocity, int cc1, int64_t step,
                                                             int captureLen, int& renders, bool& converged)
{
    auto identical = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b) {
        if (a.getNumSamples() != b.getNumSamples() || a.getNumChannels() != b.getNumChannels())
            return false;
        for (int c = 0; c < a.getNumChannels(); ++c)
            if (std::memcmp (a.getReadPointer (c), b.getReadPointer (c), sizeof (float) * (size_t) a.getNumSamples()) != 0)
                return false;
        return true;
    };
    const bool trace = juce::SystemStats::getEnvironmentVariable ("LEGATO_TRACE", "0") == "2";
    auto describe = [&] (const juce::AudioBuffer<float>& b, const juce::AudioBuffer<float>* prevB) {
        const int pre = legatoPreSamples();
        juce::String t = "render n=" + juce::String (count) + " " + noteName (notes[0]) + " cc " + juce::String (cc1) + " at "
                         + juce::String ((double) now / ctx.settings.sampleRate, 2) + " s: A onset level "
                         + juce::String (juce::Decibels::gainToDecibels (b.getMagnitude (pre, (int) msToSamples (200.0)), -200.0f), 2) + " dB";
        if (prevB != nullptr)
        {
            float md = 0;
            int first = -1;
            for (int c = 0; c < b.getNumChannels(); ++c)
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    const float dd = std::abs (b.getSample (c, i) - prevB->getSample (c, i));
                    if (dd > 0 && (first < 0 || i < first))
                        first = i;
                    md = juce::jmax (md, dd);
                }
            t << ", vs previous " << juce::String (juce::Decibels::gainToDecibels (md, -200.0f), 1) << " dB from "
              << juce::String ((first - pre) * 1000.0 / ctx.settings.sampleRate, 1) << " ms";
        }
        ctx.store->log (t);
    };
    auto prev = renderChain (notes, count, velocity, cc1, step, captureLen);
    ++renders;
    if (trace)
        describe (prev, nullptr);
    for (int attempt = 1; attempt < juce::jmax (2, ctx.settings.legatoMaxRenders) && ! threadShouldExit(); ++attempt)
    {
        auto cur = renderChain (notes, count, velocity, cc1, step, captureLen);
        ++renders;
        if (trace)
            describe (cur, &prev);
        if (identical (cur, prev))
            return cur;
        prev = std::move (cur);
        wait (20); // wall-clock time for the plugin's sample loader to catch up
    }
    converged = false;
    return prev;
}

void RenderWorker::runLegatoJob (int planIdx, int note, int d)
{
    const auto& s = ctx.settings;
    const auto& plan = ctx.plans[(size_t) planIdx];
    playerResetMs = plan.resetPauseMs (0.0);
    std::vector<LegatoHit> out;
    sendKeyswitch (plan.keyswitch);

    const int latency = plugin->getLatencySamples();
    const int pre = legatoPreSamples();
    const int captureLen = (int) msToSamples (s.legatoCaptureMs);
    // layers: CC1 for legato patches (velocity picks the legato type); a velocity-layered patch uses its layer velocity
    const bool velocityLayers = plan.dynSource == DynamicsSource::velocity;
    const int cc = velocityLayers ? 100 : plan.cc1For (d);
    // round robins: the player's count (else the docs'); chains alternate two notes, so with an even
    // count one direction only meets every other slot - a second chain starting on the other note covers the rest
    const auto shape = legatoChainShape (plan, s);
    const int R = shape.roundRobins;
    const int K = shape.transitions;
    // progress inside the job (a job can take minutes): credit each chain step as it completes
    auto credit = [&] (int notesInChain, double gap) {
        // never more than the job's estimate (renders repeated until identical may exceed it)
        const auto milli = juce::jmin ((int64_t) std::llround (1000.0 * legatoRenderUnits (plan, s, notesInChain, gap)),
                                       juce::jmax ((int64_t) 0, currentJobMilliUnits - creditedMilliUnits));
        ctx.doneMilliUnits += milli;
        creditedMilliUnits += milli;
    };

    LegatoParams lp = s.legato;
    lp.sampleRate = s.sampleRate;
    lp.latencySamples = latency;
    lp.stableToMs = juce::jmin (lp.stableToMs, s.legatoCaptureMs);
    lp.stableFromMs = juce::jmin (lp.stableFromMs, 0.5 * lp.stableToMs);

    juce::Array<int> intervals = s.legatoIntervals;
    if (s.legatoUnison)
        intervals.insert (0, 0);

    for (int ci = 0; ci < (int) plan.legatoCells.size() && ! threadShouldExit(); ++ci)
    {
        const auto& cell = plan.legatoCells[(size_t) ci];
        const int vel = velocityLayers ? plan.velocityFor (d) : cell.velocity;
        const int64_t step = blockInterval (cell.gapMs);
        const double gapMs = (double) step * 1000.0 / s.sampleRate;
        std::map<int, std::pair<juce::AudioBuffer<float>, int>> singleNote; // start note -> (render of it alone, renders)

        for (auto i : intervals)
        {
            if (threadShouldExit())
                break;
            const int other = note + i;
            if (! plan.playable.contains (other))
                continue;
            std::vector<int> starts { note };
            if (shape.bothStarts && other != note)
                starts.push_back (other);
            for (auto first : starts)
            {
                const int second = first == note ? other : note;
                std::vector<int> notes;
                for (int k = 0; k <= K; ++k)
                    notes.push_back (k % 2 == 0 ? first : second);

                // Render once per chain length. Render k+1 repeats render k exactly up to note k when the
                // plugin is deterministic, so comparing that shared prefix verifies render k (it was the
                // repeat-until-identical check before, at twice the cost). Only on a mismatch are both
                // lengths re-rendered until identical. The last render has no longer one after it, so it
                // is still rendered until identical.
                const int len = pre + captureLen + latency;
                auto window = [&] (const juce::AudioBuffer<float>& src, int k) {
                    juce::AudioBuffer<float> w (mainChannels, len);
                    for (int c = 0; c < mainChannels; ++c)
                        w.copyFrom (c, 0, src, c, (int) (k * step), len);
                    return w;
                };
                auto prefixEqual = [&] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int k) {
                    // samples before note k reaches the output (render start .. note k + latency)
                    const int n = (int) (k * step) + pre + latency;
                    if (a.getNumSamples() < n || b.getNumSamples() < n)
                        return false;
                    for (int c = 0; c < mainChannels; ++c)
                        if (std::memcmp (a.getReadPointer (c), b.getReadPointer (c), sizeof (float) * (size_t) n) != 0)
                            return false;
                    return true;
                };
                auto identical = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b) {
                    if (a.getNumSamples() != b.getNumSamples())
                        return false;
                    for (int c = 0; c < a.getNumChannels(); ++c)
                        if (std::memcmp (a.getReadPointer (c), b.getReadPointer (c), sizeof (float) * (size_t) a.getNumSamples()) != 0)
                            return false;
                    return true;
                };

                int renders = 0;
                juce::AudioBuffer<float> prev, older; // render(notes 0..k-1), render(notes 0..k-2)
                if (auto it = singleNote.find (first); it != singleNote.end())
                    prev.makeCopyOf (it->second.first);
                else
                {
                    prev = renderChain (notes, 1, vel, cc, step, captureLen);
                    renders = 1;
                    singleNote[first] = { juce::AudioBuffer<float> (prev), 1 };
                }
                credit (1, cell.gapMs);
                for (int k = 1; k <= K && ! threadShouldExit(); ++k)
                {
                    const bool last = k == K;
                    int rk = 0;
                    bool convK = true;
                    juce::AudioBuffer<float> cur;
                    if (last)
                        cur = renderChainConverged (notes, k + 1, vel, cc, step, captureLen, rk, convK);
                    else
                    {
                        cur = renderChain (notes, k + 1, vel, cc, step, captureLen);
                        rk = 1;
                        // test hook: LEGATO_INJECT_FAULT=1 corrupts the first render of every chain's second note
                        // (k = 1), which must be caught by the prefix check and settled
                        if (k == 1 && juce::SystemStats::getEnvironmentVariable ("LEGATO_INJECT_FAULT", "0") == "1")
                            for (int c = 0; c < mainChannels; ++c)
                                cur.getWritePointer (c)[pre] += 1.0e-3f;
                    }
                    credit (k + 1, cell.gapMs);
                    bool exact = prefixEqual (cur, prev, k);
                    if (! exact)
                    {
                        // one of the two is not the plugin's repeatable output (e.g. the first, cold-cache
                        // play of a streamed transition): settle both lengths
                        if (! last)
                        {
                            int r = 0;
                            cur = renderChainConverged (notes, k + 1, vel, cc, step, captureLen, r, convK);
                            rk += r;
                        }
                        int rp = 0;
                        bool convP = true;
                        auto settled = renderChainConverged (notes, k, vel, cc, step, captureLen, rp, convP);
                        rk += rp;
                        convK = convK && convP;
                        const bool prevChanged = ! identical (settled, prev);
                        prev = std::move (settled);
                        if (k == 1)
                            singleNote[first] = { juce::AudioBuffer<float> (prev), rp };
                        exact = prefixEqual (cur, prev, k);
                        // transition k-1 was measured on the old render k-1: measure it again on the settled one
                        if (prevChanged && k >= 2 && ! out.empty() && out.back().chainIndex == k - 1)
                        {
                            auto& h = out.back();
                            h.referenceExact = prefixEqual (prev, older, k - 1);
                            h.converged = h.converged && convP;
                            h.renders += rp;
                            h.result = analyseLegato (window (prev, k - 1), window (older, k - 1), pre, h.source, h.target, lp);
                        }
                        if (juce::SystemStats::getEnvironmentVariable ("LEGATO_TRACE", "0") == "1")
                            ctx.store->log ("trace: " + noteName (notes[(size_t) k - 1]) + "->" + noteName (notes[(size_t) k]) + " k" + juce::String (k)
                                            + " gap " + juce::String (gapMs, 1) + " dyn " + juce::String (d) + ": renders differed before the note-on; "
                                            + (exact ? "settled" : "still different") + " after " + juce::String (rk) + " renders"
                                            + (convK ? "" : ", NOT converged"));
                    }
                    ++ctx.legatoTransitionsDone;
                    LegatoHit h;
                    h.source = notes[(size_t) k - 1];
                    h.target = notes[(size_t) k];
                    h.dynIndex = d;
                    h.velocity = vel;
                    h.cc1 = cc;
                    h.gapMs = gapMs;
                    h.cell = ci;
                    h.type = cell.type;
                    if (velocityLayers)
                        for (auto& z : plan.sine.legatoZones)
                            if (vel >= z.velocityLo && vel <= z.velocityHi && gapMs >= z.speedLoMs && (z.speedHiMs < 0 || gapMs < z.speedHiMs))
                                h.type = z.type;
                    h.chainIndex = k;
                    h.chainLength = K;
                    h.rrSlot = R > 1 ? k % R : 0;
                    h.renders = rk + (k == 1 ? renders : 0);
                    h.converged = convK;
                    h.referenceExact = exact;
                    h.result = analyseLegato (window (cur, k), window (prev, k), pre, h.source, h.target, lp);
                    out.push_back (std::move (h));
                    older = std::move (prev);
                    prev = std::move (cur);
                }
            }
        }
    }
    if (! threadShouldExit())
        ctx.store->addLegatoJob (planIdx, std::move (out));
}

// ---------------------------------------------------------------------------- determinism check

// Does the player replay identically from its reset state? A short series (a few notes at the series
// spacing) is rendered after a reset pause, each render one note longer than the one before, and only
// the first `count` notes are compared. A round-robin counter that doesn't reset then starts every render
// one recording later - with renders of equal length it could come round to the same recording (length =
// a multiple of the cycle) and look repeatable. Passing takes three consecutive renders that agree: one
// render may differ (cold cache, a dropped note), and two consecutive length steps can't both be whole
// cycles. If they never agree, nothing that replays from the reset state can be trusted: switch to Never.
void RenderWorker::checkDeterminism()
{
    auto& s = ctx.settings;
    // the articulation most likely to show round robins
    const ArticulationPlan* plan = &ctx.plans.front();
    for (const auto& p : ctx.plans)
        if (juce::jmax (p.expectedRoundRobins(), p.art.roundRobins) > 1)
        {
            plan = &p;
            break;
        }
    const int note = plan->notes[plan->notes.size() / 2];
    const int layer = plan->art.dynamics.size() - 1;
    const int vel = plan->velocityToSend (layer), cc = plan->cc1For (layer);
    const auto& timing = s.timing.at (plan->category);
    const int64_t interval = blockInterval (juce::jmin (timing.intervalMs, s.rrMaxIntervalMs));
    const int64_t hold = juce::jmin<int64_t> ((int64_t) msToSamples (timing.holdMs), interval);
    const int pre = (int) msToSamples (s.preCaptureMs);
    const int count = juce::jmax (1, s.determinismNotes);
    const int len = pre + (int) (count * interval) + plugin->getLatencySamples();
    playerResetMs = plan->resetPauseMs (0.0);
    sendKeyswitch (plan->keyswitch);

    auto render = [&] (int extra) {
        selectArticulation (cc);
        renderUntilSilent (s.tailMaxMs);
        advanceTo (now + (int64_t) msToSamples (plan->resetPauseMs (s.rrResetPauseMs)), nullptr);
        sendResetKey();
        const int64_t t0 = alignUp (now + (int64_t) msToSamples (s.prerollMs));
        playHead.origin = t0;
        for (int i = 0; i < count + extra; ++i)
        {
            schedule (juce::MidiMessage::noteOn (1, note, (juce::uint8) vel), t0 + i * interval);
            schedule (juce::MidiMessage::noteOff (1, note), t0 + i * interval + hold);
        }
        // the capture ends before note `count` reaches the output; the extra notes still play
        Capture c;
        c.start = t0 - pre;
        c.buffer.setSize (mainChannels, len, false, false, true);
        c.buffer.clear();
        advanceTo (c.start + len, &c);
        advanceTo (t0 + (count + extra) * interval, nullptr);
        return std::move (c.buffer);
    };
    // largest sample difference relative to the peak, in dB (-200 = identical)
    float peak = 0;
    auto differenceDb = [&peak] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b) {
        float diff = 0;
        for (int c = 0; c < a.getNumChannels(); ++c)
            for (int i = 0; i < a.getNumSamples(); ++i)
            {
                peak = juce::jmax (peak, std::abs (a.getSample (c, i)));
                diff = juce::jmax (diff, std::abs (a.getSample (c, i) - b.getSample (c, i)));
            }
        return peak > 0 ? (double) juce::Decibels::gainToDecibels (diff / peak, -200.0f) : -200.0;
    };

    juce::Array<juce::var> diffs;
    juce::StringArray diffText;
    bool passed = false;
    int agreeing = 0; // consecutive pairs that agree
    auto prev = render (0);
    for (int r = 1; r < juce::jmax (3, s.determinismMaxRenders) && ! threadShouldExit(); ++r)
    {
        auto cur = render (r);
        const double d = differenceDb (prev, cur);
        diffs.add (d);
        diffText.add (d <= -200.0 ? juce::String ("identical") : juce::String (d, 1) + " dB");
        agreeing = d <= s.determinismToleranceDb ? agreeing + 1 : 0;
        if (agreeing >= 2)
        {
            passed = true;
            break;
        }
        prev = std::move (cur);
    }
    if (threadShouldExit())
        return;

    const juce::String what = plan->art.name + " " + noteName (note) + " " + plan->art.dynamics[layer];
    auto* o = new juce::DynamicObject();
    o->setProperty ("articulation", plan->art.name);
    o->setProperty ("note", noteName (note));
    o->setProperty ("dynamic", plan->art.dynamics[layer]);
    o->setProperty ("notes", count);
    o->setProperty ("renders", diffs.size() + 1);
    o->setProperty ("differenceDb", diffs);
    o->setProperty ("toleranceDb", s.determinismToleranceDb);
    if (peak <= 0)
    {
        // nothing to compare: leave the run as configured
        o->setProperty ("passed", juce::var());
        ctx.store->log ("worker " + juce::String (id) + ": determinism check inconclusive (" + what + " was silent)");
        ctx.store->setDeterminism (juce::var (o), false);
        return;
    }
    o->setProperty ("passed", passed);
    if (passed)
    {
        ctx.store->log ("worker " + juce::String (id) + ": determinism check passed (" + what + ", consecutive renders: "
                        + diffText.joinIntoString (", ") + ")");
        ctx.store->setDeterminism (juce::var (o), false);
        return;
    }
    // everything below reads the settings after this, and the other workers wait for this verdict
    s.rrNeverResets = true;
    s.rrResetKey = -1;
    o->setProperty ("forcedNever", true);
    ctx.store->setDeterminism (juce::var (o), true);
    const juce::String w = "The player doesn't replay identically from its reset state (" + what + ", consecutive renders: "
                           + diffText.joinIntoString (", ") + " difference relative to the peak). Likely humanize (random level, timing or "
                           "sample start), random round robins, or a round-robin reset that doesn't happen (check the RR reset "
                           "time or key). Switched to RR reset: Never - no tail subtraction, fast repetition or dynamics probe; "
                           "legato transitions with round robins are flagged.";
    ctx.store->log ("worker " + juce::String (id) + ": " + w);
    ctx.store->addWarning (w);
}

// The first worker that loads runs the check; the others wait for its verdict, because it may switch
// the whole run to Never before any job starts.
bool RenderWorker::awaitDeterminismVerdict()
{
    if (! ctx.settings.checkDeterminism)
        return true;
    bool expected = false;
    if (ctx.determinismClaimed.compare_exchange_strong (expected, true))
    {
        checkDeterminism();
        ctx.determinismDone = true;
        return ! threadShouldExit();
    }
    while (! ctx.determinismDone.load())
    {
        if (threadShouldExit())
            return false;
        wait (50);
    }
    return true;
}

void RenderWorker::run()
{
    wallStart = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    const bool loaded = waitForLoad();
    ++ctx.loadedWorkers; // lets the host bring up the next instance
    if (loaded && awaitDeterminismVerdict())
    {
        while (! threadShouldExit())
        {
            const size_t j = ctx.nextJob.fetch_add (1);
            if (j >= ctx.jobs.size())
                break;
            creditedMilliUnits = 0;
            currentJobMilliUnits = (int64_t) std::llround (ctx.jobs[j].units * 1000.0);
            runJob (ctx.jobs[j]);
            if (! threadShouldExit())
                ctx.doneMilliUnits += juce::jmax ((int64_t) 0, currentJobMilliUnits - creditedMilliUnits);
        }
    }
    isDone = true;
}

} // namespace tp
