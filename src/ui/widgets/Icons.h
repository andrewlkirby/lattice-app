#pragma once

#include "ui/theme/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lattice::ui
{

/** The UI's own icons (docs/05-m4-ui.md, "Theme and scaling": vector, our own), as paths in a
    unit square, so they're sharp at every zoom and drawn in the theme's colours. */
enum class Icon
{
    power,      // a device's on/off switch
    foldOpen,   // ▾ shown unfolded (click folds)
    foldClosed, // ▸ shown folded
    hotSwap,    // two arrows: swap the device for another
    save,       // an arrow into a tray: save as a preset
    allParams,  // three lines: show every parameter
    menu,       // ▾ a menu
    up,         // ▴ scroll up
    map,        // a macro mapped to a parameter
    plus,
    cross
};

/** The icon's path in the unit square (0..1 both ways). */
juce::Path iconPath(Icon icon);
/** Whether it's drawn by stroking its path (lines) rather than filling it. */
bool isStrokedIcon(Icon icon);
/** Draws an icon centred in `area`, in `colour`, with the theme's stroke. */
void drawIcon(juce::Graphics& g, Icon icon, juce::Rectangle<float> area, juce::Colour colour, const Theme& theme);

/** A small square button showing an icon, lit in the accent colour when its toggle state is on
    (Toggle draws text; this draws the icon). It never takes the keyboard focus, so Space stays
    Play/Stop. */
class IconButton final : public juce::Button
{
public:
    IconButton(const juce::String& name, Icon icon, const juce::String& tooltip);

    void setIcon(Icon i);
    Icon icon() const noexcept { return shown; }

protected:
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;

private:
    Icon shown;
};

} // namespace lattice::ui
