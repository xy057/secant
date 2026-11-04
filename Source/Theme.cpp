#include "Theme.h"

namespace ui
{
namespace
{
juce::Colour hex (juce::uint32 rgb) { return juce::Colour (0xff000000 | rgb); }

const Palette lightPalette {
    hex (0xffffff), // background
    hex (0xf3f3f1), // control
    hex (0xebebe8), // controlHover
    hex (0xe2e2de), // controlDown
    hex (0xe3e3df), // border
    hex (0xf0f0ed), // divider
    hex (0x1f1f1e), // text
    hex (0x77776f), // textSecondary
    hex (0xa9a9a2), // textTertiary
    hex (0x262625), // ink
    hex (0xffffff), // inkText
    hex (0xeef1f5), // selection
    hex (0x5b7ea6), // focus
    hex (0xb0631a), // warn
    hex (0x3f7d5a), // ok
    hex (0xf8f8f6), // logBackground
};

const Palette darkPalette {
    hex (0x19191a), // background
    hex (0x262628), // control
    hex (0x2e2e30), // controlHover
    hex (0x363639), // controlDown
    hex (0x303033), // border
    hex (0x232325), // divider
    hex (0xe8e8e5), // text
    hex (0x96968f), // textSecondary
    hex (0x65655f), // textTertiary
    hex (0xe8e8e5), // ink
    hex (0x19191a), // inkText
    hex (0x2a2f37), // selection
    hex (0x86a6cc), // focus
    hex (0xdba062), // warn
    hex (0x84bf99), // ok
    hex (0x1e1e20), // logBackground
};

bool darkActive = false;

constexpr float radius = 6.0f;
} // namespace

const Palette& colours() { return darkActive ? darkPalette : lightPalette; }
bool isDark() { return darkActive; }

juce::Font font (float size, bool bold)
{
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
}

juce::Font monoFont (float size)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size, juce::Font::plain));
}

// ============================================================================ palette

Theme::Theme()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "Secant";
    o.filenameSuffix = ".settings";
    o.folderName = "Secant";
    o.osxLibrarySubFolder = "Application Support";
    settings = std::make_unique<juce::PropertiesFile> (o);
    mode = (Mode) juce::jlimit (0, 2, settings->getIntValue ("appearance", 0));
    setNativeAppearance ((int) mode);

    darkActive = mode == Mode::dark || (mode == Mode::system && juce::Desktop::getInstance().isDarkModeActive());
    applyPalette();
    juce::Desktop::getInstance().addDarkModeSettingListener (this);
}

void Theme::setMode (Mode m)
{
    if (m == mode)
        return;
    mode = m;
    settings->setValue ("appearance", (int) mode);
    settings->saveIfNeeded();
    setNativeAppearance ((int) mode);
    update();
}

void Theme::update()
{
    setDark (mode == Mode::dark || (mode == Mode::system && juce::Desktop::getInstance().isDarkModeActive()));
}

#if ! JUCE_MAC
void setNativeAppearance (int) {}
#endif

Theme::~Theme()
{
    juce::Desktop::getInstance().removeDarkModeSettingListener (this);
}

void Theme::setDark (bool dark)
{
    if (dark == darkActive)
        return;
    darkActive = dark;
    applyPalette();
    auto& desktop = juce::Desktop::getInstance();
    for (int i = 0; i < desktop.getNumComponents(); ++i)
        if (auto* c = desktop.getComponent (i))
        {
            c->sendLookAndFeelChange();
            c->repaint();
        }
}

void Theme::applyPalette()
{
    using namespace juce;
    const auto& p = ui::colours();
    setColourScheme ({ p.background, p.background, p.background, p.border, p.text, p.ink, p.text, p.selection, p.text });

    const auto none = Colours::transparentBlack;
    const std::initializer_list<std::pair<int, Colour>> ids {
        { ResizableWindow::backgroundColourId, p.background },
        { DocumentWindow::textColourId, p.text },

        { Label::textColourId, p.text },
        { Label::backgroundColourId, none },
        { Label::outlineColourId, none },
        { Label::textWhenEditingColourId, p.text },
        { Label::backgroundWhenEditingColourId, p.background },
        { Label::outlineWhenEditingColourId, none },

        { TextButton::buttonColourId, p.control },
        { TextButton::buttonOnColourId, p.controlDown },
        { TextButton::textColourOffId, p.text },
        { TextButton::textColourOnId, p.text },
        { ToggleButton::textColourId, p.text },
        { ToggleButton::tickColourId, p.inkText },
        { ToggleButton::tickDisabledColourId, p.textTertiary },

        { ComboBox::backgroundColourId, p.control },
        { ComboBox::textColourId, p.text },
        { ComboBox::outlineColourId, none },
        { ComboBox::buttonColourId, p.control },
        { ComboBox::arrowColourId, p.textSecondary },
        { ComboBox::focusedOutlineColourId, p.focus },

        { PopupMenu::backgroundColourId, p.background },
        { PopupMenu::textColourId, p.text },
        { PopupMenu::headerTextColourId, p.textSecondary },
        { PopupMenu::highlightedBackgroundColourId, p.selection },
        { PopupMenu::highlightedTextColourId, p.text },

        { TextEditor::backgroundColourId, none }, // filled (rounded) by fillTextEditorBackground
        { TextEditor::textColourId, p.text },
        { TextEditor::highlightColourId, p.focus.withAlpha (0.25f) },
        { TextEditor::highlightedTextColourId, p.text },
        { TextEditor::outlineColourId, none },
        { TextEditor::focusedOutlineColourId, p.focus },
        { TextEditor::shadowColourId, none },
        { CaretComponent::caretColourId, p.text },

        { ListBox::backgroundColourId, p.background },
        { ListBox::outlineColourId, none },
        { ListBox::textColourId, p.text },
        { TableHeaderComponent::textColourId, p.textSecondary },
        { TableHeaderComponent::backgroundColourId, p.background },
        { TableHeaderComponent::outlineColourId, p.border },
        { TableHeaderComponent::highlightColourId, p.control },

        { ScrollBar::backgroundColourId, none },
        { ScrollBar::trackColourId, none },
        { ScrollBar::thumbColourId, p.textSecondary },

        { TooltipWindow::backgroundColourId, p.ink },
        { TooltipWindow::textColourId, p.inkText },
        { TooltipWindow::outlineColourId, none },

        { GroupComponent::outlineColourId, p.border },
        { GroupComponent::textColourId, p.text },

        { ProgressBar::backgroundColourId, p.control },
        { ProgressBar::foregroundColourId, p.ink },

        { Slider::textBoxTextColourId, p.text },
        { Slider::textBoxBackgroundColourId, p.control },
        { Slider::textBoxHighlightColourId, p.focus.withAlpha (0.25f) },
        { Slider::textBoxOutlineColourId, none },

        { AlertWindow::backgroundColourId, p.background },
        { AlertWindow::textColourId, p.text },
        { AlertWindow::outlineColourId, p.border },
    };
    for (auto& [id, c] : ids)
        setColour (id, c);
}

// ============================================================================ buttons

juce::Font Theme::getTextButtonFont (juce::TextButton&, int) { return font (13.0f); }

void Theme::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool highlighted, bool down)
{
    const auto& p = ui::colours();
    if (b.getProperties()[segment])
    {
        const auto& p = ui::colours();
        if (b.getToggleState() || highlighted)
        {
            g.setColour (b.getToggleState() ? p.control : p.control.withAlpha (0.5f));
            g.fillRoundedRectangle (b.getLocalBounds().toFloat(), 5.0f);
        }
        return;
    }
    const bool isPrimary = b.getProperties()[primary];
    auto fill = isPrimary ? p.ink : down ? p.controlDown : highlighted ? p.controlHover : p.control;
    if (isPrimary && b.isEnabled() && (down || highlighted))
        fill = fill.interpolatedWith (p.background, down ? 0.22f : 0.12f);
    if (! b.isEnabled())
        fill = isPrimary ? fill.interpolatedWith (p.background, 0.7f) : fill.interpolatedWith (p.background, 0.4f);

    // buttons joined to a neighbour (a slider's - / +) keep that side square
    const bool l = b.isConnectedOnLeft(), r = b.isConnectedOnRight(), t = b.isConnectedOnTop(), bt = b.isConnectedOnBottom();
    juce::Path shape;
    const auto area = b.getLocalBounds().toFloat();
    shape.addRoundedRectangle (area.getX(), area.getY(), area.getWidth(), area.getHeight(), radius, radius,
                               ! (l || t), ! (r || t), ! (l || bt), ! (r || bt));
    g.setColour (fill);
    g.fillPath (shape);
}

void Theme::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    const auto& p = ui::colours();
    const bool isPrimary = b.getProperties()[primary];
    if (b.getProperties()[segment])
    {
        g.setColour (b.getToggleState() ? p.text : p.textSecondary);
        g.setFont (font (12.0f));
        g.drawFittedText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred, 1);
        return;
    }
    g.setColour (isPrimary ? p.inkText : b.isEnabled() ? p.text : p.textTertiary);
    g.setFont (font (13.0f, isPrimary));
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (6, 0), juce::Justification::centred, 1);
}

void Theme::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool down)
{
    constexpr float box = 16.0f;
    const auto bounds = b.getLocalBounds().toFloat();
    const bool hasText = b.getButtonText().isNotEmpty();
    // without text (table cells) the box is centred
    const float x = hasText ? 1.0f : std::floor ((bounds.getWidth() - box) * 0.5f);
    const float y = std::floor ((bounds.getHeight() - box) * 0.5f);
    drawTickBox (g, b, x, y, box, box, b.getToggleState(), b.isEnabled(), highlighted, down);
    if (hasText)
    {
        g.setColour (b.isEnabled() ? ui::colours().text : ui::colours().textTertiary);
        g.setFont (font (13.0f));
        g.drawFittedText (b.getButtonText(), bounds.withTrimmedLeft (box + 8.0f).toNearestInt(), juce::Justification::centredLeft, 1);
    }
}

void Theme::drawTickBox (juce::Graphics& g, juce::Component&, float x, float y, float w, float h, bool ticked, bool enabled,
                         bool highlighted, bool)
{
    const auto& p = ui::colours();
    const juce::Rectangle<float> r (x, y, w, h);
    if (ticked)
    {
        g.setColour (enabled ? p.ink : p.ink.interpolatedWith (p.background, 0.6f));
        g.fillRoundedRectangle (r, 4.0f);
        juce::Path tick;
        tick.startNewSubPath (r.getX() + w * 0.27f, r.getY() + h * 0.52f);
        tick.lineTo (r.getX() + w * 0.43f, r.getY() + h * 0.68f);
        tick.lineTo (r.getX() + w * 0.74f, r.getY() + h * 0.34f);
        g.setColour (p.inkText);
        g.strokePath (tick, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    else
    {
        g.setColour (p.background);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (! enabled ? p.border : highlighted ? p.textSecondary : p.textTertiary);
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
    }
}

// ============================================================================ combo boxes, menus

void Theme::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto& p = ui::colours();
    const auto area = juce::Rectangle<float> (0, 0, (float) width, (float) height);
    const bool isCell = box.getProperties()[cell];
    if (! isCell)
    {
        g.setColour (box.isEnabled() ? p.control : p.control.interpolatedWith (p.background, 0.4f));
        g.fillRoundedRectangle (area, radius); // no focus ring: the first box gets focus at launch and would look selected
    }

    // chevron
    const float cx = (float) width - (isCell ? 9.0f : 13.0f), cy = (float) height * 0.5f;
    juce::Path chevron;
    chevron.startNewSubPath (cx - 3.5f, cy - 1.5f);
    chevron.lineTo (cx, cy + 2.0f);
    chevron.lineTo (cx + 3.5f, cy - 1.5f);
    g.setColour (box.isEnabled() ? p.textSecondary : p.textTertiary);
    g.strokePath (chevron, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font Theme::getComboBoxFont (juce::ComboBox&) { return font (13.0f); }

void Theme::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    const bool isCell = box.getProperties()[cell];
    const int left = isCell ? 1 : 6;
    label.setBounds (left, 1, box.getWidth() - left - (isCell ? 16 : 22), box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

void Theme::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (ui::colours().background);
    g.setColour (ui::colours().border);
    g.drawRect (0, 0, width, height, 1);
}

juce::Font Theme::getPopupMenuFont() { return font (13.5f); }

// ============================================================================ text

juce::Font Theme::getLabelFont (juce::Label& l) { return l.getFont(); }

void Theme::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& ed)
{
    const auto& p = ui::colours();
    const auto area = juce::Rectangle<float> (0, 0, (float) width, (float) height);
    if (ed.isMultiLine()) // the log
    {
        g.setColour (p.logBackground);
        g.fillRoundedRectangle (area, 8.0f);
        return;
    }
    // inside a table cell (a Label being edited) the field fills the cell
    if (dynamic_cast<juce::Label*> (ed.getParentComponent()) != nullptr)
    {
        g.setColour (p.background);
        g.fillRect (area);
        return;
    }
    g.setColour (ed.isEnabled() ? p.control : p.control.interpolatedWith (p.background, 0.4f));
    g.fillRoundedRectangle (area, radius);
}

void Theme::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& ed)
{
    if (! ed.isEnabled() || ed.isReadOnly() || ! ed.hasKeyboardFocus (true))
        return;
    const bool inCell = dynamic_cast<juce::Label*> (ed.getParentComponent()) != nullptr;
    g.setColour (ui::colours().focus.withAlpha (0.7f));
    const auto area = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.75f);
    if (inCell)
        g.drawRect (area, 1.5f);
    else
        g.drawRoundedRectangle (area, radius, 1.5f);
}

namespace
{
juce::TextLayout tooltipLayout (const juce::String& text, juce::Colour colour)
{
    juce::AttributedString s;
    s.setJustification (juce::Justification::centredLeft);
    s.append (text, font (12.5f), colour);
    juce::TextLayout tl;
    tl.createLayoutWithBalancedLineLengths (s, 380.0f);
    return tl;
}
} // namespace

juce::Rectangle<int> Theme::getTooltipBounds (const juce::String& tipText, juce::Point<int> pos, juce::Rectangle<int> parent)
{
    const auto tl = tooltipLayout (tipText, juce::Colours::black);
    const int w = (int) (tl.getWidth() + 22.0f), h = (int) (tl.getHeight() + 14.0f);
    return juce::Rectangle<int> (pos.x > parent.getCentreX() ? pos.x - (w + 12) : pos.x + 20,
                                 pos.y > parent.getCentreY() ? pos.y - (h + 6) : pos.y + 6, w, h)
        .constrainedWithin (parent);
}

void Theme::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    const auto& p = ui::colours();
    g.fillAll (p.ink);
    tooltipLayout (text, p.inkText).draw (g, juce::Rectangle<float> ((float) width, (float) height).reduced (11.0f, 7.0f));
}

// ============================================================================ containers

// "1. Plugin: detail" -> a faint step number, the bold name, the detail in grey
void Theme::drawGroupComponentOutline (juce::Graphics& g, int w, int h, const juce::String& text, const juce::Justification&,
                                       juce::GroupComponent&)
{
    const auto& p = ui::colours();
    g.setColour (p.border);
    g.drawRoundedRectangle (juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f), 10.0f, 1.0f);

    auto title = text;
    juce::String step, detail;
    if (title.length() > 2 && juce::CharacterFunctions::isDigit (title[0]) && title.substring (1, 3) == ". ")
    {
        step = title.substring (0, 1);
        title = title.substring (3);
    }
    if (const int colon = title.indexOf (": "); colon > 0)
    {
        detail = title.substring (colon + 2);
        title = title.substring (0, colon);
    }

    float x = 16.0f;
    constexpr float y = 11.0f, lineH = 18.0f;
    if (step.isNotEmpty())
    {
        const auto f = font (12.0f);
        g.setFont (f);
        g.setColour (p.textTertiary);
        g.drawText (step, juce::Rectangle<float> (x, y, 12.0f, lineH), juce::Justification::centredLeft);
        x += 14.0f;
    }
    const auto titleFont = font (13.0f, true);
    g.setFont (titleFont);
    g.setColour (p.text);
    const float tw = juce::GlyphArrangement::getStringWidth (titleFont, title);
    g.drawText (title, juce::Rectangle<float> (x, y, tw + 2.0f, lineH), juce::Justification::centredLeft);
    x += tw + 12.0f;
    if (detail.isNotEmpty())
    {
        g.setFont (font (12.0f));
        g.setColour (p.textSecondary);
        g.drawText (detail, juce::Rectangle<float> (x, y, (float) w - x - 16.0f, lineH), juce::Justification::centredLeft, true);
    }
}

void Theme::drawTableHeaderBackground (juce::Graphics& g, juce::TableHeaderComponent& header)
{
    const auto& p = ui::colours();
    g.fillAll (p.background);
    g.setColour (p.border);
    g.fillRect (0, header.getHeight() - 1, header.getWidth(), 1);
}

void Theme::drawTableHeaderColumn (juce::Graphics& g, juce::TableHeaderComponent&, const juce::String& columnName, int,
                                   int width, int height, bool isMouseOver, bool, int)
{
    const auto& p = ui::colours();
    if (isMouseOver)
    {
        g.setColour (p.divider);
        g.fillRect (width - 1, 8, 1, height - 16);
    }
    g.setColour (p.textSecondary);
    g.setFont (font (12.0f));
    g.drawFittedText (columnName, 6, 0, width - 10, height - 1, juce::Justification::centredLeft, 1);
}

void Theme::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height, bool vertical,
                           int thumbStart, int thumbSize, bool isMouseOver, bool isMouseDown)
{
    const auto thumb = (vertical ? juce::Rectangle<int> (x, thumbStart, width, thumbSize)
                                 : juce::Rectangle<int> (thumbStart, y, thumbSize, height))
                           .toFloat()
                           .reduced (vertical ? 2.5f : 1.0f, vertical ? 1.0f : 2.5f);
    if (thumb.isEmpty())
        return;
    g.setColour (ui::colours().textSecondary.withAlpha (isMouseOver || isMouseDown ? 0.5f : 0.25f));
    g.fillRoundedRectangle (thumb, juce::jmin (thumb.getWidth(), thumb.getHeight()) * 0.5f);
}

// a thin line; indeterminate progress slides a segment along it
void Theme::drawProgressBar (juce::Graphics& g, juce::ProgressBar&, int width, int height, double progress, const juce::String&)
{
    const auto& p = ui::colours();
    constexpr float thickness = 4.0f;
    const juce::Rectangle<float> track (0.0f, std::round (((float) height - thickness) * 0.5f), (float) width, thickness);
    g.setColour (p.control);
    g.fillRoundedRectangle (track, thickness * 0.5f);
    g.setColour (p.ink);
    if (progress >= 0.0 && progress <= 1.0)
    {
        if (progress > 0.0)
            g.fillRoundedRectangle (track.withWidth (juce::jmax (thickness, track.getWidth() * (float) progress)), thickness * 0.5f);
        return;
    }
    const float phase = (float) std::fmod (juce::Time::getMillisecondCounter() / 1400.0, 1.0);
    const float seg = track.getWidth() * 0.25f;
    const float x0 = (track.getWidth() + seg) * phase - seg;
    const auto part = track.withX (x0).withWidth (seg).getIntersection (track);
    if (! part.isEmpty())
        g.fillRoundedRectangle (part, thickness * 0.5f);
}
} // namespace ui
