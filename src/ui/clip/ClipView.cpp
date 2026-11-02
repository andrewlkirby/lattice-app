#include "ui/clip/ClipView.h"

#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{

ClipView::ClipView(SessionController& s, EngineStateFeed& feed, TuningRegistry& tunings, SessionSelectionModel& sel)
    : session(s), selection(sel), roll(s, feed, tunings), panel(s, roll)
{
    setTitle("Clip view");
    panelView.setViewedComponent(&panel, false);
    panelView.setScrollBarsShown(true, false);
    panelView.setWantsKeyboardFocus(false);
    addAndMakeVisible(panelView);
    addAndMakeVisible(roll);
    selection.addListener(this);
    follow();
}

ClipView::~ClipView()
{
    selection.removeListener(this);
    watchedSlot.removeListener(this);
}

EditTarget* ClipView::editTargetIfFocused()
{
    return roll.hasKeyboardFocus(true) && roll.hasClip() ? &roll : nullptr;
}

void ClipView::flushUpdates()
{
    handleUpdateNowIfNeeded();
    roll.flushUpdates();
}

void ClipView::handleAsyncUpdate()
{
    follow();
}

void ClipView::follow()
{
    auto& set = session.set();
    const auto s = selection.get();
    juce::ValueTree slot;
    if (s.kind == SessionSelection::Kind::slot && s.firstTrack() < set.numTracks() && s.firstScene() < set.numScenes())
        slot = set.slot(s.firstTrack(), s.firstScene());
    if (slot != watchedSlot)
    {
        watchedSlot.removeListener(this);
        watchedSlot = slot;
        watchedSlot.addListener(this);
    }
    const auto c = slot.getChildWithName(ids::clip);
    if (c != roll.clip())
    {
        roll.setClip(c);
        panel.setClip(c);
        resized();
    }
}

void ClipView::paint(juce::Graphics& g)
{
    g.fillAll(themeOf(*this).surface);
}

void ClipView::resized()
{
    const auto& t = themeOf(*this);
    auto r = getLocalBounds();
    if (!roll.hasClip())
    {
        panelView.setVisible(false);
        roll.setBounds(r);
        return;
    }
    panelView.setVisible(true);
    const int panelWidth = juce::roundToInt(t.controlHeight * 11.0f);
    auto left = r.removeFromLeft(panelWidth);
    r.removeFromLeft(1);
    panelView.setBounds(left);
    const int scrollbar = panelView.getScrollBarThickness();
    panel.setSize(left.getWidth() - (panel.preferredHeight() > left.getHeight() ? scrollbar : 0), juce::jmax(left.getHeight(), panel.preferredHeight()));
    roll.setBounds(r);
}

} // namespace lattice::ui
