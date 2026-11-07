// The set's files (docs/05-m4-ui.md, 4g): paths saved relative to the set's folder, Collect All and
// Save (the set reopens from a moved folder), missing files found in the library and relinked,
// autosave and recovery after a crash, New/Open/Save with the unsaved-changes question, recent
// sets, the title, and the undo history.

#include "core/model/SetFiles.h"
#include "library/LibraryIndex.h"
#include "support/TestDevices.h"
#include "support/TestFiles.h"
#include "ui/shell/MainShell.h"

#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::ui;
using namespace lattice::test;

namespace
{
const juce::Identifier fileId{"file"};

/** A Sampler on track `t` naming `file`. */
juce::ValueTree addSampler(LiveSet& set, int t, const juce::File& file)
{
    auto device = set.insertDevice(LiveSet::chainOf(set.track(t)), -1, "lattice.sampler");
    device.getOrCreateChildWithName(ids::deviceState, nullptr).setProperty(fileId, file.getFullPathName(), nullptr);
    return device;
}

juce::File fileOf(const juce::ValueTree& device)
{
    return juce::File(device.getChildWithName(ids::deviceState)[fileId].toString());
}

/** Answers for the file actions' dialogs, recorded. */
struct Answers
{
    SetFileActions::Dialogs dialogs()
    {
        SetFileActions::Dialogs d;
        d.ask = [this](const juce::String& title, const juce::String&, const juce::StringArray&, std::function<void(int)> answer) {
            asked.add(title);
            answer(button);
        };
        d.chooseFile = [this](const juce::String&, const juce::File&, const juce::String&, bool, std::function<void(const juce::File&)> chosen) {
            ++chooses;
            chosen(choose);
        };
        d.tell = [this](const juce::String& title, const juce::String& message) { told.add(title + ": " + message); };
        return d;
    }
    int button = 0;
    juce::File choose;
    juce::StringArray asked, told;
    int chooses = 0;
};

struct Session
{
    Session()
    {
        settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lattice-files-" + juce::Uuid().toString() + ".settings");
        juce::PropertiesFile::Options options;
        options.applicationName = "LatticeTests";
        settings = std::make_unique<juce::PropertiesFile>(settingsFile, options);
        files = std::make_unique<SetFileActions>(session, *settings, jobs, recoveryFolder.folder, answers.dialogs());
    }
    ~Session()
    {
        files.reset();
        settings.reset();
        settingsFile.deleteFile();
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings};
    JobPool jobs{1};
    TempFolder recoveryFolder{"lattice-recovery"};
    juce::File settingsFile;
    std::unique_ptr<juce::PropertiesFile> settings;
    Answers answers;
    std::unique_ptr<SetFileActions> files;
};
} // namespace

TEST_CASE("Set files: paths inside the set's folder are saved relative to it", "[ui][files]")
{
    TempFolder place("lattice-set");
    REQUIRE(writeWav(place / "Samples/Kick.wav", 0.1));
    TempFolder elsewhere("lattice-elsewhere");
    REQUIRE(writeWav(elsewhere / "Hat.wav", 0.1));

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings};
    auto& set = session.set();
    addSampler(set, 0, place / "Samples/Kick.wav");
    addSampler(set, 1, elsewhere / "Hat.wav");
    REQUIRE(session.saveSet(place / "Song.lset").wasOk());

    const auto text = (place / "Song.lset").loadFileAsString();
    CHECK(text.contains("file=\"Samples/Kick.wav\""));
    CHECK(text.contains((elsewhere / "Hat.wav").getFullPathName())); // outside the folder: in full
    CHECK(!text.contains((place / "Samples/Kick.wav").getFullPathName()));

    // Loaded, the paths are full again.
    REQUIRE(session.loadSet(place / "Song.lset").wasOk());
    const auto refs = setfiles::references(session.set().state());
    REQUIRE(refs.size() == 2);
    CHECK(refs[0].file == place / "Samples/Kick.wav");
    CHECK(refs[1].file == elsewhere / "Hat.wav");
}

TEST_CASE("Set files: Collect All and Save copies every file, and the set reopens from a moved folder", "[ui][files]")
{
    TempFolder sources("lattice-sources");
    REQUIRE(writeWav(sources / "a/Kick.wav", 0.1));
    REQUIRE(writeWav(sources / "b/Kick.wav", 0.2)); // the same name, other content
    REQUIRE(writeWav(sources / "Snare.wav", 0.1));
    TempFolder project("lattice-project");

    Session s;
    s.session.newSet(6, 1);
    auto& set = s.session.set();
    const auto kickA = addSampler(set, 0, sources / "a/Kick.wav");
    const auto kickB = addSampler(set, 1, sources / "b/Kick.wav");
    addSampler(set, 2, sources / "Snare.wav");
    addSampler(set, 3, sources / "Snare.wav"); // used twice, copied once
    addSampler(set, 4, sources / "Gone.wav");  // missing: reported

    s.answers.choose = project / "Song.lset";
    s.files->collectAllAndSave();
    CHECK(s.answers.chooses == 1); // Save As first: no folder yet
    REQUIRE((project / "Song.lset").existsAsFile());
    CHECK((project / "Samples/Kick.wav").existsAsFile());
    CHECK((project / "Samples/Kick 2.wav").existsAsFile());
    CHECK((project / "Samples/Snare.wav").existsAsFile());
    CHECK(project.folder.getChildFile("Samples").findChildFiles(juce::File::findFiles, false).size() == 3);
    CHECK(fileOf(kickA) == project / "Samples/Kick.wav");
    CHECK(fileOf(kickB) == project / "Samples/Kick 2.wav");
    REQUIRE(s.answers.told.size() == 1);
    CHECK(s.answers.told[0].contains("3 files copied"));
    CHECK(s.answers.told[0].contains("Gone.wav"));
    CHECK(!s.session.hasUnsavedChanges());

    // Move the whole folder: the set finds its samples in the new place.
    TempFolder moved("lattice-moved");
    REQUIRE(project.folder.copyDirectoryTo(moved / "Song"));
    project.folder.deleteRecursively();
    s.files->open(moved / "Song/Song.lset");
    const auto refs = setfiles::references(s.session.set().state());
    REQUIRE(refs.size() == 5);
    for (int i = 0; i < 4; ++i)
    {
        INFO(refs[static_cast<std::size_t>(i)].file.getFullPathName());
        CHECK(refs[static_cast<std::size_t>(i)].file.isAChildOf(moved / "Song"));
        CHECK(refs[static_cast<std::size_t>(i)].file.existsAsFile());
    }
    CHECK(!refs[4].file.existsAsFile());

    // Collecting again copies nothing new.
    s.answers.told.clear();
    s.files->collectAllAndSave();
    REQUIRE(s.answers.told.size() == 1);
    CHECK(s.answers.told[0].contains("Gone.wav"));
    CHECK(!s.answers.told[0].contains("copied into"));
}

TEST_CASE("Set files: a missing file is found in the library and relinked", "[ui][files]")
{
    TempFolder place("lattice-place");
    REQUIRE(writeWav(place / "Drums/Kick 01.wav", 0.1));
    library::LibraryIndex index({}, testRegistry());
    index.addPlace(place.folder);
    index.scan(place.folder);

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings};
    const auto gone = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("nowhere/Kick 01.wav");
    const auto sampler = addSampler(session.set(), 0, gone);
    addSampler(session.set(), 1, gone);
    const auto other = addSampler(session.set(), 2, gone.getSiblingFile("Unknown.wav"));

    auto missing = setfiles::missing(session.set().state());
    REQUIRE(missing.size() == 3);
    CHECK(MissingFilesView::candidates(index, gone) == std::vector<juce::File>{place / "Drums/Kick 01.wav"});

    MissingFilesView view(session, index, missing);
    REQUIRE(view.entries().size() == 2); // each file once
    CHECK(view.searchLibrary() == 1);
    CHECK(fileOf(sampler) == place / "Drums/Kick 01.wav");
    CHECK(setfiles::missing(session.set().state()).size() == 1); // only Unknown.wav
    session.undoManager().undo();
    CHECK(fileOf(sampler) == gone);
    session.undoManager().redo();

    // Located by hand: the other entry.
    REQUIRE(writeWav(place / "Other/Unknown.wav", 0.1));
    view.relink(1, place / "Other/Unknown.wav");
    CHECK(fileOf(other) == place / "Other/Unknown.wav");
    CHECK(setfiles::missing(session.set().state()).empty());
}

TEST_CASE("Set files: autosave, then a crash: the next start restores the set", "[ui][files]")
{
    TempFolder project("lattice-crash");
    TempFolder recovery("lattice-recovery");
    juce::String expected;
    {
        Session s;
        s.files = std::make_unique<SetFileActions>(s.session, *s.settings, s.jobs, recovery.folder, s.answers.dialogs());
        s.answers.choose = project / "Song.lset";
        s.files->save();
        s.files->autosaveNow(); // nothing unsaved: no copy
        s.files->waitForAutosave();
        CHECK(!s.files->recovery().exists());

        s.session.set().setTempo(133.0);
        s.session.set().createClip(0, 0, 4.0, "Before the crash");
        s.files->autosaveNow();
        s.files->waitForAutosave();
        REQUIRE(s.files->recovery().exists());
        CHECK(s.files->recovery().original() == project / "Song.lset");
        auto copy = s.session.set().state().createCopy();
        copy.removeProperty(ids::nextId, nullptr);
        expected = copy.toXmlString();
        // The crash: no quitting(), the copy stays.
        s.files->setDialogs({});
    }
    Session next;
    next.files = std::make_unique<SetFileActions>(next.session, *next.settings, next.jobs, recovery.folder, next.answers.dialogs());
    next.answers.button = 0; // Restore
    next.files->offerRecovery();
    CHECK(next.answers.asked == juce::StringArray{"Restore your work?"});
    auto restored = next.session.set().state().createCopy();
    restored.removeProperty(ids::nextId, nullptr);
    CHECK(restored.toXmlString() == expected);
    CHECK(next.session.currentFile() == project / "Song.lset");
    CHECK(next.session.hasUnsavedChanges());
    CHECK(next.files->title() == "Song* - Lattice");

    // Saved, the copy goes; a normal quit leaves none either.
    next.files->save();
    CHECK(!next.files->recovery().exists());
    next.session.set().setTempo(90.0);
    next.files->autosaveNow();
    next.files->waitForAutosave();
    CHECK(next.files->recovery().exists());
    next.files->quitting();
    CHECK(!next.files->recovery().exists());
}

TEST_CASE("Set files: New, Open and Save ask about unsaved changes; recent sets; the title", "[ui][files]")
{
    TempFolder project("lattice-sets");
    Session s;
    CHECK(s.files->title() == "Untitled - Lattice");
    s.session.set().setTempo(101.0);
    s.files->flushTitle();
    CHECK(s.files->title() == "Untitled* - Lattice");

    // New: Cancel keeps the set; Don't Save drops it.
    s.answers.button = 2;
    s.files->newSet();
    CHECK(s.session.set().tempo() == 101.0);
    s.answers.button = 1;
    s.files->newSet();
    CHECK(s.session.set().tempo() == 120.0);
    CHECK(s.answers.asked.size() == 2);

    // Save As, then Save without asking where.
    s.session.set().setTempo(102.0);
    s.answers.choose = project / "First";
    s.files->save();
    CHECK(s.session.currentFile() == project / "First.lset");
    CHECK(s.files->title() == "First - Lattice");
    const int chosen = s.answers.chooses;
    s.session.set().setTempo(103.0);
    s.files->save();
    CHECK(s.answers.chooses == chosen);

    // New with unsaved changes, answered Save: saved first.
    s.session.set().setTempo(104.0);
    s.answers.button = 0;
    s.files->newSet();
    CHECK(s.session.set().tempo() == 120.0);
    s.files->open(project / "First.lset");
    CHECK(s.session.set().tempo() == 104.0);

    // Recent sets: the latest first, each once, at most ten, only those still there.
    for (int i = 0; i < 12; ++i)
    {
        s.answers.choose = project / ("Set " + juce::String(i));
        s.files->saveAs();
    }
    const auto recent = s.files->recentSets();
    CHECK(recent.size() == SetFileActions::maxRecent);
    CHECK(recent[0] == (project / "Set 11.lset").getFullPathName());
    (project / "Set 10.lset").deleteFile();
    CHECK(s.files->recentSets()[1] == (project / "Set 9.lset").getFullPathName());

    // A set naming missing files says so after opening.
    std::vector<setfiles::Reference> reported;
    s.files->onMissingFiles = [&](std::vector<setfiles::Reference> m) { reported = std::move(m); };
    addSampler(s.session.set(), 0, project / "Nowhere.wav");
    s.files->save();
    s.files->open(s.session.currentFile());
    REQUIRE(reported.size() == 1);
    CHECK(reported[0].file == project / "Nowhere.wav");
}

TEST_CASE("Undo history: every step, oldest first; clicking one goes back to it", "[ui][files]")
{
    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings};
    auto& undo = session.undoManager();
    UndoHistoryView view(undo);
    for (const auto* step : {"Tempo", "Rename", "Colour"})
    {
        undo.beginNewTransaction(step);
        session.set().setTempo(session.set().tempo() + 1.0);
    }
    view.refresh();
    CHECK(view.rows() == juce::StringArray{"Start", "Tempo", "Rename", "Colour"});
    CHECK(view.current() == 3);
    view.goTo(1);
    CHECK(session.set().tempo() == 121.0);
    CHECK(view.current() == 1);
    CHECK(view.rows().size() == 4); // the undone steps can still be redone
    view.goTo(0);
    CHECK(session.set().tempo() == 120.0);
    view.goTo(3);
    CHECK(session.set().tempo() == 123.0);
}

TEST_CASE("Set files: the shell's commands and the File menu's recent sets", "[ui][files]")
{
    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    const auto settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lattice-files-" + juce::Uuid().toString() + ".settings");
    juce::PropertiesFile::Options options;
    options.applicationName = "LatticeTests";
    juce::PropertiesFile settings(settingsFile, options);
    {
        MainShell shell(session, engine, settings, tunings, ShellActions{});
        for (const auto* id : {"file.collectAll", "file.findMissing", "view.undoHistory"})
            CHECK(shell.commands().find(id) != nullptr);
        CHECK(!shell.isUndoHistoryVisible());
        CHECK(shell.commands().perform("view.undoHistory"));
        CHECK(shell.isUndoHistoryVisible());
        const auto file = shell.menus().getMenuForIndex(0, "File");
        bool recent = false;
        for (juce::PopupMenu::MenuItemIterator it(file); it.next();)
            recent = recent || it.getItem().text == "Open Recent";
        CHECK(recent);
    }
    settingsFile.deleteFile();
}
