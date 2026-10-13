#include "DelayExport.h"
#include "PresetModel.h"

namespace tp
{
namespace
{
    juce::var r2 (const juce::var& v)
    {
        if (v.isVoid() || ! (v.isDouble() || v.isInt() || v.isInt64()))
            return {};
        return std::round ((double) v * 100.0) / 100.0;
    }

    juce::DynamicObject* child (juce::DynamicObject* parent, const juce::String& key)
    {
        if (! parent->hasProperty (key))
            parent->setProperty (key, juce::var (new juce::DynamicObject()));
        return parent->getProperty (key).getDynamicObject();
    }

    // note-name keyed object -> MIDI-number keyed, in ascending note order
    template <typename Fn>
    void forEachNote (const juce::var& byName, Fn&& fn)
    {
        auto* obj = byName.getDynamicObject();
        if (obj == nullptr)
            return;
        std::vector<std::pair<int, juce::var>> notes;
        for (auto& p : obj->getProperties())
            if (const int n = parseNoteName (p.name.toString()); n >= 0)
                notes.emplace_back (n, p.value);
        std::sort (notes.begin(), notes.end(), [] (auto& a, auto& b) { return a.first < b.first; });
        for (auto& [n, v] : notes)
            fn (n, v);
    }
} // namespace

juce::var compactDelays (const juce::var& full)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("format", "secant.delays");
    root->setProperty ("version", 1);
    root->setProperty ("unit", "ms; negative track delay to apply (already negated), plugin latency removed");
    root->setProperty ("noteKeys", "MIDI note number (C3 = 60)");
    root->setProperty ("layout", "articulations[].notes[note][dynamic] = [rr1, rr2, ...] in the player's round-robin order; "
                                 "afterPause[note][dynamic] only where a phrase's first note plays a sample outside that cycle");
    const auto& preset = full["preset"];
    root->setProperty ("collection", preset["collection"]);
    root->setProperty ("instrument", preset["instrument"]);
    root->setProperty ("measuredAt", full["createdAt"]);
    root->setProperty ("sampleRate", full["render"]["sampleRate"]);

    juce::Array<juce::var> arts;
    if (auto* list = full["articulations"].getArray())
        for (auto& a : *list)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("name", a["name"]);
            o->setProperty ("keyswitch", a["keyswitch"]);
            o->setProperty ("dynamicsSource", a["dynamicsSource"]); // velocity or cc1: how the layers below are reached
            juce::Array<juce::var> dyn;
            if (auto* ds = a["dynamics"].getArray())
                for (auto& d : *ds)
                {
                    auto* x = new juce::DynamicObject();
                    x->setProperty ("name", d["name"]);
                    x->setProperty (a["dynamicsSource"].toString() == "cc1" ? "cc1" : "velocity",
                                    a["dynamicsSource"].toString() == "cc1" ? d["cc1"] : d["velocity"]);
                    dyn.add (juce::var (x));
                }
            o->setProperty ("dynamics", dyn);

            auto* notes = new juce::DynamicObject();
            auto* afterPause = new juce::DynamicObject();
            int maxRR = 0;
            forEachNote (a["negativeDelayMsTable"], [&] (int note, const juce::var& perDyn) {
                auto* d = perDyn.getDynamicObject();
                if (d == nullptr)
                    return;
                for (auto& p : d->getProperties())
                {
                    const auto& cell = p.value;
                    juce::Array<juce::var> rr;
                    if (auto* cyc = cell["cycle"].getArray())
                        for (auto& v : *cyc)
                            rr.add (r2 (v));
                    if (rr.isEmpty() && ! cell["afterPause"].isVoid())
                        rr.add (r2 (cell["afterPause"]));
                    if (rr.isEmpty())
                        continue;
                    maxRR = juce::jmax (maxRR, rr.size());
                    child (notes, juce::String (note))->setProperty (p.name, rr);
                    if (! (bool) cell["afterPauseIsCycleSample"] && ! cell["afterPause"].isVoid() && cell["cycle"].size() > 0)
                        child (afterPause, juce::String (note))->setProperty (p.name, r2 (cell["afterPause"]));
                }
            });
            o->setProperty ("roundRobins", maxRR);
            o->setProperty ("notes", juce::var (notes));
            if (! afterPause->getProperties().isEmpty())
                o->setProperty ("afterPause", juce::var (afterPause));
            else
                delete afterPause;

            // legato: per type, the median transition delay for each note spacing played
            const auto& L = a["legato"];
            if (auto* sugg = L["suggestedNegativeDelayMs"].getDynamicObject(); sugg != nullptr && ! sugg->getProperties().isEmpty())
            {
                auto* lg = new juce::DynamicObject();
                for (auto& t : sugg->getProperties())
                {
                    auto* x = new juce::DynamicObject();
                    if (auto* zones = L["zones"].getArray())
                        for (auto& z : *zones)
                            if (z["type"].toString() == t.name.toString())
                            {
                                x->setProperty ("velocity", z["velocity"]);
                                break;
                            }
                    auto* byGap = new juce::DynamicObject();
                    if (auto* g = t.value.getDynamicObject())
                        for (auto& gp : g->getProperties())
                            byGap->setProperty (gp.name, r2 (gp.value));
                    x->setProperty ("byNoteSpacingMs", juce::var (byGap));
                    lg->setProperty (t.name, juce::var (x));
                }
                o->setProperty ("legato", juce::var (lg));
            }
            arts.add (juce::var (o));
        }
    root->setProperty ("articulations", arts);
    return juce::var (root);
}

juce::File delaysFileFor (const juce::File& resultsFile)
{
    return resultsFile.getSiblingFile (resultsFile.getFileNameWithoutExtension() + ".delays.json");
}

juce::Result writeCompactDelays (const juce::File& resultsFile, juce::File out)
{
    juce::var full;
    if (auto r = juce::JSON::parse (resultsFile.loadFileAsString(), full); r.failed())
        return juce::Result::fail ("Could not read " + resultsFile.getFullPathName() + ": " + r.getErrorMessage());
    // (transientprobe.results: files written under the app's former name)
    if (full["format"].toString() != "secant.results" && full["format"].toString() != "transientprobe.results")
        return juce::Result::fail (resultsFile.getFileName() + " is not a Secant results file");
    if (out == juce::File())
        out = delaysFileFor (resultsFile);
    if (! out.replaceWithText (juce::JSON::toString (compactDelays (full), false)))
        return juce::Result::fail ("Could not write " + out.getFullPathName());
    return juce::Result::ok();
}

} // namespace tp
