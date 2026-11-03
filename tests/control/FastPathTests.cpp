// The fast path (docs/06-m5-controllers.md, 5b; "Fast path for pads"): the velocity curve and
// pressure, PadPlayer's notes, commands, pressure and strip; live MIDI placed at the sample its
// arrival gives, one block later, from producers on several threads; a stalled message thread
// delaying neither notes nor launches; and RealtimeCheck on an input thread during a pad fuzz.

#include "support/FakeSurface.h"
#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"
#include "control/modes/SessionMode.h"
#include "control/surface/PadPlayer.h"
#include "control/surface/Presets.h"
#include "control/surface/SurfaceController.h"
#include "ui/commands/SessionCommands.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <random>
#include <thread>

using namespace lattice;
using namespace lattice::control;
using namespace lattice::test;

namespace
{
std::vector<TimedMidi> drain(AudioEngine& engine)
{
    std::vector<TimedMidi> out(MidiInputQueue::capacity);
    int n = 0;
    {
        const rt::ScopedAudioThread audio;
        n = engine.midiInput().popInto(out.data(), static_cast<int>(out.size()));
    }
    out.resize(static_cast<std::size_t>(n));
    return out;
}

/** A 4x4 map: every pad plays note 60 + its index, except pad 15, which plays 60 too (a
    duplicate), and pad 14, which launches. */
std::shared_ptr<const PadNoteMap> noteMap(const SurfaceCapabilities& caps, Expression e = {})
{
    auto m = PadNoteMap::empty(caps);
    m.setExpression(e, caps);
    m.source = 7;
    for (int i = 0; i < caps.numPads(); ++i)
    {
        auto& pad = m.pads[static_cast<std::size_t>(i)];
        pad.kind = PadNoteMap::Pad::Kind::note;
        pad.note = static_cast<std::uint8_t>(60 + i);
    }
    m.pads[15].note = 60;
    auto& launch = m.pads[14];
    launch.kind = PadNoteMap::Pad::Kind::command;
    launch.onDown.type = CommandType::LaunchScene;
    launch.onDown.scene = 0;
    m.addModifier(Role::shift);
    return std::make_shared<const PadNoteMap>(std::move(m));
}

bool isOn(const TimedMidi& e, int note) { return (e.bytes[0] & 0xf0) == 0x90 && e.bytes[1] == note && e.bytes[2] > 0; }
bool isOff(const TimedMidi& e, int note) { return (e.bytes[0] & 0xf0) == 0x80 && e.bytes[1] == note; }

/** One armed track with a recorder, rendering through the offline renderer. */
struct EngineFixture
{
    explicit EngineFixture(BlockPattern pattern = BlockPattern::Fixed64) : renderer(engine, pattern)
    {
        session.newSet(1, 1);
        auto chain = LiveSet::chainOf(session.set().track(0));
        while (chain.getNumChildren() > 0)
            session.set().removeDevice(chain.getChild(0));
        session.set().insertDevice(chain, -1, TestRecorderDevice::typeId);
        session.set().setTrackArm(0, true, false);
        session.flush();
        renderer.renderFor(512);
        renderer.attach(session);
    }
    std::int64_t trackId() const { return session.set().trackId(0); }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer;
};
} // namespace

TEST_CASE("Expression: the velocity curve bends light hits up or down; pressure starts past its threshold", "[control][fastpath]")
{
    Expression linear;
    CHECK(linear.velocityFor(1.0f) == 127);
    CHECK(linear.velocityFor(0.0f) == 1);
    CHECK(std::abs(linear.velocityFor(0.5f) - 64) <= 1);
    Expression soft;
    soft.velocityCurve = -1.0f;
    Expression hard;
    hard.velocityCurve = 1.0f;
    CHECK(soft.velocityFor(0.5f) > linear.velocityFor(0.5f));
    CHECK(hard.velocityFor(0.5f) < linear.velocityFor(0.5f));
    CHECK(soft.velocityFor(1.0f) == 127);
    CHECK(hard.velocityFor(1.0f) == 127);
    Expression range;
    range.velocityMin = 40;
    range.velocityMax = 100;
    CHECK(range.velocityFor(0.0f) == 40);
    CHECK(range.velocityFor(1.0f) == 100);

    Expression threshold;
    threshold.pressureThreshold = 0.2f;
    CHECK(threshold.pressureFor(0.1f) == 0);
    CHECK(threshold.pressureFor(0.2f) == 0);
    CHECK(threshold.pressureFor(1.0f) == 127);
    CHECK(std::abs(threshold.pressureFor(0.6f) - 64) <= 1);

    // A device without velocity (a fixed velocity on Virtual Push) plays what it sends.
    auto caps = presets::gridOnly(4, 4);
    caps.velocity = false;
    auto m = PadNoteMap::empty(caps);
    m.setExpression(soft, caps);
    CHECK(m.velocity[64] == 64);
    CHECK(m.velocity[0] == 1);
}

TEST_CASE("PadPlayer: pads play notes, duplicates retrigger and end with the last release, a release ends the note it started",
          "[control][fastpath]")
{
    TuningRegistry tunings;
    AudioEngine engine;
    auto caps = presets::gridOnly(4, 4);
    caps.aftertouch = Aftertouch::polyphonic;
    PadPlayer player(engine, caps);
    player.publish(noteMap(caps));

    auto down = SurfaceInput::padDown(3, 1, 1.0f); // bottom row, second pad: index 13
    CHECK(player.play(down));
    CHECK(down.played);
    auto events = drain(engine);
    REQUIRE(events.size() == 1);
    CHECK(isOn(events[0], 73));
    CHECK(events[0].bytes[2] == 127);
    CHECK(events[0].source == 7);
    CHECK(player.heldNotes() == 1);

    // The layout changes while it's held: the release still ends 73.
    auto other = noteMap(caps);
    auto shifted = std::make_shared<PadNoteMap>(*other);
    for (auto& p : shifted->pads)
        p.note = static_cast<std::uint8_t>(p.note + 12);
    player.publish(shifted);
    auto up = SurfaceInput::padUp(3, 1);
    CHECK(player.play(up));
    events = drain(engine);
    REQUIRE(events.size() == 1);
    CHECK(isOff(events[0], 73));
    CHECK(player.heldNotes() == 0);
    player.publish(noteMap(caps));

    SECTION("Two pads on one note")
    {
        auto a = SurfaceInput::padDown(0, 0, 0.5f); // note 60
        auto b = SurfaceInput::padDown(3, 3, 0.5f); // pad 15: note 60 too
        player.play(a);
        player.play(b);
        events = drain(engine);
        REQUIRE(events.size() == 3); // on, then off + on: played again, not stacked
        CHECK(isOn(events[0], 60));
        CHECK(isOff(events[1], 60));
        CHECK(isOn(events[2], 60));
        auto ua = SurfaceInput::padUp(0, 0);
        player.play(ua);
        CHECK(drain(engine).empty()); // pad 15 still holds it
        auto ub = SurfaceInput::padUp(3, 3);
        player.play(ub);
        events = drain(engine);
        REQUIRE(events.size() == 1);
        CHECK(isOff(events[0], 60));
    }
    SECTION("A modifier held sends pads to the message thread")
    {
        auto shift = SurfaceInput::buttonDown({Role::shift, 0});
        CHECK_FALSE(player.play(shift));
        auto pad = SurfaceInput::padDown(0, 1, 1.0f);
        CHECK_FALSE(player.play(pad));
        CHECK_FALSE(pad.played);
        auto padUp = SurfaceInput::padUp(0, 1);
        CHECK_FALSE(player.play(padUp));
        CHECK(drain(engine).empty());
        auto release = SurfaceInput::buttonUp({Role::shift, 0});
        player.play(release);
        auto again = SurfaceInput::padDown(0, 1, 1.0f);
        CHECK(player.play(again));
        // Another button (not one of the map's modifiers) doesn't.
        auto metronome = SurfaceInput::buttonDown({Role::metronome, 0});
        player.play(metronome);
        auto next = SurfaceInput::padDown(0, 2, 1.0f);
        CHECK(player.play(next));
    }
    SECTION("Letting go of everything (the driver goes away) ends every note")
    {
        for (int c = 0; c < 4; ++c)
        {
            auto in = SurfaceInput::padDown(1, c, 1.0f);
            player.play(in);
        }
        drain(engine);
        CHECK(player.heldNotes() == 4);
        player.releaseAll();
        CHECK(player.heldNotes() == 0);
        CHECK(drain(engine).size() == 4);
    }
}

TEST_CASE("PadPlayer: pressure per note or for the channel; the strip bends and springs back, or modulates", "[control][fastpath]")
{
    TuningRegistry tunings;
    AudioEngine engine;
    auto caps = presets::gridOnly(4, 4);
    caps.aftertouch = Aftertouch::polyphonic;
    PadPlayer player(engine, caps);

    SECTION("Polyphonic")
    {
        player.publish(noteMap(caps));
        auto down = SurfaceInput::padDown(0, 2, 1.0f);
        player.play(down);
        auto press = SurfaceInput::padPressure(0, 2, 0.5f);
        CHECK(player.play(press));
        auto same = SurfaceInput::padPressure(0, 2, 0.501f); // the same step: nothing new
        player.play(same);
        const auto events = drain(engine);
        REQUIRE(events.size() == 2);
        CHECK((events[1].bytes[0] & 0xf0) == 0xa0);
        CHECK(events[1].bytes[1] == 62);
        CHECK(std::abs(events[1].bytes[2] - 64) <= 1);
    }
    SECTION("Channel: the most any held pad is pressed, back to 0 when they're let go")
    {
        Expression e;
        e.pressure = Expression::Pressure::channel;
        player.publish(noteMap(caps, e));
        auto a = SurfaceInput::padDown(0, 0, 1.0f);
        auto b = SurfaceInput::padDown(0, 1, 1.0f);
        player.play(a);
        player.play(b);
        drain(engine);
        auto pa = SurfaceInput::padPressure(0, 0, 0.8f);
        auto pb = SurfaceInput::padPressure(0, 1, 0.3f);
        player.play(pa);
        player.play(pb); // less than a's: the channel's stays
        auto events = drain(engine);
        REQUIRE(events.size() == 1);
        CHECK(events[0].bytes[0] == 0xd0);
        CHECK(events[0].size == 2);
        CHECK(std::abs(events[0].bytes[1] - 102) <= 1);
        auto ua = SurfaceInput::padUp(0, 0);
        player.play(ua);
        events = drain(engine);
        REQUIRE(events.size() == 2); // a's note off, then b's pressure for the channel
        CHECK(events[1].bytes[0] == 0xd0);
        CHECK(std::abs(events[1].bytes[1] - 38) <= 1);
        auto ub = SurfaceInput::padUp(0, 1);
        player.play(ub);
        events = drain(engine);
        REQUIRE(events.size() == 2);
        CHECK(events[1].bytes[1] == 0);
    }
    SECTION("A device without pressure sends none")
    {
        auto none = presets::gridOnly(4, 4);
        PadPlayer plain(engine, none);
        plain.publish(noteMap(none));
        auto down = SurfaceInput::padDown(0, 0, 1.0f);
        plain.play(down);
        auto press = SurfaceInput::padPressure(0, 0, 0.9f);
        CHECK_FALSE(plain.play(press));
        CHECK(drain(engine).size() == 1);
    }
    SECTION("The strip")
    {
        player.publish(noteMap(caps));
        auto top = SurfaceInput::strip(1.0f);
        player.play(top);
        auto let = SurfaceInput::stripRelease();
        player.play(let);
        auto events = drain(engine);
        REQUIRE(events.size() == 2);
        CHECK(events[0].bytes[0] == 0xe0);
        CHECK(((events[0].bytes[2] << 7) | events[0].bytes[1]) == 16383);
        CHECK(((events[1].bytes[2] << 7) | events[1].bytes[1]) == 8192); // springs back

        Expression mod;
        mod.strip = Expression::Strip::modWheel;
        player.publish(noteMap(caps, mod));
        auto half = SurfaceInput::strip(0.5f);
        player.play(half);
        auto letGo = SurfaceInput::stripRelease();
        player.play(letGo);
        events = drain(engine);
        REQUIRE(events.size() == 1); // stays where it's left
        CHECK(events[0].bytes[0] == 0xb0);
        CHECK(events[0].bytes[1] == 1);

        Expression scroll;
        scroll.strip = Expression::Strip::scroll;
        player.publish(noteMap(caps, scroll));
        auto moved = SurfaceInput::strip(0.2f);
        CHECK_FALSE(player.play(moved)); // the mode's, on the message thread
        CHECK(drain(engine).empty());
    }
}

TEST_CASE("Engine: live MIDI plays one block after it arrives, at the matching sample, whatever its arrival within the block",
          "[control][fastpath][engine]")
{
    for (const auto pattern : {BlockPattern::Fixed64, BlockPattern::Fixed512})
    {
        INFO(describe(pattern));
        EngineFixture f(pattern);
        const int block = pattern == BlockPattern::Fixed64 ? 64 : 512;
        f.renderer.useClock(1000.0);
        f.renderer.renderFor(block * 4);
        const std::int64_t next = f.renderer.now(); // the next callback's first sample
        // Arrivals spread over the block before that callback, and one older than a block (a late
        // callback): that one plays at the block's start.
        const std::vector<int> ages{block, block - 1, block * 3 / 4, block / 2, 7, 1};
        int note = 40;
        for (const int age : ages)
        {
            const std::uint8_t bytes[]{0x90, static_cast<std::uint8_t>(note++), 100};
            f.engine.midiInput().push(bytes, 3, computerKeyboardSource, f.renderer.timeAt(next - age));
        }
        const std::uint8_t late[]{0x90, 90, 100};
        f.engine.midiInput().push(late, 3, computerKeyboardSource, f.renderer.timeAt(next - block * 3));
        f.renderer.renderFor(block);

        const auto ons = f.renderer.noteOns(f.trackId());
        REQUIRE(ons.size() == ages.size() + 1);
        for (const auto& e : ons)
        {
            if (e.data1 == 90)
            {
                CHECK(e.sample == next); // late: at the start
                continue;
            }
            const int age = ages[static_cast<std::size_t>(e.data1 - 40)];
            INFO("arrived " << age << " samples before the callback");
            CHECK(e.sample == next + block - age); // exactly one block after it arrived
        }
        // In time order, as the instrument needs them.
        for (std::size_t i = 1; i < ons.size(); ++i)
            CHECK(ons[i].sample >= ons[i - 1].sample);
        // The probe measured one block (or less, for the late one) from arrival to sound.
        const auto latency = f.engine.inputLatency().summary();
        CHECK(latency.count == ages.size() + 1);
        CHECK(latency.p50Ms <= 1000.0 * block / OfflineRenderer::sampleRate + LatencyStats::bucketMs);
    }
}

TEST_CASE("Engine: live MIDI from several threads at once arrives whole, without locks, and in time order",
          "[control][fastpath][engine]")
{
    EngineFixture f;
    constexpr int threads = 4, perThread = 200;
    std::atomic<bool> go{false};
    std::vector<std::thread> producers;
    for (int t = 0; t < threads; ++t)
        producers.emplace_back([&, t] {
            while (!go.load())
                std::this_thread::yield();
            for (int i = 0; i < perThread; ++i)
            {
                const std::uint8_t on[]{0x90, static_cast<std::uint8_t>(20 + t), 100};
                const std::uint8_t off[]{0x80, static_cast<std::uint8_t>(20 + t), 0};
                f.engine.midiInput().push(on, 3, computerKeyboardSource);
                f.engine.midiInput().push(off, 3, computerKeyboardSource);
            }
        });
    go = true;
    // Render while they push (the audio thread is this one).
    for (int i = 0; i < 200; ++i)
        f.renderer.renderFor(64);
    for (auto& p : producers)
        p.join();
    f.renderer.renderFor(64 * 40);
    CHECK(f.engine.midiInput().droppedCount() == 0);
    CHECK(static_cast<int>(f.renderer.noteOns(f.trackId()).size()) == threads * perThread);
    CHECK(f.renderer.unbalancedNotes(f.trackId()).empty());
}

namespace
{
/** Notes when each of a track's note-ons was handed to its instrument (the audio thread's clock). */
struct ArrivalTap final : TrackEventTap
{
    void onTrackEvent(std::int64_t, std::int64_t, const TimedMidi& e) noexcept override
    {
        if ((e.bytes[0] & 0xf0) == 0x90 && e.bytes[2] > 0 && first.load() < 0.0)
            first.store(MidiInputQueue::nowSeconds());
    }
    std::atomic<double> first{-1.0};
};

/** Runs the engine in real time on its own thread, 10 ms blocks, as a device would. */
struct RealTimeAudio
{
    explicit RealTimeAudio(AudioEngine& e) : engine(e)
    {
        thread = std::thread([this] {
            constexpr int block = 480;
            std::array<float, block> l{}, r{};
            std::array<float*, 2> out{l.data(), r.data()};
            auto next = std::chrono::steady_clock::now();
            while (running.load())
            {
                {
                    const rt::ScopedAudioThread audio;
                    engine.process(out.data(), 2, block, MidiInputQueue::nowSeconds());
                }
                next += std::chrono::microseconds(10000);
                std::this_thread::sleep_until(next);
            }
        });
    }
    ~RealTimeAudio()
    {
        running = false;
        thread.join();
    }
    AudioEngine& engine;
    std::atomic<bool> running{true};
    std::thread thread;
};
} // namespace

TEST_CASE("Fast path: with the message thread stalled for 500 ms, pads still play notes and launch clips at once",
          "[control][fastpath][realtime]")
{
    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    ui::EngineStateFeed feed{session, engine};
    ui::CommandRegistry commands{&session.undoManager()};
    ui::SessionSelectionModel selection;
    session.newSet(2, 2);
    for (int t = 0; t < 2; ++t)
    {
        auto chain = LiveSet::chainOf(session.set().track(t));
        while (chain.getNumChildren() > 0)
            session.set().removeDevice(chain.getChild(0));
        session.set().insertDevice(chain, -1, TestRecorderDevice::typeId);
    }
    session.set().setGlobalQuantization(Quantization::None);
    addClip(session.set(), 1, 0, 4.0, {{60, 0.0, 1.0, 100}});
    engine.prepare(48000.0, 480);
    session.flush();
    session.set().setTrackArm(0, true, false);
    session.flush();

    FakeSurface surface(presets::virtualPush(8, 8));
    ui::addSessionCommands(commands, session, {[&] { return selection.get(); }, [&](ui::SessionSelection s) { selection.set(s); }}, {});
    SurfaceController controller(SurfaceContext{session, commands, selection, feed}, surface);
    controller.refresh();
    REQUIRE(controller.padMap().pads[1].kind == PadNoteMap::Pad::Kind::command); // track 1, scene 0: a clip

    ArrivalTap noteTap, launchTap;
    session.processorForTrack(0)->setEventTap(&noteTap);
    session.processorForTrack(1)->setEventTap(&launchTap);
    // Track 0 gets a note map of its own, through a second player (one map per player).
    auto caps = presets::gridOnly(4, 4);
    PadPlayer notes(engine, caps);
    notes.publish(noteMap(caps));

    session.play();
    RealTimeAudio audio(engine);
    std::atomic<double> pressedAt{-1.0};
    std::thread input([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // well inside the stall
        pressedAt = MidiInputQueue::nowSeconds();
        auto note = SurfaceInput::padDown(0, 0, 1.0f);
        notes.handle(note);
        auto launch = SurfaceInput::padDown(0, 1, 1.0f);
        controller.padPlayer().handle(launch);
    });
    // The message thread stalls: no messages, no timers, no refreshes.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    input.join();
    const double noteDelay = noteTap.first.load() - pressedAt.load();
    const double launchDelay = launchTap.first.load() - pressedAt.load();
    session.processorForTrack(0)->setEventTap(nullptr);
    session.processorForTrack(1)->setEventTap(nullptr);
    INFO("note after " << noteDelay * 1000.0 << " ms, launch after " << launchDelay * 1000.0 << " ms");
    REQUIRE(noteTap.first.load() > 0.0);
    REQUIRE(launchTap.first.load() > 0.0);
    // A block or two of 10 ms (plus the scheduler's slack on a busy machine), nowhere near the stall.
    CHECK(noteDelay < 0.1);
    CHECK(launchDelay < 0.1);
    auto up = SurfaceInput::padUp(0, 0);
    notes.handle(up);
}

TEST_CASE("Fast path: RealtimeCheck on the input thread while pads are fuzzed and maps republished", "[control][fastpath][realtime]")
{
    REQUIRE(rt::hooksInstalled());
    TuningRegistry tunings;
    AudioEngine engine;
    auto caps = presets::virtualPush(8, 8);
    caps.aftertouch = Aftertouch::polyphonic;
    PadPlayer player(engine, caps);
    player.publish(noteMap(presets::gridOnly(4, 4)));

    const auto violations = rt::violationCount();
    std::atomic<bool> done{false};
    rt::AllocationCounters counted{};
    std::thread input([&] {
        std::mt19937 rng(5);
        const rt::ScopedAudioThread realtime; // allocations and locks here count as violations
        const auto before = rt::countersForThisThread();
        for (int i = 0; i < 40000; ++i)
        {
            const int row = static_cast<int>(rng() % 8), column = static_cast<int>(rng() % 8);
            SurfaceInput in;
            switch (rng() % 7)
            {
            case 0:
            case 1: in = SurfaceInput::padDown(row, column, static_cast<float>(rng() % 128) / 127.0f); break;
            case 2:
            case 3: in = SurfaceInput::padUp(row, column); break;
            case 4: in = SurfaceInput::padPressure(row, column, static_cast<float>(rng() % 128) / 127.0f); break;
            case 5: in = rng() % 2 ? SurfaceInput::strip(static_cast<float>(rng() % 100) / 99.0f) : SurfaceInput::stripRelease(); break;
            default:
                in = rng() % 2 ? SurfaceInput::buttonDown({Role::shift, 0}) : SurfaceInput::buttonUp({Role::shift, 0});
                break;
            }
            player.play(in);
            if (i % 64 == 0)
            {
                std::array<TimedMidi, 512> sink{};
                engine.midiInput().popInto(sink.data(), static_cast<int>(sink.size())); // the audio thread's part
            }
        }
        player.releaseAll();
        const auto after = rt::countersForThisThread();
        counted.allocations = after.allocations - before.allocations;
        counted.deallocations = after.deallocations - before.deallocations;
        done = true;
    });
    // Meanwhile the message thread publishes new maps (notes, commands, other expression).
    for (int i = 0; !done.load(); ++i)
    {
        Expression e;
        e.pressure = i % 2 ? Expression::Pressure::channel : Expression::Pressure::polyphonic;
        e.strip = i % 3 == 0 ? Expression::Strip::modWheel : Expression::Strip::pitchBend;
        auto m = std::make_shared<PadNoteMap>(*noteMap(caps, e));
        m->rows = 8;
        m->columns = 8;
        m->pads.resize(64);
        for (int p = 0; p < 64; ++p)
        {
            auto& pad = m->pads[static_cast<std::size_t>(p)];
            pad.kind = (p + i) % 5 == 0 ? PadNoteMap::Pad::Kind::command : PadNoteMap::Pad::Kind::note;
            pad.note = static_cast<std::uint8_t>((p * 3 + i) % 128);
            pad.onDown.type = CommandType::StopTrack;
        }
        player.publish(m);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    input.join();
    CHECK(counted.allocations == 0);
    CHECK(counted.deallocations == 0);
    CHECK(rt::violationCount() == violations);
    CHECK(player.heldNotes() == 0);
}

TEST_CASE("Session mode: a slot's pad launches on the fast path alone; with Delete held it goes to the message thread",
          "[control][fastpath][session]")
{
    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer{engine, BlockPattern::Fixed64};
    ui::EngineStateFeed feed{session, engine};
    ui::CommandRegistry commands{&session.undoManager()};
    ui::SessionSelectionModel selection;
    session.newSet(4, 4);
    session.set().createClip(1, 0, 4.0);
    session.set().createClip(2, 1, 4.0);
    session.set().setGlobalQuantization(Quantization::None);
    session.flush();
    ui::addSessionCommands(commands, session, {[&] { return selection.get(); }, [&](ui::SessionSelection s) { selection.set(s); }}, {});
    FakeSurface surface(presets::virtualPush(8, 8));
    SurfaceController controller(SurfaceContext{session, commands, selection, feed}, surface);
    controller.refresh();
    const auto& map = controller.padMap();
    CHECK(map.pads[1].kind == PadNoteMap::Pad::Kind::command);
    CHECK(map.pads[1].onDown.type == CommandType::LaunchSlot);
    CHECK(map.pads[1].hasUp);                                        // a Gate clip's release
    CHECK(map.pads[3].kind == PadNoteMap::Pad::Kind::command); // an empty slot: its stop button
    CHECK(map.pads[3].onDown.type == CommandType::StopTrack);
    CHECK(map.pads[0].kind == PadNoteMap::Pad::Kind::none);    // track 0 is armed: its empty slot records
    CHECK(map.isModifier(Role::deleteClip));
    CHECK(map.isModifier(Role::shift));

    session.play();
    renderer.renderFor(64);
    // Only the fast path: nothing reaches the controller (as if the message thread were stalled).
    auto press = SurfaceInput::padDown(0, 1, 1.0f);
    CHECK(controller.padPlayer().handle(press));
    renderer.renderFor(64);
    session.processEngineEvents();
    CHECK(session.trackState(1).playingClipId == LiveSet::idOf(session.set().clip(1, 0)));
    auto release = SurfaceInput::padUp(0, 1);
    controller.padPlayer().handle(release);

    // Scene buttons too; with Shift held (it selects the scene instead) they don't.
    REQUIRE(controller.padMap().buttons.size() == 4);
    auto shiftDown = SurfaceInput::buttonDown({Role::shift, 0});
    controller.padPlayer().handle(shiftDown);
    auto shifted = SurfaceInput::buttonDown({Role::scene, 1});
    CHECK_FALSE(controller.padPlayer().handle(shifted));
    auto shiftUp = SurfaceInput::buttonUp({Role::shift, 0});
    controller.padPlayer().handle(shiftUp);
    auto scene = SurfaceInput::buttonDown({Role::scene, 1});
    CHECK(controller.padPlayer().handle(scene));
    CHECK(scene.played);
    renderer.renderFor(64);
    session.processEngineEvents();
    CHECK(session.trackState(2).playingClipId == LiveSet::idOf(session.set().clip(2, 1)));

    // The whole path: the controller shows it and selects the slot, without launching it again.
    surface.press(0, 1);
    CHECK(selection.get().track == 1);
    surface.release(0, 1);

    // Delete held: the fast path leaves it, the message thread deletes the clip.
    surface.down(Role::deleteClip);
    surface.press(0, 1);
    surface.release(0, 1);
    surface.up(Role::deleteClip);
    CHECK_FALSE(session.set().clip(1, 0).isValid());

    // A grid without scene buttons launches scenes from its last column, on the fast path.
    FakeSurface grid(presets::gridOnly(4, 4));
    SurfaceController gridController(SurfaceContext{session, commands, selection, feed}, grid);
    gridController.refresh();
    const auto& last = gridController.padMap().pads[3]; // row 0, the last column
    CHECK(last.kind == PadNoteMap::Pad::Kind::command);
    CHECK(last.onDown.type == CommandType::LaunchScene);
    CHECK(last.onDown.scene == 0);
}
