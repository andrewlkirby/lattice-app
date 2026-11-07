// Virtual Push (docs/06-m5-controllers.md, 5a; Tests 6), headless: its layout at every size (square
// pads inside its bounds, nothing overlapping), clicks and right-click latches reaching the
// controller, velocity from the click position, repainting only while a light moves, and the
// window shell showing it docked or in its own window.

#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"
#include "control/modes/SessionMode.h"
#include "control/surface/Presets.h"
#include "control/virtual/VirtualPush.h"
#include "ui/prefs/AppSettings.h"
#include "ui/shell/MainShell.h"

#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::control;
using namespace lattice::test;

namespace
{
juce::MouseEvent mouseAt(juce::Component& c, juce::Point<float> p, juce::ModifierKeys mods = {})
{
    const auto mouse = juce::Desktop::getInstance().getMainMouseSource();
    return juce::MouseEvent(mouse, p, mods, juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
                            juce::Time::getCurrentTime(), p, juce::Time::getCurrentTime(), 1, false);
}

struct ShellFixture
{
    ShellFixture()
    {
        settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lattice-push-" + juce::Uuid().toString() + ".settings");
        juce::PropertiesFile::Options options;
        options.applicationName = "LatticeTests";
        settings = std::make_unique<juce::PropertiesFile>(settingsFile, options);
        session.newSet(4, 4);
        session.flush();
        shell = std::make_unique<ui::MainShell>(session, engine, *settings, tunings, ui::ShellActions{});
        shell->setSize(1400, 900);
    }
    ~ShellFixture()
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
        now += 1.0 / 60.0;
        shell->feed().tick(now);
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer{engine, BlockPattern::Fixed64};
    juce::File settingsFile;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<ui::MainShell> shell;
    double now = 3.0;
};
} // namespace

TEST_CASE("Virtual Push: at every size its pads are square, inside its bounds, and nothing overlaps", "[control][virtual]")
{
    for (const auto& [rows, columns] : {std::pair{8, 8}, {8, 12}, {16, 16}, {4, 4}, {4, 16}, {16, 4}})
        for (const auto& bounds : {juce::Rectangle<float>(0, 0, 900, 600), {0, 0, 400, 900}, {10, 20, 1920, 400}})
            for (const int encoders : {0, 8, 16, 32})
            {
                INFO(rows << "x" << columns << ", " << encoders << " encoders, in " << bounds.toString());
                const auto caps = control::presets::virtualPush(rows, columns, encoders);
                const auto l = VirtualPush::layoutFor(caps, bounds);
                REQUIRE(l.pads.size() == static_cast<std::size_t>(rows * columns));
                REQUIRE(l.buttons.size() == caps.buttons.size());
                CHECK(l.unit > 0.0f);
                std::vector<juce::Rectangle<float>> all = l.pads;
                CHECK_FALSE(l.strip.isEmpty());
                CHECK_FALSE(l.status.isEmpty());
                all.push_back(l.strip);
                all.push_back(l.status);
                // The display: a cell for each encoder, inside it; a button above and below each column.
                CHECK(l.display.isEmpty() == (encoders == 0));
                REQUIRE(l.encoders.size() == static_cast<std::size_t>(encoders));
                for (const auto& cell : l.encoders)
                    CHECK(l.display.expanded(0.01f).contains(cell));
                CHECK(caps.countOf(Role::displayUpper) == (encoders > 0 ? caps.cellsAcross() : 0));
                CHECK(caps.countOf(Role::displayLower) == caps.countOf(Role::displayUpper));
                if (encoders > 0)
                    all.push_back(l.display);
                for (const auto& b : l.buttons)
                {
                    CHECK_FALSE(b.bounds.isEmpty()); // every role has a place
                    all.push_back(b.bounds);
                }
                for (const auto& p : l.pads)
                    CHECK(std::abs(p.getWidth() - p.getHeight()) < 0.01f);
                for (std::size_t i = 0; i < all.size(); ++i)
                {
                    CHECK(bounds.expanded(0.5f).contains(all[i]));
                    for (std::size_t j = i + 1; j < all.size(); ++j)
                        CHECK_FALSE(all[i].intersects(all[j]));
                }
            }
}

TEST_CASE("Virtual Push: clicks press pads and buttons; a right-click latches until clicked again", "[control][virtual]")
{
    ui::EngineStateFeed* feed = nullptr;
    ShellFixture f;
    feed = &f.shell->feed();
    VirtualPush push(*feed, {});
    push.setSize(900, 600);
    std::vector<SurfaceInput> got;
    push.onInput = [&](const SurfaceInput& in) { got.push_back(in); };

    const auto& l = push.layout();
    const auto pad = l.pads[static_cast<std::size_t>(2 * 8 + 3)];
    push.mouseDown(mouseAt(push, {pad.getCentreX(), pad.getY() + 1.0f}));
    push.mouseUp(mouseAt(push, pad.getCentre()));
    REQUIRE(got.size() == 2);
    CHECK(got[0].kind == SurfaceInput::Kind::padDown);
    CHECK(got[0].row == 2);
    CHECK(got[0].column == 3);
    CHECK(got[0].value > 0.95f); // its top: loud
    CHECK(got[1].kind == SurfaceInput::Kind::padUp);
    CHECK(push.velocityAt(0, {l.pads[0].getCentreX(), l.pads[0].getBottom() - 1.0f}) < 0.3f);

    // Latch Shift with a right-click: down now, up only at the next click on it.
    got.clear();
    const int shift = push.capabilities().indexOf({Role::shift, 0});
    const auto shiftAt = l.buttons[static_cast<std::size_t>(shift)].bounds.getCentre();
    push.mouseDown(mouseAt(push, shiftAt, juce::ModifierKeys::rightButtonModifier));
    push.mouseUp(mouseAt(push, shiftAt, juce::ModifierKeys::rightButtonModifier));
    REQUIRE(got.size() == 1);
    CHECK(got[0].kind == SurfaceInput::Kind::buttonDown);
    CHECK(got[0].button == ButtonId{Role::shift, 0});
    CHECK(push.isLatched(shift, true));
    push.mouseDown(mouseAt(push, shiftAt));
    push.mouseUp(mouseAt(push, shiftAt));
    REQUIRE(got.size() == 2);
    CHECK(got[1].kind == SurfaceInput::Kind::buttonUp);
    CHECK_FALSE(push.isLatched(shift, true));

    SECTION("A fixed velocity")
    {
        VirtualPush fixed(*feed, {.rows = 8, .columns = 8, .fixedVelocity = 64});
        fixed.setSize(900, 600);
        CHECK(fixed.velocityAt(0, fixed.layout().pads[0].getTopLeft()) == 64.0f / 127.0f);
    }
}

TEST_CASE("Virtual Push: the strip bends and springs back; dragging up from a held pad presses it harder", "[control][virtual]")
{
    ShellFixture f;
    VirtualPush push(f.shell->feed(), {});
    push.setSize(900, 600);
    std::vector<SurfaceInput> got;
    push.onInput = [&](const SurfaceInput& in) { got.push_back(in); };
    const auto& l = push.layout();

    const auto strip = l.strip;
    push.mouseDown(mouseAt(push, {strip.getCentreX(), strip.getY() + strip.getHeight() * 0.25f}));
    push.mouseDrag(mouseAt(push, {strip.getCentreX(), strip.getBottom() - 1.0f}));
    push.mouseUp(mouseAt(push, {strip.getCentreX(), strip.getBottom() - 1.0f}));
    REQUIRE(got.size() == 3);
    CHECK(got[0].kind == SurfaceInput::Kind::strip);
    CHECK(std::abs(got[0].value - 0.75f) < 0.02f);
    CHECK(got[1].value < 0.02f);
    CHECK(got[2].kind == SurfaceInput::Kind::stripRelease);

    got.clear();
    const auto pad = l.pads[10];
    // Per note (the default): sideways slides, up and down sets the timbre; a mouse presses nothing.
    REQUIRE(push.playsPerNote());
    push.mouseDown(mouseAt(push, pad.getCentre()));
    push.mouseDrag(mouseAt(push, pad.getCentre().translated(l.unit * 0.1f, -l.unit * 0.5f))); // still in tune
    push.mouseDrag(mouseAt(push, pad.getCentre().translated(l.unit * 1.5f, -l.unit * 0.5f)));
    push.mouseDrag(mouseAt(push, pad.getCentre().translated(-l.unit * 0.575f, l.unit * 2.0f)));
    push.mouseUp(mouseAt(push, pad.getCentre()));
    REQUIRE(got.size() == 6);
    CHECK(got[0].kind == SurfaceInput::Kind::padDown);
    CHECK(got[5].kind == SurfaceInput::Kind::padUp);
    CHECK(got[1].kind == SurfaceInput::Kind::padTimbre);
    CHECK(std::abs(got[1].value - 0.75f) < 0.01f);
    CHECK(got[2].kind == SurfaceInput::Kind::padSlide);
    CHECK(std::abs(got[2].value - 1.5f) < 0.01f);
    CHECK(got[3].kind == SurfaceInput::Kind::padSlide);
    CHECK(std::abs(got[3].value + 0.5f) < 0.01f); // past the still part, half way to the next pad
    CHECK(got[4].kind == SurfaceInput::Kind::padTimbre);
    CHECK(got[4].value == 0.0f);
    CHECK(got[2].row == got[0].row);
    CHECK(got[2].column == got[0].column);
    CHECK(std::abs(push.slideFor({100, 0}, {100.0f + l.unit, 0}) - 1.0f) < 1.0e-5f); // the next pad's centre: its note
    got.clear();

    // Not per note: dragging up presses harder.
    push.setPerNote(false);
    push.mouseDown(mouseAt(push, pad.getCentre()));
    push.mouseDrag(mouseAt(push, pad.getCentre().translated(0.0f, -l.unit * 0.5f)));
    push.mouseDrag(mouseAt(push, pad.getCentre().translated(0.0f, -l.unit * 0.5f))); // the same: nothing new
    push.mouseDrag(mouseAt(push, pad.getCentre().translated(0.0f, -l.unit * 2.0f)));
    push.mouseUp(mouseAt(push, pad.getCentre()));
    REQUIRE(got.size() == 4);
    CHECK(got[1].kind == SurfaceInput::Kind::padPressure);
    CHECK(std::abs(got[1].value - 0.5f) < 0.02f);
    CHECK(got[2].value == 1.0f);
    CHECK(got[3].kind == SurfaceInput::Kind::padUp);
    CHECK(push.pressureFor({0, 100}, {0, 120}, -1.0f) == 0.0f); // dragging down presses nothing
    CHECK(push.pressureFor({0, 100}, {0, 100}, 0.7f) == 0.7f);  // a pen's own pressure

    // Its text: what the controller shows.
    SurfaceText text;
    text.pads.assign(64, "47");
    text.status = "Colundi";
    push.showText(text);
    CHECK(push.text() == text);
}

TEST_CASE("Virtual Push: its display's cells are the encoders: pressed to touch, dragged or scrolled to turn", "[control][virtual]")
{
    ShellFixture f;
    VirtualPush push(f.shell->feed(), {.rows = 8, .columns = 8, .fixedVelocity = 0, .encoders = 16});
    push.setSize(900, 900);
    CHECK(push.capabilities().encoders == 16);
    CHECK(push.capabilities().cellsAcross() == 8); // two rows of 8
    std::vector<SurfaceInput> got;
    push.onInput = [&](const SurfaceInput& in) { got.push_back(in); };
    const auto& l = push.layout();
    REQUIRE(l.encoders.size() == 16);
    CHECK(l.encoders[8].getY() > l.encoders[0].getY());
    CHECK(push.hitTest(l.encoders[11].getCentre()).encoder == 11);

    const auto at = l.encoders[3].getCentre();
    const float range = l.unit * 3.0f; // a value's whole range
    push.mouseDown(mouseAt(push, at));
    push.mouseDrag(mouseAt(push, at.translated(0.0f, -range * 0.5f)));
    push.mouseDrag(mouseAt(push, at.translated(0.0f, -range * 0.5f - 0.1f))); // less than a step: kept for later
    push.mouseDrag(mouseAt(push, at.translated(0.0f, -range * 0.25f)));       // back down
    push.mouseUp(mouseAt(push, at));
    REQUIRE(got.size() == 4);
    CHECK(got[0].kind == SurfaceInput::Kind::encoderTouch);
    CHECK(got[0].encoder == 3);
    CHECK(got[1].kind == SurfaceInput::Kind::encoderTurn);
    const int resolution = push.capabilities().encoderResolution;
    CHECK(std::abs(got[1].delta - resolution / 2) <= 1);
    CHECK(got[2].kind == SurfaceInput::Kind::encoderTurn);
    CHECK(got[2].delta < 0);
    CHECK(got[3].kind == SurfaceInput::Kind::encoderRelease);
    CHECK(std::abs(push.stepsForDrag(range, true) - static_cast<float>(resolution) / 10.0f) < 0.01f); // Shift: finer

    got.clear();
    juce::MouseWheelDetails wheel{};
    wheel.deltaY = 0.1f;
    push.mouseWheelMove(mouseAt(push, l.encoders[5].getCentre()), wheel);
    wheel.deltaY = -0.001f;
    push.mouseWheelMove(mouseAt(push, l.encoders[5].getCentre()), wheel);
    REQUIRE(got.size() == 2);
    CHECK(got[0].encoder == 5);
    CHECK(got[0].delta == juce::roundToInt(0.1f * 0.25f * static_cast<float>(resolution)));
    CHECK(got[1].delta == -1); // at least a step

    // What the controller shows is drawn there.
    DisplayFrame frame;
    frame.title = "Bass";
    push.showDisplay(frame);
    CHECK(push.displayed() == frame);
}

TEST_CASE("Shell: Virtual Push shows docked at the right, or in its own window, and follows its settings", "[control][virtual][ui]")
{
    ShellFixture f;
    auto& shell = *f.shell;
    CHECK_FALSE(shell.isVirtualPushVisible());
    REQUIRE(shell.commands().perform("view.virtualPush"));
    REQUIRE(shell.isVirtualPushVisible());
    auto* push = shell.virtualPushView();
    CHECK(push->getParentComponent() == &shell);
    CHECK(push->getRight() == shell.getWidth());
    CHECK(push->getWidth() > 0);
    CHECK(shell.mainArea().getRight() <= push->getX());
    CHECK(f.settings->getBoolValue(ui::MainShell::virtualPushKey));

    // Its pads launch clips through the same session.
    f.session.set().createClip(1, 0, 4.0);
    f.session.play();
    f.render(64);
    const auto pad = push->layout().pads[1];
    push->mouseDown(mouseAt(*push, pad.getCentre()));
    push->mouseUp(mouseAt(*push, pad.getCentre()));
    f.render(64);
    CHECK(f.shell->feed().slot(1, 0) == SlotState::QueuedLaunch);
    // Queued: it blinks, so the display refresh repaints it (only while a light moves).
    CHECK(push->leds().anyAnimated());

    // Note mode: the pads show their notes, and the armed track plays them.
    const auto note = push->capabilities().indexOf({Role::modeNote, 0});
    const auto noteAt = push->layout().buttons[static_cast<std::size_t>(note)].bounds.getCentre();
    push->mouseDown(mouseAt(*push, noteAt));
    push->mouseUp(mouseAt(*push, noteAt));
    CHECK(shell.virtualPushController()->modeKind() == ModeKind::note);
    shell.virtualPushController()->refresh();
    CHECK(push->text().pads[56].isNotEmpty());
    CHECK(push->text().status.isNotEmpty());

    // A new grid size makes it again.
    f.settings->setValue(ui::prefs::virtualRowsKey, 4);
    f.settings->setValue(ui::prefs::virtualColumnsKey, 12);
    shell.applyVirtualPushSettings();
    REQUIRE(shell.virtualPushView() != nullptr);
    CHECK(shell.virtualPushView()->capabilities().rows == 4);
    CHECK(shell.virtualPushView()->capabilities().columns == 12);
    CHECK(shell.virtualPushController()->sessionMode().trackColumns() == 12);

    SECTION("In its own window")
    {
        REQUIRE(shell.commands().perform("view.virtualPushWindow"));
        CHECK_FALSE(shell.isVirtualPushDocked());
        push = shell.virtualPushView();
        REQUIRE(push != nullptr);
        CHECK(push->getParentComponent() != &shell);
        CHECK(shell.mainArea().getRight() == shell.getWidth());
        shell.setVirtualPushDocked(true);
        CHECK(push->getParentComponent() == &shell);
    }
    SECTION("Hidden: gone, and its room goes back to the main area")
    {
        REQUIRE(shell.commands().perform("view.virtualPush"));
        CHECK_FALSE(shell.isVirtualPushVisible());
        CHECK(shell.virtualPushController() == nullptr);
        CHECK(shell.mainArea().getRight() == shell.getWidth());
    }
}

TEST_CASE("Virtual Push: while nothing moves it doesn't animate, and LED frames repaint only what changed", "[control][virtual]")
{
    ShellFixture f;
    f.shell->setVirtualPushVisible(true);
    auto* push = f.shell->virtualPushView();
    REQUIRE(push != nullptr);
    f.render(64);
    CHECK_FALSE(push->leds().anyAnimated());
    CHECK_FALSE(push->isAnimating()); // also: not showing on a desktop in a headless test
    const auto sent = f.shell->virtualPushController()->framesSent();
    for (int i = 0; i < 10; ++i)
        f.render(64);
    CHECK(f.shell->virtualPushController()->framesSent() == sent);
}

