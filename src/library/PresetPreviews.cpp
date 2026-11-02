#include "library/PresetPreviews.h"

#include "core/model/LiveSet.h"
#include "devices/framework/DeviceRegistry.h"
#include "devices/racks/Rack.h"
#include "engine/AudioEngine.h"
#include "engine/SessionController.h"

#include <algorithm>
#include <cmath>

namespace lattice::library
{

namespace
{
constexpr int blockSize = 256;
constexpr std::size_t keep = 8;

struct Note
{
    double start, length;
    int note, velocity;
};

/** An instrument's phrase: a rising arpeggio, then a chord. */
std::vector<Note> instrumentPhrase()
{
    std::vector<Note> notes;
    const int arpeggio[] = {48, 55, 60, 64};
    for (int i = 0; i < 4; ++i)
        notes.push_back({0.25 * i, 0.22, arpeggio[i], 96});
    for (const int n : {60, 64, 67})
        notes.push_back({1.05, 1.0, n, 88});
    return notes;
}

/** A kit's phrase: a bar of a beat on the first pads (kick 36, snare 38, closed hat 42). */
std::vector<Note> drumPhrase()
{
    std::vector<Note> notes;
    const double step = 0.125; // sixteenths at 120 BPM
    for (int i = 0; i < 16; ++i)
    {
        if (i % 2 == 0)
            notes.push_back({i * step, 0.1, 42, i % 4 == 0 ? 100 : 70});
        if (i == 0 || i == 6 || i == 10)
            notes.push_back({i * step, 0.1, 36, 112});
        if (i == 4 || i == 12)
            notes.push_back({i * step, 0.1, 38, 104});
    }
    return notes;
}
} // namespace

PresetPreviews::PresetPreviews(TuningRegistry& t, const DeviceRegistry& d, JobPool& j) : tunings(t), registry(d), jobs(j) {}

PresetPreviews::~PresetPreviews()
{
    cancel();
    // A render still running reads the private engine: wait for it before it goes.
    jobs.waitUntilIdle(5000);
}

bool PresetPreviews::plays(const juce::String& deviceType) const
{
    if (deviceType == RackDevice::instrumentRackId || deviceType == RackDevice::drumRackId)
        return true;
    const auto* info = registry.find(deviceType);
    return info != nullptr && info->kind == DeviceKind::Instrument;
}

SamplePtr PresetPreviews::cached(const juce::String& key, double sampleRate) const
{
    for (const auto& [k, s] : recent)
        if (k == key && s != nullptr && s->sampleRate == sampleRate)
            return s;
    return nullptr;
}

void PresetPreviews::cancel()
{
    next.reset();
    if (waiting != 0)
        jobs.cancel(waiting);
    if (job != 0)
        jobs.cancel(job);
}

void PresetPreviews::request(const juce::String& key, const juce::ValueTree& device, double sampleRate,
                             std::function<void(SamplePtr)> done)
{
    if (auto s = cached(key, sampleRate))
    {
        cancel();
        done(std::move(s));
        return;
    }
    Request r{key, device.createCopy(), sampleRate, std::move(done)};
    if (job != 0 || waiting != 0)
    {
        // One render at a time (they share the private engine): this one goes next.
        if (job != 0)
            jobs.cancel(job);
        next = std::make_unique<Request>(std::move(r));
        return;
    }
    start(std::move(r));
}

void PresetPreviews::start(Request r)
{
    if (engine == nullptr)
    {
        engine = std::make_unique<AudioEngine>();
        session = std::make_unique<SessionController>(*engine, tunings, registry);
    }
    session->newSet(1, 1); // track 1 is armed: it plays live input
    auto chain = LiveSet::chainOf(session->set().track(0));
    while (chain.getNumChildren() > 0)
        session->set().removeDevice(chain.getChild(0));
    session->set().insertDeviceCopy(chain, -1, r.device);
    engine->prepare(r.sampleRate, blockSize);
    session->flush();

    // Samples the preset plays load in the background; the phrase waits for them.
    std::weak_ptr<int> life = alive;
    auto shared = std::make_shared<Request>(std::move(r));
    waiting = jobs.submit("Loading the preset's samples", [](JobPool::Context&) { SamplePool::shared().waitUntilIdle(); },
                          [this, life, shared](bool cancelled) {
                              if (life.expired())
                                  return;
                              waiting = 0;
                              if (cancelled || next != nullptr)
                              {
                                  if (next != nullptr)
                                  {
                                      auto n = std::move(*next);
                                      next.reset();
                                      start(std::move(n));
                                  }
                                  return;
                              }
                              SamplePool::shared().deliverFinished();
                              session->flush();
                              render(std::move(*shared));
                          });
}

void PresetPreviews::render(Request r)
{
    const bool kit = r.device[ids::deviceType].toString() == RackDevice::drumRackId;
    const auto notes = kit ? drumPhrase() : instrumentPhrase();
    const double rate = r.sampleRate;
    auto result = std::make_shared<SamplePtr>();
    std::weak_ptr<int> life = alive;
    auto shared = std::make_shared<Request>(std::move(r));
    auto* e = engine.get();
    job = jobs.submit(
        "Preview of a preset",
        [e, notes, rate, result, name = shared->key](JobPool::Context& work) {
            const auto total = static_cast<int>(seconds * rate);
            std::vector<float> left(static_cast<std::size_t>(total), 0.0f), right(left.size(), 0.0f);
            // Each note's on and off, in order of time.
            struct Event
            {
                int frame;
                juce::MidiMessage message;
            };
            std::vector<Event> events;
            for (const auto& n : notes)
            {
                events.push_back({static_cast<int>(n.start * rate), juce::MidiMessage::noteOn(1, n.note, static_cast<juce::uint8>(n.velocity))});
                events.push_back({static_cast<int>((n.start + n.length) * rate), juce::MidiMessage::noteOff(1, n.note)});
            }
            std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.frame < b.frame; });
            std::size_t nextEvent = 0;
            for (int at = 0; at < total; at += blockSize)
            {
                if (work.isCancelled())
                    return;
                while (nextEvent < events.size() && events[nextEvent].frame < at + blockSize)
                    e->midiInput().push(events[nextEvent++].message);
                const int n = std::min(blockSize, total - at);
                float* outs[2] = {left.data() + at, right.data() + at};
                e->process(outs, 2, n);
            }
            // Fade the last tenth of a second, so a long release doesn't stop with a click.
            const int fade = std::min(total, static_cast<int>(0.1 * rate));
            for (int i = 0; i < fade; ++i)
            {
                const float g = static_cast<float>(i) / static_cast<float>(fade);
                left[static_cast<std::size_t>(total - 1 - i)] *= g;
                right[static_cast<std::size_t>(total - 1 - i)] *= g;
            }
            *result = SampleData::fromChannels({std::move(left), std::move(right)}, rate, name);
        },
        [this, life, result, shared](bool cancelled) {
            if (life.expired())
                return;
            job = 0;
            // Notes still held from a cancelled render: everything off before the next one.
            for (int n = 0; n < 128; ++n)
                engine->midiInput().push(juce::MidiMessage::noteOff(1, n));
            if (!cancelled && *result != nullptr)
            {
                recent.remove_if([&](const auto& entry) { return entry.first == shared->key; });
                recent.emplace_front(shared->key, *result);
                while (recent.size() > keep)
                    recent.pop_back();
                if (next == nullptr && shared->done)
                    shared->done(*result);
            }
            if (next != nullptr)
            {
                auto n = std::move(*next);
                next.reset();
                start(std::move(n));
            }
        });
}

} // namespace lattice::library
