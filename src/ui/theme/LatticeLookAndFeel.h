#pragma once

#include "ui/theme/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lattice::ui
{

/** The look and feel of the whole UI: it holds the current Theme, and gives JUCE's own components
    (menus, combo boxes, text editors, scrollbars, dialogs, tooltips) the theme's colours.

    Lattice's widgets draw themselves from themeOf(*this), so the same widget drawn under two look
    and feels shows two themes (the snapshot tests rely on this). To change the theme, call
    setTheme() and then sendLookAndFeelChange() on the top-level component. */
class LatticeLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    explicit LatticeLookAndFeel(const Theme& initial = Theme::dark());

    void setTheme(const Theme& t);
    const Theme& theme() const noexcept { return current; }

    // A few of JUCE's own drawings, in the theme's shapes.
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& background, bool highlighted,
                              bool down) override;
    void drawComboBox(juce::Graphics&, int width, int height, bool down, int buttonX, int buttonY, int buttonW,
                      int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    void drawTextEditorOutline(juce::Graphics&, int width, int height, juce::TextEditor&) override;

private:
    void applyColours();

    Theme current;
};

/** The theme a component draws with: its look and feel's, or Dark if it isn't a Lattice one. */
const Theme& themeOf(const juce::Component& component);

} // namespace lattice::ui
