#include "control/push/PushDriver.h"

#include "control/display/DisplayRenderer.h"
#include "control/push/PushProtocol.h"
#include "control/surface/Presets.h"

#include <algorithm>
#include <cmath>

namespace lattice::control::push
{

namespace
{
/** A sysex's bytes on the wire, F0 and F7 too; a short message's three (or two). */
int bytesOf(const juce::MidiMessage& m) noexcept
{
    return m.getRawDataSize();
}

/** How much dimmer a pulse's low point is than its colour. */
constexpr float pulseLow = 0.25f;
} // namespace

PushDriver::PushDriver(Model m, generic::MidiSender& output, LinkOpener opener, std::function<double()> beat)
    : model(m), caps(m == Model::push2 ? presets::push2() : presets::push3()), out(output), beatNow(std::move(beat)), theme(ui::Theme::dark())
{
    // Push 3's MPE waits for a unit to test with: its pads as Push 2's until then.
    caps.aftertouch = Aftertouch::polyphonic;
    for (int r = 0; r < caps.rows; ++r)
        for (int c = 0; c < caps.columns; ++c)
            lights.push_back({.note = true, .number = padNote(r, c), .rgb = true});
    for (const auto& b : caps.buttons)
    {
        const int cc = ccOf(b.id);
        const auto* button = buttonOn(cc);
        lights.push_back({.note = false, .number = cc, .rgb = button == nullptr || button->rgb, .dirty = cc >= 0});
    }
    colours.reset();
    if (opener)
        sender = std::make_unique<DisplaySender>(std::move(opener));
}

PushDriver::~PushDriver()
{
    stopTimer();
}

// --- Input ------------------------------------------------------------------------------------------

bool PushDriver::receive(const juce::MidiMessage& m, double seconds)
{
    if (m.isNoteOnOrOff())
    {
        const int note = m.getNoteNumber();
        const bool down = m.isNoteOn();
        if (const auto pad = padOf(note))
        {
            held[static_cast<std::size_t>(note - firstPadNote)] = down;
            input(down ? SurfaceInput::padDown(pad->row, pad->column, m.getFloatVelocity()) : SurfaceInput::padUp(pad->row, pad->column), seconds);
            return true;
        }
        if (note >= 0 && note < encoderCount)
        {
            input(down ? SurfaceInput::encoderTouch(note) : SurfaceInput::encoderRelease(note), seconds);
            return true;
        }
        if (note == stripTouchNote)
        {
            // The position comes as pitch bend; letting go ends the bend (after Push's return to
            // the middle, which it sends first).
            if (!down)
                input(SurfaceInput::stripRelease(), seconds);
            return true;
        }
        return note == tempoTouchNote; // the master's and swing's touches pass on, with their turns
    }
    if (m.isAftertouch())
    {
        const auto pad = padOf(m.getNoteNumber());
        if (!pad)
            return false;
        input(SurfaceInput::padPressure(pad->row, pad->column, static_cast<float>(m.getAfterTouchValue()) / 127.0f), seconds);
        return true;
    }
    if (m.isChannelPressure())
    {
        // Channel pressure mode (Push's default, should something have set it back): every pad held.
        const float pressure = static_cast<float>(m.getChannelPressureValue()) / 127.0f;
        for (int i = 0; i < 64; ++i)
            if (held[static_cast<std::size_t>(i)])
                if (const auto pad = padOf(firstPadNote + i))
                    input(SurfaceInput::padPressure(pad->row, pad->column, pressure), seconds);
        return true;
    }
    if (m.isPitchWheel())
    {
        input(SurfaceInput::strip(stripPosition(m.getPitchWheelValue())), seconds);
        return true;
    }
    if (!m.isController())
        return false;
    const int cc = m.getControllerNumber(), value = m.getControllerValue();
    if (cc >= firstEncoderCc && cc < firstEncoderCc + encoderCount)
    {
        if (const int steps = encoderSteps(value); steps != 0)
            input(SurfaceInput::encoderTurn(cc - firstEncoderCc, steps), seconds);
        return true;
    }
    if (cc == tempoEncoderCc)
    {
        if (const int steps = encoderSteps(value); steps != 0)
            input(SurfaceInput::tempoTurn(steps), seconds);
        return true;
    }
    if (const auto* b = buttonOn(cc))
    {
        input(value > 0 ? SurfaceInput::buttonDown(b->id) : SurfaceInput::buttonUp(b->id), seconds);
        return true;
    }
    return false;
}

juce::String PushDriver::describe(const juce::MidiMessage& m) const
{
    if (m.isNoteOnOrOff() || m.isAftertouch())
    {
        const int note = m.getNoteNumber();
        if (const auto pad = padOf(note))
            return "pad row " + juce::String(pad->row + 1) + ", column " + juce::String(pad->column + 1) + (m.isAftertouch() ? " pressure" : "");
        if (m.isAftertouch())
            return {};
        if (note >= 0 && note < encoderCount)
            return "encoder " + juce::String(note + 1) + " touch";
        if (note == stripTouchNote)
            return "touch strip touch";
        if (note == tempoTouchNote)
            return "tempo encoder touch";
        return {};
    }
    if (m.isChannelPressure())
        return "pressure on the pads held";
    if (m.isPitchWheel())
        return "touch strip";
    if (!m.isController())
        return {};
    const int cc = m.getControllerNumber();
    if (cc >= firstEncoderCc && cc < firstEncoderCc + encoderCount)
        return "encoder " + juce::String(cc - firstEncoderCc + 1);
    if (cc == tempoEncoderCc)
        return "tempo encoder";
    if (const auto* b = buttonOn(cc))
        return "button " + buttonName(b->id);
    return {};
}

juce::String PushDriver::status() const
{
    return sender != nullptr ? push::describe(sender->status()) : juce::String();
}

// --- Lights -------------------------------------------------------------------------------------

void PushDriver::showLeds(const LedFrame& frame, const LedFrame::Changes& changed)
{
    const auto padCount = static_cast<int>(caps.numPads());
    const auto want = [&](std::size_t light, const LedState& s) {
        if (light >= lights.size())
            return;
        auto& l = lights[light];
        if (l.wanted == s)
            return;
        l.wanted = s;
        l.dirty = l.number >= 0;
    };
    for (const int i : changed.pads)
        if (i >= 0 && i < padCount && i < static_cast<int>(frame.pads.size()))
            want(static_cast<std::size_t>(i), frame.pads[static_cast<std::size_t>(i)]);
    for (const int i : changed.buttons)
        if (i >= 0 && i < static_cast<int>(frame.buttons.size()))
            want(static_cast<std::size_t>(padCount + i), frame.buttons[static_cast<std::size_t>(i)]);
    if (dirtySince < 0.0 && std::any_of(lights.begin(), lights.end(), [](const Light& l) { return l.dirty; }))
        dirtySince = juce::Time::getMillisecondCounterHiRes();
    flush();
    startTimerIfNeeded();
}

bool PushDriver::sendLight(Light& l, int& budget)
{
    // The most one light can take: two palette entries loaded and two messages.
    constexpr int worst = 2 * 17 + 2 * 3;
    if (budget < worst)
        return false;
    std::vector<juce::MidiMessage> load;
    const auto entry = [&](std::uint32_t rgb) {
        const int i = l.rgb ? colours.forColour(rgb, load) : colours.forWhite(Palette::whiteFor(rgb), load);
        colours.use(i); // before the next is chosen, so it can't take this one
        return i;
    };
    Shown now;
    const auto& s = l.wanted;
    if (!s.isOff())
        switch (s.animation)
        {
        case LedState::Animation::solid: now.base = entry(s.rgb); break;
        case LedState::Animation::pulse:
            now.base = entry(dimmed(s.rgb, pulseLow));
            now.animated = entry(s.rgb);
            now.channel = pulseChannel;
            break;
        case LedState::Animation::blink:
            now.animated = entry(s.rgb);
            now.channel = blinkChannel;
            break;
        }
    if (now != l.device)
    {
        for (const auto& m : load)
        {
            out.send(m);
            budget -= bytesOf(m);
        }
        const auto set = [&](int channel, int index) {
            const auto m = l.note ? juce::MidiMessage::noteOn(channel + 1, l.number, static_cast<juce::uint8>(index))
                                  : juce::MidiMessage::controllerEvent(channel + 1, l.number, index);
            out.send(m);
            budget -= bytesOf(m);
        };
        // The animation's first colour on channel 0, then its second on the animation's channel.
        set(0, now.base);
        if (now.channel != 0)
            set(now.channel, now.animated);
    }
    colours.release(l.device.base);
    colours.release(l.device.animated);
    l.device = now;
    l.dirty = false;
    return true;
}

void PushDriver::flush()
{
    int budget = bytesPerFrame;
    const auto n = lights.size();
    for (std::size_t k = 0; k < n; ++k)
    {
        const auto i = (nextDirty + k) % n;
        auto& l = lights[i];
        if (!l.dirty)
            continue;
        if (!sendLight(l, budget))
        {
            nextDirty = i; // the next frame starts here, so every light gets its turn
            return;
        }
    }
    nextDirty = 0;
    if (dirtySince >= 0.0)
    {
        ledsAfter.record(juce::Time::getMillisecondCounterHiRes() - dirtySince);
        dirtySince = -1.0;
    }
}

bool PushDriver::anyAnimatedShown() const noexcept
{
    for (const auto& l : lights)
        if (l.device.channel != 0)
            return true;
    return false;
}

void PushDriver::sendClock()
{
    if (!anyAnimatedShown())
        return; // nothing moves: no clock (idle)
    const double beat = beatNow ? beatNow() : juce::Time::getMillisecondCounterHiRes() * 0.001 * 2.0;
    const auto target = static_cast<std::int64_t>(std::floor(beat * 24.0));
    // A start puts the device's phase at a whole beat; the clocks since then bring it to now. A
    // jump (the transport started, or the lights were still a while) starts again.
    if (!clockStarted || target < clocks || target - clocks > 96)
    {
        out.send(juce::MidiMessage::midiStart());
        clocks = static_cast<std::int64_t>(std::floor(beat)) * 24;
        clockStarted = true;
    }
    for (; clocks < target; ++clocks)
        out.send(juce::MidiMessage::midiClock());
}

void PushDriver::timerCallback()
{
    flush();
    sendClock();
    startTimerIfNeeded();
}

void PushDriver::startTimerIfNeeded()
{
    bool dirty = false;
    for (const auto& l : lights)
        dirty = dirty || l.dirty;
    if (dirty || anyAnimatedShown())
    {
        if (!isTimerRunning())
            startTimerHz(60);
    }
    else
        stopTimer();
}

// --- The display ------------------------------------------------------------------------------------

void PushDriver::showDisplay(const DisplayFrame& frame)
{
    lastFrame = frame;
    hasFrame = true;
    if (sender == nullptr)
        return;
    image = DisplayRenderer::render(frame, displayWidth, displayHeight, caps.cellsAcross(), theme);
    sender->post(encodeFrame(image));
}

void PushDriver::setTheme(const ui::Theme& t)
{
    theme = t;
    if (hasFrame)
        showDisplay(lastFrame);
}

// --- Connecting -------------------------------------------------------------------------------------

void PushDriver::connect()
{
    out.send(setMidiMode(MidiMode::live));
    out.send(setAftertouchMode(true));
    out.send(setTouchStripConfiguration(stripDefault));
    out.send(setLedBrightness(127));     // limited to 8 by Push itself on USB power alone
    out.send(setDisplayBrightness(255));
    // What the device shows now isn't known (Live may have lit it): every light again, every
    // colour loaded again.
    colours.reset();
    for (auto& l : lights)
    {
        l.device = {.base = -1, .animated = 0, .channel = 0};
        l.dirty = l.number >= 0;
    }
    clockStarted = false;
    flush();
    startTimerIfNeeded();
    if (hasFrame)
        showDisplay(lastFrame);
}

void PushDriver::disconnect()
{
    stopTimer();
    for (auto& l : lights)
    {
        l.wanted = LedState::off();
        l.dirty = l.number >= 0 && l.device != Shown{};
    }
    // All at once: there's no next frame.
    for (auto& l : lights)
        if (l.dirty)
        {
            int unlimited = 1 << 30;
            sendLight(l, unlimited);
        }
}

} // namespace lattice::control::push
