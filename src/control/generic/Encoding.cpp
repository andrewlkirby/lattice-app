#include "control/generic/Encoding.h"

#include <algorithm>
#include <array>

namespace lattice::control::generic
{

namespace
{
constexpr std::array<std::pair<Encoding, const char*>, 6> names{{{Encoding::absolute, "absolute"},
                                                                 {Encoding::twosComplement, "twosComplement"},
                                                                 {Encoding::binaryOffset, "binaryOffset"},
                                                                 {Encoding::signMagnitude, "signMagnitude"},
                                                                 {Encoding::cc14, "cc14"},
                                                                 {Encoding::nrpn14, "nrpn14"}}};

bool allIn(const std::vector<int>& values, int low, int high)
{
    return std::all_of(values.begin(), values.end(), [=](int v) { return v >= low && v <= high; });
}
} // namespace

const char* encodingName(Encoding e) noexcept
{
    for (const auto& [encoding, name] : names)
        if (encoding == e)
            return name;
    return "absolute";
}

std::optional<Encoding> encodingFromName(juce::StringRef name)
{
    for (const auto& [encoding, n] : names)
        if (name == juce::StringRef(n))
            return encoding;
    return std::nullopt;
}

int relativeSteps(Encoding e, int value) noexcept
{
    value &= 0x7f;
    switch (e)
    {
    case Encoding::twosComplement: return value < 64 ? value : value - 128;
    case Encoding::binaryOffset: return value - 64;
    case Encoding::signMagnitude: return (value & 0x40) != 0 ? -(value & 0x3f) : value & 0x3f;
    case Encoding::absolute:
    case Encoding::cc14:
    case Encoding::nrpn14: break;
    }
    return 0;
}

std::optional<Encoding> detectEncoding(const std::vector<int>& clockwise, const std::vector<int>& anticlockwise)
{
    if (clockwise.size() < 3 || anticlockwise.size() < 3)
        return std::nullopt;
    // Relative: each direction's values stay near the one step value its encoding gives (fast
    // turns send a few steps at once, so a band of them).
    if (allIn(clockwise, 1, 31))
    {
        if (allIn(anticlockwise, 97, 127))
            return Encoding::twosComplement;
        if (allIn(anticlockwise, 65, 95))
            return Encoding::signMagnitude;
    }
    if (allIn(clockwise, 65, 95) && allIn(anticlockwise, 33, 63))
        return Encoding::binaryOffset;
    // Absolute: up one way, down the other, mostly in order.
    const auto rising = [](const std::vector<int>& v, int sign) {
        int along = 0;
        for (std::size_t i = 1; i < v.size(); ++i)
            along += (v[i] - v[i - 1]) * sign > 0 ? 1 : 0;
        return along * 4 >= static_cast<int>(v.size() - 1) * 3; // three steps in four
    };
    if (rising(clockwise, 1) && rising(anticlockwise, -1))
        return Encoding::absolute;
    return std::nullopt;
}

} // namespace lattice::control::generic
