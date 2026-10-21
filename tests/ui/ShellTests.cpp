// The window shell (docs/05-m4-ui.md, 4a), headless: the layout and its panels, keys reaching the
// commands, the info view describing what's under the pointer, the control bar following the
// engine, theme changes, and the status bar.

#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"
#include "ui/shell/MainShell.h"
#include "ui/widgets/Knob.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

using namespace lattice;
using namespace lattice::ui;
using namespace lattice::test;

namespace
{
struct Fixture
{
    Fixture()
    {
        settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("lattice-shell", ".settings");
        juce::PropertiesFile::Options options;
        options.applicationName = "LatticeTests";
        settings = std::make_unique<juce::PropertiesFile>(settingsFile, options);
        session.newSet(2, 2);
        session.flush();
        shell = std::make_unique<MainShell>(session, engine, *settings, tunings, ShellActions{});
        shell->setSize(1280, 900);
    }
    ~Fixture()
    {
        shell.reset();
        settings.reset();
        settingsFile.deleteFile();
    }

    void render(std::int64_t samples)
    {
        session.flush();
        renderer.renderFor(samples);
        session.processEngineEvents();
    }
    void tick()
    {
        now += 1.0 / 60.0;
        shell->feed().tick(now);
    }
    bool press(const juce::KeyPress& key) { return shell->commands().keyMappings().keyPressed(key, shell.get()); }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer{engine, BlockPattern::Fixed64};
    juce::File settingsFile;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<MainShell> shell;
    double now = 5.0;
};

template <typename T>
T* findChild(juce::Component& root, const std::function<bool(T&)>& matches = {})
{
    for (auto* c : root.getChildren())
    {
        if (auto* t = dynamic_cast<T*>(c); t != nullptr && (!matches || matches(*t)))
            return t;
        if (auto* deeper = findChild<T>(*c, matches))
            return deeper;
    }
    return nullptr;
}
} // namespace

TEST_CASE("Shell: the control bar, main area, info view and status bar are laid out; panels toggle", "[ui][shell]")
{
    Fixture f;
    auto& shell = *f.shell;
    const auto bar = shell.controlBar().getBounds();
    const auto main = shell.mainArea().getBounds();
    const auto status = shell.statusBar().getBounds();
    CHECK(bar.getY() == 0);
    CHECK(bar.getWidth() == 1280);
    CHECK(status.getBottom() == 900);
    CHECK(main.getY() >= bar.getBottom());
    CHECK(main.getBottom() <= status.getY());
    CHECK(shell.infoView().isVisible());
    CHECK(shell.infoView().getRight() <= main.getX()); // bottom-left, beside the main area
    CHECK(!shell.isBrowserVisible());
    CHECK(!shell.isDetailVisible());
    // Session, then the disabled Arrangement slot.
    CHECK(shell.mainArea().getTabNames() == juce::StringArray{"Session", "Arrangement"});
    CHECK(!shell.mainArea().getTabbedButtonBar().getTabButton(1)->isEnabled());

    // Showing the detail view takes room from the main area; the choice is remembered.
    const int before = main.getHeight();
    CHECK(shell.commands().perform("view.detail"));
    CHECK(shell.isDetailVisible());
    CHECK(shell.mainArea().getHeight() < before);
    CHECK(f.settings->getBoolValue(MainShell::detailKey, false));
    CHECK(shell.commands().perform("view.browser"));
    CHECK(shell.commands().perform("view.info"));
    CHECK(!shell.infoView().isVisible());
    CHECK(shell.commands().find("view.browser")->isTicked());

    // A debug UI tab goes last; the Session view stays in front.
    juce::Component debugUi;
    shell.setDebugUi(&debugUi);
    CHECK(shell.mainArea().getTabNames()[2] == "Debug UI");
    CHECK(shell.mainArea().getCurrentTabName() == "Session");
    CHECK(shell.commands().conflicts().empty()); // the view commands too
}

TEST_CASE("Shell: keys reach the commands through the key mappings", "[ui][shell]")
{
    Fixture f;
    const int tracks = f.session.set().numTracks();
    CHECK(f.press(juce::KeyPress('t', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)));
    CHECK(f.session.set().numTracks() == tracks + 1);
    CHECK(f.press(juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)));
    CHECK(f.session.set().numTracks() == tracks);
    CHECK(f.press(juce::KeyPress(juce::KeyPress::downKey)));
    CHECK(f.shell->selection().get().scene == 1);
    // Letters are left for the computer keyboard.
    CHECK(!f.press(juce::KeyPress('a')));
}

TEST_CASE("Shell: the info view describes what's under the pointer", "[ui][shell]")
{
    Fixture f;
    auto& shell = *f.shell;
    auto& play = shell.controlBar().playButton();
    shell.showInfoFor(&play);
    CHECK(shell.infoView().shown().title == "Play");
    CHECK(shell.infoView().shown().body == shell.commands().find("transport.play")->description);

    auto* tempo = findChild<NumberBox>(shell.controlBar(), [](NumberBox& b) { return b.getTitle() == "Tempo"; });
    REQUIRE(tempo != nullptr);
    shell.showInfoFor(tempo);
    CHECK(shell.infoView().shown().title == "Tempo");
    CHECK(shell.infoView().shown().body.contains("120.00"));
    CHECK(shell.infoView().shown().body.contains("Double-click to type"));

    juce::Component plain;
    shell.showInfoFor(&plain);
    CHECK(shell.infoView().shown().isEmpty());

    // A command button shows its shortcut.
    auto* tap = findChild<CommandButton>(shell.controlBar(), [](CommandButton& b) { return b.commandId() == "transport.tapTempo"; });
    REQUIRE(tap != nullptr);
    shell.showInfoFor(tap);
    CHECK(shell.infoView().shown().title == "Tap Tempo");
    shell.commands().keyMappings().addKeyPress(shell.commands().numericIdOf("transport.tapTempo"), juce::KeyPress(juce::KeyPress::F6Key));
    shell.showInfoFor(&plain);
    shell.showInfoFor(tap);
    CHECK(shell.infoView().shown().shortcut.isNotEmpty());
}

TEST_CASE("Shell: the control bar follows the engine and the set", "[ui][shell]")
{
    Fixture f;
    auto& bar = f.shell->controlBar();
    f.render(480);
    f.tick();
    CHECK(bar.positionText() == "1. 1. 1");
    CHECK(!bar.playButton().isOn());

    CHECK(f.shell->commands().perform("transport.play"));
    f.render(48000); // two beats at 120 BPM
    f.tick();
    CHECK(bar.playButton().isOn());
    CHECK(bar.positionText() == "1. 3. 1");
    CHECK(bar.loadMeter().text().endsWith("%"));

    // The tempo box writes the set, and follows it.
    auto* tempo = findChild<NumberBox>(bar, [](NumberBox& b) { return b.getTitle() == "Tempo"; });
    REQUIRE(tempo != nullptr);
    tempo->commitTypeIn("98.5");
    CHECK(f.session.set().tempo() == 98.5);
    f.session.undoManager().undo();
    CHECK(tempo->binding().value() == 120.0f);

    // The computer keyboard's state, and switching it.
    ComputerKeyboardInput keyboard([](const juce::MidiMessage&) {});
    bar.setComputerKeyboard(&keyboard);
    CHECK(bar.keyboardText().contains("vel 100"));
    keyboard.keyDown('v', 'V'); // velocity up
    CHECK(bar.keyboardText().contains("vel 120"));
    bar.setComputerKeyboard(nullptr);
    CHECK(keyboard.onStateChanged == nullptr); // the bar's callback is gone with it
}

TEST_CASE("Shell: a theme command restyles the window and is remembered", "[ui][shell]")
{
    Fixture f;
    CHECK(f.shell->theme().name == "Dark");
    CHECK(f.shell->commands().perform("view.themeLight"));
    CHECK(f.shell->theme().name == "Light");
    CHECK(themeOf(f.shell->controlBar()) == Theme::light());
    CHECK(f.settings->getValue(MainShell::themeKey) == "Light");
    CHECK(f.shell->commands().find("view.themeLight")->isTicked());
    CHECK(!f.shell->commands().find("view.themeDark")->isTicked());
}

TEST_CASE("Shell: the status bar shows the set's state and background jobs", "[ui][shell]")
{
    Fixture f;
    auto& status = f.shell->statusBar();
    status.refresh();
    CHECK(status.setText() == "Untitled set");
    f.session.set().setTempo(130.0);
    status.refresh();
    CHECK(status.setText() == "Untitled set (unsaved changes)");

    std::atomic<bool> release{false}, started{false};
    f.shell->jobs().submit("Building peaks", [&](JobPool::Context& c) {
        c.setProgress(0.4f);
        started = true;
        while (!release)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    while (!started)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    status.refresh();
    CHECK(status.jobsText() == "Building peaks 40%");
    release = true;
    REQUIRE(f.shell->jobs().waitUntilIdle(5000));
    status.refresh();
    CHECK(status.jobsText().isEmpty());
}

TEST_CASE("Shell: snapshots of the window in every theme, for looking at", "[.ui-snapshot]")
{
    Fixture f;
    f.shell->setDetailVisible(true);
    f.shell->setBrowserVisible(true);
    f.render(480);
    f.tick();
    const auto dir = juce::File(juce::SystemStats::getEnvironmentVariable(
        "LATTICE_SNAPSHOT_DIR", juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName()));
    for (const auto* theme : {&Theme::dark(), &Theme::light(), &Theme::highContrast()})
    {
        f.shell->setTheme(*theme);
        f.shell->showInfoFor(&f.shell->controlBar().playButton());
        const auto image = f.shell->createComponentSnapshot(f.shell->getLocalBounds(), true, 1.0f);
        juce::PNGImageFormat png;
        const auto file = dir.getChildFile("shell-" + theme->name.replace(" ", "-").toLowerCase() + ".png");
        file.deleteFile();
        juce::FileOutputStream out(file);
        REQUIRE(png.writeImageToStream(image, out));
        WARN("Wrote " << file.getFullPathName());
    }
}

TEST_CASE("Shell: every frame's UI work is measured against the frame budget", "[ui][shell]")
{
    Fixture f;
    f.shell->resetLoadStatistics();
    CHECK(f.shell->frameStats().summary().callbacks == 0);
    f.tick(); // the feed's work counts towards the next frame
    for (int i = 0; i < 3; ++i)
        f.shell->createComponentSnapshot(f.shell->getLocalBounds(), true, 1.0f); // paints the whole window
    const auto frames = f.shell->frameStats().summary();
    CHECK(frames.callbacks == 3);
    CHECK(frames.max > 0.0);

    // The load meter's tooltip reports them.
    f.tick();
    f.now += 1.0; // past the load interval
    f.tick();
    CHECK(f.shell->controlBar().loadMeter().getTooltip().contains("UI frames"));

    CHECK(f.shell->commands().perform("view.resetLoad"));
    CHECK(f.shell->frameStats().summary().callbacks == 0);
}

TEST_CASE("Shell: the control registry knows every bound control in the window", "[ui][shell]")
{
    Fixture f;
    auto& controls = f.shell->controlRegistry();
    // The control bar's bound controls: tempo, the time signature, quantization.
    const auto tempo = controls.search("tempo");
    REQUIRE(tempo.size() == 1);
    CHECK(tempo[0]->name == "Tempo");
    CHECK(tempo[0]->binding != nullptr);
    CHECK(controls.search("time signature").size() == 2); // every word must match
    CHECK(controls.search("signature beat length").size() == 1);
    CHECK(controls.search("nothing like this").empty());

    // A widget leaves the registry when it leaves the window, and joins it when added.
    juce::Component holder;
    Knob knob(ValueBinding::property(ParameterSpec::continuous("x", "Brightness", 0.0f, 1.0f, 0.5f).inGroup("Tone"),
                                     nullptr, f.session.set().state(), "x", [](double) {}));
    holder.addAndMakeVisible(knob);
    CHECK(controls.find(&knob) == nullptr);
    f.shell->addAndMakeVisible(holder);
    REQUIRE(controls.find(&knob) != nullptr);
    CHECK(controls.find(&knob)->group == "Tone");
    CHECK(controls.search("tone bright").size() == 1);
    f.shell->removeChildComponent(&holder);
    CHECK(controls.find(&knob) == nullptr);
}

TEST_CASE("Shell: the menu bar has every command, once, by category", "[ui][shell]")
{
    Fixture f;
    auto& menus = f.shell->menus();
    const auto names = menus.getMenuBarNames();
    CHECK(names == juce::StringArray{"File", "Edit", "Create", "Session", "Transport", "View"});
    int items = 0;
    for (int i = 0; i < names.size(); ++i)
    {
        const auto menu = menus.getMenuForIndex(i, names[i]);
        juce::PopupMenu::MenuItemIterator it(menu);
        while (it.next())
        {
            ++items;
            const auto* command = f.shell->commands().find(it.getItem().itemID);
            REQUIRE(command != nullptr);
            CHECK(command->category == names[i]);
        }
    }
    CHECK(items == static_cast<int>(f.shell->commands().all().size()));
}

TEST_CASE("Shell: auto-arm follows the selected MIDI track, and can be turned off", "[ui][shell]")
{
    Fixture f;
    auto& set = f.session.set();
    for (int t = 0; t < set.numTracks(); ++t)
        set.setTrackArm(t, false, true);
    f.shell->selection().set({SessionSelection::Kind::slot, 1, 0});
    CHECK(f.session.isAutoArmed(1));
    f.shell->selection().set({SessionSelection::Kind::track, 0, 0});
    CHECK(f.session.isAutoArmed(0));
    CHECK(!f.session.isArmed(1));
    f.shell->selection().set({SessionSelection::Kind::scene, 0, 1}); // a scene: no track
    CHECK(!f.session.isArmed(0));

    f.shell->selection().set({SessionSelection::Kind::slot, 1, 0});
    CHECK(f.shell->commands().perform("session.autoArm")); // off
    CHECK(!f.shell->isAutoArmOn());
    CHECK(!f.session.isArmed(1));
    CHECK(!f.settings->getBoolValue(MainShell::autoArmKey, true));
}
