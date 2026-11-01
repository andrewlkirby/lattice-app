#include "ui/shell/SetFileActions.h"

namespace lattice::ui
{

SetFileActions::SetFileActions(SessionController& s, juce::PropertiesFile& settingsFile, JobPool& pool, juce::File recoveryFolder,
                               Dialogs d)
    : session(s), settings(settingsFile), jobs(pool), recoveryCopy(std::move(recoveryFolder)), dialogs(std::move(d))
{
    session.set().state().addListener(this);
    shownTitle = title();
    startTimer(autosaveSeconds * 1000);
}

SetFileActions::~SetFileActions()
{
    stopTimer();
    session.set().state().removeListener(this);
    waitForAutosave();
}

juce::File SetFileActions::setsFolder() const
{
    const juce::File remembered(settings.getValue(lastFolderKey));
    if (remembered.isDirectory())
        return remembered;
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Lattice");
}

// --- Title --------------------------------------------------------------------------------------

juce::String SetFileActions::title() const
{
    const auto& file = session.currentFile();
    const auto name = file != juce::File() ? file.getFileNameWithoutExtension() : juce::String("Untitled");
    return name + (session.hasUnsavedChanges() ? "*" : "") + " - Lattice";
}

void SetFileActions::titleMayHaveChanged()
{
    const auto now = title();
    if (now == shownTitle)
        return;
    shownTitle = now;
    if (onTitleChanged)
        onTitleChanged();
}

void SetFileActions::handleAsyncUpdate()
{
    // Comparing the document with the saved one walks it: once per batch of changes, not per change.
    titleMayHaveChanged();
}

// --- Recent -------------------------------------------------------------------------------------

juce::StringArray SetFileActions::recentSets() const
{
    auto all = juce::StringArray::fromLines(settings.getValue(recentKey));
    all.removeEmptyStrings();
    juce::StringArray there;
    for (const auto& path : all)
        if (juce::File::isAbsolutePath(path) && juce::File(path).existsAsFile())
            there.add(path);
    return there;
}

void SetFileActions::addRecent(const juce::File& file)
{
    auto all = juce::StringArray::fromLines(settings.getValue(recentKey));
    all.removeEmptyStrings();
    all.removeString(file.getFullPathName());
    all.insert(0, file.getFullPathName());
    while (all.size() > maxRecent)
        all.remove(all.size() - 1);
    settings.setValue(recentKey, all.joinIntoString("\n"));
}

juce::PopupMenu SetFileActions::recentMenu()
{
    juce::PopupMenu menu;
    for (const auto& path : recentSets())
        menu.addItem(juce::File(path).getFileNameWithoutExtension() + "   (" + juce::File(path).getParentDirectory().getFullPathName() + ")",
                     [this, path] {
                         confirmDiscardingChanges([this, path](bool go) {
                             if (go)
                                 open(juce::File(path));
                         });
                     });
    if (menu.getNumItems() == 0)
        menu.addItem("No recent sets", false, false, nullptr);
    return menu;
}

// --- New, open, save ----------------------------------------------------------------------------

void SetFileActions::confirmDiscardingChanges(std::function<void(bool)> proceed)
{
    if (!session.hasUnsavedChanges() || !dialogs.ask)
    {
        proceed(true);
        return;
    }
    dialogs.ask("Unsaved changes", "Save the changes to " + title().upToLastOccurrenceOf(" - ", false, false).trimCharactersAtEnd("*") + " first?",
                {"Save", "Don't Save", "Cancel"}, [this, proceed = std::move(proceed)](int answer) {
                    if (answer == 0)
                        save([proceed](bool saved) { proceed(saved); });
                    else
                        proceed(answer == 1);
                });
}

void SetFileActions::newSet()
{
    confirmDiscardingChanges([this](bool go) {
        if (!go)
            return;
        session.newSet(); // its tracks get the Preferences' tuning (LiveSet::newTrackDefaults)
        recoveryCopy.clear();
        lastAutosaved = {};
        titleMayHaveChanged();
    });
}

void SetFileActions::open()
{
    confirmDiscardingChanges([this](bool go) {
        if (!go || !dialogs.chooseFile)
            return;
        dialogs.chooseFile("Open a Lattice set", setsFolder(), "*.lset", false, [this](const juce::File& file) {
            if (file.existsAsFile())
                open(file);
        });
    });
}

void SetFileActions::open(const juce::File& file)
{
    if (const auto result = session.loadSet(file); result.failed())
    {
        if (dialogs.tell)
            dialogs.tell("Can't open the set", result.getErrorMessage());
        return;
    }
    opened(file);
}

void SetFileActions::opened(const juce::File& file)
{
    settings.setValue(lastFolderKey, file.getParentDirectory().getFullPathName());
    addRecent(file);
    recoveryCopy.clear();
    lastAutosaved = {};
    titleMayHaveChanged();
    if (auto missing = setfiles::missing(session.set().state()); !missing.empty() && onMissingFiles)
        onMissingFiles(std::move(missing));
}

void SetFileActions::save(std::function<void(bool)> done)
{
    if (session.currentFile() == juce::File())
        saveAs(std::move(done));
    else
        saveTo(session.currentFile(), std::move(done));
}

void SetFileActions::saveAs(std::function<void(bool)> done)
{
    if (!dialogs.chooseFile)
    {
        if (done)
            done(false);
        return;
    }
    const auto folder = setsFolder();
    folder.createDirectory();
    const auto start = session.currentFile() != juce::File() ? session.currentFile() : folder.getChildFile("Untitled.lset");
    dialogs.chooseFile("Save the set", start, "*.lset", true, [this, done = std::move(done)](const juce::File& chosen) {
        if (chosen == juce::File())
        {
            if (done)
                done(false);
            return;
        }
        saveTo(chosen.withFileExtension("lset"), done);
    });
}

void SetFileActions::saveTo(const juce::File& file, std::function<void(bool)> done)
{
    const auto result = session.saveSet(file);
    if (result.failed())
    {
        if (dialogs.tell)
            dialogs.tell("Can't save the set", result.getErrorMessage());
    }
    else
    {
        settings.setValue(lastFolderKey, file.getParentDirectory().getFullPathName());
        addRecent(file);
        recoveryCopy.clear();
        lastAutosaved = {};
    }
    titleMayHaveChanged();
    if (done)
        done(result.wasOk());
}

void SetFileActions::collectAllAndSave()
{
    const auto collectInto = [this](bool saved) {
        if (!saved)
            return;
        session.undoManager().beginNewTransaction("Collect All and Save");
        const auto result = setfiles::collect(session.set(), session.currentFile().getParentDirectory());
        saveTo(session.currentFile(), [this, result](bool ok) {
            if (!ok || !dialogs.tell)
                return;
            juce::String message = result.copied == 0 ? juce::String("Every file the set uses is in its folder already.")
                                                      : juce::String(result.copied) + (result.copied == 1 ? " file" : " files") +
                                                            " copied into " + session.currentFile().getParentDirectory().getChildFile("Samples").getFullPathName() + ".";
            if (!result.failed.isEmpty())
                message << "\n\nNot copied:\n" << result.failed.joinIntoString("\n");
            dialogs.tell("Collect All and Save", message);
        });
    };
    // A set with no file yet needs a folder first.
    if (session.currentFile() == juce::File())
        saveAs(collectInto);
    else
        collectInto(true);
}

// --- Autosave -----------------------------------------------------------------------------------

void SetFileActions::autosaveNow()
{
    if (!session.hasUnsavedChanges() || writing->load() > 0)
        return;
    if (lastAutosaved.isValid() && lastAutosaved.isEquivalentTo(session.set().state()))
        return; // nothing new since the last copy
    lastAutosaved = session.set().state().createCopy();
    // The document is read here, on the message thread; the disk is the job's.
    std::shared_ptr<juce::XmlElement> xml = session.set().toXml();
    if (xml == nullptr)
        return;
    const auto original = session.currentFile();
    ++*writing;
    jobs.submit("Autosaving", [copy = recoveryCopy, xml, original, w = writing](JobPool::Context&) {
        copy.write(*xml, original);
        --*w;
    });
}

void SetFileActions::waitForAutosave()
{
    for (int i = 0; i < 1000 && writing->load() > 0; ++i)
        juce::Thread::sleep(5);
}

void SetFileActions::offerRecovery()
{
    if (!recoveryCopy.exists() || !dialogs.ask)
        return;
    const auto original = recoveryCopy.original();
    const auto name = original != juce::File() ? original.getFileNameWithoutExtension() : juce::String("an unsaved set");
    dialogs.ask("Restore your work?",
                "Lattice didn't close properly. A copy of " + name + " from " + recoveryCopy.time().toString(true, true, false) +
                    " was kept. Open it?",
                {"Restore", "Discard"}, [this, original](int answer) {
                    if (answer == 0)
                    {
                        const auto result = session.restoreSet(recoveryCopy.setFile(), original);
                        if (result.failed() && dialogs.tell)
                            dialogs.tell("Can't restore the set", result.getErrorMessage());
                        lastAutosaved = {};
                        titleMayHaveChanged();
                        if (result.wasOk())
                        {
                            if (auto missing = setfiles::missing(session.set().state()); !missing.empty() && onMissingFiles)
                                onMissingFiles(std::move(missing));
                            return; // the copy stays until the set is saved: a crash now loses nothing
                        }
                    }
                    recoveryCopy.clear();
                });
}

void SetFileActions::quitting()
{
    stopTimer();
    waitForAutosave();
    recoveryCopy.clear();
}

} // namespace lattice::ui
