#pragma once

#include "control/generic/GenericMidiDriver.h"
#include "control/push/PushDisplay.h"
#include "control/surface/SurfaceController.h"
#include "engine/AudioEngine.h"
#include "ui/theme/Theme.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <memory>
#include <vector>

namespace lattice::ui
{

/** The MIDI ports, as the surface manager uses them: the system's, or a test's. */
class MidiPorts
{
public:
    virtual ~MidiPorts() = default;
    virtual juce::Array<juce::MidiDeviceInfo> inputs() const = 0;
    virtual juce::Array<juce::MidiDeviceInfo> outputs() const = 0;
    virtual std::unique_ptr<control::generic::MidiSender> openOutput(const juce::String& identifier) = 0;
    /** Opens an input for the engine's MIDI callback (or lets it go). */
    virtual void setInputOpen(const juce::String& identifier, bool open) = 0;
    virtual bool isInputOpen(const juce::String& identifier) const = 0;
    /** Called when devices come or go (message thread). */
    std::function<void()> onChange;

    /** The system's ports, through the engine's device manager. */
    static std::unique_ptr<MidiPorts> system(AudioEngine& engine);
};

/** The control surfaces on MIDI ports (docs/06-m5-controllers.md, 5e): which definition is on
    which ports (kept in the settings), each connected surface's driver and controller, and the
    devices recognised but not yet set up.

    - A surface's input is a "surface port": the engine hands its messages to the surface first;
      what the surface doesn't use goes to MIDI Map, and notes reach the armed tracks only with
      `notesToTracks` (on by default for a definition without a grid: a keyboard's keys).
    - Surfaces connect when their ports appear and let go when they go. A port is found by its
      name, as identifiers can change between sessions.
    - A device whose ports match a definition, with no surface on them, is offered (onOffer)
      once a session, unless declined for good.
    - Definitions: the included ones and the user's folder's (a user's file of the same name
      wins). A user's file that doesn't read is listed in problems(), with why.
    - A definition naming a native driver ("driver": "push2") gets that driver on its ports
      (docs/06-m5-controllers.md, 5f): Push's own pads, lights and display. */
class SurfaceManager
{
public:
    struct Entry
    {
        juce::String definition; // its name
        juce::String input, output; // port names
        bool notesToTracks = false;
        bool operator==(const Entry&) const = default;
    };
    struct Offer
    {
        juce::String definition, input, output;
        bool operator==(const Offer&) const = default;
    };

    SurfaceManager(AudioEngine& engine, juce::PropertiesFile& settings, MidiPorts& ports,
                   std::function<control::SurfaceContext()> context, juce::File userFolder);
    ~SurfaceManager();

    // --- Definitions --------------------------------------------------------------------------
    const std::vector<control::generic::SurfaceDefinition>& definitions() const noexcept { return all; }
    const control::generic::SurfaceDefinition* definition(const juce::String& name) const;
    /** User files that didn't read: "File.surface.json: buttons[2].role: no role 'x'". */
    const juce::StringArray& problems() const noexcept { return broken; }
    const juce::File& userFolder() const noexcept { return folder; }
    /** Reads the definitions again (after a user file changed) and reconnects. */
    void reloadDefinitions();

    // --- Surfaces -----------------------------------------------------------------------------
    const std::vector<Entry>& entries() const noexcept { return list; }
    /** A new surface (connected at once if its ports are there). Its index. */
    int add(const Entry& entry);
    void remove(int index);
    void setNotesToTracks(int index, bool on);
    bool isConnected(int index) const;
    control::SurfaceController* controller(int index) const;
    control::generic::MidiSurfaceDriver* driver(int index) const;
    /** What's wrong beside the MIDI (Push's display needing a driver), or empty. */
    juce::String status(int index) const;
    /** The ports a definition would use now: the first input matching it that no surface has,
        and the output named most like it. Empty if none is there. */
    Offer portsFor(const control::generic::SurfaceDefinition& d) const;

    /** Looks at the ports again: connects surfaces whose ports came, lets go of those whose ports
        went, and offers recognised devices. Called when devices change. */
    void refreshPorts();
    std::function<void(const Offer&)> onOffer;
    /** Never offers that definition again. */
    void decline(const Offer& offer);
    /** Called when entries or their connections change (the Preferences list). */
    std::function<void()> onChanged;

    // --- Surface Monitor ----------------------------------------------------------------------
    struct Seen
    {
        int entry = -1;
        juce::MidiMessage message;
        juce::String meaning; // the driver's describe(); empty: not the surface's (passed on)
    };
    /** While set, every message on a surface port is reported (message thread, soon after). */
    void setMonitor(std::function<void(const Seen&)> monitor);
    /** Reports what's waiting now (tests; otherwise it goes soon). */
    void deliverMonitor();

    // --- The learn wizard ------------------------------------------------------------------------
    /** Every message from an input goes to `heard` (message thread, soon after), and nowhere else,
        until endLearning(). False if the input isn't there or a surface uses it. */
    bool beginLearning(const juce::String& inputName, std::function<void(const juce::MidiMessage&)> heard);
    void endLearning();
    /** Reports what's waiting now (tests; otherwise it goes soon). */
    void deliverLearning();
    /** An output port by name, to try lights on (null if it isn't there). */
    std::unique_ptr<control::generic::MidiSender> openOutput(const juce::String& outputName);
    /** The ports there now. */
    juce::StringArray inputNames() const;
    juce::StringArray outputNames() const;

    /** How the surfaces' lights and pads look and play (as Virtual Push's). */
    void setColours(const control::SurfaceColours& c);
    void setExpression(const control::Expression& e);
    /** The theme Push's display is drawn in. */
    void setTheme(const Theme& t);
    /** How a native driver opens Push's display (by USB product ID): openUsbDisplay unless a test
        says otherwise. Applies to surfaces connected after. */
    void setDisplayOpener(std::function<control::push::OpenedLink(int productId)> opener) { displayOpener = std::move(opener); }

    static constexpr const char* settingsKey = "surfaces";
    static constexpr const char* declinedKey = "surfaces.declined";

private:
    struct Live;
    void loadDefinitions();
    void connect(int index);
    void disconnect(int index);
    void save() const;
    void changed() const
    {
        if (onChanged)
            onChanged();
    }

    AudioEngine& engine;
    juce::PropertiesFile& settings;
    MidiPorts& ports;
    std::function<control::SurfaceContext()> context;
    juce::File folder;
    std::vector<control::generic::SurfaceDefinition> all;
    juce::StringArray broken;
    std::vector<Entry> list;
    std::vector<std::unique_ptr<Live>> live; // by entry: connected, or null
    std::vector<Offer> offered;              // this session's
    std::optional<control::SurfaceColours> colours;
    std::optional<control::Expression> expression;
    std::optional<Theme> theme;
    std::function<control::push::OpenedLink(int productId)> displayOpener = control::push::openUsbDisplay;

    struct MonitorQueue;
    std::shared_ptr<MonitorQueue> monitorQueue;
    struct Learning;
    std::unique_ptr<Learning> learning;
};

} // namespace lattice::ui
