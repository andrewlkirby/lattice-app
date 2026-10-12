// Snapshot stress tests (docs/03-m2-engine-session.md, test 11): edit a playing clip continually
// while the engine renders. No crash, no data race (run under ThreadSanitizer in CI), nothing
// allocated or freed on the audio thread, and no stuck notes.

#include "support/OfflineRenderer.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

using namespace lattice;
using namespace lattice::test;

namespace
{
/** Adds a note, or removes the oldest one, alternately keeping the clip between 8 and 24 notes. */
void editClip(LiveSet& set, const juce::ValueTree& clip, int step)
{
    auto notes = clip.getChildWithName(ids::notes);
    if (notes.getNumChildren() > 24 || (notes.getNumChildren() > 8 && step % 3 == 0))
    {
        set.removeNote(clip, notes.getChild(0));
        return;
    }
    NoteData n;
    n.pitch = 48 + (step * 7) % 36;
    n.start = (step % 16) * 0.25;
    n.length = 0.125 + (step % 5) * 0.25;
    set.addNote(clip, n);
}

void simulatedEdits(int seconds)
{
    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session(engine, tunings);
    OfflineRenderer renderer(engine, BlockPattern::Random);
    session.newSet(1, 1);
    session.set().setTrackArm(0, false, false);
    auto clip = session.set().createClip(0, 0, 4.0);
    session.set().setClipLoop(clip, 0.0, 4.0);
    renderer.attach(session);
    session.launchSlot(0, 0);
    const auto violations = rt::violationCount();

    // 1,000 edits per second of audio: one every 48 samples.
    for (int step = 0; step < seconds * 1000; ++step)
    {
        editClip(session.set(), clip, step);
        session.flush();
        renderer.renderFor(48);
        if (step % 100 == 0)
            session.snapshots().collectGarbage();
    }
    session.stopAllClips();
    session.stopAllClips(); // twice: at once
    renderer.renderFor(512);

    CHECK(rt::violationCount() == violations);
    CHECK(renderer.unbalancedNotes(session.set().trackId(0)).empty());
    CHECK(renderer.overflow == 0);
    session.snapshots().collectGarbage();
}
} // namespace

TEST_CASE("Snapshots: editing a playing clip 1,000 times a second (2 s simulated)", "[engine][snapshot]")
{
    simulatedEdits(2);
}

TEST_CASE("Snapshots: editing a playing clip 1,000 times a second (10 s simulated)",
          "[engine][snapshot][stress]")
{
    simulatedEdits(10);
}

TEST_CASE("Snapshots: a real audio thread renders while the document is edited",
          "[engine][snapshot][threads]")
{
    TuningRegistry tunings;
    AudioEngine engine;
    engine.prepare(48000.0, 256);
    SessionController session(engine, tunings);
    session.newSet(2, 2);
    auto clip = session.set().createClip(0, 0, 4.0);
    session.set().createClip(1, 0, 2.0);
    session.flush();
    session.launchScene(0);
    const auto violations = rt::violationCount();

    std::atomic<bool> done{false};
    std::atomic<std::int64_t> blocks{0};
    std::thread audio([&] {
        std::vector<float> left(256), right(256);
        std::array<float*, 2> outputs{left.data(), right.data()};
        const rt::ScopedAudioThread audioThread;
        while (!done.load())
        {
            engine.process(outputs.data(), 2, 256);
            blocks.fetch_add(1);
        }
    });

    for (int step = 0; step < 3000; ++step)
    {
        editClip(session.set(), clip, step);
        if (step % 50 == 0)
            session.set().setTrackVolumeDb(1, -(step % 20)); // mixer changes too
        session.flush();
        session.processEngineEvents();
        session.snapshots().collectGarbage();
        if (step % 200 == 0)
            session.launchScene(0); // relaunch: commands while snapshots change
        if (step % 10 == 0)
            std::this_thread::yield();
    }

    // Let the audio thread pass the last snapshot, then stop it.
    const auto seen = blocks.load();
    while (blocks.load() < seen + 4)
        std::this_thread::yield();
    done.store(true);
    audio.join();

    CHECK(blocks.load() > 0);
    CHECK(rt::violationCount() == violations);
    session.snapshots().collectGarbage();
}
