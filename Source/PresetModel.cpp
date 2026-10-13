#include "PresetModel.h"

namespace tp
{
juce::String noteName (int midiNote)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (midiNote < 0 || midiNote > 127)
        return "?";
    return juce::String (names[midiNote % 12]) + juce::String (midiNote / 12 - (5 - kMiddleCOctave));
}

int parseNoteName (const juce::String& raw)
{
    auto s = raw.trim();
    if (s.isEmpty())
        return -1;
    if (s.containsOnly ("0123456789"))
        return juce::jlimit (0, 127, s.getIntValue());

    static const int pcs[] = { 9, 11, 0, 2, 4, 5, 7 }; // A..G
    auto letter = juce::CharacterFunctions::toUpperCase (s[0]);
    if (letter < 'A' || letter > 'G')
        return -1;
    int pc = pcs[letter - 'A'];
    int pos = 1;
    if (s[pos] == '#') { ++pc; ++pos; }
    else if (s[pos] == 'b') { --pc; ++pos; }
    auto octStr = s.substring (pos);
    if (! octStr.containsOnly ("-0123456789") || octStr.isEmpty())
        return -1;
    int midi = (octStr.getIntValue() + (5 - kMiddleCOctave)) * 12 + pc;
    return (midi >= 0 && midi <= 127) ? midi : -1;
}

juce::String toString (Category c)
{
    switch (c)
    {
        case Category::shortNote: return "short";
        case Category::longNote:  return "long";
        case Category::dynamic:   return "dynamic";
        case Category::phrase:    return "phrase";
    }
    return "short";
}

Category categoryFromString (const juce::String& s)
{
    if (s == "long")    return Category::longNote;
    if (s == "dynamic") return Category::dynamic;
    if (s == "phrase")  return Category::phrase;
    return Category::shortNote;
}

juce::String toString (DynamicsSource d)
{
    return d == DynamicsSource::cc1 ? "cc1" : d == DynamicsSource::autoDetect ? "auto" : "velocity";
}

DynamicsSource dynamicsSourceFromString (const juce::String& s)
{
    return s == "cc1" ? DynamicsSource::cc1 : s == "velocity" ? DynamicsSource::velocity : DynamicsSource::autoDetect;
}

juce::Array<int> Articulation::allNotes() const
{
    juce::Array<int> notes;
    for (auto& r : ranges)
        for (int n = r.lo; n <= r.hi; ++n)
            notes.addIfNotAlreadyThere (n);
    notes.sort();
    return notes;
}

juce::String Articulation::rangeText() const
{
    juce::StringArray parts;
    for (auto& r : ranges)
        parts.add (r.lo == r.hi ? noteName (r.lo) : noteName (r.lo) + "-" + noteName (r.hi));
    return parts.joinIntoString (", ");
}

juce::Result Collection::load (const juce::File& f, Collection& out)
{
    auto json = juce::JSON::parse (f);
    if (! json.isObject())
        return juce::Result::fail ("Not a JSON object: " + f.getFullPathName());
    // (transientprobe.preset: the app's former name)
    if (json["format"].toString() != "secant.preset" && json["format"].toString() != "transientprobe.preset")
        return juce::Result::fail ("Not a Secant preset (missing format: secant.preset)");

    Collection c;
    c.file = f;
    c.json = json;
    c.vendor = json["vendor"].toString();
    c.name = json["collection"].toString();
    c.player = json["player"].toString();
    c.source = json["source"].toString();
    c.keyswitchBase = json["keyswitchBase"].toString();

    if (auto* insts = json["instruments"].getArray())
    {
        for (auto& iv : *insts)
        {
            Instrument inst;
            inst.json = iv;
            inst.name = iv["name"].toString();
            inst.group = iv["group"].toString();
            if (auto* arts = iv["articulations"].getArray())
            {
                for (auto& av : *arts)
                {
                    Articulation a;
                    a.json = av;
                    a.index = (int) av["index"];
                    a.docNumber = av["docNumber"];
                    a.name = av["name"].toString();
                    a.category = categoryFromString (av["category"].toString());
                    a.legato = (bool) av["legato"];
                    a.roundRobins = juce::jmax (1, (int) av["roundRobins"]);
                    if (av.hasProperty ("keyswitch"))
                        a.keyswitch = av["keyswitch"].isString() ? parseNoteName (av["keyswitch"].toString()) : (int) av["keyswitch"];
                    a.dynamicsSource = dynamicsSourceFromString (av["dynamicsSource"].toString());
                    if (auto* rs = av["ranges"].getArray())
                        for (auto& rv : *rs)
                            a.ranges.add ({ (int) rv["lo"], (int) rv["hi"] });
                    if (auto* ds = av["dynamics"].getArray())
                        for (auto& d : *ds)
                            a.dynamics.add (d.toString());
                    if (a.dynamics.isEmpty())
                        a.dynamics.add ("mf");
                    if (auto* is = av["issues"].getArray())
                        for (auto& s : *is)
                            a.issues.add (s.toString());
                    inst.articulations.push_back (std::move (a));
                }
            }
            c.instruments.push_back (std::move (inst));
        }
    }

    if (c.instruments.empty())
        return juce::Result::fail ("Preset contains no instruments");
    out = std::move (c);
    return juce::Result::ok();
}

bool parseRangeText (const juce::String& text, juce::Array<NoteRange>& out)
{
    juce::Array<NoteRange> r;
    auto note = [] (const juce::String& t) {
        const auto s = t.trim();
        return s.containsOnly ("0123456789") && s.isNotEmpty() ? s.getIntValue() : parseNoteName (s);
    };
    for (auto part : juce::StringArray::fromTokens (text, ",;", ""))
    {
        part = part.trim();
        if (part.isEmpty())
            continue;
        // a single note ("C-1", "60"), else a range split at whichever '-' leaves two notes ("C2-C6", "C-1-C1", "36-96")
        int lo = note (part), hi = lo;
        if (lo < 0)
            for (int i = part.indexOf (1, "-"); i > 0; i = part.indexOf (i + 1, "-"))
            {
                const int a = note (part.substring (0, i)), b = note (part.substring (i + 1));
                if (a >= 0 && b >= 0)
                {
                    lo = a;
                    hi = b;
                    break;
                }
            }
        if (lo < 0 || hi < 0 || lo > 127 || hi > 127)
            return false;
        r.add ({ juce::jmin (lo, hi), juce::jmax (lo, hi) });
    }
    out = r;
    return true;
}

namespace
{
// a copy of the loaded object (fields Secant doesn't model stay, in their order), or a new one
juce::DynamicObject* objectFrom (const juce::var& loaded)
{
    if (auto* o = loaded.getDynamicObject())
        return o->clone().release();
    return new juce::DynamicObject();
}
} // namespace

juce::Result Collection::save (const juce::File& f) const
{
    auto* root = objectFrom (json);
    root->setProperty ("format", "secant.preset");
    root->setProperty ("version", 1);
    root->setProperty ("vendor", vendor);
    root->setProperty ("collection", name);
    root->setProperty ("player", player);
    root->setProperty ("noteConvention", "C3=60");
    root->setProperty ("source", source);
    root->setProperty ("keyswitchBase", keyswitchBase);
    juce::Array<juce::var> insts;
    for (auto& inst : instruments)
    {
        auto* io = objectFrom (inst.json);
        io->setProperty ("name", inst.name);
        if (inst.group.isNotEmpty())
            io->setProperty ("group", inst.group);
        else
            io->removeProperty ("group");
        juce::Array<juce::var> arts;
        for (auto& a : inst.articulations)
        {
            auto* ao = objectFrom (a.json);
            ao->setProperty ("index", a.index);
            ao->setProperty ("docNumber", a.docNumber);
            ao->setProperty ("name", a.name);
            ao->setProperty ("category", toString (a.category));
            ao->setProperty ("legato", a.legato);
            juce::Array<juce::var> ranges;
            for (auto& r : a.ranges)
            {
                auto* ro = new juce::DynamicObject();
                ro->setProperty ("from", noteName (r.lo));
                ro->setProperty ("to", noteName (r.hi));
                ro->setProperty ("lo", r.lo);
                ro->setProperty ("hi", r.hi);
                ranges.add (juce::var (ro));
            }
            ao->setProperty ("ranges", ranges);
            juce::Array<juce::var> dyn;
            for (auto& d : a.dynamics)
                dyn.add (d);
            ao->setProperty ("dynamics", dyn);
            ao->setProperty ("roundRobins", a.roundRobins);
            if (a.keyswitch >= 0)
                ao->setProperty ("keyswitch", noteName (a.keyswitch));
            else
                ao->removeProperty ("keyswitch");
            if (a.dynamicsSource != DynamicsSource::autoDetect)
                ao->setProperty ("dynamicsSource", toString (a.dynamicsSource));
            else
                ao->removeProperty ("dynamicsSource");
            juce::Array<juce::var> issues;
            for (auto& s : a.issues)
                issues.add (s);
            ao->setProperty ("issues", issues);
            arts.add (juce::var (ao));
        }
        io->setProperty ("articulations", arts);
        insts.add (juce::var (io));
    }
    root->setProperty ("instruments", insts);
    f.getParentDirectory().createDirectory();
    if (! f.replaceWithText (juce::JSON::toString (juce::var (root), false)))
        return juce::Result::fail ("Could not write " + f.getFullPathName());
    return juce::Result::ok();
}

} // namespace tp
