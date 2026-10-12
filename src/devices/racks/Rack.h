#pragma once

#include "devices/framework/StudioDevice.h"
#include "devices/racks/RackLayout.h"

#include <atomic>

namespace lattice
{

/** Instrument Rack, Audio Effect Rack and Drum Rack (docs/04-m3-devices.md, "3d").

    A rack holds parallel chains, each a full device chain with its own volume, pan, mute and solo;
    their outputs are mixed. Its structure is a RackLayout, part of the track's chain layout:
    activate() hands it the current one on the audio thread when the track switches layouts.

    - Instrument racks send each note to the chains whose key and velocity zones contain it
      (velocity scaled down across a zone's fades). Effect racks feed their input to every chain.
    - Drum racks send each pad's note to its chain (as the chain's play note, or as the pad's own
      note with "tuned pads"); a pad in a choke group cuts the others in the group.
    - Macros (0-64, set per rack; parameters "macro1".."macro64", 0..127) drive mapped parameters
      inside the rack through their overrides. Macros are the rack's first parameter group, and
      controllers page only the macros in use.
    - Chains with less latency than the slowest are delayed to line up with it.

    The rack works through its chains in pieces no longer than its buffers (sized for the engine's
    block size), so it never allocates, whatever block the driver sends. */
class RackDevice final : public StudioDevice
{
public:
    static constexpr const char* instrumentRackId = "lattice.instrumentRack";
    static constexpr const char* effectRackId = "lattice.effectRack";
    static constexpr const char* drumRackId = "lattice.drumRack";
    static constexpr int maxMacros = 64;
    static constexpr int defaultMacros = 8;
    static constexpr int maxDepth = 4;

    explicit RackDevice(RackKind kind);

    RackKind rackKind() const noexcept { return kindOfRack; }
    static RackKind kindForType(const juce::String& typeId) noexcept;
    static bool isRackType(const juce::String& typeId) noexcept;

    /** Message thread: the number of macros in use (from the STATE). */
    int macroCount() const noexcept { return macros.load(std::memory_order_relaxed); }
    std::vector<ParameterGroup> customPages(int encoderCount) const override;

    /** Audio thread, when the track switches to a new layout: this rack's part of it. */
    void activate(const RackLayout* layout) noexcept LATTICE_NONBLOCKING;

    void processInChain(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept override;
    void releaseAllNotes() noexcept override;
    void choke() noexcept override;
    DeviceKind switchKind() const noexcept override
    {
        return kindOfRack == RackKind::AudioEffect ? DeviceKind::AudioEffect : DeviceKind::Instrument;
    }

protected:
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept override;
    void stateChanged(const juce::ValueTree& state) override;

private:
    void routeNote(const RackLayout& layout, const TimedMidi& e, int offset) noexcept LATTICE_NONBLOCKING;
    void renderPiece(const RackLayout& layout, DeviceBuffers& io, const DeviceTimeInfo& time, int start,
                     int count) noexcept LATTICE_NONBLOCKING;

    const RackKind kindOfRack;
    const RackLayout* layout = nullptr; // audio thread
    std::atomic<int> macros{defaultMacros};
    float onMix = 1.0f;
    float fadeStep = 0.001f;
};

} // namespace lattice
