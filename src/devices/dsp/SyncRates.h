#pragma once

#include <juce_core/juce_core.h>

#include <array>

namespace lattice::dsp
{

/** Tempo-synced note values shared by LFOs, delays, the arpeggiator and filters. The order is part
    of saved sets (Choice parameters store the index): only ever append. */
struct SyncRates
{
    struct Rate
    {
        const char* name;
        double beats; // length in quarter notes
    };

    static constexpr std::array<Rate, 18> rates{{
        {"8 bars", 32.0},
        {"4 bars", 16.0},
        {"2 bars", 8.0},
        {"1 bar", 4.0},
        {"1/2", 2.0},
        {"1/2T", 4.0 / 3.0},
        {"1/4D", 1.5},
        {"1/4", 1.0},
        {"1/4T", 2.0 / 3.0},
        {"1/8D", 0.75},
        {"1/8", 0.5},
        {"1/8T", 1.0 / 3.0},
        {"1/16D", 0.375},
        {"1/16", 0.25},
        {"1/16T", 1.0 / 6.0},
        {"1/32", 0.125},
        {"1/32T", 1.0 / 12.0},
        {"1/64", 0.0625},
    }};

    static juce::StringArray names()
    {
        juce::StringArray n;
        for (const auto& r : rates)
            n.add(r.name);
        return n;
    }

    static double beatsAt(int index) noexcept
    {
        return rates[static_cast<std::size_t>(juce::jlimit(0, static_cast<int>(rates.size()) - 1, index))].beats;
    }

    static int indexOf(const char* name) noexcept
    {
        for (std::size_t i = 0; i < rates.size(); ++i)
            if (juce::String(rates[i].name) == name)
                return static_cast<int>(i);
        return 0;
    }
};

} // namespace lattice::dsp
