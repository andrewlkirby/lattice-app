#include "devices/racks/Rack.h"

#include <algorithm>
#include <cmath>

namespace lattice
{
namespace
{
std::vector<ParameterSpec> makeSpecs()
{
    std::vector<ParameterSpec> s;
    for (int m = 1; m <= RackDevice::maxMacros; ++m)
        s.push_back(ParameterSpec::continuous("macro" + juce::String(m), "Macro " + juce::String(m), 0.0f, 127.0f, 0.0f)
                        .withFormatter([](float v) { return juce::String(juce::roundToInt(v)); })
                        .inGroup("Macros"));
    return s;
}

const char* nameFor(RackKind kind)
{
    switch (kind)
    {
    case RackKind::Instrument: return "Instrument Rack";
    case RackKind::AudioEffect: return "Audio Effect Rack";
    case RackKind::Drum: return "Drum Rack";
    case RackKind::MidiEffect: return "MIDI Effect Rack";
    }
    return "Rack";
}

const char* idFor(RackKind kind)
{
    switch (kind)
    {
    case RackKind::AudioEffect: return RackDevice::effectRackId;
    case RackKind::Drum: return RackDevice::drumRackId;
    case RackKind::Instrument:
    case RackKind::MidiEffect: break;
    }
    return RackDevice::instrumentRackId;
}
} // namespace

RackDevice::RackDevice(RackKind kind) : StudioDevice(idFor(kind), nameFor(kind), DeviceKind::Rack, makeSpecs()), kindOfRack(kind)
{
}

RackKind RackDevice::kindForType(const juce::String& typeId) noexcept
{
    if (typeId == effectRackId)
        return RackKind::AudioEffect;
    if (typeId == drumRackId)
        return RackKind::Drum;
    return RackKind::Instrument;
}

bool RackDevice::isRackType(const juce::String& typeId) noexcept
{
    return typeId == instrumentRackId || typeId == effectRackId || typeId == drumRackId;
}

std::vector<ParameterGroup> RackDevice::customPages(int encoderCount) const
{
    // Only the macros in use, paged by the surface's encoder count.
    encoderCount = std::max(1, encoderCount);
    std::vector<ParameterGroup> pages;
    const int count = macroCount();
    for (int first = 0; first < count; first += encoderCount)
    {
        ParameterGroup page;
        const int last = std::min(count, first + encoderCount);
        page.name = count <= encoderCount ? juce::String("Macros")
                                          : "Macros " + juce::String(first + 1) + "-" + juce::String(last);
        for (int m = first; m < last; ++m)
            page.parameters.push_back(m);
        pages.push_back(std::move(page));
    }
    if (pages.empty())
        pages.push_back({"Macros", {}}); // a rack with no macros still has its (empty) page
    return pages;
}

void RackDevice::stateChanged(const juce::ValueTree& state)
{
    macros.store(juce::jlimit(0, maxMacros, static_cast<int>(state.getProperty("macroCount", defaultMacros))));
}

void RackDevice::prepare(double sr, int)
{
    fadeStep = static_cast<float>(1.0 / (bypassFadeSeconds * sr));
    onMix = isEnabled() ? 1.0f : 0.0f;
    layout = nullptr; // the track hands the layout over again when it starts
}

void RackDevice::process(DeviceBuffers&, const DeviceTimeInfo&) noexcept
{
    // Not used: processInChain() does the rack's work.
}

void RackDevice::activate(const RackLayout* next) noexcept
{
    // Parameters a macro stops controlling go back to their own values.
    if (layout != nullptr)
        for (const auto& m : layout->mappings)
        {
            bool stillMapped = false;
            if (next != nullptr)
                for (const auto& n : next->mappings)
                    stillMapped = stillMapped || n.target == m.target;
            if (!stillMapped)
                m.target->clearOverride();
        }
    layout = next;
}

void RackDevice::releaseAllNotes() noexcept
{
    if (layout == nullptr)
        return;
    for (const auto& c : layout->chains)
    {
        for (const auto& d : c.chain.devices)
            d->releaseAllNotes();
        for (auto& channel : c.state->held)
            channel.reset();
    }
}

void RackDevice::choke() noexcept
{
    if (layout == nullptr)
        return;
    for (const auto& c : layout->chains)
        for (const auto& d : c.chain.devices)
            d->choke();
}

void RackDevice::routeNote(const RackLayout& l, const TimedMidi& e, int offset) noexcept
{
    const int channel = e.bytes[0] & 0x0f;
    const int note = e.bytes[1] & 127;
    const auto send = [&](const RackChainLayout& c, std::uint8_t status, int outNote, int velocity) {
        auto& st = *c.state;
        MidiBlock block{st.midiStorage.data(), static_cast<int>(st.midiStorage.size()), st.midiCount, 0};
        block.add(offset, status, static_cast<std::uint8_t>(juce::jlimit(0, 127, outNote)),
                  static_cast<std::uint8_t>(juce::jlimit(0, 127, velocity)));
        st.midiCount = block.count;
    };

    if (isNoteOn(e))
    {
        if (l.kind == RackKind::Drum)
        {
            for (const auto& c : l.chains)
            {
                if (c.receiveNote != note)
                    continue;
                if (c.chokeGroup > 0) // the other pads in the group stop
                    for (const auto& other : l.chains)
                        if (&other != &c && other.chokeGroup == c.chokeGroup)
                            for (const auto& d : other.chain.devices)
                                d->choke();
                const int playNote = l.tunedPads ? note : c.playNote;
                c.state->held[static_cast<std::size_t>(channel)].set(static_cast<std::size_t>(note));
                send(c, e.bytes[0], playNote, e.bytes[2]);
            }
            return;
        }
        for (const auto& c : l.chains)
        {
            const float g = l.kind == RackKind::Instrument ? c.zoneGain(note, e.bytes[2]) : 1.0f;
            if (g <= 0.0f)
                continue;
            c.state->held[static_cast<std::size_t>(channel)].set(static_cast<std::size_t>(note));
            send(c, e.bytes[0], note, std::max(1, juce::roundToInt(e.bytes[2] * g)));
        }
        return;
    }

    // A note-off goes wherever its note-on went.
    for (const auto& c : l.chains)
    {
        auto& held = c.state->held[static_cast<std::size_t>(channel)];
        if (!held.test(static_cast<std::size_t>(note)))
            continue;
        held.reset(static_cast<std::size_t>(note));
        const int outNote = l.kind == RackKind::Drum && !l.tunedPads ? c.playNote : note;
        send(c, static_cast<std::uint8_t>(0x80 | channel), outNote, 0);
    }
}

void RackDevice::renderPiece(const RackLayout& l, DeviceBuffers& io, const DeviceTimeInfo& time, int start,
                             int count) noexcept
{
    const bool effect = l.kind == RackKind::AudioEffect;
    // Effect racks: every chain starts from the rack's input.
    for (const auto& c : l.chains)
    {
        auto& st = *c.state;
        for (int ch = 0; ch < 2; ++ch)
        {
            float* buf = st.audio.getWritePointer(ch);
            if (effect)
                std::copy_n(io.channels[std::min(ch, io.numChannels - 1)] + start, count, buf);
            else
                std::fill_n(buf, count, 0.0f);
        }
    }
    for (int ch = 0; ch < io.numChannels; ++ch)
        std::fill_n(io.channels[ch] + start, count, 0.0f);

    bool anySolo = false;
    for (const auto& c : l.chains)
        anySolo = anySolo || c.state->soloed.load(std::memory_order_relaxed);

    DeviceTimeInfo pieceTime = time;
    pieceTime.beatAtStart = time.beatAt(start);
    pieceTime.blockStartSample = time.blockStartSample + start;
    pieceTime.sidechainOffset = time.sidechainOffset + start;
    const float rampSamples = static_cast<float>(0.02 * sampleRate());

    for (const auto& c : l.chains)
    {
        auto& st = *c.state;
        float* channels[2] = {st.audio.getWritePointer(0), st.audio.getWritePointer(1)};
        MidiBlock midi{st.midiStorage.data(), static_cast<int>(st.midiStorage.size()), st.midiCount, 0};
        st.runner.process(c.chain, channels, count, midi, pieceTime);
        st.midiCount = 0;
        if (c.compensation != nullptr)
            c.compensation->process(channels, count);

        const bool audible = !st.muted.load(std::memory_order_relaxed) &&
                             (!anySolo || st.soloed.load(std::memory_order_relaxed));
        const float targetL = audible ? st.volumeLeft.load(std::memory_order_relaxed) : 0.0f;
        const float targetR = audible ? st.volumeRight.load(std::memory_order_relaxed) : 0.0f;
        if (!st.started)
        {
            st.gainLeft = st.rampTargetLeft = targetL;
            st.gainRight = st.rampTargetRight = targetR;
            st.started = true;
        }
        if (targetL != st.rampTargetLeft || targetR != st.rampTargetRight)
        {
            // A new mix: ramp to it in 20 ms, whatever the distance.
            st.rampTargetLeft = targetL;
            st.rampTargetRight = targetR;
            st.rampRemaining = static_cast<int>(rampSamples);
            st.stepLeft = (targetL - st.gainLeft) / rampSamples;
            st.stepRight = (targetR - st.gainRight) / rampSamples;
        }
        float* outL = io.channels[0] + start;
        float* outR = io.channels[io.numChannels > 1 ? 1 : 0] + start;
        for (int i = 0; i < count; ++i)
        {
            if (st.rampRemaining > 0)
            {
                st.gainLeft += st.stepLeft;
                st.gainRight += st.stepRight;
                if (--st.rampRemaining == 0)
                {
                    st.gainLeft = st.rampTargetLeft;
                    st.gainRight = st.rampTargetRight;
                }
            }
            outL[i] += channels[0][i] * st.gainLeft * 1.41421356f;
            outR[i] += channels[1][i] * st.gainRight * 1.41421356f;
        }
    }
}

void RackDevice::processInChain(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept
{
    const int n = io.numSamples;
    const auto* l = layout;
    const bool effect = kindOfRack == RackKind::AudioEffect;
    if (l == nullptr || l->chains.empty())
    {
        if (!effect) // an empty instrument rack is silent; an empty effect rack passes audio through
            for (int ch = 0; ch < io.numChannels; ++ch)
                std::fill_n(io.channels[ch], n, 0.0f);
        return;
    }

    // The on/off switch: off, an instrument rack is silent and an effect rack passes audio.
    const float target = isEnabled() ? 1.0f : 0.0f;
    if (onMix == 0.0f && target == 0.0f)
    {
        if (!effect)
            for (int ch = 0; ch < io.numChannels; ++ch)
                std::fill_n(io.channels[ch], n, 0.0f);
        return;
    }
    if (onMix == 0.0f)
        releaseAllNotes();

    // Macros drive their parameters (once per block: devices read parameters per block anyway).
    for (const auto& m : l->mappings)
        m.target->setOverride(m.valueFor(value(m.macro)));

    const int capacity = l->chains.front().state->capacity();
    int nextEvent = 0;
    for (int start = 0; start < n; start += capacity)
    {
        const int count = std::min(capacity, n - start);
        // This piece's MIDI, to the chains it is for.
        while (nextEvent < io.midiIn->count && io.midiIn->events[nextEvent].sampleOffset < start + count)
        {
            const auto& e = io.midiIn->events[nextEvent++];
            const int offset = std::max(0, e.sampleOffset - start);
            if (isNoteOn(e) || isNoteOff(e))
            {
                routeNote(*l, e, offset);
            }
            else
            {
                for (const auto& c : l->chains) // controllers, bend, pressure: every chain
                {
                    auto& st = *c.state;
                    MidiBlock block{st.midiStorage.data(), static_cast<int>(st.midiStorage.size()), st.midiCount, 0};
                    TimedMidi copy = e;
                    copy.sampleOffset = offset;
                    block.add(copy);
                    st.midiCount = block.count;
                }
            }
        }

        if (onMix == target)
        {
            renderPiece(*l, io, time, start, count);
            continue;
        }
        // Switching on or off: crossfade the rack's output against its input (silence for
        // instrument racks), keeping a copy of the input first.
        std::array<std::array<float, 64>, 2> dryScratch{};
        for (int s = 0; s < count; s += 64)
        {
            const int m = std::min(64, count - s);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < m; ++i)
                    dryScratch[static_cast<std::size_t>(ch)][static_cast<std::size_t>(i)] =
                        effect ? io.channels[std::min(ch, io.numChannels - 1)][start + s + i] : 0.0f;
            renderPiece(*l, io, time, start + s, m);
            for (int i = 0; i < m; ++i)
            {
                onMix = std::clamp(onMix + (target > onMix ? fadeStep : -fadeStep), 0.0f, 1.0f);
                for (int ch = 0; ch < io.numChannels; ++ch)
                {
                    float& y = io.channels[ch][start + s + i];
                    const float d = dryScratch[static_cast<std::size_t>(std::min(ch, 1))][static_cast<std::size_t>(i)];
                    y = d + onMix * (y - d);
                }
            }
        }
    }
}

} // namespace lattice
