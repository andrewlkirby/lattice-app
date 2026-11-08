#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace lattice::mts
{

/** ODDSound's MTS-ESP library (libMTS, docs/07-m6-plugins-external.md, 6c, "MTS-ESP master"): the
    shared library through which one tuning master and its client plugins meet, in this process and
    (through its own IPC) in others. It comes with ODDSound's free MTS-ESP Mini, which installs it
    where every master and client looks for it (systemPath()).

    Loaded here rather than through ODDSound's Master/libMTSMaster.cpp, which can only look in that
    one place, so the tests can use the SDK's own copy. The functions are libMTS's, called as that
    file calls them (its SDK is 0BSD-licensed). A library once loaded stays loaded: clients hold
    pointers into it. Message thread, like libMTS itself. */
class Library
{
public:
    /** Where MTS-ESP's installer puts the library on this OS. */
    static juce::File systemPath();
    /** The library at that file, loaded once per process (null if it isn't there or isn't libMTS). */
    static Library* at(const juce::File& file);

    int version() const noexcept { return getVersion != nullptr ? getVersion() : 0; }

    // As a master.
    bool hasMaster() const noexcept { return fHasMaster != nullptr && fHasMaster(); }
    bool hasIpc() const noexcept { return fHasIpc != nullptr && fHasIpc(); }
    void registerMaster() const noexcept;
    void deregisterMaster() const noexcept;
    void reinitialize() const noexcept;
    int numClients() const noexcept { return fNumClients != nullptr ? fNumClients() : 0; }
    void setNoteTunings(const double* hz) const noexcept;
    void setScaleName(const char* name) const noexcept;
    void setPeriodRatio(double ratio) const noexcept;
    void filterNote(bool filter, int note, int channel) const noexcept;
    void clearNoteFilter() const noexcept;
    void setMultiChannel(bool on, int channel) const noexcept;
    void setMultiChannelNoteTunings(const double* hz, int channel) const noexcept;
    void filterNoteMultiChannel(bool filter, int note, int channel) const noexcept;
    void clearNoteFilterMultiChannel(int channel) const noexcept;

    // As a client (the tests' client reads what the master sent).
    void registerClient() const noexcept;
    void deregisterClient() const noexcept;
    const double* tuningTable() const noexcept { return fTuning != nullptr ? fTuning() : nullptr; }
    const double* multiChannelTuningTable(int channel) const noexcept;
    bool usesMultiChannelTuning(int channel) const noexcept;
    bool shouldFilterNote(int note, int channel) const noexcept;
    bool shouldFilterNoteMultiChannel(int note, int channel) const noexcept;
    juce::String scaleName() const;

private:
    explicit Library(const juce::File& file);
    bool complete() const noexcept;

    juce::DynamicLibrary library;
    // libMTS's exports (see libMTSMaster.cpp and libMTSClient.cpp).
    int (*getVersion)() = nullptr;
    void (*fRegisterMaster)(void*) = nullptr;
    void (*fDeregisterMaster)() = nullptr;
    void (*fReinitialize)() = nullptr;
    bool (*fHasMaster)() = nullptr;
    bool (*fHasIpc)() = nullptr;
    int (*fNumClients)() = nullptr;
    void (*fSetNoteTunings)(const double*) = nullptr;
    void (*fSetScaleName)(const char*) = nullptr;
    void (*fSetPeriodRatio)(double) = nullptr;
    void (*fFilterNote)(bool, char, signed char) = nullptr;
    void (*fClearNoteFilter)() = nullptr;
    void (*fSetMultiChannel)(bool, signed char) = nullptr;
    void (*fSetMultiChannelNoteTunings)(const double*, signed char) = nullptr;
    void (*fFilterNoteMultiChannel)(bool, char, signed char) = nullptr;
    void (*fClearNoteFilterMultiChannel)(signed char) = nullptr;
    void (*fRegisterClient)() = nullptr;
    void (*fDeregisterClient)() = nullptr;
    const double* (*fTuning)() = nullptr;
    const double* (*fMultiChannelTuning)(signed char) = nullptr;
    bool (*fUseMultiChannelTuning)(signed char) = nullptr;
    bool (*fShouldFilterNote)(char, signed char) = nullptr;
    bool (*fShouldFilterNoteMultiChannel)(char, signed char) = nullptr;
    const char* (*fScaleName)() = nullptr;

    JUCE_DECLARE_NON_COPYABLE(Library)
};

/** A tuning a master sends: each note's frequency (0: filtered, so clients skip the note), its name
    and its repeat interval. */
struct Tuning
{
    std::array<double, 128> hz{};
    juce::String name;
    double periodRatio = 2.0;

    bool operator==(const Tuning&) const = default;
};

/** Lattice as the MTS-ESP master (docs/07-m6-plugins-external.md, 6c):
    - It registers when it's on and the library is installed, unless another program already is
      the master: there can only be one, and that one is left alone (retry() tries again later).
    - send() gets the tunings the tracks with plugins use, one per track:
      - All the same: the master table is that tuning, on every channel.
      - Different: multi-channel mode. Each different tuning gets a MIDI channel (0..15) and that
        channel's table; a tuning keeps its channel while it stays in use, so held notes don't
        move. The master table is the first tuning's, for clients that don't ask by channel.
        Beyond 16 different tunings, the rest get no channel (and play the first one's).
    - Notes a tuning doesn't play (unmapped, or out of range) are filtered.
    Message thread. */
class Master
{
public:
    static constexpr int maxChannels = 16;

    enum class Status
    {
        off,
        noLibrary,   // MTS-ESP isn't installed
        otherMaster, // another program is the master
        active
    };

    /** `library`: where libMTS is (the system's, or a test's copy). */
    explicit Master(juce::File library = Library::systemPath());
    ~Master();

    void setEnabled(bool on);
    bool isEnabled() const noexcept { return enabled; }
    Status status() const noexcept { return state; }
    /** The status for a person, with what to do about it. */
    juce::String statusText() const;
    /** Plugins connected (as libMTS counts them). */
    int clients() const noexcept;
    /** Tries again to become the master, if it's on and isn't: another master may have gone, or
        MTS-ESP been installed since. */
    void retry();
    /** Resets libMTS's shared state, for when a master closed without letting go (it crashed):
        only to be offered while another master seems to be there. Then registers. */
    void reset();

    /** The tunings in use, one per track; returns each one's channel (-1: none, the master table). */
    std::vector<int> send(const std::vector<Tuning>& tunings);
    /** Different tunings that got no channel at the last send() (over 16). */
    int withoutChannel() const noexcept { return overflow; }
    /** The master table last sent. */
    const Tuning& masterTuning() const noexcept { return global; }

    /** Called when the status or the number of clients may have changed. */
    std::function<void()> onStatusChanged;

private:
    void becomeMaster();
    void letGo();
    void sendGlobal(const Tuning& t);
    void sendChannel(int channel, const Tuning& t);
    void notifyIfChanged();

    juce::File libraryFile;
    Library* library = nullptr;
    bool enabled = false, registered = false;
    Status state = Status::off;
    Status notifiedState = Status::off;

    Tuning global;
    bool globalSent = false;
    struct Channel
    {
        bool used = false;
        Tuning tuning;
    };
    std::array<Channel, maxChannels> channels{};
    int overflow = 0;
};

} // namespace lattice::mts
