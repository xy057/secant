#include "ResultStore.h"

namespace tp
{
namespace
{
    struct Stats
    {
        int count = 0;
        double mean = 0, median = 0, min = 0, max = 0, std = 0;

        static Stats of (std::vector<double> v)
        {
            Stats s;
            s.count = (int) v.size();
            if (v.empty())
                return s;
            std::sort (v.begin(), v.end());
            s.min = v.front();
            s.max = v.back();
            s.median = v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
            for (auto x : v) s.mean += x;
            s.mean /= (double) v.size();
            for (auto x : v) s.std += (x - s.mean) * (x - s.mean);
            s.std = std::sqrt (s.std / (double) v.size());
            return s;
        }

        juce::var toVar() const
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("count", count);
            if (count > 0)
            {
                o->setProperty ("mean", round4 (mean));
                o->setProperty ("median", round4 (median));
                o->setProperty ("min", round4 (min));
                o->setProperty ("max", round4 (max));
                o->setProperty ("std", round4 (std));
            }
            return juce::var (o);
        }

        static double round4 (double x) { return std::round (x * 10000.0) / 10000.0; }
    };

    double r4 (double x) { return Stats::round4 (x); }

    juce::var methodStats (const std::vector<const Hit*>& hs)
    {
        auto* o = new juce::DynamicObject();
        for (auto& key : onsetMethodKeys())
        {
            std::vector<double> v;
            for (auto* h : hs)
                if (h->onset.valid)
                    v.push_back (h->onset.get (key));
            o->setProperty (key, Stats::of (std::move (v)).toVar());
        }
        return juce::var (o);
    }
} // namespace

void ResultStore::reset (std::vector<ArticulationPlan> p, const RunSettings& s, const Instrument& inst, const Collection& c)
{
    const juce::ScopedLock sl (lock);
    plans = std::move (p);
    settings = s;
    hits.assign (plans.size(), {});
    series.assign (plans.size(), {});
    legato.assign (plans.size(), {});
    jobsDonePer.assign (plans.size(), 0);
    total = 0;
    for (size_t i = 0; i < plans.size(); ++i)
        total += totalJobsForLocked (i);
    sine = {};
    sineNotes.clear();
    done = 0;
    hitCount = 0;
    instrumentName = inst.name;
    instrumentGroup = inst.group;
    collectionName = c.name;
    collectionSource = c.source;
    vendor = c.vendor;
    warnings.clear();
    pendingLog.clear();
    startedAt = juce::Time::getCurrentTime();
}

void ResultStore::addJob (int planIdx, std::vector<Hit>&& hs, std::vector<Series>&& ss)
{
    const juce::ScopedLock sl (lock);
    hitCount += (int64_t) hs.size();
    auto& dst = hits[(size_t) planIdx];
    dst.insert (dst.end(), std::make_move_iterator (hs.begin()), std::make_move_iterator (hs.end()));
    auto& sd = series[(size_t) planIdx];
    sd.insert (sd.end(), ss.begin(), ss.end());
    ++jobsDonePer[(size_t) planIdx];
    ++done;
}

void ResultStore::addLegatoJob (int planIdx, std::vector<LegatoHit>&& ts)
{
    const juce::ScopedLock sl (lock);
    hitCount += (int64_t) ts.size();
    auto& dst = legato[(size_t) planIdx];
    dst.insert (dst.end(), std::make_move_iterator (ts.begin()), std::make_move_iterator (ts.end()));
    ++jobsDonePer[(size_t) planIdx];
    ++done;
}

void ResultStore::setSineInfo (const SineState& st, const juce::StringArray& notes)
{
    const juce::ScopedLock sl (lock);
    sine = st;
    sineNotes = notes;
}

int64_t ResultStore::totalJobsForLocked (size_t planIdx) const
{
    const auto& p = plans[planIdx];
    return p.notes.size() + (p.measureLegato ? (int64_t) p.notes.size() * p.art.dynamics.size() : 0);
}

void ResultStore::log (const juce::String& line)
{
    const juce::ScopedLock sl (lock);
    pendingLog.add (juce::Time::getCurrentTime().toString (false, true, true, true) + "  " + line);
}

juce::StringArray ResultStore::drainLog()
{
    const juce::ScopedLock sl (lock);
    auto out = pendingLog;
    pendingLog.clear();
    return out;
}

void ResultStore::addWarning (const juce::String& w)
{
    const juce::ScopedLock sl (lock);
    warnings.addIfNotAlreadyThere (w);
}

int64_t ResultStore::doneJobsFor (int planIdx) const
{
    const juce::ScopedLock sl (lock);
    return jobsDonePer[(size_t) planIdx];
}

int64_t ResultStore::totalJobsFor (int planIdx) const
{
    const juce::ScopedLock sl (lock);
    return totalJobsForLocked ((size_t) planIdx);
}

int64_t ResultStore::legatoTransitionsFor (int planIdx) const
{
    const juce::ScopedLock sl (lock);
    return (int64_t) legato[(size_t) planIdx].size();
}

double ResultStore::medianLegatoFor (int planIdx) const
{
    const juce::ScopedLock sl (lock);
    std::vector<double> v;
    for (auto& t : legato[(size_t) planIdx])
        if (t.result.valid || (t.source == t.target && t.result.diffValid))
            v.push_back (t.result.get (t.source == t.target ? juce::String ("diffAttackEnd") : settings.legatoPrimary));
    if (v.empty())
        return std::numeric_limits<double>::quiet_NaN();
    return Stats::of (std::move (v)).median;
}

juce::String ResultStore::rrSummaryFor (int planIdx) const
{
    const juce::ScopedLock sl (lock);
    int lo = std::numeric_limits<int>::max(), hi = 0;
    bool open = false;
    for (auto& sr : series[(size_t) planIdx])
    {
        if (sr.silent || sr.fastRepetition)
            continue;
        if (sr.period == 0)
        {
            open = true;
            continue;
        }
        lo = juce::jmin (lo, sr.period); // RR cycle length
        hi = juce::jmax (hi, sr.period);
    }
    if (hi == 0)
        return {};
    return (lo == hi ? juce::String (lo) : juce::String (lo) + "-" + juce::String (hi)) + (open ? "+?" : "");
}

double ResultStore::medianPrimaryFor (int planIdx) const
{
    const juce::ScopedLock sl (lock);
    auto& plan = plans[(size_t) planIdx];
    auto method = settings.timing.at (plan.category).primaryMethod;
    std::vector<double> v;
    for (auto& h : hits[(size_t) planIdx])
        if (h.onset.valid && h.isOriginal() && ! h.fastRepetition && ! h.tailContaminated)
            v.push_back (h.onset.get (method));
    if (v.empty())
        return std::numeric_limits<double>::quiet_NaN();
    return Stats::of (std::move (v)).median;
}

void ResultStore::setResolvedDynamics (int planIdx, DynamicsSource src)
{
    const juce::ScopedLock sl (lock);
    auto& p = plans[(size_t) planIdx];
    if (p.dynSource == DynamicsSource::autoDetect)
        p.dynSource = src;
}

juce::var ResultStore::sineJson() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("valid", sine.valid);
    if (! sine.valid)
    {
        o->setProperty ("error", sine.error);
        return juce::var (o);
    }
    o->setProperty ("samplerVersion", sine.samplerVersion);
    if (auto* inst = sine.instrument())
    {
        o->setProperty ("instrument", inst->title);
        if (inst->hasAdaptive)
        {
            auto* al = new juce::DynamicObject();
            al->setProperty ("amplitudeLoTimeMs", inst->adaptiveAmplitudeLoMs);
            al->setProperty ("amplitudeHiTimeMs", inst->adaptiveAmplitudeHiMs);
            al->setProperty ("offsetLoTimeMs", inst->adaptiveOffsetLoMs);
            al->setProperty ("offsetHiTimeMs", inst->adaptiveOffsetHiMs);
            al->setProperty ("transitionMinTimeMs", inst->adaptiveTransitionMinMs);
            o->setProperty ("adaptiveLegato", juce::var (al));
        }
        juce::Array<juce::var> arts;
        for (auto& a : inst->articulations)
        {
            auto* x = new juce::DynamicObject();
            x->setProperty ("title", a.title);
            x->setProperty ("keyswitch", a.keyswitch);
            x->setProperty ("keyswitchName", a.keyswitch >= 0 ? noteName (a.keyswitch) : juce::String());
            x->setProperty ("roundRobins", a.roundRobins);
            x->setProperty ("rrResetMs", a.rrResetMs);
            x->setProperty ("randomRR", a.randomRR);
            x->setProperty ("modwheelDynamics", a.modwheelDynamics);
            juce::Array<juce::var> dyn;
            for (auto& d : a.dynamics)
                dyn.add (d);
            x->setProperty ("dynamics", dyn);
            x->setProperty ("legato", a.legato);
            arts.add (juce::var (x));
        }
        o->setProperty ("articulations", arts);
    }
    juce::Array<juce::var> notes;
    for (auto& n : sineNotes)
        notes.add (n);
    o->setProperty ("appliedToPlans", notes);
    return juce::var (o);
}

// legato / portamento transitions of one articulation
juce::var ResultStore::legatoJson (size_t i, juce::StringArray& allWarnings) const
{
    const auto& plan = plans[i];
    const auto& ts = legato[i];
    const auto primary = settings.legatoPrimary;
    auto* L = new juce::DynamicObject();
    L->setProperty ("measured", true);
    L->setProperty ("zonesFrom", plan.hasSine && ! plan.sine.legatoZones.empty() ? "SINE state" : "default (single zone)");
    L->setProperty ("primaryMethod", primary);
    L->setProperty ("methodNotes",
                    "Each transition is rendered as a legato chain from the plugin's reset state (A B A B ... at gapMs note-on spacing, "
                    "previous note released " + juce::String (settings.legatoOverlapMs, 0) + " ms after the next note-on) and measured as "
                    "render(notes 0..k) against render(notes 0..k-1), which are identical up to note k. Every render is repeated until two "
                    "consecutive renders are bit-identical. Every analysis frame is tested for periodicity in the source note's and in the "
                    "target note's period (YIN d' < " + juce::String (settings.legato.periodicDprime, 2) + ", +-"
                    + juce::String (settings.legato.noteCents, 0) + " cents). arrive: the target pitch is established for good (the new note "
                    "has arrived - the point for a negative track delay). start: the source pitch ends for good. mid: a glide's "
                    "log-frequency midpoint (else halfway start..arrive). diverge: first audible difference from the render without the "
                    "transition. diffAttackEnd: end of attack of the difference signal (used for unison / repeated-note legato). "
                    "Negative delay = -value.");
    const int R = plan.expectedRoundRobins() > 0 ? plan.expectedRoundRobins() : juce::jmax (1, plan.art.roundRobins);
    L->setProperty ("roundRobins", R);
    L->setProperty ("overlapMs", settings.legatoOverlapMs);
    {
        juce::Array<juce::var> zones;
        for (auto& z : plan.sine.legatoZones)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("type", z.type);
            o->setProperty ("typeId", z.typeId);
            o->setProperty ("velocity", juce::Array<juce::var> { z.velocityLo, z.velocityHi });
            o->setProperty ("speedMs", juce::Array<juce::var> { z.speedLoMs, z.speedHiMs < 0 ? juce::var() : juce::var (z.speedHiMs) });
            zones.add (juce::var (o));
        }
        L->setProperty ("zones", zones);
        juce::Array<juce::var> cells;
        for (auto& c : plan.legatoCells)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("type", c.type);
            o->setProperty ("velocity", c.velocity);
            o->setProperty ("gapMs", r4 (c.gapMs));
            cells.add (juce::var (o));
        }
        L->setProperty ("cells", cells);
        juce::Array<juce::var> iv;
        for (auto x : settings.legatoIntervals)
            iv.add (x);
        L->setProperty ("intervalsUp", iv);
        L->setProperty ("unison", settings.legatoUnison);
    }

    auto methodFor = [&] (const LegatoHit& t) { return t.source == t.target ? juce::String ("diffAttackEnd") : primary; };
    auto usable = [] (const LegatoHit& t) { return t.source == t.target ? t.result.diffValid : t.result.valid; };
    auto valueOf = [&] (const LegatoHit& t) { return t.result.get (methodFor (t)); };
    auto gapKey = [] (double g) { return juce::String (g, 1); };
    auto intervalKey = [] (int semis) { return (semis > 0 ? "+" : "") + juce::String (semis); };

    int failed = 0, notConverged = 0, refMismatch = 0;
    std::map<juce::String, int> failures;
    for (auto& t : ts)
    {
        if (! usable (t))
        {
            ++failed;
            failures[t.result.failure]++;
        }
        notConverged += t.converged ? 0 : 1;
        refMismatch += t.referenceExact ? 0 : 1;
    }
    L->setProperty ("transitionCount", (int) ts.size());
    L->setProperty ("failed", failed);
    L->setProperty ("notConverged", notConverged);
    L->setProperty ("referenceMismatch", refMismatch);
    {
        auto* f = new juce::DynamicObject();
        for (auto& [why, n] : failures)
            f->setProperty (why.isEmpty() ? juce::String ("unknown") : why, n);
        L->setProperty ("failureReasons", juce::var (f));
    }
    if (failed > 0)
        allWarnings.addIfNotAlreadyThere (plan.art.name + ": " + juce::String (failed) + " of " + juce::String ((int) ts.size())
                                          + " legato transitions could not be measured (see legato.failureReasons)");
    if (notConverged > 0 || refMismatch > 0)
        allWarnings.addIfNotAlreadyThere (plan.art.name + ": " + juce::String (notConverged) + " legato renders never repeated identically, "
                                          + juce::String (refMismatch) + " references differed before the transition - "
                                          "the plugin did not render deterministically there");

    // --- summary: per legato type and note spacing
    {
        std::map<juce::String, std::map<double, std::vector<const LegatoHit*>>> groups;
        for (auto& t : ts)
            if (usable (t))
                groups[t.type][t.gapMs].push_back (&t);
        auto* sum = new juce::DynamicObject();
        auto* byInt = new juce::DynamicObject();
        auto* suggested = new juce::DynamicObject();
        for (auto& [type, byGap] : groups)
        {
            auto* g = new juce::DynamicObject();
            auto* gi = new juce::DynamicObject();
            auto* sg = new juce::DynamicObject();
            for (auto& [gap, list] : byGap)
            {
                auto* m = new juce::DynamicObject();
                for (auto& key : legatoMethodKeys())
                {
                    std::vector<double> v;
                    for (auto* t : list)
                        if (t->source != t->target)
                            v.push_back (t->result.get (key));
                    m->setProperty (key, Stats::of (std::move (v)).toVar());
                }
                std::vector<double> pv;
                for (auto* t : list)
                    pv.push_back (valueOf (*t));
                const auto st = Stats::of (pv);
                m->setProperty ("primary", st.toVar());
                g->setProperty (gapKey (gap), juce::var (m));
                if (st.count > 0)
                    sg->setProperty (gapKey (gap), r4 (-st.median));

                std::map<int, std::vector<double>> perInterval;
                for (auto* t : list)
                    perInterval[t->target - t->source].push_back (valueOf (*t));
                auto* pi = new juce::DynamicObject();
                for (auto& [semis, v] : perInterval)
                    pi->setProperty (intervalKey (semis), r4 (Stats::of (v).median));
                gi->setProperty (gapKey (gap), juce::var (pi));
            }
            sum->setProperty (type, juce::var (g));
            byInt->setProperty (type, juce::var (gi));
            suggested->setProperty (type, juce::var (sg));
        }
        L->setProperty ("summary", juce::var (sum));                       // type -> gap -> method stats
        L->setProperty ("medianByInterval", juce::var (byInt));            // type -> gap -> interval -> median primary
        L->setProperty ("suggestedNegativeDelayMs", juce::var (suggested)); // type -> gap -> -median
    }

    // --- the table: type -> dynamic -> gap -> source note -> interval -> { first, rr[], median }
    //   first:  the transition played first in its chain for this direction (closest to a phrase start)
    //   rr:     distinct recordings in play order (round robins; same recording = all of start/mid/arrive within 1 ms)
    {
        struct Key
        {
            juce::String type;
            int dyn;
            double gap;
            int source, target;
            bool operator< (const Key& o) const
            {
                return std::tie (type, dyn, gap, source, target) < std::tie (o.type, o.dyn, o.gap, o.source, o.target);
            }
        };
        std::map<Key, std::vector<const LegatoHit*>> cells;
        for (auto& t : ts)
            cells[{ t.type, t.dynIndex, t.gapMs, t.source, t.target }].push_back (&t);

        auto* table = new juce::DynamicObject();
        auto child = [] (juce::DynamicObject* parent, const juce::String& key) {
            if (! parent->hasProperty (key))
                parent->setProperty (key, juce::var (new juce::DynamicObject()));
            return parent->getProperty (key).getDynamicObject();
        };
        for (auto& [k, list] : cells)
        {
            auto sorted = list;
            std::sort (sorted.begin(), sorted.end(), [] (auto* a, auto* b) { return a->chainIndex < b->chainIndex; });
            juce::Array<juce::var> rr;
            std::vector<const LegatoHit*> distinct;
            std::vector<double> all;
            juce::var first;
            for (auto* t : sorted)
            {
                if (! usable (*t))
                    continue;
                if (first.isVoid())
                    first = r4 (-valueOf (*t));
                all.push_back (valueOf (*t));
                const bool seen = std::any_of (distinct.begin(), distinct.end(), [&] (const LegatoHit* x) {
                    return std::abs (x->result.arriveMs - t->result.arriveMs) < 1.0 && std::abs (x->result.startMs - t->result.startMs) < 1.0
                           && std::abs (x->result.midMs - t->result.midMs) < 1.0 && std::abs (x->result.diffAttackEndMs - t->result.diffAttackEndMs) < 1.0;
                });
                if (! seen)
                {
                    distinct.push_back (t);
                    rr.add (r4 (-valueOf (*t)));
                }
            }
            auto* cell = new juce::DynamicObject();
            cell->setProperty ("first", first);
            cell->setProperty ("rr", rr);
            if (! all.empty())
                cell->setProperty ("median", r4 (-Stats::of (all).median));
            const auto dynName = k.dyn < plan.art.dynamics.size() ? plan.art.dynamics[k.dyn] : juce::String (k.dyn);
            auto* bySource = child (child (child (table, k.type), dynName), gapKey (k.gap));
            child (bySource, noteName (k.source))->setProperty (intervalKey (k.target - k.source), juce::var (cell));
        }
        L->setProperty ("negativeDelayMsTable", juce::var (table));
    }

    // --- raw transitions
    {
        juce::Array<juce::var> raw;
        for (auto& t : ts)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("source", noteName (t.source));
            o->setProperty ("target", noteName (t.target));
            o->setProperty ("interval", t.target - t.source);
            o->setProperty ("dynamic", t.dynIndex < plan.art.dynamics.size() ? plan.art.dynamics[t.dynIndex] : juce::String (t.dynIndex));
            o->setProperty ("type", t.type);
            o->setProperty ("velocity", t.velocity);
            o->setProperty ("cc1", t.cc1);
            o->setProperty ("gapMs", r4 (t.gapMs));
            o->setProperty ("chainIndex", t.chainIndex);
            o->setProperty ("chainLength", t.chainLength);
            if (R > 1)
                o->setProperty ("rrSlot", t.rrSlot);
            o->setProperty ("renders", t.renders);
            if (! t.converged)
                o->setProperty ("converged", false);
            if (! t.referenceExact)
                o->setProperty ("referenceExact", false);
            const auto& r = t.result;
            o->setProperty ("valid", usable (t));
            if (usable (t))
                o->setProperty ("primaryMs", r4 (valueOf (t)));
            if (r.valid)
            {
                auto* m = new juce::DynamicObject();
                for (auto& key : legatoMethodKeys())
                    m->setProperty (key, r4 (r.get (key)));
                o->setProperty ("ms", juce::var (m));
                o->setProperty ("midFromPitch", r.midFromPitch);
                o->setProperty ("targetCents", r4 (r.targetCentsError));
                o->setProperty ("vibratoCents", r4 (r.vibratoCents));
                o->setProperty ("voicedFraction", r4 (r.voicedFraction));
            }
            else
            {
                if (r.diffValid)
                {
                    o->setProperty ("divergeMs", r4 (r.divergeMs));
                    o->setProperty ("diffAttackEndMs", r4 (r.diffAttackEndMs));
                }
                o->setProperty ("failure", r.failure);
            }
            o->setProperty ("steadyDb", r4 (r.steadyDb));
            raw.add (juce::var (o));
        }
        L->setProperty ("transitions", raw);
    }
    return juce::var (L);
}

juce::var ResultStore::toJson() const
{
    const juce::ScopedLock sl (lock);
    auto allWarnings = warnings; // plus findings computed below (RR mismatches)

    auto* root = new juce::DynamicObject();
    root->setProperty ("format", "secant.results");
    root->setProperty ("version", 3);
    root->setProperty ("app", juce::String (JUCE_APPLICATION_NAME_STRING) + " " + JUCE_APPLICATION_VERSION_STRING);
    root->setProperty ("createdAt", juce::Time::getCurrentTime().toISO8601 (true));
    root->setProperty ("startedAt", startedAt.toISO8601 (true));
    root->setProperty ("timeUnit", "ms relative to note-on, plugin latency subtracted; negative delay = -value");
    root->setProperty ("roundRobinMethod", "Each (note, dynamic) is played repeatedly until a hit is identical (similarity > "
                                           "rrMatchThreshold) to an earlier one, closing the RR cycle; with confirmCycle one more "
                                           "confirmHits consecutive hits must line up with the cycle. Distinct samples get rr slots 0..U-1 in play order; "
                                           "repeats carry duplicateOf and are excluded from all statistics and tables.");
    root->setProperty ("noteConvention", "C3=60");

    {
        auto* p = new juce::DynamicObject();
        p->setProperty ("name", plugin.name);
        p->setProperty ("manufacturer", plugin.manufacturer);
        p->setProperty ("version", plugin.version);
        p->setProperty ("file", plugin.fileOrIdentifier);
        p->setProperty ("latencySamples", plugin.latencySamples);
        p->setProperty ("outputChannels", plugin.numOutputChannels);
        p->setProperty ("stateFile", plugin.stateFile);
        root->setProperty ("plugin", juce::var (p));
    }
    {
        auto* pr = new juce::DynamicObject();
        pr->setProperty ("vendor", vendor);
        pr->setProperty ("collection", collectionName);
        pr->setProperty ("group", instrumentGroup);
        pr->setProperty ("instrument", instrumentName);
        pr->setProperty ("source", collectionSource);
        root->setProperty ("preset", juce::var (pr));
    }
    {
        auto* r = new juce::DynamicObject();
        r->setProperty ("sampleRate", settings.sampleRate);
        r->setProperty ("blockSize", settings.blockSize);
        r->setProperty ("workers", settings.numWorkers);
        r->setProperty ("noteStep", settings.noteStep);
        r->setProperty ("confirmCycle", settings.confirmCycle);
        r->setProperty ("confirmHits", settings.confirmHits);
        r->setProperty ("maxHitsPerSeries", settings.maxHitsPerSeries);
        r->setProperty ("rrMatchThreshold", settings.rrMatchThreshold);
        r->setProperty ("rrLooseSimilarity", settings.rrLooseSimilarity);
        r->setProperty ("rrLoosePeakDb", settings.rrLoosePeakDb);
        r->setProperty ("rrLooseOnsetMs", settings.rrLooseOnsetMs);
        r->setProperty ("similarityMs", settings.similarityMs);
        r->setProperty ("nonRealtime", settings.nonRealtime);
        r->setProperty ("maxSpeed", settings.maxSpeed);
        r->setProperty ("sendAllSoundOff", settings.sendAllSoundOff);
        r->setProperty ("prerollMs", settings.prerollMs);
        r->setProperty ("cc1SettleMs", settings.cc1SettleMs);
        r->setProperty ("warmupHit", settings.warmupHit);
        r->setProperty ("rrResetPauseMs", settings.rrResetPauseMs);
        // > 0: the player's RR reset time was set to this for every articulation for the run
        r->setProperty ("rrResetOverrideMs", settings.rrResetOverrideMs);
        r->setProperty ("rrMaxIntervalMs", settings.rrMaxIntervalMs);
        r->setProperty ("fastRepetition", settings.fastRepetition);
        r->setProperty ("subtractTailsForLongNotes", settings.subtractTailsForLongNotes);
        r->setProperty ("subtractTailsWhenMasked", settings.subtractTailsWhenMasked);
        r->setProperty ("fastRepIntervalMs", settings.fastRepIntervalMs);
        r->setProperty ("tailContaminationDb", settings.tailContaminationDb);
        r->setProperty ("rrPeakMatchSimilarity", settings.rrPeakMatchSimilarity);
        r->setProperty ("rrPeakMatchDb", settings.rrPeakMatchDb);
        r->setProperty ("rrMaxLagSamples", settings.rrMaxLagSamples);
        r->setProperty ("rrLaggedPeakDb", settings.rrLaggedPeakDb);
        r->setProperty ("useRRPrior", settings.useRRPrior);
        r->setProperty ("rrPriorSimilarity", settings.rrPriorSimilarity);
        r->setProperty ("rrPriorPeakDb", settings.rrPriorPeakDb);
        r->setProperty ("rrPriorOnsetMs", settings.rrPriorOnsetMs);
        {
            auto* lg = new juce::DynamicObject();
            lg->setProperty ("enabled", settings.measureLegato);
            juce::Array<juce::var> gaps, iv;
            for (auto g : settings.legatoGapsMs)
                gaps.add (g);
            for (auto x : settings.legatoIntervals)
                iv.add (x);
            lg->setProperty ("gapsMs", gaps);
            lg->setProperty ("intervalsUp", iv);
            lg->setProperty ("unison", settings.legatoUnison);
            lg->setProperty ("primaryMethod", settings.legatoPrimary);
            lg->setProperty ("captureMs", settings.legatoCaptureMs);
            lg->setProperty ("overlapMs", settings.legatoOverlapMs);
            lg->setProperty ("resetPauseMs", settings.legatoResetPauseMs);
            lg->setProperty ("maxRenders", settings.legatoMaxRenders);
            lg->setProperty ("rrConfirm", settings.legatoRRConfirm);
            lg->setProperty ("periodicDprime", settings.legato.periodicDprime);
            lg->setProperty ("noteCents", settings.legato.noteCents);
            lg->setProperty ("minRunMs", settings.legato.minRunMs);
            lg->setProperty ("arriveConfirmMs", settings.legato.arriveConfirmMs);
            lg->setProperty ("arriveConfirmShare", settings.legato.arriveConfirmShare);
            r->setProperty ("legato", juce::var (lg));
        }
        r->setProperty ("absThresholdDb", settings.onset.absThresholdDb);
        r->setProperty ("envelope", settings.onset.envelope == EnvelopeMode::hilbert ? "hilbert" : "rms");
        r->setProperty ("hilbertSmoothMs", settings.onset.hilbertSmoothMs);
        r->setProperty ("periodSmoothing", settings.onset.periodSmoothing);
        r->setProperty ("rmsWindowMs", settings.onset.rmsWindowMs);
        r->setProperty ("slopeWindowMs", settings.onset.slopeWindowMs);
        r->setProperty ("attackSmoothMs", settings.onset.attackSmoothMs);
        r->setProperty ("attackDecayDb", settings.onset.attackDecayDb);
        r->setProperty ("attackPlateauMs", settings.onset.attackPlateauMs);
        r->setProperty ("attackPlateauRiseDb", settings.onset.attackPlateauRiseDb);
        r->setProperty ("attackEndDb", settings.onset.attackEndDb);
        auto* t = new juce::DynamicObject();
        for (auto& [cat, ct] : settings.timing)
        {
            auto* c = new juce::DynamicObject();
            c->setProperty ("captureMs", ct.captureMs);
            c->setProperty ("peakWindowMs", ct.peakWindowMs);
            c->setProperty ("primaryMethod", ct.primaryMethod);
            c->setProperty ("holdMs", ct.holdMs);
            c->setProperty ("intervalMs", ct.intervalMs);
            t->setProperty (toString (cat), juce::var (c));
        }
        r->setProperty ("categories", juce::var (t));
        juce::Array<juce::var> methods;
        for (auto& k : onsetMethodKeys())
            methods.add (k);
        r->setProperty ("methods", methods);
        r->setProperty ("methodNotes", "first: first sample above absThresholdDb (sample-accurate). Envelope = |analytic signal| (Hilbert), "
                                       "centred-smoothed over max(hilbertSmoothMs, periodSmoothing x fundamental period of the played note). "
                                       "rel-N: envelope first crosses N dB below its peak (peak searched within peakWindowMs; crossing "
                                       "searched from 'first'). maxSlope: steepest rise of the max(slopeWindowMs, period) envelope. "
                                       "attackPeak: end of attack / start of decay - walking up from rel-20, the first maximum before the "
                                       "envelope decays attackDecayDb, or where it settles (rises < attackPlateauRiseDb over attackPlateauMs). "
                                       "attackEnd: envelope first within attackEndDb of attackPeak (the default primary - the point to put "
                                       "on the grid, i.e. negative track delay = -attackEnd). "
                                       "peak: global envelope peak in peakWindowMs. Sub-sample via linear / parabolic interpolation. Below ~150 Hz harmonic "
                                       "beating limits envelope-based methods to a fraction of the fundamental period.");
        root->setProperty ("render", juce::var (r));
    }

    juce::Array<juce::var> arts;
    for (size_t i = 0; i < plans.size(); ++i)
    {
        auto& plan = plans[i];
        auto& hs = hits[i];
        const auto primary = settings.timing.at (plan.category).primaryMethod;

        auto* a = new juce::DynamicObject();
        a->setProperty ("index", plan.art.index);
        a->setProperty ("docNumber", plan.art.docNumber);
        a->setProperty ("name", plan.art.name);
        a->setProperty ("category", toString (plan.category));
        a->setProperty ("keyswitch", plan.keyswitch);
        a->setProperty ("keyswitchName", plan.keyswitch >= 0 ? noteName (plan.keyswitch) : juce::String());
        a->setProperty ("dynamicsSource", toString (plan.dynSource));
        a->setProperty ("range", plan.art.rangeText());
        a->setProperty ("roundRobinsDocs", plan.art.roundRobins);
        if (plan.hasSine)
        {
            a->setProperty ("roundRobinsSine", plan.sine.roundRobins);
            a->setProperty ("sineTitle", plan.sine.title);
        }
        a->setProperty ("primaryMethod", primary);

        juce::Array<juce::var> dyn;
        for (int d = 0; d < plan.art.dynamics.size(); ++d)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("name", plan.art.dynamics[d]);
            o->setProperty ("velocity", plan.velocityToSend (d));
            o->setProperty ("cc1", plan.cc1For (d));
            dyn.add (juce::var (o));
        }
        a->setProperty ("dynamics", dyn);

        juce::Array<juce::var> issues;
        for (auto& s : plan.art.issues)
            issues.add (s);
        a->setProperty ("docIssues", issues);

        // --- hits in play order; statistics only over distinct samples (each RR counted once)
        // main series and fast-repetition series are reported separately
        std::vector<const Hit*> all, fast, originals, distinct;
        for (auto& h : hs)
            (h.fastRepetition ? fast : all).push_back (&h);
        auto byPlayOrder = [] (const Hit* x, const Hit* y) {
            return std::tie (x->note, x->dynIndex, x->seq) < std::tie (y->note, y->dynIndex, y->seq);
        };
        std::sort (all.begin(), all.end(), byPlayOrder);
        std::sort (fast.begin(), fast.end(), byPlayOrder);
        for (auto* h : all)
            if (h->isOriginal())
                distinct.push_back (h);
        // statistics use distinct samples whose onset was not masked by the previous note's tail
        for (auto* h : distinct)
            if (! h->tailContaminated)
                originals.push_back (h);
        if (originals.empty())
            originals = distinct;
        const int contaminated = (int) (distinct.size() - (size_t) std::count_if (distinct.begin(), distinct.end(),
                                                                                   [] (auto* h) { return ! h->tailContaminated; }));

        const int silent = (int) std::count_if (originals.begin(), originals.end(), [] (auto* h) { return ! h->onset.valid; });
        const int bleed = (int) std::count_if (all.begin(), all.end(), [] (auto* h) { return h->onset.preRollDb > -90.0; });

        auto* summary = new juce::DynamicObject();
        summary->setProperty ("hitsPlayed", (int) all.size());
        summary->setProperty ("distinctSamples", (int) distinct.size());
        summary->setProperty ("tailContaminatedSamples", contaminated); // excluded from the statistics
        summary->setProperty ("silentSamples", silent);
        summary->setProperty ("bleedHits", bleed);
        summary->setProperty ("methods", methodStats (originals));

        {
            std::vector<double> v;
            for (auto* h : originals)
                if (h->onset.valid)
                    v.push_back (h->onset.get (primary));
            auto st = Stats::of (std::move (v));
            summary->setProperty ("primary", st.toVar());
            if (st.count > 0)
            {
                summary->setProperty ("suggestedNegativeDelayMs", r4 (-st.median));
                summary->setProperty ("suggestedNegativeDelayMsWorstCase", r4 (-st.max));
            }
        }

        auto* byDyn = new juce::DynamicObject();
        for (int d = 0; d < plan.art.dynamics.size(); ++d)
        {
            std::vector<const Hit*> sub;
            for (auto* h : originals)
                if (h->dynIndex == d)
                    sub.push_back (h);
            byDyn->setProperty (plan.art.dynamics[d] + "#" + juce::String (d), methodStats (sub));
        }
        summary->setProperty ("byDynamic", juce::var (byDyn));

        auto* byNote = new juce::DynamicObject();
        for (auto note : plan.notes)
        {
            std::vector<double> v;
            for (auto* h : originals)
                if (h->note == note && h->onset.valid)
                    v.push_back (h->onset.get (primary));
            if (! v.empty())
                byNote->setProperty (noteName (note), Stats::of (std::move (v)).toVar());
        }
        summary->setProperty ("byNotePrimary", juce::var (byNote));

        // --- round robin discovery
        {
            std::vector<Series> ss;
            for (auto& x : series[i])
                if (! x.fastRepetition)
                    ss.push_back (x);
            std::sort (ss.begin(), ss.end(), [] (auto& x, auto& y) { return std::tie (x.note, x.dynIndex) < std::tie (y.note, y.dynIndex); });
            std::map<int, int> uniqueHist;
            int confirmed = 0, unconfirmed = 0, open = 0, silentSeries = 0;
            juce::Array<juce::var> sv;
            for (auto& sr : ss)
            {
                if (sr.silent) { ++silentSeries; }
                else
                {
                    uniqueHist[sr.uniqueSamples]++;
                    if (sr.randomOrder) { /* reported separately (randomOrderSeries) */ }
                    else if (sr.period == 0) ++open;
                    else if (sr.confirmed || ! settings.confirmCycle) ++confirmed;
                    else ++unconfirmed;
                }
                auto* o = new juce::DynamicObject();
                o->setProperty ("note", noteName (sr.note));
                o->setProperty ("dynamic", plan.art.dynamics[sr.dynIndex]);
                o->setProperty ("hitsPlayed", sr.hitsPlayed);
                o->setProperty ("distinctSamples", sr.uniqueSamples);
                o->setProperty ("period", sr.period);
                o->setProperty ("cycleStart", sr.cycleStart);
                o->setProperty ("confirmed", sr.confirmed);
                if (sr.retries > 0)
                    o->setProperty ("retries", sr.retries);
                if (sr.randomOrder)
                    o->setProperty ("randomOrder", true);
                o->setProperty ("silent", sr.silent);
                sv.add (juce::var (o));
            }
            auto* rr = new juce::DynamicObject();
            auto* hist = new juce::DynamicObject();
            for (auto& [u, n] : uniqueHist)
                hist->setProperty (juce::String (u), n);
            std::map<int, int> periodHist;
            int afterPauseOutside = 0;
            int randomSeries = 0;
            for (auto& sr : ss)
                if (! sr.silent)
                {
                    if (sr.randomOrder)
                    {
                        // random order: the RR set is every distinct sample after the after-pause hit
                        ++randomSeries;
                        int setSize = 0;
                        for (auto* h : all)
                            if (h->note == sr.note && h->dynIndex == sr.dynIndex && h->isOriginal() && ! h->afterPause)
                                ++setSize;
                        bool afterPauseInSet = false;
                        for (auto* h : all)
                            if (h->note == sr.note && h->dynIndex == sr.dynIndex && h->duplicateOf == 0)
                                afterPauseInSet = true;
                        periodHist[setSize + (afterPauseInSet ? 1 : 0)]++;
                    }
                    else
                        periodHist[sr.period]++;
                    afterPauseOutside += sr.cycleStart > 0 ? 1 : 0;
                }
            int mode = 0, modeCount = 0;
            auto* phist = new juce::DynamicObject();
            for (auto& [pp, n] : periodHist)
            {
                phist->setProperty (pp == 0 ? juce::String ("none") : juce::String (pp), n);
                if (pp > 0 && n > modeCount) { mode = pp; modeCount = n; }
            }
            rr->setProperty ("cycleLengthHistogram", juce::var (phist)); // random-order series count their sample-set size
            rr->setProperty ("randomOrderSeries", randomSeries);
            rr->setProperty ("afterPauseOutsideCycle", afterPauseOutside); // series whose after-pause sample is not part of the cycle
            rr->setProperty ("docs", plan.art.roundRobins);
            if (plan.hasSine)
                rr->setProperty ("sine", plan.sine.roundRobins);
            rr->setProperty ("detected", mode);
            rr->setProperty ("distinctSamplesHistogram", juce::var (hist));
            rr->setProperty ("cyclesConfirmed", confirmed);
            rr->setProperty ("cyclesUnconfirmed", unconfirmed);
            rr->setProperty ("cyclesNeverRepeated", open);
            rr->setProperty ("silentSeries", silentSeries);
            rr->setProperty ("series", sv);
            summary->setProperty ("roundRobin", juce::var (rr));
            const int expectedRR = plan.hasSine ? plan.sine.roundRobins : plan.art.roundRobins;
            if (mode != 0 && mode != expectedRR)
                allWarnings.addIfNotAlreadyThere (plan.art.name + ": detected " + juce::String (mode) + " round robins, "
                                                  + (plan.hasSine ? "SINE has " : "docs say ") + juce::String (expectedRR));
            {
                // series whose cycle differs from the player's own count (identical RR recordings, choked voices...)
                int off = 0;
                for (auto& sr : ss)
                    off += (! sr.silent && ! sr.randomOrder && sr.period > 0 && sr.period != expectedRR) ? 1 : 0;
                rr->setProperty ("seriesNotMatchingExpected", off);
            }
            const bool nonCycling = std::any_of (ss.begin(), ss.end(), [] (auto& sr) {
                return ! sr.silent && ! sr.randomOrder && sr.period > sr.uniqueSamples;
            });
            if (nonCycling || unconfirmed > 0)
                allWarnings.addIfNotAlreadyThere (plan.art.name + ": RR order does not form a stable cycle in some series "
                                                  "(random round robins?) - distinct-sample counts are still valid");
            if (randomSeries > 0)
                allWarnings.addIfNotAlreadyThere (plan.art.name + ": round robins play in random order in " + juce::String (randomSeries)
                                                  + " series - the delay table lists the set of samples instead of a cycle");
            if (open > 0)
                allWarnings.addIfNotAlreadyThere (
                    plan.art.name + ": " + juce::String (open) + " series never repeated within " + juce::String (settings.maxHitsPerSeries)
                    + " hits (random RR / non-deterministic playback?)");
        }

        // --- fast repetition: how many extra variants appear, and their delays
        if (! fast.empty())
        {
            std::map<int, int> variants;
            std::vector<double> v;
            for (auto& x : series[i])
                if (x.fastRepetition && ! x.silent)
                    variants[x.uniqueSamples]++;
            for (auto* h : fast)
                if (h->isOriginal() && h->onset.valid)
                    v.push_back (h->onset.get (primary));
            auto* fr = new juce::DynamicObject();
            auto* vh = new juce::DynamicObject();
            for (auto& [u, n] : variants)
                vh->setProperty (juce::String (u), n);
            fr->setProperty ("intervalMs", settings.fastRepIntervalMs);
            fr->setProperty ("distinctSamplesHistogram", juce::var (vh));
            fr->setProperty ("primary", Stats::of (std::move (v)).toVar());
            summary->setProperty ("fastRepetition", juce::var (fr));
        }

        // --- dynamic layers that turn out to be the same sample (docs list more layers than exist,
        //     or the layer values sent don't reach distinct layers)
        {
            const int nd = plan.art.dynamics.size();
            for (int d1 = 0; d1 < nd; ++d1)
                for (int d2 = d1 + 1; d2 < nd; ++d2)
                {
                    int same = 0, compared = 0;
                    for (auto note : plan.notes)
                    {
                        const Hit *x = nullptr, *y = nullptr;
                        for (auto* h : all)
                        {
                            if (h->note != note || ! h->afterPause || ! h->onset.valid)
                                continue;
                            if (h->dynIndex == d1)
                                x = h;
                            else if (h->dynIndex == d2)
                                y = h;
                        }
                        if (x == nullptr || y == nullptr)
                            continue;
                        ++compared;
                        same += std::abs (x->onset.peakDb - y->onset.peakDb) < 0.01
                                && std::abs (x->onset.attackEndMs - y->onset.attackEndMs) < 0.05;
                    }
                    if (compared > 0 && same * 5 >= compared * 4)
                        allWarnings.addIfNotAlreadyThere (plan.art.name + ": dynamic layers " + plan.art.dynamics[d1] + " and "
                                                          + plan.art.dynamics[d2] + " play the same sample (" + juce::String (same)
                                                          + "/" + juce::String (compared) + " notes) - fewer real layers than listed, "
                                                          + "or the layer values don't reach them");
                }
        }

        // --- the comprehensive table: note -> dynamic -> { afterPause, cycle[] } (negative delays, ms)
        //   afterPause: the sample played after >= rrResetPauseMs without notes (start of a phrase)
        //   cycle:      the round-robin cycle in the plugin's own order, starting where the cycle begins
        {
            auto* table = new juce::DynamicObject();
            auto neg = [&] (const Hit* h) { return h->onset.valid ? juce::var (r4 (-h->onset.get (primary))) : juce::var(); };
            for (auto note : plan.notes)
            {
                auto* perDyn = new juce::DynamicObject();
                bool any = false;
                for (int d = 0; d < plan.art.dynamics.size(); ++d)
                {
                    std::vector<const Hit*> sh;
                    for (auto* h : all)
                        if (h->note == note && h->dynIndex == d)
                            sh.push_back (h);
                    if (sh.empty())
                        continue;
                    const Series* sr = nullptr;
                    for (auto& x : series[i])
                        if (x.note == note && x.dynIndex == d && ! x.fastRepetition)
                            sr = &x;
                    auto* cell = new juce::DynamicObject();
                    cell->setProperty ("afterPause", neg (sh.front()));
                    // is the after-pause hit one of the cycle's recordings (just started at a different offset)?
                    for (auto* h : sh)
                        if (h->duplicateOf == 0)
                        {
                            cell->setProperty ("afterPauseIsCycleSample", true);
                            cell->setProperty ("afterPauseStartsEarlierMs", r4 (h->matchLagSamples * 1000.0 / settings.sampleRate));
                            break;
                        }
                    // the cycle's members are the distinct samples from the cycle start on, in first-appearance order
                    juce::Array<juce::var> cycle;
                    const int from = sr != nullptr && sr->period > 0 ? sr->cycleStart : 1;
                    for (auto* h : sh)
                        if (h->isOriginal() && h->seq >= from)
                            cycle.add (neg (h));
                    cell->setProperty ("cycle", cycle);
                    cell->setProperty ("cycleConfirmed", sr != nullptr && sr->confirmed);
                    // fast repetition: distinct samples in play order (tail-subtracted)
                    {
                        const Series* fs = nullptr;
                        for (auto& x : series[i])
                            if (x.note == note && x.dynIndex == d && x.fastRepetition)
                                fs = &x;
                        if (fs != nullptr)
                        {
                            auto* fr = new juce::DynamicObject();
                            juce::Array<juce::var> seq;
                            for (auto* h : fast)
                                if (h->note == note && h->dynIndex == d && h->isOriginal())
                                    seq.add (neg (h));
                            fr->setProperty ("intervalMs", r4 (fs->intervalMs));
                            fr->setProperty ("distinct", seq);
                            fr->setProperty ("cycleStart", fs->cycleStart);
                            fr->setProperty ("period", fs->period);
                            fr->setProperty ("confirmed", fs->confirmed);
                            cell->setProperty ("fastRepetition", juce::var (fr));
                        }
                    }
                    perDyn->setProperty (plan.art.dynamics[d], juce::var (cell));
                    any = true;
                }
                if (any)
                    table->setProperty (noteName (note), juce::var (perDyn));
            }
            a->setProperty ("negativeDelayMsTable", juce::var (table));
        }
        a->setProperty ("summary", juce::var (summary));

        // --- raw hits (play order, repeats included and marked; fast-repetition hits flagged)
        juce::Array<juce::var> hv;
        auto rawHits = all;
        rawHits.insert (rawHits.end(), fast.begin(), fast.end());
        for (auto* h : rawHits)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("note", h->note);
            o->setProperty ("noteName", noteName (h->note));
            o->setProperty ("dynamic", plan.art.dynamics[h->dynIndex]);
            o->setProperty ("dynIndex", h->dynIndex);
            o->setProperty ("seq", h->seq);
            o->setProperty ("rr", h->rr);
            o->setProperty ("duplicateOf", h->duplicateOf);
            if (h->afterPause)
                o->setProperty ("afterPause", true);
            if (h->fastRepetition)
                o->setProperty ("fastRepetition", true);
            if (h->onset.valid)
                o->setProperty ("tailDb", r4 (juce::jmax (-200.0, h->tailDb)));
            if (h->tailContaminated)
                o->setProperty ("tailContaminated", true);
            if (h->tailSubtracted)
                o->setProperty ("tailSubtracted", true);
            if (h->unresolved)
                o->setProperty ("unresolved", true);
            if (! std::isnan (h->matchSimilarity))
                o->setProperty ("matchSimilarity", std::round (h->matchSimilarity * 1e6) / 1e6);
            if (h->looseMatch)
                o->setProperty ("looseMatch", true);
            if (h->priorMatch)
                o->setProperty ("priorMatch", true);
            if (h->matchLagSamples != 0)
                o->setProperty ("matchLagSamples", h->matchLagSamples);
            if (h->isOriginal() && ! std::isnan (h->bestSimilarity))
            {
                o->setProperty ("bestSimilarity", std::round (h->bestSimilarity * 1e6) / 1e6);
                o->setProperty ("bestSimilarityTo", h->bestSimilarityTo);
            }
            o->setProperty ("velocity", h->velocity);
            o->setProperty ("cc1", h->cc1);
            o->setProperty ("valid", h->onset.valid);
            if (h->onset.valid)
            {
                auto* on = new juce::DynamicObject();
                for (auto& key : onsetMethodKeys())
                    on->setProperty (key, r4 (h->onset.get (key)));
                o->setProperty ("onsetMs", juce::var (on));
                o->setProperty ("primaryMs", r4 (h->onset.get (primary)));
                o->setProperty ("peakDb", r4 (h->onset.peakDb));
            }
            o->setProperty ("preRollDb", r4 (juce::jmax (-200.0, h->onset.preRollDb)));
            hv.add (juce::var (o));
        }
        a->setProperty ("hits", hv);
        if (plan.measureLegato)
            a->setProperty ("legato", legatoJson (i, allWarnings));
        arts.add (juce::var (a));
    }
    root->setProperty ("articulations", arts);
    root->setProperty ("player", sineJson());

    juce::Array<juce::var> w;
    for (auto& s : allWarnings)
        w.add (s);
    root->setProperty ("warnings", w);
    return juce::var (root);
}

juce::Result ResultStore::writeJson (const juce::File& f) const
{
    auto json = juce::JSON::toString (toJson(), false);
    f.getParentDirectory().createDirectory();
    if (! f.replaceWithText (json))
        return juce::Result::fail ("Could not write " + f.getFullPathName());
    return juce::Result::ok();
}

} // namespace tp
