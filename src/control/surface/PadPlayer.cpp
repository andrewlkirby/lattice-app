#include "control/surface/PadPlayer.h"

#include "engine/AudioEngine.h"

#include <algorithm>
#include <cmath>

namespace lattice::control
{

namespace
{
constexpr std::uint8_t noteOn = 0x90, noteOff = 0x80, polyPressure = 0xa0, controller = 0xb0, channelPressureStatus = 0xd0,
                       pitchBend = 0xe0;
constexpr std::uint8_t modWheel = 1;

bool sameCommand(const EngineCommand& a, const EngineCommand& b) noexcept
{
    return a.type == b.type && a.flag == b.flag && a.scene == b.scene && a.trackId == b.trackId && a.clipId == b.clipId &&
           a.value == b.value;
}

int rawIndex(float value) noexcept
{
    return std::clamp(static_cast<int>(std::lround(value * 127.0f)), 0, 127);
}
} // namespace

// --- Expression -------------------------------------------------------------------------------------

int Expression::velocityFor(float raw) const noexcept
{
    const float x = std::clamp(raw, 0.0f, 1.0f);
    const float shaped = std::pow(x, std::pow(3.0f, std::clamp(velocityCurve, -1.0f, 1.0f)));
    const int lo = std::clamp(velocityMin, 1, 127);
    const int hi = std::clamp(velocityMax, lo, 127);
    return std::clamp(static_cast<int>(std::lround(static_cast<float>(lo) + static_cast<float>(hi - lo) * shaped)), 1, 127);
}

int Expression::pressureFor(float raw) const noexcept
{
    const float t = std::clamp(pressureThreshold, 0.0f, 0.99f);
    const float scaled = std::clamp((raw - t) / (1.0f - t), 0.0f, 1.0f);
    return static_cast<int>(std::lround(scaled * 127.0f));
}

// --- PadNoteMap -------------------------------------------------------------------------------------

bool PadNoteMap::Pad::operator==(const Pad& o) const noexcept
{
    return kind == o.kind && note == o.note && hasUp == o.hasUp && sameCommand(onDown, o.onDown) && sameCommand(onUp, o.onUp);
}

bool PadNoteMap::ButtonCommand::operator==(const ButtonCommand& o) const noexcept
{
    return id == o.id && sameCommand(command, o.command);
}

PadNoteMap PadNoteMap::empty(const SurfaceCapabilities& caps)
{
    PadNoteMap m;
    m.rows = caps.rows;
    m.columns = caps.columns;
    m.pads.resize(static_cast<std::size_t>(caps.numPads()));
    m.setExpression({}, caps);
    return m;
}

void PadNoteMap::setExpression(const Expression& e, const SurfaceCapabilities& caps)
{
    for (int i = 0; i < 128; ++i)
    {
        const float raw = static_cast<float>(i) / 127.0f;
        velocity[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(caps.velocity ? e.velocityFor(raw) : std::max(1, i));
        pressure[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(e.pressureFor(raw));
    }
    pressureMode = caps.aftertouch == Aftertouch::none ? Expression::Pressure::off : e.pressure;
    strip = e.strip;
}

// --- PadPlayer --------------------------------------------------------------------------------------

PadPlayer::PadPlayer(AudioEngine& e, const SurfaceCapabilities& caps)
    : engine(e), held(static_cast<std::size_t>(caps.numPads())), numPads(caps.numPads())
{
    maps.publish(std::make_shared<const PadNoteMap>(PadNoteMap::empty(caps)));
}

PadPlayer::~PadPlayer()
{
    releaseAll();
}

void PadPlayer::publish(std::shared_ptr<const PadNoteMap> map)
{
    maps.publish(std::move(map));
}

int PadPlayer::heldNotes() const noexcept
{
    return notesHeld.load(std::memory_order_relaxed);
}

bool PadPlayer::handle(SurfaceInput& input, double seconds)
{
    const bool sent = play(input, seconds);
    if (sent)
        engine.noteActivity();
    return sent;
}

bool PadPlayer::play(SurfaceInput& input, double seconds) noexcept
{
    if (seconds < 0.0)
        seconds = MidiInputQueue::nowSeconds();
    const RtShared<PadNoteMap>::ReadScope scope(maps);
    const auto* map = scope.get();
    if (map == nullptr)
        return false;

    using Kind = SurfaceInput::Kind;
    const int index = input.row * map->columns + input.column;
    const bool onGrid = input.row >= 0 && input.row < map->rows && input.column >= 0 && input.column < map->columns &&
                        index < numPads;
    bool sent = false;
    switch (input.kind)
    {
    case Kind::buttonDown:
    case Kind::buttonUp:
    {
        const auto bit = std::uint64_t{1} << static_cast<int>(input.button.role);
        const bool modified = (heldModifiers & map->modifiers) != 0;
        if (input.kind == Kind::buttonDown)
            heldModifiers |= bit;
        else
            heldModifiers &= ~bit;
        if (input.kind == Kind::buttonDown && !modified)
            for (const auto& b : map->buttons)
                if (b.id == input.button)
                {
                    sent = engine.queueCommand(b.command);
                    break;
                }
        break; // anything else a button does is the message thread's
    }
    case Kind::padDown:
        if (onGrid && (heldModifiers & map->modifiers) == 0)
            sent = padDown(*map, index, input.value, seconds);
        break;
    case Kind::padUp:
        if (onGrid)
            sent = padUp(index, seconds);
        break;
    case Kind::padPressure:
        if (onGrid)
            sent = padPressure(*map, index, input.value, seconds);
        break;
    case Kind::strip:
    case Kind::stripRelease:
        sent = strip(*map, input, seconds);
        break;
    default:
        break;
    }
    input.played = input.played || sent;
    return sent;
}

bool PadPlayer::send(std::uint8_t status, std::uint8_t data1, std::uint8_t data2, int size, std::uint8_t source,
                     double seconds) noexcept
{
    const std::uint8_t bytes[3]{status, data1, data2};
    return engine.midiInput().push(bytes, size, source, seconds);
}

bool PadPlayer::padDown(const PadNoteMap& map, int index, float velocity, double seconds) noexcept
{
    auto& h = held[static_cast<std::size_t>(index)];
    if (h.kind != PadNoteMap::Pad::Kind::none)
        padUp(index, seconds); // a press without its release (a lost message): end the first

    const auto& pad = map.pads[static_cast<std::size_t>(index)];
    switch (pad.kind)
    {
    case PadNoteMap::Pad::Kind::none:
        return false;
    case PadNoteMap::Pad::Kind::note:
    {
        const auto note = static_cast<std::uint8_t>(pad.note & 0x7f);
        const auto v = map.velocity[static_cast<std::size_t>(rawIndex(velocity))];
        // Another pad holds it: play it again rather than stack a second voice on it.
        if (noteHolders[note] > 0)
            send(static_cast<std::uint8_t>(noteOff | map.channel), note, 0, 3, map.source, seconds);
        else
            notesHeld.fetch_add(1, std::memory_order_relaxed);
        ++noteHolders[note];
        h = {PadNoteMap::Pad::Kind::note, note, map.channel, map.source, 0, false,
             map.pressureMode == Expression::Pressure::channel, {}};
        send(static_cast<std::uint8_t>(noteOn | map.channel), note, std::max<std::uint8_t>(1, v), 3, map.source, seconds);
        return true;
    }
    case PadNoteMap::Pad::Kind::command:
        h = {PadNoteMap::Pad::Kind::command, 0, 0, 0, 0, pad.hasUp, false, pad.onUp};
        engine.queueCommand(pad.onDown);
        return true;
    }
    return false;
}

bool PadPlayer::padUp(int index, double seconds) noexcept
{
    auto& h = held[static_cast<std::size_t>(index)];
    const auto was = h;
    h = {};
    switch (was.kind)
    {
    case PadNoteMap::Pad::Kind::none:
        return false;
    case PadNoteMap::Pad::Kind::note:
        if (noteHolders[was.note] > 0 && --noteHolders[was.note] == 0)
        {
            notesHeld.fetch_sub(1, std::memory_order_relaxed);
            send(static_cast<std::uint8_t>(noteOff | was.channel), was.note, 0, 3, was.source, seconds);
        }
        if (was.channelWide && was.lastPressure > 0)
            sendChannelPressure(was.channel, was.source, seconds); // what the others still press
        return true;
    case PadNoteMap::Pad::Kind::command:
        if (was.hasUp)
            engine.queueCommand(was.onUp);
        return true;
    }
    return false;
}

bool PadPlayer::padPressure(const PadNoteMap& map, int index, float pressure, double seconds) noexcept
{
    auto& h = held[static_cast<std::size_t>(index)];
    if (h.kind != PadNoteMap::Pad::Kind::note || map.pressureMode == Expression::Pressure::off)
        return false;
    const auto value = map.pressure[static_cast<std::size_t>(rawIndex(pressure))];
    if (value == h.lastPressure)
        return true; // nothing new to send
    h.lastPressure = value;
    if (map.pressureMode == Expression::Pressure::polyphonic)
        return send(static_cast<std::uint8_t>(polyPressure | h.channel), h.note, value, 3, h.source, seconds);

    sendChannelPressure(h.channel, h.source, seconds);
    return true;
}

void PadPlayer::sendChannelPressure(std::uint8_t channel, std::uint8_t source, double seconds) noexcept
{
    // The channel's pressure: the most any held pad is pressed.
    std::uint8_t most = 0;
    for (const auto& other : held)
        if (other.kind == PadNoteMap::Pad::Kind::note && other.channelWide)
            most = std::max(most, other.lastPressure);
    if (most == channelPressure)
        return;
    channelPressure = most;
    send(static_cast<std::uint8_t>(channelPressureStatus | channel), most, 0, 2, source, seconds);
}

bool PadPlayer::strip(const PadNoteMap& map, const SurfaceInput& input, double seconds) noexcept
{
    const bool release = input.kind == SurfaceInput::Kind::stripRelease;
    switch (map.strip)
    {
    case Expression::Strip::pitchBend:
    {
        const int value = release ? 8192 : std::clamp(static_cast<int>(std::lround(input.value * 16383.0f)), 0, 16383);
        return send(static_cast<std::uint8_t>(pitchBend | map.channel), static_cast<std::uint8_t>(value & 0x7f),
                    static_cast<std::uint8_t>(value >> 7), 3, map.source, seconds);
    }
    case Expression::Strip::modWheel:
        if (release)
            return true; // it stays where it was left
        return send(static_cast<std::uint8_t>(controller | map.channel), modWheel,
                    static_cast<std::uint8_t>(rawIndex(input.value)), 3, map.source, seconds);
    case Expression::Strip::scroll:
        return false; // the mode's, on the message thread
    }
    return false;
}

void PadPlayer::releaseAll() noexcept
{
    const double now = MidiInputQueue::nowSeconds();
    for (int i = 0; i < numPads; ++i)
        padUp(i, now);
    heldModifiers = 0;
}

} // namespace lattice::control
