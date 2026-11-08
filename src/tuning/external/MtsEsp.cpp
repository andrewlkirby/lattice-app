#include "tuning/external/MtsEsp.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace lattice::mts
{

namespace
{
/** What a client gets for a note the master filters: 12-TET's pitch (it shouldn't play it). */
double fallbackHz(int note)
{
    return 440.0 * std::pow(2.0, (note - 69) / 12.0);
}

std::array<double, 128> sendable(const Tuning& t)
{
    auto hz = t.hz;
    for (int n = 0; n < 128; ++n)
        if (!(hz[static_cast<std::size_t>(n)] > 0.0))
            hz[static_cast<std::size_t>(n)] = fallbackHz(n);
    return hz;
}

template <typename F>
void bind(juce::DynamicLibrary& library, F& function, const char* name)
{
    function = reinterpret_cast<F>(library.getFunction(name));
}
} // namespace

// --- Library -------------------------------------------------------------------------------------------

juce::File Library::systemPath()
{
#if JUCE_WINDOWS
    // Where ODDSound's code looks: the Common Files folder (FOLDERID_ProgramFilesCommon).
    const auto common = juce::SystemStats::getEnvironmentVariable("CommonProgramFiles", {});
    const auto folder = common.isNotEmpty() ? juce::File(common)
                                            : juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory).getChildFile("Common Files");
    return folder.getChildFile("MTS-ESP").getChildFile("LIBMTS.dll");
#elif JUCE_MAC
    return juce::File("/Library/Application Support/MTS-ESP/libMTS.dylib");
#else
    return juce::File("/usr/local/lib/libMTS.so");
#endif
}

Library* Library::at(const juce::File& file)
{
    // Never unloaded: client plugins keep pointers into it for as long as they live.
    static auto* libraries = new std::map<juce::String, std::unique_ptr<Library>>();
    const auto key = file.getFullPathName();
    if (const auto it = libraries->find(key); it != libraries->end())
        return it->second.get();
    if (!file.existsAsFile())
        return nullptr; // not cached: it may be installed later
    std::unique_ptr<Library> library(new Library(file));
    auto* result = library->complete() ? library.get() : nullptr;
    (*libraries)[key] = result != nullptr ? std::move(library) : nullptr;
    return result;
}

Library::Library(const juce::File& file)
{
    if (!library.open(file.getFullPathName()))
        return;
    bind(library, getVersion, "MTS_GetVersionNumber");
    bind(library, fRegisterMaster, "MTS_RegisterMaster");
    bind(library, fDeregisterMaster, "MTS_DeregisterMaster");
    bind(library, fReinitialize, "MTS_Reinitialize");
    bind(library, fHasMaster, "MTS_HasMaster");
    bind(library, fHasIpc, "MTS_HasIPC");
    bind(library, fNumClients, "MTS_GetNumClients");
    bind(library, fSetNoteTunings, "MTS_SetNoteTunings");
    bind(library, fSetScaleName, "MTS_SetScaleName");
    bind(library, fSetPeriodRatio, "MTS_SetPeriodRatio");
    bind(library, fFilterNote, "MTS_FilterNote");
    bind(library, fClearNoteFilter, "MTS_ClearNoteFilter");
    bind(library, fSetMultiChannel, "MTS_SetMultiChannel");
    bind(library, fSetMultiChannelNoteTunings, "MTS_SetMultiChannelNoteTunings");
    bind(library, fFilterNoteMultiChannel, "MTS_FilterNoteMultiChannel");
    bind(library, fClearNoteFilterMultiChannel, "MTS_ClearNoteFilterMultiChannel");
    bind(library, fRegisterClient, "MTS_RegisterClient");
    bind(library, fDeregisterClient, "MTS_DeregisterClient");
    bind(library, fTuning, "MTS_GetTuningTable");
    bind(library, fMultiChannelTuning, "MTS_GetMultiChannelTuningTable");
    bind(library, fUseMultiChannelTuning, "MTS_UseMultiChannelTuning");
    bind(library, fShouldFilterNote, "MTS_ShouldFilterNote");
    bind(library, fShouldFilterNoteMultiChannel, "MTS_ShouldFilterNoteMultiChannel");
    bind(library, fScaleName, "MTS_GetScaleName");
}

bool Library::complete() const noexcept
{
    // What a master needs. The rest (a newer library's, or a client's) is checked where it's used.
    return fRegisterMaster != nullptr && fDeregisterMaster != nullptr && fHasMaster != nullptr && fSetNoteTunings != nullptr &&
           fFilterNote != nullptr && fClearNoteFilter != nullptr;
}

void Library::registerMaster() const noexcept
{
    if (fRegisterMaster != nullptr)
        fRegisterMaster(nullptr);
}
void Library::deregisterMaster() const noexcept
{
    if (fDeregisterMaster != nullptr)
        fDeregisterMaster();
}
void Library::reinitialize() const noexcept
{
    if (fReinitialize != nullptr)
        fReinitialize();
}
void Library::setNoteTunings(const double* hz) const noexcept
{
    if (fSetNoteTunings != nullptr)
        fSetNoteTunings(hz);
}
void Library::setScaleName(const char* name) const noexcept
{
    if (fSetScaleName != nullptr)
        fSetScaleName(name);
}
void Library::setPeriodRatio(double ratio) const noexcept
{
    if (fSetPeriodRatio != nullptr)
        fSetPeriodRatio(ratio);
}
void Library::filterNote(bool filter, int note, int channel) const noexcept
{
    if (fFilterNote != nullptr)
        fFilterNote(filter, static_cast<char>(note), static_cast<signed char>(channel));
}
void Library::clearNoteFilter() const noexcept
{
    if (fClearNoteFilter != nullptr)
        fClearNoteFilter();
}
void Library::setMultiChannel(bool on, int channel) const noexcept
{
    if (fSetMultiChannel != nullptr)
        fSetMultiChannel(on, static_cast<signed char>(channel));
}
void Library::setMultiChannelNoteTunings(const double* hz, int channel) const noexcept
{
    if (fSetMultiChannelNoteTunings != nullptr)
        fSetMultiChannelNoteTunings(hz, static_cast<signed char>(channel));
}
void Library::filterNoteMultiChannel(bool filter, int note, int channel) const noexcept
{
    if (fFilterNoteMultiChannel != nullptr)
        fFilterNoteMultiChannel(filter, static_cast<char>(note), static_cast<signed char>(channel));
}
void Library::clearNoteFilterMultiChannel(int channel) const noexcept
{
    if (fClearNoteFilterMultiChannel != nullptr)
        fClearNoteFilterMultiChannel(static_cast<signed char>(channel));
}
void Library::registerClient() const noexcept
{
    if (fRegisterClient != nullptr)
        fRegisterClient();
}
void Library::deregisterClient() const noexcept
{
    if (fDeregisterClient != nullptr)
        fDeregisterClient();
}
const double* Library::multiChannelTuningTable(int channel) const noexcept
{
    return fMultiChannelTuning != nullptr ? fMultiChannelTuning(static_cast<signed char>(channel)) : nullptr;
}
bool Library::usesMultiChannelTuning(int channel) const noexcept
{
    return fUseMultiChannelTuning != nullptr && fUseMultiChannelTuning(static_cast<signed char>(channel));
}
bool Library::shouldFilterNote(int note, int channel) const noexcept
{
    return fShouldFilterNote != nullptr && fShouldFilterNote(static_cast<char>(note), static_cast<signed char>(channel));
}
bool Library::shouldFilterNoteMultiChannel(int note, int channel) const noexcept
{
    return fShouldFilterNoteMultiChannel != nullptr &&
           fShouldFilterNoteMultiChannel(static_cast<char>(note), static_cast<signed char>(channel));
}
juce::String Library::scaleName() const
{
    const char* name = fScaleName != nullptr ? fScaleName() : nullptr;
    return name != nullptr ? juce::String::fromUTF8(name) : juce::String();
}

// --- Master --------------------------------------------------------------------------------------------

Master::Master(juce::File file) : libraryFile(std::move(file)) {}

Master::~Master()
{
    letGo();
}

void Master::setEnabled(bool on)
{
    if (enabled == on)
        return;
    enabled = on;
    if (on)
        becomeMaster();
    else
    {
        letGo();
        state = Status::off;
    }
    notifyIfChanged();
}

void Master::retry()
{
    if (enabled && !registered)
    {
        becomeMaster();
        notifyIfChanged();
    }
}

void Master::reset()
{
    if (!enabled || registered)
        return;
    if (library == nullptr)
        library = Library::at(libraryFile);
    if (library != nullptr)
        library->reinitialize();
    becomeMaster();
    notifyIfChanged();
}

int Master::clients() const noexcept
{
    return registered && library != nullptr ? library->numClients() : 0;
}

void Master::becomeMaster()
{
    if (registered)
        return;
    if (library == nullptr)
        library = Library::at(libraryFile);
    if (library == nullptr)
    {
        state = Status::noLibrary;
        return;
    }
    if (library->hasMaster())
    {
        state = Status::otherMaster; // there can only be one: it's left alone
        return;
    }
    library->registerMaster();
    registered = true;
    state = Status::active;
    // What was asked for before, now.
    if (globalSent)
        sendGlobal(global);
    for (int ch = 0; ch < maxChannels; ++ch)
        if (channels[static_cast<std::size_t>(ch)].used)
            sendChannel(ch, channels[static_cast<std::size_t>(ch)].tuning);
}

void Master::letGo()
{
    if (!registered)
        return;
    library->deregisterMaster(); // libMTS resets what clients see
    registered = false;
}

juce::String Master::statusText() const
{
    switch (state)
    {
    case Status::off: return "Off.";
    case Status::noLibrary:
        return "MTS-ESP isn't installed. Install ODDSound's free MTS-ESP Mini (oddsound.com), which puts its library at " +
               libraryFile.getFullPathName() + "; Lattice finds it there. Until then, export the tuning as a file.";
    case Status::otherMaster:
        return "Another program is the MTS-ESP master, so Lattice leaves it alone; plugins follow that one.";
    case Status::active:
    {
        const int n = clients();
        juce::String text = "Lattice is the MTS-ESP master: " + juce::String(n) + (n == 1 ? " plugin" : " plugins") + " connected.";
        if (overflow > 0)
            text << " " << overflow << (overflow == 1 ? " tuning" : " tunings")
                 << " beyond 16 play the first track's: MTS-ESP has 16 channels.";
        return text;
    }
    }
    return {};
}

void Master::notifyIfChanged()
{
    if (state != notifiedState)
    {
        notifiedState = state;
        if (onStatusChanged)
            onStatusChanged();
    }
}

std::vector<int> Master::send(const std::vector<Tuning>& tunings)
{
    std::vector<int> result(tunings.size(), -1);
    std::vector<std::size_t> distinctOf(tunings.size());
    std::vector<const Tuning*> distinct;
    for (std::size_t i = 0; i < tunings.size(); ++i)
    {
        std::size_t d = 0;
        while (d < distinct.size() && !(*distinct[d] == tunings[i]))
            ++d;
        if (d == distinct.size())
            distinct.push_back(&tunings[i]);
        distinctOf[i] = d;
    }
    const int before = overflow;
    overflow = 0;
    if (distinct.empty())
        return result;

    if (!globalSent || !(global == *distinct[0]))
    {
        global = *distinct[0];
        globalSent = true;
        if (registered)
            sendGlobal(global);
    }

    // One tuning: no channels.
    std::vector<int> channelOf(distinct.size(), -1);
    if (distinct.size() > 1)
    {
        // A tuning still in use keeps its channel.
        for (std::size_t d = 0; d < distinct.size(); ++d)
            for (int ch = 0; ch < maxChannels; ++ch)
                if (channels[static_cast<std::size_t>(ch)].used && channelOf[d] < 0 &&
                    std::find(channelOf.begin(), channelOf.end(), ch) == channelOf.end() &&
                    channels[static_cast<std::size_t>(ch)].tuning == *distinct[d])
                    channelOf[d] = ch;
    }
    for (int ch = 0; ch < maxChannels; ++ch)
        if (channels[static_cast<std::size_t>(ch)].used && std::find(channelOf.begin(), channelOf.end(), ch) == channelOf.end())
        {
            channels[static_cast<std::size_t>(ch)].used = false;
            if (registered)
            {
                library->setMultiChannel(false, ch);
                library->clearNoteFilterMultiChannel(ch);
            }
        }
    if (distinct.size() > 1)
        for (std::size_t d = 0; d < distinct.size(); ++d)
        {
            if (channelOf[d] >= 0)
                continue;
            int ch = 0;
            while (ch < maxChannels && channels[static_cast<std::size_t>(ch)].used)
                ++ch;
            if (ch == maxChannels)
            {
                ++overflow;
                continue;
            }
            channels[static_cast<std::size_t>(ch)] = {true, *distinct[d]};
            channelOf[d] = ch;
            if (registered)
                sendChannel(ch, *distinct[d]);
        }

    for (std::size_t i = 0; i < tunings.size(); ++i)
        result[i] = channelOf[distinctOf[i]];
    if (overflow != before && onStatusChanged)
        onStatusChanged();
    return result;
}

void Master::sendGlobal(const Tuning& t)
{
    const auto hz = sendable(t);
    library->setNoteTunings(hz.data());
    library->setScaleName(t.name.toRawUTF8());
    library->setPeriodRatio(t.periodRatio);
    library->clearNoteFilter();
    for (int n = 0; n < 128; ++n)
        if (!(t.hz[static_cast<std::size_t>(n)] > 0.0))
            library->filterNote(true, n, -1);
}

void Master::sendChannel(int channel, const Tuning& t)
{
    const auto hz = sendable(t);
    library->setMultiChannel(true, channel);
    library->setMultiChannelNoteTunings(hz.data(), channel);
    library->clearNoteFilterMultiChannel(channel);
    for (int n = 0; n < 128; ++n)
        if (!(t.hz[static_cast<std::size_t>(n)] > 0.0))
            library->filterNoteMultiChannel(true, n, channel);
}

} // namespace lattice::mts
