#include "ui/shell/DetailView.h"

#include "ui/theme/LatticeLookAndFeel.h"

#include <tuple>

namespace lattice::ui
{

DetailView::DetailView(ClipView& c, juce::Component& d) : clip(c), device(d)
{
    setTitle("Detail view");
    addAndMakeVisible(clip);
    addChildComponent(device);
}

void DetailView::show(Page p)
{
    shown = p;
    clip.setVisible(p == Page::clip);
    device.setVisible(p == Page::device);
    repaint();
}

void DetailView::resized()
{
    const auto& t = themeOf(*this);
    auto r = getLocalBounds();
    auto strip = r.removeFromTop(juce::roundToInt(t.fontSmall + t.spacing * 2.0f));
    const int tabWidth = juce::roundToInt(t.controlHeight * 3.0f);
    clipButton = strip.removeFromLeft(tabWidth);
    deviceButton = strip.removeFromLeft(tabWidth);
    clip.setBounds(r);
    device.setBounds(r);
}

void DetailView::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    g.fillAll(t.surface);
    const auto strip = getLocalBounds().withHeight(clipButton.getHeight());
    g.setColour(t.surfaceRaised);
    g.fillRect(strip);
    g.setFont(t.font(t.fontSmall));
    for (const auto& [r, name, on] : {std::tuple{clipButton, "Clip", shown == Page::clip}, std::tuple{deviceButton, "Device", shown == Page::device}})
    {
        if (on)
        {
            g.setColour(t.surface);
            g.fillRect(r);
            g.setColour(t.accent);
            g.fillRect(r.withHeight(juce::jmax(2, juce::roundToInt(t.stroke * 2.0f))));
        }
        g.setColour(on ? t.text : t.textDim);
        g.drawText(name, r, juce::Justification::centred, false);
    }
    g.setColour(t.border);
    g.fillRect(strip.getX(), strip.getBottom() - 1, strip.getWidth(), 1);
}

void DetailView::mouseDown(const juce::MouseEvent& e)
{
    if (clipButton.contains(e.getPosition()))
        show(Page::clip);
    else if (deviceButton.contains(e.getPosition()))
        show(Page::device);
}

} // namespace lattice::ui
