#include "tuning/parsers/NumberParsing.h"

#include <cmath>

namespace lattice::parsing
{
namespace
{
bool isDigit(juce::juce_wchar c)
{
    return c >= '0' && c <= '9';
}
} // namespace

std::optional<double> parseDecimal(juce::StringRef text)
{
    auto p = text.text;
    if (p.isEmpty())
        return std::nullopt;

    // Validate the shape first; JUCE's reader would otherwise stop quietly at the first bad char.
    auto q = p;
    if (*q == '+' || *q == '-')
        ++q;
    int intDigits = 0, fracDigits = 0;
    while (isDigit(*q))
    {
        ++q;
        ++intDigits;
    }
    if (*q == '.')
    {
        ++q;
        while (isDigit(*q))
        {
            ++q;
            ++fracDigits;
        }
    }
    if (intDigits + fracDigits == 0)
        return std::nullopt;
    if (*q == 'e' || *q == 'E')
    {
        ++q;
        if (*q == '+' || *q == '-')
            ++q;
        int expDigits = 0;
        while (isDigit(*q))
        {
            ++q;
            ++expDigits;
        }
        if (expDigits == 0)
            return std::nullopt;
    }
    if (!q.isEmpty())
        return std::nullopt;

    // JUCE converts with strtod_l in the "C" locale, so the system locale never matters.
    const double value = juce::CharacterFunctions::readDoubleValue(p);
    if (!std::isfinite(value))
        return std::nullopt;
    return value;
}

std::optional<std::int64_t> parseInteger(juce::StringRef text)
{
    auto p = text.text;
    if (p.isEmpty())
        return std::nullopt;

    std::int64_t value = 0;
    for (; !p.isEmpty(); ++p)
    {
        if (!isDigit(*p))
            return std::nullopt;
        value = value * 10 + (*p - '0');
        if (value > (std::int64_t{1} << 53))
            return std::nullopt;
    }
    return value;
}

} // namespace lattice::parsing
