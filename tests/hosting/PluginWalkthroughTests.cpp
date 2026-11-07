// A walkthrough of 6a's hands-on check (docs/07-m6-plugins-external.md, 6a): the app's own plugin
// scan over a folder of real plugins, then the browser's Plugins category and the Preferences'
// Plugins tab, used as a person would. Hidden ([.]): run it by its tag, with
//   LATTICE_PLUGIN_FOLDER   the plugins to scan (e.g. Surge XT's plugins-only zip, unpacked)
//   LATTICE_WALKTHROUGH_DIR where to save an image at each step (optional)
// It adds the crashing test plugins, so the blocked list has something in it.

#include "support/Snapshots.h"
#include "support/TestDevices.h"
#include "ui/prefs/AppSettings.h"
#include "ui/shell/MainShell.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::test;

namespace
{
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

struct Walk
{
    Walk()
    {
        settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lattice-plugin-walk-" + juce::Uuid().toString() + ".settings");
        juce::PropertiesFile::Options options;
        options.applicationName = "LatticeTests";
        settings = std::make_unique<juce::PropertiesFile>(settingsFile, options);
        session.newSet(2, 2);
        session.flush();
        shell = std::make_unique<ui::MainShell>(session, engine, *settings, tunings, ui::ShellActions{});
        shell->setSize(1800, 1000);
        shell->setBrowserVisible(true);
        settle();
    }
    ~Walk()
    {
        shell.reset();
        settings.reset();
        settingsFile.deleteFile();
    }

    ui::BrowserModel& browser() { return shell->browserView().model(); }

    void settle()
    {
        shell->plugins().waitForTests(120000);
        for (int i = 0; i < 20; ++i)
        {
            shell->jobs().waitUntilIdle(5000);
            shell->library().deliverForTests();
            const bool wasIdle = shell->jobs().idle();
            if (shell->jobs().deliverFinished() == 0 && wasIdle)
                break;
        }
    }

    void showPlugins()
    {
        for (const auto& s : browser().sources())
            if (s.kind == ui::BrowserModel::Source::Kind::category && s.name == "Plugins")
                browser().setSource(s);
        settle();
        // Every group open, as a person would to look.
        for (std::size_t r = 0; r < browser().rows().size(); ++r)
            if (browser().rows()[r].expandable && !browser().rows()[r].expanded)
            {
                browser().setExpanded(static_cast<int>(r), true);
                settle();
            }
    }

    juce::String rows()
    {
        juce::StringArray lines;
        for (const auto& r : browser().rows())
            lines.add(juce::String::repeatedString("  ", r.depth) + r.item.name +
                      (r.item.detail.isNotEmpty() ? "   [" + r.item.detail + "]" : juce::String()));
        return lines.joinIntoString("\n");
    }

    void shot(juce::Component& c, const juce::String& name, float scale = 1.0f)
    {
        const auto dir = juce::SystemStats::getEnvironmentVariable("LATTICE_WALKTHROUGH_DIR", {});
        if (dir.isNotEmpty())
            writePng(snapshot(c, scale), juce::File(dir).getChildFile("plugins-" + name + ".png"));
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    juce::File settingsFile;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<ui::MainShell> shell;
};
} // namespace

TEST_CASE("Walkthrough: 6a's plugin scan over real plugins, the browser and the Plugins tab", "[.walkthrough][plugins]")
{
    const juce::File folder(juce::SystemStats::getEnvironmentVariable("LATTICE_PLUGIN_FOLDER", {}));
    if (!folder.isDirectory())
    {
        // Not SKIP: its exception crashed while unwinding in some MSVC Release layouts (2026-11-08).
        WARN("Set LATTICE_PLUGIN_FOLDER to a folder of plugins");
        return;
    }
    Walk w;

    // Preferences > Plugins, as a person sets it: the folder added, the standard folders off (so
    // only what's asked for is scanned), and the crashing test plugins' folders too.
    hosting::PluginSettings s;
    s.useDefaultFolders = false;
    s.extraFolders = {folder.getFullPathName(), juce::File(LATTICE_CRASH_CLAP).getParentDirectory().getFullPathName(),
                      juce::File(LATTICE_CRASH_VST3).getParentDirectory().getFullPathName()};
    ui::prefs::savePluginSettings(*w.settings, s);
    w.shell->plugins().setSettings(s, false);

    const auto started = juce::Time::getMillisecondCounterHiRes();
    w.shell->startPluginScan(); // as the app does once it's ready
    w.settle();
    const auto seconds = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
    const auto status = w.shell->plugins().status();
    INFO(status.lastError);
    CHECK(status.lastError.isEmpty());
    WARN("Scan: " << status.done << " of " << status.total << " files, " << status.plugins << " plugins, in "
                  << juce::String(seconds, 1) << " s");

    // What each plugin was found to be.
    juce::StringArray found;
    for (const auto& stored : w.shell->library().index().storedPlugins())
        if (const auto xml = juce::parseXML(stored.xml))
            if (const auto p = hosting::scan::ScannedPlugin::fromXml(*xml))
            {
                const auto& d = p->description;
                found.add(d.pluginFormatName + " | " + d.name + " | " + d.manufacturerName + " | " + d.version + " | " + d.category +
                          " | instrument " + juce::String(d.isInstrument ? "yes" : "no") + " | in " + juce::String(d.numInputChannels) +
                          " out " + juce::String(d.numOutputChannels) + " | MIDI in " + juce::String(p->midiInputs) + " out " +
                          juce::String(p->midiOutputs) + " | MPE " + juce::String(p->mpe));
            }
    WARN("Found:\n" << found.joinIntoString("\n"));
    juce::StringArray blocked;
    for (const auto& f : w.shell->plugins().problems())
        blocked.add(f.format + " | " + juce::File(f.identifier).getFileName() + " | " + juce::String(static_cast<int>(f.status)) + " | " +
                    f.reason);
    WARN("Blocked or failed:\n" << blocked.joinIntoString("\n"));
    CHECK(w.shell->plugins().problems().size() == 2); // the two crashing test plugins, and nothing real

    // The browser: Plugins, every group open.
    w.showPlugins();
    WARN("Browser (preferred format automatic, by vendor):\n" << w.rows());
    w.shot(w.shell->browserView(), "1-browser-vendor");
    w.shot(*w.shell, "1-window", 0.6f);

    // The Plugins tab.
    ui::PreferencesView prefs(w.shell->preferencesContext());
    prefs.setSize(1100, 760);
    prefs.show(ui::PreferencesView::Page::plugins);
    w.settle();
    auto* page = prefs.pageComponent();
    REQUIRE(page != nullptr);
    auto* scanStatus = childTitled<juce::Label>(*page, "Plugin Scan");
    REQUIRE(scanStatus != nullptr);
    WARN("Plugins tab status: " << scanStatus->getText());
    w.shot(prefs, "2-preferences");

    // VST3 preferred, from the tab's own menu: the rows follow.
    auto* format = childTitled<juce::ComboBox>(*page, "Preferred format");
    REQUIRE(format != nullptr);
    format->setText("VST3", juce::sendNotificationSync);
    w.settle();
    w.showPlugins();
    WARN("Browser (VST3 preferred):\n" << w.rows());
    w.shot(w.shell->browserView(), "3-browser-vst3");

    // Grouped by format.
    auto* grouping = childTitled<juce::ComboBox>(*page, "Browser groups by");
    REQUIRE(grouping != nullptr);
    grouping->setSelectedId(2, juce::sendNotificationSync);
    w.settle();
    w.showPlugins();
    WARN("Browser (by format):\n" << w.rows());
    w.shot(w.shell->browserView(), "4-browser-format");
    w.shot(prefs, "4-preferences");

    // Scanning again with nothing changed: nothing is scanned.
    const auto again = juce::Time::getMillisecondCounterHiRes();
    w.shell->plugins().scan();
    w.settle();
    WARN("Second scan: " << w.shell->plugins().status().total << " files scanned, in "
                         << juce::String((juce::Time::getMillisecondCounterHiRes() - again) / 1000.0, 2) << " s");
    CHECK(w.shell->plugins().status().total == 0);
}

namespace
{
/** Every control's title in a component, for what a panel shows. */
juce::StringArray titlesIn(juce::Component& root)
{
    juce::StringArray titles;
    for (auto* c : root.getChildren())
    {
        if (c->getTitle().isNotEmpty())
            titles.add(c->getTitle());
        titles.addArray(titlesIn(*c));
    }
    return titles;
}

/** The format a walkthrough wants (LATTICE_WALKTHROUGH_FORMAT), or empty: any. */
juce::String format() { return juce::SystemStats::getEnvironmentVariable("LATTICE_WALKTHROUGH_FORMAT", {}); }

/** Puts the browser's plugin of that name on the selected track, as a double-click does, and lets
    it load. */
juce::ValueTree insertPlugin(Walk& w, const juce::String& name)
{
    juce::String key;
    for (const auto& stored : w.shell->library().index().storedPlugins())
        if (const auto xml = juce::parseXML(stored.xml))
            if (const auto p = hosting::scan::ScannedPlugin::fromXml(*xml); p && p->description.name == name && key.isEmpty() &&
                (format().isEmpty() || p->description.pluginFormatName == format()))
                key = stored.key;
    REQUIRE(key.isNotEmpty());
    // An empty track: an instrument where there is one would ask first.
    for (auto chain = LiveSet::chainOf(w.session.set().track(0)); chain.getNumChildren() > 0;)
        w.session.set().removeDevice(chain.getChild(0));
    w.session.flush();
    REQUIRE(w.shell->insertFromBrowser("plugin:" + key));
    w.shell->pluginDevices().finishLoading();
    w.shell->deviceView().flushUpdates();
    const auto chain = LiveSet::chainOf(w.session.set().track(0));
    for (const auto& d : chain)
        if (d[ids::name].toString() == name)
            return d;
    FAIL("not inserted");
    return {};
}
} // namespace

TEST_CASE("A plugin from the browser goes on the track, and its panel shows its pages", "[ui][plugins]")
{
    Walk w;
    hosting::PluginSettings s;
    s.useDefaultFolders = false;
    s.extraFolders = {juce::File(LATTICE_TEST_CLAP).getParentDirectory().getFullPathName()};
    w.shell->plugins().setSettings(s, false);
    w.shell->startPluginScan();
    w.settle();

    const auto device = insertPlugin(w, "Lattice Test Plugin");
    CHECK(device[ids::deviceType].toString() == pluginTypes::instrument);
    REQUIRE(w.shell->pluginDevices().liveDevice(LiveSet::idOf(device)) != nullptr);
    auto* frame = w.shell->deviceView().frameFor(device);
    REQUIRE(frame != nullptr);
    REQUIRE(frame->body() != nullptr);
    const auto titles = titlesIn(*frame->body());
    CHECK(titles.contains("Level"));
    CHECK(titles.contains("Wave"));
    CHECK_FALSE(titles.contains("Hidden"));
    CHECK(titles.contains("Configure"));

    // Taken off its pages, its knob goes.
    w.shell->pluginDevices().removeFromPages(device, "p20");
    w.shell->deviceView().flushUpdates();
    CHECK_FALSE(titlesIn(*frame->body()).contains("Wave"));
    w.shot(*w.shell, "6b-panel");
}

TEST_CASE("Walkthrough: 6b, a real plugin on a track, its panel and its window", "[.walkthrough][plugins]")
{
    const juce::File folder(juce::SystemStats::getEnvironmentVariable("LATTICE_PLUGIN_FOLDER", {}));
    const auto name = juce::SystemStats::getEnvironmentVariable("LATTICE_WALKTHROUGH_PLUGIN", "Surge XT");
    if (!folder.isDirectory())
    {
        // Not SKIP: its exception crashed while unwinding in some MSVC Release layouts (2026-11-08).
        WARN("Set LATTICE_PLUGIN_FOLDER to a folder of plugins");
        return;
    }
    Walk w;
    hosting::PluginSettings s;
    s.useDefaultFolders = false;
    s.extraFolders = {folder.getFullPathName()};
    w.shell->plugins().setSettings(s, false);
    w.shell->startPluginScan();
    w.settle();

    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto device = insertPlugin(w, name);
    WARN(name << " loaded in " << juce::String((juce::Time::getMillisecondCounterHiRes() - started) / 1000.0, 2) << " s");
    auto* live = w.shell->pluginDevices().liveDevice(LiveSet::idOf(device));
    REQUIRE(live != nullptr);
    WARN(live->numParameters() << " parameters; on its pages: "
                               << device.getChildWithName(ids::deviceState)[pluginTypes::configured].toString().length() << " characters of IDs; latency "
                               << live->latencySamples());
    w.shell->showDeviceView();
    w.shot(*w.shell, "6b-device");
    if (live->plugin().hasEditor())
    {
        REQUIRE(w.shell->pluginDevices().windows().open(*live, device));
        auto* window = w.shell->pluginDevices().windows().windowFor(LiveSet::idOf(device));
        REQUIRE(window != nullptr);
        WARN("Window " << window->getWidth() << " x " << window->getHeight() << ", its editor "
                       << window->getContentComponent()->getWidth() << " x " << window->getContentComponent()->getHeight()
                       << ", display scale " << juce::Desktop::getInstance().getDisplays().getDisplayForRect(window->getBounds())->scale);
        w.shot(*window->getContentComponent(), "6b-window");
        w.shell->pluginDevices().windows().close(LiveSet::idOf(device));
    }

    // Saved and loaded again, it's as it was.
    w.session.set().setDeviceParameter(device, live->parameter(0).id(), 0.123);
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lattice-walk-" + juce::Uuid().toString() + ".lset");
    REQUIRE(w.session.saveSet(file).wasOk());
    REQUIRE(w.session.loadSet(file).wasOk());
    w.shell->pluginDevices().finishLoading();
    const auto again = LiveSet::chainOf(w.session.set().track(0)).getChild(LiveSet::chainOf(w.session.set().track(0)).getNumChildren() - 1);
    auto* back = w.shell->pluginDevices().liveDevice(LiveSet::idOf(again));
    REQUIRE(back != nullptr);
    CHECK(back->plugin().getParameters()[0]->getValue() == Catch::Approx(0.123f).margin(0.001f));
    file.deleteFile();
}
