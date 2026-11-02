#pragma once

#include <algorithm>

namespace lattice::ui
{

/** What's selected in the Session view: a clip slot, a scene (its launch cell), or a track (its
    header), or a range of them (Shift+click): the rectangle from (track, scene) to (track2,
    scene2). Commands such as Duplicate, Delete, Copy and Launch act on it. */
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
    int track2 = -1; // the range's other corner; -1: the same as track
    int scene2 = -1; // the same as scene

    int firstTrack() const noexcept { return track2 < 0 ? track : std::min(track, track2); }
    int lastTrack() const noexcept { return track2 < 0 ? track : std::max(track, track2); }
    int firstScene() const noexcept { return scene2 < 0 ? scene : std::min(scene, scene2); }
    int lastScene() const noexcept { return scene2 < 0 ? scene : std::max(scene, scene2); }
    bool isRange() const noexcept { return firstTrack() != lastTrack() || firstScene() != lastScene(); }

    bool containsTrack(int t) const noexcept { return t >= firstTrack() && t <= lastTrack(); }
    bool containsScene(int s) const noexcept { return s >= firstScene() && s <= lastScene(); }
    bool containsSlot(int t, int s) const noexcept { return kind == Kind::slot && containsTrack(t) && containsScene(s); }

    /** The same selection extended to another corner (Shift+click), kept to its kind. */
    SessionSelection extendedTo(int otherTrack, int otherScene) const noexcept
    {
        auto s = *this;
        s.track2 = otherTrack;
        s.scene2 = otherScene;
        if (kind == Kind::track)
            s.scene2 = -1;
        if (kind == Kind::scene)
            s.track2 = -1;
        return s;
    }

    bool operator==(const SessionSelection&) const = default;
};

} // namespace lattice::ui
