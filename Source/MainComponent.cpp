#include "MainComponent.h"
#include "DelayExport.h"

using namespace tp;

namespace
{
const juce::StringArray kCategories { "short", "long", "dynamic", "phrase" };
const Category kCategoryOrder[] { Category::shortNote, Category::longNote, Category::dynamic, Category::phrase };

juce::File defaultPresetDir()
{
    // look next to the app bundle / up the tree for presets/ (dev builds), else ~/Documents
    auto f = juce::File::getSpecialLocation (juce::File::currentApplicationFile);
    for (int i = 0; i < 8 && f.exists(); ++i, f = f.getParentDirectory())
        if (f.getChildFile ("presets").isDirectory())
            return f.getChildFile ("presets");
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
}

// ~/Documents/Secant (Results, Logs), or $SECANT_DATA_DIR (tests). The folder of the app's former name is moved here once.
juce::File dataDir()
{
    if (const auto custom = juce::SystemStats::getEnvironmentVariable ("SECANT_DATA_DIR", {}); juce::File::isAbsolutePath (custom))
        return juce::File (custom);
    const auto docs = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    auto dir = docs.getChildFile ("Secant");
    if (auto old = docs.getChildFile ("TransientProbe"); old.isDirectory() && ! dir.exists())
        old.moveFileTo (dir);
    return dir;
}

juce::File resultsDir()
{
    return dataDir().getChildFile ("Results");
}

// UI text with typographic characters (…)
juce::String utf8 (const char* s)
{
    return juce::String (juce::CharPointer_UTF8 (s));
}
} // namespace

// ============================================================================ cell editors

class MainComponent::RunToggle : public juce::ToggleButton
{
public:
    explicit RunToggle (MainComponent& o) : owner (o)
    {
        onClick = [this] {
            if (row >= 0 && row < (int) owner.rows.size())
                owner.rows[(size_t) row].enabled = getToggleState();
        };
    }
    void setRow (int r)
    {
        row = r;
        setToggleState (owner.rows[(size_t) r].enabled, juce::dontSendNotification);
        setEnabled (! owner.running);
    }

private:
    MainComponent& owner;
    int row = -1;
};

class MainComponent::CategoryBox : public juce::ComboBox
{
public:
    explicit CategoryBox (MainComponent& o) : owner (o)
    {
        addItemList (kCategories, 1);
        onChange = [this] {
            if (row >= 0 && getSelectedItemIndex() >= 0)
            {
                owner.rows[(size_t) row].category = kCategoryOrder[getSelectedItemIndex()];
                if (auto* a = owner.articulationAt (row); a != nullptr && a->category != kCategoryOrder[getSelectedItemIndex()])
                {
                    a->category = kCategoryOrder[getSelectedItemIndex()];
                    owner.presetModified = true;
                    owner.updatePresetLabel();
                }
            }
        };
    }
    void setRow (int r)
    {
        row = r;
        setSelectedItemIndex (kCategories.indexOf (toString (owner.rows[(size_t) r].category)), juce::dontSendNotification);
        setEnabled (! owner.running);
    }

private:
    MainComponent& owner;
    int row = -1;
};

class MainComponent::DynSrcBox : public juce::ComboBox
{
public:
    explicit DynSrcBox (MainComponent& o) : owner (o)
    {
        addItem ("Auto", 3);
        addItem ("Velocity", 1);
        addItem ("CC1", 2);
        setTooltip ("How this articulation switches dynamic layers.\n"
                    "Auto: detect it.\nVelocity: send layers as velocity.\nCC1: send layers as mod wheel.");
        onChange = [this] {
            if (row < 0)
                return;
            const auto src = getSelectedId() == 2 ? DynamicsSource::cc1 : getSelectedId() == 1 ? DynamicsSource::velocity : DynamicsSource::autoDetect;
            owner.rows[(size_t) row].dynSource = src;
            if (auto* a = owner.articulationAt (row); a != nullptr && a->dynamicsSource != src)
            {
                a->dynamicsSource = src; // saved with the preset
                owner.presetModified = true;
                owner.updatePresetLabel();
            }
        };
    }
    void setRow (int r)
    {
        row = r;
        const auto src = owner.rows[(size_t) r].dynSource;
        setSelectedId (src == DynamicsSource::cc1 ? 2 : src == DynamicsSource::velocity ? 1 : 3, juce::dontSendNotification);
        setEnabled (! owner.running);
    }

private:
    MainComponent& owner;
    int row = -1;
};

class MainComponent::KeyswitchLabel : public juce::Label
{
public:
    explicit KeyswitchLabel (MainComponent& o) : owner (o)
    {
        setEditable (false, true, false);
        setTooltip ("Double-click to set a note (C-1) or MIDI number. Clear to use the keyswitch base.");
        onTextChange = [this] {
            if (row < 0)
                return;
            auto t = getText().trim();
            owner.rows[(size_t) row].keyswitchOverride = t.isEmpty() ? -1 : parseNoteName (t);
            if (auto* a = owner.articulationAt (row))
            {
                a->keyswitch = owner.rows[(size_t) row].keyswitchOverride; // saved with the preset
                owner.presetModified = true;
                owner.updatePresetLabel();
            }
            refresh();
        };
    }
    void setRow (int r)
    {
        row = r;
        refresh();
        setEnabled (! owner.running);
    }

private:
    void refresh()
    {
        const int ks = owner.keyswitchFor (row);
        const bool overridden = owner.rows[(size_t) row].keyswitchOverride >= 0;
        setText (ks < 0 ? juce::String ("-") : noteName (ks) + " (" + juce::String (ks) + ")" + (overridden ? " *" : ""),
                 juce::dontSendNotification);
        setColour (textColourId, overridden ? juce::Colours::orange : juce::Colours::white);
    }

    MainComponent& owner;
    int row = -1;
};

// double-click to edit a preset field (name, range, dynamics, RR); invalid input is refused
class MainComponent::TextCell : public juce::Label
{
public:
    explicit TextCell (MainComponent& o) : owner (o)
    {
        setEditable (false, true, false);
        setFont (juce::FontOptions (13.0f));
        onTextChange = [this] {
            auto* a = owner.articulationAt (row);
            if (a == nullptr)
                return;
            const auto t = getText().trim();
            bool ok = true;
            switch (col)
            {
                case colName: ok = t.isNotEmpty(); if (ok) a->name = t; break;
                case colRange:
                {
                    juce::Array<NoteRange> r;
                    ok = parseRangeText (t, r);
                    if (ok)
                    {
                        a->ranges = r;
                        owner.rows[(size_t) row].enabled = ! r.isEmpty();
                    }
                    break;
                }
                case colDynamics:
                {
                    auto d = juce::StringArray::fromTokens (t, " ,;", "");
                    d.removeEmptyStrings();
                    ok = ! d.isEmpty();
                    if (ok)
                        a->dynamics = d;
                    break;
                }
                case colRR: ok = t.containsOnly ("0123456789") && t.getIntValue() >= 1; if (ok) a->roundRobins = t.getIntValue(); break;
                default: break;
            }
            if (! ok)
                owner.log ("Invalid value '" + t + "'. " + juce::String (col == colRange ? "Use ranges like C2-C6, D7 or 36-96."
                                                                          : col == colRR ? "Enter 1 or more." : "Enter a value."));
            else
                owner.presetEdited();
            refresh();
        };
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        owner.table.selectRowsBasedOnModifierKeys (row, e.mods, false);
        juce::Label::mouseDown (e);
    }
    void setRowCol (int r, int c)
    {
        row = r;
        col = c;
        refresh();
        setEnabled (! owner.running);
        setTooltip (col == colRange ? "Double-click to edit, e.g. C2-C6, D7 (C3 = 60)"
                    : col == colDynamics ? "Double-click to edit, e.g. pp mf ff.\nNumbers (0-127, e.g. 20 80 127) are sent as that velocity / CC1 value."
                    : col == colRR ? "Double-click to edit round robins per note"
                                   : "Double-click to rename");
    }

private:
    void refresh()
    {
        auto* a = owner.articulationAt (row);
        if (a == nullptr)
            return;
        setText (col == colName ? a->name : col == colRange ? a->rangeText() : col == colDynamics ? a->dynamics.joinIntoString (" ")
                                                                                                  : juce::String (a->roundRobins),
                 juce::dontSendNotification);
    }

    MainComponent& owner;
    int row = -1, col = 0;
};

class MainComponent::LegatoToggle : public juce::ToggleButton
{
public:
    explicit LegatoToggle (MainComponent& o) : owner (o)
    {
        setTooltip ("Legato articulation. Transitions are measured when Legato is on.");
        onClick = [this] {
            if (auto* a = owner.articulationAt (row); a != nullptr && a->legato != getToggleState())
            {
                a->legato = getToggleState();
                owner.presetModified = true;
                owner.updatePresetLabel();
            }
        };
    }
    void setRow (int r)
    {
        row = r;
        if (auto* a = owner.articulationAt (r))
            setToggleState (a->legato, juce::dontSendNotification);
        setEnabled (! owner.running);
    }

private:
    MainComponent& owner;
    int row = -1;
};

class MainComponent::PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioProcessorEditor* editor, std::function<void()> onClose)
        : DocumentWindow (editor->getAudioProcessor()->getName(), juce::Colours::black, DocumentWindow::closeButton),
          closeCallback (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { closeCallback(); }

private:
    std::function<void()> closeCallback;
};

// The instance the user configures is never part of a run, but a plugin that never gets an audio callback
// looks unconnected: Kontakt instruments whose scripts initialise from audio-time callbacks then stay at
// "Instrument not initialised". Like a DAW with an idle track, this feeds it silent blocks in real time
// (stopped transport at 120 bpm) for as long as it is loaded. Its output is discarded.
class MainComponent::MasterPump : public juce::Thread
{
public:
    MasterPump (juce::AudioPluginInstance& p, double rate, int block)
        : juce::Thread ("MasterPump"), plugin (p), sampleRate (rate), blockSize (block)
    {
        playHead.sampleRate = rate;
        plugin.setPlayHead (&playHead);
        startThread();
    }
    ~MasterPump() override
    {
        stopThread (2000);
        plugin.setPlayHead (nullptr);
    }

    void run() override
    {
        juce::AudioBuffer<float> buffer (juce::jmax (1, plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels()), blockSize);
        juce::MidiBuffer midi;
        const double blockMs = blockSize * 1000.0 / sampleRate;
        double next = juce::Time::getMillisecondCounterHiRes();
        while (! threadShouldExit())
        {
            buffer.clear();
            midi.clear();
            plugin.processBlock (buffer, midi);
            playHead.samples += blockSize;
            next += blockMs;
            const double ahead = next - juce::Time::getMillisecondCounterHiRes();
            if (ahead > 1.0)
                wait ((int) ahead);
            else if (ahead < -500.0)
                next = juce::Time::getMillisecondCounterHiRes(); // fell behind (system busy): don't burst to catch up
        }
    }

private:
    struct PlayHead : juce::AudioPlayHead
    {
        std::atomic<int64_t> samples { 0 };
        double sampleRate = 48000.0;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setBpm (120.0);
            info.setTimeSignature (TimeSignature { 4, 4 });
            info.setIsPlaying (false);
            info.setTimeInSamples (samples.load());
            info.setTimeInSeconds ((double) samples.load() / sampleRate);
            return info;
        }
    };

    juce::AudioPluginInstance& plugin;
    const double sampleRate;
    const int blockSize;
    PlayHead playHead;
};

// ============================================================================ construction

MainComponent::MainComponent()
{
    formatManager.addFormat (new juce::VST3PluginFormat());

    for (auto* g : { &pluginGroup, &presetGroup, &settingsGroup })
        addAndMakeVisible (g);

    // --- plugin row
    addAndMakeVisible (pluginBox);
    addAndMakeVisible (rescanButton);
    addAndMakeVisible (loadPluginButton);
    addAndMakeVisible (editorButton);
    addAndMakeVisible (pluginStatus);
    pluginStatus.setText ("No plugin loaded", juce::dontSendNotification);
    rescanButton.onClick = [this] { scanPlugins(); };
    loadPluginButton.onClick = [this] { loadPlugin(); };
    editorButton.onClick = [this] { showEditor(); };

    // --- preset row
    addAndMakeVisible (presetButton);
    addAndMakeVisible (presetLabel);
    addAndMakeVisible (instrumentBox);
    presetLabel.setText ("No preset", juce::dontSendNotification);
    presetButton.onClick = [this] { choosePreset(); };
    presetButton.setTooltip ("Open a preset file");
    for (auto* b : { &newPresetButton, &savePresetButton, &addInstrumentButton, &removeInstrumentButton })
        addAndMakeVisible (b);
    newPresetButton.setTooltip ("Create an empty preset");
    savePresetButton.setTooltip ("Save the preset with your edits");
    addInstrumentButton.setTooltip ("Add an instrument");
    removeInstrumentButton.setTooltip ("Remove the selected instrument");
    newPresetButton.onClick = [this] { newPreset(); };
    savePresetButton.onClick = [this] { showSaveMenu(); };
    addInstrumentButton.onClick = [this] { addInstrument(); };
    removeInstrumentButton.onClick = [this] { removeInstrument(); };
    // typing a name renames the selected instrument
    instrumentBox.setEditableText (true);
    instrumentBox.setTextWhenNothingSelected ("Choose the loaded instrument");
    instrumentBox.onChange = [this] {
        if (instrumentBox.getSelectedId() > 0)
        {
            if (instrumentBox.getSelectedItemIndex() != instrumentIndex)
                selectInstrument (instrumentBox.getSelectedItemIndex());
            return;
        }
        const auto t = instrumentBox.getText().trim();
        if (instrumentIndex >= 0 && t.isNotEmpty() && t != collection.instruments[(size_t) instrumentIndex].displayName())
        {
            auto& inst = collection.instruments[(size_t) instrumentIndex];
            inst.name = t;
            inst.group = {};
            presetModified = true;
            refreshInstrumentBox();
        }
    };

    // --- settings
    primaryLabel.setTooltip ("Onset method per category. All methods are exported.");
    ksLabel.setTooltip ("Keyswitch of articulation #1 (C3 = 60)");
    for (auto& l : primaryCatLabels)
    {
        l.setFont (juce::FontOptions (12.0f));
        l.setJustificationType (juce::Justification::centredRight);
        l.setColour (juce::Label::textColourId, juce::Colours::grey);
        addAndMakeVisible (l);
    }
    for (auto* l : { &srLabel, &workersLabel, &ksLabel, &stepLabel, &speedLabel, &primaryLabel })
    {
        addAndMakeVisible (l);
        l->setFont (juce::FontOptions (13.0f));
    }
    sampleRateBox.addItemList ({ "44100", "48000", "88200", "96000" }, 1);
    sampleRateBox.setSelectedId (2);
    addAndMakeVisible (sampleRateBox);

    workersSlider.setRange (1, juce::jmax (1, juce::SystemStats::getNumCpus()), 1);
    workersSlider.setValue (juce::jlimit (1, 8, juce::SystemStats::getNumCpus() / 2));
    workersSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 40, 24);
    workersSlider.setTooltip ("Plugin instances running in parallel. Each loads its own samples.");
    addAndMakeVisible (workersSlider);

    ksBaseEdit.setText ("C-2");
    ksBaseEdit.setTooltip ("Keyswitch of articulation #1. Articulation n uses base + n - 1. "
                           "Double-click a KS cell to override one.");
    ksBaseEdit.onTextChange = [this] { table.updateContent(); table.repaint(); };
    addAndMakeVisible (ksBaseEdit);
    useKsToggle.setToggleState (true, juce::dontSendNotification);
    useKsToggle.onClick = [this] { table.updateContent(); table.repaint(); };
    addAndMakeVisible (useKsToggle);

    stepBox.addItem ("Every note", 1);
    stepBox.addItem ("Every 2nd note", 2);
    stepBox.addItem ("Every 3rd note", 3);
    stepBox.addItem ("Every 6th note", 6);
    stepBox.addItem ("Every 12th note", 12);
    stepBox.setSelectedId (1);
    addAndMakeVisible (stepBox);

    speedBox.addItem ("Offline (max)", 1);
    speedBox.addItem ("8x realtime", 2);
    speedBox.addItem ("4x realtime", 3);
    speedBox.addItem ("2x realtime", 4);
    speedBox.addItem ("Realtime", 5);
    speedBox.setSelectedId (1);
    speedBox.setTooltip ("Slow down rendering if the plugin can't stream samples fast enough");
    addAndMakeVisible (speedBox);

    confirmLabel.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (confirmLabel);
    confirmBox.addItem ("Off", 1);
    confirmBox.addItem ("+1 hit", 2);
    confirmBox.addItem ("+2 hits", 3);
    confirmBox.addItem ("+4 hits", 5);
    confirmBox.setSelectedId (3);
    confirmBox.setTooltip ("Extra hits that must match after the round-robin cycle repeats. Catches random round robins.");
    addAndMakeVisible (confirmBox);
    rrResetLabel.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (rrResetLabel);
    // items + tooltip depend on the loaded player ("As in SINE" only with SINE Player): see updateRrResetBox()
    rrResetKeyLabel.setFont (juce::FontOptions (13.0f));
    rrResetKeyLabel.setTooltip ("Optional: key that resets the player's round robins");
    addChildComponent (rrResetKeyLabel);
    rrResetKeyEdit.setTooltip ("Optional: key that resets the player's round robins (C3 = 60), sent before every series. "
                               "Leave empty if the player has none.");
    rrResetKeyEdit.setTextToShowWhenEmpty ("none", juce::Colours::grey);
    rrResetKeyEdit.onTextChange = [this] { refreshNextStep(); };
    addChildComponent (rrResetKeyEdit);
    updateRrResetBox (false);
    addAndMakeVisible (rrResetBox);
    maxHitsLabel.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (maxHitsLabel);
    for (int n : { 16, 32, 48, 64, 128 })
        maxHitsBox.addItem (juce::String (n), n);
    maxHitsBox.setSelectedId (48);
    maxHitsBox.setTooltip ("Hit limit per note and layer, for instruments that never repeat");
    addAndMakeVisible (maxHitsBox);
    allSoundOffToggle.setToggleState (true, juce::dontSendNotification);
    addAndMakeVisible (allSoundOffToggle);

    smoothLabel.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (smoothLabel);
    smoothBox.addItem ("Off", 1);
    smoothBox.addItem ("1/4 period", 2);
    smoothBox.addItem ("1/2 period", 3);
    smoothBox.addItem ("1 period", 4);
    smoothBox.setSelectedId (3);
    smoothBox.setTooltip ("Envelope smoothing, in periods of the note's pitch. More is steadier but blurs low notes.");
    addAndMakeVisible (smoothBox);

    const RunSettings defaults;
    for (int i = 0; i < 4; ++i)
    {
        primaryBoxes[i].addItemList (onsetMethodKeys(), 1);
        primaryBoxes[i].setSelectedItemIndex (onsetMethodKeys().indexOf (defaults.timing.at (kCategoryOrder[i]).primaryMethod));
        primaryBoxes[i].setTooltip ("Onset method for " + kCategories[i] + " articulations");
        addAndMakeVisible (primaryBoxes[i]);
    }

    // --- legato
    legatoToggle.setToggleState (defaults.measureLegato, juce::dontSendNotification);
    legatoToggle.setTooltip ("Measure transition times of legato articulations");
    addAndMakeVisible (legatoToggle);
    for (auto* l : { &legatoIntervalsLabel, &legatoGapsLabel, &legatoPrimaryLabel })
    {
        l->setFont (juce::FontOptions (13.0f));
        addAndMakeVisible (l);
    }
    legatoIntervalsBox.addItem ("Up to an octave", 12);
    legatoIntervalsBox.addItem ("Up to a fifth", 7);
    legatoIntervalsBox.addItem ("Up to a third", 4);
    legatoIntervalsBox.addItem ("Seconds only", 2);
    legatoIntervalsBox.addItem ("Octave + unison", 13);
    legatoIntervalsBox.setSelectedId (12);
    legatoIntervalsBox.setTooltip ("Intervals measured up and down from each note");
    addAndMakeVisible (legatoIntervalsBox);
    legatoGapsBox.addItem ("Standard 100-1000 ms", 1);
    legatoGapsBox.addItem ("Sweep 60-1500 ms", 2);
    legatoGapsBox.addItem ("Slow 1000 ms", 3);
    legatoGapsBox.setSelectedId (1);
    legatoGapsBox.setTooltip ("Time between notes. Some players change the legato type with playing speed.");
    addAndMakeVisible (legatoGapsBox);
    legatoPrimaryBox.addItem ("Arrival", 1);
    legatoPrimaryBox.addItem ("Midpoint", 2);
    legatoPrimaryBox.addItem ("Start", 3);
    legatoPrimaryBox.setSelectedId (1);
    legatoPrimaryBox.setTooltip ("Transition point used for the delay. All are exported.\n"
                                 "Arrival: target pitch reached\nMidpoint: halfway through the glide\nStart: first pitch ends");
    addAndMakeVisible (legatoPrimaryBox);

    // --- table
    auto& h = table.getHeader();
    // narrow defaults; the header stretches them to the table's width (wide windows) or shrinks them
    // down to their minimums (narrow ones). Doc issues is a tooltip-sized column - hover a row for the full text.
    h.addColumn ("Run", colRun, 36, 36, 36, juce::TableHeaderComponent::notResizable);
    h.addColumn ("#", colIndex, 28, 24);
    h.addColumn ("Articulation", colName, 150, 100);
    h.addColumn ("Leg", colLegato, 34, 30, 34, juce::TableHeaderComponent::notResizable);
    h.addColumn ("Category", colCategory, 86, 80);
    h.addColumn ("Range", colRange, 90, 60);
    h.addColumn ("Dynamics", colDynamics, 96, 56);
    h.addColumn ("RR", colRR, 40, 32);
    h.addColumn ("Found", colRRFound, 48, 40);
    h.addColumn ("KS", colKeyswitch, 64, 50);
    h.addColumn ("Dyn src", colDynSrc, 84, 78);
    h.addColumn ("Progress", colProgress, 80, 56);
    h.addColumn ("Median", colMedian, 62, 48);
    h.addColumn ("Doc issues", colIssues, 110, 40);
    h.setStretchToFitActive (true);
    table.setRowHeight (24);
    table.setColour (juce::ListBox::outlineColourId, juce::Colours::grey);
    table.setOutlineThickness (1);
    addAndMakeVisible (table);
    addAndMakeVisible (allButton);
    addAndMakeVisible (noneButton);
    table.setMultipleSelectionEnabled (true);
    addAndMakeVisible (addArtButton);
    addAndMakeVisible (removeArtButton);
    addArtButton.setTooltip ("Add an articulation");
    removeArtButton.setTooltip ("Remove the selected articulations");
    addArtButton.onClick = [this] { addArticulation(); };
    removeArtButton.onClick = [this] { removeArticulations(); };
    allButton.onClick = [this] { setAllRuns (true); };
    noneButton.onClick = [this] { setAllRuns (false); };

    // --- run controls
    addAndMakeVisible (startButton);
    addAndMakeVisible (stopButton);
    addAndMakeVisible (exportButton);
    addAndMakeVisible (progressBar);
    addAndMakeVisible (etaLabel);
    nextStepLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    addAndMakeVisible (nextStepLabel);
    startButton.onClick = [this] { startRun(); };
    stopButton.onClick = [this] { stopRun(); };
    exportButton.onClick = [this] { exportResults(); };

    logView.setMultiLine (true);
    logView.setReadOnly (true);
    logView.setScrollbarsShown (true);
    logView.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    addAndMakeVisible (logView);

    scanPlugins();
    updateEnablement();
    setSize (980, 860);
    startTimerHz (10);

    log ("Load the plugin and your instrument (one mic position). Open a preset, choose the same instrument, then click Start.");
}

MainComponent::~MainComponent()
{
    stopTimer();
    for (auto& w : workers)
        w->signalThreadShouldExit();
    workers.clear();
    pool.clear();
    closeEditor();
    releaseMaster();
}

void MainComponent::releaseMaster()
{
    masterPump = nullptr; // no audio callback may be running when the instance goes
    master = nullptr;
}

// ============================================================================ layout

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (10);
    constexpr int rowH = 26, gap = 6, groupTop = 20, groupPad = 10;

    // 1 + 2: plugin and preset, one line each (side by side when the window is wide)
    {
        const bool sideBySide = area.getWidth() >= 1200;
        auto rowArea = area.removeFromTop (groupTop + rowH + groupPad);
        auto pa = sideBySide ? rowArea.removeFromLeft (rowArea.getWidth() / 2 - 5) : rowArea;
        if (! sideBySide)
        {
            area.removeFromTop (gap);
            rowArea = area.removeFromTop (groupTop + rowH + groupPad);
        }
        else
            rowArea.removeFromLeft (10);

        pluginGroup.setBounds (pa);
        auto line = pa.reduced (groupPad, 0).withTrimmedTop (groupTop).withHeight (rowH);
        editorButton.setBounds (line.removeFromRight (96));
        line.removeFromRight (4);
        loadPluginButton.setBounds (line.removeFromRight (52));
        line.removeFromRight (4);
        rescanButton.setBounds (line.removeFromRight (64));
        line.removeFromRight (4);
        pluginBox.setBounds (line);
        pluginStatus.setBounds (pa.getX() + 10, pa.getBottom() - 4, pa.getWidth() - 20, 0); // collapsed; status shown in group text

        presetGroup.setBounds (rowArea);
        line = rowArea.reduced (groupPad, 0).withTrimmedTop (groupTop).withHeight (rowH);
        presetButton.setBounds (line.removeFromLeft (64));
        line.removeFromLeft (4);
        newPresetButton.setBounds (line.removeFromLeft (48));
        line.removeFromLeft (4);
        savePresetButton.setBounds (line.removeFromLeft (60));
        line.removeFromLeft (8);
        instrumentBox.setBounds (line.removeFromLeft (juce::jmin (280, line.getWidth() / 2)));
        line.removeFromLeft (2);
        addInstrumentButton.setBounds (line.removeFromLeft (26));
        removeInstrumentButton.setBounds (line.removeFromLeft (26));
        line.removeFromLeft (8);
        presetLabel.setBounds (line);
    }
    area.removeFromTop (gap);

    // 3: settings - label + control pairs flowing left to right, wrapping at the group's width;
    // `newRow` starts a line (keeps related controls together)
    {
        struct Item
        {
            juce::Component* label;
            int lw;
            juce::Component* c;
            int cw;
            bool newRow = false;
        };
        juce::Component* none = nullptr;
        std::vector<Item> items {
            { &srLabel, 74, &sampleRateBox, 76 },
            { &workersLabel, 56, &workersSlider, 100 },
            { &ksLabel, 54, &ksBaseEdit, 48 },
            { none, 0, &useKsToggle, 140 },
            { &stepLabel, 42, &stepBox, 128 },
            { &speedLabel, 44, &speedBox, 116 },
            { &confirmLabel, 70, &confirmBox, 104 },
            { &maxHitsLabel, 58, &maxHitsBox, 60 },
            { &rrResetLabel, 58, &rrResetBox, 100 },
            { &rrResetKeyLabel, 50, &rrResetKeyEdit, 48 },
            { &smoothLabel, 68, &smoothBox, 110 },
            { none, 0, &allSoundOffToggle, 160 },
            { &primaryLabel, 46, &primaryCatLabels[0], 40, true },
            { none, 0, &primaryBoxes[0], 100 },
            { &primaryCatLabels[1], 34, &primaryBoxes[1], 100 },
            { &primaryCatLabels[2], 54, &primaryBoxes[2], 100 },
            { &primaryCatLabels[3], 46, &primaryBoxes[3], 100 },
            { none, 0, &legatoToggle, 76, true },
            { &legatoIntervalsLabel, 56, &legatoIntervalsBox, 130 },
            { &legatoGapsLabel, 52, &legatoGapsBox, 170 },
            { &legatoPrimaryLabel, 38, &legatoPrimaryBox, 96 },
        };
        const int innerW = area.getWidth() - 2 * groupPad;
        auto layout = [&] (int x0, int y0, bool apply) {
            int x = 0, y = 0;
            for (auto& it : items)
            {
                if (! it.c->isVisible())
                    continue;
                const int w = it.lw + it.cw;
                if (x > 0 && (it.newRow || x + w > innerW))
                {
                    x = 0;
                    y += rowH + gap;
                }
                if (apply)
                {
                    if (it.label != nullptr)
                        it.label->setBounds (x0 + x, y0 + y, it.lw, rowH);
                    it.c->setBounds (x0 + x + it.lw, y0 + y, it.cw, rowH);
                }
                // the category label + its box stay together; other pairs get a gap
                x += w + (it.c == &primaryCatLabels[0] ? 0 : 10);
            }
            return y + rowH;
        };
        const int h = groupTop + layout (0, 0, false) + groupPad;
        auto sa = area.removeFromTop (h);
        settingsGroup.setBounds (sa);
        layout (sa.getX() + groupPad, sa.getY() + groupTop, true);
    }

    area.removeFromTop (8);
    auto bottom = area.removeFromBottom (170);
    logView.setBounds (bottom.removeFromBottom (130));
    bottom.removeFromBottom (8);
    auto controls = bottom;
    startButton.setBounds (controls.removeFromLeft (80));
    controls.removeFromLeft (6);
    stopButton.setBounds (controls.removeFromLeft (70));
    controls.removeFromLeft (6);
    exportButton.setBounds (controls.removeFromRight (110));
    controls.removeFromRight (6);
    etaLabel.setBounds (controls.removeFromRight (juce::jmin (300, controls.getWidth() / 2)));
    controls.removeFromRight (6);
    progressBar.setBounds (controls);

    auto tableHeader = area.removeFromTop (26);
    allButton.setBounds (tableHeader.removeFromLeft (50));
    tableHeader.removeFromLeft (4);
    noneButton.setBounds (tableHeader.removeFromLeft (50));
    tableHeader.removeFromLeft (8);
    addArtButton.setBounds (tableHeader.removeFromLeft (28));
    removeArtButton.setBounds (tableHeader.removeFromLeft (28));
    tableHeader.removeFromLeft (12);
    nextStepLabel.setBounds (tableHeader);
    area.removeFromTop (4);
    table.setBounds (area);
}

// ============================================================================ plugin handling

void MainComponent::scanPlugins()
{
    pluginFiles.clear();
    pluginBox.clear (juce::dontSendNotification);
    // the platform's standard VST3 folders (macOS: ~/Library and /Library, Windows: Common Files and the
    // per-user Programs folder), including vendor subfolders; a .vst3 bundle is a leaf, not searched into
    for (auto* format : formatManager.getFormats())
        for (auto& path : format->searchPathsForPlugins (format->getDefaultLocationsToSearch(), true, false))
            pluginFiles.addIfNotAlreadyThere (juce::File (path));

    // by the name shown, wherever the vendor put the bundle
    std::sort (pluginFiles.begin(), pluginFiles.end(), [] (const juce::File& a, const juce::File& b) {
        return a.getFileNameWithoutExtension().compareNatural (b.getFileNameWithoutExtension()) < 0;
    });
    int preferred = 0;
    for (int i = 0; i < pluginFiles.size(); ++i)
    {
        pluginBox.addItem (pluginFiles[i].getFileNameWithoutExtension(), i + 1);
        if (pluginFiles[i].getFileNameWithoutExtension() == "SINE Player")
            preferred = i + 1;
    }
    pluginBox.setSelectedId (preferred != 0 ? preferred : 1, juce::dontSendNotification);
}

void MainComponent::loadPlugin()
{
    const int i = pluginBox.getSelectedId() - 1;
    if (i < 0 || i >= pluginFiles.size())
        return;

    pool.clear();
    closeEditor();
    releaseMaster();

    juce::OwnedArray<juce::PluginDescription> descs;
    for (auto* format : formatManager.getFormats())
        format->findAllTypesForFile (descs, pluginFiles[i].getFullPathName());
    if (descs.isEmpty())
    {
        log ("Couldn't open " + pluginFiles[i].getFullPathName());
        return;
    }
    masterDesc = *descs[0];
    for (auto* d : descs)
        if (d->isInstrument)
        {
            masterDesc = *d;
            break;
        }

    const auto settings = collectSettings();
    log (utf8 ("Loading ") + masterDesc.name + " " + masterDesc.version + utf8 ("…"));
    loadPluginButton.setEnabled (false);
    juce::Component::SafePointer<MainComponent> safe (this);
    formatManager.createPluginInstanceAsync (masterDesc, settings.sampleRate, settings.blockSize,
        [safe, settings] (std::unique_ptr<juce::AudioPluginInstance> inst, const juce::String& error) {
            if (safe == nullptr)
                return;
            safe->loadPluginButton.setEnabled (true);
            if (inst == nullptr)
            {
                safe->log ("Couldn't load the plugin: " + error);
                return;
            }
            inst->enableAllBuses();
            inst->prepareToPlay (settings.sampleRate, settings.blockSize);
            safe->master = std::move (inst);
            safe->masterPump = std::make_unique<MasterPump> (*safe->master, settings.sampleRate, settings.blockSize);
            safe->pluginGroup.setText ("1. Plugin: " + safe->masterDesc.name + " (latency "
                                       + juce::String (safe->master->getLatencySamples()) + " samples, "
                                       + juce::String (safe->master->getTotalNumOutputChannels()) + " outputs)");
            safe->log ("Load your instrument in the plugin window.");
            safe->updatePlayerMode();
            safe->updateEnablement();
            safe->showEditor();
        });
}

void MainComponent::showEditor()
{
    if (master == nullptr)
        return;
    if (editorWindow != nullptr)
    {
        editorWindow->toFront (true);
        return;
    }
    if (auto* ed = master->createEditorIfNeeded())
        editorWindow = std::make_unique<PluginWindow> (ed, [this] { juce::MessageManager::callAsync ([this] { closeEditor(); }); });
}

void MainComponent::closeEditor()
{
    editorWindow = nullptr; // deletes the editor before the processor
}

// ============================================================================ preset handling

void MainComponent::choosePreset()
{
    chooser = std::make_unique<juce::FileChooser> ("Open Preset", defaultPresetDir(), "*.json");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc) {
                              if (fc.getResult().existsAsFile())
                                  loadPreset (fc.getResult());
                          });
}

void MainComponent::loadPreset (const juce::File& f)
{
    Collection c;
    auto r = Collection::load (f, c);
    if (r.failed())
    {
        log ("Couldn't open the preset: " + r.getErrorMessage());
        return;
    }
    collection = std::move (c);
    presetModified = false;
    instrumentIndex = -1;
    rows.clear(); // before any refresh: the table reads rows against the selected instrument
    refreshInstrumentBox();
    table.updateContent();
    log ("Opened " + f.getFileName() + ". Choose the instrument to measure.");
    // lead straight into the instrument choice
    juce::Component::SafePointer<juce::ComboBox> box (&instrumentBox);
    juce::MessageManager::callAsync ([box] { if (box != nullptr) box->showPopup(); });
    updateEnablement();
}

void MainComponent::selectInstrument (int index)
{
    if (index < 0 || index >= (int) collection.instruments.size())
        return;
    instrumentIndex = index;
    rows.clear();
    for (auto& a : collection.instruments[(size_t) index].articulations)
    {
        Row r;
        r.enabled = ! a.ranges.isEmpty();
        r.category = a.category;
        r.keyswitchOverride = a.keyswitch;
        // auto: detected per articulation at run time (SINE mixes velocity- and CC1-switched articulations)
        r.dynSource = a.dynamicsSource;
        rows.push_back (r);
    }
    table.updateContent();
    table.repaint();
    updateEnablement();
    const auto& inst = collection.instruments[(size_t) index];
    if (collection.keyswitchBase.isNotEmpty() && parseNoteName (collection.keyswitchBase) >= 0
        && ksBaseEdit.getText().trim() != collection.keyswitchBase)
    {
        ksBaseEdit.setText (collection.keyswitchBase, juce::sendNotification);
        log ("Keyswitch base: " + collection.keyswitchBase + " (from the preset)");
    }
    log ("Instrument: " + inst.displayName() + " (" + juce::String ((int) inst.articulations.size()) + " articulations, "
         + juce::String ((int) std::count_if (rows.begin(), rows.end(), [] (auto& r) { return r.enabled; })) + " selected)");
}

void MainComponent::setAllRuns (bool enabled)
{
    if (running || instrumentIndex < 0)
        return;
    auto& inst = collection.instruments[(size_t) instrumentIndex];
    for (size_t i = 0; i < rows.size(); ++i)
        rows[i].enabled = enabled && ! inst.articulations[i].ranges.isEmpty();
    table.updateContent();
    table.repaint();
}

int MainComponent::keyswitchFor (int row) const
{
    if (! useKsToggle.getToggleState() || instrumentIndex < 0 || instrumentIndex >= (int) collection.instruments.size() || row < 0
        || row >= (int) rows.size() || row >= (int) collection.instruments[(size_t) instrumentIndex].articulations.size())
        return -1;
    if (rows[(size_t) row].keyswitchOverride >= 0)
        return rows[(size_t) row].keyswitchOverride;
    const int base = parseNoteName (ksBaseEdit.getText());
    if (base < 0)
        return -1;
    const int ks = base + collection.instruments[(size_t) instrumentIndex].articulations[(size_t) row].index - 1;
    return ks <= 127 ? ks : -1;
}

// ============================================================================ player mode / preset editing

bool MainComponent::isSinePlayer() const
{
    return master != nullptr && (masterDesc.name.containsIgnoreCase ("SINE") || masterDesc.manufacturerName.containsIgnoreCase ("Orchestral Tools"));
}

void MainComponent::updatePlayerMode()
{
    // SINE-only: reading the player's articulation list / RR reset times and patching them
    const bool sine = isSinePlayer();
    updateRrResetBox (sine);
    log (sine ? juce::String ("SINE Player: using its articulations, layers, round robins and legato zones.")
              : masterDesc.name + ": using the preset. RR reset is set to Never.");
}

void MainComponent::updateRrResetBox (bool sine)
{
    // a ComboBox can't hide an item: rebuild the list, keeping the chosen time. Switching player
    // type moves between the two defaults ('As in SINE' <-> 'Never'). The optional RR key is for other players only.
    int selected = rrResetBox.getSelectedId();
    if (selected == 1 && ! sine)
        selected = 2;
    else if ((selected == 2 || selected == 0) && sine)
        selected = 1;
    else if (selected == 0)
        selected = 2;
    rrResetBox.clear (juce::dontSendNotification);
    if (sine)
        rrResetBox.addItem ("As in SINE", 1);
    rrResetBox.addItem ("Never", 2);
    for (int ms : { 1000, 1500, 2000, 3000 })
        rrResetBox.addItem (juce::String (ms) + " ms", ms);
    rrResetBox.setSelectedId (selected, juce::dontSendNotification);
    rrResetKeyLabel.setVisible (! sine);
    rrResetKeyEdit.setVisible (! sine);
    if (getWidth() > 0)
        resized();

    juce::String tip ("Pause after which the player restarts its round robins.\n");
    if (sine)
        tip << "As in SINE: each articulation's own setting.\n";
    tip << "Never: they keep cycling or stay random (common in Kontakt).\n";
    tip << "A time: wait that long before each series" << (sine ? ", and set it in SINE." : ".");
    if (! sine)
        tip << "\nRR key (optional): the player resets them on a key; it is sent before each series (with Never, it is the reset).";
    tip << "\nUnless the round robins never reset, Secant first checks that the player replays identically, and switches to Never if not.";
    rrResetBox.setTooltip (tip);
}

tp::Articulation* MainComponent::articulationAt (int row)
{
    if (instrumentIndex < 0 || instrumentIndex >= (int) collection.instruments.size())
        return nullptr;
    auto& arts = collection.instruments[(size_t) instrumentIndex].articulations;
    return row >= 0 && row < (int) arts.size() ? &arts[(size_t) row] : nullptr;
}

void MainComponent::presetEdited()
{
    presetModified = true;
    updatePresetLabel();
    table.updateContent();
    table.repaint();
}

void MainComponent::updatePresetLabel()
{
    if (collection.instruments.empty())
    {
        presetLabel.setText ("No preset", juce::dontSendNotification);
        return;
    }
    presetLabel.setText ((collection.name.isNotEmpty() ? collection.name : juce::String ("Untitled")) + (presetModified ? " *" : "")
                             + "  (" + juce::String ((int) collection.instruments.size()) + " instruments)",
                         juce::dontSendNotification);
    presetLabel.setTooltip (presetModified ? "Edited. Save to keep your changes." : collection.file.getFullPathName());
}

void MainComponent::refreshInstrumentBox()
{
    instrumentBox.clear (juce::dontSendNotification);
    for (size_t i = 0; i < collection.instruments.size(); ++i)
        instrumentBox.addItem (collection.instruments[i].displayName(), (int) i + 1);
    if (instrumentIndex >= 0)
        instrumentBox.setSelectedItemIndex (instrumentIndex, juce::dontSendNotification);
    updatePresetLabel();
    updateEnablement();
}

void MainComponent::newPreset()
{
    if (running)
        return;
    Collection c;
    c.name = "Untitled";
    c.player = master != nullptr ? masterDesc.name : juce::String ("Generic");
    c.vendor = master != nullptr ? masterDesc.manufacturerName : juce::String();
    c.keyswitchBase = ksBaseEdit.getText().trim();
    Instrument inst;
    inst.name = "Instrument 1";
    c.instruments.push_back (inst);
    collection = std::move (c);
    presetModified = true;
    instrumentIndex = -1;
    rows.clear();
    refreshInstrumentBox();
    selectInstrument (0);
    instrumentBox.setSelectedItemIndex (0, juce::dontSendNotification);
    addArticulation();
    log ("New preset. Add articulations with +, double-click a cell to edit it, then save.");
}

void MainComponent::showSaveMenu()
{
    if (collection.instruments.empty())
    {
        log ("Nothing to save. Open or create a preset first.");
        return;
    }
    juce::MemoryBlock state;
    if (master != nullptr && instrumentIndex >= 0)
        master->getStateInformation (state);
    const bool sine = parseSineState (state).instrument() != nullptr;
    juce::PopupMenu m;
    m.addItem (utf8 ("Save Preset…"), [this] { savePreset(); });
    // only offered with SINE Player: other players' configuration can't be read
    if (isSinePlayer())
    {
        m.addItem (utf8 ("Save with SINE Settings…"), sine, false, [this, state] {
            if (applySineConfiguration (state))
                savePreset();
        });
        if (! sine)
            m.addSectionHeader ("Load an instrument in SINE Player first");
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (savePresetButton));
}

bool MainComponent::applySineConfiguration (const juce::MemoryBlock& state)
{
    const auto sineState = parseSineState (state);
    const auto* si = sineState.instrument();
    if (si == nullptr || instrumentIndex < 0 || running)
        return false;
    auto& inst = collection.instruments[(size_t) instrumentIndex];
    const int base = parseNoteName (ksBaseEdit.getText());
    int changed = 0;
    juce::StringArray matched, missing;
    for (size_t i = 0; i < inst.articulations.size(); ++i)
    {
        auto& a = inst.articulations[i];
        // the same matching a run uses: by keyswitch, else by title (SINE wins on the key)
        ArticulationPlan p;
        p.art = a;
        p.keyswitch = a.keyswitch >= 0 ? a.keyswitch : base >= 0 ? base + a.index - 1 : -1;
        PlanNotes ignored;
        attachSine (p, si, DynamicsSource::autoDetect, ignored);
        if (! p.hasSine)
        {
            missing.add ("#" + juce::String (a.index) + " " + a.name);
            continue;
        }
        const auto& s = p.sine;
        matched.add (s.title);
        juce::StringArray what;
        const int defaultKs = base >= 0 ? base + a.index - 1 : -1;
        const int ks = p.keyswitch == defaultKs ? -1 : p.keyswitch; // explicit only where it leaves the grid
        if (ks != a.keyswitch)
        {
            what.add ("keyswitch " + (ks >= 0 ? noteName (ks) : "base + " + juce::String (a.index) + " - 1"));
            a.keyswitch = ks;
            rows[i].keyswitchOverride = ks;
        }
        if (! s.dynamics.isEmpty() && s.dynamics != a.dynamics)
        {
            what.add ("layers " + a.dynamics.joinIntoString (" ") + " -> " + s.dynamics.joinIntoString (" "));
            a.dynamics = s.dynamics;
        }
        const auto src = s.modwheelDynamics ? DynamicsSource::cc1 : DynamicsSource::velocity;
        if (a.dynamicsSource == DynamicsSource::autoDetect)
        {
            what.add ("dyn src " + toString (src));
            a.dynamicsSource = src;
            rows[i].dynSource = src;
        }
        if (s.roundRobins >= 1 && s.roundRobins != a.roundRobins)
        {
            what.add ("RR " + juce::String (a.roundRobins) + " -> " + juce::String (s.roundRobins));
            a.roundRobins = s.roundRobins;
        }
        if (s.legato != a.legato)
        {
            what.add (juce::String ("legato ") + (s.legato ? "on" : "off"));
            a.legato = s.legato;
        }
        if (s.keyLow >= 0 && s.keyHigh >= s.keyLow && ! a.ranges.isEmpty())
        {
            // the player's playable range: keep the docs' ranges where they overlap it
            juce::Array<NoteRange> clipped;
            for (auto r : a.ranges)
                if (r.hi >= s.keyLow && r.lo <= s.keyHigh)
                    clipped.add ({ juce::jmax (r.lo, s.keyLow), juce::jmin (r.hi, s.keyHigh) });
            const bool same = clipped.size() == a.ranges.size()
                              && std::equal (clipped.begin(), clipped.end(), a.ranges.begin(),
                                             [] (const NoteRange& x, const NoteRange& y) { return x.lo == y.lo && x.hi == y.hi; });
            if (! clipped.isEmpty() && ! same)
            {
                what.add ("range " + a.rangeText() + " -> " + noteName (s.keyLow) + "-" + noteName (s.keyHigh) + " overlap");
                a.ranges = clipped;
            }
        }
        if (! what.isEmpty())
        {
            ++changed;
            log ("#" + juce::String (a.index) + " " + a.name + ": " + what.joinIntoString (", "));
        }
    }
    juce::StringArray extra;
    for (auto& s : si->articulations)
        if (s.enabled && ! matched.contains (s.title))
            extra.add (s.title);
    log ("Updated " + juce::String (changed) + " of " + juce::String ((int) inst.articulations.size()) + " articulations from SINE '"
         + si->title + "'."
         + (missing.isEmpty() ? juce::String() : " Not in SINE: " + missing.joinIntoString (", ") + ".")
         + (extra.isEmpty() ? juce::String() : " Not in the preset: " + extra.joinIntoString (", ") + "."));
    if (changed > 0)
        presetEdited();
    return true;
}

void MainComponent::savePreset()
{
    if (collection.instruments.empty())
    {
        log ("Nothing to save. Open or create a preset first.");
        return;
    }
    auto start = collection.file != juce::File() ? collection.file
                                                 : defaultPresetDir().getChildFile (juce::File::createLegalFileName (collection.name) + ".json");
    chooser = std::make_unique<juce::FileChooser> ("Save Preset", start, "*.json");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc) {
                              auto f = fc.getResult();
                              if (f == juce::File())
                                  return;
                              f = f.withFileExtension ("json");
                              if (collection.name.isEmpty() || collection.name == "Untitled")
                                  collection.name = f.getFileNameWithoutExtension();
                              collection.keyswitchBase = ksBaseEdit.getText().trim();
                              auto r = collection.save (f);
                              if (r.wasOk())
                              {
                                  collection.file = f;
                                  presetModified = false;
                                  refreshInstrumentBox();
                              }
                              log (r.wasOk() ? "Saved " + f.getFullPathName() : "Couldn't save: " + r.getErrorMessage());
                          });
}

void MainComponent::addInstrument()
{
    if (running)
        return;
    if (collection.instruments.empty())
    {
        newPreset();
        return;
    }
    Instrument inst;
    inst.name = "Instrument " + juce::String ((int) collection.instruments.size() + 1);
    collection.instruments.push_back (inst);
    presetModified = true;
    instrumentIndex = (int) collection.instruments.size() - 1;
    rows.clear(); // the new instrument has no articulations yet
    refreshInstrumentBox();
    selectInstrument (instrumentIndex);
    addArticulation();
}

void MainComponent::removeInstrument()
{
    if (running || instrumentIndex < 0 || instrumentIndex >= (int) collection.instruments.size())
        return;
    log ("Removed " + collection.instruments[(size_t) instrumentIndex].displayName() + " (unsaved)");
    collection.instruments.erase (collection.instruments.begin() + instrumentIndex);
    presetModified = true;
    instrumentIndex = -1;
    rows.clear();
    refreshInstrumentBox();
    if (! collection.instruments.empty())
    {
        selectInstrument (0);
        instrumentBox.setSelectedItemIndex (0, juce::dontSendNotification);
    }
    table.updateContent();
}

void MainComponent::addArticulation()
{
    if (running)
        return;
    if (collection.instruments.empty())
    {
        newPreset(); // creates the preset and its first articulation
        return;
    }
    if (instrumentIndex < 0)
    {
        log ("Choose an instrument first.");
        return;
    }
    auto& arts = collection.instruments[(size_t) instrumentIndex].articulations;
    const int sel = table.getSelectedRow();
    const int at = sel >= 0 && sel < (int) arts.size() ? sel + 1 : (int) arts.size();
    Articulation a;
    // like the row it follows (same range / layers is the common case), keyswitch slot after the last one
    if (at > 0)
    {
        const auto& prev = arts[(size_t) at - 1];
        a.ranges = prev.ranges;
        a.dynamics = prev.dynamics;
        a.category = prev.category;
    }
    else
    {
        a.ranges.add ({ parseNoteName ("C2"), parseNoteName ("C6") });
        a.dynamics = { "mf" };
    }
    int maxIndex = 0;
    for (auto& x : arts)
        maxIndex = juce::jmax (maxIndex, x.index);
    a.index = maxIndex + 1;
    a.name = "Articulation " + juce::String (a.index);
    arts.insert (arts.begin() + at, a);
    Row r;
    r.enabled = true;
    r.category = a.category;
    rows.insert (rows.begin() + at, r);
    presetEdited();
    refreshInstrumentBox();
    table.selectRow (at);
}

void MainComponent::removeArticulations()
{
    if (running || instrumentIndex < 0)
        return;
    auto& arts = collection.instruments[(size_t) instrumentIndex].articulations;
    const auto sel = table.getSelectedRows();
    if (sel.isEmpty())
    {
        log ("Select the articulations to remove.");
        return;
    }
    for (int i = sel.getNumRanges(); --i >= 0;)
    {
        const auto range = sel.getRange (i);
        for (int r = range.getEnd(); --r >= range.getStart();)
            if (r < (int) arts.size())
            {
                arts.erase (arts.begin() + r);
                rows.erase (rows.begin() + r);
            }
    }
    // keyswitch slots (#) of the remaining articulations stay as they are: the player's keys don't move
    table.deselectAllRows();
    presetEdited();
    refreshInstrumentBox();
    log ("Removed " + juce::String (sel.size()) + (sel.size() == 1 ? " articulation" : " articulations") + " (unsaved)");
}

// ============================================================================ table

int MainComponent::getNumRows() { return (int) rows.size(); }

void MainComponent::paintRowBackground (juce::Graphics& g, int row, int, int, bool selected)
{
    auto c = getLookAndFeel().findColour (juce::ListBox::backgroundColourId);
    if (row % 2)
        c = c.brighter (0.04f);
    if (selected)
        c = c.interpolatedWith (juce::Colours::steelblue, 0.4f);
    g.fillAll (c);
}

void MainComponent::paintCell (juce::Graphics& g, int row, int col, int w, int h, bool)
{
    const auto* art = articulationAt (row);
    if (art == nullptr || row >= (int) rows.size())
        return;
    const auto& a = *art;
    const auto& r = rows[(size_t) row];
    juce::String text;
    auto colour = juce::Colours::white;

    switch (col)
    {
        case colIndex: text = juce::String (a.index); break;
        case colRR:
            if (r.planIdx >= 0 && ctx != nullptr && r.planIdx < (int) ctx->plans.size() && ctx->plans[(size_t) r.planIdx].hasSine
                && ctx->plans[(size_t) r.planIdx].sine.roundRobins != a.roundRobins)
            {
                // the cell editor shows the preset's count; the player's own count is drawn on the right
                text = "SINE " + juce::String (ctx->plans[(size_t) r.planIdx].sine.roundRobins);
                colour = juce::Colours::orange;
                g.setColour (colour);
                g.setFont (juce::FontOptions (11.0f));
                g.drawText (text, 0, 0, w - 4, h, juce::Justification::centredRight, true);
            }
            text = {};
            break;
        case colRRFound:
            if (r.planIdx >= 0)
            {
                text = store.rrSummaryFor (r.planIdx);
                int expected = a.roundRobins;
                if (ctx != nullptr && r.planIdx < (int) ctx->plans.size() && ctx->plans[(size_t) r.planIdx].hasSine)
                    expected = ctx->plans[(size_t) r.planIdx].sine.roundRobins;
                if (text.isNotEmpty() && text != juce::String (expected))
                    colour = juce::Colours::orange; // differs from the player's own count (or never repeated)
            }
            break;
        case colIssues:
            text = a.issues.joinIntoString ("; ");
            colour = juce::Colours::orange;
            break;
        case colProgress:
            if (r.planIdx >= 0)
            {
                const auto done = store.doneJobsFor (r.planIdx), total = store.totalJobsFor (r.planIdx);
                const float frac = total > 0 ? (float) done / (float) total : 0.0f;
                g.setColour (juce::Colours::seagreen.withAlpha (0.6f));
                g.fillRect (2.0f, 4.0f, (float) (w - 4) * frac, (float) h - 8.0f);
                text = juce::String (done) + " / " + juce::String (total);
            }
            break;
        case colMedian:
            if (r.planIdx >= 0)
            {
                const auto m = store.medianPrimaryFor (r.planIdx);
                if (! std::isnan (m))
                    text = juce::String (m, 2);
                const auto lm = store.medianLegatoFor (r.planIdx);
                if (! std::isnan (lm))
                    text << (text.isEmpty() ? "" : " | ") << "L " << juce::String (lm, 1);
            }
            break;
        default: break;
    }
    g.setColour (colour);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (text, 4, 0, w - 8, h, juce::Justification::centredLeft, true);
}

juce::Component* MainComponent::refreshComponentForCell (int row, int col, bool, juce::Component* existing)
{
    // rows and the selected instrument's articulations can disagree for a moment while either changes
    if (row < 0 || row >= (int) rows.size() || articulationAt (row) == nullptr)
    {
        delete existing;
        return nullptr;
    }

    auto make = [&] (auto* typed, auto factory) -> juce::Component* {
        if (typed == nullptr)
        {
            delete existing;
            typed = factory();
        }
        typed->setRow (row);
        return typed;
    };

    switch (col)
    {
        case colRun: return make (dynamic_cast<RunToggle*> (existing), [this] { return new RunToggle (*this); });
        case colCategory: return make (dynamic_cast<CategoryBox*> (existing), [this] { return new CategoryBox (*this); });
        case colDynSrc: return make (dynamic_cast<DynSrcBox*> (existing), [this] { return new DynSrcBox (*this); });
        case colKeyswitch: return make (dynamic_cast<KeyswitchLabel*> (existing), [this] { return new KeyswitchLabel (*this); });
        case colLegato: return make (dynamic_cast<LegatoToggle*> (existing), [this] { return new LegatoToggle (*this); });
        case colName:
        case colRange:
        case colDynamics:
        case colRR:
        {
            auto* cell = dynamic_cast<TextCell*> (existing);
            if (cell == nullptr)
            {
                delete existing;
                cell = new TextCell (*this);
            }
            cell->setRowCol (row, col);
            cell->setInterceptsMouseClicks (true, true);
            return cell;
        }
        default:
            delete existing;
            return nullptr;
    }
}

juce::String MainComponent::getCellTooltip (int row, int col)
{
    const auto* a = articulationAt (row);
    if (a == nullptr || row >= (int) rows.size())
        return {};
    if (col == colIssues || col == colName)
        return a->issues.joinIntoString ("\n");
    return {};
}

// ============================================================================ run

RunSettings MainComponent::collectSettings() const
{
    RunSettings s;
    s.sampleRate = sampleRateBox.getText().getDoubleValue();
    if (s.sampleRate <= 0)
        s.sampleRate = 48000.0;
    s.numWorkers = (int) workersSlider.getValue();
    s.noteStep = juce::jmax (1, stepBox.getSelectedId());
    const int rrId = rrResetBox.getSelectedId();
    s.rrNeverResets = rrId == 2;
    s.rrResetOverrideMs = rrId >= 1000 ? (double) rrId : 0.0;
    // empty (or SINE Player, where it is hidden) = the player has no reset key
    s.rrResetKey = rrResetKeyEdit.isVisible() && rrResetKeyEdit.getText().trim().isNotEmpty()
                       ? parseNoteName (rrResetKeyEdit.getText().trim()) : -1;
    s.confirmCycle = confirmBox.getSelectedId() > 1;
    s.confirmHits = juce::jmax (1, confirmBox.getSelectedId() - 1);
    s.maxHitsPerSeries = juce::jmax (2, maxHitsBox.getSelectedId());
    s.sendAllSoundOff = allSoundOffToggle.getToggleState();
    switch (speedBox.getSelectedId())
    {
        case 2: s.maxSpeed = 8.0; break;
        case 3: s.maxSpeed = 4.0; break;
        case 4: s.maxSpeed = 2.0; break;
        case 5: s.maxSpeed = 1.0; s.nonRealtime = false; break;
        default: s.maxSpeed = 0.0; break;
    }
    s.onset.sampleRate = s.sampleRate;
    switch (smoothBox.getSelectedId())
    {
        case 1: s.onset.periodSmoothing = 0.0; break;
        case 2: s.onset.periodSmoothing = 0.25; break;
        case 4: s.onset.periodSmoothing = 1.0; break;
        default: s.onset.periodSmoothing = 0.5; break;
    }
    for (int i = 0; i < 4; ++i)
        if (primaryBoxes[i].getSelectedItemIndex() >= 0)
            s.timing[kCategoryOrder[i]].primaryMethod = primaryBoxes[i].getText();

    s.measureLegato = legatoToggle.getToggleState();
    {
        const int id = legatoIntervalsBox.getSelectedId();
        s.legatoIntervals.clear();
        for (int i = 1; i <= juce::jmin (12, id); ++i)
            s.legatoIntervals.add (i);
        s.legatoUnison = id == 13;
    }
    switch (legatoGapsBox.getSelectedId())
    {
        case 2: s.legatoGapsMs = { 60.0, 100.0, 150.0, 200.0, 300.0, 450.0, 650.0, 900.0, 1500.0 }; break;
        case 3: s.legatoGapsMs = { 1000.0 }; break;
        default: s.legatoGapsMs = { 100.0, 250.0, 500.0, 1000.0 }; break;
    }
    s.legatoPrimary = legatoPrimaryBox.getSelectedId() == 2 ? "mid" : legatoPrimaryBox.getSelectedId() == 3 ? "start" : "arrive";
    return s;
}

void MainComponent::startRun()
{
    if (running || creatingWorkers)
        return;
    if (auto missing = missingStep(); missing.isNotEmpty())
    {
        log ("Can't start. " + missing);
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Can't Start", missing);
        if (master != nullptr && ! collection.instruments.empty() && instrumentIndex < 0)
            instrumentBox.showPopup();
        return;
    }
    auto settings = collectSettings();
    const auto& inst = collection.instruments[(size_t) instrumentIndex];

    // ---- the player's own configuration (SINE): keyswitches, layers, RR counts, legato zones
    runState.reset();
    master->getStateInformation (runState);
    const bool sineRun = parseSineState (runState).valid;
    if (settings.rrResetKey >= 0 && settings.rrNeverResets)
    {
        // no reset after a pause, but on the key: the pauses only let tails and controllers settle
        settings.rrNeverResets = false;
        settings.rrResetPauseMs = 500.0;
        settings.legatoResetPauseMs = 1000.0;
        log ("RR reset: key " + noteName (settings.rrResetKey) + " before each series, pauses "
             + juce::String (settings.rrResetPauseMs, 0) + " ms");
    }
    if (! sineRun && ! settings.rrNeverResets && settings.rrResetOverrideMs <= 0 && settings.rrResetKey < 0)
    {
        // "As in SINE" has nothing to read from another player: assume the safe case
        settings.rrNeverResets = true;
        log ("RR reset: can't read the player's setting, using Never. Choose a time if the player restarts its round robins.");
    }
    if (settings.rrNeverResets)
    {
        log ("RR reset: Never. Notes wait for silence, no tail subtraction or fast repetition, layers use velocity unless set to CC1.");
        settings.rrResetOverrideMs = 0;
    }
    else if (settings.rrResetOverrideMs > 0 && ! sineRun)
    {
        // another player with a known reset time: only the pauses follow it (nothing to patch)
        settings.rrResetPauseMs = settings.rrResetOverrideMs + 250.0;
        settings.legatoResetPauseMs = settings.rrResetPauseMs;
        log ("RR reset: " + juce::String (settings.rrResetOverrideMs, 0) + " ms, pauses " + juce::String (settings.rrResetPauseMs, 0) + " ms"
             + (settings.rrResetKey >= 0 ? ", then key " + noteName (settings.rrResetKey) : juce::String()));
    }
    else if (settings.rrResetOverrideMs > 0)
    {
        // every worker loads this state: the player then resets its RR counters after the chosen time
        juce::String err;
        auto patched = withRoundRobinReset (runState, settings.rrResetOverrideMs, &err);
        if (! patched.isEmpty() && parseSineState (patched).valid)
        {
            runState = patched;
            // the pauses only need the chosen reset time (+ margin; tails are rendered out first). Legato
            // chains also wait out SINE's adaptive legato timing.
            settings.rrResetPauseMs = settings.rrResetOverrideMs + 250.0;
            double adaptiveMs = 0;
            if (auto* si = parseSineState (runState).instrument(); si != nullptr && si->hasAdaptive)
                adaptiveMs = juce::jmax (si->adaptiveAmplitudeHiMs, si->adaptiveOffsetHiMs, si->adaptiveTransitionMinMs);
            settings.legatoResetPauseMs = juce::jmax (settings.rrResetPauseMs, adaptiveMs + 250.0);
            if (settings.measureLegato && ! settings.legatoGapsMs.empty()
                && *std::max_element (settings.legatoGapsMs.begin(), settings.legatoGapsMs.end()) >= settings.rrResetOverrideMs)
                log ("Note: legato spacing of " + juce::String (settings.rrResetOverrideMs, 0) + " ms or more can restart round robins "
                     "in short patches. Those cells reach only the first recordings.");
            log ("RR reset: " + juce::String (settings.rrResetOverrideMs, 0) + " ms in the workers (your instance is unchanged), pauses "
                 + juce::String (settings.rrResetPauseMs, 0) + " ms, legato " + juce::String (settings.legatoResetPauseMs, 0) + " ms");
        }
        else
        {
            log ("Warning: couldn't set RR reset (" + (err.isNotEmpty() ? err : juce::String ("unknown state"))
                 + "). Using the player's settings.");
            settings.rrResetOverrideMs = 0;
        }
    }
    // anything that relies on the reset gets checked by the first worker (it may switch the run to Never)
    settings.checkDeterminism = ! settings.rrNeverResets;
    const auto sineState = parseSineState (runState);
    const auto* sineInst = sineState.instrument();
    if (sineInst != nullptr)
        log ("SINE " + sineState.samplerVersion + ": '" + sineInst->title + "', " + juce::String ((int) sineInst->articulations.size())
             + " articulations");
    else if (isSinePlayer())
        log ("Couldn't read SINE's settings (" + sineState.error + "). Using the preset.");
    PlanNotes planNotes;

    // ---- plans + jobs
    auto c = std::make_unique<RunContext>();
    c->settings = settings;
    juce::Array<int> playedNotes;
    for (size_t i = 0; i < rows.size(); ++i)
    {
        rows[i].planIdx = -1;
        const auto& a = inst.articulations[i];
        if (! rows[i].enabled || a.ranges.isEmpty())
            continue;
        ArticulationPlan p;
        p.articulationPos = (int) i;
        p.art = a;
        p.keyswitch = keyswitchFor ((int) i);
        p.category = rows[i].category;
        p.dynSource = rows[i].dynSource;
        attachSine (p, sineInst, rows[i].dynSource, planNotes);
        auto all = a.allNotes();
        if (p.hasSine && p.sine.keyLow >= 0 && p.sine.keyHigh >= p.sine.keyLow)
        {
            // the player's playable range wins over the docs
            juce::Array<int> inRange;
            for (auto n : all)
                if (n >= p.sine.keyLow && n <= p.sine.keyHigh)
                    inRange.add (n);
            if (inRange.size() != all.size())
                planNotes.warnings.add ("#" + juce::String (a.index) + " " + a.name + ": preset range " + a.rangeText() + ", SINE plays "
                                        + noteName (p.sine.keyLow) + "-" + noteName (p.sine.keyHigh) + ". Measuring the overlap.");
            if (! inRange.isEmpty())
                all = inRange;
        }
        for (int k = 0; k < all.size(); k += settings.noteStep)
            p.notes.add (all[k]);
        if (! all.isEmpty() && p.notes.getLast() != all.getLast())
            p.notes.add (all.getLast()); // always include the top note
        p.playable = all;
        p.measureLegato = settings.measureLegato && (a.legato || (p.hasSine && p.sine.legato));
        if (p.measureLegato)
            p.legatoCells = buildLegatoCells (p, settings);
        for (auto n : p.notes)
            playedNotes.addIfNotAlreadyThere (n);
        if (useKsToggle.getToggleState() && p.keyswitch < 0)
        {
            log ("No keyswitch for #" + juce::String (a.index) + " " + a.name + ". Check the keyswitch base.");
            return;
        }
        rows[i].planIdx = (int) c->plans.size();
        c->plans.push_back (std::move (p));
    }
    if (c->plans.empty())
    {
        log ("Nothing to run. Select at least one articulation.");
        return;
    }
    for (auto& p : c->plans)
        if (p.keyswitch >= 0 && playedNotes.contains (p.keyswitch))
            log ("Warning: keyswitch " + noteName (p.keyswitch) + " (#" + juce::String (p.art.index) + " " + p.art.name
                 + ") is inside a played range. Check the keyswitch base.");
    if (settings.rrResetKey >= 0 && playedNotes.contains (settings.rrResetKey))
        log ("Warning: RR reset key " + noteName (settings.rrResetKey) + " is inside a played range.");

    // one job per note: a single worker plays every articulation and layer of it (SINE keeps
    // its round-robin counters per instance, so a note's sequence must never be split)
    // expected work per job, so progress and ETA aren't fooled by quick note jobs finishing first:
    // ~hits for a note job, and for a legato job its transitions x ~8 hit-sized renders each
    // (repeated until identical, chains of growing length)
    auto noteUnits = [&] (int n) {
        double u = 0;
        for (auto& p : c->plans)
            if (p.notes.contains (n))
            {
                const int rr = p.expectedRoundRobins() > 0 ? p.expectedRoundRobins() : juce::jmax (1, p.art.roundRobins);
                double perLayer = rr + 1 + settings.confirmHits;
                if (p.category == Category::shortNote && settings.fastRepetition)
                    perLayer *= 2.0;
                if (p.category == Category::longNote || p.category == Category::dynamic)
                    perLayer *= 2.0; // tail-subtracted: two renders per hit
                u += perLayer * p.art.dynamics.size();
            }
        return juce::jmax (1.0, u);
    };
    playedNotes.sort();
    for (auto n : playedNotes)
    {
        Job j;
        j.note = n;
        j.units = noteUnits (n);
        c->jobs.push_back (j);
    }
    // legato chains start from the reset state, so they are independent jobs (any worker, any order)
    int legatoJobs = 0;
    for (int pi = 0; pi < (int) c->plans.size(); ++pi)
    {
        const auto& p = c->plans[(size_t) pi];
        if (! p.measureLegato)
            continue;
        juce::StringArray cellText;
        for (auto& cell : p.legatoCells)
            cellText.add (cell.type + " @ " + juce::String (cell.gapMs, 1) + " ms (vel " + juce::String (cell.velocity) + ")");
        const auto shape = legatoChainShape (p, settings);
        if (settings.rrNeverResets && shape.roundRobins > 1)
            log ("Warning: " + p.art.name + " has " + juce::String (shape.roundRobins) + " round robins that never reset. "
                 "Its legato transitions will be flagged.");
        log ("Legato " + p.art.name + ": " + cellText.joinIntoString (", ") + "; " + juce::String (p.art.dynamics.size()) + " layers, RR "
             + juce::String (shape.roundRobins) + " (chains of " + juce::String (shape.transitions) + " transitions"
             + (shape.bothStarts ? ", starting on either note" : "") + ")");
        for (auto n : p.notes)
        {
            const double units = legatoJobUnits (p, n, settings);
            const int transitions = legatoJobTransitions (p, n, settings);
            for (int d = 0; d < p.art.dynamics.size(); ++d)
            {
                Job j;
                j.note = n;
                j.legatoPlan = pi;
                j.legatoDyn = d;
                j.units = units;
                c->legatoTransitionsTotal += transitions;
                c->jobs.push_back (j);
                ++legatoJobs;
            }
        }
    }

    for (auto& j : c->jobs)
        c->totalUnits += j.units;
    store.reset (c->plans, settings, inst, collection);
    store.setSineInfo (sineState, planNotes.log);
    for (auto& l : planNotes.log)
        log (l);
    for (auto& w : planNotes.warnings)
    {
        log ("Warning: " + w);
        store.addWarning (w);
    }
    ResultStore::PluginInfo info;
    info.name = masterDesc.name;
    info.manufacturer = masterDesc.manufacturerName;
    info.version = masterDesc.version;
    info.fileOrIdentifier = masterDesc.fileOrIdentifier;
    info.latencySamples = master->getLatencySamples();
    info.numOutputChannels = master->getTotalNumOutputChannels();
    store.setPluginInfo (info);
    c->store = &store;
    ctx = std::move (c);

    log ("Run: " + juce::String ((int) ctx->plans.size()) + " articulations, " + juce::String ((int) ctx->jobs.size())
         + " notes, " + juce::String (settings.numWorkers) + " workers at " + juce::String (settings.sampleRate, 0) + " Hz, pause "
         + juce::String (settings.rrResetPauseMs, 0) + " ms, short/long spacing "
         + juce::String (settings.timing.at (tp::Category::shortNote).intervalMs, 0) + "/"
         + juce::String (settings.timing.at (tp::Category::longNote).intervalMs, 0) + " ms"
         + (settings.fastRepetition ? ", fast repetition " + juce::String (settings.fastRepIntervalMs, 0) + " ms" : juce::String())
         + (legatoJobs > 0 ? ", " + juce::String (legatoJobs) + " legato jobs" : juce::String()));

    // ---- clone the configured plugin into N workers (VST3 instances are created on the message thread)
    // keep the exact plugin setup next to the results (re-runs, offline RR / timing investigations)
    runStateFile = resultsDir().getChildFile (juce::File::createLegalFileName (
        collection.name + " - " + inst.name + " - " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S") + ".pluginstate"));
    runStateFile.getParentDirectory().createDirectory();
    if (runStateFile.replaceWithData (runState.getData(), runState.getSize()))
        log ("Saved " + runStateFile.getFileName());
    info.stateFile = runStateFile.getFullPathName();
    store.setPluginInfo (info);
    workers.clear();
    creatingWorkers = true;
    stopRequested = false;
    updateEnablement();
    table.updateContent();

    const bool reusable = ! pool.empty() && runState == poolState && juce::approximatelyEqual (poolRate, settings.sampleRate) && poolBlock == settings.blockSize;
    const int delayMs = 0;
    juce::Component::SafePointer<MainComponent> safe (this);
    if (reusable)
        log ("Reusing " + juce::String ((int) pool.size()) + " loaded workers");
    else if (! pool.empty())
    {
        // SINE's sample preloader thread can crash when instances are destroyed while it works (seen twice,
        // both times right after a setup change released a whole pool at once): release them one at a
        // time, then give the plugin a few seconds before new instances start preloading
        log (utf8 ("Plugin setup changed. Releasing ") + juce::String ((int) pool.size()) + utf8 (" workers…"));
        releasePoolThen ([safe, settings] { if (safe != nullptr) safe->createNextWorker (0, settings); });
        return;
    }
    juce::Timer::callAfterDelay (delayMs, [safe, settings] { if (safe != nullptr) safe->createNextWorker (0, settings); });
}

void MainComponent::releasePoolThen (std::function<void()> next)
{
    juce::Component::SafePointer<MainComponent> safe (this);
    if (pool.empty())
    {
        juce::Timer::callAfterDelay (3000, [safe, next] { if (safe != nullptr) next(); });
        return;
    }
    pool.pop_back(); // destroys one instance
    juce::Timer::callAfterDelay (750, [safe, next] { if (safe != nullptr) safe->releasePoolThen (next); });
}

// Workers are brought up strictly one at a time: instance i must report that it is sounding
// before instance i+1 is created, so the plugin never preloads several copies concurrently.
// Rendering starts as soon as the first worker is ready.
void MainComponent::createNextWorker (int index, RunSettings settings)
{
    juce::Component::SafePointer<MainComponent> safe (this);
    const bool allJobsTaken = ctx != nullptr && ctx->nextJob.load() >= ctx->jobs.size();
    if (stopRequested || index >= settings.numWorkers || allJobsTaken)
    {
        creatingWorkers = false;
        awaitingWorker = -1;
        if (workers.empty())
        {
            log (stopRequested ? "Stopped." : "Couldn't create any workers.");
            updateEnablement();
        }
        return;
    }

    auto startWorker = [this, safe, index, settings] (std::unique_ptr<juce::AudioPluginInstance> instance, bool reused) {
        workers.push_back (std::make_unique<RenderWorker> (index, std::move (instance), *ctx));
        workers.back()->startThread();
        log ("Worker " + juce::String (index) + (reused ? " reused" : " created") + utf8 (", waiting for sound…"));
        if (! running)
        {
            running = true;
            runStartedMs = juce::Time::getMillisecondCounterHiRes();
            updateEnablement();
            table.updateContent();
        }
        // timerCallback creates the next worker once this one has loaded (or given up). Its position, not
        // its id: an instance that failed to load leaves a gap in the ids
        awaitingWorker = (int) workers.size() - 1;
        nextWorkerIndex = index + 1;
        creationSettings = settings;
        juce::ignoreUnused (safe);
    };

    if (! pool.empty())
    {
        auto instance = std::move (pool.back());
        pool.pop_back();
        // the pool is keyed on state / rate / block size only: the speed setting may have changed
        instance->setNonRealtime (settings.nonRealtime);
        startWorker (std::move (instance), true);
        return;
    }

    formatManager.createPluginInstanceAsync (masterDesc, settings.sampleRate, settings.blockSize,
        [safe, settings, index, startWorker] (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& err) {
            if (safe == nullptr)
                return;
            auto& self = *safe;
            if (instance == nullptr || self.stopRequested)
            {
                if (instance == nullptr)
                    self.log ("Couldn't create worker " + juce::String (index) + ": " + err);
                self.createNextWorker (index + 1, settings);
                return;
            }
            instance->enableAllBuses();
            instance->setStateInformation (self.runState.getData(), (int) self.runState.getSize());
            instance->setNonRealtime (settings.nonRealtime);
            instance->prepareToPlay (settings.sampleRate, settings.blockSize);
            startWorker (std::move (instance), false);
        });
}

void MainComponent::stopRun()
{
    stopRequested = true;
    for (auto& w : workers)
        w->signalThreadShouldExit();
    log (utf8 ("Stopping…"));
}

void MainComponent::finishRun()
{
    running = false;
    for (auto& w : workers)
    {
        w->stopThread (10000);
        if (auto p = w->releasePlugin())
        {
            p->setPlayHead (nullptr);
            pool.push_back (std::move (p)); // kept loaded for the next run with the same setup
        }
    }
    workers.clear();
    poolState = runState;
    poolRate = ctx != nullptr ? ctx->settings.sampleRate : 0.0;
    poolBlock = ctx != nullptr ? ctx->settings.blockSize : 0;

    const auto elapsed = (juce::Time::getMillisecondCounterHiRes() - runStartedMs) / 1000.0;
    log ("Finished: " + juce::String (store.doneJobs()) + " / " + juce::String (store.totalJobs()) + " jobs, "
         + juce::String (store.hitsRendered()) + " hits in "
         + juce::String (elapsed, 1) + " s");
    if (store.doneJobs() < store.totalJobs() && ! stopRequested)
        log ("Some hits weren't rendered. See the worker messages above.");

    if (store.doneJobs() > 0)
    {
        const auto& inst = collection.instruments[(size_t) instrumentIndex];
        auto name = juce::File::createLegalFileName (collection.name + " - " + inst.name + " - "
                                                     + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S") + ".json");
        lastResultFile = resultsDir().getChildFile (name);
        auto r = store.writeJson (lastResultFile);
        log (r.wasOk() ? "Saved " + lastResultFile.getFullPathName() : "Couldn't save: " + r.getErrorMessage());
        if (r.wasOk())
        {
            auto d = writeCompactDelays (lastResultFile);
            log (d.wasOk() ? "Saved " + delaysFileFor (lastResultFile).getFileName() : "Couldn't save: " + d.getErrorMessage());
            auto m = writeExpressionMap (lastResultFile);
            log (m.wasOk() ? "Saved " + expressionMapFileFor (lastResultFile).getFileName() : "Couldn't save: " + m.getErrorMessage());
        }
    }
    updateEnablement();
    table.repaint();
}

void MainComponent::exportResults()
{
    if (store.doneJobs() == 0)
    {
        log ("No results yet.");
        return;
    }
    auto start = lastResultFile != juce::File() ? lastResultFile : resultsDir().getChildFile ("results.json");
    chooser = std::make_unique<juce::FileChooser> ("Export Results", start, "*.json");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc) {
                              auto f = fc.getResult();
                              if (f == juce::File())
                                  return;
                              f = f.withFileExtension ("json");
                              auto r = store.writeJson (f);
                              log (r.wasOk() ? "Exported " + f.getFullPathName() : "Couldn't export: " + r.getErrorMessage());
                              if (r.wasOk())
                              {
                                  auto d = writeCompactDelays (f);
                                  log (d.wasOk() ? "Exported " + delaysFileFor (f).getFileName() : "Couldn't export: " + d.getErrorMessage());
                                  auto m = writeExpressionMap (f);
                                  log (m.wasOk() ? "Exported " + expressionMapFileFor (f).getFileName() : "Couldn't export: " + m.getErrorMessage());
                              }
                          });
}

void MainComponent::timerCallback()
{
    for (auto& line : store.drainLog())
        log (line);
    refreshNextStep();

    if (creatingWorkers && awaitingWorker >= 0 && awaitingWorker < (int) workers.size())
    {
        // workers load one after another, so the first `i + 1` loaded ones include the awaited one
        const int i = awaitingWorker;
        if (ctx->loadedWorkers.load() > i || workers[(size_t) i]->finished() || stopRequested)
        {
            awaitingWorker = -1;
            createNextWorker (nextWorkerIndex, creationSettings);
        }
    }

    if (running)
    {
        const auto total = store.totalJobs(), done = store.doneJobs();
        // weighted by each job's expected work (legato jobs are far longer than note jobs)
        const double unitsDone = ctx != nullptr ? (double) ctx->doneMilliUnits.load() / 1000.0 : 0.0;
        const double unitsTotal = ctx != nullptr ? juce::jmax (1.0, ctx->totalUnits) : 1.0;
        progress = juce::jlimit (0.0, 1.0, unitsDone / unitsTotal);
        const double elapsed = (juce::Time::getMillisecondCounterHiRes() - runStartedMs) / 1000.0;
        juce::String eta;
        if (unitsDone > 0)
        {
            const double remainingSec = elapsed * (unitsTotal - unitsDone) / unitsDone;
            eta = juce::RelativeTime::seconds (remainingSec).getDescription() + " left (" + juce::String (100.0 * progress, 0) + "%)";
        }
        // legato jobs run for minutes and report results only at their end: count their transitions live
        if (ctx != nullptr && ctx->legatoTransitionsTotal > 0)
            eta << "  legato " << juce::String (ctx->legatoTransitionsDone.load()) << "/" << juce::String (ctx->legatoTransitionsTotal);
        etaLabel.setText (eta, juce::dontSendNotification);
        if (juce::Time::getMillisecondCounterHiRes() - lastStatusLogMs > 10000.0)
        {
            lastStatusLogMs = juce::Time::getMillisecondCounterHiRes();
            log ("Status: jobs " + juce::String (done) + "/" + juce::String (total) + ", hits " + juce::String (store.hitsRendered())
                 + ", workers " + juce::String ((int) std::count_if (workers.begin(), workers.end(), [] (auto& w) { return ! w->finished(); }))
                 + " " + eta);
        }
        table.repaint();

        if (! creatingWorkers && std::all_of (workers.begin(), workers.end(), [] (auto& w) { return w->finished(); }))
            finishRun();
    }
}

void MainComponent::updateEnablement()
{
    const bool busy = running || creatingWorkers;
    startButton.setEnabled (! busy); // explains what's missing instead of silently staying grey
    refreshNextStep();
    stopButton.setEnabled (busy);
    loadPluginButton.setEnabled (! busy);
    rescanButton.setEnabled (! busy);
    pluginBox.setEnabled (! busy);
    editorButton.setEnabled (master != nullptr);
    presetButton.setEnabled (! busy);
    instrumentBox.setEnabled (! busy && ! collection.instruments.empty());
    for (auto* c : std::initializer_list<juce::Component*> { &sampleRateBox, &workersSlider, &ksBaseEdit, &useKsToggle, &stepBox,
                                                             &speedBox, &confirmBox, &maxHitsBox, &allSoundOffToggle, &smoothBox, &rrResetBox, &rrResetKeyEdit, &rrResetKeyLabel,
                                                             &allButton, &noneButton,
                                                             &newPresetButton, &savePresetButton, &addInstrumentButton, &removeInstrumentButton,
                                                             &addArtButton, &removeArtButton })
        c->setEnabled (! busy);
    for (auto& b : primaryBoxes)
        b.setEnabled (! busy);
    table.updateContent();
}

void MainComponent::refreshNextStep()
{
    const bool busy = running || creatingWorkers;
    const auto missing = missingStep();
    nextStepLabel.setText (busy ? utf8 ("Running…")
                                : missing.isNotEmpty() ? "Next: " + missing
                                                       : "Ready. Check the keyswitch base, then click Start.",
                           juce::dontSendNotification);
    nextStepLabel.setColour (juce::Label::textColourId, missing.isNotEmpty() && ! busy ? juce::Colours::orange : juce::Colours::lightgreen);
}

juce::String MainComponent::missingStep() const
{
    if (master == nullptr)
        return "Load the plugin, then your instrument in its window.";
    if (collection.instruments.empty())
        return "Open a preset.";
    if (instrumentIndex < 0)
        return "Choose the instrument you loaded in the plugin.";
    if (std::none_of (rows.begin(), rows.end(), [] (auto& r) { return r.enabled; }))
        return "Select at least one articulation.";
    if (rrResetKeyEdit.isVisible() && rrResetKeyEdit.getText().trim().isNotEmpty() && parseNoteName (rrResetKeyEdit.getText().trim()) < 0)
        return "RR key isn't a note (e.g. C-1). Leave it empty if the player has none.";
    return {};
}

void MainComponent::log (const juce::String& line)
{
    logView.moveCaretToEnd();
    logView.insertTextAtCaret (line + "\n");

    // mirror to a file so a session can be followed from outside the app
    if (logFile == nullptr)
    {
        auto f = dataDir().getChildFile ("Logs/latest.log");
        f.getParentDirectory().createDirectory();
        f.deleteFile();
        logFile = std::make_unique<juce::FileOutputStream> (f);
    }
    if (logFile->openedOk())
    {
        logFile->writeText (juce::Time::getCurrentTime().formatted ("%H:%M:%S ") + line + "\n", false, false, nullptr);
        logFile->flush();
    }
}
