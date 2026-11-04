#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// The app's look: plain white (or near-black in dark mode), hairline borders, flat controls, one ink colour.
// Follows the system's light / dark setting while the app runs.
namespace ui
{
struct Palette
{
    juce::Colour background,   // window, cards, table
        control,               // buttons, combo boxes, text fields
        controlHover,
        controlDown,
        border,                // card and table outlines
        divider,               // table row separators
        text,
        textSecondary,         // labels, headers
        textTertiary,          // placeholders, disabled hints
        ink,                   // primary button, ticks, progress
        inkText,               // text on ink
        selection,             // selected table rows
        focus,
        warn,                  // needs attention (missing step, overridden keyswitch, doc issues)
        ok,
        logBackground;
};

const Palette& colours();          // the current palette (light until a Theme says otherwise)
bool isDark();

// fonts: the system UI face at a few fixed sizes
juce::Font font (float size, bool bold = false);
juce::Font monoFont (float size);

// Button property: getProperties().set (ui::primary, true) draws it filled with the ink colour
inline const juce::Identifier primary { "uiPrimary" };
// TextButton property: one part of a segmented switch (toggled = the chosen part)
inline const juce::Identifier segment { "uiSegment" };
// ComboBox property: a borderless box for table cells
inline const juce::Identifier cell { "uiCell" };

// macOS: the app's appearance (title bars, file dialogs); 0 = follow the system, 1 = light, 2 = dark. No-op elsewhere.
void setNativeAppearance (int mode);

class Theme : public juce::LookAndFeel_V4,
              private juce::DarkModeSettingListener
{
public:
    enum class Mode { system = 0, light, dark };

    Theme();
    ~Theme() override;

    // the user's choice, remembered in Secant.settings; `system` follows macOS / Windows
    Mode getMode() const { return mode; }
    void setMode (Mode);

    // buttons
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawTickBox (juce::Graphics&, juce::Component&, float x, float y, float w, float h, bool ticked, bool enabled,
                      bool highlighted, bool down) override;

    // combo boxes, menus
    void drawComboBox (juce::Graphics&, int width, int height, bool down, int, int, int, int, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    juce::Font getPopupMenuFont() override;

    // text
    juce::Font getLabelFont (juce::Label&) override;
    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;
    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;

    // containers
    void drawGroupComponentOutline (juce::Graphics&, int w, int h, const juce::String& text, const juce::Justification&,
                                    juce::GroupComponent&) override;
    void drawTableHeaderBackground (juce::Graphics&, juce::TableHeaderComponent&) override;
    void drawTableHeaderColumn (juce::Graphics&, juce::TableHeaderComponent&, const juce::String& columnName, int columnId,
                                int width, int height, bool isMouseOver, bool isMouseDown, int columnFlags) override;
    int getDefaultScrollbarWidth() override { return 10; }
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool vertical,
                        int thumbStart, int thumbSize, bool isMouseOver, bool isMouseDown) override;
    void drawProgressBar (juce::Graphics&, juce::ProgressBar&, int width, int height, double progress, const juce::String&) override;

private:
    void darkModeSettingChanged() override { update(); }
    void update();            // picks the palette for the mode, redraws every window when it changed
    void applyPalette();
    void setDark (bool dark);

    Mode mode = Mode::system;
    std::unique_ptr<juce::PropertiesFile> settings;
};
} // namespace ui
