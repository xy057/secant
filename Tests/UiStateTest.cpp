// MainComponent's preset editing with an instrument selected, no plugin needed. The table reads its rows
// against the selected instrument's articulations, synchronously inside every updateContent(): an action
// that changes one before the other read out of bounds (opening a preset, New and "+ instrument" crashed).
// Built with libc++'s bounds checks where available, so an out-of-range index aborts instead of passing.
#include "../Source/MainComponent.h"
#include <cstdio>
#include <cstdlib>

struct MainComponentTest
{
    MainComponent& mc;

    // rows and the selected instrument's articulations always agree
    bool consistent() const
    {
        if (mc.instrumentIndex < 0)
            return mc.rows.empty();
        if (mc.instrumentIndex >= (int) mc.collection.instruments.size())
            return false;
        return mc.rows.size() == mc.collection.instruments[(size_t) mc.instrumentIndex].articulations.size();
    }

    // what the screen does next: refresh the cell components, paint every visible cell
    void refreshAndPaint()
    {
        mc.table.updateContent();
        juce::ignoreUnused (mc.table.createComponentSnapshot (mc.table.getLocalBounds()));
    }

    // every action starts from: preset open, its first instrument selected (rows shown in the table)
    static void runAll (const juce::File& preset);
};

namespace
{
int failures = 0;

void expect (bool ok, const juce::String& what)
{
    std::printf ("  %-60s %s\n", what.toRawUTF8(), ok ? "ok" : "FAIL");
    if (! ok)
        ++failures;
}

void setEnv (const char* name, const juce::String& value)
{
   #if JUCE_WINDOWS
    _putenv_s (name, value.toRawUTF8());
   #else
    setenv (name, value.toRawUTF8(), 1);
   #endif
}
} // namespace

void MainComponentTest::runAll (const juce::File& preset)
{
    {
        // the platform's VST3 folders, vendor subfolders included (informational: CI machines have none)
        MainComponent mc;
        std::printf ("  (VST3 plugins found: %d)\n", mc.pluginFiles.size());
    }
    const std::pair<const char*, std::function<void (MainComponent&)>> actions[] {
        { "open a preset again", [&] (MainComponent& m) { m.loadPreset (preset); } },
        { "New preset", [] (MainComponent& m) { m.newPreset(); } },
        { "+ instrument", [] (MainComponent& m) { m.addInstrument(); } },
        { "- instrument", [] (MainComponent& m) { m.removeInstrument(); } },
        { "choose another instrument", [] (MainComponent& m) { m.selectInstrument (1); } },
        { "+ articulation", [] (MainComponent& m) { m.addArticulation(); } },
        { "- articulation", [] (MainComponent& m) { m.table.selectRow (0); m.removeArticulations(); } },
    };
    for (auto& [name, action] : actions)
    {
        MainComponent mc;
        MainComponentTest t { mc };
        mc.loadPreset (preset);
        mc.selectInstrument (0);
        const bool before = ! mc.rows.empty() && t.consistent();
        action (mc);
        t.refreshAndPaint();
        expect (before && t.consistent(), juce::String (name) + ": rows match the instrument");
    }
}

int main()
{
    // logs and results go to a scratch folder, never to the user's ~/Documents/Secant
    const auto dataDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("SecantUiStateTest");
    setEnv ("SECANT_DATA_DIR", dataDir.getFullPathName());

    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File preset (juce::String (SECANT_SOURCE_DIR) + "/presets/berlin/berlin_brass.json");
    if (! preset.existsAsFile())
    {
        std::printf ("missing %s\n", preset.getFullPathName().toRawUTF8());
        return 1;
    }

    MainComponentTest::runAll (preset);

    dataDir.deleteRecursively();
    std::printf ("\n%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
