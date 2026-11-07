#pragma once

#include "core/RealtimeCheck.h"

#include <clap/clap.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lattice::hosting
{

class ClapLibrary;
class ClapParameter;
class ClapEditor;
struct ClapHostContext;

/** A CLAP plugin as a juce::AudioPluginInstance (docs/07-m6-plugins-external.md, 6b), so the rest
    of Lattice hosts every format the same way. JUCE 8 has no CLAP host; this is ours, from the CLAP
    headers.

    - Buses: one per audio port, all always on (a port's channels may not be turned off in CLAP).
    - Parameters: one HostedAudioProcessorParameter per CLAP parameter, grouped by its module path
      ("Osc/Osc 1"), normalised 0..1 over its range. A value set from any thread reaches the plugin
      as a parameter event in its next process() call (or a flush() while it isn't active). The
      plugin's own changes (its window, its automation out) are announced to the parameters'
      listeners on the message thread, with its gestures.
    - Notes: in CLAP's own note events when the note port takes them (each note gets an ID), MIDI
      otherwise. With setMpeInput(), the member channels' pitch bend, pressure and CC 74 become
      CLAP note expressions (tuning, pressure, brightness) of the notes on those channels; a plugin
      that takes only MIDI gets the MPE MIDI as it is.
    - Transport, tempo and time signature come from the play head, every block.
    - Latency is read when activated. A plugin that wants to change it, or its ports, while active
      asks for a restart: onRestartRequested is called, and the owner deactivates it and prepares it
      again (releaseResources() and prepareToPlay()) when it's safe to.
    - Timers, fd callbacks (Linux), thread checks and logging are provided; the plugin's window is a
      ClapEditor.

    Threads: as CLAP says. Construction, prepareToPlay(), releaseResources(), state, the editor and
    text conversion on the message thread; processBlock() on the audio thread. */
class ClapPluginInstance final : public juce::AudioPluginInstance, private juce::Timer
{
public:
    /** The plugin a description names (its file and CLAP id), created and initialised. Null with
        `error` set if it can't be. */
    static std::unique_ptr<ClapPluginInstance> create(const juce::PluginDescription& description, double sampleRate,
                                                      int blockSize, juce::String& error);
    ~ClapPluginInstance() override;

    /** The MIDI coming in is MPE (a lower zone: channel 1 the master, 2-16 the members). */
    void setMpeInput(bool on) noexcept { mpeInput.store(on, std::memory_order_relaxed); }
    /** Whether its note input takes CLAP's own note events (so expressions reach it per note). */
    bool takesClapNotes() const noexcept { return clapNotes; }
    /** Whether its note input says it understands MPE MIDI. */
    bool takesMpe() const noexcept { return mpeMidi; }

    /** Message thread: the plugin asked to be restarted (to change its latency or ports). */
    std::function<void()> onRestartRequested;
    /** Message thread: the plugin's window asked to be closed (it was closed from inside). */
    std::function<void()> onEditorClosed;

    const clap_plugin_t* clapPlugin() const noexcept { return plugin; }

    /** Message thread: what the plugin asked for since the last call (timers do this 30 times a
        second; tests without a message loop call it themselves). */
    void idle();

    /** A member channel's pitch bend range either way, in semitones (MPE's default). */
    static constexpr double perNoteSemitones = 48.0;

    // For the plugin's calls to its host (message thread).
    bool registerTimer(std::uint32_t periodMs, clap_id& id);
    bool unregisterTimer(clap_id id);
#if JUCE_LINUX || JUCE_BSD
    bool watchFd(int fd, std::uint32_t flags);
    bool unwatchFd(int fd);
#endif

    // --- juce::AudioPluginInstance ---------------------------------------------------------------
    void fillInPluginDescription(juce::PluginDescription& description) const override;
    const juce::String getName() const override { return info.name; }
    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) noexcept override LATTICE_NONBLOCKING;
    using juce::AudioPluginInstance::processBlock;
    void reset() noexcept override LATTICE_NONBLOCKING;
    bool isBusesLayoutSupported(const BusesLayout& layout) const override;
    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override { return noteInputs > 0; }
    bool producesMidi() const override { return noteOutputs > 0; }
    bool hasEditor() const override;
    juce::AudioProcessorEditor* createEditor() override;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& destination) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

private:
    friend class ClapParameter;
    friend class ClapEditor;
    friend struct ClapHostContext;
    class Events;

    ClapPluginInstance(BusesProperties buses, std::shared_ptr<ClapLibrary> library,
                       std::unique_ptr<ClapHostContext> context, const clap_plugin_t* plugin,
                       const juce::PluginDescription& description);
    void readExtensions();
    void readParameters();
    void refreshParameterInfo();
    void deactivate();
    void timerCallback() override;

    /** A parameter's change for the plugin (any thread). */
    void queueValue(int index, double plainValue) noexcept LATTICE_NONBLOCKING;
    juce::String valueToText(int index, double plainValue) const;
    std::optional<double> textToValue(int index, const juce::String& text) const;

    void addTransport(Events& in) noexcept LATTICE_NONBLOCKING;
    void addParameterChanges(Events& in) noexcept LATTICE_NONBLOCKING;
    void addMidi(Events& in, const juce::MidiBuffer& midi, int numSamples) noexcept LATTICE_NONBLOCKING;
    void readOutput(const Events& out, juce::MidiBuffer& midi) noexcept LATTICE_NONBLOCKING;
    void deliverReports();

    std::shared_ptr<ClapLibrary> library;
    std::unique_ptr<ClapHostContext> host;
    const clap_plugin_t* plugin = nullptr;
    juce::PluginDescription info;

    const clap_plugin_params_t* params = nullptr;
    const clap_plugin_state_t* state = nullptr;
    const clap_plugin_latency_t* latency = nullptr;
    const clap_plugin_tail_t* tail = nullptr;
    const clap_plugin_gui_t* gui = nullptr;
    const clap_plugin_timer_support_t* timers = nullptr;
    const clap_plugin_posix_fd_support_t* fds = nullptr;
    const clap_plugin_audio_ports_t* audioPorts = nullptr;
    const clap_plugin_note_ports_t* notePorts = nullptr;

    int noteInputs = 0, noteOutputs = 0;
    bool clapNotes = false, midiNotes = true, mpeMidi = false;
    std::atomic<bool> mpeInput{false};

    // Parameters
    std::vector<ClapParameter*> parameters; // owned by juce::AudioProcessor
    std::unordered_map<clap_id, int> indexOfId;
    std::unique_ptr<std::atomic<double>[]> pendingValue;
    std::unique_ptr<std::atomic<bool>[]> pendingFlag;
    std::atomic<bool> anyPending{false};
    // The plugin's own changes, for the listeners (delivered on the message thread)
    std::unique_ptr<std::atomic<bool>[]> reportedValue;
    std::unique_ptr<std::atomic<int>[]> reportedGestures; // +1 begun, +2 ended (bits), cleared when delivered
    std::atomic<bool> anyReport{false};
    std::vector<bool> gestureOpen; // as delivered to the listeners

    // Audio
    std::atomic<bool> active{false};
    std::atomic<bool> processing{false};
    std::int64_t steadyTime = 0;
    std::vector<clap_audio_buffer_t> inputBuffers, outputBuffers;
    std::vector<std::vector<float*>> inputPointers, outputPointers;
    std::vector<int> inputChannels, outputChannels; // per port
    std::vector<std::vector<int>> inputIndex, outputIndex; // per port: its channels in the processBlock buffer
    juce::AudioBuffer<float> inputScratch;
    juce::AudioBuffer<float> outputSpare; // for outputs the buffer has no room for
    std::unique_ptr<Events> inEvents, outEvents; // audio thread
    std::unique_ptr<Events> mainIn, mainOut;     // flush() on the message thread
    clap_event_transport_t transport{};
    std::array<std::array<std::int32_t, 128>, 16> noteIds{}; // the ID of the note playing on each channel and key, or -1
    std::array<float, 16> channelBend{}, channelPressure{}, channelTimbre{};
    std::int32_t nextNoteId = 0;

    // Timers the plugin registered (message thread)
    class PluginTimer;
    std::map<clap_id, std::unique_ptr<PluginTimer>> pluginTimers;
    clap_id nextTimerId = 1;
    std::vector<int> watchedFds;

    juce::Component::SafePointer<ClapEditor> editor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClapPluginInstance)
};

} // namespace lattice::hosting
