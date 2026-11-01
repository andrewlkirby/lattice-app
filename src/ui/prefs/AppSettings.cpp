#include "ui/prefs/AppSettings.h"

#include "core/model/LiveSet.h"
#include "engine/WorkerPool.h"
#include "tuning/TuningTable.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lattice::ui::prefs
{

double zoom(const juce::PropertiesFile& settings)
{
    return juce::jlimit(minZoom, maxZoom, settings.getDoubleValue(zoomKey, 1.0));
}

void applyZoom(double z)
{
    juce::Desktop::getInstance().setGlobalScaleFactor(static_cast<float>(juce::jlimit(minZoom, maxZoom, z)));
}

int workerThreads(const juce::PropertiesFile& settings)
{
    if (!settings.getBoolValue(multiThreadedKey, true))
        return 0;
    const int chosen = settings.getIntValue(workersKey, 0);
    return chosen > 0 ? chosen : WorkerPool::defaultWorkers();
}

void applyNewTrackDefaults(const juce::PropertiesFile& settings)
{
    auto& defaults = LiveSet::newTrackDefaults();
    defaults.tuningId = settings.getValue(defaultTuningKey);
    defaults.highNotePolicy = juce::jlimit(0, 1, settings.getIntValue(defaultHighNotesKey, 0));
}

void restoreShortcuts(const juce::PropertiesFile& settings, CommandRegistry& registry)
{
    if (const auto xml = settings.getXmlValue(shortcutsKey))
        registry.restoreShortcuts(*xml);
}

void saveShortcuts(juce::PropertiesFile& settings, const CommandRegistry& registry)
{
    if (const auto xml = registry.shortcutsToXml())
        settings.setValue(shortcutsKey, xml.get());
}

void applyKeyboard(const juce::PropertiesFile& settings, ComputerKeyboardInput& keyboard)
{
    const auto keys = settings.getValue(noteKeysKey, ComputerKeyboardInput::noteKeys);
    if (!keyboard.setNoteKeys(keys))
        keyboard.setNoteKeys(ComputerKeyboardInput::noteKeys);
    keyboard.pageSizeFor = [&settings](const TuningTable& tuning) { return pageSizeFor(settings, tuning.id); };
}

int pageSizeFor(const juce::PropertiesFile& settings, const juce::String& tuningId)
{
    return juce::jlimit(0, TuningTable::numNotes, settings.getIntValue(pageKeyPrefix + tuningId, 0));
}

juce::StringArray remoteInputs(const juce::PropertiesFile& settings)
{
    auto all = juce::StringArray::fromLines(settings.getValue(remoteInputsKey));
    all.removeEmptyStrings();
    return all;
}

void setRemoteInput(juce::PropertiesFile& settings, const juce::String& identifier, bool on)
{
    auto all = remoteInputs(settings);
    all.removeString(identifier);
    if (on)
        all.add(identifier);
    settings.setValue(remoteInputsKey, all.joinIntoString("\n"));
}

} // namespace lattice::ui::prefs
