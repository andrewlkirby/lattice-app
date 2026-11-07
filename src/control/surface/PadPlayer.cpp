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
constexpr std::uint8_t modWheel = 1, timbreCc = 74;
constexpr int bendCentre = 8192;

/** A pitch bend (-1..1) as its 14 bits. */
int bendValue(double bend) noexcept
{
    return std::clamp(bendCentre + static_cast<int>(std::lround(std::clamp(bend, -1.0, 1.0) * 8191.0)), 0, 16383);
}

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
    return kind == o.kind && note == o.note && hasUp == o.hasUp && slides == o.slides && sameCommand(onDown, o.onDown) && sameCommand(onUp, o.onUp);
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
    perNote = e.perNote && caps.aftertouch == Aftertouch::mpe;
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
    case Kind::padSlide:
        if (onGrid)
            sent = padSlide(*map, index, input.value, seconds);
        break;
    case Kind::padTimbre:
        if (onGrid)
            sent = padTimbre(index, input.value, seconds);
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
                     std::uint32_t target, double seconds, std::uint8_t flags) noexcept
{
    const std::uint8_t bytes[3]{status, data1, data2};
    return engine.midiInput().push(bytes, size, source, seconds, target, flags);
}

bool PadPlayer::send(const Held& h, std::uint8_t status, std::uint8_t data1, std::uint8_t data2, int size, double seconds) noexcept
{
    return send(static_cast<std::uint8_t>(status | h.channel), data1, data2, size, h.source, h.target, seconds,
                h.perNote ? TimedMidi::perNote : std::uint8_t{0});
}

std::uint8_t PadPlayer::channelForNote(const PadNoteMap& map) noexcept
{
    int best = -1;
    for (int c = 0; c < 16; ++c)
    {
        if (c == map.channel)
            continue;
        if (best < 0)
        {
            best = c;
            continue;
        }
        const auto i = static_cast<std::size_t>(c), b = static_cast<std::size_t>(best);
        // A free channel before a busy one; then the one used longest ago (a free channel's
        // release has died away the longest; a busy one's note is the oldest).
        const bool free = notesOnChannel[i] == 0, bestFree = notesOnChannel[b] == 0;
        if (free != bestFree ? free : channelUsed[i] < channelUsed[b])
            best = c;
    }
    return static_cast<std::uint8_t>(best);
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
        const auto v = std::max<std::uint8_t>(1, map.velocity[static_cast<std::size_t>(rawIndex(velocity))]);
        h = {};
        h.kind = PadNoteMap::Pad::Kind::note;
        h.note = note;
        h.source = map.source;
        h.target = map.target;
        h.master = map.channel;
        h.channelWide = map.pressureMode == Expression::Pressure::channel;
        if (map.perNote)
        {
            // A channel of its own, its slide, timbre and pressure at rest before the note starts.
            h.perNote = true;
            h.channel = channelForNote(map);
            const auto c = static_cast<std::size_t>(h.channel);
            ++notesOnChannel[c];
            channelUsed[c] = ++uses;
            notesHeld.fetch_add(1, std::memory_order_relaxed);
            send(h, pitchBend, bendCentre & 0x7f, bendCentre >> 7, 3, seconds);
            send(h, controller, timbreCc, 64, 3, seconds);
            if (!h.channelWide)
                send(h, channelPressureStatus, 0, 0, 2, seconds);
            send(h, noteOn, note, v, 3, seconds);
            return true;
        }
        h.channel = map.channel;
        // Another pad holds it: play it again rather than stack a second voice on it.
        if (noteHolders[note] > 0)
            send(h, noteOff, note, 0, 3, seconds);
        else
            notesHeld.fetch_add(1, std::memory_order_relaxed);
        ++noteHolders[note];
        send(h, noteOn, note, v, 3, seconds);
        return true;
    }
    case PadNoteMap::Pad::Kind::command:
        h = {};
        h.kind = PadNoteMap::Pad::Kind::command;
        h.hasUp = pad.hasUp;
        h.onUp = pad.onUp;
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
        if (was.perNote)
        {
            const auto c = static_cast<std::size_t>(was.channel);
            notesOnChannel[c] = static_cast<std::uint8_t>(std::max(0, notesOnChannel[c] - 1));
            channelUsed[c] = ++uses;
            notesHeld.fetch_sub(1, std::memory_order_relaxed);
            send(was, noteOff, was.note, 0, 3, seconds);
        }
        else if (noteHolders[was.note] > 0 && --noteHolders[was.note] == 0)
        {
            notesHeld.fetch_sub(1, std::memory_order_relaxed);
            send(was, noteOff, was.note, 0, 3, seconds);
        }
        if (was.channelWide && was.lastPressure > 0)
            sendChannelPressure(was, seconds); // what the others still press
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
    if (!h.channelWide)
        return h.perNote ? send(h, channelPressureStatus, value, 0, 2, seconds) // its channel's is its own
                         : send(h, polyPressure, h.note, value, 3, seconds);

    sendChannelPressure(h, seconds);
    return true;
}

bool PadPlayer::padSlide(const PadNoteMap& map, int index, float pads, double seconds) noexcept
{
    auto& h = held[static_cast<std::size_t>(index)];
    if (h.kind != PadNoteMap::Pad::Kind::note || !h.perNote)
        return false;
    const int row = index / map.columns, from = index % map.columns;
    const auto noteAt = [&](int column) -> int {
        if (column < 0 || column >= map.columns)
            return -1;
        const auto& p = map.pads[static_cast<std::size_t>(row * map.columns + column)];
        return p.kind == PadNoteMap::Pad::Kind::note && p.slides ? p.note : -1;
    };
    const int start = noteAt(from);
    if (start < 0)
        return false; // a pad that doesn't slide (a drum's), or the layout changed under it
    // The pitch at a column: its pad's note; past the row's end (or a gap), the last step
    // carried on.
    const auto pitchAt = [&](int column) {
        const int dir = column >= from ? 1 : -1;
        double pitch = start;
        double step = dir;
        for (int c = from; c != column; c += dir)
        {
            const int here = noteAt(c), next = noteAt(c + dir);
            if (here >= 0 && next >= 0)
                step = next - here;
            pitch = next >= 0 ? next : pitch + step;
        }
        return pitch;
    };
    const double x = from + static_cast<double>(pads);
    const int left = static_cast<int>(std::floor(x));
    const double f = x - left;
    const double pitch = pitchAt(left) + f * (pitchAt(left + 1) - pitchAt(left));
    const int value = bendValue((pitch - start) / perNoteBendSteps);
    if (value == h.lastBend)
        return true;
    h.lastBend = value;
    return send(h, pitchBend, static_cast<std::uint8_t>(value & 0x7f), static_cast<std::uint8_t>(value >> 7), 3, seconds);
}

bool PadPlayer::padTimbre(int index, float timbre, double seconds) noexcept
{
    auto& h = held[static_cast<std::size_t>(index)];
    if (h.kind != PadNoteMap::Pad::Kind::note || !h.perNote)
        return false;
    const int value = rawIndex(timbre);
    if (value == h.lastTimbre)
        return true;
    h.lastTimbre = value;
    return send(h, controller, timbreCc, static_cast<std::uint8_t>(value), 3, seconds);
}

void PadPlayer::sendChannelPressure(const Held& h, double seconds) noexcept
{
    // The channel's pressure: the most any held pad is pressed.
    std::uint8_t most = 0;
    for (const auto& other : held)
        if (other.kind == PadNoteMap::Pad::Kind::note && other.channelWide)
            most = std::max(most, other.lastPressure);
    if (most == channelPressure)
        return;
    channelPressure = most;
    // On the master channel: a per-note pad's pressure for the whole channel is every note's.
    send(static_cast<std::uint8_t>(channelPressureStatus | h.master), most, 0, 2, h.source, h.target, seconds);
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
                    static_cast<std::uint8_t>(value >> 7), 3, map.source, map.target, seconds);
    }
    case Expression::Strip::modWheel:
        if (release)
            return true; // it stays where it was left
        return send(static_cast<std::uint8_t>(controller | map.channel), modWheel,
                    static_cast<std::uint8_t>(rawIndex(input.value)), 3, map.source, map.target, seconds);
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
