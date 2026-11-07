#pragma once

#include "devices/framework/PluginTypes.h"
#include "devices/framework/StudioDevice.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace lattice::hosting
{

/** What a plugin's parameters say in words, for the device's descriptions (ParameterSpec's
    formatter and parser): asks the plugin while it lives, and gives a plain number after.
    Message thread. */
struct PluginText
{
    juce::AudioPluginInstance* plugin = nullptr; // cleared when the plugin goes
};

/** Parameter specs for every parameter of a plugin, in its order: normalised 0..1 values (a
    stepped one in steps), named and grouped as the plugin does (VST3 units, CLAP modules), with
    text from the plugin. IDs are "p" and the plugin's own parameter ID (stable across versions,
    unlike indices). */
std::vector<ParameterSpec> describeParameters(juce::AudioPluginInstance& plugin, const std::shared_ptr<PluginText>& text);

/** The ID a plugin parameter gets ("p" + its own ID, made a valid ParameterSpec ID). */
juce::String parameterIdFor(const juce::AudioProcessorParameter& parameter);

/** A plugin as a device (docs/07-m6-plugins-external.md, 6b): slots into chains, racks, Drum
    Rack pads, returns and the master like any built-in device, and gets StudioDevice's on/off
    switch and latency-aligned dry path.

    Parameters: every plugin parameter is a DeviceParameter with the plugin's normalised range.
    Only those with a value in the document are the document's (the ones on its pages, mapped, or
    on a macro); the rest live in the plugin's own state, as in Live, so a plugin with 2,000
    parameters doesn't put 2,000 values in the set. A document value is sent to the plugin when it
    changes (or a macro moves it), from the audio thread; the plugin's own changes (its window) are
    collected for the message thread (takeReports()), with its gestures, for the host to write into
    the document.

    The plugin is told the transport (as its play head), gets the chain's MIDI (per-note
    expression as MPE when it takes MPE, plain channel messages otherwise), its sidechain when it
    has an input for one, and as a MIDI effect passes its MIDI out to the next device.

    Latency: read when prepared; a change the plugin reports is taken on the message thread
    (takeLatencyChange()), up to half a second more than when prepared (latencyReserve()).

    Threads: construction, prepare and the take...() calls on the message thread, process() on the
    audio thread. The plugin is destroyed with the device, on the message thread (the engine frees
    devices there). */
class HostedPluginDevice final : public StudioDevice,
                                 private juce::AudioProcessorListener,
                                 private juce::AudioPlayHead
{
public:
    HostedPluginDevice(std::int64_t deviceId, const juce::String& typeId, std::unique_ptr<juce::AudioPluginInstance> plugin);
    ~HostedPluginDevice() override;

    std::int64_t deviceId() const noexcept { return id; }
    juce::AudioPluginInstance& plugin() const noexcept { return *instance; }
    const std::shared_ptr<PluginText>& text() const noexcept { return words; }

    /** Message thread: per-note expression goes to the plugin as MPE (or, for a CLAP plugin that
        takes its own note events, as note expressions). Off: notes come on channel 1, with no
        per-note bend, pressure or timbre. */
    void setMpe(bool on) noexcept;
    bool mpe() const noexcept { return mpeOn.load(std::memory_order_relaxed); }
    /** Whether it gets MPE when its STATE doesn't say (what its scan found). */
    void setMpeDefault(bool on) { mpeDefault = on; }
    /** Message thread: a latency the plugin reported (takeLatencyChange()), for delay compensation.
        False if it's more than the dry path can hold: the device must be prepared again. */
    bool followLatency(int samples) { return changeLatency(samples); }

    /** Message thread: the plugin's values as they are now are what's known, and what it announces
        for a moment is taken as that, not as edits (it loaded a preset of its own). */
    void forgetChanges();
    /** Message thread: the plugin's state, and putting it back. Restoring doesn't count as the
        plugin changing (takeStateChanged()). */
    juce::MemoryBlock saveState();
    void restoreState(const juce::MemoryBlock& state);

    // --- What the plugin did, for the message thread ------------------------------------------------
    struct Report
    {
        int index = 0;
        float value = 0.0f;
        bool begin = false, end = false, changed = false;
    };
    /** Its parameter changes (and gestures) since the last call, for values that changed from what
        the message thread last knew. */
    void takeReports(std::vector<Report>& out);
    /** Whether its state changed (any parameter, or state that isn't one) since the last call. */
    bool takeStateChanged() noexcept { return stateDirty.exchange(false); }
    /** Its new latency, if it changed since the last call. */
    bool takeLatencyChange(int& newLatency);
    /** Whether its parameters' names or groups changed. */
    bool takeParameterInfoChange() noexcept { return infoDirty.exchange(false); }
    /** The plugin's value of a parameter now, as the message thread knows it. */
    float knownValue(int index) const noexcept;

    /** Message thread: the value the document gives a parameter is the plugin's now: sent once, as
        if it had come from the document (the host writes it there). */
    void adoptPluginValue(int index, float value) noexcept;

    /** Called (message thread) as the device goes, with the plugin, if `keepPlugin` was set: it
        restarts in a new device rather than being destroyed (CLAP's request_restart). */
    std::function<void(std::unique_ptr<juce::AudioPluginInstance>)> keepPlugin;
    /** Called (message thread) as the device goes, before the plugin does: its window must go
        first. */
    std::function<void()> onGoing;

    // --- Diagnostics ---------------------------------------------------------------------------------
    /** Allocations the plugin made on the audio thread (where they can be seen: on Linux; a Windows
        plugin allocates with its own allocator). */
    std::uint64_t allocationsInPlugin() const noexcept { return allocations.load(std::memory_order_relaxed); }

    // --- StudioDevice ----------------------------------------------------------------------------------
    void applyDocumentValue(int index, const juce::var* documentValue) override;
    void releaseAllNotes() noexcept override { notesOff.store(true, std::memory_order_relaxed); }
    void choke() noexcept override { notesOff.store(true, std::memory_order_relaxed); }
    void reset() noexcept override {}
    int latencyReserve() const noexcept override;
    double getTailLengthSeconds() const override { return instance->getTailLengthSeconds(); }
    bool hasEditor() const override { return false; } // its window is the host's (PluginWindows)

protected:
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept override;
    void stateChanged(const juce::ValueTree& state) override;

private:
    HostedPluginDevice(std::int64_t deviceId, const juce::String& typeId, std::unique_ptr<juce::AudioPluginInstance> plugin,
                       std::shared_ptr<PluginText> text);
    void watch(int index);
    void unwatch(int index);

    // juce::AudioProcessorListener (any thread)
    void audioProcessorParameterChanged(juce::AudioProcessor*, int parameterIndex, float newValue) override;
    void audioProcessorChanged(juce::AudioProcessor*, const juce::AudioProcessorListener::ChangeDetails& details) override;
    void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int parameterIndex) override;
    void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int parameterIndex) override;

    // juce::AudioPlayHead (audio thread, inside process())
    juce::Optional<PositionInfo> getPosition() const override;

    const std::int64_t id;
    std::unique_ptr<juce::AudioPluginInstance> instance;
    std::shared_ptr<PluginText> words;
    std::vector<juce::AudioProcessorParameter*> pluginParameters;
    const int count;

    // Parameters the document has values for (audio thread reads them; the message thread adds)
    std::unique_ptr<std::atomic<bool>[]> watched;
    std::vector<int> watchList;       // indices, appended to (never shrinks)
    std::vector<bool> everWatched;    // message thread
    std::atomic<int> watchCount{0};
    std::unique_ptr<std::atomic<float>[]> sent; // the value last sent to the plugin

    // The plugin's own changes (written from any thread)
    std::unique_ptr<std::atomic<float>[]> reported;
    std::unique_ptr<std::atomic<int>[]> reportFlags; // 1 changed, 2 gesture begun, 4 ended
    std::atomic<bool> anyReport{false}, stateDirty{false}, latencyDirty{false}, infoDirty{false};
    std::vector<float> known; // message thread: its values as last known
    bool restoring = false;   // message thread: inside restoreState()
    juce::uint32 settleUntil = 0; // message thread: announcements until then are a restore's

    // Processing
    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
    std::vector<int> mainIn, sideIn, mainOut; // channel indices in `buffer`
    DeviceTimeInfo blockTime;                 // for the play head, during process()
    std::atomic<bool> mpeOn{false}, notesOff{false};
    bool mpeDefault = false;
    std::atomic<std::uint64_t> allocations{0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HostedPluginDevice)
};

/** A plugin device as the UI and controllers see it (DeviceSpecs::forDevice): the parameters on
    its pages (STATE's `configured`), described as the live plugin describes them, grouped as the
    plugin groups them. No plugin inside; never prepared or processed. */
class PluginSpecDevice final : public StudioDevice
{
public:
    PluginSpecDevice(const juce::String& typeId, const juce::String& name, std::vector<ParameterSpec> specs)
        : StudioDevice(typeId, name, pluginTypes::kindOf(typeId), std::move(specs))
    {
    }

protected:
    void prepare(double, int) override {}
    void process(DeviceBuffers&, const DeviceTimeInfo&) noexcept override {}
};

} // namespace lattice::hosting
