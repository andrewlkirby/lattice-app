#pragma once

#include "control/surface/Mode.h"

namespace lattice::control
{

/** The clip a surface edits on a track (the step sequencers, Clip mode, Double Loop, Quantize):
    the selected scene's clip, or (that slot being empty) the clip the track is playing, or an
    invalid tree. As the Clip view shows the selected slot's clip, the two agree. */
inline juce::ValueTree selectedClip(const SurfaceContext& c, int track)
{
    const auto& set = c.session.set();
    if (track < 0 || track >= set.numTracks())
        return {};
    const int scene = c.selection.get().scene;
    if (scene >= 0 && scene < set.numScenes())
        if (const auto clip = set.clip(track, scene); clip.isValid())
            return clip;
    if (const auto id = c.feed.trackPlay(track).playingClipId; id != 0)
        if (const auto [t, s] = set.findClip(id); t == track)
            return set.clip(t, s);
    return {};
}

/** The track the selection is on (its first track), or -1 for a scene or a group. */
inline int selectedTrack(const SurfaceContext& c)
{
    const auto s = c.selection.get();
    const auto& set = c.session.set();
    if (s.kind == ui::SessionSelection::Kind::scene)
        return -1;
    const int t = s.firstTrack();
    return t >= 0 && t < set.numTracks() && !set.isGroupTrack(t) ? t : -1;
}

} // namespace lattice::control
