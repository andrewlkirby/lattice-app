#include "ui/controllers/SurfaceLearner.h"

#include "control/generic/GenericMidiDriver.h"

#include <algorithm>

namespace lattice::ui
{

using control::generic::Encoding;
using control::generic::LedOutput;
using control::generic::SurfaceDefinition;

namespace
{
juce::String describe(const juce::MidiMessage& m)
{
    const auto ch = " on channel " + juce::String(m.getChannel());
    if (m.isNoteOn())
        return "note " + juce::String(m.getNoteNumber()) + ch + ", velocity " + juce::String(m.getVelocity());
    if (m.isNoteOff())
        return "note " + juce::String(m.getNoteNumber()) + " off" + ch;
    if (m.isController())
        return "CC " + juce::String(m.getControllerNumber()) + ch + ", value " + juce::String(m.getControllerValue());
    if (m.isAftertouch())
        return "pressure on note " + juce::String(m.getNoteNumber()) + ch;
    if (m.isChannelPressure())
        return "channel pressure" + ch;
    if (m.isSysEx())
        return "a sysex message";
    return m.getDescription();
}

/** A port name as a pattern matching only it. */
juce::String exactly(const juce::String& name)
{
    juce::String out;
    for (const auto c : name)
    {
        if (juce::String("\\^$.|?*+()[]{}").containsChar(c))
            out << '\\';
        out << c;
    }
    return out;
}
} // namespace

void SurfaceLearner::start(const juce::String& name, const juce::String& input, const juce::String& output, int r, int c)
{
    *this = {};
    result.name = name;
    result.source = "Learned with Lattice's learn wizard on " + juce::Time::getCurrentTime().formatted("%Y-%m-%d") + ".";
    result.notes = "Learned from the device: check the roles and the lights (Preferences > Controllers > Monitor).";
    if (input.isNotEmpty())
        result.inputPorts.add(exactly(input));
    if (output.isNotEmpty())
        result.outputPorts.add(exactly(output));
    else
        result.outputPorts = result.inputPorts;
    rows = r > 0 && c > 0 ? r : 0;
    columns = rows > 0 ? c : 0;
    now = rows > 0 ? Step::grid : Step::encoders;
}

juce::String SurfaceLearner::instruction() const
{
    switch (now)
    {
    case Step::grid:
        if (padByPad)
            return "Press the pad in row " + juce::String(padsLearned() / columns + 1) + ", column " + juce::String(padsLearned() % columns + 1) +
                   " (counting from the top-left).";
        if (pads.empty())
            return "Press the grid's top-left pad.";
        if (pads.size() == 1 && columns > 1)
            return "Press the pad just to its right.";
        return "Press the grid's bottom-right pad.";
    case Step::encoders:
        if (!turning.cc && turning.nrpn < 0)
            return "Turn the next encoder (or fader) clockwise, a little, a few times. No more? Go on to the buttons.";
        if (!turning.backwards)
            return "Keep turning it clockwise, then say so and turn it back.";
        return "Turn it anticlockwise a few times, then say it's done.";
    case Step::buttons:
        return hasPendingButton() ? "Choose what that button does." : "Press a button to give it a role. No more? Go on to the lights.";
    case Step::lights: return "Try each way of lighting: choose the one that lights the corners red, green, blue and white.";
    case Step::done: break;
    }
    return "Save the definition.";
}

std::optional<SurfaceLearner::MidiAddress> SurfaceLearner::addressOf(const juce::MidiMessage& m)
{
    if (m.isNoteOnOrOff() || m.isAftertouch())
        return MidiAddress{MidiAddress::Type::note, m.getChannel() - 1, m.getNoteNumber()};
    if (m.isController())
        return MidiAddress{MidiAddress::Type::cc, m.getChannel() - 1, m.getControllerNumber()};
    return std::nullopt;
}

bool SurfaceLearner::taken(const MidiAddress& a) const
{
    const auto def = definition();
    if (std::find(def.grid.pads.begin(), def.grid.pads.end(), a) != def.grid.pads.end())
        return true;
    for (const auto& b : def.buttons)
        if (b.address == a)
            return true;
    for (const auto& e : def.encoders)
        if (e.encoding != Encoding::nrpn14 && (e.address == a || (e.lsb >= 0 && MidiAddress{MidiAddress::Type::cc, e.address.channel, e.lsb} == a)))
            return true;
    return false;
}

void SurfaceLearner::heard(const juce::MidiMessage& m)
{
    if (m.isActiveSense() || m.isMidiClock())
        return;
    heardText = describe(m);
    if (now == Step::grid)
    {
        if (m.isAftertouch())
            result.grid.aftertouch = control::Aftertouch::polyphonic;
        else if (m.isChannelPressure() && result.grid.aftertouch == control::Aftertouch::none)
            result.grid.aftertouch = control::Aftertouch::channel;
        const bool press = m.isNoteOn() || (m.isController() && m.getControllerValue() > 0);
        if (const auto a = addressOf(m); a && press)
            gridPress(*a, m);
        return;
    }
    if (now == Step::encoders)
    {
        if (m.isController())
            encoderMessage(m);
        return;
    }
    if (now == Step::buttons)
    {
        const bool press = m.isNoteOn() || (m.isController() && m.getControllerValue() > 0);
        if (const auto a = addressOf(m); a && press)
        {
            if (taken(*a))
                heardText << " (already learned)";
            else
                pending = *a;
        }
    }
}

// --- The grid -----------------------------------------------------------------------------------------

void SurfaceLearner::gridPress(const MidiAddress& a, const juce::MidiMessage& m)
{
    if (std::find(pads.begin(), pads.end(), a) != pads.end())
    {
        heardText << " (already pressed)";
        return;
    }
    velocities.push_back(m.isNoteOn() ? m.getVelocity() : m.getControllerValue());
    pads.push_back(a);
    if (padByPad)
    {
        if (padsLearned() == rows * columns)
            finishGrid();
        return;
    }
    const std::size_t corners = columns > 1 ? 3u : 2u;
    if (rows == 1 && columns == 1)
    {
        finishGrid();
        return;
    }
    if (pads.size() < corners)
        return;

    // Evenly numbered: the first's number, a step to the right, the step down from the corners.
    const auto& first = pads.front();
    const auto& last = pads.back();
    const int columnStep = columns > 1 ? pads[1].number - first.number : 1;
    const int across = (columns - 1) * columnStep;
    const bool same = std::all_of(pads.begin(), pads.end(), [&](const MidiAddress& p) { return p.type == first.type && p.channel == first.channel; });
    int rowStep = 0;
    bool even = same && columnStep != 0;
    if (even && rows > 1)
    {
        const int down = last.number - first.number - across;
        even = down % (rows - 1) == 0;
        rowStep = even ? down / (rows - 1) : 0;
    }
    else if (even)
        even = last.number - first.number == across;
    std::vector<MidiAddress> all;
    for (int r = 0; r < rows && even; ++r)
        for (int c = 0; c < columns && even; ++c)
        {
            const int n = first.number + r * rowStep + c * columnStep;
            even = n >= 0 && n <= 127;
            all.push_back({first.type, first.channel, n});
        }
    if (even)
    {
        // Each pad its own number.
        auto sorted = all;
        std::sort(sorted.begin(), sorted.end(), [](const auto& x, const auto& y) { return x.number < y.number; });
        even = std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
    }
    if (even)
    {
        pads = all;
        finishGrid();
        return;
    }
    // Not evenly numbered: pad by pad from the top-left (already pressed).
    padByPad = true;
    pads.resize(1);
    velocities.resize(1);
    heardText << ": not numbered evenly, so each pad in turn";
}

void SurfaceLearner::finishGrid()
{
    result.grid.rows = rows;
    result.grid.columns = columns;
    result.grid.pads = pads;
    // Velocity: if every press came at full (or at one value), the pads have none.
    const bool varied = std::adjacent_find(velocities.begin(), velocities.end(), std::not_equal_to<>()) != velocities.end();
    result.grid.velocity = varied || (!velocities.empty() && velocities.front() != 127);
    now = Step::encoders;
}

// --- Encoders -----------------------------------------------------------------------------------------

void SurfaceLearner::encoderMessage(const juce::MidiMessage& m)
{
    const int cc = m.getControllerNumber(), value = m.getControllerValue(), channel = m.getChannel() - 1;
    // An NRPN: 99 and 98 name the parameter; 6 (and 38) carry the value.
    if (cc == 99)
    {
        turning.nrpnMsb = value;
        return;
    }
    if (cc == 98)
    {
        turning.nrpn = turning.nrpnMsb * 128 + value;
        turning.channel = channel;
        return;
    }
    if ((cc == 6 || cc == 38) && turning.nrpn >= 0)
    {
        if (cc == 6)
            (turning.backwards ? turning.anticlockwise : turning.clockwise).push_back(value);
        return;
    }
    if (!turning.cc)
    {
        const MidiAddress a{MidiAddress::Type::cc, channel, cc};
        if (taken(a))
        {
            heardText << " (already learned)";
            return;
        }
        turning.cc = cc;
        turning.channel = channel;
    }
    if (channel != turning.channel)
        return;
    if (cc == *turning.cc + 32)
    {
        turning.lsbSeen = true;
        return;
    }
    if (cc == *turning.cc)
        (turning.backwards ? turning.anticlockwise : turning.clockwise).push_back(value);
}

void SurfaceLearner::encoderTurnedClockwise()
{
    turning.backwards = true;
}

bool SurfaceLearner::encoderDone()
{
    SurfaceDefinition::Encoder e;
    if (turning.nrpn >= 0)
    {
        e.encoding = Encoding::nrpn14;
        e.nrpn = turning.nrpn;
        e.address = {MidiAddress::Type::cc, turning.channel, 99};
    }
    else if (turning.cc)
    {
        e.address = {MidiAddress::Type::cc, turning.channel, *turning.cc};
        if (turning.lsbSeen)
        {
            e.encoding = Encoding::cc14;
            e.lsb = *turning.cc + 32;
        }
        else if (const auto found = control::generic::detectEncoding(turning.clockwise, turning.anticlockwise))
            e.encoding = *found;
        else
        {
            heardText = "Its values didn't say how it counts: turn it a few more times each way.";
            turning.clockwise.clear();
            turning.anticlockwise.clear();
            turning.backwards = false;
            return false;
        }
    }
    else
    {
        heardText = "Nothing came from an encoder: turn one, then try again.";
        return false;
    }
    result.encoders.push_back(e);
    heardText = "Encoder " + juce::String(encodersLearned()) + ": " + control::generic::encodingName(e.encoding);
    turning = {};
    return true;
}

void SurfaceLearner::encodersFinished()
{
    turning = {};
    now = Step::buttons;
}

// --- Buttons ------------------------------------------------------------------------------------------

bool SurfaceLearner::assign(control::ButtonId id)
{
    if (!pending)
        return false;
    for (const auto& b : result.buttons)
        if (b.id == id)
            return false;
    // A numbered role's buttons run 0, 1, 2...: only the next one.
    if (control::isNumbered(id.role))
    {
        const auto count = std::count_if(result.buttons.begin(), result.buttons.end(), [&](const auto& b) { return b.id.role == id.role; });
        if (id.index != static_cast<int>(count))
            return false;
    }
    else if (id.index != 0)
        return false;
    result.buttons.push_back({id, *pending, control::LedKind::none, {}});
    pending.reset();
    return true;
}

void SurfaceLearner::buttonsFinished()
{
    pending.reset();
    now = Step::lights;
}

// --- Lights -------------------------------------------------------------------------------------------

const std::vector<SurfaceLearner::LightChoice>& SurfaceLearner::lightChoices()
{
    static const std::vector<LightChoice> choices = [] {
        std::vector<LightChoice> c;
        LedOutput palette;
        palette.kind = LedOutput::Kind::palette;
        palette.paletteName = "novation";
        palette.palette = control::generic::namedPalette("novation");
        c.push_back({"Colours by velocity (Novation and Akai's 128-colour table)", palette});
        auto bright = palette;
        bright.paletteName = "akai";
        bright.channel = 6;
        c.push_back({"Colours by velocity, on channel 7 (Akai: full brightness)", bright});
        LedOutput on127;
        on127.kind = LedOutput::Kind::single;
        c.push_back({"On and off: 127 and 0", on127});
        auto on1 = on127;
        on1.onValue = 1;
        on1.blinkValue = 2;
        c.push_back({"On and off: 1 and 0 (2 blinks)", on1});
        c.push_back({"No lights", {}});
        return c;
    }();
    return choices;
}

std::vector<juce::MidiMessage> SurfaceLearner::testPattern(int choice) const
{
    struct Collect final : control::generic::MidiSender
    {
        void send(const juce::MidiMessage& m) override { sent.push_back(m); }
        std::vector<juce::MidiMessage> sent;
    } out;
    if (choice < 0 || choice >= static_cast<int>(lightChoices().size()))
        return {};
    auto def = definition();
    const auto& led = lightChoices()[static_cast<std::size_t>(choice)].led;
    def.grid.led = led;
    for (auto& b : def.buttons)
    {
        b.led = led;
        b.ledKind = led.kind == LedOutput::Kind::none ? control::LedKind::none : control::LedKind::rgb;
    }
    control::generic::GenericMidiDriver driver(def, out);
    const auto& caps = driver.capabilities();
    auto frame = control::LedFrame::blank(caps);
    if (caps.numPads() > 0)
    {
        frame.pad(0, 0, caps.columns) = control::LedState::solid(0xff0000);
        frame.pad(0, caps.columns - 1, caps.columns) = control::LedState::solid(0x00ff00);
        frame.pad(caps.rows - 1, 0, caps.columns) = control::LedState::solid(0x0000ff);
        frame.pad(caps.rows - 1, caps.columns - 1, caps.columns) = control::LedState::solid(0xffffff);
    }
    for (auto& b : frame.buttons)
        b = control::LedState::solid(0xffffff);
    // Everything, so lights another way tried before go out.
    control::LedFrame::Changes all;
    for (int i = 0; i < static_cast<int>(frame.pads.size()); ++i)
        all.pads.push_back(i);
    for (int i = 0; i < static_cast<int>(frame.buttons.size()); ++i)
        all.buttons.push_back(i);
    driver.showLeds(frame, all);
    return out.sent;
}

void SurfaceLearner::chooseLights(int choice)
{
    if (choice < 0 || choice >= static_cast<int>(lightChoices().size()))
        return;
    const auto& led = lightChoices()[static_cast<std::size_t>(choice)].led;
    result.grid.led = led;
    const bool rgb = led.kind == LedOutput::Kind::palette || led.kind == LedOutput::Kind::sysex;
    result.rgbLed = rgb ? led : LedOutput{};
    result.singleLed = led.kind == LedOutput::Kind::single ? led : LedOutput{};
    for (auto& b : result.buttons)
        b.ledKind = led.kind == LedOutput::Kind::none ? control::LedKind::none : rgb ? control::LedKind::rgb : control::LedKind::single;
    now = Step::done;
}

SurfaceDefinition SurfaceLearner::definition() const
{
    auto d = result;
    if (now == Step::grid)
    {
        d.grid = {}; // not finished
        if (padByPad)
        {
            d.grid.rows = rows;
            d.grid.columns = columns;
        }
    }
    d.verified = false;
    return d;
}

} // namespace lattice::ui
