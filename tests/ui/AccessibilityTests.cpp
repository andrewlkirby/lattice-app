// Accessibility (docs/05-m4-ui.md, 4h): every control a screen reader reaches has a name; the
// views that draw their own items say what's selected; F6 and Shift+F6 move between the window's
// areas; Tab and Shift+Tab move through controls (Shift+Tab stays Live's Clip/Device switch in the
// views). Keys go through the window's peer, as Windows sends them.

#include "support/TestDevices.h"
#include "ui/shell/MainShell.h"
#include "ui/widgets/Accessible.h"
#include "ui/widgets/BoundControl.h"

#include <catch2/catch_test_macros.hpp>

#include <functional>

using namespace lattice;
using namespace lattice::ui;
using namespace lattice::test;

namespace
{
struct Fixture
{
    Fixture()
    {
        settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lattice-accessibility-" + juce::Uuid().toString() + ".settings");
        juce::PropertiesFile::Options options;
        options.applicationName = "LatticeTests";
        settings = std::make_unique<juce::PropertiesFile>(settingsFile, options);
        session.newSet(2, 3);
        session.set().setTrackTuning(1, TuningRegistry::colundiId);
        auto clip = session.set().createClip(0, 1, 4.0);
        session.set().setClipProperty(clip, ids::name, "Bass", false);
        session.set().addNote(clip, {48, 1.0, 0.5, 90, false});
        session.set().addNote(clip, {55, 2.0, 0.5, 100, false});
        session.flush();
        shell = std::make_unique<MainShell>(session, engine, *settings, tunings, ShellActions{});
        shell->setSize(1600, 1000);
        shell->setBrowserVisible(true);
        shell->setDetailVisible(true);
        shell->setUndoHistoryVisible(true);
        // The handlers exist only for components in a window.
        shell->addToDesktop(juce::ComponentPeer::windowIsTemporary);
        shell->setVisible(true);
        settle();
    }
    ~Fixture()
    {
        shell.reset();
        settings.reset();
        settingsFile.deleteFile();
    }

    LiveSet& set() { return session.set(); }
    void settle()
    {
        session.flush();
        shell->sessionView().flushUpdates();
        shell->deviceView().flushUpdates();
        shell->clipView().flushUpdates();
        shell->undoHistory().refresh();
    }
    /** A key, as the window's peer gets it. */
    bool press(const juce::KeyPress& key)
    {
        settle();
        auto* peer = shell->getPeer();
        REQUIRE(peer != nullptr);
        const bool taken = peer->handleKeyPress(key);
        settle();
        return taken;
    }
    static juce::Component* focused() { return juce::Component::getCurrentlyFocusedComponent(); }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings};
    juce::File settingsFile;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<MainShell> shell;
};

const juce::KeyPress f6{juce::KeyPress::F6Key};
const juce::KeyPress shiftF6{juce::KeyPress::F6Key, juce::ModifierKeys::shiftModifier, 0};
const juce::KeyPress tab{juce::KeyPress::tabKey};
const juce::KeyPress shiftTab{juce::KeyPress::tabKey, juce::ModifierKeys::shiftModifier, 0};

/** The controls a screen reader stops at (and reads by name) that have none, by their path. */
juce::StringArray unnamedControls(juce::Component& root)
{
    juce::StringArray unnamed;
    std::function<void(juce::Component&, const juce::String&)> visit = [&](juce::Component& c, const juce::String& path) {
        if (!c.isShowing())
            return;
        const auto here = path + "/" + (c.getTitle().isNotEmpty() ? c.getTitle() : juce::String(typeid(c).name()));
        if (auto* handler = c.getAccessibilityHandler(); handler != nullptr && !handler->isIgnored())
        {
            using R = juce::AccessibilityRole;
            const auto role = handler->getRole();
            const bool interactive = role == R::button || role == R::toggleButton || role == R::radioButton || role == R::comboBox ||
                                     role == R::slider || role == R::editableText || role == R::list || role == R::table ||
                                     role == R::tree || role == R::scrollBar || c.getWantsKeyboardFocus();
            if (interactive && role != R::scrollBar && handler->getTitle().trim().isEmpty() && handler->getHelp().trim().isEmpty())
                unnamed.add(here);
        }
        for (auto* child : c.getChildren())
            visit(*child, here);
    };
    visit(root, {});
    return unnamed;
}
} // namespace

TEST_CASE("Accessibility: every control a screen reader reaches has a name, in every view", "[ui][accessibility]")
{
    Fixture f;
    const auto chain = LiveSet::chainOf(f.set().track(0));
    f.set().insertDevice(chain, -1, "lattice.synth");
    f.set().insertDevice(chain, -1, "lattice.reverb");
    f.settle();
    const auto check = [&](const char* view) {
        f.settle();
        const auto unnamed = unnamedControls(*f.shell);
        INFO(view << ": " << unnamed.joinIntoString("\n"));
        CHECK(unnamed.isEmpty());
    };
    f.shell->selection().set({SessionSelection::Kind::slot, 0, 1});
    f.shell->showClipView();
    check("Session, Clip view");
    f.shell->selection().set({SessionSelection::Kind::track, 0, 0});
    f.shell->showDeviceView();
    check("Device view (Synth, Reverb)");
    f.shell->selection().set({SessionSelection::Kind::track, 1, 0});
    f.shell->showTuningView();
    check("Tuning view");
    f.shell->openFind(false);
    f.shell->findView().setText("cut");
    check("Find");
}

TEST_CASE("Accessibility: the views say what's selected", "[ui][accessibility]")
{
    Fixture f;
    auto& sessionView = f.shell->sessionView();

    SECTION("Session grid")
    {
        f.shell->selection().set({SessionSelection::Kind::slot, 0, 1});
        CHECK(accessible::valueOf(sessionView).startsWith("Bass"));
        CHECK(accessible::valueOf(sessionView).contains("Scene 2"));
        f.shell->selection().set({SessionSelection::Kind::slot, 1, 0});
        CHECK(accessible::valueOf(sessionView).startsWith("Empty slot"));
        f.shell->selection().set(SessionSelection{SessionSelection::Kind::slot, 0, 0}.extendedTo(1, 2));
        CHECK(accessible::valueOf(sessionView) == "6 slots selected, 2 tracks by 3 scenes");
        f.shell->selection().set({SessionSelection::Kind::scene, 0, 2});
        CHECK(accessible::valueOf(sessionView) == "Scene 3");
        auto* handler = sessionView.getAccessibilityHandler();
        REQUIRE(handler != nullptr);
        CHECK(handler->getTitle() == "Session view");
        CHECK(handler->getRole() == juce::AccessibilityRole::table);
    }
    SECTION("Piano roll")
    {
        f.shell->selection().set({SessionSelection::Kind::slot, 0, 1});
        f.shell->showClipView();
        f.settle();
        auto& roll = f.shell->clipView().pianoRoll();
        roll.notes().clearSelection();
        CHECK(accessible::valueOf(roll) == "2 notes, none selected");
        roll.notes().select({0}, false);
        CHECK(accessible::valueOf(roll) == "C3, velocity 90, at 1.2.1, length 0.0.2");
        roll.notes().select({1}, true);
        CHECK(accessible::valueOf(roll) == "2 of 2 notes selected");
    }
    SECTION("Device view")
    {
        const auto chain = LiveSet::chainOf(f.set().track(0));
        f.set().insertDevice(chain, -1, "lattice.synth");
        const auto reverb = f.set().insertDevice(chain, -1, "lattice.reverb");
        f.shell->selection().set({SessionSelection::Kind::track, 0, 0});
        f.shell->showDeviceView();
        f.settle();
        f.shell->deviceView().select(reverb);
        INFO(accessible::valueOf(f.shell->deviceView()));
        CHECK(accessible::valueOf(f.shell->deviceView()).startsWith("Reverb"));
        const auto count = juce::String(chain.getNumChildren());
        CHECK(accessible::valueOf(f.shell->deviceView()).endsWith(", " + count + " of " + count));
        f.set().setDeviceEnabled(reverb, false);
        CHECK(accessible::valueOf(f.shell->deviceView()).contains(", off,"));
    }
    SECTION("Tuning table")
    {
        f.shell->selection().set({SessionSelection::Kind::track, 1, 0});
        f.shell->showTuningView();
        f.shell->tuningView().flushUpdates();
        f.shell->tuningView().selectEntries({47});
        const auto value = accessible::valueOf(f.shell->tuningView().tableComponent());
        INFO(value);
        CHECK(value.startsWith("Entry 47, Hz 440"));
        CHECK(value.contains("Hz"));
        f.shell->tuningView().selectEntries({1, 2, 3});
        CHECK(accessible::valueOf(f.shell->tuningView().tableComponent()) == "3 entries selected");
    }
    SECTION("Undo history")
    {
        auto& history = f.shell->undoHistory();
        f.session.undoManager().clearUndoHistory();
        f.settle();
        CHECK(accessible::valueOf(history) == "Start, no changes yet");
        f.session.undoManager().beginNewTransaction("Rename track");
        f.set().setTrackName(0, "Lead");
        f.session.undoManager().beginNewTransaction("Rename track");
        f.set().setTrackName(1, "Pad");
        f.settle();
        CHECK(accessible::valueOf(history) == "Rename track, step 2 of 2");
        // Up steps back (undoes), Down forward.
        history.grabKeyboardFocus();
        REQUIRE(f.press(juce::KeyPress(juce::KeyPress::upKey)));
        CHECK(f.set().track(1)[ids::name].toString() != "Pad");
        CHECK(accessible::valueOf(history) == "Rename track, step 1 of 2");
        REQUIRE(f.press(juce::KeyPress(juce::KeyPress::downKey)));
        CHECK(f.set().track(1)[ids::name].toString() == "Pad");
    }
    SECTION("Find")
    {
        f.shell->openFind(false);
        f.shell->findView().setText("Tempo");
        INFO(accessible::valueOf(f.shell->findView()));
        REQUIRE(!f.shell->findView().results().empty());
        CHECK(accessible::valueOf(f.shell->findView()).startsWith(f.shell->findView().results().front().name + ", "));
        CHECK(accessible::valueOf(f.shell->findView()).contains(" of "));
        f.shell->findView().close();
    }
    SECTION("Browser")
    {
        auto& list = f.shell->browserView().listComponent();
        auto& model = f.shell->browserView().model();
        if (!model.rows().empty())
        {
            model.select(0);
            CHECK(accessible::valueOf(list).contains("1 of "));
        }
        CHECK(list.getAccessibilityHandler()->getRole() == juce::AccessibilityRole::list);
    }
}

TEST_CASE("Accessibility: F6 and Shift+F6 move the keys between the window's areas", "[ui][accessibility]")
{
    Fixture f;
    f.shell->showClipView();
    f.shell->sessionView().grabKeyboardFocus();
    REQUIRE(f.shell->focusedArea() == "Session");

    const juce::StringArray forward{"Mixer", "Detail", "Undo history", "Control bar", "Browser", "Session"};
    for (const auto& area : forward)
    {
        REQUIRE(f.press(f6));
        CHECK(f.shell->focusedArea() == area);
    }
    // In the detail view the keys are in the piano roll (the arrows move notes there).
    REQUIRE(f.press(f6));
    REQUIRE(f.press(f6));
    CHECK(Fixture::focused() == &f.shell->clipView().pianoRoll());
    // Back again.
    REQUIRE(f.press(shiftF6));
    CHECK(f.shell->focusedArea() == "Mixer");
    REQUIRE(f.press(shiftF6));
    CHECK(f.shell->focusedArea() == "Session");
    CHECK(Fixture::focused() == &f.shell->sessionView());

    // Areas hidden are passed over.
    f.shell->setBrowserVisible(false);
    f.shell->setUndoHistoryVisible(false);
    f.shell->sessionView().setMixerVisible(false);
    f.settle();
    f.shell->sessionView().grabKeyboardFocus();
    REQUIRE(f.press(f6));
    CHECK(f.shell->focusedArea() == "Detail");
    REQUIRE(f.press(f6));
    CHECK(f.shell->focusedArea() == "Control bar");
    REQUIRE(f.press(f6));
    CHECK(f.shell->focusedArea() == "Session");
}

TEST_CASE("Accessibility: Tab and Shift+Tab move through controls; in a view Shift+Tab switches Clip/Device", "[ui][accessibility]")
{
    Fixture f;
    const auto chain = LiveSet::chainOf(f.set().track(0));
    f.set().insertDevice(chain, -1, "lattice.synth");
    f.shell->selection().set({SessionSelection::Kind::track, 0, 0});
    f.shell->showDeviceView();
    f.settle();

    // In the Session grid (a view), Shift+Tab is the Clip/Device switch, as in Live.
    f.shell->sessionView().grabKeyboardFocus();
    REQUIRE(f.shell->detailView().page() == DetailView::Page::device);
    REQUIRE(f.press(shiftTab));
    CHECK(f.shell->detailView().page() == DetailView::Page::clip);
    REQUIRE(f.press(shiftTab));
    CHECK(f.shell->detailView().page() == DetailView::Page::device);

    // On a knob reached with Tab, Shift+Tab goes back to the control before instead.
    // The Device view's first knob (the top-left one), so Tab stays among its controls. (The
    // registry's order is its hash map's: picking the first entry found could pick the last knob,
    // from which Tab leaves the view.)
    BoundControl* knob = nullptr;
    const auto before = [](const juce::Component& a, const juce::Component& b) {
        const auto pa = a.getScreenPosition(), pb = b.getScreenPosition();
        return pa.y != pb.y ? pa.y < pb.y : pa.x < pb.x;
    };
    for (const auto* e : f.shell->controlRegistry().entries())
        if (auto* c = dynamic_cast<BoundControl*>(e->component.getComponent());
            c != nullptr && c->isShowing() && f.shell->deviceView().isParentOf(c) && (knob == nullptr || before(*c, *knob)))
            knob = c;
    REQUIRE(knob != nullptr);
    knob->grabKeyboardFocus();
    REQUIRE(Fixture::focused() == knob);
    REQUIRE(f.press(tab));
    auto* next = Fixture::focused();
    CHECK(next != knob);
    REQUIRE(f.press(shiftTab));
    CHECK(Fixture::focused() == knob);
    CHECK(f.shell->detailView().page() == DetailView::Page::device);
}
