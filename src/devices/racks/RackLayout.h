#pragma once

#include "devices/dsp/DelayLine.h"
#include "devices/framework/DeviceChain.h"

#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <memory>
#include <vector>

namespace lattice
{

/** A rack chain's working state that lasts from one layout to the next (kept per chain ID, so a
    chain's sound and smoothing continue when the rack is edited). Allocated on the message thread
    for the engine's block size; the rack processes longer blocks in pieces. */
struct RackChainState
{
    static constexpr int maxEvents = 2048;

    void prepare(int maxBlockSize);
    int capacity() const noexcept { return blockSize; }

    juce::AudioBuffer<float> audio;
    std::vector<TimedMidi> midiStorage;
    ChainRunner runner;
    float gainLeft = 0.0f; // ramping towards the chain's volume and pan
    float gainRight = 0.0f;
    float rampTargetLeft = 0.0f;
    float rampTargetRight = 0.0f;
    float stepLeft = 0.0f;
    float stepRight = 0.0f;
    int rampRemaining = 0;
    bool started = false;
    std::array<std::bitset<128>, 16> held{}; // notes this chain was sent, so their offs follow them

    // The chain's mix, live: set from the document (message thread) without rebuilding anything.
    std::atomic<float> volumeLeft{0.70710678f}; // volume x constant-power pan
    std::atomic<float> volumeRight{0.70710678f};
    std::atomic<bool> muted{false};
    std::atomic<bool> soloed{false};

    /** Message thread: the chain's volume (dB), pan (-1..1), mute and solo. */
    void setMix(double volumeDb, double pan, bool mute, bool solo) noexcept;
    int midiCount = 0;                       // events queued for the piece being rendered
    int blockSize = 0;
};

/** Delays a chain with less latency than the rack's slowest chain, so the chains line up when they
    are mixed (docs/04-m3-devices.md, "Delay compensation inside a rack"). Sized when the layout is
    built (message thread). */
struct CompensationDelay
{
    explicit CompensationDelay(int samples);
    void process(float* const* channels, int numSamples) noexcept LATTICE_NONBLOCKING;

    int samples = 0;
    dsp::DelayLine lines[2];
};

/** One chain of a rack in a layout: its devices, mix settings and zones. Immutable once published
    (except the state, which only the audio thread touches). */
struct RackChainLayout
{
    std::int64_t id = 0;
    DeviceChainLayout chain;

    // Instrument racks: which notes reach the chain. Fades scale the velocity of notes at the
    // edges of a zone (0 at the zone's edge, full at the fade's end).
    int keyLow = 0, keyHigh = 127, keyFadeLow = 0, keyFadeHigh = 0;
    int velocityLow = 1, velocityHigh = 127, velocityFadeLow = 0, velocityFadeHigh = 0;

    // Drum racks: the pad's note, the note the chain plays, and its choke group (0 = none).
    int receiveNote = -1;
    int playNote = 60;
    int chokeGroup = 0;

    std::shared_ptr<RackChainState> state;
    std::shared_ptr<CompensationDelay> compensation; // null when the chain is the slowest

    /** 0..1: how much of a note reaches this chain (0 = not at all). */
    float zoneGain(int note, int velocity) const noexcept;
};

/** A macro's mapping to one parameter inside the rack: the macro's 0..127 range maps onto
    [minimum, maximum] of the target's normalised range, through a curve. */
struct MacroMapping
{
    int macro = 0;
    DeviceParameter* target = nullptr;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float curve = 0.0f; // -1 (fast start) .. 0 (linear) .. 1 (slow start)

    /** The target's plain value for a macro value (0..127). */
    float valueFor(float macroValue) const noexcept;
};

enum class RackKind : std::uint8_t
{
    Instrument,
    AudioEffect,
    Drum,
    MidiEffect
};

/** One version of a rack's structure (docs/04-m3-devices.md, "Rack"): its chains, its macro
    mappings and its latency. Part of the track's chain layout, so it switches (with a fade)
    together with everything else in the track's device tree. */
struct RackLayout
{
    RackKind kind = RackKind::Instrument;
    std::vector<RackChainLayout> chains;
    std::vector<MacroMapping> mappings;
    bool tunedPads = false;
    int latencySamples = 0; // the slowest chain's

    /** Message thread, once the chains' devices are prepared: their latencies, and the delays that
        line the faster chains up with the slowest. */
    void finalise();
};

/** Calls fn(rackDevice, rackLayout) for every rack in a chain's tree, outermost first. */
template <typename F>
void forEachRack(const DeviceChainLayout& chain, F&& fn)
{
    for (std::size_t i = 0; i < chain.devices.size() && i < chain.racks.size(); ++i)
        if (chain.racks[i] != nullptr)
        {
            fn(*chain.devices[i], *chain.racks[i]);
            for (const auto& c : chain.racks[i]->chains)
                forEachRack(c.chain, fn);
        }
}

template <typename F>
void DeviceChainLayout::forEachDevice(F&& fn) const
{
    for (std::size_t i = 0; i < devices.size(); ++i)
    {
        fn(*devices[i]);
        if (i < racks.size() && racks[i] != nullptr)
            for (const auto& chain : racks[i]->chains)
                chain.chain.forEachDevice(fn);
    }
}

} // namespace lattice
