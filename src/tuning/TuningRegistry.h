#pragma once

#include "tuning/TuningTable.h"

#include <vector>

namespace lattice
{

/** All tunings the app knows: the built-ins (12-TET, Colundi) plus files the user loads or keeps in
    <userdata>/Tunings. Message thread only. Listeners hear about every change (the hook M6's
    MTS-ESP master uses). */
class TuningRegistry
{
public:
    static constexpr const char* colundiId = "builtin.colundi";

    /** Colundi's repeat interval: entry 119 is exactly 30 Hz x 938.667 (28160 Hz), and the table's
        Name column wraps back to its start there. Used by the FoldDown high-note policy. */
    static constexpr double colundiPeriodRatio = 28160.0 / 30.0;

    struct Listener
    {
        virtual ~Listener() = default;
        virtual void tuningsChanged() = 0;
    };

    TuningRegistry();

    const std::vector<TuningPtr>& getAll() const noexcept { return tunings; }
    /** Any tuning by its ID, also one a newer version of its file has replaced in the list (sets
        and undo steps may still name it). */
    TuningPtr findById(const juce::String& id) const;
    TuningPtr defaultTuning() const; // 12-TET A440

    /** Loads a .scl (with a .kbm of the same name next to it, if there is one) or a Colundi-format
        .txt file and adds it. Loading the same content again returns the existing entry. A file
        loaded before with other content (the tuning editor saved over it) takes the older
        version's place in the list; the older one is still found by ID. */
    TuningLoadResult loadFile(const juce::File& file);

    /** Loads every .scl and .txt file in a folder. Returns the number added; errors are appended
        as "file: message". */
    int loadFolder(const juce::File& folder, juce::StringArray* errors = nullptr);

    void addListener(Listener* l) { listeners.add(l); }
    void removeListener(Listener* l) { listeners.remove(l); }

    /** The built-in Colundi table, parsed from the copy embedded in the app. */
    static TuningPtr makeBuiltinColundi();

private:
    void add(TuningPtr tuning);

    std::vector<TuningPtr> tunings;
    std::vector<TuningPtr> replaced; // older versions of files in the list
    juce::ListenerList<Listener> listeners;
};

} // namespace lattice
