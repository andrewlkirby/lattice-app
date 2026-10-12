#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <optional>

namespace lattice::parsing
{

/** Strict, locale-independent decimal: [+-]digits[.digits][(e|E)[+-]digits], or [+-].digits.
    The whole string must match (no surrounding spaces). A comma decimal separator is rejected,
    whatever the system locale, so "1,5" can never silently become 1 or 1.5. */
std::optional<double> parseDecimal(juce::StringRef text);

/** Strict non-negative integer (digits only). */
std::optional<std::int64_t> parseInteger(juce::StringRef text);

} // namespace lattice::parsing
