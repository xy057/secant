// Round-robin behaviour probe: loads a VST3 with a saved state (.pluginstate written by
// Secant), selects an articulation by keyswitch, and plays the same note repeatedly
// at several note spacings. Each hit is labelled by which sample played (A, B, C...), which
// shows whether / how the plugin cycles round robins and whether pauses reset the counter.
//
//   RRProbe <plugin.vst3> <state.pluginstate> <keyswitch> <note> [velocity=100] [cc1=100] [hits=12] [gapsMs=150,400,...]

#include <juce_audio_processors/juce_audio_processors.h>
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

    // render until `end`; copy channels 0/1 into cap (cap starts at capStart) if given
    void run (int64_t end, juce::AudioBuffer<float>* cap = nullptr, int64_t capStart = 0)
    {
        while (now < end)
        {
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
    int64_t ms (double v) const { return (int64_t) (v * fs / 1000.0); }
};
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 5)
    {
        std::puts ("usage: RRProbe <plugin.vst3> <state.pluginstate> <keyswitch> <note> [velocity=100] [cc1=100] [hits=12]");
        return 2;
    }
    const int ks = parseNoteName (argv[3]), note = parseNoteName (argv[4]);
    const int vel = argc > 5 ? juce::String (argv[5]).getIntValue() : 100;
    const int cc1 = argc > 6 ? juce::String (argv[6]).getIntValue() : 100;
    const int hits = argc > 7 ? juce::String (argv[7]).getIntValue() : 12;

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
    {
        std::puts ("cannot read state file");
        return 1;
    }
    plugin->enableAllBuses();
    plugin->setStateInformation (state.getData(), (int) state.getSize());
    plugin->setNonRealtime (true);
    plugin->prepareToPlay (fs, bs);
    std::printf ("plugin %s, keyswitch %s, note %s, vel %d, cc1 %d\n", descs[0]->name.toRawUTF8(), noteName (ks).toRawUTF8(),
                 noteName (note).toRawUTF8(), vel, cc1);

    Engine e (*plugin);
    // wait until the instrument sounds (samples load asynchronously)
    for (int tries = 0;; ++tries)
    {
        e.at (e.now, juce::MidiMessage::noteOn (1, ks, (juce::uint8) 100));
        e.at (e.now + e.ms (30), juce::MidiMessage::noteOff (1, ks));
        e.run (e.now + e.ms (150));
        juce::AudioBuffer<float> cap (2, (int) e.ms (300));
        cap.clear();
        const int64_t t = e.now;
        e.at (t, juce::MidiMessage::noteOn (1, note, (juce::uint8) vel));
        e.at (t + e.ms (200), juce::MidiMessage::noteOff (1, note));
        e.run (t + cap.getNumSamples(), &cap, t);
        e.at (e.now, juce::MidiMessage::allSoundOff (1));
        e.run (e.now + e.ms (2000));
        if (cap.getMagnitude (0, 0, cap.getNumSamples()) > 1e-4f)
            break;
        if (tries > 120)
        {
            std::puts ("instrument never sounded");
            return 1;
        }
        juce::Thread::sleep (1000);
    }

    struct Scenario { const char* name; double gapMs; bool allSoundOff; double holdMs; };
    std::vector<Scenario> scenarios = {
        { "fast 150 ms, overlapping", 150, false, 120 },
        { "400 ms", 400, false, 300 },
        { "1 s", 1000, false, 600 },
        { "1 s + all-sound-off", 1000, true, 600 },
        { "3 s", 3000, false, 600 },
        { "6 s", 6000, false, 600 },
    };
    std::vector<std::string> names;
    if (argc > 8)
    {
        scenarios.clear();
        names.clear();
        for (auto& g : juce::StringArray::fromTokens (argv[8], ",", ""))
            names.push_back ((g + " ms (gap:hold)").toStdString());
        int i = 0;
        // entries "gap" or "gap:hold" (ms)
        for (auto& g : juce::StringArray::fromTokens (argv[8], ",", ""))
        {
            const double gap = g.upToFirstOccurrenceOf (":", false, false).getDoubleValue();
            const double hold = g.contains (":") ? g.fromFirstOccurrenceOf (":", false, false).getDoubleValue() : juce::jmin (600.0, gap * 0.6);
            scenarios.push_back ({ names[(size_t) i++].c_str(), gap, false, hold });
        }
    }

    e.at (e.now, juce::MidiMessage::controllerEvent (1, 1, cc1));
    e.run (e.now + e.ms (500));
    for (auto& sc : scenarios)
    {
        // keyswitch again + long silence so every scenario starts from the same state
        e.at (e.now, juce::MidiMessage::noteOn (1, ks, (juce::uint8) 100));
        e.at (e.now + e.ms (30), juce::MidiMessage::noteOff (1, ks));
        e.at (e.now + e.ms (40), juce::MidiMessage::allSoundOff (1));
        e.run (e.now + e.ms (4000));

        const int win = (int) e.ms (juce::jmin (100.0, sc.gapMs - 20.0));
        std::vector<juce::AudioBuffer<float>> caps;
        std::vector<double> peaks;
        float worstPre = 0, attackPeak = 0;
        int64_t t = (e.now / bs + 1) * bs; // note-ons on block boundaries
        for (int k = 0; k < hits; ++k, t += e.ms (sc.gapMs))
        {
            caps.emplace_back (2, win);
            caps.back().clear();
            juce::AudioBuffer<float> pre (2, (int) e.ms (10));
            pre.clear();
            e.run (t - pre.getNumSamples());
            e.run (t, &pre, t - pre.getNumSamples()); // level of the previous note's tail right before this note
            if (k > 0)
                worstPre = juce::jmax (worstPre, pre.getMagnitude (0, 0, pre.getNumSamples()));
            e.at (t, juce::MidiMessage::noteOn (1, note, (juce::uint8) vel));
            e.at (t + e.ms (sc.holdMs), juce::MidiMessage::noteOff (1, note));
            if (sc.allSoundOff)
                e.at (t + e.ms (sc.gapMs - 15.0), juce::MidiMessage::allSoundOff (1));
            e.run (t + win, &caps.back(), t);
            peaks.push_back (caps.back().getMagnitude (0, 0, win));
            attackPeak = juce::jmax (attackPeak, (float) peaks.back());
        }
        e.run (t);

        // label hits: same letter = same sample (similarity > 0.99 over the first window)
        std::vector<int> label (caps.size(), -1);
        int next = 0;
        juce::String seq;
        for (size_t k = 0; k < caps.size(); ++k)
        {
            for (size_t j = 0; j < k && label[k] < 0; ++j)
                if (similarity (caps[j], caps[k], 0, win) > 0.99)
                    label[k] = label[j];
            if (label[k] < 0)
                label[k] = next++;
            seq << juce::String::charToString ((juce::juce_wchar) ('A' + juce::jmin (25, label[k]))) << " ";
        }
        std::printf ("  %-26s %s  -> %d distinct   tail before next note: %6.1f dB re attack peak\n", sc.name, seq.toRawUTF8(), next,
                     juce::Decibels::gainToDecibels (worstPre / juce::jmax (1e-9f, attackPeak), -200.0f));
    }
    return 0;
}
