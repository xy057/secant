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

namespace
{
    juce::Result loadResults (const juce::File& resultsFile, juce::var& full)
    {
        if (auto r = juce::JSON::parse (resultsFile.loadFileAsString(), full); r.failed())
            return juce::Result::fail ("Could not read " + resultsFile.getFullPathName() + ": " + r.getErrorMessage());
        // (transientprobe.results: files written under the app's former name)
        if (full["format"].toString() != "secant.results" && full["format"].toString() != "transientprobe.results")
            return juce::Result::fail (resultsFile.getFileName() + " is not a Secant results file");
        return juce::Result::ok();
    }
} // namespace

juce::Result writeCompactDelays (const juce::File& resultsFile, juce::File out)
{
    juce::var full;
    if (auto r = loadResults (resultsFile, full); r.failed())
        return r;
    if (out == juce::File())
        out = delaysFileFor (resultsFile);
    if (! out.replaceWithText (juce::JSON::toString (compactDelays (full), false)))
        return juce::Result::fail ("Could not write " + out.getFullPathName());
    return juce::Result::ok();
}

namespace
{
    double median (std::vector<double> v)
    {
        if (v.empty())
            return std::numeric_limits<double>::quiet_NaN();
        std::sort (v.begin(), v.end());
        return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
    }

    // the GUI's "L" median (ResultStore::medianLegatoFor): every usable transition, all types and spacings
    double legatoMedianMs (const juce::var& art)
    {
        std::vector<double> v;
        if (auto* ts = art["legato"]["transitions"].getArray())
            for (auto& t : *ts)
                if ((bool) t["valid"] && ! t["primaryMs"].isVoid())
                    v.push_back ((double) t["primaryMs"]);
        return median (std::move (v));
    }

    // the GUI's onset median (ResultStore::medianPrimaryFor) = summary.primary over the same hits
    double onsetMedianMs (const juce::var& art)
    {
        const auto& m = art["summary"]["primary"]["median"];
        return m.isVoid() || (int) art["summary"]["primary"]["count"] == 0 ? std::numeric_limits<double>::quiet_NaN() : (double) m;
    }

    // closest built-in Cubase playback technique, so the score and other maps can still match it
    juce::String techniqueIdFor (const juce::String& articulationName)
    {
        const auto n = articulationName.toLowerCase();
        static const std::pair<const char*, const char*> table[] = {
            { "legato", "pt.legato" }, { "staccatissimo", "pt.staccatissimo" }, { "stacc", "pt.staccato" },
            { "spicc", "pt.spiccato" }, { "pizz", "pt.pizzicato" }, { "trem", "pt.tremolo" }, { "trill", "pt.trill" },
            { "col legno", "pt.colLegnoBattuto" }, { "sul pont", "pt.sulPonticello" }, { "sul tasto", "pt.sulTasto" },
            { "flaut", "pt.flautando" }, { "harmonic", "pt.naturalHarmonic" }, { "marcato", "pt.marcato" },
            { "tenuto", "pt.tenuto" }, { "portato", "pt.portatoloure" }, { "sfz", "pt.sfz" }, { "sforzando", "pt.sfz" },
            { "cresc", "pt.crescendo" }, { "dim", "pt.diminuendo" }, { "mute", "pt.muted" }, { "sord", "pt.muted" },
        };
        for (auto& [key, id] : table)
            if (n.contains (key))
                return id;
        return "pt.natural";
    }

    // Cubase's typed serialization: <int name="x" value="1"/>, <string name="x" value="..." wide="true"/>
    struct MapWriter
    {
        juce::int64 nextId = 1000000000;

        juce::String newId() { return juce::String (nextId++); }

        static juce::XmlElement* add (juce::XmlElement& parent, const char* tag, const juce::String& name)
        {
            auto* e = parent.createNewChildElement (tag);
            if (name.isNotEmpty())
                e->setAttribute ("name", name);
            return e;
        }
        static void addInt (juce::XmlElement& p, const juce::String& name, int v) { add (p, "int", name)->setAttribute ("value", v); }
        static void addFloat (juce::XmlElement& p, const juce::String& name, float v) { add (p, "float", name)->setAttribute ("value", juce::String (v)); }
        static void addString (juce::XmlElement& p, const juce::String& name, const juce::String& v, bool wide = true)
        {
            auto* e = add (p, "string", name);
            e->setAttribute ("value", v);
            if (wide)
                e->setAttribute ("wide", "true");
        }
        juce::XmlElement* addObj (juce::XmlElement& p, const char* cls, const juce::String& name = {})
        {
            auto* e = p.createNewChildElement ("obj");
            e->setAttribute ("class", cls);
            if (name.isNotEmpty())
                e->setAttribute ("name", name);
            e->setAttribute ("ID", newId());
            return e;
        }
        static juce::XmlElement* addObjList (juce::XmlElement& member, bool owned = true)
        {
            if (owned)
                addInt (member, "ownership", 1);
            auto* l = member.createNewChildElement ("list");
            l->setAttribute ("name", "obj");
            l->setAttribute ("type", "obj");
            return l;
        }
        static void addNoteModifiers (juce::XmlElement& p, bool withPitchOrderOfAction)
        {
            addInt (p, "channel", -1);
            addFloat (p, "velocityFact", 1.0f);
            addFloat (p, "lengthFact", 1.0f);
            addInt (p, "minVelocity", 0);
            addInt (p, "maxVelocity", 127);
            addInt (p, "transpose", 0);
            if (withPitchOrderOfAction) // (PSlotMidiAction writes maxPitch before minPitch)
            {
                addInt (p, "maxPitch", 127);
                addInt (p, "minPitch", 0);
            }
            else
            {
                addInt (p, "minPitch", 0);
                addInt (p, "maxPitch", 127);
            }
        }

        // Cubase's reader doesn't take self-closing containers: after <member name="offMidiMessages"/>
        // it drops the rest of the slot and every later slot. Write them as <member ...></member>.
        static void closeEmptyContainers (juce::XmlElement& e)
        {
            if ((e.hasTagName ("member") || e.hasTagName ("list")) && e.getFirstChildElement() == nullptr)
                e.addChildElement (juce::XmlElement::createTextElement ({}));
            for (auto* c : e.getChildIterator())
                if (! c->isTextElement())
                    closeEmptyContainers (*c);
        }
    };
} // namespace

juce::String cubaseExpressionMap (const juce::var& full)
{
    MapWriter w;
    juce::XmlElement root ("InstrumentMap");
    const auto& preset = full["preset"];
    const auto instrument = preset["instrument"].toString();
    MapWriter::addString (root, "name", preset["collection"].toString() + (instrument.isNotEmpty() ? " - " + instrument : juce::String()));
    MapWriter::addInt (root, "fileVersion", 2);

    struct Slot { juce::String name, visualId; int keyswitch; int delayMs; };
    std::vector<Slot> slots;

    auto* visuals = MapWriter::addObjList (*MapWriter::add (root, "member", "slotvisuals"));
    if (auto* list = full["articulations"].getArray())
        for (auto& a : *list)
        {
            const auto name = a["name"].toString();
            auto ms = legatoMedianMs (a); // legato articulations: the transition delay, not the first note's
            if (std::isnan (ms))
                ms = onsetMedianMs (a);
            const int ks = a["keyswitch"].isVoid() ? -1 : (int) a["keyswitch"];

            auto* v = w.addObj (*visuals, "USlotVisuals");
            MapWriter::addInt (*v, "displaytype", 1);      // text
            MapWriter::addInt (*v, "articulationtype", 1); // direction: holds until the next one, like a keyswitch
            MapWriter::addString (*v, "text", name);
            MapWriter::addInt (*v, "symbol", 0);
            MapWriter::addInt (*v, "smuflSymbol", -1);
            MapWriter::addString (*v, "description", name);
            MapWriter::addInt (*v, "group", 0);            // one group: the articulations exclude each other
            MapWriter::addInt (*v, "userPlayingTechnique", 1);
            MapWriter::addString (*v, "techniqueID", techniqueIdFor (name), false);
            MapWriter::addString (*v, "ptaID", "pta.user." + name, false);
            MapWriter::addInt (*v, "ptaCategory", 1);
            slots.push_back ({ name, v->getStringAttribute ("ID"), ks,
                               std::isnan (ms) ? 0 : juce::jmax (0, juce::roundToInt (ms)) });
        }

    auto* slotList = MapWriter::addObjList (*MapWriter::add (root, "member", "slots"));
    for (size_t i = 0; i < slots.size(); ++i)
    {
        const auto& s = slots[i];
        auto* slot = w.addObj (*slotList, "PSoundSlot");

        auto* remote = w.addObj (*slot, "PSlotThruTrigger", "remote");
        if (s.keyswitch >= 0)
        {
            MapWriter::addInt (*remote, "status", 0x90);
            MapWriter::addInt (*remote, "data1", s.keyswitch);
        }
        MapWriter::addInt (*remote, "remoteTriggerMode", 0);
        MapWriter::addInt (*remote, "version", 2);

        auto* action = w.addObj (*slot, "PSlotMidiAction", "action");
        MapWriter::addInt (*action, "version", 1500);
        auto* changers = MapWriter::addObjList (*MapWriter::add (*action, "member", "noteChanger"));
        MapWriter::addNoteModifiers (*w.addObj (*changers, "PSlotNoteChanger"), false);
        auto* on = MapWriter::add (*action, "member", "midiMessages");
        if (s.keyswitch >= 0)
        {
            auto* ev = w.addObj (*MapWriter::addObjList (*on), "POutputEvent");
            MapWriter::addInt (*ev, "status", 0x90);
            MapWriter::addInt (*ev, "data1", s.keyswitch);
            MapWriter::addInt (*ev, "data2", 120);
        }
        else
            MapWriter::addInt (*on, "ownership", 1);
        MapWriter::add (*action, "member", "offMidiMessages");
        MapWriter::addInt (*action, "preDelayMs", s.delayMs); // Attack Compensation: notes start this much earlier
        MapWriter::addInt (*action, "actionSeparationMs", 0);
        MapWriter::addNoteModifiers (*action, true);
        MapWriter::addInt (*action, "key", s.keyswitch);

        auto* sv = MapWriter::addObjList (*MapWriter::add (*slot, "member", "sv"), false);
        sv->createNewChildElement ("item")->setAttribute ("value", s.visualId);
        MapWriter::addString (*MapWriter::add (*slot, "member", "name"), "s", s.name);
        MapWriter::addInt (*slot, "color", (int) (i % 16));
        MapWriter::addInt (*slot, "slotType", 0);
        MapWriter::addString (*slot, "visualIndexes", juce::String ((int) i));
    }

    auto* groups = MapWriter::addObjList (*MapWriter::add (root, "member", "techniqueGroups"), false);
    auto* g = w.addObj (*groups, "ExpressionMapTechniqueGroup");
    MapWriter::addString (*g, "name", "Articulations", false);
    MapWriter::addString (*g, "id", {}, false);
    MapWriter::add (root, "member", "customPlaybackTechniques");
    MapWriter::addInt (*MapWriter::add (root, "member", "controller"), "ownership", 1);
    MapWriter::addInt (root, "remoteTriggerMode", 0);
    MapWriter::addInt (root, "latchModeActive", 1);

    MapWriter::closeEmptyContainers (root);
    juce::XmlElement::TextFormat fmt;
    fmt.customEncoding = "utf-8";
    fmt.newLineChars = "\n";
    fmt.lineWrapLength = 1 << 20; // never wrap attributes onto new lines
    return root.toString (fmt);
}

juce::File expressionMapFileFor (const juce::File& resultsFile)
{
    return resultsFile.withFileExtension ("expressionmap");
}

juce::Result writeExpressionMap (const juce::File& resultsFile, juce::File out)
{
    juce::var full;
    if (auto r = loadResults (resultsFile, full); r.failed())
        return r;
    if (out == juce::File())
        out = expressionMapFileFor (resultsFile);
    if (! out.replaceWithText (cubaseExpressionMap (full)))
        return juce::Result::fail ("Could not write " + out.getFullPathName());
    return juce::Result::ok();
}

} // namespace tp
