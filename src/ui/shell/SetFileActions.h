#pragma once

#include "core/JobPool.h"
#include "core/model/Recovery.h"
#include "core/model/SetFiles.h"
#include "engine/SessionController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <memory>

namespace lattice::ui
{

/** The set's files (docs/05-m4-ui.md, 4g): New, Open, Open Recent, Save, Save As, Collect All and
    Save, the question before unsaved changes are lost, the window's title, and autosave.

    - The title is "<name>* - Lattice" while there are unsaved changes ("Untitled" before the first
      save).
    - Autosave: every two minutes while there are unsaved changes, a recovery copy (RecoveryCopy) is
      written on the job pool (the document is copied on the message thread first). A save, a new
      set, an opened one and a normal quit clear it; one found at startup is offered back.
    - After a set opens, files it names that aren't there are reported (onMissingFiles).

    The dialogs are given (Dialogs), so tests answer them. Message thread only. */
class SetFileActions final : private juce::Timer, private juce::ValueTree::Listener, private juce::AsyncUpdater
{
public:
    struct Dialogs
    {
        /** A question with buttons; the answer is the button's index, or -1 if dismissed. */
        std::function<void(const juce::String& title, const juce::String& message, const juce::StringArray& buttons,
                           std::function<void(int)>)>
            ask;
        /** A file to open (save false) or to save as; an empty File if cancelled. */
        std::function<void(const juce::String& title, const juce::File& start, const juce::String& patterns, bool save,
                           std::function<void(const juce::File&)>)>
            chooseFile;
        std::function<void(const juce::String& title, const juce::String& message)> tell;
    };

    SetFileActions(SessionController& session, juce::PropertiesFile& settings, JobPool& jobs, juce::File recoveryFolder,
                   Dialogs dialogs);
    ~SetFileActions() override;

    /** Tests answer the questions themselves. */
    void setDialogs(Dialogs d) { dialogs = std::move(d); }

    void newSet();
    void open();
    void open(const juce::File& file);
    /** Save As if the set has no file yet. done(saved). */
    void save(std::function<void(bool)> done = {});
    void saveAs(std::function<void(bool)> done = {});
    /** Copies the files the set uses into its folder and saves (Save As first if it has none). */
    void collectAllAndSave();
    /** Asks to save unsaved changes first: proceed(true) to go on, false if cancelled. */
    void confirmDiscardingChanges(std::function<void(bool)> proceed);

    juce::StringArray recentSets() const; // most recent first, only those still there
    void addRecent(const juce::File& file);
    juce::PopupMenu recentMenu();
    static constexpr int maxRecent = 10;

    juce::String title() const;
    std::function<void()> onTitleChanged;
    /** Tests: the title's pending check, now. */
    void flushTitle() { handleUpdateNowIfNeeded(); }
    /** After a set opens with files that aren't there. */
    std::function<void(std::vector<setfiles::Reference>)> onMissingFiles;

    // --- Autosave --------------------------------------------------------------------------------
    static constexpr int autosaveSeconds = 120;
    /** Writes the recovery copy now if there are unsaved changes since the last one. */
    void autosaveNow();
    /** Tests: waits for a copy being written. */
    void waitForAutosave();
    const RecoveryCopy& recovery() const noexcept { return recoveryCopy; }
    /** At startup: if a recovery copy is there, offers to open it (and clears it either way). */
    void offerRecovery();
    /** A normal quit. */
    void quitting();

    static constexpr auto recentKey = "recentSets";
    static constexpr auto lastFolderKey = "lastSetFolder";
    juce::File setsFolder() const;

private:
    void timerCallback() override { autosaveNow(); }
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { triggerAsyncUpdate(); }
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { triggerAsyncUpdate(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override;
    void opened(const juce::File& file);
    void saveTo(const juce::File& file, std::function<void(bool)> done);
    void titleMayHaveChanged();

    SessionController& session;
    juce::PropertiesFile& settings;
    JobPool& jobs;
    RecoveryCopy recoveryCopy;
    Dialogs dialogs;
    juce::ValueTree lastAutosaved;
    std::shared_ptr<std::atomic<int>> writing = std::make_shared<std::atomic<int>>(0);
    juce::String shownTitle;
};

} // namespace lattice::ui
