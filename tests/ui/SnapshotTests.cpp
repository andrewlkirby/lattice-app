// Render snapshots (docs/05-m4-ui.md, Tests 5): the session grid, the Colundi piano-roll ruler,
// the EQ curve, the Drum Rack pads, the macro section and Virtual Push (docs/06-m5-controllers.md,
// Tests 6), from the window as the app builds it,
// in every theme at 100% and 200% zoom, compared with reference images (support/Snapshots.h).
//
// "[.ui-snapshot]" also writes the whole window and each component at 50%, 100%, 150%, 200% and
// 250% in every theme, drawn as the app draws (its own renderer), to LATTICE_SNAPSHOT_DIR, for
// looking at.

#include "devices/effects/EqEight.h"
#include "devices/racks/Rack.h"
#include "devices/racks/RackEditing.h"
#include "support/Snapshots.h"
#include "ui/shell/MainShell.h"

#include <catch2/catch_test_macros.hpp>

#include <functional>

using namespace lattice;
using namespace lattice::ui;
using namespace lattice::test;
using Kind = SessionSelection::Kind;

namespace
{
/** A window over a set with something in each component: clips (named, one playing state is
    left out, as it moves), a group, an armed track and a Colundi track; an EQ, a Drum Rack and
    a rack with 16 macros on their own tracks. */
struct Fixture
{
    Fixture()
    {
        settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("lattice-snapshots", ".settings");
        juce::PropertiesFile::Options options;
        options.applicationName = "LatticeTests";
        settings = std::make_unique<juce::PropertiesFile>(settingsFile, options);
        session.newSet(6, 5);
        auto& s = session.set();
        s.setTrackName(0, "Bass");
        s.setTrackName(1, "Colundi Lead");
        s.setTrackName(2, "Drums");
        s.setTrackName(3, "Keys");
        s.setTrackTuning(1, TuningRegistry::colundiId);
        for (const auto& [t, sc, name] : {std::tuple{0, 0, "Root"}, {0, 1, "Walk"}, {1, 0, "Theme"}, {1, 2, "Answer"},
                                          {2, 0, "Beat"}, {2, 1, "Fill"}, {3, 3, "Chords"}, {4, 0, "Pad"}})
        {
            auto clip = s.createClip(t, sc, 4.0);
            s.setClipProperty(clip, ids::name, name, false);
        }
        lead = s.createClip(1, 1, 4.0);
        s.setClipProperty(lead, ids::name, "Colundi", false);
        const int steps[] = {0, 3, 5, 7, 10, 12, 7, 5};
        for (int i = 0; i < 8; ++i)
            s.addNote(lead, {40 + steps[i], i * 0.5, 0.5, 40 + i * 12, false});
        s.setHighlight(lead, {40, 45, 47});
        s.groupTracks(4, 5);
        s.setTrackArm(3, true, false);
        s.setSceneName(0, "Intro");
        s.setSceneTempo(1, 98.0);

        const auto chain = [&s](int t) { return LiveSet::chainOf(s.track(t)); };
        auto eq = s.insertDevice(chain(0), -1, EqEightDevice::typeId);
        s.setDeviceParameter(eq, "band3.gain", 6.0);
        s.setDeviceParameter(eq, "band5.gain", -4.5);
        s.setDeviceParameter(eq, "band5.q", 3.0);
        auto drums = racks::insertRack(s, chain(2), -1, RackDevice::drumRackId);
        for (const auto& [note, name] : {std::pair{36, "Kick"}, {38, "Snare"}, {42, "Hat"}, {46, "Open Hat"}, {49, "Crash"}})
            racks::addDrumPad(s, drums, note).setProperty(ids::name, name, nullptr);
        auto rack = racks::insertRack(s, chain(3), -1, RackDevice::instrumentRackId);
        racks::setMacroCount(s, rack, 16);
        racks::setMacroName(s, rack, 0, "Brightness");
        racks::setMacroName(s, rack, 1, "Space");
        racks::setMacroColour(s, rack, 1, LiveSet::paletteColour(3));
        racks::setMacroColour(s, rack, 5, LiveSet::paletteColour(9));
        s.setDeviceParameter(rack, "macro1", 90.0);
        s.setDeviceParameter(rack, "macro6", 30.0);
        session.flush();

        shell = std::make_unique<MainShell>(session, engine, *settings, tunings, ShellActions{});
        shell->setSize(1600, 1000);
        shell->setBrowserVisible(false);
        shell->setDetailVisible(true);
        settle();
    }
    ~Fixture()
    {
        shell.reset();
        settings.reset();
        settingsFile.deleteFile();
    }

    void settle()
    {
        session.flush();
        shell->sessionView().flushUpdates();
        shell->deviceView().flushUpdates();
        shell->clipView().flushUpdates();
    }
    void showDevicesOf(int track)
    {
        shell->selection().set({Kind::track, track, 0});
        shell->showDeviceView();
        settle();
    }
    /** The first showing component under root with this title. */
    static juce::Component* titled(juce::Component& root, const juce::String& title)
    {
        for (auto* child : root.getChildren())
        {
            if (child->getTitle() == title && child->isVisible())
                return child;
            if (auto* found = titled(*child, title))
                return found;
        }
        return nullptr;
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings};
    juce::File settingsFile;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<MainShell> shell;
    juce::ValueTree lead;
};

/** A component of the window, made ready (shown, laid out) and given as what to draw: a
    component and an area of it. It's drawn from the window, so what's behind it is too. */
struct Subject
{
    const char* name;
    std::function<std::pair<juce::Component*, juce::Rectangle<int>>(Fixture&)> prepare;
    /** The window's area that shows it, or an empty one if it isn't there. */
    juce::Rectangle<int> in(Fixture& f) const
    {
        const auto [component, area] = prepare(f);
        return component != nullptr && !area.isEmpty() ? f.shell->getLocalArea(component, area) : juce::Rectangle<int>();
    }
};

const std::vector<Subject>& subjects()
{
    static const std::vector<Subject> all{
        {"session-grid",
         [](Fixture& f) {
             f.shell->setVirtualPushVisible(false); // the last subject shows it
             f.shell->selection().set({Kind::slot, 1, 1});
             f.settle();
             auto& view = f.shell->sessionView();
             const auto area = view.headerArea().getUnion(view.slotsArea()).getUnion(view.sceneColumn()).getUnion(view.stopArea());
             return std::pair<juce::Component*, juce::Rectangle<int>>{&view, area};
         }},
        {"colundi-ruler",
         [](Fixture& f) {
             f.shell->selection().set({Kind::slot, 1, 1});
             f.shell->showClipView();
             f.settle();
             auto& roll = f.shell->clipView().pianoRoll();
             roll.zoomToClip();
             f.settle();
             return std::pair<juce::Component*, juce::Rectangle<int>>{&roll, roll.rulerArea()};
         }},
        {"eq-curve",
         [](Fixture& f) {
             f.showDevicesOf(0);
             auto* curve = Fixture::titled(f.shell->deviceView(), "EQ curve");
             return std::pair<juce::Component*, juce::Rectangle<int>>{curve, curve != nullptr ? curve->getLocalBounds() : juce::Rectangle<int>()};
         }},
        {"drum-pads",
         [](Fixture& f) {
             f.showDevicesOf(2);
             auto* pads = Fixture::titled(f.shell->deviceView(), "Pads");
             return std::pair<juce::Component*, juce::Rectangle<int>>{pads, pads != nullptr ? pads->getLocalBounds() : juce::Rectangle<int>()};
         }},
        {"macros",
         [](Fixture& f) {
             f.showDevicesOf(3);
             auto* macros = Fixture::titled(f.shell->deviceView(), "Macros");
             return std::pair<juce::Component*, juce::Rectangle<int>>{macros, macros != nullptr ? macros->getLocalBounds() : juce::Rectangle<int>()};
         }},
        {"virtual-push",
         [](Fixture& f) {
             // Docked, 8 x 8, with the set's clips and the armed track's empty slots lit, and track 2
             // selected (its button white).
             f.shell->setVirtualPushDocked(true);
             f.shell->setVirtualPushVisible(true);
             f.shell->selection().set({Kind::slot, 2, 0});
             f.settle();
             auto* push = f.shell->virtualPushView();
             push->setSize(push->getWidth(), push->getHeight()); // laid out
             f.shell->virtualPushController()->refresh();
             return std::pair<juce::Component*, juce::Rectangle<int>>{push, push->getLocalBounds()};
         }},
    };
    return all;
}

juce::String themeTag(const Theme& theme)
{
    return theme.name.removeCharacters(" ").toLowerCase();
}
} // namespace

TEST_CASE("Snapshots: the key components match their reference images in every theme at 100% and 200%", "[ui][snapshot]")
{
    Fixture f;
    for (const auto& subject : subjects())
    {
        CAPTURE(subject.name);
        for (const auto* theme : {&Theme::dark(), &Theme::light(), &Theme::highContrast()})
        {
            f.shell->setTheme(*theme);
            const auto area = subject.in(f);
            REQUIRE(!area.isEmpty());
            REQUIRE(f.shell->getLocalBounds().contains(area)); // all of it in the window
            for (const float scale : {1.0f, 2.0f})
            {
                const auto name = juce::String(subject.name) + "-" + themeTag(*theme) + "-" + juce::String(juce::roundToInt(scale * 100));
                const auto problem = compareWithReference(snapshot(*f.shell, area, scale), name);
                INFO(problem);
                CHECK(problem.isEmpty());
            }
        }
    }
}

TEST_CASE("Snapshots: the window and its key components at every zoom, for looking at", "[.ui-snapshot]")
{
    const juce::File dir(juce::SystemStats::getEnvironmentVariable(
        "LATTICE_SNAPSHOT_DIR", juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName()));
    Fixture f;
    const auto write = [&dir](juce::Component& c, juce::Rectangle<int> area, float scale, const juce::String& name) {
        REQUIRE(writePng(c.createComponentSnapshot(area, true, scale), dir.getChildFile(name + ".png")));
    };
    for (const auto* theme : {&Theme::dark(), &Theme::light(), &Theme::highContrast()})
    {
        f.shell->setTheme(*theme);
        for (const float scale : {0.5f, 1.0f, 1.5f, 2.0f, 2.5f})
        {
            const auto zoom = juce::String(juce::roundToInt(scale * 100));
            for (const auto& subject : subjects())
            {
                const auto area = subject.in(f);
                REQUIRE(!area.isEmpty());
                write(*f.shell, area, scale, juce::String(subject.name) + "-" + themeTag(*theme) + "-" + zoom);
            }
            for (const int page : {0, 1})
            {
                if (page == 0)
                    f.showDevicesOf(3);
                else
                {
                    f.shell->selection().set({Kind::slot, 1, 1});
                    f.shell->showClipView();
                    f.settle();
                }
                write(*f.shell, f.shell->getLocalBounds(), scale,
                      "window-" + juce::String(page == 0 ? "devices" : "clip") + "-" + themeTag(*theme) + "-" + zoom);
            }
        }
    }
    WARN("Wrote " << dir.getFullPathName());
}
