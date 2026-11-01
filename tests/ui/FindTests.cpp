// Find (docs/05-m4-ui.md, 4g; Tests, item 2): every parameter of every device type found by its name
// and by its group, on a track, a return, the master and inside a rack chain; the order of results;
// choosing one shows and flashes the control; mixer controls, settings and commands; the command
// palette; and a 32-track set with 64-macro racks searched within a frame.

#include "core/model/UndoNames.h"
#include "devices/racks/Rack.h"
#include "devices/racks/RackEditing.h"
#include "support/TestDevices.h"
#include "ui/shell/MainShell.h"

#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::ui;
using namespace lattice::test;
using Kind = FindIndex::Kind;

namespace
{
struct Fixture
{
    explicit Fixture(int tracks = 2)
    {
        settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("lattice-find", ".settings");
        juce::PropertiesFile::Options options;
        options.applicationName = "LatticeTests";
        settings = std::make_unique<juce::PropertiesFile>(settingsFile, options);
        session.newSet(tracks, 1);
        for (int t = 0; t < tracks; ++t)
        {
            auto chain = LiveSet::chainOf(session.set().track(t));
            while (chain.getNumChildren() > 0)
                session.set().removeDevice(chain.getChild(0));
        }
        session.flush();
        shell = std::make_unique<MainShell>(session, engine, *settings, tunings, ShellActions{});
        shell->setSize(1600, 1000);
    }
    ~Fixture()
    {
        shell.reset();
        settings.reset();
        settingsFile.deleteFile();
    }

    LiveSet& set() { return session.set(); }
    FindIndex& find() { return shell->findIndex(); }
    bool finds(const juce::String& text, const juce::ValueTree& device, const juce::String& parameterId, int selected = -1)
    {
        for (const auto& r : find().search(text, selected))
            if (r.kind == Kind::parameter && r.device == device && r.parameterId == parameterId)
                return true;
        return false;
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings};
    juce::File settingsFile;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<MainShell> shell;
};

/** Each parameter's group, as the device groups them (what Find shows). */
std::vector<juce::String> groupsOf(const StudioDevice& device)
{
    std::vector<juce::String> groups(static_cast<std::size_t>(device.numParameters()));
    for (const auto& g : device.parameterGroups())
        for (const int i : g.parameters)
            if (i >= 0 && i < device.numParameters() && groups[static_cast<std::size_t>(i)].isEmpty())
                groups[static_cast<std::size_t>(i)] = g.name;
    for (int i = 0; i < device.numParameters(); ++i)
        if (groups[static_cast<std::size_t>(i)].isEmpty())
            groups[static_cast<std::size_t>(i)] = device.parameter(i).spec().group;
    return groups;
}
} // namespace

TEST_CASE("Find: every parameter of every device type, by its name and by its group", "[ui][find]")
{
    const auto& registry = DeviceRegistry::builtin();
    std::vector<const DeviceTypeInfo*> types;
    for (const auto& type : registry.types())
        if (!type.hidden && !RackDevice::isRackType(type.id))
            types.push_back(&type);
    Fixture f(static_cast<int>(types.size()));
    DeviceSpecs specs(registry);
    for (std::size_t i = 0; i < types.size(); ++i)
    {
        const auto device = f.set().insertDevice(LiveSet::chainOf(f.set().track(static_cast<int>(i))), -1, types[i]->id);
        REQUIRE(device.isValid());
        const auto* spec = specs.forType(types[i]->id);
        const auto groups = groupsOf(*spec);
        for (int p = 0; p < spec->numParameters(); ++p)
        {
            const auto& parameter = spec->parameter(p).spec();
            INFO(types[i]->name << ": " << parameter.name << " (" << groups[static_cast<std::size_t>(p)] << ")");
            CHECK(f.finds(parameter.name, device, parameter.id));
            if (groups[static_cast<std::size_t>(p)].isNotEmpty())
                CHECK(f.finds(groups[static_cast<std::size_t>(p)] + " " + parameter.name, device, parameter.id));
        }
    }
}

TEST_CASE("Find: devices on a return, the master and inside a rack chain", "[ui][find]")
{
    Fixture f;
    auto& set = f.set();
    const auto onReturn = set.insertDevice(LiveSet::chainOf(set.returnTrack(0)), -1, "lattice.reverb");
    const auto onMaster = set.insertDevice(LiveSet::chainOf(set.master()), -1, "lattice.compressor");
    const auto rack = racks::insertRack(set, LiveSet::chainOf(set.track(0)), -1, RackDevice::instrumentRackId);
    const auto chain = racks::addChain(set, rack);
    const auto inRack = set.insertDevice(racks::devicesOf(chain), -1, "lattice.synth");
    DeviceSpecs specs(DeviceRegistry::builtin());
    for (const auto& device : {onReturn, onMaster, inRack})
    {
        const auto* spec = specs.forType(device[ids::deviceType].toString());
        const auto& first = spec->parameter(0).spec();
        INFO(device[ids::deviceType].toString() << ": " << first.name);
        CHECK(f.finds(first.name, device, first.id));
    }
    // The rack's own macros too.
    CHECK(f.finds("Macro 1", rack, "macro1"));

    // Inside a rack: its chain and the rack, as the undo history names them (undoNames).
    const auto& cutoff = specs.forType("lattice.synth")->findParameter("filter.cutoff")->spec();
    bool named = false;
    for (const auto& r : f.find().search("cutoff", -1))
        if (r.device == inRack)
            named = r.location == undoNames::locationOf(inRack, DeviceRegistry::builtin(), cutoff.group);
    CHECK(named);
    CHECK(undoNames::locationOf(inRack, DeviceRegistry::builtin()).contains(chain[ids::name].toString()));

    // Where it lives.
    const auto results = f.find().search("Macro 1", -1);
    REQUIRE(!results.empty());
    CHECK(results.front().location.contains("1 MIDI"));
}

TEST_CASE("Find: the selected track's results first, then other tracks', settings, commands", "[ui][find]")
{
    Fixture f(3);
    for (int t = 0; t < 3; ++t)
        f.set().insertDevice(LiveSet::chainOf(f.set().track(t)), -1, "lattice.compressor");
    const auto results = f.find().search("threshold", 2);
    REQUIRE(results.size() >= 3);
    CHECK(results[0].location.endsWith("3 MIDI"));
    CHECK(results[1].section == 1);

    // "volume": the mixer's, then the setting-less commands (none), and the Preferences entry for zoom isn't one.
    const auto volume = f.find().search("volume", 1);
    REQUIRE(!volume.empty());
    CHECK(volume.front().kind == Kind::mixer);
    CHECK(volume.front().location.endsWith("2 MIDI"));

    // Word starts, in any order: "amp rel" finds the Synth's amp envelope release.
    f.set().insertDevice(LiveSet::chainOf(f.set().track(0)), 0, "lattice.synth");
    bool ampRelease = false;
    for (const auto& r : f.find().search("amp rel", 0))
        ampRelease = ampRelease || (r.kind == Kind::parameter && r.name.containsIgnoreCase("release") && r.location.containsIgnoreCase("amp"));
    CHECK(ampRelease);

    // Settings and commands come last, in that order.
    const auto zoom = f.find().search("zoom", 0);
    REQUIRE(!zoom.empty());
    CHECK(zoom.front().kind == Kind::setting);
    const auto undo = f.find().search("undo", 0);
    bool command = false;
    for (const auto& r : undo)
        command = command || (r.kind == Kind::command && r.commandId == "edit.undo");
    CHECK(command);
    CHECK(f.find().search("undo", 0, true).front().kind == Kind::command); // the command palette
    for (const auto& r : f.find().search("e", 0, true))
        CHECK(r.kind == Kind::command);
}

TEST_CASE("Find: choosing a result shows the control and flashes it", "[ui][find]")
{
    Fixture f;
    auto& set = f.set();
    const auto synth = set.insertDevice(LiveSet::chainOf(set.track(1)), -1, "lattice.synth");
    f.session.flush();
    f.shell->setDetailVisible(true);
    f.shell->deviceView().flushUpdates();

    SECTION("a device's parameter: its track selected, the Device view showing it")
    {
        const auto results = f.find().search("Cutoff", -1);
        auto it = std::find_if(results.begin(), results.end(), [&](const auto& r) { return r.device == synth; });
        REQUIRE(it != results.end());
        f.shell->goTo(*it);
        CHECK(f.shell->selection().get().track == 1);
        CHECK(f.shell->detailView().page() == DetailView::Page::device);
        auto* control = f.shell->deviceView().flashedControl();
        REQUIRE(control != nullptr);
        CHECK(control->isVisible());
        const auto inView = f.shell->deviceView().getLocalArea(control, control->getLocalBounds());
        CHECK(f.shell->deviceView().getLocalBounds().contains(inView.getCentre()));
    }
    SECTION("a mixer control: its strip's widget, the mixer and its section on")
    {
        f.shell->sessionView().setIOVisible(false);
        const auto results = f.find().search("high notes", 1);
        REQUIRE(!results.empty());
        REQUIRE(results.front().kind == Kind::mixer);
        f.shell->goTo(results.front());
        CHECK(f.shell->sessionView().isIOVisible());
        auto* control = f.shell->flashedControl();
        REQUIRE(control != nullptr);
        auto* strip = f.shell->sessionView().stripForColumn(1);
        REQUIRE(strip != nullptr);
        CHECK(strip->isParentOf(control));
        CHECK(control == strip->tuningControls()->highNotesBox());
    }
    SECTION("a command runs")
    {
        const bool before = set.metronomeOn();
        const auto results = f.find().search("metronome", -1, true);
        REQUIRE(!results.empty());
        f.shell->goTo(results.front());
        CHECK(set.metronomeOn() != before);
    }
    SECTION("the box: typing, moving, Enter")
    {
        auto& box = f.shell->findView();
        f.shell->openFind(false);
        CHECK(box.isVisible());
        box.setText("solo");
        REQUIRE(!box.results().empty());
        box.moveSelection(1);
        CHECK(box.selectedRow() == 1);
        box.chooseSelected();
        CHECK(!box.isVisible());
    }
}

TEST_CASE("Find timing: a 32-track set with 64-macro racks answers within a frame", "[ui][find][timing]")
{
    Fixture f(32);
    auto& set = f.set();
    for (int t = 0; t < 32; ++t)
    {
        const auto rack = racks::insertRack(set, LiveSet::chainOf(set.track(t)), -1, RackDevice::instrumentRackId);
        racks::setMacroCount(set, rack, 64);
        set.insertDevice(racks::devicesOf(racks::addChain(set, rack)), -1, "lattice.synth");
        set.insertDevice(LiveSet::chainOf(set.track(t)), -1, "lattice.eq8");
    }
    std::vector<double> times;
    for (const auto* text : {"m", "ma", "mac", "macro", "macro 4", "cut", "cutoff", "release", "amp rel", "gain", "q", "vol"})
    {
        const auto start = juce::Time::getHighResolutionTicks();
        const auto results = f.find().search(text, 3, false, FindView::maxResults);
        times.push_back(juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - start));
        CHECK(!results.empty());
    }
    std::sort(times.begin(), times.end());
    const double median = times[times.size() / 2];
    WARN("Find over 32 tracks with 64-macro racks: median " << median * 1000.0 << " ms, worst " << times.back() * 1000.0 << " ms");
    CHECK(median < 0.008);
}
