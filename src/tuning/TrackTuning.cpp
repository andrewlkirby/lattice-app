#include "tuning/TrackTuning.h"

#include <cmath>

namespace lattice
{

TrackTuning::TrackTuning(TuningPtr initial)
{
    setTuning(std::move(initial));
}

void TrackTuning::setTuning(TuningPtr tuning)
{
    table.publish(std::move(tuning));
}

double TrackTuning::hzFor(int note, float bend) const noexcept
{
    const RtShared<TuningTable>::ReadScope scope(table);
    const TuningTable* t = scope.get();
    if (t == nullptr)
        return 0.0;

    const int index = note + noteOffset.load(std::memory_order_relaxed);
    if (index < 0 || index >= TuningTable::numNotes)
        return 0.0;

    double hz = 0.0;
    if (bendMode.load(std::memory_order_relaxed) == BendMode::Cents)
    {
        hz = t->hz[static_cast<std::size_t>(index)];
        if (hz > 0.0 && bend != 0.0f)
            hz *= std::exp2(static_cast<double>(bend) * bendRangeCents.load(std::memory_order_relaxed) / 1200.0);
    }
    else
    {
        hz = t->frequencyFor(index + static_cast<double>(bend) * bendRangeSteps.load(std::memory_order_relaxed));
    }

    return applyHighNotePolicy(hz, *t);
}

double TrackTuning::hzAt(double fractionalNote, double cents) const noexcept
{
    const RtShared<TuningTable>::ReadScope scope(table);
    const TuningTable* t = scope.get();
    if (t == nullptr)
        return 0.0;

    const double index = fractionalNote + noteOffset.load(std::memory_order_relaxed);
    if (!(index > -0.5 && index < TuningTable::numNotes - 0.5))
        return 0.0;
    double hz = t->frequencyFor(index);
    if (hz > 0.0 && cents != 0.0)
        hz *= std::exp2(cents / 1200.0);
    return applyHighNotePolicy(hz, *t);
}

double TrackTuning::applyHighNotePolicy(double hz, const TuningTable& t) const noexcept
{
    if (hz <= 0.0)
        return 0.0;

    const double sr = sampleRate.load(std::memory_order_relaxed);
    if (sr <= 0.0)
        return hz;

    const double limit = maxPlayableFraction * sr;
    if (hz < limit)
        return hz;
    if (highNotePolicy.load(std::memory_order_relaxed) == HighNotePolicy::Mute)
        return 0.0;

    const double period = t.periodRatio > 1.0 ? t.periodRatio : 2.0;
    while (hz >= limit)
        hz /= period;
    return hz;
}

} // namespace lattice
