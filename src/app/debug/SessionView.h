#pragma once

#include "app/debug/DetailView.h"
#include "app/debug/MixerStrip.h"
#include "app/debug/SessionGrid.h"
#include "app/debug/SessionUi.h"
#include "app/debug/TransportBar.h"
#include "engine/SessionController.h"
#include "tuning/TuningRegistry.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>
#include <vector>

namespace lattice::debugui
{

/** The debug session UI (docs/03-m2-engine-session.md, part 2e; docs/04-m3-devices.md, "3f"):
    transport bar, clip launcher grid, mixer (tracks, returns, master) and, below a divider, the
    detail view: the device list and the selected chain's devices. Every engine command can be
    reached from here with the mouse or a Live-style shortcut, so a whole session can be built,
    recorded, launched and saved with no hardware. Throwaway: replaced by the real UI in M4. */
class SessionView final : public juce::Component, private juce::Timer, private juce::ValueTree::Listener
{
public:
    static constexpr int refreshHz = 30;

    SessionView(SessionController& session, AudioEngine& engine, TuningRegistry& tunings,
                juce::PropertiesFile& settings);
    ~SessionView() override;

    const Selection& selection() const noexcept { return selected; }
    /** Called when the selected track changes (the play panel follows it). */
    std::function<void()> onSelectedTrackChanged;

    /** Handles the session shortcuts; returns false for keys it doesn't use. */
    bool handleKey(const juce::KeyPress& key);
    /** Polls for released keys (Enter releases a Gate clip). */
    void handleKeyStateChange();

    /** If the set has unsaved changes, asks whether to save them first; then calls proceed(true),
        or proceed(false) if the user cancelled. */
    void confirmDiscardingChanges(std::function<void(bool proceed)> proceed);

    void newSet();
    void openSet();
    void saveSet(std::function<void(bool saved)> done = {});
    void saveSetAs(std::function<void(bool saved)> done = {});
    void importMidi(int track, int scene);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    class Content;

    void timerCallback() override;
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { needsRefresh = true; }
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child) override
    {
        childChanged(parent, child);
    }
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int) override
    {
        childChanged(parent, child);
    }
    void valueTreeChildOrderChanged(juce::ValueTree& parent, int, int) override
    {
        childChanged(parent, parent);
    }
    void valueTreeRedirected(juce::ValueTree&) override { needsRebuild = true; }
    /** Tracks or scenes added, removed or moved (or a whole set loaded) rebuild the strips; anything
        else (clips, notes) only needs a repaint. */
    void childChanged(const juce::ValueTree& parent, const juce::ValueTree& child)
    {
        if (parent.hasType(ids::liveSet) || parent.hasType(ids::tracks) || parent.hasType(ids::scenes) ||
            parent.hasType(ids::returns) || child.hasType(ids::track) || child.hasType(ids::scene) ||
            child.hasType(ids::returnTrack))
            needsRebuild = true;
        else
            needsRefresh = true;
    }

    void rebuild();
    void showChain(ChainOwner owner);
    void moveSelection(int dTrack, int dScene);
    void selectionChanged();
    juce::File setsFolder() const;
    void showError(const juce::String& title, const juce::String& message);

    SessionController& session;
    TuningRegistry& registry;
    juce::PropertiesFile& settings;
    Selection selected;
    int lastSelectedTrack = -1;

    TransportBar transportBar;
    std::unique_ptr<Content> content;
    juce::Viewport viewport;
    DetailView detail;
    juce::StretchableLayoutManager split;
    juce::StretchableLayoutResizerBar divider{&split, 1, false};
    std::unique_ptr<juce::FileChooser> chooser;
    bool needsRefresh = false;
    bool needsRebuild = false;
    bool enterHeld = false;
    Selection enterSlot;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SessionView)
};

} // namespace lattice::debugui
