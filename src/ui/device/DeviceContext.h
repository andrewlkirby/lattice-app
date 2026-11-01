#pragma once

#include "core/JobPool.h"
#include "core/model/UndoNames.h"
#include "devices/framework/DeviceSpecs.h"
#include "devices/presets/Presets.h"
#include "engine/SessionController.h"
#include "ui/feed/EngineStateFeed.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace lattice::library
{
class PeakCache;
}

namespace lattice::ui
{

class DeviceView;

/** What is being dragged in and onto the Device view, as a juce::var drag description (the browser
    in 4e drags the same):
    - "device:<typeId>"   a new device
    - "preset:<which>"    a preset: an index into PresetLibrary::all(), "factory:<resource>" for a
                          factory preset, or a .lpreset file's path (the browser's items)
    - "sample:<path>"     a sample file (also files dragged in from the OS)
    - "move:<deviceId>"   a device already in the set
    - "pad:<note>"        a Drum Rack pad, dragged to another pad
    - "tuning:<which>"    a tuning (the browser): a TuningRegistry ID or a tuning file's path,
                          dropped on a track header to set its tuning */
namespace drag
{
inline juce::String device(const juce::String& typeId) { return "device:" + typeId; }
inline juce::String preset(int index) { return "preset:" + juce::String(index); }
inline juce::String sample(const juce::File& f) { return "sample:" + f.getFullPathName(); }
inline juce::String move(std::int64_t deviceId) { return "move:" + juce::String(deviceId); }
inline juce::String pad(int note) { return "pad:" + juce::String(note); }
inline juce::String presetKey(const juce::String& key) { return "preset:" + key; }
inline juce::String tuning(const juce::String& idOrPath) { return "tuning:" + idOrPath; }

inline bool is(const juce::var& description, const char* kind)
{
    return description.toString().startsWith(juce::String(kind) + ":");
}
inline juce::String payload(const juce::var& description)
{
    return description.toString().fromFirstOccurrenceOf(":", false, false);
}
} // namespace drag

/** Audio file types the Sampler loads. */
inline bool isSampleFile(const juce::File& f)
{
    return f.hasFileExtension("wav;aif;aiff;flac;ogg;mp3");
}
inline constexpr const char* sampleFilePatterns = "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3";

/** What the Device view and its panels share (docs/05-m4-ui.md, 4d): the session they edit, the
    feed they read live state from, the device descriptions, the presets and tunings, the job pool
    for disk work, and the dialogs the window provides. Message thread. */
struct DeviceContext
{
    SessionController& session;
    EngineStateFeed& feed;
    DeviceSpecs& specs;
    presets::PresetLibrary& presets;
    TuningRegistry& tunings;
    JobPool& jobs;
    /** Asks for a file with the OS chooser; calls chosen with it. */
    std::function<void(const juce::String& title, const juce::String& patterns, std::function<void(const juce::File&)> chosen)>
        chooseFile = {};
    /** Asks a question with a yes and a no; calls yes if the answer is yes. With no yesText it's a
        message with an OK button and yes is never called. (Tests answer at once.) */
    std::function<void(const juce::String& title, const juce::String& question, const juce::String& yesText,
                       std::function<void()> yes)>
        confirm = {};
    /** Called after a preset was saved (lists of presets read the library again). */
    std::function<void()> presetsChanged = {};
    /** Waveform peaks kept on disk (none in tests that don't ask). */
    const library::PeakCache* peaks = nullptr;
    /** The view the panels are in: the selection, the inserting point, hot-swap and map mode. */
    DeviceView* view = nullptr;

    /** Lives as long as the context: work that finishes later (a sample read in the background)
        holds a weak pointer to it, and does nothing once it's gone. */
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    LiveSet& set() const { return session.set(); }
    juce::UndoManager& undo() const { return session.undoManager(); }
    /** Begins an undo step named for what's done and where `where` is (SessionController::beginEdit). */
    void beginEdit(const juce::String& action, const juce::ValueTree& where) const { session.beginEdit(action, where); }
    const DeviceRegistry& registry() const { return specs.devices(); }

    /** The device's name: the user's, or its type's ("Synth"). */
    juce::String nameOf(const juce::ValueTree& device) const { return undoNames::deviceName(device, registry()); }

    /** The track (or return, or master) a device is on, as the info view and Find name it. */
    juce::String ownerNameOf(const juce::ValueTree& device) const
    {
        const auto owner = LiveSet::chainOwnerOf(device);
        if (owner.hasType(ids::master))
            return "Master";
        return owner[ids::name].toString();
    }
};

} // namespace lattice::ui
