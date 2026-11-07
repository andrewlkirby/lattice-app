#include "ui/controllers/SurfaceManager.h"

#include "control/push/PushDriver.h"
#include "control/push/PushProtocol.h"

#include <mutex>
#include <regex>

namespace lattice::ui
{

using control::generic::GenericMidiDriver;
using control::generic::MidiSender;
using control::generic::MidiSurfaceDriver;
using control::push::PushDriver;
using control::generic::SurfaceDefinition;

// --- The system's ports -----------------------------------------------------------------------------

namespace
{
class PortSender final : public MidiSender
{
public:
    explicit PortSender(std::unique_ptr<juce::MidiOutput> o) : port(std::move(o)) {}
    void send(const juce::MidiMessage& m) override { port->sendMessageNow(m); }

private:
    std::unique_ptr<juce::MidiOutput> port;
};

class SystemPorts final : public MidiPorts
{
public:
    explicit SystemPorts(AudioEngine& e) : engine(e)
    {
        connection = juce::MidiDeviceListConnection::make([this] {
            if (onChange)
                onChange();
        });
    }
    juce::Array<juce::MidiDeviceInfo> inputs() const override { return juce::MidiInput::getAvailableDevices(); }
    juce::Array<juce::MidiDeviceInfo> outputs() const override { return juce::MidiOutput::getAvailableDevices(); }
    std::unique_ptr<MidiSender> openOutput(const juce::String& identifier) override
    {
        if (auto o = juce::MidiOutput::openDevice(identifier))
            return std::make_unique<PortSender>(std::move(o));
        return nullptr;
    }
    void setInputOpen(const juce::String& identifier, bool open) override
    {
        if (!engine.isOpening())
            engine.getDeviceManager().setMidiInputDeviceEnabled(identifier, open);
    }
    bool isInputOpen(const juce::String& identifier) const override
    {
        return !engine.isOpening() && engine.getDeviceManager().isMidiInputDeviceEnabled(identifier);
    }

private:
    AudioEngine& engine;
    juce::MidiDeviceListConnection connection;
};

bool matches(const juce::StringArray& patterns, const juce::String& name)
{
    for (const auto& p : patterns)
    {
        try
        {
            if (std::regex_search(name.toStdString(), std::regex(p.toStdString(), std::regex::icase)))
                return true;
        }
        catch (const std::regex_error&)
        {
        }
    }
    return false;
}

/** How alike two port names are: the same name most, then the longest shared run. */
int likeness(const juce::String& a, const juce::String& b)
{
    if (a == b)
        return 100000;
    int best = 0;
    for (int i = 0; i < a.length(); ++i)
        for (int j = 0; j < b.length(); ++j)
        {
            int n = 0;
            while (i + n < a.length() && j + n < b.length() && a[i + n] == b[j + n])
                ++n;
            best = std::max(best, n);
        }
    return best;
}

juce::String idOf(const juce::Array<juce::MidiDeviceInfo>& devices, const juce::String& name)
{
    for (const auto& d : devices)
        if (d.name == name)
            return d.identifier;
    return {};
}
} // namespace

std::unique_ptr<MidiPorts> MidiPorts::system(AudioEngine& engine)
{
    return std::make_unique<SystemPorts>(engine);
}

// --- Monitoring ---------------------------------------------------------------------------------------

/** What a surface port sent, from its MIDI thread to the message thread. */
struct SurfaceManager::MonitorQueue final : juce::AsyncUpdater
{
    std::mutex lock;
    std::vector<Seen> seen;
    std::function<void(const Seen&)> report; // message thread
    std::atomic<bool> on{false};

    void add(Seen s)
    {
        {
            const std::scoped_lock l(lock);
            if (seen.size() < 2048) // a flood can't grow it without end
                seen.push_back(std::move(s));
        }
        triggerAsyncUpdate();
    }
    void handleAsyncUpdate() override
    {
        std::vector<Seen> now;
        {
            const std::scoped_lock l(lock);
            now.swap(seen);
        }
        if (report)
            for (const auto& s : now)
                report(s);
    }
};

// --- A connected surface ------------------------------------------------------------------------------

struct SurfaceManager::Live final : AudioEngine::SurfaceSink
{
    int entry = -1;
    juce::String inputId;
    bool openedInput = false; // opened for the surface (not already open for the tracks or MIDI Map)
    std::unique_ptr<MidiSender> out;
    std::unique_ptr<MidiSurfaceDriver> driver;
    std::unique_ptr<control::SurfaceController> controller;
    std::shared_ptr<MonitorQueue> monitor;

    /** Quiet: no output port (a device without lights), or one that wouldn't open. */
    struct Nowhere final : MidiSender
    {
        void send(const juce::MidiMessage&) override {}
    };

    bool receive(const juce::MidiMessage& message, double seconds) override
    {
        const bool own = driver->receive(message, seconds);
        if (monitor->on.load(std::memory_order_relaxed))
            monitor->add({entry, message, own ? driver->describe(message) : juce::String()});
        return own;
    }
};

// --- The learn wizard's input -----------------------------------------------------------------------

struct SurfaceManager::Learning final : AudioEngine::SurfaceSink, juce::AsyncUpdater
{
    juce::String inputId;
    bool openedInput = false;
    std::function<void(const juce::MidiMessage&)> heard;
    std::mutex lock;
    std::vector<juce::MidiMessage> waiting;

    bool receive(const juce::MidiMessage& message, double) override
    {
        {
            const std::scoped_lock l(lock);
            if (waiting.size() < 4096)
                waiting.push_back(message);
        }
        triggerAsyncUpdate();
        return true; // all of it the wizard's
    }
    void handleAsyncUpdate() override
    {
        std::vector<juce::MidiMessage> now;
        {
            const std::scoped_lock l(lock);
            now.swap(waiting);
        }
        if (heard)
            for (const auto& m : now)
                heard(m);
    }
};

bool SurfaceManager::beginLearning(const juce::String& inputName, std::function<void(const juce::MidiMessage&)> heard)
{
    endLearning();
    const auto id = idOf(ports.inputs(), inputName);
    if (id.isEmpty() || engine.isSurfaceInput(id))
        return false;
    learning = std::make_unique<Learning>();
    learning->inputId = id;
    learning->heard = std::move(heard);
    engine.setSurfaceSink(id, learning.get(), false);
    learning->openedInput = !ports.isInputOpen(id);
    if (learning->openedInput)
        ports.setInputOpen(id, true);
    return true;
}

void SurfaceManager::endLearning()
{
    if (learning == nullptr)
        return;
    engine.setSurfaceSink(learning->inputId, nullptr, false);
    if (learning->openedInput)
        ports.setInputOpen(learning->inputId, false);
    learning->cancelPendingUpdate();
    learning.reset();
}

void SurfaceManager::deliverLearning()
{
    if (learning != nullptr)
        learning->handleUpdateNowIfNeeded();
}

std::unique_ptr<MidiSender> SurfaceManager::openOutput(const juce::String& outputName)
{
    const auto id = idOf(ports.outputs(), outputName);
    return id.isNotEmpty() ? ports.openOutput(id) : nullptr;
}

juce::StringArray SurfaceManager::inputNames() const
{
    juce::StringArray out;
    for (const auto& d : ports.inputs())
        out.add(d.name);
    return out;
}

juce::StringArray SurfaceManager::outputNames() const
{
    juce::StringArray out;
    for (const auto& d : ports.outputs())
        out.add(d.name);
    return out;
}

// --- The manager ----------------------------------------------------------------------------------

SurfaceManager::SurfaceManager(AudioEngine& e, juce::PropertiesFile& s, MidiPorts& p, std::function<control::SurfaceContext()> c,
                               juce::File userFolder)
    : engine(e), settings(s), ports(p), context(std::move(c)), folder(std::move(userFolder)), monitorQueue(std::make_shared<MonitorQueue>())
{
    if (const auto xml = settings.getXmlValue(settingsKey))
        for (const auto* el : xml->getChildIterator())
            list.push_back({el->getStringAttribute("definition"), el->getStringAttribute("input"), el->getStringAttribute("output"),
                            el->getBoolAttribute("notesToTracks"), el->getBoolAttribute("followsSelection", true)});
    live.resize(list.size());
    loadDefinitions();
    // The surfaces whose ports are there; offers wait for refreshPorts() (onOffer set by then).
    for (int i = 0; i < static_cast<int>(list.size()); ++i)
        connect(i);
    ports.onChange = [this] { refreshPorts(); };
}

SurfaceManager::~SurfaceManager()
{
    endLearning();
    ports.onChange = nullptr;
    for (int i = 0; i < static_cast<int>(live.size()); ++i)
        disconnect(i);
    monitorQueue->cancelPendingUpdate();
}

const SurfaceDefinition* SurfaceManager::definition(const juce::String& name) const
{
    for (const auto& d : all)
        if (d.name == name)
            return &d;
    return nullptr;
}

void SurfaceManager::reloadDefinitions()
{
    for (int i = 0; i < static_cast<int>(live.size()); ++i)
        disconnect(i);
    loadDefinitions();
    refreshPorts();
}

void SurfaceManager::loadDefinitions()
{
    all = SurfaceDefinition::included();
    broken.clear();
    if (folder.isDirectory())
        for (const auto& f : folder.findChildFiles(juce::File::findFiles, false, "*.surface.json"))
        {
            SurfaceDefinition d;
            if (const auto r = SurfaceDefinition::parse(f.loadFileAsString(), d); r.failed())
            {
                broken.add(f.getFileName() + ": " + r.getErrorMessage());
                continue;
            }
            // A user's file of the same name replaces the included one.
            all.erase(std::remove_if(all.begin(), all.end(), [&](const auto& x) { return x.name == d.name; }), all.end());
            all.push_back(std::move(d));
        }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
}

SurfaceManager::Offer SurfaceManager::portsFor(const SurfaceDefinition& d) const
{
    if (d.isTemplate)
        return {};
    Offer o;
    o.definition = d.name;
    for (const auto& in : ports.inputs())
    {
        const bool taken = std::any_of(list.begin(), list.end(), [&](const Entry& e) { return e.input == in.name; });
        if (!taken && matches(d.inputPorts, in.name))
        {
            o.input = in.name;
            break;
        }
    }
    if (o.input.isEmpty())
        return {};
    int best = -1;
    for (const auto& out : ports.outputs())
        if (matches(d.outputPorts, out.name))
            if (const int l = likeness(out.name, o.input); l > best)
            {
                best = l;
                o.output = out.name;
            }
    return o;
}

int SurfaceManager::add(const Entry& entry)
{
    list.push_back(entry);
    live.emplace_back();
    save();
    const int index = static_cast<int>(list.size()) - 1;
    connect(index);
    changed();
    return index;
}

void SurfaceManager::remove(int index)
{
    if (index < 0 || index >= static_cast<int>(list.size()))
        return;
    disconnect(index);
    list.erase(list.begin() + index);
    live.erase(live.begin() + index);
    for (int i = 0; i < static_cast<int>(live.size()); ++i)
        if (live[static_cast<std::size_t>(i)] != nullptr)
            live[static_cast<std::size_t>(i)]->entry = i;
    save();
    changed();
}

void SurfaceManager::setNotesToTracks(int index, bool on)
{
    if (index < 0 || index >= static_cast<int>(list.size()))
        return;
    list[static_cast<std::size_t>(index)].notesToTracks = on;
    save();
    if (const auto& l = live[static_cast<std::size_t>(index)])
        engine.setSurfaceSink(l->inputId, l.get(), on);
    changed();
}

void SurfaceManager::setFollowsSelection(int index, bool on)
{
    if (index < 0 || index >= static_cast<int>(list.size()))
        return;
    list[static_cast<std::size_t>(index)].followsSelection = on;
    save();
    if (const auto& l = live[static_cast<std::size_t>(index)])
        l->controller->setFollowsSelection(on);
    changed();
}

bool SurfaceManager::isConnected(int index) const
{
    return index >= 0 && index < static_cast<int>(live.size()) && live[static_cast<std::size_t>(index)] != nullptr;
}

control::SurfaceController* SurfaceManager::controller(int index) const
{
    return isConnected(index) ? live[static_cast<std::size_t>(index)]->controller.get() : nullptr;
}

MidiSurfaceDriver* SurfaceManager::driver(int index) const
{
    return isConnected(index) ? live[static_cast<std::size_t>(index)]->driver.get() : nullptr;
}

juce::String SurfaceManager::status(int index) const
{
    const auto* d = driver(index);
    return d != nullptr ? d->status() : juce::String();
}

void SurfaceManager::connect(int index)
{
    if (isConnected(index))
        return;
    const auto& e = list[static_cast<std::size_t>(index)];
    const auto* d = definition(e.definition);
    const auto inputs = ports.inputs();
    const auto inputId = idOf(inputs, e.input);
    if (d == nullptr || inputId.isEmpty() || engine.isSurfaceInput(inputId))
        return;
    auto l = std::make_unique<Live>();
    l->entry = index;
    l->inputId = inputId;
    l->monitor = monitorQueue;
    if (const auto outputId = idOf(ports.outputs(), e.output); outputId.isNotEmpty())
        l->out = ports.openOutput(outputId);
    if (l->out == nullptr)
        l->out = std::make_unique<Live::Nowhere>();
    if (d->driver.isEmpty())
        l->driver = std::make_unique<GenericMidiDriver>(*d, *l->out);
    else
    {
        const bool push3 = d->driver == "push3";
        const int product = push3 ? control::push::push3ProductId : control::push::push2ProductId;
        control::push::LinkOpener opener;
        if (displayOpener)
            opener = [open = displayOpener, product] { return open(product); };
        auto push = std::make_unique<PushDriver>(push3 ? PushDriver::Model::push3 : PushDriver::Model::push2, *l->out, std::move(opener),
                                                 [this] { return control::animationBeat(context().feed); });
        if (theme)
            push->setTheme(*theme);
        l->driver = std::move(push);
    }
    l->driver->connect();
    l->controller = std::make_unique<control::SurfaceController>(context(), *l->driver);
    if (colours)
        l->controller->setColours(*colours);
    if (expression)
        l->controller->setExpression(*expression);
    l->controller->setFollowsSelection(e.followsSelection);
    engine.setSurfaceSink(inputId, l.get(), e.notesToTracks);
    l->openedInput = !ports.isInputOpen(inputId);
    if (l->openedInput)
        ports.setInputOpen(inputId, true);
    live[static_cast<std::size_t>(index)] = std::move(l);
}

void SurfaceManager::disconnect(int index)
{
    if (!isConnected(index))
        return;
    auto l = std::move(live[static_cast<std::size_t>(index)]);
    // No MIDI thread in the sink past this; then the controller before the driver it drives.
    engine.setSurfaceSink(l->inputId, nullptr, false);
    if (l->openedInput)
        ports.setInputOpen(l->inputId, false);
    l->controller.reset();
    l->driver->disconnect();
}

void SurfaceManager::refreshPorts()
{
    const auto inputs = ports.inputs();
    bool any = false;
    for (int i = 0; i < static_cast<int>(list.size()); ++i)
    {
        const bool there = idOf(inputs, list[static_cast<std::size_t>(i)].input).isNotEmpty();
        if (there && !isConnected(i))
        {
            connect(i);
            any = any || isConnected(i);
        }
        else if (there && isConnected(i))
        {
            // The audio device may have opened since (it opens the saved inputs): keep ours open.
            auto& l = live[static_cast<std::size_t>(i)];
            if (!ports.isInputOpen(l->inputId))
            {
                ports.setInputOpen(l->inputId, true);
                l->openedInput = true;
            }
        }
        else if (!there && isConnected(i))
        {
            // Gone: its port can't be written to; let the driver go without sending.
            auto& l = live[static_cast<std::size_t>(i)];
            engine.setSurfaceSink(l->inputId, nullptr, false);
            if (l->openedInput)
                ports.setInputOpen(l->inputId, false);
            l->controller.reset();
            l.reset();
            any = true;
        }
    }
    if (any)
        changed();

    // Recognised devices with no surface on them, offered once a session.
    const auto declined = juce::StringArray::fromLines(settings.getValue(declinedKey));
    for (const auto& d : all)
    {
        if (d.isTemplate || declined.contains(d.name))
            continue;
        const auto o = portsFor(d);
        if (o.input.isEmpty() || std::find(offered.begin(), offered.end(), o) != offered.end())
            continue;
        offered.push_back(o);
        if (onOffer)
            onOffer(o);
    }
}

void SurfaceManager::decline(const Offer& offer)
{
    auto declined = juce::StringArray::fromLines(settings.getValue(declinedKey));
    declined.addIfNotAlreadyThere(offer.definition);
    declined.removeEmptyStrings();
    settings.setValue(declinedKey, declined.joinIntoString("\n"));
}

void SurfaceManager::save() const
{
    juce::XmlElement xml("SURFACES");
    for (const auto& e : list)
    {
        auto* el = xml.createNewChildElement("SURFACE");
        el->setAttribute("definition", e.definition);
        el->setAttribute("input", e.input);
        el->setAttribute("output", e.output);
        el->setAttribute("notesToTracks", e.notesToTracks);
        el->setAttribute("followsSelection", e.followsSelection);
    }
    settings.setValue(settingsKey, &xml);
}

void SurfaceManager::setMonitor(std::function<void(const Seen&)> monitor)
{
    monitorQueue->report = std::move(monitor);
    monitorQueue->on = monitorQueue->report != nullptr;
}

void SurfaceManager::deliverMonitor()
{
    monitorQueue->handleUpdateNowIfNeeded();
}

void SurfaceManager::setColours(const control::SurfaceColours& c)
{
    colours = c;
    for (const auto& l : live)
        if (l != nullptr)
            l->controller->setColours(c);
}

void SurfaceManager::setTheme(const Theme& t)
{
    theme = t;
    for (const auto& l : live)
        if (l != nullptr)
            if (auto* push = dynamic_cast<PushDriver*>(l->driver.get()))
                push->setTheme(t);
}

void SurfaceManager::setExpression(const control::Expression& e)
{
    expression = e;
    for (const auto& l : live)
        if (l != nullptr)
            l->controller->setExpression(e);
}

} // namespace lattice::ui
