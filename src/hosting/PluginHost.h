#pragma once

#include "core/model/LiveSet.h"
#include "devices/framework/DeviceSource.h"
#include "devices/framework/PluginTypes.h"
#include "hosting/PluginDevice.h"
#include "hosting/scan/ScanProtocol.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>

namespace lattice
{
class SessionController;
}

namespace lattice::hosting
{

class PluginLibrary;
class PluginWindows;

/** Where a plugin device is. */
struct PluginStatus
{
    enum class State
    {
        none, // not a plugin device
        loading,
        ready,
        missing, // not installed
        failed   // installed, but wouldn't load
    };
    State state = State::none;
    juce::String message;
};

/** Plugins as devices (docs/07-m6-plugins-external.md, 6b): the DeviceSource the snapshot build
    gets plugin devices from, and what keeps each plugin and the document in step.

    Loading: a plugin device's first build gets a stand-in (PluginStandIn: silent for an
    instrument, passing audio through for an effect) and its plugin is loaded in the background,
    one at a time on the message thread (most formats must be made there), with Windows' error
    boxes off. When it's ready the chain is built again and the plugin takes the stand-in's place,
    with the chain's usual fade. Its saved state goes back in first. A plugin that isn't installed,
    or won't load, stays a stand-in: the set loads, and its state and values stay in the document
    untouched; it's tried again when a scan finds plugins.

    Values: the document is the truth for the parameters on a device's pages (STATE's configured
    list) and any others it has values for; a plugin's own changes to those (its window) are written
    into the document, a gesture as one undo step, the rest as a step each. A new device's pages are
    the plugin's default (saved per plugin) or its first 64 automatable parameters. In Configure
    mode, a parameter touched in the plugin's window is added to its pages.

    State: a plugin's own state is written into the document (STATE's `data`, without an undo step)
    a second after it last changed, and at once before the set is saved, copied or its device goes,
    so saving and undo never lose a change.

    Latency: a change a plugin reports reaches delay compensation (the chain is published again);
    a CLAP plugin that asks for a restart is prepared again in a new device, keeping its state.

    Message thread. */
class PluginHost final : public DeviceSource, public juce::ChangeBroadcaster, private juce::Timer, private juce::ChangeListener
{
public:
    /** `library`: where installed plugins are found when a saved one moved (null: as saved). */
    PluginHost(SessionController& session, PluginLibrary* library);
    ~PluginHost() override;

    static constexpr int defaultPageParameters = 64;

    // --- Putting plugins on tracks --------------------------------------------------------------------
    /** The device type a plugin goes on a track as. */
    static juce::String typeFor(const scan::ScannedPlugin& plugin);
    /** Makes a device node (just inserted, of typeFor()'s type) this plugin's: its name and
        description, as part of the edit under way. */
    static void describeDevice(LiveSet& set, juce::ValueTree device, const scan::ScannedPlugin& plugin);
    /** An installed plugin by its key (a browser item's, after "plugin:"), if the library knows it. */
    std::optional<scan::ScannedPlugin> installedPlugin(const juce::String& key) const;
    /** The plugin a device node is (from its STATE), if it is one. */
    static std::optional<scan::ScannedPlugin> pluginOf(const juce::ValueTree& device);

    // --- What the UI and controllers see ----------------------------------------------------------------
    PluginStatus statusOf(const juce::ValueTree& device) const;
    /** The device as the UI describes it: its pages' parameters, as the live plugin describes them
        (no parameters while it isn't loaded). Lives as long as the host. */
    const StudioDevice* describe(const juce::ValueTree& device);
    /** Every parameter the live plugin has (for adding to its pages), or empty. */
    std::vector<ParameterSpec> allParameters(const juce::ValueTree& device);
    HostedPluginDevice* liveDevice(std::int64_t deviceId) const;

    /** Configure mode: parameters touched in the plugin's window join its pages. */
    void setConfiguring(std::int64_t deviceId, bool on);
    bool isConfiguring(std::int64_t deviceId) const { return configuring.contains(deviceId); }
    /** Adds a parameter to a device's pages, or takes it off (one undo step each). */
    void addToPages(juce::ValueTree device, const juce::String& parameterId);
    void removeFromPages(juce::ValueTree device, const juce::String& parameterId);
    /** The pages new devices of this plugin start with. */
    void saveDefaultPages(const juce::ValueTree& device);
    /** Whether a device gets MPE, and setting it (one undo step). */
    bool mpeOf(const juce::ValueTree& device) const;
    void setMpe(juce::ValueTree device, bool on);

    /** Where the defaults for each plugin are kept (an app setting), by plugin key. Unset: nowhere. */
    std::function<juce::String(const juce::String& pluginKey)> loadDefaultPages;
    std::function<void(const juce::String& pluginKey, const juce::String& pages)> storeDefaultPages;

    PluginWindows& windows() noexcept { return *pluginWindows; }

    // --- Keeping the document up to date ---------------------------------------------------------------
    /** Writes each plugin's state into the document now, if it changed. */
    void captureAll();
    /** What the timer does (tests without a message loop call it): loads the next plugin, takes
        what plugins did into the document, writes state that settled. */
    void idle();
    /** Tests: loads every plugin waiting, and builds their chains. */
    void finishLoading();
    /** Tries again the plugins that weren't installed (a scan found plugins). */
    void retryMissing();
    /** How many plugins are waiting to load. */
    int loadsWaiting() const noexcept { return static_cast<int>(queue.size()); }

    // --- DeviceSource ------------------------------------------------------------------------------------
    bool makes(const juce::String& typeId) const override { return pluginTypes::isPlugin(typeId); }
    std::unique_ptr<StudioDevice> create(const juce::ValueTree& deviceNode, const DeviceCreateContext& context) override;
    bool isStale(const StudioDevice& device, const juce::ValueTree& deviceNode) const override;

    juce::AudioPluginFormatManager& formats() noexcept { return formatManager; }

private:
    struct Waiting
    {
        juce::String identity;
        juce::PluginDescription description;
    };
    struct Ready
    {
        juce::String identity;
        std::unique_ptr<juce::AudioPluginInstance> instance;
        bool live = false; // it comes from a device that restarts: its state is in it
    };
    struct Failure
    {
        juce::String identity;
        PluginStandIn::Why why = PluginStandIn::Why::failed;
        juce::String message;
    };
    struct Live
    {
        HostedPluginDevice* device = nullptr;
        juce::uint32 changedAt = 0; // when its state last changed, unwritten (0: written)
        juce::ValueTree node;       // its node (kept when it leaves the set: undo can bring it back)
        std::set<int> gestures;     // parameters in a gesture from the plugin
        std::set<int> gestureSteps; // ...whose undo step has begun
    };

    void timerCallback() override { idle(); }
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void loadNext();
    std::optional<juce::PluginDescription> installed(const juce::PluginDescription& saved) const;
    void prepareBuses(juce::AudioPluginInstance& instance, const juce::String& typeId) const;
    void takeChanges(std::int64_t id, Live& live);
    void capture(std::int64_t id, Live& live);
    void configureNew(std::int64_t id);
    void restart(std::int64_t id);
    void wakeUp();
    juce::StringArray pagesOf(const juce::ValueTree& device) const;
    void setPages(juce::ValueTree device, const juce::StringArray& pages, juce::UndoManager* undo);
    static juce::String identityOf(const juce::PluginDescription& d) { return d.createIdentifierString(); }

    SessionController& session;
    PluginLibrary* library;
    juce::AudioPluginFormatManager formatManager;
    std::unique_ptr<PluginWindows> pluginWindows;

    std::deque<std::int64_t> queue;
    std::map<std::int64_t, Waiting> waiting;
    std::map<std::int64_t, Ready> ready;
    std::map<std::int64_t, Failure> failures;
    std::map<std::int64_t, Live> live;
    std::set<std::int64_t> restarting, configuring, newDevices;

    struct Described
    {
        const HostedPluginDevice* device = nullptr;
        juce::String pages;
        int revision = 0;
        const StudioDevice* spec = nullptr;
    };
    std::map<std::int64_t, Described> described;
    std::vector<std::unique_ptr<StudioDevice>> specs; // every description handed out (callers keep pointers)
    std::unique_ptr<StudioDevice> noParameters[3];    // instrument, effect, MIDI effect
    std::map<std::int64_t, int> revisions;             // parameter names changed

    std::vector<HostedPluginDevice::Report> reports; // scratch

    JUCE_DECLARE_NON_COPYABLE(PluginHost)
};

} // namespace lattice::hosting
