#include "tuning/TuningTable.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lattice
{

double TuningTable::frequencyFor(double fractionalNote) const noexcept
{
    const double x = std::clamp(fractionalNote, 0.0, static_cast<double>(numNotes - 1));
    const int n = std::min(static_cast<int>(x), numNotes - 2);
    const double frac = x - n;
    const double a = hz[static_cast<std::size_t>(n)];
    const double b = hz[static_cast<std::size_t>(n + 1)];

    if (frac == 0.0)
        return a;
    if (frac == 1.0)
        return b;
    if (a <= 0.0 || b <= 0.0)
        return frac < 0.5 ? a : b;
    return a * std::pow(b / a, frac);
}

int TuningTable::nearestNoteTo(double targetHz) const noexcept
{
    if (targetHz <= 0.0)
        return -1;

    int best = -1;
    double bestDistance = std::numeric_limits<double>::max();
    for (int i = 0; i < numNotes; ++i)
    {
        const double f = hz[static_cast<std::size_t>(i)];
        if (f <= 0.0)
            continue;
        const double distance = std::abs(std::log(f / targetHz));
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

double TuningTable::baseHz() const noexcept
{
    for (const double f : hz)
        if (f > 0.0)
            return f;
    return 0.0;
}

bool TuningTable::isAnchor(int note) const noexcept
{
    if (usesNoteNames || note < 0 || note >= numNotes)
        return false;
    const double base = baseHz();
    const double f = hz[static_cast<std::size_t>(note)];
    if (base <= 0.0 || f <= 0.0)
        return false;
    // The table holds frequencies to a thousandth of a hertz (59.9 Hz is not 2 x 30 Hz).
    const double multiple = std::round(f / base);
    return multiple >= 1.0 && std::abs(f - multiple * base) < 0.0005;
}

int TuningTable::defaultKeyboardBase() const noexcept
{
    const int page = std::max(1, keyboardPageSize);
    const int middle = nearestNoteTo(261.6255653005986);
    return middle < 0 ? 0 : (middle / page) * page;
}

} // namespace lattice
