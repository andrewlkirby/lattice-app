// The browser's library (docs/05-m4-ui.md, 4e; Tests, item 7): indexing a fixed folder gives the
// expected records, rescans only read what changed, search ranking on fixed queries, the watcher
// noticing added, removed and renamed files, waveform peaks on disk, the preview bus, and the
// phrases instrument presets play.

#include "core/model/LiveSet.h"
#include "devices/presets/Presets.h"
#include "engine/AudioEngine.h"
#include "engine/SessionController.h"
#include "library/Library.h"
#include "library/LibraryIndex.h"
#include "library/WaveformPeaks.h"
#include "platform/FileWatcher.h"
#include "support/TestDevices.h"
#include "support/TestFiles.h"
#include "tuning/TuningRegistry.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

using namespace lattice;
using namespace lattice::library;
using namespace lattice::test;

namespace
{
/** The fixed test folder:
        Kick 01.wav        mono 48 kHz, 0.5 s
        Pad A4.wav         stereo 44.1 kHz, 1.25 s, root A4 in its smpl chunk
        Drums/Snare 02.wav mono 48 kHz, 0.25 s
        Warm Pad.lpreset   a Synth preset
        Five.scl           a Scala scale
        Table.txt          a Colundi-format table
        notes.txt          not a tuning
        readme.md          not anything the library shows
        .hidden/Ghost.wav  in a hidden folder */
void makeFixedFolder(const juce::File& root)
{
    REQUIRE(writeWav(root.getChildFile("Kick 01.wav"), 0.5));
    REQUIRE(writeWav(root.getChildFile("Pad A4.wav"), 1.25, 44100.0, 2, 69));
    REQUIRE(writeWav(root.getChildFile("Drums/Snare 02.wav"), 0.25));
    REQUIRE(writeWav(root.getChildFile(".hidden/Ghost.wav"), 0.1));
    LiveSet set;
    set.resetToDefault(1, 1);
    const auto synth = set.insertDevice(LiveSet::chainOf(set.track(0)), -1, "lattice.synth");
    REQUIRE(presets::write(synth, "Warm Pad", root.getChildFile("Warm Pad.lpreset")).wasOk());
    REQUIRE(root.getChildFile("Five.scl").replaceWithText(scalaScale()));
    REQUIRE(root.getChildFile("Table.txt").replaceWithText(colundiTable()));
    REQUIRE(root.getChildFile("notes.txt").replaceWithText("Just some notes."));
    REQUIRE(root.getChildFile("readme.md").replaceWithText("# Read me"));
}

const Item* byName(const std::vector<Item>& items, const juce::String& name)
{
    for (const auto& i : items)
        if (i.name == name)
            return &i;
    return nullptr;
}

juce::StringArray names(const std::vector<Item>& items)
{
    juce::StringArray result;
    for (const auto& i : items)
        result.add(i.name);
    return result;
}

/** Built-in items as the app sets them: a few devices, a preset, a tuning. */
std::vector<Item> someBuiltIns()
{
    std::vector<Item> items;
    const auto add = [&](Kind kind, const juce::String& key, const juce::String& name, const juce::String& category,
                         const juce::String& type = {}) {
        Item i;
        i.kind = kind;
        i.key = key;
        i.name = name;
        i.category = category;
        i.deviceType = type;
        items.push_back(i);
    };
    add(Kind::device, "device:lattice.synth", "Synth", category::instruments, "lattice.synth");
    add(Kind::device, "device:lattice.sampler", "Sampler", category::instruments, "lattice.sampler");
    add(Kind::device, "device:lattice.drumRack", "Drum Rack", category::instruments, "lattice.drumRack");
    add(Kind::device, "device:lattice.reverb", "Reverb", category::audioEffects, "lattice.reverb");
    add(Kind::device, "device:lattice.chord", "Chord", category::midiEffects, "lattice.chord");
    add(Kind::preset, "factory:pad", "Pad", category::instruments, "lattice.synth");
    add(Kind::preset, "factory:kit", "Studio Kit", category::drums, "lattice.drumRack");
    add(Kind::tuning, "tuning:builtin.colundi", "Colundi", category::tunings);
    return items;
}

/** Waits (running the library's deliveries) until `done` or a few seconds pass. */
template <typename F>
bool waitFor(Library& library, JobPool& jobs, F done)
{
    for (int i = 0; i < 400; ++i)
    {
        jobs.deliverFinished();
        library.deliverForTests();
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
} // namespace

TEST_CASE("Library: indexing a fixed folder gives the expected records", "[library]")
{
    TempFolder temp("lattice-library");
    makeFixedFolder(temp.folder);
    LibraryIndex index({}, testRegistry());
    REQUIRE(index.openResult().wasOk());
    index.addPlace(temp.folder);
    const auto stats = index.scan(temp.folder);
    CHECK(stats.added == 7); // 5 files the library shows (+ Table.txt) and the Drums folder
    CHECK(stats.removed == 0);

    const auto all = index.find({});
    CHECK(names(all).size() == 7);
    CHECK(byName(all, "Ghost") == nullptr);  // hidden folder
    CHECK(byName(all, "notes") == nullptr);  // not a tuning
    CHECK(byName(all, "readme") == nullptr); // not anything

    const auto* kick = byName(all, "Kick 01");
    REQUIRE(kick != nullptr);
    CHECK(kick->kind == Kind::sample);
    CHECK(kick->category == category::samples);
    CHECK(kick->channels == 1);
    CHECK(kick->sampleRate == 48000.0);
    CHECK(kick->seconds == Catch::Approx(0.5).margin(1e-3));
    CHECK(kick->rootHz == 0.0);
    CHECK(kick->folder == temp.folder.getFullPathName());
    CHECK(kick->place == temp.folder.getFullPathName());

    const auto* pad = byName(all, "Pad A4");
    REQUIRE(pad != nullptr);
    CHECK(pad->channels == 2);
    CHECK(pad->sampleRate == 44100.0);
    CHECK(pad->seconds == Catch::Approx(1.25).margin(1e-3));
    CHECK(pad->rootHz == Catch::Approx(440.0)); // the smpl chunk's root
    CHECK(pad->detail.contains("44.1 kHz"));
    CHECK(pad->detail.contains("stereo"));

    const auto* snare = byName(all, "Snare 02");
    REQUIRE(snare != nullptr);
    CHECK(snare->folder == (temp / "Drums").getFullPathName());
    const auto* drums = byName(all, "Drums");
    REQUIRE(drums != nullptr);
    CHECK(drums->kind == Kind::folder);

    const auto* preset = byName(all, "Warm Pad");
    REQUIRE(preset != nullptr);
    CHECK(preset->kind == Kind::preset);
    CHECK(preset->deviceType == "lattice.synth");
    CHECK(preset->category == category::instruments);

    const auto* scale = byName(all, "Five");
    REQUIRE(scale != nullptr);
    CHECK(scale->kind == Kind::tuning);
    CHECK(scale->detail.startsWith("5 notes"));
    const auto* table = byName(all, "Table");
    REQUIRE(table != nullptr);
    CHECK(table->kind == Kind::tuning);

    // Browsing: what's directly in a folder, folders first.
    const auto top = index.find({.folder = temp.folder.getFullPathName()});
    REQUIRE(!top.empty());
    CHECK(top.front().name == "Drums");
    CHECK(byName(top, "Snare 02") == nullptr);
    CHECK(names(index.find({.folder = (temp / "Drums").getFullPathName()})) == juce::StringArray{"Snare 02"});
}

TEST_CASE("Library: a rescan reads only what changed, and forgets what's gone", "[library]")
{
    TempFolder temp("lattice-library");
    makeFixedFolder(temp.folder);
    LibraryIndex index({}, testRegistry());
    index.addPlace(temp.folder);
    index.scan(temp.folder);
    const auto generation = index.generation();

    auto again = index.scan(temp.folder);
    CHECK(again.total() == 0);
    CHECK(again.unchanged == 7);
    CHECK(index.generation() == generation); // nothing to tell the browser

    // A changed file is read again; a deleted one goes; a new one comes.
    REQUIRE(writeWav(temp / "Kick 01.wav", 2.0));
    (temp / "Kick 01.wav").setLastModificationTime(juce::Time::getCurrentTime() + juce::RelativeTime::seconds(10));
    REQUIRE((temp / "Drums/Snare 02.wav").deleteFile());
    REQUIRE(writeWav(temp / "Drums/Hat 03.wav", 0.1));
    again = index.scan(temp.folder);
    CHECK(again.changed == 1);
    CHECK(again.removed == 1);
    CHECK(again.added == 1);
    const auto all = index.find({});
    REQUIRE(byName(all, "Kick 01") != nullptr);
    CHECK(byName(all, "Kick 01")->seconds == Catch::Approx(2.0).margin(1e-3));
    CHECK(byName(all, "Snare 02") == nullptr);
    CHECK(byName(all, "Hat 03") != nullptr);

    // A rescan of one folder leaves the rest alone.
    REQUIRE(writeWav(temp / "Drums/Clap 04.wav", 0.1));
    REQUIRE(writeWav(temp / "Elsewhere.wav", 0.1));
    again = index.scan(temp.folder, {}, {}, temp / "Drums");
    CHECK(again.added == 1);
    CHECK(byName(index.find({}), "Elsewhere") == nullptr);

    // Removing the place removes its items; collections stay with their keys.
    index.setInCollection((temp / "Kick 01.wav").getFullPathName(), 0, true);
    index.removePlace(temp.folder);
    CHECK(index.find({}).empty());
    index.addPlace(temp.folder);
    index.scan(temp.folder);
    REQUIRE(byName(index.find({}), "Kick 01") != nullptr);
    CHECK(byName(index.find({}), "Kick 01")->inCollection(0));
}

TEST_CASE("Library: search ranks exact names, then names that start with the text, then the rest", "[library]")
{
    TempFolder temp("lattice-library");
    makeFixedFolder(temp.folder);
    REQUIRE(writeWav(temp / "Synth Stab.wav", 0.1));
    REQUIRE(writeWav(temp / "Old Synth Loop.wav", 0.1));
    REQUIRE(writeWav(temp / "Pads/Airy.wav", 0.1));
    LibraryIndex index({}, testRegistry());
    index.setBuiltIns(someBuiltIns());
    index.addPlace(temp.folder);
    index.scan(temp.folder);

    // Exact name first, then names starting with it, then anything with the word.
    auto found = names(index.find({.text = "synth"}));
    REQUIRE(found.size() >= 4);
    CHECK(found[0] == "Synth");
    CHECK(found[1] == "Synth Stab");
    CHECK(found.indexOf("Old Synth Loop") > 1);
    CHECK(found.contains("Warm Pad")); // a Synth preset: the device's name is one of its words

    // As you type: every word matches as a word start, on any field.
    CHECK(names(index.find({.text = "sy"})).contains("Synth"));
    CHECK(names(index.find({.text = "old loop"})) == juce::StringArray{"Old Synth Loop"});
    CHECK(names(index.find({.text = "pad"}))[0] == "Pad");
    CHECK(names(index.find({.text = "pads"})).contains("Airy")); // its folder's name
    CHECK(names(index.find({.text = "44.1"})).contains("Pad A4"));
    CHECK(names(index.find({.text = "stereo"})) == juce::StringArray{"Pad A4"});
    CHECK(names(index.find({.text = "scl"})) == juce::StringArray{"Five"});
    CHECK(index.find({.text = "zzzz"}).empty());
    CHECK(index.find({.text = "  ,  "}).size() == index.find({}).size()); // nothing searchable typed

    // Narrowing: a category, a collection.
    CHECK(names(index.find({.text = "synth", .category = category::samples})) ==
          juce::StringArray{"Synth Stab", "Old Synth Loop"});
    CHECK(names(index.find({.category = category::midiEffects})) == juce::StringArray{"Chord"});
    // Drums: the kits, and the Drum Rack itself.
    CHECK(names(index.find({.category = category::drums})) == juce::StringArray{"Drum Rack", "Studio Kit"});
    index.setInCollection("device:lattice.reverb", 0, true);
    index.setInCollection((temp / "Kick 01.wav").getFullPathName(), 0, true);
    index.setInCollection((temp / "Kick 01.wav").getFullPathName(), 3, true);
    CHECK(names(index.find({.collection = 0})) == juce::StringArray{"Reverb", "Kick 01"});
    CHECK(names(index.find({.collection = 3})) == juce::StringArray{"Kick 01"});
    auto favourites = names(index.find({.text = "favourites"}));
    favourites.sort(true);
    CHECK(favourites == juce::StringArray{"Kick 01", "Reverb"});
    index.setInCollection("device:lattice.reverb", 0, false);
    CHECK(names(index.find({.collection = 0})) == juce::StringArray{"Kick 01"});
    CHECK(index.find({.text = "kick"}).front().inCollection(3));

    // Limits.
    CHECK(index.find({.limit = 3}).size() == 3);
}

TEST_CASE("Library: the match expression quotes each word as a prefix", "[library]")
{
    CHECK(LibraryIndex::matchExpression("Amp Rel") == "\"amp\"* \"rel\"*");
    CHECK(LibraryIndex::matchExpression("kick-01.wav") == "\"kick\"* \"01\"* \"wav\"*");
    CHECK(LibraryIndex::matchExpression("\"; DROP TABLE items; --") == "\"drop\"* \"table\"* \"items\"*");
    CHECK(LibraryIndex::matchExpression(" ") == "");
}

TEST_CASE("Library: the index survives being opened again, and a damaged one is rebuilt", "[library]")
{
    TempFolder temp("lattice-library");
    makeFixedFolder(temp / "Place");
    const auto file = temp / "index.sqlite";
    {
        LibraryIndex index(file, testRegistry());
        REQUIRE(index.openResult().wasOk());
        index.addPlace(temp / "Place");
        index.scan(temp / "Place");
        index.setInCollection("device:lattice.synth", 2, true);
    }
    {
        LibraryIndex index(file, testRegistry());
        CHECK(index.places() == juce::StringArray{(temp / "Place").getFullPathName()});
        CHECK(index.find({}).size() == 7);
    }
    REQUIRE(file.replaceWithText("not a database at all, but quite long enough to look like a header of one"));
    juce::File(file.getFullPathName() + "-wal").deleteFile();
    LibraryIndex index(file, testRegistry());
    CHECK(index.openResult().wasOk());
    CHECK(index.find({}).empty());
}

TEST_CASE("Library: the watcher notices files added, removed and renamed", "[library][watcher]")
{
    TempFolder temp("lattice-watch");
    (temp / "Sub").createDirectory();
    platform::FileWatcher watcher;
    std::atomic<int> told{0};
    watcher.onChange = [&] { ++told; };
    watcher.watch(temp.folder);
    CHECK(watcher.watched().size() == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the watch is set up on its thread

    const auto waitForChange = [&](const juce::File& folder) {
        for (int i = 0; i < 400; ++i)
        {
            for (const auto& f : watcher.takeChanges())
                if (f == folder)
                    return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };

    REQUIRE(writeWav(temp / "Sub/New.wav", 0.1));
    CHECK(waitForChange(temp / "Sub"));
    REQUIRE((temp / "Sub/New.wav").moveFileTo(temp / "Sub/Renamed.wav"));
    CHECK(waitForChange(temp / "Sub"));
    REQUIRE((temp / "Sub/Renamed.wav").deleteFile());
    CHECK(waitForChange(temp / "Sub"));
    // A folder made after the watch started is watched too.
    REQUIRE((temp / "Later").createDirectory());
    CHECK(waitForChange(temp.folder));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(writeWav(temp / "Later/Inside.wav", 0.1));
    CHECK(waitForChange(temp / "Later"));
    CHECK(told > 0);

    watcher.unwatch(temp.folder);
    CHECK(watcher.watched().isEmpty());
}

TEST_CASE("Library: files added to a place show up without a rescan by hand", "[library][watcher]")
{
    TempFolder temp("lattice-library");
    makeFixedFolder(temp / "Place");
    TuningRegistry tunings;
    JobPool jobs(2);
    presets::PresetLibrary presetLibrary(temp / "User/Library/Presets", testRegistry());
    Library library(temp / "User", {}, testRegistry(), presetLibrary, tunings, jobs);
    int messages = 0;
    struct Counter : juce::ChangeListener
    {
        int& n;
        explicit Counter(int& c) : n(c) {}
        void changeListenerCallback(juce::ChangeBroadcaster*) override { ++n; }
    } counter(messages);
    library.addChangeListener(&counter);

    // Built in: the registry's devices (not the hidden test ones), the tunings.
    auto& index = library.index();
    CHECK(byName(index.find({}), "Synth") != nullptr);
    CHECK(byName(index.find({}), "Test Gain") == nullptr);
    CHECK(byName(index.find({}), "Colundi") != nullptr);
    // Racks go with what they hold.
    CHECK(byName(index.find({.category = category::instruments}), "Instrument Rack") != nullptr);
    CHECK(byName(index.find({.category = category::instruments}), "Drum Rack") != nullptr);
    CHECK(byName(index.find({.category = category::audioEffects}), "Audio Effect Rack") != nullptr);
    CHECK(byName(index.find({.category = category::drums}), "Drum Rack") != nullptr);
    CHECK(library.places().getFirst() == temp / "User/Library");

    library.addPlace(temp / "Place");
    CHECK(waitFor(library, jobs, [&] { return byName(index.find({}), "Kick 01") != nullptr && !library.scanning(); }));
    CHECK(messages > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    REQUIRE(writeWav(temp / "Place/Drums/Tom 05.wav", 0.2));
    CHECK(waitFor(library, jobs, [&] { return byName(index.find({}), "Tom 05") != nullptr; }));
    REQUIRE((temp / "Place/Drums/Tom 05.wav").moveFileTo(temp / "Place/Drums/Tom 06.wav"));
    CHECK(waitFor(library, jobs, [&] {
        const auto all = index.find({});
        return byName(all, "Tom 06") != nullptr && byName(all, "Tom 05") == nullptr;
    }));
    REQUIRE((temp / "Place/Drums/Tom 06.wav").deleteFile());
    CHECK(waitFor(library, jobs, [&] { return byName(index.find({}), "Tom 06") == nullptr; }));

    // A preset saved goes to the User Library, which is watched too.
    LiveSet set;
    set.resetToDefault(1, 1);
    const auto reverb = set.insertDevice(LiveSet::chainOf(set.track(0)), -1, "lattice.reverb");
    juce::File written;
    REQUIRE(presetLibrary.saveUserPreset(reverb, "Big Hall", written).wasOk());
    CHECK(waitFor(library, jobs, [&] { return byName(index.find({}), "Big Hall") != nullptr; }));

    library.removePlace(temp / "Place");
    CHECK(byName(index.find({}), "Kick 01") == nullptr);
    library.removeChangeListener(&counter);
}

TEST_CASE("Library: waveform peaks from the file match the decoded sample's, and are kept on disk", "[library][peaks]")
{
    TempFolder temp("lattice-peaks");
    const auto wav = temp / "Tone.wav";
    REQUIRE(writeWav(wav, 3.0, 48000.0, 2));
    const auto sample = SamplePool::shared().loadNow(wav);
    REQUIRE(sample != nullptr);
    const auto fromMemory = WaveformPeaks::build(*sample, {});
    std::unique_ptr<juce::AudioFormatReader> reader(SamplePool::formats().createReaderFor(wav));
    REQUIRE(reader != nullptr);
    const auto fromFile = WaveformPeaks::build(*reader, {});
    REQUIRE(fromMemory != nullptr);
    REQUIRE(fromFile != nullptr);
    CHECK(fromFile->length == fromMemory->length);
    CHECK(fromFile->levels.size() == fromMemory->levels.size());
    for (const double at : {0.0, 1000.0, 50000.0, 140000.0})
    {
        const auto a = fromMemory->range(at, at + 4096.0);
        const auto b = fromFile->range(at, at + 4096.0);
        CHECK(a.first == Catch::Approx(b.first).margin(1e-4));
        CHECK(a.second == Catch::Approx(b.second).margin(1e-4));
    }

    PeakCache cache(temp / "Peaks");
    CHECK(cache.load(wav) == nullptr);
    const auto got = cache.get(wav);
    REQUIRE(got != nullptr);
    CHECK(cache.fileFor(wav).existsAsFile());
    const auto loaded = cache.load(wav);
    REQUIRE(loaded != nullptr);
    CHECK(loaded->length == got->length);
    CHECK(loaded->sampleRate == 48000.0);
    CHECK(loaded->levels.size() == got->levels.size());
    const auto r1 = got->range(10000.0, 20000.0);
    const auto r2 = loaded->range(10000.0, 20000.0);
    CHECK(r1.second == Catch::Approx(r2.second).margin(1.0 / 16000.0));

    // A changed file gets new peaks; pruning keeps the cache under its size.
    REQUIRE(writeWav(wav, 1.0));
    wav.setLastModificationTime(juce::Time::getCurrentTime() + juce::RelativeTime::seconds(10));
    CHECK(cache.load(wav) == nullptr);
    cache.get(wav);
    CHECK(cache.folder().getNumberOfChildFiles(juce::File::findFiles, "*.peaks") == 2);
    // The new one is the one used last (dates can be a second coarse on some file systems).
    cache.fileFor(wav).setLastModificationTime(juce::Time::getCurrentTime() + juce::RelativeTime::seconds(60));
    cache.prune(cache.fileFor(wav).getSize());
    CHECK(cache.folder().getNumberOfChildFiles(juce::File::findFiles, "*.peaks") == 1);
}

TEST_CASE("Preview bus: a sound plays after the master, resampled, and switching fades", "[library][preview][engine]")
{
    AudioEngine engine;
    engine.prepare(48000.0, 512);
    auto& preview = engine.preview();
    preview.setGain(1.0f);
    std::vector<float> left(512), right(512);
    float* outs[2] = {left.data(), right.data()};

    // A 24 kHz mono ramp: at 48 kHz it plays at half speed, and twice as long.
    std::vector<float> ramp(2400);
    for (std::size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = 0.5f;
    const auto sound = SampleData::fromChannels({ramp}, 24000.0, "Ramp");
    preview.play(sound);
    int frames = 0;
    float peak = 0.0f;
    for (int block = 0; block < 20; ++block)
    {
        engine.process(outs, 2, 512);
        bool any = false;
        for (int i = 0; i < 512; ++i)
        {
            peak = std::max(peak, left[static_cast<std::size_t>(i)]);
            any = any || left[static_cast<std::size_t>(i)] != 0.0f;
            frames += left[static_cast<std::size_t>(i)] != 0.0f ? 1 : 0;
            CHECK(left[static_cast<std::size_t>(i)] == right[static_cast<std::size_t>(i)]); // mono on both sides
        }
        if (!any && block > 0)
            break;
    }
    CHECK(peak == Catch::Approx(0.5f).margin(0.01f));
    CHECK(frames == Catch::Approx(4800).margin(4));
    CHECK_FALSE(preview.isPlaying());

    // Switching mid-sound: the old one fades out rather than stopping dead.
    std::vector<float> loud(48000, 0.5f);
    preview.play(SampleData::fromChannels({loud}, 48000.0));
    engine.process(outs, 2, 512);
    CHECK(preview.isPlaying());
    CHECK(preview.position() >= 512);
    preview.stop();
    engine.process(outs, 2, 512);
    CHECK(left[0] > 0.4f);                       // still sounding as the fade starts
    CHECK(left[PreviewPlayer::fadeFrames] == 0.0f); // and gone after it
    engine.process(outs, 2, 512);
    CHECK_FALSE(preview.isPlaying());

    // The sounds the audio thread is done with are freed.
    engine.collectGarbage();
    CHECK(preview.soundsKept() == 1);
}

TEST_CASE("Preview bus: synced, a sound waits for the next bar while the set plays", "[library][preview][engine]")
{
    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session(engine, tunings, testRegistry());
    session.newSet(1, 1);
    engine.prepare(48000.0, 512);
    std::vector<float> left(512), right(512);
    float* outs[2] = {left.data(), right.data()};
    session.play();
    for (int i = 0; i < 10; ++i)
        engine.process(outs, 2, 512); // a little way into the first bar
    const auto playedBefore = engine.session().currentSample();
    engine.preview().setGain(1.0f);
    engine.preview().play(SampleData::fromChannels({std::vector<float>(4800, 0.5f)}, 48000.0), true);
    // 120 BPM in 4/4: a bar is 96000 samples.
    std::int64_t firstSound = -1;
    for (int block = 0; block < 400 && firstSound < 0; ++block)
    {
        const auto start = engine.session().currentSample();
        std::fill(left.begin(), left.end(), 0.0f);
        engine.process(outs, 2, 512);
        for (int i = 0; i < 512; ++i)
            if (left[static_cast<std::size_t>(i)] > 0.25f)
            {
                firstSound = start + i;
                break;
            }
    }
    CHECK(playedBefore > 0);
    CHECK(firstSound == 96000);
    session.stop();
}

TEST_CASE("Preset previews: an instrument preset plays a phrase, rendered off the message thread", "[library][preview]")
{
    TuningRegistry tunings;
    JobPool jobs(2);
    PresetPreviews previews(tunings, testRegistry(), jobs);
    CHECK(previews.plays("lattice.synth"));
    CHECK(previews.plays("lattice.drumRack"));
    CHECK_FALSE(previews.plays("lattice.reverb"));

    LiveSet set;
    set.resetToDefault(1, 1);
    const auto synth = set.insertDevice(LiveSet::chainOf(set.track(0)), -1, "lattice.synth");
    SamplePtr got;
    int calls = 0;
    previews.request("synth-test", synth, 48000.0, [&](SamplePtr s) {
        got = std::move(s);
        ++calls;
    });
    for (int i = 0; i < 500 && calls == 0; ++i)
    {
        jobs.deliverFinished();
        SamplePool::shared().deliverFinished();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(got != nullptr);
    CHECK(got->sampleRate == 48000.0);
    CHECK(got->length == static_cast<std::int64_t>(PresetPreviews::seconds * 48000.0));
    float peak = 0.0f;
    for (std::int64_t i = 0; i < got->length; ++i)
        peak = std::max(peak, std::abs(got->channel(0)[i]));
    CHECK(peak > 0.01f); // it played
    CHECK(std::abs(got->channel(0)[got->length - 1]) < 1e-3f); // and fades out at the end

    // Asked again: the same rendering, at once.
    SamplePtr again;
    previews.request("synth-test", synth, 48000.0, [&](SamplePtr s) { again = std::move(s); });
    CHECK(again == got);
    CHECK_FALSE(previews.busy());
}
