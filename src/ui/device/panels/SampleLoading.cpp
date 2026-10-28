#include "devices/samples/SamplePool.h"
#include "ui/device/DevicePanels.h"

namespace lattice::ui::panels
{

void loadSampleInto(DeviceContext& context, juce::ValueTree sampler, const juce::File& file, bool newTransaction)
{
    auto& set = context.set();
    auto& undo = context.undo();
    if (newTransaction)
        undo.beginNewTransaction("Load sample");
    auto state = sampler.getChildWithName(ids::deviceState);
    if (!state.isValid())
    {
        state = juce::ValueTree(ids::deviceState);
        sampler.appendChild(state, set.undoManager());
    }
    state.setProperty("file", file.getFullPathName(), set.undoManager());

    // The root the file names, for a sampler that plays the track's tuning (a drum pad plays fixed
    // pitches, at its sample's own pitch, so its root stays). The file is read in the background;
    // the root joins this undo step if nothing else happened meanwhile, and a root set by hand
    // since then is left alone.
    if (LiveSet::deviceParameter(sampler, "sample.tuning", 0.0) >= 0.5)
        return;
    const auto transaction = undo.getCurrentTransactionName();
    const double rootBefore = LiveSet::deviceParameter(sampler, "sample.root", 261.6256);
    const std::weak_ptr<int> alive = context.lifetime;
    auto* session = &context.session;
    SamplePool::shared().request(file, [alive, session, sampler, path = file.getFullPathName(), transaction,
                                        rootBefore](SamplePtr sample, const juce::String&) {
        if (alive.expired() || sample == nullptr || sample->suggestedRootHz <= 0.0)
            return;
        auto& liveSet = session->set();
        auto& um = session->undoManager();
        if (!sampler.isAChildOf(liveSet.state()) || sampler.getChildWithName(ids::deviceState)["file"].toString() != path ||
            LiveSet::deviceParameter(sampler, "sample.root", 261.6256) != rootBefore)
            return;
        if (um.getCurrentTransactionName() != transaction)
            um.beginNewTransaction("Root from sample");
        liveSet.setDeviceParameter(sampler, "sample.root", sample->suggestedRootHz);
    });
}

} // namespace lattice::ui::panels
