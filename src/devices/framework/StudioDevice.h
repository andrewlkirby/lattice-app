#pragma once

#include "core/RealtimeCheck.h"
#include "devices/framework/DeviceParameter.h"
#include "devices/framework/DeviceTypes.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_data_structures/juce_data_structures.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace lattice
{

/** A named, ordered group of parameters: a section of the generic editor and the unit the
    ParameterPager packs into controller pages. */
struct ParameterGroup
{
    juce::String name;
    std::vector<int> parameters; // indices into the device's parameters, most important first
};

/** Base of every built-in device (docs/04-m3-devices.md, "3a"): MIDI effects, instruments, audio
    effects and racks.

    - Parameters come from the ParameterSpec list given to the constructor. Their values live in the
      document; the engine copies changes into the DeviceParameters, which the audio thread reads.
    - Groups are formed from the specs' group names in order of first appearance.
    - processInChain() adds what every device in a chain needs around process(): the on/off switch
      with a 5 ms crossfade (and, for MIDI effects, note-offs for whatever was playing), a dry path
      delayed by the device's latency so that switching never shifts the signal, and a reset when a
      device that was fully off comes back.

    Threading: construction, prepareDevice(), setState() and setLatency() are message-thread only.
    processInChain() and releaseAllNotes() are audio-thread only and never allocate or lock. */
class StudioDevice : public juce::AudioProcessor
{
public:
    static constexpr double bypassFadeSeconds = 0.005;

    StudioDevice(juce::String typeId, juce::String displayName, DeviceKind kind,
                 std::vector<ParameterSpec> parameterSpecs);
    ~StudioDevice() override;

    const juce::String& typeId() const noexcept { return deviceType; }
    DeviceKind kind() const noexcept { return deviceKind; }

    // --- Parameters -------------------------------------------------------------------------------
    int numParameters() const noexcept { return static_cast<int>(params.size()); }
    DeviceParameter& parameter(int index) const noexcept { return *params[static_cast<std::size_t>(index)]; }
    /** -1 if the device has no parameter with that ID. */
    int parameterIndex(const juce::String& parameterId) const noexcept;
    DeviceParameter* findParameter(const juce::String& parameterId) const noexcept;
    const std::vector<ParameterGroup>& parameterGroups() const noexcept { return groups; }

    /** A device may arrange its own controller pages for a given encoder count. Empty means "use
        the automatic layout" (ParameterPager). */
    virtual std::vector<ParameterGroup> customPages(int encoderCount) const;

    // --- Message thread ---------------------------------------------------------------------------
    /** Allocates everything processing needs. May be called again (new sample rate or block size). */
    void prepareDevice(double sampleRate, int maxBlockSize);
    bool isPrepared() const noexcept { return preparedBlock > 0; }
    double preparedSampleRate() const noexcept { return currentSampleRate; }
    int preparedBlockSize() const noexcept { return preparedBlock; }

    /** The device's own, non-parameter state (a sample file, pads...) from the document's STATE
        node. Called when the device is created and whenever that node changes. */
    void setState(const juce::ValueTree& state);
    const juce::ValueTree& state() const noexcept { return stateCopy; }

    /** Latency in samples, as reported to delay compensation. Fixed between prepares. */
    int latencySamples() const noexcept { return getLatencySamples(); }

    /** Any thread: the device's on/off switch. */
    void setEnabled(bool on) noexcept { enabled.store(on, std::memory_order_relaxed); }
    bool isEnabled() const noexcept { return enabled.load(std::memory_order_relaxed); }

    // --- Audio thread -----------------------------------------------------------------------------
    /** process() with the on/off switch, latency-aligned dry path and MIDI-effect pass-through.
        (Racks do their own.) */
    virtual void processInChain(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept LATTICE_NONBLOCKING;

    /** Instruments release every voice (with their release stage); MIDI effects forget held notes
        and send note-offs for what they were playing. Called when the chain's MIDI path changes. */
    virtual void releaseAllNotes() noexcept LATTICE_NONBLOCKING {}

    /** Audio thread: stops every voice at once, with a fade of a few milliseconds (Drum Rack choke
        groups). Instruments override it; by default it releases notes. */
    virtual void choke() noexcept LATTICE_NONBLOCKING { releaseAllNotes(); }

    /** Audio thread: clears internal state (delay lines, voices, filters). (Also
        juce::AudioProcessor's reset().) */
    void reset() noexcept LATTICE_NONBLOCKING override {}

    /** How the on/off switch treats the device: racks act as an instrument or an audio effect. */
    virtual DeviceKind switchKind() const noexcept { return deviceKind; }

    // --- juce::AudioProcessor -----------------------------------------------------------------------
    const juce::String getName() const override { return name; }
    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override {}
    /** For JUCE hosts and tools: processes one block with no transport. Not used by the engine. */
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using juce::AudioProcessor::processBlock;
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return deviceKind != DeviceKind::AudioEffect; }
    bool producesMidi() const override { return deviceKind == DeviceKind::MidiEffect; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    /** Parameter values (by ID) and the STATE node, as XML. */
    void getStateInformation(juce::MemoryBlock& destination) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

protected:
    /** Message thread: allocate for this sample rate and block size. */
    virtual void prepare(double sampleRate, int maxBlockSize) = 0;
    /** Audio thread: one block. */
    virtual void process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept LATTICE_NONBLOCKING = 0;

    /** Message thread: the STATE node changed (a copy; never empty, but may have no content). */
    virtual void stateChanged(const juce::ValueTree& state);

    /** Message thread, in prepare(): the device's latency at this sample rate. */
    void setLatency(int samples) { setLatencySamples(juce::jmax(0, samples)); }

    float value(int index) const noexcept LATTICE_NONBLOCKING
    {
        return params[static_cast<std::size_t>(index)]->plain();
    }
    double sampleRate() const noexcept { return currentSampleRate; }

private:
    void processAudioEffect(DeviceBuffers& io, const DeviceTimeInfo& time, float target) noexcept LATTICE_NONBLOCKING;
    void processInstrument(DeviceBuffers& io, const DeviceTimeInfo& time, float target) noexcept LATTICE_NONBLOCKING;
    void processMidiEffect(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept LATTICE_NONBLOCKING;
    /** Pushes the block's input through the dry delay line, leaving the delayed signal in `dryBuffer`. */
    void delayDry(const DeviceBuffers& io) noexcept LATTICE_NONBLOCKING;
    void trackOutputNotes(const MidiBlock& out) noexcept LATTICE_NONBLOCKING;
    /** Note-offs for every note held at the output, at offset 0. */
    void flushOutputNotes(MidiBlock& out) noexcept LATTICE_NONBLOCKING;

    const juce::String deviceType;
    const juce::String name;
    const DeviceKind deviceKind;
    std::vector<DeviceParameter*> params; // owned by juce::AudioProcessor
    std::vector<ParameterGroup> groups;
    juce::ValueTree stateCopy;

    double currentSampleRate = 48000.0;
    int preparedBlock = 0;

    // On/off switch
    std::atomic<bool> enabled{true};
    float onMix = 1.0f; // 1 = fully on
    float onMixStep = 1.0f;
    bool midiEffectWasOn = true;
    juce::AudioBuffer<float> dryBuffer;
    juce::AudioBuffer<float> dryDelay; // ring buffer, latency samples per channel
    int dryDelayPos = 0;
    std::array<std::uint16_t, 128> heldOutput{}; // MIDI effects: a bit per channel for each note

    // processBlock() for JUCE hosts
    std::vector<TimedMidi> hostEventsIn;
    std::vector<TimedMidi> hostEventsOut;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StudioDevice)
};

using DevicePtr = std::shared_ptr<StudioDevice>;

} // namespace lattice
