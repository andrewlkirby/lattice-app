// Ableton Push 2/3 (docs/06-m5-controllers.md, 5f): the protocol's numbers against Ableton's manual
// (each example the manual gives, byte for byte), the display's frame bytes against reference bytes
// made independently from the manual (tests/data/push2/make_reference.py), the palette Lattice
// loads, the driver's input and lights (with the message budget and the animation clock), the
// display's USB thread on a fake link, and Push as a surface in the app.

#include "support/OfflineRenderer.h"
#include "support/Snapshots.h"
#include "support/TestDevices.h"
#include "control/push/PushDisplay.h"
#include "control/push/PushDriver.h"
#include "control/push/PushPalette.h"
#include "control/push/PushProtocol.h"
#include "control/surface/Presets.h"
#include "control/surface/SurfaceController.h"
#include "ui/commands/SessionCommands.h"
#include "ui/controllers/SurfaceManager.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <atomic>
#include <chrono>
#include <set>
#include <thread>

using namespace lattice;
using namespace lattice::control;
using namespace lattice::control::push;
using namespace lattice::test;
using Catch::Matchers::WithinAbs;

namespace
{
std::vector<std::uint8_t> bytesOf(const juce::MidiMessage& m)
{
    return {m.getRawData(), m.getRawData() + m.getRawDataSize()};
}

std::vector<std::uint8_t> hex(const char* text)
{
    std::vector<std::uint8_t> out;
    for (const auto& t : juce::StringArray::fromTokens(text, " ", ""))
        if (t.isNotEmpty())
            out.push_back(static_cast<std::uint8_t>(t.getHexValue32()));
    return out;
}

struct Recorder final : generic::MidiSender
{
    void send(const juce::MidiMessage& m) override { sent.push_back(m); }
    std::vector<juce::MidiMessage> sent;
    void clear() { sent.clear(); }
    std::vector<juce::MidiMessage> sysex() const
    {
        std::vector<juce::MidiMessage> out;
        for (const auto& m : sent)
            if (m.isSysEx())
                out.push_back(m);
        return out;
    }
    std::vector<juce::MidiMessage> lights() const
    {
        std::vector<juce::MidiMessage> out;
        for (const auto& m : sent)
            if (m.isNoteOn(true) || m.isController())
                out.push_back(m);
        return out;
    }
    int bytes() const
    {
        int n = 0;
        for (const auto& m : sent)
            n += m.getRawDataSize();
        return n;
    }
};

/** A display link that keeps what's written (on the sender's thread). */
struct FakeLink
{
    std::mutex lock;
    std::vector<std::vector<std::uint8_t>> writes;
    int opens = 0;
    bool failWrites = false;
    DisplayStatus refuse = DisplayStatus::connected; // what opening says (connected: it opens)

    struct Link final : DisplayLink
    {
        explicit Link(FakeLink& f) : fake(f) {}
        bool write(const std::uint8_t* data, int size) override
        {
            const std::scoped_lock l(fake.lock);
            if (fake.failWrites)
                return false;
            fake.writes.emplace_back(data, data + size);
            return true;
        }
        FakeLink& fake;
    };
    LinkOpener opener()
    {
        return [this] {
            const std::scoped_lock l(lock);
            ++opens;
            if (refuse != DisplayStatus::connected)
                return OpenedLink{nullptr, refuse};
            return OpenedLink{std::make_unique<Link>(*this), DisplayStatus::connected};
        };
    }
    std::size_t count()
    {
        const std::scoped_lock l(lock);
        return writes.size();
    }
};

/** Waits (up to a few seconds) for a condition another thread makes true. */
template <typename F>
bool eventually(F done)
{
    for (int i = 0; i < 400; ++i)
    {
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

juce::Image testPattern()
{
    // The same pattern as make_reference.py.
    juce::Image image(juce::Image::RGB, displayWidth, displayHeight, false);
    for (int y = 0; y < displayHeight; ++y)
        for (int x = 0; x < displayWidth; ++x)
            image.setPixelAt(x, y, juce::Colour(static_cast<juce::uint8>(x * 255 / (displayWidth - 1)), static_cast<juce::uint8>(y * 255 / (displayHeight - 1)),
                                                static_cast<juce::uint8>((x + y) & 255)));
    return image;
}

struct Inputs
{
    explicit Inputs(SurfaceDriver& d)
    {
        d.onInput = [this](const SurfaceInput& in) { got.push_back(in); };
    }
    std::vector<SurfaceInput> got;
};
} // namespace

// --- The protocol -------------------------------------------------------------------------------------

TEST_CASE("Push protocol: the pads, encoders, strip and buttons as Ableton's manual numbers them", "[control][push]")
{
    // "0x90 0x63 0x7F: set the top right pad", "0x90 0x24: the bottom left pad".
    CHECK(padNote(0, 7) == 0x63);
    CHECK(padNote(7, 0) == 0x24);
    CHECK(padNote(7, 7) == 43); // "0x90 0x2B: right bottom pad"
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c)
        {
            const auto p = padOf(padNote(r, c));
            REQUIRE(p);
            CHECK(p->row == r);
            CHECK(p->column == c);
        }
    CHECK_FALSE(padOf(35));
    CHECK_FALSE(padOf(100));

    // Encoders: "0x4F 0x01: master encoder right by 1", "0x0E 0x7C: tempo encoder left by 4".
    CHECK(encoderSteps(0x01) == 1);
    CHECK(encoderSteps(0x0a) == 10);
    CHECK(encoderSteps(0x7f) == -1);
    CHECK(encoderSteps(0x7c) == -4);
    CHECK(encoderSteps(0x40) == -64);

    // The strip: "E0 40 7F: at top", "E0 00 40: at center", "E0 00 00: at bottom".
    const auto bend = [](int lsb, int msb) { return (msb << 7) | lsb; };
    CHECK_THAT(stripPosition(bend(0x40, 0x7f)), WithinAbs(1.0, 1e-6));
    CHECK_THAT(stripPosition(bend(0x00, 0x40)), WithinAbs(0.5, 1e-6));
    CHECK_THAT(stripPosition(bend(0x00, 0x00)), WithinAbs(0.0, 1e-6));
    CHECK(stripPosition(bend(0x40, 0x40)) > 0.5f);
    CHECK(stripPosition(bend(0x40, 0x3f)) < 0.5f);

    // The buttons: the map's numbers, and RGB exactly where Push2-map.json says so.
    CHECK(ccOf({Role::play, 0}) == 85);
    CHECK(ccOf({Role::record, 0}) == 86);
    CHECK(ccOf({Role::metronome, 0}) == 9); // "0xB0 0x09: metronome button"
    CHECK(ccOf({Role::tapTempo, 0}) == 3);
    CHECK(ccOf({Role::scene, 0}) == 43); // scene 1, the top
    CHECK(ccOf({Role::scene, 7}) == 36);
    CHECK(ccOf({Role::displayUpper, 0}) == 102);
    CHECK(ccOf({Role::displayLower, 7}) == 27);
    CHECK(buttonOn(60)->id == ButtonId{Role::mute, 0}); // "0xB0 0x3C: the mute button RGB LED"
    CHECK(buttonOn(60)->rgb);
    CHECK_FALSE(buttonOn(119)->rgb); // "0xBF 0x77: the undo button white LED"
    CHECK_FALSE(buttonOn(28));       // Master: not a role (passes on to MIDI Map)
    CHECK_FALSE(buttonOn(59));       // User
    juce::Array<int> ccs;
    for (const auto& b : buttons())
    {
        INFO(buttonName(b.id));
        CHECK_FALSE(ccs.contains(b.cc));
        ccs.add(b.cc);
        CHECK((b.cc < firstEncoderCc || b.cc >= firstEncoderCc + encoderCount));
    }

    // The preset the modes are tested on has exactly these buttons, with the same lights.
    const auto caps = presets::push2();
    CHECK(caps.buttons.size() == buttons().size());
    for (const auto& b : caps.buttons)
    {
        INFO(buttonName(b.id));
        const int cc = ccOf(b.id);
        REQUIRE(cc >= 0);
        CHECK((b.led == LedKind::rgb) == buttonOn(cc)->rgb);
    }
}

TEST_CASE("Push protocol: every sysex the manual gives as an example, byte for byte", "[control][push]")
{
    // "F0 00 21 1D 01 01 03 7D 00 00 00 00 7F 01 7E 00 F7 = set entry 125 to 0/0/255 and 126".
    CHECK(bytesOf(setPaletteEntry(125, 0x0000ff, 126)) == hex("F0 00 21 1D 01 01 03 7D 00 00 00 00 7F 01 7E 00 F7"));
    CHECK(bytesOf(reapplyPalette()) == hex("F0 00 21 1D 01 01 05 F7"));
    CHECK(bytesOf(setLedBrightness(64)) == hex("F0 00 21 1D 01 01 06 40 F7"));
    CHECK(bytesOf(setDisplayBrightness(255)) == hex("F0 00 21 1D 01 01 08 7F 01 F7"));
    CHECK(bytesOf(setMidiMode(MidiMode::user)) == hex("F0 00 21 1D 01 01 0A 01 F7"));
    CHECK(bytesOf(setAftertouchMode(true)) == hex("F0 00 21 1D 01 01 1E 01 F7"));
    CHECK(bytesOf(setTouchStripConfiguration(stripDefault)) == hex("F0 00 21 1D 01 01 17 68 F7"));
    CHECK(bytesOf(identityRequest()) == hex("F0 7E 7F 06 01 F7"));
    // The manual's example identity reply.
    const auto reply = hex("F0 7E 01 06 02 00 21 1D 67 32 02 00 01 00 2F 00 73 4D 1F 08 00 01 F7");
    CHECK(isPushIdentity(juce::MidiMessage(reply.data(), static_cast<int>(reply.size()))));
    const auto other = hex("F0 7E 01 06 02 00 20 29 13 01 00 00 F7");
    CHECK_FALSE(isPushIdentity(juce::MidiMessage(other.data(), static_cast<int>(other.size()))));
    // Animations: a pulse a quarter note (channel 9), a blink an eighth (13).
    CHECK(animationChannel(LedState::Animation::solid) == 0);
    CHECK(animationChannel(LedState::Animation::pulse) == 9);
    CHECK(animationChannel(LedState::Animation::blink) == 13);
}

TEST_CASE("Push display: pixels as BGR565, lines with their filler, XORed: the same bytes as the reference made from the manual", "[control][push]")
{
    CHECK(pixel(0xff0000) == 0x001f); // red: the low five bits
    CHECK(pixel(0x00ff00) == 0x07e0); // green: the middle six
    CHECK(pixel(0x0000ff) == 0xf800); // blue: the top five
    CHECK(pixel(0xffffff) == 0xffff);
    CHECK(frameBytes == 327680);
    CHECK(frameBytes % 512 == 0); // 640 buffers of 512 bytes

    // One red pixel at the top left, the rest black.
    juce::Image dot(juce::Image::RGB, displayWidth, displayHeight, true);
    dot.setPixelAt(0, 0, juce::Colours::red);
    const auto d = encodeFrame(dot);
    REQUIRE(d.size() == static_cast<std::size_t>(frameBytes));
    CHECK(d[0] == (0x1f ^ 0xe7)); // the low byte first, XORed with E7
    CHECK(d[1] == (0x00 ^ 0xf3));
    CHECK(d[2] == 0xe7); // black, XORed
    CHECK(d[3] == 0xff);
    CHECK(d[1920] == 0xe7); // the filler, XORed too
    CHECK(d[2047] == 0xff);
    CHECK(d[2048] == 0xe7); // the next line starts the pattern again

    // The reference: make_reference.py's hash of the same pattern.
    const auto frame = encodeFrame(testPattern());
    std::uint64_t hash = 0xcbf29ce484222325ull; // FNV-1a, 64 bits
    for (const auto b : frame)
        hash = (hash ^ b) * 0x100000001b3ull;
    CHECK(hash == 0x456c7e91ffe8b605ull);

    // Smaller images (none should come, but they mustn't overrun): black beyond.
    const auto small = encodeFrame(juce::Image(juce::Image::RGB, 10, 10, true));
    CHECK(small[frameBytes - 1] == 0xff);
}

TEST_CASE("Push display: the udev rule shipped for Linux is the one the app tells people to install", "[control][push]")
{
    const auto file = juce::File(LATTICE_SOURCE_DIR).getChildFile("resources/linux/udev/50-lattice-ableton-push.rules");
    REQUIRE(file.existsAsFile());
    for (const auto& line : juce::StringArray::fromLines(udevRule()))
        CHECK(file.loadFileAsString().contains(line));
    CHECK(udevRule().contains("\"2982\""));
    CHECK(udevRule().contains("\"1967\""));
    for (const auto s : {DisplayStatus::notFound, DisplayStatus::noDriver, DisplayStatus::noPermission, DisplayStatus::busy,
                         DisplayStatus::failed, DisplayStatus::unsupported})
        CHECK(describe(s).isNotEmpty());
    CHECK(describe(DisplayStatus::connected).isEmpty());
    CHECK(describe(DisplayStatus::noDriver).contains("WinUSB"));
    CHECK(describe(DisplayStatus::noPermission).contains("udev"));
}

// --- The palette --------------------------------------------------------------------------------------

TEST_CASE("Push palette: each colour loaded once into a free entry; a shown entry never reloaded; full: the nearest", "[control][push]")
{
    Palette p;
    p.reset();
    std::vector<juce::MidiMessage> load;
    CHECK(p.forColour(0, load) == 0); // off: black, never loaded
    CHECK(load.empty());

    const int red = p.forColour(0xff0000, load);
    CHECK(red > 0);
    REQUIRE(load.size() == 1);
    CHECK(bytesOf(load[0]) == bytesOf(setPaletteEntry(red, 0xff0000, Palette::whiteFor(0xff0000))));
    load.clear();
    CHECK(p.forColour(0xff0000, load) == red); // already there
    CHECK(load.empty());
    CHECK(Palette::whiteFor(0xffffff) == 128); // as the device's own full white
    CHECK(Palette::whiteFor(0x808080) == 64);

    // White LEDs ask for a level: any entry of that level will do.
    const int white = p.forWhite(128, load);
    CHECK(p.whiteOf(white) == 128);
    CHECK(p.forWhite(Palette::whiteFor(0xff0000), load) == red); // red's entry has full white

    // Every entry shown: no new colour can take one, so the nearest is used and nothing reloads.
    p.reset();
    load.clear();
    for (int i = 1; i < Palette::size; ++i)
        p.use(p.forColour(static_cast<std::uint32_t>(0x010000 * i), load));
    CHECK(load.size() == static_cast<std::size_t>(Palette::size - 1));
    load.clear();
    const int near = p.forColour(0x7f0001, load);
    CHECK(load.empty());
    CHECK(p.colourOf(near) == 0x7f0000);

    // Let one go: the next new colour takes it (the one least recently used).
    p.release(5);
    const int fresh = p.forColour(0x00ff00, load);
    CHECK(fresh == 5);
    CHECK(load.size() == 1);
}

// --- The driver -----------------------------------------------------------------------------------------

TEST_CASE("Push driver: pads, pressure, encoders, the tempo encoder, the strip and buttons; the rest passes on", "[control][push]")
{
    Recorder out;
    PushDriver d(PushDriver::Model::push2, out, nullptr);
    Inputs in(d);
    using Kind = SurfaceInput::Kind;

    CHECK(d.capabilities().rows == 8);
    CHECK(d.capabilities().encoders == 8);
    CHECK(d.capabilities().displayWidth == displayWidth);
    CHECK(d.capabilities().aftertouch == Aftertouch::polyphonic);

    REQUIRE(d.receive(juce::MidiMessage::noteOn(1, 0x24, static_cast<juce::uint8>(127)))); // bottom left, hard
    REQUIRE(d.receive(juce::MidiMessage::aftertouchChange(1, 0x24, 64)));
    REQUIRE(d.receive(juce::MidiMessage::noteOff(1, 0x24)));
    REQUIRE(d.receive(juce::MidiMessage::noteOn(1, 0x63, static_cast<juce::uint8>(1)))); // top right, softly
    REQUIRE(d.receive(juce::MidiMessage::noteOn(1, 0x63, static_cast<juce::uint8>(0)))); // a note-on at 0 is a release
    d.deliverPending();
    REQUIRE(in.got.size() == 5);
    CHECK(in.got[0].kind == Kind::padDown);
    CHECK(in.got[0].row == 7);
    CHECK(in.got[0].column == 0);
    CHECK_THAT(in.got[0].value, WithinAbs(1.0, 1e-6));
    CHECK(in.got[1].kind == Kind::padPressure);
    CHECK_THAT(in.got[1].value, WithinAbs(64.0 / 127.0, 1e-6));
    CHECK(in.got[2].kind == Kind::padUp);
    CHECK(in.got[3].row == 0);
    CHECK(in.got[3].column == 7);
    CHECK(in.got[4].kind == Kind::padUp);
    in.got.clear();

    // Encoders: "0x90 0x47 0x7F: leftmost track encoder touched".
    REQUIRE(d.receive(juce::MidiMessage::noteOn(1, 0, static_cast<juce::uint8>(127))));
    REQUIRE(d.receive(juce::MidiMessage::controllerEvent(1, 71, 0x7d)));
    REQUIRE(d.receive(juce::MidiMessage::noteOn(1, 0, static_cast<juce::uint8>(0))));
    REQUIRE(d.receive(juce::MidiMessage::controllerEvent(1, 78, 3)));
    REQUIRE(d.receive(juce::MidiMessage::controllerEvent(1, tempoEncoderCc, 0x7f)));
    REQUIRE(d.receive(juce::MidiMessage::noteOn(1, tempoTouchNote, static_cast<juce::uint8>(127)))); // swallowed
    d.deliverPending();
    REQUIRE(in.got.size() == 5);
    CHECK(in.got[0].kind == Kind::encoderTouch);
    CHECK(in.got[1].kind == Kind::encoderTurn);
    CHECK(in.got[1].encoder == 0);
    CHECK(in.got[1].delta == -3);
    CHECK(in.got[2].kind == Kind::encoderRelease);
    CHECK(in.got[3].encoder == 7);
    CHECK(in.got[3].delta == 3);
    CHECK(in.got[4].kind == Kind::tempoTurn);
    CHECK(in.got[4].delta == -1);
    in.got.clear();

    // The strip, and a button.
    REQUIRE(d.receive(juce::MidiMessage::noteOn(1, stripTouchNote, static_cast<juce::uint8>(127))));
    REQUIRE(d.receive(juce::MidiMessage::pitchWheel(1, 16320)));
    REQUIRE(d.receive(juce::MidiMessage::noteOn(1, stripTouchNote, static_cast<juce::uint8>(0))));
    REQUIRE(d.receive(juce::MidiMessage::controllerEvent(1, 85, 127)));
    REQUIRE(d.receive(juce::MidiMessage::controllerEvent(1, 85, 0)));
    d.deliverPending();
    REQUIRE(in.got.size() == 4);
    CHECK(in.got[0].kind == Kind::strip);
    CHECK_THAT(in.got[0].value, WithinAbs(1.0, 1e-6));
    CHECK(in.got[1].kind == Kind::stripRelease);
    CHECK(in.got[2].kind == Kind::buttonDown);
    CHECK(in.got[2].button == ButtonId{Role::play, 0});
    CHECK(in.got[3].kind == Kind::buttonUp);

    // Not Lattice's: the master and swing encoders, Select, User, a pedal.
    CHECK_FALSE(d.receive(juce::MidiMessage::controllerEvent(1, masterEncoderCc, 1)));
    CHECK_FALSE(d.receive(juce::MidiMessage::noteOn(1, masterTouchNote, static_cast<juce::uint8>(127))));
    CHECK_FALSE(d.receive(juce::MidiMessage::controllerEvent(1, swingEncoderCc, 1)));
    CHECK_FALSE(d.receive(juce::MidiMessage::controllerEvent(1, 48, 127)));
    CHECK_FALSE(d.receive(juce::MidiMessage::controllerEvent(1, 59, 127)));
    CHECK_FALSE(d.receive(juce::MidiMessage::controllerEvent(1, 64, 127)));

    CHECK(d.describe(juce::MidiMessage::noteOn(1, 0x24, static_cast<juce::uint8>(9))) == "pad row 8, column 1");
    CHECK(d.describe(juce::MidiMessage::controllerEvent(1, 43, 127)) == "button scene[0]");
    CHECK(d.describe(juce::MidiMessage::controllerEvent(1, 72, 1)) == "encoder 2");
    CHECK(d.describe(juce::MidiMessage::controllerEvent(1, 48, 127)).isEmpty());
}

TEST_CASE("Push driver: lights as loaded palette entries, animations on the device's channels, only changes, within a budget", "[control][push]")
{
    Recorder out;
    PushDriver d(PushDriver::Model::push2, out, nullptr);
    d.connect();
    // Connecting: Live mode, polyphonic pressure, the strip, brightness; then every light off
    // (what Push showed before isn't known).
    const auto first = out.sysex();
    REQUIRE(first.size() >= 5);
    CHECK(bytesOf(first[0]) == bytesOf(setMidiMode(MidiMode::live)));
    CHECK(bytesOf(first[1]) == bytesOf(setAftertouchMode(true)));
    const auto& caps = d.capabilities();
    CHECK(out.lights().size() == static_cast<std::size_t>(caps.numPads()) + caps.buttons.size());
    for (const auto& m : out.lights())
        CHECK((m.isNoteOn(true) ? m.getVelocity() : m.getControllerValue()) == 0);
    out.clear();

    auto frame = LedFrame::blank(caps);
    const auto show = [&](const LedFrame& f) {
        auto changes = f.changesFrom(frame);
        frame = f;
        d.showLeds(f, changes);
    };

    // A solid pad: its colour loaded, then the pad set to that entry on channel 0.
    auto next = frame;
    next.pad(0, 0, 8) = LedState::solid(0x123456);
    show(next);
    REQUIRE(out.sysex().size() == 1);
    REQUIRE(out.lights().size() == 1);
    const auto pad = out.lights()[0];
    CHECK(pad.getChannel() == 1);
    CHECK(pad.getNoteNumber() == padNote(0, 0));
    const int entry = pad.getVelocity();
    CHECK(bytesOf(out.sysex()[0]) == bytesOf(setPaletteEntry(entry, 0x123456, Palette::whiteFor(0x123456))));
    out.clear();

    // The same colour on another pad: no loading.
    next.pad(0, 1, 8) = LedState::solid(0x123456);
    show(next);
    CHECK(out.sysex().empty());
    REQUIRE(out.lights().size() == 1);
    CHECK(out.lights()[0].getVelocity() == entry);
    out.clear();

    // Pulsing: its dim colour on channel 0, then the colour on the pulse channel; blinking: off,
    // then the colour on the blink channel.
    next.pad(1, 0, 8) = LedState::pulse(0x00ff00);
    next.pad(1, 1, 8) = LedState::blink(0x0000ff);
    show(next);
    const auto lights = out.lights();
    REQUIRE(lights.size() == 4);
    CHECK(lights[0].getChannel() == 1);
    CHECK(d.palette().colourOf(lights[0].getVelocity()) == dimmed(0x00ff00, 0.25f));
    CHECK(lights[1].getChannel() == pulseChannel + 1);
    CHECK(d.palette().colourOf(lights[1].getVelocity()) == 0x00ff00);
    CHECK(lights[2].getChannel() == 1);
    CHECK(lights[2].getVelocity() == 0);
    CHECK(lights[3].getChannel() == blinkChannel + 1);
    out.clear();

    // A white button: a CC set to an entry of its level.
    const int metronome = caps.indexOf({Role::metronome, 0});
    REQUIRE(metronome >= 0);
    next.buttons[static_cast<std::size_t>(metronome)] = LedState::solid(0xffffff);
    show(next);
    REQUIRE(out.lights().size() == 1);
    CHECK(out.lights()[0].getControllerNumber() == 9);
    CHECK(d.palette().whiteOf(out.lights()[0].getControllerValue()) == 128);
    out.clear();

    // Nothing changed: nothing sent.
    show(next);
    CHECK(out.sent.empty());

    // Every pad pulsing in a colour of its own: more than a frame's budget, so it goes over a few
    // frames, each within the budget, and all of it arrives.
    auto rainbow = next;
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c)
            rainbow.pad(r, c, 8) = LedState::pulse(0x200000u + static_cast<std::uint32_t>(r * 8 + c) * 0x020301u);
    show(rainbow);
    CHECK(out.bytes() <= PushDriver::bytesPerFrame);
    int frames = 1;
    for (int before = -1; before != out.bytes() && frames < 10; ++frames)
    {
        before = out.bytes();
        d.flush();
        CHECK(out.bytes() - before <= PushDriver::bytesPerFrame);
    }
    CHECK(frames > 2);
    std::set<int> padsLit;
    for (const auto& m : out.lights())
        if (m.getChannel() == pulseChannel + 1)
            padsLit.insert(m.getNoteNumber());
    CHECK(padsLit.size() == 64);

    // Letting go: every light off.
    out.clear();
    d.disconnect();
    for (const auto& m : out.lights())
        CHECK((m.isNoteOn(true) ? m.getVelocity() : m.getControllerValue()) == 0);
    CHECK(out.lights().size() >= 64);
}

TEST_CASE("Push driver: MIDI clock keeps the device's animations on the beat, and only while something moves", "[control][push]")
{
    Recorder out;
    double beat = 10.25;
    PushDriver d(PushDriver::Model::push2, out, nullptr, [&] { return beat; });
    d.connect();
    auto frame = LedFrame::blank(d.capabilities());
    d.tick();
    out.clear();
    d.tick();
    CHECK(out.sent.empty()); // nothing animated: no clock

    auto next = frame;
    next.pad(0, 0, 8) = LedState::blink(0xff0000);
    d.showLeds(next, next.changesFrom(frame));
    out.clear();
    d.tick();
    // A start (the device's phase at beat 10), then six clocks to 10.25.
    REQUIRE(!out.sent.empty());
    CHECK(out.sent[0].isMidiStart());
    int clocks = 0;
    for (const auto& m : out.sent)
        clocks += m.isMidiClock() ? 1 : 0;
    CHECK(clocks == 6);
    out.clear();
    beat = 10.5;
    d.tick();
    CHECK(out.sent.size() == 6); // six more, no start
    for (const auto& m : out.sent)
        CHECK(m.isMidiClock());
    out.clear();
    beat = 0.0; // the transport started again
    d.tick();
    REQUIRE(!out.sent.empty());
    CHECK(out.sent[0].isMidiStart());
}

TEST_CASE("Push display thread: header then 16 KB writes; a new frame at once, the same again each second; reopened when lost", "[control][push]")
{
    FakeLink fake;
    {
        DisplaySender sender(fake.opener(), 0.1, 0.05);
        CHECK(eventually([&] { return sender.status() == DisplayStatus::connected; }));
        CHECK(fake.count() == 0); // nothing to show yet
        sender.post(encodeFrame(testPattern()));
        REQUIRE(eventually([&] { return fake.count() >= 21; }));
        {
            const std::scoped_lock l(fake.lock);
            CHECK(fake.writes[0] == std::vector<std::uint8_t>(frameHeader.begin(), frameHeader.end()));
            std::vector<std::uint8_t> joined;
            for (std::size_t i = 1; i <= 20; ++i)
            {
                CHECK(fake.writes[i].size() == static_cast<std::size_t>(DisplaySender::chunkBytes));
                joined.insert(joined.end(), fake.writes[i].begin(), fake.writes[i].end());
            }
            CHECK(joined == encodeFrame(testPattern()));
        }
        // Nothing new: sent again before the display would go dark.
        CHECK(eventually([&] { return sender.framesSent() >= 3; }));

        // Unplugged: the writes fail, the link goes, and it's opened again.
        const int opensBefore = [&] {
            const std::scoped_lock l(fake.lock);
            fake.failWrites = true;
            return fake.opens;
        }();
        CHECK(eventually([&] {
            const std::scoped_lock l(fake.lock);
            return fake.opens > opensBefore;
        }));
        {
            const std::scoped_lock l(fake.lock);
            fake.failWrites = false;
        }
        const auto sentBefore = sender.framesSent();
        CHECK(eventually([&] { return sender.framesSent() > sentBefore; })); // the frame again on the new link
    }

    // No driver: the status says so, and it keeps trying.
    FakeLink refused;
    refused.refuse = DisplayStatus::noDriver;
    DisplaySender sender(refused.opener(), 0.1, 0.02);
    CHECK(eventually([&] { return sender.status() == DisplayStatus::noDriver; }));
    CHECK(eventually([&] {
        const std::scoped_lock l(refused.lock);
        return refused.opens >= 2;
    }));
}

TEST_CASE("Push driver: the display drawn at 960 x 160 and sent; its trouble is the driver's status", "[control][push]")
{
    Recorder out;
    FakeLink fake;
    PushDriver d(PushDriver::Model::push2, out, fake.opener());
    d.connect();
    DisplayFrame frame;
    frame.title = "Synth";
    frame.cells.resize(8);
    frame.cells[0] = {.name = "Cutoff", .value = "1.20 kHz", .position = 0.4f};
    d.showDisplay(frame);
    CHECK(d.displayImage().getWidth() == displayWidth);
    CHECK(d.displayImage().getHeight() == displayHeight);
    REQUIRE(d.display() != nullptr);
    CHECK(eventually([&] { return d.display()->framesSent() >= 1; }));
    CHECK(d.status().isEmpty());

    FakeLink refused;
    refused.refuse = DisplayStatus::noPermission;
    PushDriver noRule(PushDriver::Model::push2, out, refused.opener());
    CHECK(eventually([&] { return noRule.status().contains("udev"); }));
}

// --- In the app -----------------------------------------------------------------------------------------

namespace
{
struct World
{
    World()
    {
        session.newSet(4, 4);
        session.set().createClip(0, 0, 4.0);
        session.flush();
        ui::addSessionCommands(commands, session, {[this] { return selection.get(); }, [this](ui::SessionSelection s) { selection.set(s); }}, {});
    }
    SurfaceContext context() { return {session, commands, selection, feed}; }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer{engine, BlockPattern::Fixed64};
    ui::EngineStateFeed feed{session, engine};
    ui::CommandRegistry commands{&session.undoManager()};
    ui::SessionSelectionModel selection;
};
} // namespace

TEST_CASE("Push driver: every mode runs on it; a pad launches a clip through the fast path; the tempo encoder sets the tempo", "[control][push]")
{
    for (const auto model : {PushDriver::Model::push2, PushDriver::Model::push3})
    {
        World w;
        Recorder out;
        FakeLink fake;
        PushDriver driver(model, out, fake.opener());
        driver.connect();
        auto controller = std::make_unique<SurfaceController>(w.context(), driver);
        for (const auto kind : {ModeKind::session, ModeKind::note, ModeKind::device, ModeKind::mix, ModeKind::clip})
        {
            controller->setMode(kind);
            controller->refresh();
            if (const auto dir = juce::SystemStats::getEnvironmentVariable("LATTICE_WALKTHROUGH_DIR", {});
                dir.isNotEmpty() && driver.displayImage().isValid() && model == PushDriver::Model::push2)
                writePng(driver.displayImage(), juce::File(dir).getChildFile("push-display-" + juce::String(static_cast<int>(kind)) + ".png"));
        }
        controller->setMode(ModeKind::session);
        controller->refresh();
        CHECK(!out.lights().empty());
        CHECK(driver.displayImage().isValid()); // the display shows Mix or Device

        // The top-left pad: the first track's first clip.
        w.session.play();
        w.renderer.renderFor(64);
        REQUIRE(driver.receive(juce::MidiMessage::noteOn(1, padNote(0, 0), static_cast<juce::uint8>(100))));
        driver.deliverPending();
        w.session.flush();
        w.renderer.renderFor(64);
        w.session.processEngineEvents();
        w.feed.tick(5.0);
        CHECK(w.feed.slot(0, 0) != SlotState::Stopped);

        // Tempo: a BPM a step, a turn one undo step.
        const double before = w.session.set().tempo();
        for (int i = 0; i < 3; ++i)
            REQUIRE(driver.receive(juce::MidiMessage::controllerEvent(1, tempoEncoderCc, 1)));
        driver.deliverPending();
        CHECK_THAT(w.session.set().tempo(), WithinAbs(before + 3.0, 1e-9));
        w.session.undoManager().undo();
        CHECK_THAT(w.session.set().tempo(), WithinAbs(before, 1e-9));
        controller.reset();
        driver.disconnect();
    }
}

TEST_CASE("Push in the app: its Live port is recognised and offered, the User port isn't; added, it runs on the Push driver", "[control][push][surfaces]")
{
    World w;
    struct Ports final : ui::MidiPorts
    {
        juce::Array<juce::MidiDeviceInfo> ins, outs;
        juce::StringArray open;
        std::vector<juce::MidiMessage> sent;
        struct Sender final : generic::MidiSender
        {
            explicit Sender(std::vector<juce::MidiMessage>& s) : to(s) {}
            void send(const juce::MidiMessage& m) override { to.push_back(m); }
            std::vector<juce::MidiMessage>& to;
        };
        juce::Array<juce::MidiDeviceInfo> inputs() const override { return ins; }
        juce::Array<juce::MidiDeviceInfo> outputs() const override { return outs; }
        std::unique_ptr<generic::MidiSender> openOutput(const juce::String&) override { return std::make_unique<Sender>(sent); }
        void setInputOpen(const juce::String& id, bool on) override
        {
            if (on)
                open.addIfNotAlreadyThere(id);
            else
                open.removeString(id);
        }
        bool isInputOpen(const juce::String& id) const override { return open.contains(id); }
    } ports;
    // As Windows names them.
    for (const auto& name : {juce::String("Ableton Push 2"), juce::String("MIDIIN2 (Ableton Push 2)")})
        ports.ins.add({name, "in:" + name});
    for (const auto& name : {juce::String("Ableton Push 2"), juce::String("MIDIOUT2 (Ableton Push 2)")})
        ports.outs.add({name, "out:" + name});

    const auto settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("lattice-push", ".settings");
    juce::PropertiesFile::Options options;
    options.applicationName = "LatticeTests";
    juce::PropertiesFile settings(settingsFile, options);
    {
        FakeLink fake; // outlives the manager: its display thread opens it
        fake.refuse = DisplayStatus::noDriver;
        std::atomic<int> product{0};
        ui::SurfaceManager manager(w.engine, settings, ports, [&] { return w.context(); }, juce::File());
        manager.setDisplayOpener([&](int id) {
            product = id;
            return fake.opener()();
        });
        std::vector<ui::SurfaceManager::Offer> offers;
        manager.onOffer = [&](const ui::SurfaceManager::Offer& o) { offers.push_back(o); };
        manager.refreshPorts();
        REQUIRE(offers.size() == 1);
        CHECK(offers[0].definition == "Ableton Push 2");
        CHECK(offers[0].input == "Ableton Push 2"); // the Live port, not MIDIIN2
        CHECK(offers[0].output == "Ableton Push 2");
        const auto* def = manager.definition("Ableton Push 2");
        REQUIRE(def != nullptr);
        CHECK_FALSE(def->playsKeys());
        CHECK_FALSE(def->verified);

        const int i = manager.add({offers[0].definition, offers[0].input, offers[0].output, def->playsKeys(), true});
        REQUIRE(manager.isConnected(i));
        CHECK(dynamic_cast<PushDriver*>(manager.driver(i)) != nullptr);
        CHECK(!ports.sent.empty()); // connected: Live mode and the lights
        CHECK(bytesOf(ports.sent[0]) == bytesOf(setMidiMode(MidiMode::live)));
        CHECK(eventually([&] { return manager.status(i).contains("WinUSB"); }));
        CHECK(product.load() == push2ProductId);

        // A pad on the surface's port plays the session, not the tracks.
        w.session.play();
        w.renderer.renderFor(64);
        w.engine.receiveMidi("in:Ableton Push 2", juce::MidiMessage::noteOn(1, padNote(0, 0), static_cast<juce::uint8>(100)));
        manager.driver(i)->deliverPending();
        w.session.flush();
        w.renderer.renderFor(64);
        w.session.processEngineEvents();
        w.feed.tick(5.0);
        CHECK(w.feed.slot(0, 0) != SlotState::Stopped);
    }
    settingsFile.deleteFile();
}

TEST_CASE("Push definitions: they name their native driver; a driver Lattice doesn't have is an error", "[control][push]")
{
    for (const auto* name : {"Ableton Push 2", "Ableton Push 3"})
    {
        INFO(name);
        bool found = false;
        for (const auto& d : generic::SurfaceDefinition::included())
            if (d.name == name)
            {
                found = true;
                CHECK(d.driver.isNotEmpty());
                generic::SurfaceDefinition back;
                REQUIRE(generic::SurfaceDefinition::parse(d.toJson(), back).wasOk());
                CHECK(back.toJson() == d.toJson());
                CHECK(back.driver == d.driver);
            }
        CHECK(found);
    }
    generic::SurfaceDefinition d;
    const auto r = generic::SurfaceDefinition::parse(R"({"lattice": "surface", "version": 1, "name": "X", "driver": "push9", "ports": {"input": ["X"]}})", d);
    CHECK(r.failed());
    CHECK(r.getErrorMessage().contains("driver"));
}

TEST_CASE("Push response: lights go within the frame they change in; only a redraw of new colours past a frame's budget takes more",
          "[control][push][response]")
{
    // docs/06-m5-controllers.md, 5g (the charter's ≤ 1 frame).
    Recorder out;
    PushDriver d(PushDriver::Model::push2, out, nullptr);
    d.connect();
    const auto& caps = d.capabilities();
    auto frame = LedFrame::blank(caps);
    const auto show = [&](const LedFrame& f) {
        auto changes = f.changesFrom(frame);
        frame = f;
        d.showLeds(f, changes);
    };
    const auto framesToSend = [&](const LedFrame& f) {
        const auto before = d.ledDelays().summary().count;
        show(f);
        int frames = 0;
        while (d.ledDelays().summary().count == before && frames < 20)
        {
            d.tick();
            ++frames;
        }
        return frames; // 0: all sent within the frame that changed them
    };
    d.ledDelays().reset();

    // A page of clips in 16 colours, shown for the first time: their colours loaded too.
    const auto page = [&](int shift, bool pulse) {
        auto f = LedFrame::blank(caps);
        for (int i = 0; i < caps.numPads(); ++i)
        {
            const auto rgb = 0x101010u * static_cast<std::uint32_t>(1 + (i + shift) % 15) + 0x200000u;
            f.pads[static_cast<std::size_t>(i)] = pulse ? LedState::pulse(rgb) : LedState::solid(rgb);
        }
        return f;
    };
    const int firstShown = framesToSend(page(0, false));
    INFO("a first page of new colours: " << firstShown << " frames more");
    CHECK(firstShown <= 1);
    // Scrolling to the next page: the same colours, loaded already: all within the frame.
    CHECK(framesToSend(page(3, false)) == 0);
    CHECK(framesToSend(page(7, false)) == 0);
    // Every pad pulsing (two entries each): still within a frame once its colours are loaded.
    framesToSend(page(0, true));
    CHECK(framesToSend(page(5, true)) == 0);
    const auto s = d.ledDelays().summary();
    CHECK(s.count == 5); // one for each page
    CHECK(s.maxMs < 1000.0 / 60.0 * (firstShown + 1));
}

TEST_CASE("Push response: a display frame is written within a frame of being posted", "[control][push][response]")
{
    FakeLink fake;
    DisplaySender sender(fake.opener());
    REQUIRE(eventually([&] { return sender.status() == DisplayStatus::connected; }));
    for (int i = 0; i < 5; ++i)
    {
        const auto before = sender.delays().summary().count;
        sender.post(encodeFrame(testPattern()));
        REQUIRE(eventually([&] { return sender.delays().summary().count > before; }));
    }
    const auto s = sender.delays().summary();
    INFO("post to written: p50 " << s.p50Ms << " ms, max " << s.maxMs << " ms");
    CHECK(s.count == 5);
    // At most the wait for the next 60 fps slot plus the writes (instant on the fake link); a
    // loaded test machine may wake the thread late, so this checks the order of it.
    CHECK(s.p50Ms < 1000.0 / 60.0 + 5.0);
}
