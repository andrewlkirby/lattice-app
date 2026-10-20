// The widget library (docs/05-m4-ui.md, "Widget library"), driven headless: bindings read and
// write the document with one undo step per gesture, drags and steps move values as specified,
// typed values parse, meters hold and fall, toggles follow the document, screen readers see each
// control, and every widget draws in every theme.

#include "ui/theme/LatticeLookAndFeel.h"
#include "ui/widgets/Fader.h"
#include "ui/widgets/Knob.h"
#include "ui/widgets/NumberBox.h"
#include "ui/widgets/Toggle.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::ui;

namespace
{
ParameterSpec cutoffSpec()
{
    return ParameterSpec::continuous("filter.cutoff", "Cutoff", 20.0f, 20000.0f, 1000.0f, ParamUnit::Hz)
        .withCentre(1000.0f)
        .inGroup("Filter");
}
ParameterSpec voicesSpec() { return ParameterSpec::integer("voice.polyphony", "Voices", 1, 32, 16); }
ParameterSpec panSpec() { return ParameterSpec::continuous("out.pan", "Pan", -1.0f, 1.0f, 0.0f); }
ParameterSpec modeSpec() { return ParameterSpec::choice("voice.mode", "Mode", {"Poly", "Mono", "Legato"}, 0); }

/** A set with one device on track 1, and its undo history. */
struct Fixture
{
    Fixture()
    {
        set.resetToDefault(2, 1);
        device = set.insertDevice(LiveSet::chainOf(set.track(0)), -1, "lattice.synth");
        undo.clearUndoHistory();
    }

    std::unique_ptr<ValueBinding> bind(ParameterSpec spec) { return ValueBinding::deviceParameter(set, device, std::move(spec)); }
    double stored(const juce::String& id, double fallback) const { return LiveSet::deviceParameter(device, id, fallback); }

    juce::UndoManager undo;
    LiveSet set{&undo};
    juce::ValueTree device;
};

juce::Image draw(juce::Component& c, float scale)
{
    return c.createComponentSnapshot(c.getLocalBounds(), true, scale);
}

/** A click as JUCE's Button::triggerClick() delivers it (it posts this message; the tests have no
    message loop, so it's handed over directly). It goes through the button's real click path. */
void click(juce::Button& b)
{
    constexpr int juceButtonClickMessage = 0x2f3f4f99; // juce_Button.cpp, clickMessageId
    static_cast<juce::Component&>(b).handleCommandMessage(juceButtonClickMessage);
}

bool sameImage(const juce::Image& a, const juce::Image& b)
{
    if (a.getWidth() != b.getWidth() || a.getHeight() != b.getHeight())
        return false;
    for (int y = 0; y < a.getHeight(); ++y)
        for (int x = 0; x < a.getWidth(); ++x)
            if (a.getPixelAt(x, y) != b.getPixelAt(x, y))
                return false;
    return true;
}
} // namespace

TEST_CASE("Widgets: a binding reads and writes a device parameter, and hears changes from anywhere", "[ui][widgets]")
{
    Fixture f;
    auto binding = f.bind(cutoffSpec());
    CHECK(binding->value() == 1000.0f); // nothing stored yet: the default
    CHECK(binding->normalised() == Catch::Approx(0.5f).margin(1e-4)); // the centre sits mid-travel
    CHECK(binding->text() == cutoffSpec().format(1000.0f));

    int changes = 0;
    binding->onChange = [&] { ++changes; };
    f.set.setDeviceParameter(f.device, "filter.cutoff", 250.0);
    CHECK(changes == 1);
    CHECK(binding->value() == 250.0f);
    f.set.setDeviceParameter(f.device, "filter.resonance", 0.4); // another parameter: not this one
    CHECK(changes == 1);

    binding->setValue(50000.0f); // constrained to the range
    CHECK(f.stored("filter.cutoff", 0.0) == 20000.0);
    CHECK(changes == 2);
    f.undo.undo();
    CHECK(binding->value() == 250.0f);
    CHECK(changes == 3);
}

TEST_CASE("Widgets: a drag covers the range in 200 px, Shift is ten times finer, and it's one undo step",
          "[ui][widgets]")
{
    Fixture f;
    Knob knob(f.bind(cutoffSpec()));
    knob.setSize(60, 80);

    knob.beginDrag();
    knob.dragBy(50.0f, false);
    CHECK(knob.binding().normalised() == Catch::Approx(0.75f).margin(1e-4));
    knob.dragBy(100.0f, false); // past the end: stops at the top
    CHECK(knob.binding().value() == 20000.0f);
    knob.dragBy(-10.0f, false); // turning back responds at once
    CHECK(knob.binding().normalised() == Catch::Approx(0.95f).margin(1e-4));
    knob.dragBy(-100.0f, true); // fine: 2000 px for the whole range
    CHECK(knob.binding().normalised() == Catch::Approx(0.90f).margin(1e-4));
    knob.endDrag();

    f.undo.undo(); // the whole drag at once
    CHECK(knob.binding().value() == 1000.0f);
    CHECK(!f.undo.canUndo());
}

TEST_CASE("Widgets: discrete values step one at a time; keys and the wheel step, a burst is one undo step",
          "[ui][widgets]")
{
    Fixture f;
    Knob voices(f.bind(voicesSpec()));
    voices.setSize(60, 80);
    // 31 steps at about 10 px each.
    CHECK(voices.pixelsForFullTravel(false) == Catch::Approx(300.0f));
    voices.beginDrag();
    voices.dragBy(22.0f, false); // 2.3 steps: rounds to whole steps
    CHECK(voices.binding().value() == 18.0f);
    voices.endDrag();

    voices.stepBy(1, false);
    voices.stepBy(1, false);
    CHECK(voices.keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
    CHECK(voices.binding().value() == 21.0f);
    voices.finishSteps();
    f.undo.undo(); // the three steps
    CHECK(voices.binding().value() == 18.0f);
    f.undo.undo(); // the drag
    CHECK(voices.binding().value() == 16.0f);

    // Continuous: a step is 1% of the travel, 0.1% with Shift.
    Knob cutoff(f.bind(cutoffSpec()));
    cutoff.stepBy(-1, false);
    CHECK(cutoff.binding().normalised() == Catch::Approx(0.49f).margin(1e-4));
    cutoff.stepBy(1, true);
    CHECK(cutoff.binding().normalised() == Catch::Approx(0.491f).margin(1e-4));
    cutoff.finishSteps();

    // Delete resets; letters are left for the computer keyboard.
    CHECK(cutoff.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
    CHECK(cutoff.binding().value() == 1000.0f);
    CHECK(!cutoff.keyPressed(juce::KeyPress('a')));
}

TEST_CASE("Widgets: typed values parse in the parameter's units, and choices by name", "[ui][widgets]")
{
    Fixture f;
    Knob cutoff(f.bind(cutoffSpec()));
    cutoff.setSize(60, 80);
    cutoff.commitTypeIn("2.5k");
    CHECK(cutoff.binding().value() == Catch::Approx(2500.0f));
    cutoff.commitTypeIn("440 Hz");
    CHECK(cutoff.binding().value() == Catch::Approx(440.0f));
    f.undo.undo();
    CHECK(cutoff.binding().value() == Catch::Approx(2500.0f)); // each typed value is one undo step

    cutoff.showTypeInEditor();
    CHECK(cutoff.isTypingIn());

    NumberBox mode(f.bind(modeSpec()));
    mode.commitTypeIn("Legato");
    CHECK(mode.binding().value() == 2.0f);
    CHECK(mode.binding().text() == "Legato");
    // A click on a choice box lists the choices, the current one ticked.
    const auto menu = mode.choicesMenu();
    juce::PopupMenu::MenuItemIterator items(menu);
    juce::StringArray names;
    juce::String ticked;
    while (items.next())
    {
        names.add(items.getItem().text);
        if (items.getItem().isTicked)
            ticked = items.getItem().text;
    }
    CHECK(names == juce::StringArray{"Poly", "Mono", "Legato"});
    CHECK(ticked == "Legato");
}

TEST_CASE("Widgets: a knob is bipolar when its range is symmetric around zero", "[ui][widgets]")
{
    Fixture f;
    CHECK(Knob(f.bind(panSpec())).isBipolar());
    CHECK(!Knob(f.bind(cutoffSpec())).isBipolar());
    CHECK(!Knob(f.bind(ParameterSpec::continuous("out.volume", "Volume", -70.0f, 6.0f, 0.0f, ParamUnit::Decibels))).isBipolar());
    CHECK(Knob::angleFor(0.5f) == Catch::Approx(0.0f).margin(1e-6)); // the middle points straight up
}

TEST_CASE("Widgets: a fader follows the pointer one to one and marks 0 dB", "[ui][widgets]")
{
    juce::UndoManager undo;
    LiveSet set(&undo);
    set.resetToDefault(1, 1);
    const auto volume = ParameterSpec::continuous("volume", "Volume", static_cast<float>(LiveSet::minVolumeDb),
                                                  static_cast<float>(LiveSet::maxVolumeDb), 0.0f, ParamUnit::Decibels)
                            .withCentre(-12.0f);
    Fader fader(ValueBinding::property(volume, &undo, set.track(0), ids::volumeDb,
                                       [&set](double db) { set.setTrackVolumeDb(0, db); }));
    fader.setSize(40, 220);

    const float travel = fader.pixelsForFullTravel(false);
    const float startY = fader.capCentreFor(fader.binding().normalised());
    fader.beginDrag();
    fader.dragBy(-travel * 0.25f, false); // down a quarter (0 dB starts most of the way up)
    fader.endDrag();
    // The cap moved exactly as far as the pointer did.
    CHECK(fader.capCentreFor(fader.binding().normalised()) - startY == Catch::Approx(travel * 0.25f).margin(0.01));
    CHECK(static_cast<double>(set.track(0)[ids::volumeDb]) < -6.0);
    undo.undo();
    CHECK(static_cast<double>(set.track(0)[ids::volumeDb]) == Catch::Approx(0.0));
    CHECK(fader.pixelsForFullTravel(true) == Catch::Approx(travel * 10.0f));
}

TEST_CASE("Widgets: a meter holds its peak, then falls; the clip light stays on until clicked", "[ui][widgets]")
{
    Meter meter;
    meter.setLevels(0, 0.5f, 0.25f);
    CHECK(meter.peakDb(0) == Catch::Approx(-6.02f).margin(0.01));
    CHECK(meter.rmsDb(0) == Catch::Approx(-12.04f).margin(0.01));
    meter.setLevels(0, 0.0f, 0.0f);
    meter.advance(0.5);
    CHECK(meter.heldPeakDb(0) == Catch::Approx(-6.02f).margin(0.01)); // held for a second
    meter.advance(1.0); // 0.5 s past the hold: down 10 dB
    CHECK(meter.heldPeakDb(0) == Catch::Approx(-16.02f).margin(0.01));
    meter.advance(10.0);
    CHECK(meter.heldPeakDb(0) == Meter::floorDb);

    CHECK(!meter.clipped());
    meter.setLevels(1, 1.2f, 0.7f);
    meter.setLevels(1, 0.1f, 0.1f);
    CHECK(meter.clipped());
    meter.resetClip();
    CHECK(!meter.clipped());

    CHECK(Meter::proportionForDb(Meter::floorDb - 20.0f) == 0.0f);
    CHECK(Meter::proportionForDb(Meter::topDb) == 1.0f);
    CHECK(Meter::proportionForDb(0.0f) == Catch::Approx(60.0f / 66.0f));
}

TEST_CASE("Widgets: a bound toggle flips the document as one undo step and follows it", "[ui][widgets]")
{
    juce::UndoManager undo;
    LiveSet set(&undo);
    set.resetToDefault(1, 1);
    undo.clearUndoHistory();
    Toggle mute(ValueBinding::property(ParameterSpec::toggle("mute", "Mute", false), &undo, set.track(0), ids::mute,
                                       [&set](double on) { set.setTrackMute(0, on > 0.5); }),
                Toggle::Role::mute, "M");
    CHECK(!mute.isOn());
    click(mute);
    CHECK(mute.isOn());
    CHECK(static_cast<bool>(set.track(0)[ids::mute]));
    undo.undo();
    CHECK(!mute.isOn());
    set.setTrackMute(0, true); // from elsewhere
    CHECK(mute.isOn());

    // Unbound: an ordinary toggle button.
    Toggle loop("Loop");
    int clicks = 0;
    loop.onClick = [&] { ++clicks; };
    click(loop);
    CHECK(loop.isOn());
    CHECK(clicks == 1);
}

TEST_CASE("Widgets: screen readers see a control's name, its value as text, and can set it", "[ui][widgets]")
{
    Fixture f;
    Knob cutoff(f.bind(cutoffSpec()));
    const auto handler = cutoff.createAccessibilityHandler();
    REQUIRE(handler != nullptr);
    CHECK(handler->getRole() == juce::AccessibilityRole::slider);
    CHECK(handler->getTitle() == "Cutoff");
    auto* value = handler->getValueInterface();
    REQUIRE(value != nullptr);
    CHECK(value->getCurrentValueAsString() == cutoffSpec().format(1000.0f));
    CHECK(value->getRange().getMinimumValue() == Catch::Approx(20.0));
    CHECK(value->getRange().getMaximumValue() == Catch::Approx(20000.0));
    value->setValue(3000.0);
    CHECK(f.stored("filter.cutoff", 0.0) == Catch::Approx(3000.0));
    value->setValueAsString("500");
    CHECK(f.stored("filter.cutoff", 0.0) == Catch::Approx(500.0));
}

TEST_CASE("Widgets: every widget draws in every theme, at 100% and 200%", "[ui][widgets]")
{
    Fixture f;
    Knob knob(f.bind(cutoffSpec()));
    NumberBox box(f.bind(voicesSpec()));
    Toggle toggle("Loop", Toggle::Role::accent);
    toggle.setToggleState(true, juce::dontSendNotification);
    auto fader = std::make_unique<Fader>(f.bind(ParameterSpec::continuous("out.volume", "Volume", -70.0f, 6.0f, 0.0f,
                                                                           ParamUnit::Decibels)));
    fader->meter().setLevels(0, 0.7f, 0.4f);
    juce::Component* widgets[] = {&knob, &box, &toggle, fader.get()};
    knob.setSize(knob.preferredSize().getWidth(), knob.preferredSize().getHeight());
    box.setSize(box.preferredSize().getWidth(), box.preferredSize().getHeight());
    toggle.setSize(toggle.preferredSize().getWidth(), toggle.preferredSize().getHeight());
    fader->setSize(40, 160);

    LatticeLookAndFeel dark(Theme::dark()), light(Theme::light()), high(Theme::highContrast());
    for (auto* w : widgets)
    {
        INFO(w->getTitle() << " " << w->getName());
        CHECK(w->getWidth() > 0);
        CHECK(w->getHeight() > 0);
        juce::Image images[3];
        int i = 0;
        for (auto* laf : {&dark, &light, &high})
        {
            w->setLookAndFeel(laf);
            images[i] = draw(*w, 1.0f);
            const auto big = draw(*w, 2.0f);
            CHECK(big.getWidth() == images[i].getWidth() * 2);
            ++i;
        }
        CHECK(!sameImage(images[0], images[1]));
        CHECK(!sameImage(images[0], images[2]));
        w->setLookAndFeel(nullptr);
    }
}

TEST_CASE("Widgets: a gallery of every widget in every theme, for looking at", "[.ui-snapshot]")
{
    // lattice_tests "[.ui-snapshot]" writes PNGs to LATTICE_SNAPSHOT_DIR (default: temp).
    Fixture f;
    f.set.setDeviceParameter(f.device, "filter.cutoff", 3200.0);
    f.set.setDeviceParameter(f.device, "out.pan", -0.35);
    const auto dir = juce::File(juce::SystemStats::getEnvironmentVariable(
        "LATTICE_SNAPSHOT_DIR", juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName()));

    for (const auto* theme : {&Theme::dark(), &Theme::light(), &Theme::highContrast()})
    {
        LatticeLookAndFeel laf(*theme);
        juce::Component gallery;
        gallery.setLookAndFeel(&laf);
        gallery.setOpaque(true);
        struct Background final : juce::Component
        {
            void paint(juce::Graphics& g) override { g.fillAll(themeOf(*this).surface); }
        } background;
        gallery.addAndMakeVisible(background);

        Knob cutoff(f.bind(cutoffSpec()));
        Knob pan(f.bind(panSpec()));
        Knob voices(f.bind(voicesSpec()));
        Knob disabled(f.bind(cutoffSpec()));
        disabled.setEnabled(false);
        NumberBox box(f.bind(voicesSpec()));
        NumberBox mode(f.bind(modeSpec()));
        Toggle muteOn("Track", Toggle::Role::mute), soloOff("S", Toggle::Role::solo), armOn("Arm", Toggle::Role::arm),
            playOn("Play", Toggle::Role::play);
        muteOn.setToggleState(true, juce::dontSendNotification);
        armOn.setToggleState(true, juce::dontSendNotification);
        playOn.setToggleState(true, juce::dontSendNotification);
        Fader fader(f.bind(ParameterSpec::continuous("out.volume", "Volume", -70.0f, 6.0f, -4.0f, ParamUnit::Decibels).withCentre(-12.0f)));
        fader.meter().setLevels(0, 0.8f, 0.45f);
        fader.meter().setLevels(1, 1.1f, 0.5f);

        int x = 12;
        for (auto* k : {&cutoff, &pan, &voices, &disabled})
        {
            gallery.addAndMakeVisible(*k);
            k->setBounds(k->preferredSize().withPosition(x, 12));
            x += k->getWidth() + 8;
        }
        int y = 110;
        x = 12;
        for (auto* b : {&box, &mode})
        {
            gallery.addAndMakeVisible(*b);
            b->setBounds(b->preferredSize().withPosition(x, y));
            x += b->getWidth() + 8;
        }
        x = 12;
        y += 34;
        for (auto* t : {&muteOn, &soloOff, &armOn, &playOn})
        {
            gallery.addAndMakeVisible(*t);
            t->setBounds(t->preferredSize().withPosition(x, y));
            x += t->getWidth() + 8;
        }
        gallery.addAndMakeVisible(fader);
        fader.setBounds(300, 12, 44, 170);
        gallery.setSize(360, 196);
        background.setBounds(gallery.getLocalBounds());

        const auto image = gallery.createComponentSnapshot(gallery.getLocalBounds(), true, 2.0f);
        juce::PNGImageFormat png;
        const auto file = dir.getChildFile("widgets-" + theme->name.replace(" ", "-").toLowerCase() + ".png");
        file.deleteFile();
        juce::FileOutputStream out(file);
        REQUIRE(png.writeImageToStream(image, out));
        WARN("Wrote " << file.getFullPathName());
        gallery.setLookAndFeel(nullptr);
    }
}
