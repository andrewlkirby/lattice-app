// The Preferences (docs/05-m4-ui.md, 4g): each setting is saved and applied (zoom, worker threads,
// new tracks' tuning and high notes, MIDI Map's inputs, the computer keyboard's keys and range steps,
// shortcuts saved and restored, imported and exported), and every tab opens.

#include "support/TestDevices.h"
#include "tuning/Tet12.h"
#include "ui/find/FindIndex.h"
#include "ui/prefs/AppSettings.h"
#include "ui/shell/MainShell.h"

#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::ui;
using namespace lattice::test;

namespace
{
struct Settings
{
    Settings()
    {
        file = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("lattice-prefs", ".settings");
        juce::PropertiesFile::Options options;
        options.applicationName = "LatticeTests";
        props = std::make_unique<juce::PropertiesFile>(file, options);
    }
    ~Settings()
    {
        props.reset();
        file.deleteFile();
        LiveSet::newTrackDefaults() = {}; // shared by every set made after
        prefs::applyZoom(1.0);
    }
    juce::File file;
    std::unique_ptr<juce::PropertiesFile> props;
};

struct Fixture : Settings
{
    Fixture()
    {
        session.newSet(2, 2);
        shell = std::make_unique<MainShell>(session, engine, *props, tunings, ShellActions{});
        shell->setSize(1280, 900);
    }
    ~Fixture() { shell.reset(); }

    std::unique_ptr<PreferencesView> view()
    {
        auto v = std::make_unique<PreferencesView>(shell->preferencesContext());
        v->setSize(900, 600);
        return v;
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    std::unique_ptr<MainShell> shell;
};

template <typename T>
T* childTitled(juce::Component& root, const juce::String& title)
{
    for (auto* c : root.getChildren())
    {
        if (auto* t = dynamic_cast<T*>(c); t != nullptr && c->getTitle() == title)
            return t;
        if (auto* deeper = childTitled<T>(*c, title))
            return deeper;
    }
    return nullptr;
}
} // namespace

TEST_CASE("Preferences: zoom, worker threads, new tracks' tuning and high notes, MIDI Map's inputs", "[ui][prefs]")
{
    Settings s;
    auto& settings = *s.props;

    CHECK(prefs::zoom(settings) == 1.0);
    settings.setValue(prefs::zoomKey, 9.0);
    CHECK(prefs::zoom(settings) == prefs::maxZoom);
    prefs::applyZoom(1.5);
    CHECK(juce::Desktop::getInstance().getGlobalScaleFactor() == 1.5f);

    CHECK(prefs::workerThreads(settings) == WorkerPool::defaultWorkers());
    settings.setValue(prefs::workersKey, 2);
    CHECK(prefs::workerThreads(settings) == 2);
    settings.setValue(prefs::multiThreadedKey, false);
    CHECK(prefs::workerThreads(settings) == 0);

    settings.setValue(prefs::defaultTuningKey, TuningRegistry::colundiId);
    settings.setValue(prefs::defaultHighNotesKey, 1);
    prefs::applyNewTrackDefaults(settings);
    LiveSet set;
    set.resetToDefault(3, 1);
    for (int t = 0; t < set.numTracks(); ++t)
    {
        CHECK(set.track(t)[ids::tuningId].toString() == TuningRegistry::colundiId);
        CHECK(static_cast<int>(set.track(t)[ids::highNotePolicy]) == 1);
    }

    prefs::setRemoteInput(settings, "port-a", true);
    prefs::setRemoteInput(settings, "port-b", true);
    prefs::setRemoteInput(settings, "port-a", false);
    CHECK(prefs::remoteInputs(settings) == juce::StringArray{"port-b"});
}

TEST_CASE("Preferences: the computer keyboard's note keys and each tuning's range step", "[ui][prefs]")
{
    Settings s;
    auto& settings = *s.props;
    std::vector<int> played;
    ComputerKeyboardInput keyboard([&](const juce::MidiMessage& m) {
        if (m.isNoteOn())
            played.push_back(m.getNoteNumber());
    });

    CHECK(ComputerKeyboardInput::isValidNoteKeys(ComputerKeyboardInput::noteKeys));
    CHECK(!ComputerKeyboardInput::isValidNoteKeys("abc"));                 // not 17
    CHECK(!ComputerKeyboardInput::isValidNoteKeys("qwertyuiopasdfghz"));   // Z moves the range
    CHECK(!ComputerKeyboardInput::isValidNoteKeys("qwertyuiopasdfghq"));   // Q twice
    CHECK(ComputerKeyboardInput::isValidNoteKeys("1234567890-=qwert"));

    settings.setValue(prefs::noteKeysKey, "1234567890-=qwert");
    settings.setValue(juce::String(prefs::pageKeyPrefix) + Tet12::builtinId, 5);
    prefs::applyKeyboard(settings, keyboard);
    CHECK(keyboard.getNoteKeys() == "1234567890-=qwert");
    keyboard.configureForTuning(*Tet12::make());
    CHECK(keyboard.getPageSize() == 5);
    const int base = keyboard.getBaseNote();
    CHECK(keyboard.keyDown('1', 1));
    CHECK(keyboard.keyDown('t', 2));
    CHECK(!keyboard.keyDown('a', 3)); // not a note key any more
    CHECK(played == std::vector<int>{base, base + 16});
    keyboard.releaseAll();

    // A tuning with no step of its own here keeps its own.
    keyboard.configureForTuning(*TuningRegistry::makeBuiltinColundi());
    CHECK(keyboard.getPageSize() == 17);

    // A bad row in the settings falls back to Live's.
    settings.setValue(prefs::noteKeysKey, "zzz");
    prefs::applyKeyboard(settings, keyboard);
    CHECK(keyboard.getNoteKeys() == ComputerKeyboardInput::noteKeys);
}

TEST_CASE("Preferences: shortcuts are saved, restored at startup, exported and imported", "[ui][prefs]")
{
    Fixture f;
    auto& commands = f.shell->commands();
    const auto key = juce::KeyPress('k', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0);
    commands.keyMappings().addKeyPress(commands.numericIdOf("transport.metronome"), key);
    prefs::saveShortcuts(*f.props, commands);

    // Exported, then imported over the defaults.
    const auto exported = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("shortcuts", ".xml");
    REQUIRE(commands.shortcutsToXml()->writeTo(exported));
    commands.resetShortcutsToDefaults();
    CHECK(commands.shortcutsFor("transport.metronome").isEmpty());
    commands.restoreShortcuts(*juce::XmlDocument::parse(exported));
    CHECK(commands.shortcutsFor("transport.metronome").contains(key));
    exported.deleteFile();

    // The next start has it.
    f.shell.reset();
    f.shell = std::make_unique<MainShell>(f.session, f.engine, *f.props, f.tunings, ShellActions{});
    CHECK(f.shell->commands().shortcutsFor("transport.metronome").contains(key));
    CHECK(f.shell->commands().find("app.preferences") != nullptr);
    CHECK(f.shell->commands().conflicts().empty());
}

TEST_CASE("Preferences: every tab opens; the tabs apply what's chosen", "[ui][prefs]")
{
    Fixture f;
    auto v = f.view();
    for (int p = 0; p < PreferencesView::numPages; ++p)
    {
        const auto page = static_cast<PreferencesView::Page>(p);
        INFO(PreferencesView::pageName(page));
        v->show(page);
        REQUIRE(v->pageComponent() != nullptr);
        CHECK(v->pageComponent()->getTitle() == PreferencesView::pageName(page));
        CHECK(f.props->getIntValue(PreferencesView::lastPageKey) == p);
    }

    SECTION("Find knows every labelled setting, on its tab")
    {
        for (int p = 0; p < PreferencesView::numPages; ++p)
        {
            const auto page = static_cast<PreferencesView::Page>(p);
            v->show(page);
            for (auto* c : v->pageComponent()->getChildren())
            {
                // A row's control is titled with its label; buttons say what they do on their face.
                if (c->getTitle().isEmpty() || dynamic_cast<juce::Button*>(c) != nullptr)
                    continue;
                // Typing the entry's name finds it ("Note Keys": "Note keys (lowest first)").
                bool found = false;
                for (const auto& s : FindIndex::settings())
                    found = found || (s.page == p && FindIndex::matches(FindIndex::wordsOf(s.name), c->getTitle()));
                INFO(PreferencesView::pageName(page) << ": " << c->getTitle());
                CHECK(found);
            }
        }
    }

    SECTION("Tuning: new tracks' tuning")
    {
        v->show(PreferencesView::Page::tuning);
        auto* box = childTitled<juce::ComboBox>(*v->pageComponent(), "New tracks' tuning");
        REQUIRE(box != nullptr);
        box->setText("Colundi", juce::sendNotificationSync);
        CHECK(f.props->getValue(prefs::defaultTuningKey) == TuningRegistry::colundiId);
        CHECK(LiveSet::newTrackDefaults().tuningId == TuningRegistry::colundiId);
        f.session.newSet(1, 1);
        CHECK(f.session.set().track(0)[ids::tuningId].toString() == TuningRegistry::colundiId);
    }
    SECTION("CPU: the worker threads")
    {
        v->show(PreferencesView::Page::cpu);
        auto* box = childTitled<juce::ComboBox>(*v->pageComponent(), "Worker threads");
        REQUIRE(box != nullptr);
        box->setSelectedId(2 + 1, juce::sendNotificationSync); // 2 threads
        CHECK(f.props->getIntValue(prefs::workersKey) == 2);
        CHECK(f.engine.workerThreads() == 2);
    }
    SECTION("Look & Feel: the theme and the zoom")
    {
        v->show(PreferencesView::Page::look);
        auto* theme = childTitled<juce::ComboBox>(*v->pageComponent(), "Theme");
        REQUIRE(theme != nullptr);
        theme->setText("Light", juce::sendNotificationSync);
        CHECK(f.shell->theme().name == "Light");
        auto* zoom = childTitled<juce::Slider>(*v->pageComponent(), "Zoom");
        REQUIRE(zoom != nullptr);
        zoom->setValue(150.0, juce::sendNotificationSync);
        CHECK(juce::Desktop::getInstance().getGlobalScaleFactor() == 1.5f);
        CHECK(f.props->getDoubleValue(prefs::zoomKey) == 1.5);
    }
    SECTION("Computer Keyboard: the note keys reach the keyboard")
    {
        ComputerKeyboardInput keyboard([](const juce::MidiMessage&) {});
        f.shell->setComputerKeyboard(&keyboard);
        int changed = 0;
        f.shell->onKeyboardSettingsChanged = [&] { ++changed; };
        auto keys = f.view();
        keys->show(PreferencesView::Page::keyboard);
        auto* editor = childTitled<juce::TextEditor>(*keys->pageComponent(), "Note keys (lowest first)");
        REQUIRE(editor != nullptr);
        editor->setText("1234567890-=qwert", false);
        editor->onTextChange();
        CHECK(keyboard.getNoteKeys() == "1234567890-=qwert");
        CHECK(changed == 1);
        editor->setText("not valid", false);
        editor->onTextChange();
        CHECK(keyboard.getNoteKeys() == "1234567890-=qwert"); // kept until it's 17 good keys
        f.shell->setComputerKeyboard(nullptr);
    }
    SECTION("Shortcuts: every command is listed, by category")
    {
        v->show(PreferencesView::Page::shortcuts);
        juce::TreeView* tree = nullptr; // JUCE's editor's
        std::function<void(juce::Component&)> find = [&](juce::Component& c) {
            if (auto* t = dynamic_cast<juce::TreeView*>(&c))
                tree = t;
            for (auto* child : c.getChildren())
                find(*child);
        };
        find(*v->pageComponent());
        REQUIRE(tree != nullptr);
        auto* root = tree->getRootItem();
        REQUIRE(root != nullptr);
        CHECK(root->getNumSubItems() == f.shell->menus().getMenuBarNames().size());
    }
    SECTION("Library: the places, with the User Library first")
    {
        v->show(PreferencesView::Page::library);
        auto* list = childTitled<juce::ListBox>(*v->pageComponent(), "Places");
        REQUIRE(list != nullptr);
        CHECK(list->getListBoxModel()->getNumRows() == f.shell->library().places().size());
    }
}

TEST_CASE("Preferences snapshot: every tab, dark and light", "[.ui-snapshot]")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable("LATTICE_SNAPSHOT_DIR", {});
    if (dir.isEmpty())
        return;
    Fixture f;
    for (const auto* name : {"Dark", "Light"})
    {
        f.shell->setTheme(*Theme::builtIn(name));
        auto v = f.view();
        v->setLookAndFeel(&f.shell->getLookAndFeel());
        for (int p = 0; p < PreferencesView::numPages; ++p)
        {
            v->show(static_cast<PreferencesView::Page>(p));
            const auto image = v->createComponentSnapshot(v->getLocalBounds(), true, 1.0f);
            const auto file = juce::File(dir).getChildFile("prefs-" + juce::String(p) + "-" + juce::String(name) + ".png");
            file.deleteFile();
            juce::FileOutputStream out(file);
            juce::PNGImageFormat png;
            REQUIRE(png.writeImageToStream(image, out));
        }
        v->setLookAndFeel(nullptr);
    }
}
