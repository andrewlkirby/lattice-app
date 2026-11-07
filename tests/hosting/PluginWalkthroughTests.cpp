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
        SKIP("Set LATTICE_PLUGIN_FOLDER to a folder of plugins");
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
