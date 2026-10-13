// Legato behaviour probe: loads a VST3 with a saved state (.pluginstate written by Secant),
// selects an articulation by keyswitch, holds note A and plays note B on top (legato) for every
// combination of B velocity and A->B note-on gap. Prints the transition timing and labels which
// transition recording played (same letter = same audio), which shows how velocity and playing
// speed select the legato type.
//
//   LegatoProbe <plugin.vst3> <state> <keyswitch> <noteA> <noteB> [velocities=20,64,100,127] [gapsMs=1000] [cc1=64]

#include <juce_audio_processors/juce_audio_processors.h>
#include "../Source/LegatoAnalyzer.h"
#include "../Source/OnsetAnalyzer.h"
#include "../Source/PresetModel.h"

using namespace tp;

namespace
{
constexpr double fs = 48000.0;
constexpr int bs = 256;

struct Engine
{
    juce::AudioPluginInstance& p;
    int64_t now = 0;
    juce::AudioBuffer<float> block;
    std::vector<std::pair<int64_t, juce::MidiMessage>> pending;

    explicit Engine (juce::AudioPluginInstance& plugin)
        : p (plugin), block (juce::jmax (2, plugin.getTotalNumOutputChannels()), bs) {}

    void at (int64_t t, const juce::MidiMessage& m)
    {
        pending.emplace_back (t, m);
        std::stable_sort (pending.begin(), pending.end(), [] (auto& a, auto& b) { return a.first < b.first; });
    }
    double pace = juce::SystemStats::getEnvironmentVariable ("PACE", "0").getDoubleValue(); // x realtime, 0 = offline
    double wall0 = -1;
    int64_t sample0 = 0;
    void run (int64_t end, juce::AudioBuffer<float>* cap = nullptr, int64_t capStart = 0)
    {
        while (now < end)
        {
            if (pace > 0)
            {
                if (wall0 < 0)
                    wall0 = juce::Time::getMillisecondCounterHiRes(), sample0 = now;
                const double want = wall0 + (double) (now - sample0) / fs * 1000.0 / pace;
                const double nowMs = juce::Time::getMillisecondCounterHiRes();
                if (want > nowMs + 1)
                    juce::Thread::sleep ((int) (want - nowMs));
            }
            const int n = (int) juce::jmin<int64_t> (bs, end - now);
            juce::MidiBuffer midi;
            size_t used = 0;
            for (; used < pending.size() && pending[used].first < now + n; ++used)
                midi.addEvent (pending[used].second, (int) (pending[used].first - now));
            pending.erase (pending.begin(), pending.begin() + (long) used);
            juce::AudioBuffer<float> view (block.getArrayOfWritePointers(), block.getNumChannels(), n);
            view.clear();
            p.processBlock (view, midi);
            if (cap != nullptr)
            {
                const int64_t s = juce::jmax (now, capStart), e = juce::jmin (now + n, capStart + cap->getNumSamples());
                for (int c = 0; c < 2 && e > s; ++c)
                    cap->copyFrom (c, (int) (s - capStart), view, c, (int) (s - now), (int) (e - s));
            }
            now += n;
        }
    }
    int64_t ms (double v) const { return (int64_t) std::llround (v * fs / 1000.0); }
    int64_t aligned() const { return (now / bs + 1) * bs; }
};

juce::Array<double> listArg (int argc, char* argv[], int i, juce::Array<double> def)
{
    if (argc <= i)
        return def;
    juce::Array<double> v;
    for (auto& t : juce::StringArray::fromTokens (argv[i], ",", ""))
        v.add (t.getDoubleValue());
    return v;
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 6)
    {
        std::puts ("usage: LegatoProbe <plugin.vst3> <state> <keyswitch> <noteA> <noteB> [velocities] [gapsMs] [cc1=64]");
        return 2;
    }
    const int ks = parseNoteName (argv[3]), noteA = parseNoteName (argv[4]), noteB = parseNoteName (argv[5]);
    const auto vels = listArg (argc, argv, 6, { 20, 64, 100, 127 });
    const auto gaps = listArg (argc, argv, 7, { 1000 });
    const int cc1 = argc > 8 ? juce::String (argv[8]).getIntValue() : 64;

    juce::AudioPluginFormatManager fm;
    fm.addFormat (new juce::VST3PluginFormat());
    juce::OwnedArray<juce::PluginDescription> descs;
    fm.getFormat (0)->findAllTypesForFile (descs, argv[1]);
    if (descs.isEmpty())
        return 1;
    juce::String err;
    auto plugin = fm.createPluginInstance (*descs[0], fs, bs, err);
    juce::MemoryBlock state;
    if (! juce::File (argv[2]).loadFileAsData (state))
        return 1;
    plugin->enableAllBuses();
    plugin->setStateInformation (state.getData(), (int) state.getSize());
    plugin->setNonRealtime (true);
    plugin->prepareToPlay (fs, bs);
    const int latency = plugin->getLatencySamples();
    std::printf ("keyswitch %s, %s -> %s, cc1 %d, latency %d\n", noteName (ks).toRawUTF8(), noteName (noteA).toRawUTF8(),
                 noteName (noteB).toRawUTF8(), cc1, latency);

    Engine e (*plugin);
    for (int tries = 0;; ++tries)
    {
        e.at (e.now, juce::MidiMessage::noteOn (1, ks, (juce::uint8) 100));
        e.at (e.now + e.ms (30), juce::MidiMessage::noteOff (1, ks));
        e.run (e.now + e.ms (150));
        juce::AudioBuffer<float> cap (2, (int) e.ms (300));
        cap.clear();
        const int64_t t = e.now;
        e.at (t, juce::MidiMessage::noteOn (1, noteA, (juce::uint8) 100));
        e.at (t + e.ms (200), juce::MidiMessage::noteOff (1, noteA));
        e.run (t + cap.getNumSamples(), &cap, t);
        e.run (e.now + e.ms (3000));
        if (cap.getMagnitude (0, 0, cap.getNumSamples()) > 1e-4f)
            break;
        if (tries > 120)
            return 1;
        juce::Thread::sleep (1000);
    }

    const int preMs = 100, postMs = 1000;
    const double pauseMs = juce::SystemStats::getEnvironmentVariable ("PAUSE_MS", "2000").getDoubleValue();
    LegatoParams lp;
    lp.sampleRate = fs;
    lp.latencySamples = latency;
    // one render: reset, A at tA (held), optionally B at tA+gap; capture around B's note-on
    const bool noKs = juce::SystemStats::getEnvironmentVariable ("NO_KS", "0") == "1";
    auto render = [&] (bool withB, int vel, double gap) {
        if (! noKs)
        {
            e.at (e.now, juce::MidiMessage::noteOn (1, ks, (juce::uint8) 100));
            e.at (e.now + e.ms (30), juce::MidiMessage::noteOff (1, ks));
        }
        e.at (e.now + e.ms (40), juce::MidiMessage::controllerEvent (1, 1, cc1));
        // wait for digital silence (release tails are up to ~3 s), then the RR reset time
        for (int quiet = 0, guard = 0; quiet < 10 && guard < 4000; ++guard)
        {
            juce::AudioBuffer<float> q (2, bs);
            q.clear();
            e.run (e.now + bs, &q, e.now);
            quiet = q.getMagnitude (0, bs) < 1.0e-5f ? quiet + 1 : 0;
        }
        e.run (e.now + e.ms (pauseMs));
        const int64_t tA = e.aligned();
        const int64_t tB = tA + (e.ms (gap) / bs) * bs;
        e.at (tA, juce::MidiMessage::noteOn (1, noteA, (juce::uint8) 100));
        if (withB)
        {
            e.at (tB, juce::MidiMessage::noteOn (1, noteB, (juce::uint8) vel));
            e.at (tB + e.ms (10), juce::MidiMessage::noteOff (1, noteA));
            e.at (tB + e.ms (postMs + 100), juce::MidiMessage::noteOff (1, noteB));
        }
        else
            e.at (tB + e.ms (postMs + 100), juce::MidiMessage::noteOff (1, noteA));
        juce::AudioBuffer<float> cap (2, (int) e.ms (preMs + postMs));
        cap.clear();
        e.run (tB + cap.getNumSamples() - e.ms (preMs), &cap, tB - e.ms (preMs));
        e.run (e.now + e.ms (200));
        return cap;
    };

    if (juce::SystemStats::getEnvironmentVariable ("CCDRIFT", "0") == "1")
    {
        // after a CC1 change: do successive identical renders of A alone drift?
        e.at (e.now, juce::MidiMessage::controllerEvent (1, 1, 0));
        e.run (e.now + e.ms (3000));
        std::vector<juce::AudioBuffer<float>> rs;
        const double tw0 = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < 12; ++i)
        {
            rs.push_back (render (false, 0, gaps[0]));
            if (const int sl = juce::SystemStats::getEnvironmentVariable ("SLEEP_MS", "0").getIntValue(); sl > 0)
                juce::Thread::sleep (sl);
        }
        for (size_t i = 0; i < rs.size(); ++i)
        {
            float md = 0;
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < rs[i].getNumSamples(); ++k)
                    md = juce::jmax (md, std::abs (rs[i].getSample (c, k) - rs.back().getSample (c, k)));
            std::printf ("render %2zu: differs from the last by %6.1f dBFS, level %.2f dBFS\n", i, juce::Decibels::gainToDecibels (md, -200.0f),
                         juce::Decibels::gainToDecibels (rs[i].getMagnitude (0, rs[i].getNumSamples()), -200.0f));
        }
        std::printf ("wall %.0f ms\n", juce::Time::getMillisecondCounterHiRes() - tw0);
        return 0;
    }
    if (const auto precede = juce::SystemStats::getEnvironmentVariable ("PRECEDE", ""); precede.isNotEmpty())
    {
        // does the note played before the reset pause change how a fresh A sounds?
        juce::AudioBuffer<float> ref;
        for (auto& xs : juce::StringArray::fromTokens (precede, ",", ""))
        {
            const int x = parseNoteName (xs);
            for (int rep = 0; rep < 2; ++rep)
            {
                // play X (with the same velocity) then let everything ring out
                e.at (e.now, juce::MidiMessage::noteOn (1, x, (juce::uint8) vels[0]));
                e.at (e.now + e.ms (400), juce::MidiMessage::noteOff (1, x));
                e.run (e.now + e.ms (450));
                auto a = render (false, 0, gaps[0]);
                if (ref.getNumSamples() == 0)
                    ref.makeCopyOf (a);
                float md = 0;
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < a.getNumSamples(); ++i)
                        md = juce::jmax (md, std::abs (a.getSample (c, i) - ref.getSample (c, i)));
                std::printf ("after %-4s (rep %d): A-alone differs from the first by %.1f dBFS\n", xs.toRawUTF8(), rep,
                             juce::Decibels::gainToDecibels (md, -200.0f));
            }
        }
        return 0;
    }
    if (const int chain = juce::SystemStats::getEnvironmentVariable ("CHAIN", "0").getIntValue(); chain > 1)
    {
        // A B A B ... legato at `gap`; transition k measured as render(0..k) vs render(0..k-1)
        const double gap = gaps[0];
        const int vel = (int) vels[0];
        auto renderChain = [&] (int count) {
            auto once = [&] {
                e.at (e.now, juce::MidiMessage::noteOn (1, ks, (juce::uint8) 100));
                e.at (e.now + e.ms (30), juce::MidiMessage::noteOff (1, ks));
                e.at (e.now + e.ms (40), juce::MidiMessage::controllerEvent (1, 1, cc1));
                for (int quiet = 0, guard = 0; quiet < 10 && guard < 4000; ++guard)
                {
                    juce::AudioBuffer<float> q (2, bs);
                    q.clear();
                    e.run (e.now + bs, &q, e.now);
                    quiet = q.getMagnitude (0, bs) < 1.0e-5f ? quiet + 1 : 0;
                }
                e.run (e.now + e.ms (pauseMs));
                const int64_t t0 = e.aligned(), step = (e.ms (gap) / bs) * bs;
                for (int k = 0; k < count; ++k)
                {
                    e.at (t0 + k * step, juce::MidiMessage::noteOn (1, k % 2 ? noteB : noteA, (juce::uint8) vel));
                    if (k > 0)
                        e.at (t0 + k * step + e.ms (10), juce::MidiMessage::noteOff (1, k % 2 ? noteA : noteB));
                }
                const int64_t last = t0 + (count - 1) * step;
                juce::AudioBuffer<float> cap (2, (int) (last - t0 + e.ms (1000 + preMs)));
                cap.clear();
                e.run (last + e.ms (1000), &cap, t0 - e.ms (preMs));
                e.at (e.now, juce::MidiMessage::noteOff (1, (count - 1) % 2 ? noteB : noteA));
                e.run (e.now + e.ms (100));
                return cap;
            };
            auto prev = once();
            for (int a = 0; a < 6; ++a)
            {
                juce::Thread::sleep (20);
                auto cur = once();
                bool same = true;
                for (int c = 0; c < 2 && same; ++c)
                    same = std::memcmp (cur.getReadPointer (c), prev.getReadPointer (c), sizeof (float) * (size_t) cur.getNumSamples()) == 0;
                if (same)
                    return cur;
                prev = std::move (cur);
            }
            std::puts ("NOT CONVERGED");
            return prev;
        };
        std::vector<juce::AudioBuffer<float>> rr;
        for (int k = 0; k < chain; ++k)
            rr.push_back (renderChain (k + 1));
        const int64_t step = (e.ms (gap) / bs) * bs;
        for (int k = 1; k < chain; ++k)
        {
            // align render(0..k-1) (shorter) with render(0..k): both start at t0 - pre
            juce::AudioBuffer<float> withB (2, rr[(size_t) k].getNumSamples()), alone (2, rr[(size_t) k].getNumSamples());
            withB.makeCopyOf (rr[(size_t) k]);
            alone.clear();
            for (int c = 0; c < 2; ++c)
                alone.copyFrom (c, 0, rr[(size_t) k - 1], c, 0, juce::jmin (alone.getNumSamples(), rr[(size_t) k - 1].getNumSamples()));
            const int tk = (int) (e.ms (preMs) + k * step);
            const int src = k % 2 ? noteA : noteB, dst = k % 2 ? noteB : noteA;
            auto res = analyseLegato (withB, alone, tk, src, dst, lp);
            // similarity of this transition's new material with earlier same-direction transitions
            juce::AudioBuffer<float> d (2, (int) e.ms (400));
            for (int c = 0; c < 2; ++c)
            {
                d.copyFrom (c, 0, withB, c, tk, d.getNumSamples());
                d.addFrom (c, 0, alone, c, tk, d.getNumSamples(), -1.0f);
            }
            static std::vector<std::pair<int, juce::AudioBuffer<float>>> seen;
            juce::String sims;
            for (auto& [kk, dd] : seen)
                if (kk % 2 == k % 2)
                    sims << "k" << kk << ":" << juce::String (similarity (dd, d, 0, d.getNumSamples()), 4) << " ";
            seen.emplace_back (k, d);
            std::printf ("transition %2d %s->%s  diverge %7.2f start %7.2f mid %7.2f arrive %7.2f  %s | %s\n", k, noteName (src).toRawUTF8(),
                         noteName (dst).toRawUTF8(), res.divergeMs, res.startMs, res.midMs, res.arriveMs, res.failure.toRawUTF8(), sims.toRawUTF8());
        }
        return 0;
    }
    if (const int n = juce::SystemStats::getEnvironmentVariable ("DETERMINISM", "0").getIntValue(); n > 0)
    {
        // render the same A+B n times; report how each differs from the first
        std::vector<juce::AudioBuffer<float>> rs;
        for (int i = 0; i < n; ++i)
        {
            const double t = juce::Time::getMillisecondCounterHiRes();
            rs.push_back (render (true, (int) vels[0], gaps[0]));
            juce::Thread::sleep (juce::SystemStats::getEnvironmentVariable ("SLEEP_MS", "0").getIntValue());
            auto& r = rs.back();
            int firstDiff = -1;
            float md = 0;
            for (int k = 0; k < r.getNumSamples(); ++k)
                for (int c = 0; c < 2; ++c)
                {
                    const float d = std::abs (r.getSample (c, k) - rs[0].getSample (c, k));
                    if (d > 1e-6f && firstDiff < 0)
                        firstDiff = k;
                    md = juce::jmax (md, d);
                }
            auto lvl = [&] (int from, int len) { return juce::Decibels::gainToDecibels (r.getMagnitude (from, len), -200.0f); };
            const int tb = (int) e.ms (preMs);
            std::printf ("render %2d: pre-B %6.1f dB, B..+200ms %6.1f dB, +200..+1000 %6.1f dB | diff to #0 %6.1f dB from %7.1f ms | %.0f ms wall\n", i,
                         lvl (0, tb), lvl (tb, (int) e.ms (200)), lvl (tb + (int) e.ms (200), (int) e.ms (800)),
                         juce::Decibels::gainToDecibels (md, -200.0f), firstDiff < 0 ? 0.0 : (firstDiff - tb) * 1000.0 / fs,
                         juce::Time::getMillisecondCounterHiRes() - t);
        }
        return 0;
    }
    // SINE streams transition samples: the first play of a cold sample differs, and now and then a
    // note is dropped. Render until two consecutive renders are bit-identical.
    auto converged = [&] (bool withB, int vel, double gap) {
        auto prev = render (withB, vel, gap);
        for (int attempt = 1; attempt < 6; ++attempt)
        {
            auto cur = render (withB, vel, gap);
            bool same = true;
            for (int c = 0; c < 2 && same; ++c)
                same = std::memcmp (cur.getReadPointer (c), prev.getReadPointer (c), sizeof (float) * (size_t) cur.getNumSamples()) == 0;
            if (same)
            {
                if (attempt > 1)
                    std::printf ("  (converged after %d renders)\n", attempt + 1);
                return cur;
            }
            prev = std::move (cur);
        }
        std::printf ("  NOT CONVERGED\n");
        return prev;
    };

    const int t0 = (int) e.ms (preMs);
    struct Row { double gap; int vel; LegatoResult r; juce::AudioBuffer<float> cap; };
    std::vector<Row> rows;
    std::map<double, juce::AudioBuffer<float>> alone;
    for (auto gap : gaps)
    {
        alone[gap] = converged (false, 0, gap);
        auto again = render (false, 0, gap);
        float md = 0;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < again.getNumSamples(); ++i)
                md = juce::jmax (md, std::abs (again.getSample (c, i) - alone[gap].getSample (c, i)));
        std::printf ("gap %4.0f ms: A-alone vs A-alone max diff %.1f dBFS (peak %.1f dBFS)\n", gap, juce::Decibels::gainToDecibels (md, -200.0f),
                     juce::Decibels::gainToDecibels (again.getMagnitude (0, again.getNumSamples()), -200.0f));
        for (auto v : vels)
        {
            Row row { gap, (int) v, {}, converged (true, (int) v, gap) };
            if (juce::SystemStats::getEnvironmentVariable ("REPEAT", "0").getIntValue() > 0)
            {
                auto second = render (true, (int) v, gap);
                float md2 = 0;
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < second.getNumSamples(); ++i)
                        md2 = juce::jmax (md2, std::abs (second.getSample (c, i) - row.cap.getSample (c, i)));
                std::printf ("  gap %.0f vel %d: first vs repeated A+B render max diff %.1f dBFS\n", gap, (int) v, juce::Decibels::gainToDecibels (md2, -200.0f));
            }
            row.r = analyseLegato (row.cap, alone[gap], t0, noteA, noteB, lp);
            float pre = 0;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < t0 + latency; ++i)
                    pre = juce::jmax (pre, std::abs (row.cap.getSample (c, i) - alone[gap].getSample (c, i)));
            if (pre > 0)
                std::printf ("  gap %.0f vel %d: differs from A-alone BEFORE B: %.1f dBFS\n", gap, (int) v, juce::Decibels::gainToDecibels (pre, -200.0f));
            rows.push_back (std::move (row));
        }
    }
    // label transition audio over the first 400 ms after B's note-on
    std::vector<int> label (rows.size(), -1);
    int next = 0;
    for (size_t k = 0; k < rows.size(); ++k)
    {
        for (size_t j = 0; j < k && label[k] < 0; ++j)
            if (similarity (rows[j].cap, rows[k].cap, t0, (int) e.ms (400)) > 0.99)
                label[k] = label[j];
        if (label[k] < 0)
            label[k] = next++;
    }
    std::printf ("%6s %4s %3s  %8s %8s %8s %8s %8s  %6s %6s %6s %5s  %s\n", "gap", "vel", "id", "diverge", "start", "mid", "arrive",
                 "diffAE", "tune", "vib", "dB", "vcd", "note");
    for (size_t k = 0; k < rows.size(); ++k)
    {
        auto& r = rows[k].r;
        std::printf ("%6.0f %4d  %c   %8.2f %8.2f %8.2f %8.2f %8.2f  %6.1f %6.1f %6.1f %5.2f  %s\n", rows[k].gap, rows[k].vel,
                     'A' + juce::jmin (25, label[k]), r.divergeMs, r.startMs, r.midMs, r.arriveMs, r.diffAttackEndMs,
                     r.targetCentsError, r.vibratoCents, r.steadyDb, r.voicedFraction, r.failure.toRawUTF8());
    }
    if (argc > 9) // dump the pitch track of the first row
    {
        PitchTrackParams pp;
        pp.minHz = juce::jmin (midiToHz (noteA), midiToHz (noteB)) / 1.3;
        pp.maxHz = juce::jmax (midiToHz (noteA), midiToHz (noteB)) * 1.3;
        pp.hopMs = 5;
        pp.windowPeriods = 2.0;
        pp.probeHz[0] = midiToHz (noteA);
        pp.probeHz[1] = midiToHz (noteB);
        pp.probeCents = juce::jmin (40.0, std::abs (noteB - noteA) * 100.0 / 3.0);
        const auto which = (size_t) juce::String (argv[9]).getIntValue();
        for (auto& f : trackPitch (rows[which].cap, 0, rows[which].cap.getNumSamples(), t0 + latency, pp))
            std::printf ("  %7.1f ms  %8.2f Hz  ap %.2f  %6.1f dB  dA %.2f dB %.2f\n", f.timeMs, f.hz, f.aperiodicity, f.rmsDb, f.probe[0], f.probe[1]);
    }
    return 0;
}
