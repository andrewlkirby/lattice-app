#include "control/generic/GenericMidiDriver.h"

#include "control/surface/PadPlayer.h"

namespace lattice::control::generic
{

GenericMidiDriver::GenericMidiDriver(SurfaceDefinition definition, MidiSender& output)
    : def(std::move(definition)), caps(def.capabilities()), out(output)
{
    for (std::size_t i = 0; i < def.grid.pads.size(); ++i)
    {
        const auto& a = def.grid.pads[i];
        target(a.type, a.channel, a.number) = {Part::pad, static_cast<std::uint16_t>(i)};
    }
    for (std::size_t i = 0; i < def.buttons.size(); ++i)
    {
        const auto& a = def.buttons[i].address;
        target(a.type, a.channel, a.number) = {Part::button, static_cast<std::uint16_t>(i)};
    }
    for (std::size_t i = 0; i < def.encoders.size(); ++i)
    {
        const auto& e = def.encoders[i];
        const auto index = static_cast<std::uint16_t>(i);
        if (e.encoding != Encoding::nrpn14)
            target(e.address.type, e.address.channel, e.address.number) = {Part::encoder, index};
        if (e.lsb >= 0)
            target(MidiAddress::Type::cc, e.address.channel, e.lsb) = {Part::encoderLsb, index};
        if (e.touch)
            target(e.touch->type, e.touch->channel, e.touch->number) = {Part::touch, index};
    }
    held.assign(def.grid.pads.size(), false);
    msb.assign(def.encoders.size(), -1);
    shown = LedFrame::blank(caps);
}

GenericMidiDriver::~GenericMidiDriver()
{
    cancelPendingUpdate();
}

// --- Input ------------------------------------------------------------------------------------------

void GenericMidiDriver::input(SurfaceInput in, double seconds)
{
    if (pads != nullptr)
        pads->handle(in, seconds);
    {
        const std::scoped_lock lock(pendingLock);
        pending.push_back(in);
    }
    triggerAsyncUpdate();
}

void GenericMidiDriver::deliverPending()
{
    std::vector<SurfaceInput> now;
    {
        const std::scoped_lock lock(pendingLock);
        now.swap(pending);
    }
    for (const auto& in : now)
        if (onInput)
            onInput(in);
}

bool GenericMidiDriver::encoderValue(int index, int value)
{
    const auto& e = def.encoders[static_cast<std::size_t>(index)];
    if (isRelative(e.encoding))
    {
        if (const int steps = relativeSteps(e.encoding, value); steps != 0)
            input(SurfaceInput::encoderTurn(index, steps), -1.0);
        return true;
    }
    if (e.encoding == Encoding::absolute)
    {
        input(SurfaceInput::encoderSet(index, static_cast<float>(value) / 127.0f), -1.0);
        return true;
    }
    // cc14: the high part waits for its low part.
    msb[static_cast<std::size_t>(index)] = value;
    return true;
}

bool GenericMidiDriver::receive(const juce::MidiMessage& m, double seconds)
{
    const int channel = m.getChannel() - 1;
    if (channel < 0 || channel > 15)
        return false; // sysex, clock: not a control's
    if (m.isNoteOnOrOff())
    {
        const auto t = target(MidiAddress::Type::note, channel, m.getNoteNumber());
        const bool down = m.isNoteOn(); // a note-on at velocity 0 is a release
        switch (t.part)
        {
        case Part::pad:
        {
            const int r = t.index / caps.columns, c = t.index % caps.columns;
            held[t.index] = down;
            input(down ? SurfaceInput::padDown(r, c, def.grid.velocity ? m.getFloatVelocity() : 1.0f) : SurfaceInput::padUp(r, c), seconds);
            return true;
        }
        case Part::button:
            input(down ? SurfaceInput::buttonDown(def.buttons[t.index].id) : SurfaceInput::buttonUp(def.buttons[t.index].id), seconds);
            return true;
        case Part::touch:
            input(down ? SurfaceInput::encoderTouch(t.index) : SurfaceInput::encoderRelease(t.index), seconds);
            return true;
        default: return false;
        }
    }
    if (m.isAftertouch())
    {
        const auto t = target(MidiAddress::Type::note, channel, m.getNoteNumber());
        if (t.part != Part::pad)
            return false;
        input(SurfaceInput::padPressure(t.index / caps.columns, t.index % caps.columns, static_cast<float>(m.getAfterTouchValue()) / 127.0f), seconds);
        return true;
    }
    if (m.isChannelPressure())
    {
        if (def.grid.aftertouch != Aftertouch::channel)
            return false;
        // The whole channel's: each pad held now.
        const float pressure = static_cast<float>(m.getChannelPressureValue()) / 127.0f;
        for (std::size_t i = 0; i < held.size(); ++i)
            if (held[i] && def.grid.pads[i].channel == channel)
                input(SurfaceInput::padPressure(static_cast<int>(i) / caps.columns, static_cast<int>(i) % caps.columns, pressure), seconds);
        return true;
    }
    if (!m.isController())
        return false;

    const int number = m.getControllerNumber(), value = m.getControllerValue();
    // NRPN: 99 and 98 choose the parameter, 6 and 38 its value (14-bit, sent on the low part).
    auto& n = nrpn[static_cast<std::size_t>(channel)];
    const auto nrpnEncoder = [&]() {
        for (std::size_t i = 0; i < def.encoders.size(); ++i)
            if (def.encoders[i].encoding == Encoding::nrpn14 && def.encoders[i].address.channel == channel && def.encoders[i].nrpn == n.parameter)
                return static_cast<int>(i);
        return -1;
    };
    const auto t = target(MidiAddress::Type::cc, channel, number);
    if (t.part == Part::none)
    {
        if (number == 99 || number == 98 || number == 6 || number == 38)
        {
            if (number == 99)
                n.parameterMsb = value;
            else if (number == 98)
                n.parameter = n.parameterMsb * 128 + value;
            const int e = nrpnEncoder();
            if (e < 0)
                return false;
            if (number == 6)
                n.high = value;
            else if (number == 38 && n.high >= 0)
                input(SurfaceInput::encoderSet(e, static_cast<float>(n.high * 128 + value) / 16383.0f), seconds);
            return true;
        }
        return false;
    }
    switch (t.part)
    {
    case Part::pad:
    {
        const int r = t.index / caps.columns, c = t.index % caps.columns;
        held[t.index] = value > 0;
        input(value > 0 ? SurfaceInput::padDown(r, c, def.grid.velocity ? static_cast<float>(value) / 127.0f : 1.0f) : SurfaceInput::padUp(r, c), seconds);
        return true;
    }
    case Part::button:
        input(value > 0 ? SurfaceInput::buttonDown(def.buttons[t.index].id) : SurfaceInput::buttonUp(def.buttons[t.index].id), seconds);
        return true;
    case Part::encoder: return encoderValue(t.index, value);
    case Part::encoderLsb:
        if (const int high = msb[t.index]; high >= 0)
            input(SurfaceInput::encoderSet(t.index, static_cast<float>(high * 128 + value) / 16383.0f), seconds);
        return true;
    case Part::touch:
        input(value > 0 ? SurfaceInput::encoderTouch(t.index) : SurfaceInput::encoderRelease(t.index), seconds);
        return true;
    case Part::none: break;
    }
    return false;
}

juce::String GenericMidiDriver::describe(const juce::MidiMessage& m) const
{
    const int channel = m.getChannel() - 1;
    if (channel < 0 || channel > 15)
        return {};
    Target t;
    if (m.isNoteOnOrOff() || m.isAftertouch())
        t = target(MidiAddress::Type::note, channel, m.getNoteNumber());
    else if (m.isController())
    {
        t = target(MidiAddress::Type::cc, channel, m.getControllerNumber());
        if (t.part == Part::none)
            for (std::size_t i = 0; i < def.encoders.size(); ++i)
                if (def.encoders[i].encoding == Encoding::nrpn14 && def.encoders[i].address.channel == channel &&
                    juce::Array<int>{99, 98, 6, 38}.contains(m.getControllerNumber()))
                    return "encoder " + juce::String(static_cast<int>(i) + 1) + " (NRPN, if it's its parameter)";
    }
    else if (m.isChannelPressure() && def.grid.aftertouch == Aftertouch::channel)
        return "pressure on the pads held";
    switch (t.part)
    {
    case Part::pad: return "pad row " + juce::String(t.index / caps.columns + 1) + ", column " + juce::String(t.index % caps.columns + 1);
    case Part::button: return "button " + buttonName(def.buttons[t.index].id);
    case Part::encoder:
    case Part::encoderLsb: return "encoder " + juce::String(t.index + 1);
    case Part::touch: return "encoder " + juce::String(t.index + 1) + " touch";
    case Part::none: break;
    }
    return {};
}

// --- Output -----------------------------------------------------------------------------------------

const LedOutput& GenericMidiDriver::buttonLed(const SurfaceDefinition::Button& b) const noexcept
{
    if (b.led.kind != LedOutput::Kind::none)
        return b.led;
    return b.ledKind == LedKind::single ? def.singleLed : def.rgbLed;
}

void GenericMidiDriver::sendLight(const MidiAddress& at, const LedOutput& led, LedState state)
{
    const int own = led.channel >= 0 ? led.channel : at.channel;
    const auto value = [&](int v, int channel) {
        const auto ch = juce::jlimit(1, 16, channel + 1);
        return at.type == MidiAddress::Type::note ? juce::MidiMessage::noteOn(ch, at.number, static_cast<juce::uint8>(v))
                                                  : juce::MidiMessage::controllerEvent(ch, at.number, v);
    };
    using Animation = LedState::Animation;
    switch (led.kind)
    {
    case LedOutput::Kind::none: return;
    case LedOutput::Kind::single:
        out.send(value(state.isOff()                                                     ? led.offValue
                       : state.animation != Animation::solid && led.blinkValue >= 0 ? led.blinkValue
                                                                                      : led.onValue,
                       own));
        return;
    case LedOutput::Kind::palette:
    {
        int channel = own;
        if (!state.isOff() && state.animation == Animation::blink && led.blinkChannel >= 0)
            channel = led.blinkChannel;
        else if (!state.isOff() && state.animation == Animation::pulse && led.pulseChannel >= 0)
            channel = led.pulseChannel;
        out.send(value(led.nearest(state.rgb), channel));
        return;
    }
    case LedOutput::Kind::sysex:
    {
        const auto b = led.sysex.fill({.id = at.number, .rgb = state.rgb, .line = 0, .text = {}, .width = 0});
        if (b.size() >= 2 && b.front() == 0xf0 && b.back() == 0xf7)
            out.send(juce::MidiMessage::createSysExMessage(b.data() + 1, static_cast<int>(b.size()) - 2));
        return;
    }
    }
}

void GenericMidiDriver::showLeds(const LedFrame& frame, const LedFrame::Changes& changed)
{
    for (const int i : changed.pads)
        if (i >= 0 && i < static_cast<int>(def.grid.pads.size()) && i < static_cast<int>(frame.pads.size()))
            sendLight(def.grid.pads[static_cast<std::size_t>(i)], def.grid.led, frame.pads[static_cast<std::size_t>(i)]);
    for (const int i : changed.buttons)
        if (i >= 0 && i < static_cast<int>(def.buttons.size()) && i < static_cast<int>(frame.buttons.size()))
        {
            const auto& b = def.buttons[static_cast<std::size_t>(i)];
            sendLight(b.address, buttonLed(b), frame.buttons[static_cast<std::size_t>(i)]);
        }
    shown = frame;
    shownAny = true;
}

void GenericMidiDriver::showDisplay(const DisplayFrame& frame)
{
    if (def.display.lines <= 0)
        return;
    const auto now = displayText(frame, def.display.lines, def.display.characters);
    lines.resize(now.size());
    for (std::size_t i = 0; i < now.size(); ++i)
    {
        if (now[i] == lines[i])
            continue;
        lines[i] = now[i];
        const auto b = def.display.line.fill({0, 0, static_cast<int>(i), now[i], def.display.characters});
        if (b.size() >= 2 && b.front() == 0xf0 && b.back() == 0xf7)
            out.send(juce::MidiMessage::createSysExMessage(b.data() + 1, static_cast<int>(b.size()) - 2));
    }
}

void GenericMidiDriver::connect()
{
    for (const auto& m : def.init)
        out.send(m);
    // Every light as last shown (all off before anything was).
    LedFrame::Changes all;
    for (int i = 0; i < static_cast<int>(shown.pads.size()); ++i)
        all.pads.push_back(i);
    for (int i = 0; i < static_cast<int>(shown.buttons.size()); ++i)
        all.buttons.push_back(i);
    showLeds(shown, all);
    lines.clear();
}

void GenericMidiDriver::disconnect()
{
    const auto blank = LedFrame::blank(caps);
    LedFrame::Changes all;
    for (int i = 0; i < static_cast<int>(blank.pads.size()); ++i)
        all.pads.push_back(i);
    for (int i = 0; i < static_cast<int>(blank.buttons.size()); ++i)
        all.buttons.push_back(i);
    const auto before = shown;
    showLeds(blank, all);
    shown = before; // shown again on the next connect()
    for (const auto& m : def.exit)
        out.send(m);
}

} // namespace lattice::control::generic
