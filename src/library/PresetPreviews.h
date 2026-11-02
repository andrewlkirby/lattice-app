#pragma once

#include "core/JobPool.h"
#include "devices/samples/SamplePool.h"

#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <list>
#include <memory>

namespace lattice
{
class AudioEngine;
class DeviceRegistry;
class SessionController;
class TuningRegistry;
} // namespace lattice

namespace lattice::library
{

/** The short phrase an instrument preset plays when the browser selects it (docs/05-m4-ui.md, 4e).

    The preset is put on the one track of a private set, with its own engine that never opens an
    audio device, and played offline on the job pool: a rising arpeggio and a chord for an
    instrument, a beat on the first pads for a Drum Rack kit. What it renders is kept for the last
    few presets, so selecting one again plays at once. Message thread. */
class PresetPreviews
{
public:
    static constexpr double seconds = 3.0;

    PresetPreviews(TuningRegistry& tunings, const DeviceRegistry& devices, JobPool& jobs);
    ~PresetPreviews();

    /** Renders (or finds) the phrase for a preset's DEVICE tree, at the given rate, and calls done
        with it on the message thread. A newer request replaces one that hasn't finished: only the
        last one's done is called. */
    void request(const juce::String& key, const juce::ValueTree& device, double sampleRate, std::function<void(SamplePtr)> done);
    void cancel();

    /** True for presets that get a phrase: instruments (racks of them too) and Drum Rack kits. */
    bool plays(const juce::String& deviceType) const;

    /** Tests: whether a render is still running. */
    bool busy() const noexcept { return job != 0 || waiting != 0; }

private:
    struct Request
    {
        juce::String key;
        juce::ValueTree device;
        double sampleRate = 48000.0;
        std::function<void(SamplePtr)> done;
    };
    void start(Request r);
    void render(Request r);
    SamplePtr cached(const juce::String& key, double sampleRate) const;

    TuningRegistry& tunings;
    const DeviceRegistry& registry;
    JobPool& jobs;
    std::unique_ptr<AudioEngine> engine;
    std::unique_ptr<SessionController> session;
    std::uint64_t job = 0, waiting = 0;
    std::unique_ptr<Request> next; // asked for while one was rendering
    std::list<std::pair<juce::String, SamplePtr>> recent; // newest first
    std::shared_ptr<int> alive = std::make_shared<int>(0);
};

} // namespace lattice::library
