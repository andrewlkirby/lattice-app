#include "hosting/PluginTuning.h"

#include "engine/SessionController.h"
#include "engine/TrackProcessor.h"
#include "hosting/PluginHost.h"

namespace lattice::hosting
{

namespace
{
/** How often an absent master is looked for again, in ticks (30 a second). */
constexpr int retryTicks = 60;

/** The track a device is on (inside racks too), or an invalid tree if it isn't in the set. */
juce::ValueTree trackOf(juce::ValueTree node)
{
    while (node.isValid() && !node.hasType(ids::track))
        node = node.getParent();
    return node;
}
} // namespace

PluginTuning::PluginTuning(SessionController& s, PluginHost& p, mts::Master& m) : session(s), plugins(p), master(m)
{
    startTimerHz(30);
}

PluginTuning::~PluginTuning()
{
    stopTimer();
}

void PluginTuning::timerCallback()
{
    if (++ticks % retryTicks == 0)
        master.retry();
    update();
}

mts::Tuning PluginTuning::tuningOf(const TrackTuning& tuning)
{
    mts::Tuning t;
    for (int n = 0; n < 128; ++n)
        t.hz[static_cast<std::size_t>(n)] = tuning.hzFor(n);
    if (const auto& table = tuning.getTuning())
    {
        t.name = table->name;
        t.periodRatio = table->periodRatio;
    }
    return t;
}

void PluginTuning::update()
{
    // The plugin instruments in the set, by track.
    std::vector<std::pair<HostedPluginDevice*, std::int64_t>> players;
    std::vector<HostedPluginDevice*> others;
    plugins.forEachLive([&](HostedPluginDevice& device, const juce::ValueTree& node) {
        const auto track = trackOf(node);
        if (device.kind() == DeviceKind::Instrument && track.isValid())
            players.emplace_back(&device, LiveSet::idOf(track));
        else
            others.push_back(&device);
    });
    for (auto* d : others)
        d->setMtsChannel(-1);
    if (players.empty())
        return;

    // Each track's tuning, worked out again only if something it depends on changed.
    std::vector<std::int64_t> order;
    std::vector<mts::Tuning> tunings;
    for (const auto& [device, trackId] : players)
    {
        if (std::find(order.begin(), order.end(), trackId) != order.end())
            continue;
        const auto processor = session.snapshots().processorFor(trackId);
        if (processor == nullptr)
            continue;
        const auto& tuning = processor->tuning();
        auto& cached = tracks[trackId];
        const void* table = tuning.getTuning().get();
        const int offset = tuning.getNoteOffset(), policy = static_cast<int>(tuning.getHighNotePolicy());
        const double rate = tuning.getSampleRate();
        if (cached.table != table || cached.offset != offset || cached.policy != policy || cached.sampleRate != rate)
            cached = {table, offset, policy, rate, tuningOf(tuning)};
        order.push_back(trackId);
        tunings.push_back(cached.tuning);
    }
    std::erase_if(tracks, [&](const auto& t) { return std::find(order.begin(), order.end(), t.first) == order.end(); });

    // Sent even while it isn't the master, so it has them when it becomes one; the channels only
    // matter while it is.
    const auto channels = master.send(tunings);
    const bool active = master.status() == mts::Master::Status::active;
    for (const auto& [device, trackId] : players)
    {
        const auto it = std::find(order.begin(), order.end(), trackId);
        device->setMtsChannel(active && it != order.end() ? channels[static_cast<std::size_t>(it - order.begin())] : -1);
    }
}

} // namespace lattice::hosting
