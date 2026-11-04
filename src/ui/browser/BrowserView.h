#pragma once

#include "library/Library.h"
#include "ui/browser/BrowserModel.h"
#include "ui/feed/EngineStateFeed.h"
#include "ui/widgets/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace lattice
{
class AudioEngine;
namespace presets
{
class PresetLibrary;
}
} // namespace lattice

namespace lattice::ui
{

/** The browser (docs/05-m4-ui.md, 4e), on the left of the window:
    - a search box along the top (Ctrl+Alt+F) that searches everything as you type, ranked;
    - the sources down the side: Collections (colour tags, the first Favourites), Categories
      (Instruments, Drums, Audio Effects, MIDI Effects, Samples, Presets, Tunings) and Places (the
      User Library and folders you add);
    - the rows: devices open to show their presets, folders to show what's in them;
    - the preview strip along the bottom: selecting a sample plays it on the preview bus (and shows
      its waveform, with where it's playing), selecting an instrument preset plays a phrase.

    Enter or a double-click inserts what's selected where the Device view is looking (M3 hands-on
    finding 4: the same DeviceView::insert as everything else); rows drag onto tracks, devices,
    Samplers and pads. Message thread. */
class BrowserView final : public juce::Component, private EngineStateFeed::Listener
{
public:
    struct Actions
    {
        /** Enter or a double-click on a row: insert it (a drag description, ui::drag). */
        std::function<bool(const juce::String& description)> insert;
        /** "Add Folder...": asks for a folder (the OS chooser), then calls chosen with it. */
        std::function<void(std::function<void(const juce::File&)> chosen)> chooseFolder;
        /** The sample rate previews are rendered at (the audio device's). */
        std::function<double()> sampleRate;
    };

    BrowserView(library::Library& library, JobPool& jobs, AudioEngine& engine, EngineStateFeed& feed,
                presets::PresetLibrary& presets, juce::PropertiesFile* settings, Actions actions);
    ~BrowserView() override;

    BrowserModel& model() noexcept { return browser; }
    library::Library& library() noexcept { return lib; }

    /** The search box gets the keys (Ctrl+Alt+F). */
    void focusSearch();
    juce::TextEditor& searchBox();
    /** The list gets the keys. */
    void focusList();

    bool isPreviewOn() const noexcept { return previewing; }
    void setPreviewOn(bool on);
    bool isPreviewSynced() const noexcept { return synced; }
    void setPreviewSynced(bool on);

    /** Inserts the selected row (Enter): a folder opens or closes instead. */
    bool insertSelected();
    /** Plays the selected row on the preview bus, if preview is on and it's something to hear. */
    void previewSelected();
    /** Asks for a folder and adds it to the places. */
    void addPlace();

    /** Called after the rows, the selection or the source changed (a controller's Browse mode). */
    std::function<void()> onBrowserChanged;

    /** The drag description of an item (ui::drag), or empty for one that can't be dragged. */
    static juce::String descriptionFor(const library::Item& item);
    /** A collection's colour. */
    static juce::Colour collectionColour(int collection, const Theme& theme);

    /** The list's first visible row and how many fit (tests). */
    int firstVisibleRow() const;
    int visibleRows() const;
    /** Scrolls so the row is in view. */
    void scrollToRow(int row);
    /** The list (it takes the keys: arrows, Enter, Space). */
    juce::Component& listComponent() noexcept;
    /** The list's bounds within this component (tests drive clicks there). */
    juce::Rectangle<int> listBounds() const;
    juce::Rectangle<int> rowBounds(int row) const; // in this component's coordinates
    /** The sidebar row of a source (tests). */
    juce::Rectangle<int> sourceBounds(const BrowserModel::Source& source) const;

    /** What the preview strip shows: the waveform's sound (empty if none) and the playing frame. */
    bool showsWaveform() const;

    void resized() override;
    void paint(juce::Graphics&) override;
    void lookAndFeelChanged() override;

    class List;
    class Sidebar;
    class PreviewStrip;

private:
    void engineStateChanged(std::uint32_t aspects) override;
    void rowsChanged();
    void selectionChanged();
    void saveSettings();

    library::Library& lib;
    JobPool& jobs;
    AudioEngine& engine;
    EngineStateFeed& feed;
    presets::PresetLibrary& presetLibrary;
    juce::PropertiesFile* settings;
    Actions actions;
    BrowserModel browser;

    class SearchBox;
    std::unique_ptr<SearchBox> search;
    std::unique_ptr<Sidebar> sidebar;
    std::unique_ptr<juce::Viewport> sidebarView;
    std::unique_ptr<List> list;
    std::unique_ptr<PreviewStrip> strip;

    bool previewing = true;
    bool synced = false;
    std::uint64_t previewRequest = 0; // the newest selection's preview (late answers are dropped)
    std::uint64_t peaksJob = 0;
    std::shared_ptr<int> alive = std::make_shared<int>(0);
};

} // namespace lattice::ui
