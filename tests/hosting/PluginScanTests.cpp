// The plugin scan (docs/07-m6-plugins-external.md, 6a; "Tests" 1): the scanner's protocol, CLAP
// files described, crashing and hanging plugins blocked with the right reason while the scan goes
// on, incremental rescans, and what the browser's Plugins category shows.

#include "hosting/PluginFormats.h"
#include "hosting/PluginLibrary.h"
#include "hosting/clap/ClapFormat.h"
#include "hosting/scan/PluginScanner.h"
#include "hosting/scan/ScanProtocol.h"
#include "library/LibraryIndex.h"
#include "support/TestDevices.h"
#include "support/TestFiles.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace lattice;
using namespace lattice::hosting;
using namespace lattice::test;
using library::Item;
using library::Kind;
using library::PluginFile;
using scan::PluginScanner;

namespace
{
const juce::File testClap{LATTICE_TEST_CLAP};
const juce::File crashClap{LATTICE_CRASH_CLAP};
const juce::File hangClap{LATTICE_HANG_CLAP};
const juce::File crashVst3{LATTICE_CRASH_VST3};
const juce::File hangVst3{LATTICE_HANG_VST3};
const juce::File scannerExe{LATTICE_PLUGIN_SCANNER};

/** The JUCE-built VST3's bundle: two folders above its binary. */
juce::File testVst3() { return juce::File(LATTICE_TEST_VST3_BINARY_DIR).getParentDirectory().getParentDirectory(); }

/** Copies a plugin file or bundle into a folder. */
juce::File copyInto(const juce::File& plugin, const juce::File& folder)
{
    const auto to = folder.getChildFile(plugin.getFileName());
    if (plugin.isDirectory())
        REQUIRE(plugin.copyDirectoryTo(to));
    else
        REQUIRE(plugin.copyFileTo(to));
    return to;
}

const scan::ScannedPlugin* named(const std::vector<scan::ScannedPlugin>& plugins, const juce::String& name)
{
    for (const auto& p : plugins)
        if (p.description.name == name)
            return &p;
    return nullptr;
}

const Item* itemNamed(const std::vector<Item>& items, const juce::String& name)
{
    for (const auto& i : items)
        if (i.name == name)
            return &i;
    return nullptr;
}
} // namespace

TEST_CASE("The scanner's lines read back as written, whatever the text holds", "[hosting]")
{
    for (const juce::String text : {"plain", "tab\there", "two\nlines", "back\\slash\\t", "\r\n\t\\", ""})
        CHECK(scan::unescape(scan::escape(text)) == text);

    const std::vector<scan::Job> jobs{{"VST3", "C:/Plugins/A.vst3"}, {"CLAP", "/odd\tname/with\nnewline.clap"}, {"LV2", "\\\\server\\b.lv2"}};
    CHECK(scan::readJobs(scan::writeJobs(jobs)) == jobs);

    scan::ScannedPlugin p;
    p.description.name = "Odd \"Name\" <&>";
    p.description.pluginFormatName = "CLAP";
    p.description.manufacturerName = "Vendor\tWith Tab";
    p.description.fileOrIdentifier = "/a/b.clap";
    p.description.category = "Instrument|Synthesizer";
    p.description.isInstrument = true;
    p.description.numOutputChannels = 2;
    p.description.uniqueId = 1234;
    p.midiInputs = 1;
    p.midiOutputs = 0;
    p.mpe = 1;
    const auto line = scan::parseLine(scan::pluginLine(7, p));
    REQUIRE(line);
    CHECK(line->type == scan::Line::Type::plugin);
    CHECK(line->job == 7);
    REQUIRE(line->plugin);
    CHECK(line->plugin->description.name == p.description.name);
    CHECK(line->plugin->description.manufacturerName == p.description.manufacturerName);
    CHECK(line->plugin->description.isInstrument);
    CHECK(line->plugin->description.numOutputChannels == 2);
    CHECK(line->plugin->key() == p.key());
    CHECK(line->plugin->midiInputs == 1);
    CHECK(line->plugin->midiOutputs == 0);
    CHECK(line->plugin->mpe == 1);

    const auto fail = scan::parseLine(scan::failLine(3, "can't\tread it"));
    REQUIRE(fail);
    CHECK(fail->type == scan::Line::Type::fail);
    CHECK(fail->job == 3);
    CHECK(fail->text == "can't\tread it");
    CHECK(scan::parseLine(scan::beginLine(12))->job == 12);
    CHECK(scan::parseLine(scan::endLine(12))->type == scan::Line::Type::end);
    CHECK(scan::parseLine(scan::crashLine(4, "access violation"))->text == "access violation");
    CHECK(scan::parseLine(scan::doneLine())->type == scan::Line::Type::done);

    // Damaged lines (a crash part way through one) are skipped, not misread.
    CHECK_FALSE(scan::parseLine("plugin\t3\t<PLUGIN name=\"cut off"));
    CHECK_FALSE(scan::parseLine("begin\tx"));
    CHECK_FALSE(scan::parseLine("something else"));
    CHECK_FALSE(scan::parseLine(""));
}

TEST_CASE("A CLAP file's plugins are described from their descriptors and ports", "[hosting]")
{
    juce::String error;
    const auto plugins = ClapPluginFormat::scanFile(testClap.getFullPathName(), error);
    INFO(error);
    REQUIRE(plugins.size() == 2);

    const auto* synth = named(plugins, "Lattice Test Plugin");
    REQUIRE(synth != nullptr);
    const auto& s = synth->description;
    CHECK(s.pluginFormatName == "CLAP");
    CHECK(s.manufacturerName == "Lattice Tests");
    CHECK(s.version == "1.2.3");
    CHECK(s.descriptiveName == "A CLAP synth that makes no sound");
    CHECK(s.category == "Instrument|Synthesizer|Stereo");
    CHECK(s.isInstrument);
    CHECK(s.numInputChannels == 0);
    CHECK(s.numOutputChannels == 2);
    CHECK(s.fileOrIdentifier == testClap.getFullPathName());
    CHECK(s.uniqueId == ClapPluginFormat::uniqueIdOf("org.lattice.tests.clap-synth"));
    CHECK(synth->midiInputs == 1);
    CHECK(synth->midiOutputs == 0);
    CHECK(synth->mpe == 1);

    const auto* arp = named(plugins, "Lattice Test Clap Arp");
    REQUIRE(arp != nullptr);
    CHECK(arp->description.category == "Note Effect|Arpeggiator");
    CHECK_FALSE(arp->description.isInstrument);
    CHECK(arp->description.numInputChannels == 0);
    CHECK(arp->description.numOutputChannels == 0);
    CHECK(arp->midiInputs == 1);
    CHECK(arp->midiOutputs == 1);
    CHECK(arp->mpe == 0);
    CHECK(arp->key() != synth->key()); // two plugins in one file, told apart

    // The id's number is the same on every machine: FNV-1a of its UTF-8.
    CHECK(ClapPluginFormat::uniqueIdOf("") == static_cast<int>(2166136261u));
    CHECK(ClapPluginFormat::uniqueIdOf("a") == static_cast<int>(0xe40c292cu));

    // Not a CLAP.
    TempFolder folder;
    const auto notClap = folder / "Not.clap";
    REQUIRE(notClap.replaceWithText("not a library"));
    CHECK(ClapPluginFormat::scanFile(notClap.getFullPathName(), error).empty());
    CHECK(error.isNotEmpty());
}

TEST_CASE("CLAP files are found in folders and their subfolders", "[hosting]")
{
    TempFolder folder;
    copyInto(testClap, folder.folder);
    REQUIRE((folder / "Vendor/Deeper").createDirectory());
    copyInto(testClap, folder / "Vendor/Deeper");
    REQUIRE((folder / "readme.txt").replaceWithText("no"));
    ClapPluginFormat format;
    const auto found = format.searchPathsForPlugins(juce::FileSearchPath(folder.folder.getFullPathName()), true, false);
    CHECK(found.size() == 2);
    CHECK(format.searchPathsForPlugins(juce::FileSearchPath(folder.folder.getFullPathName()), false, false).size() == 1);
    CHECK(hostedFormatNames()[0] == "CLAP");
    CHECK(hostedFormatNames().contains("VST3"));
}

TEST_CASE("A plugin that crashes or hangs the scanner is caught, and the scan goes on", "[hosting]")
{
    TempFolder work;
    const auto notClap = work / "Not a plugin.clap";
    REQUIRE(notClap.replaceWithText("not a library"));
    const std::vector<scan::Job> jobs{
        {"CLAP", testClap.getFullPathName()},  {"CLAP", crashClap.getFullPathName()}, {"VST3", testVst3().getFullPathName()},
        {"CLAP", hangClap.getFullPathName()},  {"VST3", crashVst3.getFullPathName()}, {"CLAP", notClap.getFullPathName()},
        {"VST3", hangVst3.getFullPathName()},  {"CLAP", testClap.getFullPathName()},  {"Unknown", "x"},
    };
    PluginScanner scanner({scannerExe, work.folder, 1.5});
    std::vector<PluginScanner::Result> results;
    REQUIRE(scanner.run(jobs, [&](const PluginScanner::Result& r) { results.push_back(r); }));
    INFO(scanner.error());
    REQUIRE(results.size() == jobs.size());
    for (std::size_t i = 0; i < jobs.size(); ++i)
        CHECK(results[i].job == jobs[i]);

    using O = PluginScanner::Outcome;
    CHECK(results[0].outcome == O::found);
    CHECK(results[0].plugins.size() == 2);
    CHECK(results[1].outcome == O::crashed);
    CHECK(results[2].outcome == O::found);
    REQUIRE(results[2].plugins.size() == 1);
    CHECK(results[2].plugins[0].description.name == "Lattice Test Plugin");
    CHECK(results[2].plugins[0].description.pluginFormatName == "VST3");
    CHECK(results[2].plugins[0].description.isInstrument);
    CHECK(results[3].outcome == O::hung);
    CHECK(results[3].reason == "took longer than 1.5 s");
    CHECK(results[4].outcome == O::crashed);
    CHECK(results[5].outcome == O::failed);
    CHECK(results[5].reason.isNotEmpty());
    CHECK(results[6].outcome == O::hung);
    CHECK(results[7].outcome == O::found); // the same file again, after all that
    CHECK(results[8].outcome == O::failed);
    CHECK(results[8].reason.contains("Unknown"));

    // How the crashes went.
    for (const auto i : {1, 4})
    {
        INFO(results[static_cast<std::size_t>(i)].reason);
#if JUCE_WINDOWS
        CHECK(results[static_cast<std::size_t>(i)].reason.contains("access violation"));
#else
        CHECK(results[static_cast<std::size_t>(i)].reason.contains("segmentation fault"));
#endif
    }
    // One scanner, and one more after each crash or hang.
    CHECK(scanner.processesStarted() == 5);
    // Its files are cleared away.
    CHECK(work.folder.findChildFiles(juce::File::findFilesAndDirectories, false).size() == 1); // the not-plugin
}

TEST_CASE("A scan that can't start says why", "[hosting]")
{
    TempFolder work;
    PluginScanner scanner({work / "missing scanner", work.folder, 1.0});
    int calls = 0;
    CHECK_FALSE(scanner.run({{"CLAP", testClap.getFullPathName()}}, [&](const PluginScanner::Result&) { ++calls; }));
    CHECK(calls == 0);
    CHECK(scanner.error().contains("missing"));
    CHECK(scanner.run({}, {})); // nothing to do
}

TEST_CASE("The plugin library scans what changed, blocks what crashes, and fills the browser's Plugins", "[hosting]")
{
    TempFolder plugins, work;
    const auto clap = copyInto(testClap, plugins.folder);
    copyInto(testVst3(), plugins.folder);
    const auto crash = copyInto(crashClap, plugins.folder);

    library::LibraryIndex index({}, testRegistry());
    PluginLibrary lib(index, scannerExe, work.folder);
    PluginSettings settings;
    settings.useDefaultFolders = false; // not the machine's own plugins
    settings.extraFolders.add(plugins.folder.getFullPathName());
    settings.timeLimitSeconds = 5.0;
    lib.setSettings(settings);
    REQUIRE(lib.waitForTests(60000));

    const auto status = lib.status();
    INFO(status.lastError);
    CHECK_FALSE(status.scanning);
    CHECK(status.total == 3);
    CHECK(status.done == 3);
    CHECK(index.storedPlugins().size() == 3); // two in the CLAP file, one VST3
    CHECK(status.plugins == 2);               // the test plugin, in two formats, counted once
    CHECK(status.formats == "CLAP 2, VST3 1");

    // The crash is blocked, with why.
    const auto problems = lib.problems();
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].identifier == crash.getFullPathName());
    CHECK(problems[0].status == PluginFile::Status::crashed);
    CHECK(problems[0].reason.isNotEmpty());

    // The browser: the Plugins category lists the vendors' folders, which open on their plugins. The
    // test plugin comes as a CLAP and a VST3, and shows once, as the CLAP.
    library::Query groups;
    groups.category = library::category::plugins;
    groups.kind = static_cast<int>(Kind::folder);
    auto folders = index.find(groups);
    REQUIRE(folders.size() == 1);
    CHECK(folders[0].name == "Lattice Tests");
    library::Query inside;
    inside.folder = folders[0].key;
    auto shown = index.find(inside);
    CHECK(shown.size() == 2);
    const auto* plugin = itemNamed(shown, "Lattice Test Plugin");
    REQUIRE(plugin != nullptr);
    CHECK(plugin->kind == Kind::plugin);
    CHECK(plugin->detail == "Lattice Tests \xc2\xb7 CLAP \xc2\xb7 Instrument");
    const auto* arp = itemNamed(shown, "Lattice Test Clap Arp");
    REQUIRE(arp != nullptr);
    CHECK(arp->detail.endsWith("MIDI Effect"));
    // Searchable.
    library::Query search;
    search.text = "lattice test plugin";
    CHECK(itemNamed(index.find(search), "Lattice Test Plugin") != nullptr);

    SECTION("Nothing changed: nothing is scanned")
    {
        lib.scan();
        REQUIRE(lib.waitForTests(30000));
        CHECK(lib.status().total == 0);
        CHECK(index.storedPlugins().size() == 3);
        CHECK(lib.problems().size() == 1); // still blocked, and not tried again
    }
    SECTION("A changed file is scanned again, alone")
    {
        REQUIRE(clap.setLastModificationTime(clap.getLastModificationTime() + juce::RelativeTime::seconds(10)));
        lib.scan();
        REQUIRE(lib.waitForTests(30000));
        CHECK(lib.status().total == 1);
        CHECK(index.storedPlugins().size() == 3);
    }
    SECTION("Rescanning everything leaves out what's blocked; a retry doesn't")
    {
        lib.scan(true);
        REQUIRE(lib.waitForTests(30000));
        CHECK(lib.status().total == 2);
        lib.retry(lib.problems());
        REQUIRE(lib.waitForTests(30000));
        CHECK(lib.status().total == 1);
        REQUIRE(lib.problems().size() == 1); // it crashes again
        CHECK(lib.problems()[0].status == PluginFile::Status::crashed);
    }
    SECTION("Files gone from the disk are forgotten")
    {
        REQUIRE(crash.deleteFile());
        REQUIRE(clap.deleteFile());
        lib.scan();
        REQUIRE(lib.waitForTests(30000));
        CHECK(lib.problems().empty());
        CHECK(index.storedPlugins().size() == 1);
        inside.folder = index.find(groups).at(0).key;
        shown = index.find(inside);
        REQUIRE(shown.size() == 1);
        CHECK(shown[0].detail == "Lattice Tests \xc2\xb7 VST3 \xc2\xb7 Instrument"); // the VST3 now
    }
    SECTION("The preferred format, and the grouping, change what's shown")
    {
        settings.preferredFormat = "VST3";
        settings.grouping = PluginGrouping::format;
        lib.setSettings(settings);
        REQUIRE(lib.waitForTests(30000));
        CHECK(lib.status().total == 3); // still the first scan's: no scan for that (one would find nothing changed)
        folders = index.find(groups);
        REQUIRE(folders.size() == 2);
        CHECK(folders[0].name == "CLAP");
        CHECK(folders[1].name == "VST3");
        inside.folder = folders[1].key;
        shown = index.find(inside);
        REQUIRE(shown.size() == 1);
        CHECK(shown[0].name == "Lattice Test Plugin");
        // Grouped by format, each format's folder has all of its own, the preferred or not.
        inside.folder = folders[0].key;
        shown = index.find(inside);
        CHECK(shown.size() == 2);
        CHECK(itemNamed(shown, "Lattice Test Plugin") != nullptr);

        settings.grouping = PluginGrouping::type;
        lib.setSettings(settings);
        REQUIRE(lib.waitForTests(30000));
        folders = index.find(groups);
        REQUIRE(folders.size() == 2);
        CHECK(folders[0].name == "Instruments");
        CHECK(folders[1].name == "MIDI Effects");
        inside.folder = folders[0].key;
        shown = index.find(inside);
        REQUIRE(shown.size() == 1); // once again, as the VST3
        CHECK(shown[0].detail.contains("VST3"));
    }
    SECTION("What's built in being set again leaves the plugins")
    {
        index.setBuiltIns({});
        CHECK(index.find(groups).size() == 1);
        CHECK(index.pluginFiles().size() == 3);
    }
}

#if defined(LATTICE_TEST_LV2)
TEST_CASE("LV2 plugins are scanned on Linux", "[hosting]")
{
    TempFolder plugins, work;
    copyInto(juce::File(LATTICE_TEST_LV2), plugins.folder);
    library::LibraryIndex index({}, testRegistry());
    PluginLibrary lib(index, scannerExe, work.folder);
    PluginSettings settings;
    settings.useDefaultFolders = false;
    settings.extraFolders.add(plugins.folder.getFullPathName());
    lib.setSettings(settings);
    REQUIRE(lib.waitForTests(60000));
    INFO(lib.status().lastError);
    // (JUCE's LV2 format also reads the standard folders, so the machine's own LV2s may be there too.)
    const auto stored = index.storedPlugins();
    CHECK(std::any_of(stored.begin(), stored.end(), [](const library::StoredPlugin& p) {
        return p.format == "LV2" && p.xml.contains("Lattice Test Plugin");
    }));
}
#endif

TEST_CASE("A bundle's change is seen in its files, not its resources", "[hosting]")
{
    TempFolder folder;
    const auto bundle = folder / "Thing.vst3";
    REQUIRE(bundle.getChildFile("Contents/x86_64-win").createDirectory());
    REQUIRE(bundle.getChildFile("Contents/Resources").createDirectory());
    REQUIRE(bundle.getChildFile("Contents/x86_64-win/Thing.vst3").replaceWithText("code"));
    const auto before = PluginLibrary::stampOf(bundle.getFullPathName());
    REQUIRE(bundle.getChildFile("Contents/Resources/big.png").replaceWithText("a picture"));
    CHECK(PluginLibrary::stampOf(bundle.getFullPathName()).first == before.first);
    REQUIRE(bundle.getChildFile("Contents/x86_64-win/Thing.vst3").replaceWithText("new code"));
    CHECK(PluginLibrary::stampOf(bundle.getFullPathName()).first != before.first);
    CHECK(PluginLibrary::stampOf("urn:not:a:file") == std::pair<juce::int64, juce::int64>{0, 0});
}
