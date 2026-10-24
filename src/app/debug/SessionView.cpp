#include "app/debug/SessionView.h"

namespace lattice::debugui
{
namespace
{
constexpr int transportHeight = 68;
constexpr int trackColumnWidth = 120;
constexpr int dividerHeight = 6;
constexpr auto lastSetFolderKey = "lastSetFolder";
constexpr auto detailHeightKey = "detailViewHeight";
} // namespace

/** Everything inside the scrolling viewport: the grid, with the mixer strips lined up under its
    columns, the master strip under the scene column and the return strips after it. */
class SessionView::Content final : public juce::Component
{
public:
    Content(SessionController& s, TuningRegistry& tunings, Selection& selection)
        : session(s), registry(tunings), grid(s, selection)
    {
        grid.setColumnWidth(trackColumnWidth);
        addAndMakeVisible(grid);
        rebuildStrips();
    }

    /** Called when a strip is clicked: show its chain. */
    std::function<void(ChainOwner)> onSelectChain;

    void rebuildStrips()
    {
        strips.clear();
        returns.clear();
        auto add = [this](MixerStrip::Kind kind, int index, ChainOwner owner) {
            auto strip = std::make_unique<MixerStrip>(session, registry, kind, index);
            strip->onSelect = [this, owner] {
                if (onSelectChain)
                    onSelectChain(owner);
            };
            addAndMakeVisible(*strip);
            return strip;
        };
        for (int t = 0; t < session.set().numTracks(); ++t)
            strips.push_back(add(MixerStrip::Kind::Track, t, {ChainOwner::Kind::Track, t}));
        master = add(MixerStrip::Kind::Master, -1, {ChainOwner::Kind::Master, 0});
        for (int r = 0; r < session.set().numReturns(); ++r)
            returns.push_back(add(MixerStrip::Kind::Return, r, {ChainOwner::Kind::Return, r}));
        stripHeight = MixerStrip::heightFor(session.set().numReturns());
        setSize(grid.preferredWidth() + static_cast<int>(returns.size()) * trackColumnWidth,
                grid.preferredHeight() + stripHeight);
        resized();
        highlight(shown);
    }

    /** Marks the strip whose chain the detail view shows. */
    void highlight(ChainOwner owner)
    {
        shown = owner;
        for (std::size_t t = 0; t < strips.size(); ++t)
            strips[t]->setHighlighted(owner.kind == ChainOwner::Kind::Track && owner.index == static_cast<int>(t));
        for (std::size_t r = 0; r < returns.size(); ++r)
            returns[r]->setHighlighted(owner.kind == ChainOwner::Kind::Return && owner.index == static_cast<int>(r));
        if (master != nullptr)
            master->setHighlighted(owner.kind == ChainOwner::Kind::Master);
    }

    void refresh()
    {
        for (auto& s : strips)
            s->refresh();
        for (auto& s : returns)
            s->refresh();
        master->refresh();
        grid.repaint();
    }

    void rebuildTuningMenus()
    {
        for (auto& s : strips)
            s->rebuildTuningMenu();
    }

    void tick()
    {
        grid.poll();
        // Taking the peaks resets them: while the Session view shows the meters instead, they're
        // left for it.
        if (!isShowing())
            return;
        for (auto& s : strips)
            s->updateMeter();
        for (auto& s : returns)
            s->updateMeter();
        master->updateMeter();
    }

    void resized() override
    {
        grid.setBounds(0, 0, grid.preferredWidth(), grid.preferredHeight());
        const int y = grid.getBottom();
        for (std::size_t t = 0; t < strips.size(); ++t)
            strips[t]->setBounds(static_cast<int>(t) * trackColumnWidth, y, trackColumnWidth, stripHeight);
        if (master != nullptr)
            master->setBounds(static_cast<int>(strips.size()) * trackColumnWidth, y,
                              SessionGrid::sceneColumnWidth, stripHeight);
        const int returnsX = static_cast<int>(strips.size()) * trackColumnWidth + SessionGrid::sceneColumnWidth;
        for (std::size_t r = 0; r < returns.size(); ++r)
            returns[r]->setBounds(returnsX + static_cast<int>(r) * trackColumnWidth, y, trackColumnWidth, stripHeight);
    }

    SessionGrid& sessionGrid() noexcept { return grid; }

private:
    SessionController& session;
    TuningRegistry& registry;
    SessionGrid grid;
    std::vector<std::unique_ptr<MixerStrip>> strips;
    std::vector<std::unique_ptr<MixerStrip>> returns;
    std::unique_ptr<MixerStrip> master;
    int stripHeight = MixerStrip::heightFor(0);
    ChainOwner shown;
};

SessionView::SessionView(SessionController& s, AudioEngine& engine, TuningRegistry& tunings,
                         juce::PropertiesFile& settingsFile)
    : session(s), registry(tunings), settings(settingsFile), transportBar(s), detail(s, engine, settingsFile)
{
    setOpaque(true);
    addAndMakeVisible(transportBar);
    transportBar.onNew = [this] {
        newSet();
    };
    transportBar.onOpen = [this] {
        openSet();
    };
    transportBar.onSave = [this] {
        saveSet();
    };
    transportBar.onSaveAs = [this] {
        saveSetAs();
    };
    transportBar.onImportMidi = [this] {
        if (!selected.sceneColumn)
            importMidi(selected.track, selected.scene);
    };
    transportBar.selectedScene = [this] {
        return selected.scene;
    };

    content = std::make_unique<Content>(session, registry, selected);
    content->sessionGrid().onSelectionChanged = [this] {
        selectionChanged();
    };
    content->onSelectChain = [this](ChainOwner owner) {
        if (owner.kind == ChainOwner::Kind::Track && owner.index != selected.track)
        {
            selected.track = owner.index;
            selected.sceneColumn = false;
            selectionChanged(); // shows the track's chain
        }
        else
            showChain(owner);
    };
    content->sessionGrid().onImportMidi = [this](int t, int sc) {
        importMidi(t, sc);
    };
    viewport.setViewedComponent(content.get(), false);
    viewport.setWantsKeyboardFocus(false);
    viewport.setScrollBarsShown(true, true);
    addAndMakeVisible(viewport);

    // The detail view below a divider, at the height it had last time.
    const double detailHeight = settings.getDoubleValue(detailHeightKey, 330.0);
    split.setItemLayout(0, 120, -1.0, -0.6);
    split.setItemLayout(1, dividerHeight, dividerHeight, dividerHeight);
    split.setItemLayout(2, 150, -0.85, -std::clamp(detailHeight / 900.0, 0.2, 0.8));
    addAndMakeVisible(divider);
    addAndMakeVisible(detail);
    showChain({ChainOwner::Kind::Track, selected.track});

    session.set().state().addListener(this);
    startTimerHz(refreshHz);
}

SessionView::~SessionView()
{
    session.set().state().removeListener(this);
}

void SessionView::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void SessionView::resized()
{
    auto area = getLocalBounds();
    transportBar.setBounds(area.removeFromTop(transportHeight));
    juce::Component* parts[] = {&viewport, &divider, &detail};
    split.layOutComponents(parts, 3, area.getX(), area.getY(), area.getWidth(), area.getHeight(), true, true);
    if (detail.getHeight() > 0)
        settings.setValue(detailHeightKey, detail.getHeight());
}

void SessionView::showChain(ChainOwner owner)
{
    detail.showChainOf(owner);
    content->highlight(owner);
}

void SessionView::timerCallback()
{
    if (needsRebuild)
        rebuild();
    if (needsRefresh)
    {
        needsRefresh = false;
        transportBar.refresh();
        content->refresh();
    }
    transportBar.update();
    content->tick();
}

void SessionView::rebuild()
{
    needsRebuild = false;
    needsRefresh = false;
    content->rebuildStrips();
    content->refresh();
    transportBar.refresh();
    moveSelection(0, 0); // clamp to what exists now
    // A deleted return (or a new set) may have taken the shown chain away.
    auto owner = detail.owner();
    if ((owner.kind == ChainOwner::Kind::Return && owner.index >= session.set().numReturns()) ||
        (owner.kind == ChainOwner::Kind::Track && owner.index >= session.set().numTracks()))
        owner = {ChainOwner::Kind::Track, selected.track};
    showChain(owner);
}

void SessionView::moveSelection(int dTrack, int dScene)
{
    const int tracks = session.set().numTracks();
    const int scenes = session.set().numScenes();
    if (dTrack > 0 && !selected.sceneColumn && selected.track + dTrack >= tracks)
        selected.sceneColumn = true;
    else if (dTrack < 0 && selected.sceneColumn)
    {
        selected.sceneColumn = false;
        selected.track = tracks - 1;
    }
    else if (!selected.sceneColumn)
        selected.track += dTrack;
    selected.track = juce::jlimit(0, std::max(0, tracks - 1), selected.track);
    selected.scene = juce::jlimit(0, std::max(0, scenes - 1), selected.scene + dScene);
    selectionChanged();
}

void SessionView::setSelection(const Selection& s)
{
    selected = s;
    moveSelection(0, 0); // clamps, repaints and follows the track
}

void SessionView::selectionChanged()
{
    content->sessionGrid().repaint();
    if (onSelectionChanged)
        onSelectionChanged();
    if (selected.track != lastSelectedTrack)
    {
        lastSelectedTrack = selected.track;
        showChain({ChainOwner::Kind::Track, selected.track});
        if (onSelectedTrackChanged)
            onSelectedTrackChanged();
    }
    // Keep the selection visible.
    const auto r = juce::Rectangle<int>(selected.sceneColumn ? session.set().numTracks() * trackColumnWidth
                                                             : selected.track * trackColumnWidth,
                                        SessionGrid::headerHeight + selected.scene * SessionGrid::rowHeight,
                                        trackColumnWidth, SessionGrid::rowHeight);
    const auto view = viewport.getViewArea();
    if (!view.contains(r))
        viewport.setViewPosition(
            juce::jlimit(0, std::max(0, content->getWidth() - view.getWidth()),
                         r.getX() < view.getX() ? r.getX() : r.getRight() - view.getWidth()),
            juce::jlimit(0, std::max(0, content->getHeight() - view.getHeight()),
                         r.getY() < view.getY() ? r.getY() : r.getBottom() - view.getHeight()));
}

bool SessionView::handleKey(const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    const int code = key.getKeyCode();
    auto& undo = session.undoManager();

    if (code == juce::KeyPress::spaceKey)
    {
        if (mods.isShiftDown())
            session.continuePlayback();
        else
            session.togglePlayStop();
        return true;
    }
    if (code == juce::KeyPress::F9Key)
    {
        session.setSessionRecord(!session.isSessionRecordOn(), selected.scene);
        return true;
    }
    if (code == juce::KeyPress::returnKey && !mods.isAnyModifierKeyDown())
    {
        if (selected.sceneColumn)
        {
            session.launchScene(selected.scene);
            if (session.set().selectNextSceneOnLaunch())
                moveSelection(0, 1);
        }
        else if (!enterHeld)
        {
            session.launchSlot(selected.track, selected.scene);
            enterHeld = true;
            enterSlot = selected;
        }
        return true;
    }
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey ||
        code == juce::KeyPress::rightKey)
    {
        moveSelection(code == juce::KeyPress::leftKey    ? -1
                      : code == juce::KeyPress::rightKey ? 1
                                                         : 0,
                      code == juce::KeyPress::upKey     ? -1
                      : code == juce::KeyPress::downKey ? 1
                                                        : 0);
        return true;
    }
    if ((code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) && !selected.sceneColumn)
    {
        undo.beginNewTransaction("Delete clip");
        session.set().deleteClip(selected.track, selected.scene);
        return true;
    }

    if (mods.isCommandDown())
    {
        const auto c = juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(code));
        if (c == 'z' && !mods.isShiftDown())
        {
            undo.undo();
            return true;
        }
        if (c == 'y' || (c == 'z' && mods.isShiftDown()))
        {
            undo.redo();
            return true;
        }
        if (c == 's')
        {
            if (mods.isShiftDown())
                saveSetAs();
            else
                saveSet();
            return true;
        }
        if (c == 'o')
        {
            openSet();
            return true;
        }
        if (c == 'n')
        {
            newSet();
            return true;
        }
        if (c == 'd' && !selected.sceneColumn)
        {
            undo.beginNewTransaction("Duplicate clip");
            session.set().duplicateClip(selected.track, selected.scene);
            return true;
        }
        if (c == 't')
        {
            undo.beginNewTransaction("Add track");
            session.set().addTrack(selected.track + 1);
            return true;
        }
        if (c == 'i')
        {
            undo.beginNewTransaction("Insert scene");
            session.set().addScene(selected.scene + 1);
            return true;
        }
    }
    return false;
}

void SessionView::handleKeyStateChange()
{
    if (enterHeld && !juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::returnKey))
    {
        enterHeld = false;
        session.releaseSlot(enterSlot.track, enterSlot.scene);
    }
}

// --- Files -------------------------------------------------------------------------------------------

juce::File SessionView::setsFolder() const
{
    juce::File remembered(settings.getValue(lastSetFolderKey));
    if (remembered.isDirectory())
        return remembered;
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Lattice");
}

void SessionView::showError(const juce::String& title, const juce::String& message)
{
    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                     .withIconType(juce::MessageBoxIconType::WarningIcon)
                                     .withTitle(title)
                                     .withMessage(message)
                                     .withButton("OK"),
                                 nullptr);
}

void SessionView::confirmDiscardingChanges(std::function<void(bool)> proceed)
{
    if (!session.hasUnsavedChanges())
    {
        proceed(true);
        return;
    }
    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                     .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                     .withTitle("Unsaved changes")
                                     .withMessage("Save the changes to this set first?")
                                     .withButton("Save")
                                     .withButton("Don't save")
                                     .withButton("Cancel"),
                                 [this, proceed = std::move(proceed)](int result) {
                                     if (result == 1)
                                         saveSet([proceed](bool saved) { proceed(saved); });
                                     else
                                         proceed(result == 2);
                                 });
}

void SessionView::newSet()
{
    confirmDiscardingChanges([this](bool proceed) {
        if (!proceed)
            return;
        const auto tuning = settings.getValue("tuningId");
        session.newSet();
        if (tuning.isNotEmpty() && registry.findById(tuning) != nullptr)
        {
            for (int t = 0; t < session.set().numTracks(); ++t)
                session.set().setTrackTuning(t, tuning);
            session.markClean();
        }
    });
}

void SessionView::openSet()
{
    confirmDiscardingChanges([this](bool proceed) {
        if (!proceed)
            return;
        chooser = std::make_unique<juce::FileChooser>("Open a Lattice set", setsFolder(), "*.lset");
        chooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc) {
                const auto file = fc.getResult();
                if (file == juce::File())
                    return;
                settings.setValue(lastSetFolderKey, file.getParentDirectory().getFullPathName());
                if (const auto result = session.loadSet(file); result.failed())
                    showError("Could not open the set", result.getErrorMessage());
            });
    });
}

void SessionView::saveSet(std::function<void(bool)> done)
{
    if (session.currentFile() == juce::File())
    {
        saveSetAs(std::move(done));
        return;
    }
    const auto result = session.saveSet(session.currentFile());
    if (result.failed())
        showError("Could not save the set", result.getErrorMessage());
    if (done)
        done(result.wasOk());
}

void SessionView::saveSetAs(std::function<void(bool)> done)
{
    const auto folder = setsFolder();
    if (!folder.exists())
        folder.createDirectory();
    const auto initial =
        session.currentFile() != juce::File() ? session.currentFile() : folder.getChildFile("Untitled.lset");
    chooser = std::make_unique<juce::FileChooser>("Save the set", initial, "*.lset");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                             juce::FileBrowserComponent::warnAboutOverwriting,
                         [this, done = std::move(done)](const juce::FileChooser& fc) {
                             auto file = fc.getResult();
                             if (file == juce::File())
                             {
                                 if (done)
                                     done(false);
                                 return;
                             }
                             file = file.withFileExtension("lset");
                             settings.setValue(lastSetFolderKey, file.getParentDirectory().getFullPathName());
                             const auto result = session.saveSet(file);
                             if (result.failed())
                                 showError("Could not save the set", result.getErrorMessage());
                             if (done)
                                 done(result.wasOk());
                         });
}

void SessionView::importMidi(int track, int scene)
{
    chooser = std::make_unique<juce::FileChooser>("Import a MIDI file", setsFolder(), "*.mid;*.midi");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this, track, scene](const juce::FileChooser& fc) {
                             const auto file = fc.getResult();
                             if (file == juce::File())
                                 return;
                             const auto result = session.importMidiFile(file, track, scene);
                             if (!result.ok())
                                 showError("Could not import " + file.getFileName(), result.error);
                         });
}

} // namespace lattice::debugui
