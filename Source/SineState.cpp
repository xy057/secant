#include "SineState.h"

namespace tp
{
namespace
{
    juce::String normalise (const juce::String& s)
    {
        juce::String out;
        for (auto c : s.toLowerCase())
            if (juce::CharacterFunctions::isLetterOrDigit (c))
                out << juce::String::charToString (c);
        return out;
    }

    int asInt (const juce::var& v, int def = 0) { return v.isVoid() ? def : v.toString().getIntValue(); }
    double asDouble (const juce::var& v, double def = 0) { return v.isVoid() ? def : v.toString().getDoubleValue(); }
    bool asBool (const juce::var& v) { return v.toString().equalsIgnoreCase ("true"); }

    // the JSON object starting at `from`, ignoring trailing binary data
    juce::String balancedObject (const char* data, size_t size, size_t from)
    {
        int depth = 0;
        bool inString = false, escaped = false;
        for (size_t i = from; i < size; ++i)
        {
            const char c = data[i];
            if (inString)
            {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') inString = false;
                continue;
            }
            if (c == '"') inString = true;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0)
                return juce::String::fromUTF8 (data + from, (int) (i + 1 - from));
        }
        return {};
    }

    // ranges from a SINE zone list: entries carry "fromValue"; each zone runs up to the next higher one
    struct Range { double lo, hi; }; // hi < 0 = unbounded
    std::vector<Range> rangesOf (const juce::var& list)
    {
        std::vector<Range> r;
        std::vector<double> froms;
        if (auto* arr = list.getArray())
            for (auto& z : *arr)
                froms.push_back (asDouble (z["fromValue"]));
        for (auto f : froms)
        {
            double hi = -1;
            for (auto g : froms)
                if (g > f && (hi < 0 || g < hi))
                    hi = g;
            r.push_back ({ f, hi });
        }
        return r;
    }
} // namespace

const SineArticulation* SineInstrument::byKeyswitch (int ks) const
{
    for (auto& a : articulations)
        if (a.keyswitch == ks)
            return &a;
    return nullptr;
}

const SineArticulation* SineInstrument::byTitle (const juce::String& t) const
{
    const auto n = normalise (t);
    for (auto& a : articulations)
        if (normalise (a.title) == n)
            return &a;
    return nullptr;
}

namespace
{
    // the plugin state's layers: JUCE's binary XML ("VC2!" + length + XML) holding the VST3 component
    // state in <IComponent> as JUCE base64; that component is SINE's chunk (VST2-style header + JSON)
    struct Container
    {
        std::unique_ptr<juce::XmlElement> xml; // null: the state was the component itself
        juce::MemoryBlock component;
    };

    Container unpack (const juce::MemoryBlock& state)
    {
        Container c;
        if (state.getSize() > 8 && std::memcmp (state.getData(), "VC2!", 4) == 0)
        {
            const auto len = juce::jmin ((size_t) juce::ByteOrder::littleEndianInt (static_cast<const char*> (state.getData()) + 4),
                                         state.getSize() - 8);
            c.xml = juce::parseXML (juce::String::fromUTF8 (static_cast<const char*> (state.getData()) + 8, (int) len));
        }
        else
            c.xml = juce::parseXML (state.toString());

        if (c.xml != nullptr)
        {
            if (auto* ic = c.xml->getChildByName ("IComponent"))
                c.component.fromBase64Encoding (ic->getAllSubText().trim());
        }
        else
            c.component = state;
        return c;
    }

    juce::MemoryBlock repack (const Container& c)
    {
        if (c.xml == nullptr)
            return c.component;
        auto xml = std::make_unique<juce::XmlElement> (*c.xml);
        if (auto* ic = xml->getChildByName ("IComponent"))
        {
            ic->deleteAllTextElements();
            ic->addTextElement (c.component.toBase64Encoding());
        }
        // as juce::AudioProcessor::copyXmlToBinary
        const auto text = xml->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader());
        const auto n = text.getNumBytesAsUTF8();
        juce::MemoryBlock out (n + 9, true);
        auto* d = static_cast<char*> (out.getData());
        std::memcpy (d, "VC2!", 4);
        const auto len32 = juce::ByteOrder::swapIfBigEndian ((juce::uint32) n);
        std::memcpy (d + 4, &len32, 4);
        text.copyToUTF8 (d + 8, n + 1);
        return out;
    }

    // offset of SINE's JSON inside the component, or -1
    int jsonStart (const juce::MemoryBlock& component)
    {
        const auto* data = static_cast<const char*> (component.getData());
        for (size_t i = 0; i < component.getSize(); ++i)
            if (data[i] == '{')
            {
                auto json = balancedObject (data, component.getSize(), i);
                return json.contains ("\"samplerVersion\"") || json.contains ("\"instruments\"") ? (int) i : -1;
            }
        return -1;
    }
} // namespace

juce::String extractSineJson (const juce::MemoryBlock& state)
{
    const auto c = unpack (state);
    const int at = jsonStart (c.component);
    if (at < 0)
        return {};
    return balancedObject (static_cast<const char*> (c.component.getData()), c.component.getSize(), (size_t) at);
}

juce::MemoryBlock withRoundRobinReset (const juce::MemoryBlock& state, double ms, juce::String* error)
{
    auto fail = [&] (const juce::String& why) {
        if (error != nullptr)
            *error = why;
        return juce::MemoryBlock();
    };
    auto c = unpack (state);
    const int at = jsonStart (c.component);
    if (at < 0)
        return fail ("not a SINE Player state");
    // Rewrite each "resetAfterMS": <number> in place, padded with spaces to the old number's width,
    // so the chunk keeps its exact size (its header carries byte counts). A value that does not fit
    // in the old width is refused rather than resizing the chunk.
    auto* data = static_cast<char*> (c.component.getData());
    const size_t size = c.component.getSize();
    const juce::String key ("\"resetAfterMS\"");
    const auto value = juce::String (juce::roundToInt (ms));
    int patched = 0;
    for (size_t i = (size_t) at; i + (size_t) key.length() < size; ++i)
    {
        if (std::memcmp (data + i, key.toRawUTF8(), (size_t) key.length()) != 0)
            continue;
        size_t p = i + (size_t) key.length();
        while (p < size && (data[p] == ' ' || data[p] == ':'))
            ++p;
        // SINE writes the number as a string ("4000"): pad with leading zeros inside the quotes; a bare
        // number is padded with trailing spaces
        const bool quoted = p < size && data[p] == '"';
        if (quoted)
            ++p;
        size_t e = p;
        while (e < size && (juce::CharacterFunctions::isDigit (data[e]) || data[e] == '.' || data[e] == '-' || (! quoted && data[e] == ' ')))
            ++e;
        const size_t width = e - p;
        if (width == 0 || (quoted && (e >= size || data[e] != '"')))
            continue;
        if ((size_t) value.length() > width)
            return fail ("resetAfterMS " + juce::String (data + p, width) + " has no room for " + value);
        const auto text = quoted ? value.paddedLeft ('0', (int) width) : value.paddedRight (' ', (int) width);
        std::memcpy (data + p, text.toRawUTF8(), width);
        ++patched;
        i = e;
    }
    if (patched == 0)
        return fail ("no resetAfterMS in the state");
    return repack (c);
}

SineState parseSineState (const juce::MemoryBlock& pluginState)
{
    SineState st;
    const auto json = extractSineJson (pluginState);
    if (json.isEmpty())
    {
        st.error = "not a SINE Player state";
        return st;
    }
    juce::var root;
    if (auto r = juce::JSON::parse (json, root); r.failed())
    {
        st.error = "SINE state JSON: " + r.getErrorMessage();
        return st;
    }
    st.samplerVersion = root["samplerVersion"].toString();
    st.selectedInstrument = asInt (root["selectedEntry"]["instrumentIndex"]);

    if (auto* insts = root["instruments"].getArray())
        for (auto& iv : *insts)
        {
            SineInstrument inst;
            inst.title = iv["title"].toString();
            inst.ksShift = asInt (iv["ksshift"]);
            if (auto* mics = iv["micPositions"].getArray())
                for (auto& m : *mics)
                    if (asBool (m["enabled"]) && m["adaptiveLegato"].isObject())
                    {
                        const auto& al = m["adaptiveLegato"];
                        inst.hasAdaptive = true;
                        inst.adaptiveAmplitudeLoMs = asDouble (al["amplitudeLoTime"]);
                        inst.adaptiveAmplitudeHiMs = asDouble (al["amplitudeHiTime"]);
                        inst.adaptiveOffsetLoMs = asDouble (al["offsetLoTime"]);
                        inst.adaptiveOffsetHiMs = asDouble (al["offsetHiTime"]);
                        inst.adaptiveTransitionMinMs = asDouble (al["transitionMinTime"]);
                        break;
                    }

            std::function<void (const juce::var&)> addArticulations = [&] (const juce::var& list) {
                if (auto* arts = list.getArray())
                    for (auto& av : *arts)
                    {
                        // poly / combined entries nest their own articulation lists
                        if (av["articulations"].isArray())
                            addArticulations (av["articulations"]);
                        if (! av["mainOptions"].isObject())
                            continue;
                        SineArticulation a;
                        a.id = av["id"].toString();
                        a.title = av["title"].toString().trim();
                        a.enabled = ! av["enabled"].isVoid() ? asBool (av["enabled"]) : true;
                        const auto& mo = av["mainOptions"];
                        a.keyswitch = mo["switchValue"].isVoid() ? -1 : asInt (mo["switchValue"]) + inst.ksShift;
                        a.modwheelDynamics = asBool (mo["modwheelControl"]);
                        a.keyLow = asInt (av["keyRangeLow"], -1);
                        a.keyHigh = asInt (av["keyRangeHigh"], -1);
                        const auto& rr = av["roundRobinOptions"];
                        int enabledRR = 0;
                        if (auto* slots = rr["roundRobins"].getArray())
                            for (auto& s : *slots)
                                enabledRR += asBool (s["enabled"]) ? 1 : 0;
                        a.roundRobins = juce::jmax (1, enabledRR);
                        a.rrResetMs = asDouble (rr["resetAfterMS"]);
                        a.randomRR = asBool (rr["randomRoundRobin"]);
                        if (auto* layers = av["dynamicsOptions"]["dynamicLayers"].getArray())
                            for (auto& l : *layers)
                                if (l["enabled"].isVoid() || asBool (l["enabled"]))
                                    a.dynamics.add (l["title"].toString());

                        const auto& lo = av["legatoOptions"];
                        a.legato = asBool (lo["legato"]);
                        if (auto* types = lo["zoneTypeFeatures"].getArray())
                            for (auto& t : *types)
                                a.legatoTypes.push_back ({ asInt (t["id"]), t["title"].toString().trim() });
                        const auto vz = rangesOf (lo["velocityZones"]);
                        const auto sz = rangesOf (lo["speedZones"]);
                        if (auto* zones = lo["zones"].getArray())
                            for (auto& z : *zones)
                            {
                                const int vi = asInt (z["velocityZone"]) - 1, si = asInt (z["speedZone"]) - 1;
                                if (vi < 0 || vi >= (int) vz.size() || si < 0 || si >= (int) sz.size())
                                    continue;
                                SineLegatoZone zone;
                                zone.velocityLo = (int) vz[(size_t) vi].lo;
                                zone.velocityHi = vz[(size_t) vi].hi < 0 ? 127 : (int) vz[(size_t) vi].hi - 1;
                                zone.speedLoMs = sz[(size_t) si].lo;
                                zone.speedHiMs = sz[(size_t) si].hi;
                                zone.typeId = asInt (z["zoneTypeId"]);
                                for (auto& t : a.legatoTypes)
                                    if (t.id == zone.typeId)
                                        zone.type = t.title;
                                if (zone.type.isEmpty())
                                    zone.type = "Legato type " + juce::String (zone.typeId);
                                a.legatoZones.push_back (zone);
                            }
                        inst.articulations.push_back (std::move (a));
                    }
            };
            addArticulations (iv["articulations"]);
            st.instruments.push_back (std::move (inst));
        }
    st.valid = ! st.instruments.empty();
    if (! st.valid)
        st.error = "SINE state has no instruments";
    return st;
}

} // namespace tp
