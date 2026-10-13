#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "Planning.h"
#include "RenderWorker.h"

// Workflow:
//  1. pick a VST3 instrument (any player; SINE Player gets extra support), load it, open its editor and load the
//     instrument you want to measure (pick one mic position to save RAM per worker)
//  2. load an instrument preset JSON (presets/berlin/*.json) and pick the matching instrument
//  3. set the keyswitch base, tick articulations, Start
//  The configured plugin state is cloned into N worker instances that render in parallel.
class MainComponent : public juce::Component,
                      private juce::TableListBoxModel,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // ---- table model
    int getNumRows() override;
    void paintRowBackground (juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell (juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    juce::Component* refreshComponentForCell (int row, int col, bool selected, juce::Component* existing) override;
    juce::String getCellTooltip (int row, int col) override;

    void timerCallback() override;

    // ---- actions
    void scanPlugins();
    void loadPlugin();
    void showEditor();
    void closeEditor();
    void choosePreset();
    void loadPreset (const juce::File&);
    void selectInstrument (int index);
    void startRun();
    void stopRun();
    void finishRun();
    void exportResults();
    void log (const juce::String&);
    void setAllRuns (bool enabled);
    // preset editing (in memory until saved)
    void newPreset();
    void savePreset();
    void showSaveMenu();
    // writes the loaded SINE instrument's keyswitches, layers, dyn src, RR, legato and playable range
    // into the selected instrument (in memory, logged per articulation); false if there is none
    bool applySineConfiguration (const juce::MemoryBlock& pluginState);
    void addInstrument();
    void removeInstrument();
    void addArticulation();
    void removeArticulations();
    void refreshInstrumentBox();
    void presetEdited();                 // marks the preset modified, refreshes the table
    void updatePresetLabel();
    tp::Articulation* articulationAt (int row);
    void updatePlayerMode();             // SINE Player vs any other player (Kontakt, ...)
    bool isSinePlayer() const;
    void updateRrResetBox (bool sinePlayer); // "As in SINE" is listed only with SINE Player
    bool presetModified = false;
    int keyswitchFor (int row) const;
    tp::RunSettings collectSettings() const;
    void updateEnablement();

    enum Col { colRun = 1, colIndex, colName, colLegato, colCategory, colRange, colDynamics, colRR, colRRFound, colKeyswitch, colDynSrc, colProgress, colMedian, colIssues };

    struct Row
    {
        bool enabled = true;
        int keyswitchOverride = -1;
        tp::Category category = tp::Category::shortNote;
        tp::DynamicsSource dynSource = tp::DynamicsSource::autoDetect;
        int planIdx = -1; // index into the running plan, -1 if not part of the run
    };

    // cell editors
    class RunToggle;
    class CategoryBox;
    class DynSrcBox;
    class KeyswitchLabel;
    class TextCell;
    class LegatoToggle;

    class PluginWindow;
    class MasterPump;

    // ---- plugin
    juce::AudioPluginFormatManager formatManager;
    juce::Array<juce::File> pluginFiles;
    juce::PluginDescription masterDesc;
    std::unique_ptr<juce::AudioPluginInstance> master;
    std::unique_ptr<MasterPump> masterPump; // after `master`: destroyed first
    std::unique_ptr<PluginWindow> editorWindow;
    void releaseMaster();

    // ---- preset
    tp::Collection collection;
    int instrumentIndex = -1;
    std::vector<Row> rows;

    // ---- run
    std::unique_ptr<tp::RunContext> ctx;
    tp::ResultStore store;
    std::vector<std::unique_ptr<tp::RenderWorker>> workers;
    // Loaded worker instances kept between runs: SINE can crash when many instances are torn
    // down and re-created at once, so identical setups reuse them instead of re-loading.
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> pool;
    juce::MemoryBlock poolState, runState;
    double poolRate = 0.0;
    int poolBlock = 0;
    void createNextWorker (int index, tp::RunSettings settings);
    void releasePoolThen (std::function<void()> next); // one instance at a time (SINE preloader safety)
    int awaitingWorker = -1;          // index of the worker that must sound before the next is created
    tp::RunSettings creationSettings;
    bool running = false, creatingWorkers = false, stopRequested = false;
    double progress = 0.0;
    double runStartedMs = 0.0;
    juce::File lastResultFile, runStateFile;
    std::unique_ptr<juce::FileOutputStream> logFile; // ~/Documents/Secant/Logs/latest.log
    double lastStatusLogMs = 0.0;

    // ---- widgets
    juce::GroupComponent pluginGroup { {}, "1. Plugin" }, presetGroup { {}, "2. Preset" }, settingsGroup { {}, "3. Settings" };
    juce::ComboBox pluginBox;
    juce::TextButton rescanButton { "Rescan" }, loadPluginButton { "Load" }, editorButton { "Open Editor" };
    juce::Label pluginStatus;

    juce::TextButton presetButton { juce::CharPointer_UTF8 ("Open…") }, newPresetButton { "New" }, savePresetButton { juce::CharPointer_UTF8 ("Save…") },
        addInstrumentButton { "+" }, removeInstrumentButton { "-" };
    juce::Label presetLabel;
    juce::ComboBox instrumentBox;

    juce::Label srLabel { {}, "Sample rate" }, workersLabel { {}, "Workers" }, ksLabel { {}, "KS base" },
        stepLabel { {}, "Notes" }, speedLabel { {}, "Speed" }, primaryLabel { {}, "Onset" };
    juce::Label primaryCatLabels[4] { { {}, "short" }, { {}, "long" }, { {}, "dynamic" }, { {}, "phrase" } };
    juce::ComboBox sampleRateBox, stepBox, speedBox;
    juce::Slider workersSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::TextEditor ksBaseEdit;
    juce::ToggleButton useKsToggle { "Send keyswitches" },
        allSoundOffToggle { "All Sound Off per hit" };
    juce::Label smoothLabel { {}, "Smoothing" }, maxHitsLabel { {}, "Max hits" };
    juce::ComboBox maxHitsBox, confirmBox;
    juce::Label confirmLabel { {}, "RR confirm" };
    juce::ComboBox smoothBox;
    juce::Label rrResetLabel { {}, "RR reset" };
    juce::ComboBox rrResetBox;
    juce::Label rrResetKeyLabel { {}, "RR key" };
    juce::TextEditor rrResetKeyEdit; // optional, players other than SINE
    juce::ComboBox primaryBoxes[4];
    juce::ToggleButton legatoToggle { "Legato" };
    juce::Label legatoIntervalsLabel { {}, "Intervals" }, legatoGapsLabel { {}, "Spacing" }, legatoPrimaryLabel { {}, "Point" };
    juce::ComboBox legatoIntervalsBox, legatoGapsBox, legatoPrimaryBox;

    juce::TableListBox table { "articulations", this };
    juce::TextButton allButton { "All" }, noneButton { "None" }, addArtButton { "+" }, removeArtButton { "-" };

    juce::TextButton startButton { "Start" }, stopButton { "Stop" }, exportButton { juce::CharPointer_UTF8 ("Export…") };
    juce::ProgressBar progressBar { progress };
    juce::Label etaLabel, nextStepLabel;
    juce::String missingStep() const; // empty when ready to run
    void refreshNextStep();
    juce::TextEditor logView;

    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltips { this, 400 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
