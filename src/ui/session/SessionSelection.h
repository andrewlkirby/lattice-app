#pragma once

namespace lattice::ui
{

/** What's selected in the Session view: a clip slot, a scene (its launch cell), or a track (its
    header). Commands such as Duplicate, Delete and Launch act on it. */
struct SessionSelection
{
    enum class Kind
    {
        slot,
        scene,
        track
    };

    Kind kind = Kind::slot;
    int track = 0;
    int scene = 0;

    bool operator==(const SessionSelection&) const = default;
};

} // namespace lattice::ui
