#include "control/ComputerKeyboardInput.h"
#include "tuning/Tet12.h"
#include "tuning/TuningRegistry.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace lattice;

namespace
{
struct Recorder
{
    std::vector<juce::MidiMessage> messages;

    ComputerKeyboardInput::Sender sender()
    {
        return [this](const juce::MidiMessage& m) { messages.push_back(m); };
    }

    int noteOns() const
    {
        int n = 0;
        for (const auto& m : messages)
            n += m.isNoteOn() ? 1 : 0;
        return n;
    }

    int noteOffs() const
    {
        int n = 0;
        for (const auto& m : messages)
            n += m.isNoteOff() ? 1 : 0;
        return n;
    }
};

// Key codes are arbitrary per platform; the tests use the character's code.
bool press(ComputerKeyboardInput& kb, char c)
{
    return kb.keyDown(static_cast<juce::juce_wchar>(c), static_cast<int>(c));
}
} // namespace

TEST_CASE("Keyboard mapping: 17 keys play 17 consecutive notes", "[control][keyboard]")
{
    const char* keys = ComputerKeyboardInput::noteKeys;
    for (int i = 0; i < ComputerKeyboardInput::numNoteKeys; ++i)
        CHECK(ComputerKeyboardInput::noteForKey(static_cast<juce::juce_wchar>(keys[i]), 60) == 60 + i);

    CHECK(ComputerKeyboardInput::noteForKey('a', 60) == 60);
    CHECK(ComputerKeyboardInput::noteForKey(';', 60) == 76);
    CHECK(ComputerKeyboardInput::noteForKey('A', 60) == 60); // case-insensitive
    CHECK_FALSE(ComputerKeyboardInput::noteForKey('q', 60).has_value());
    CHECK_FALSE(ComputerKeyboardInput::noteForKey('z', 60).has_value());
    CHECK_FALSE(ComputerKeyboardInput::noteForKey(';', 120).has_value()); // 136 is off the table
    CHECK(ComputerKeyboardInput::noteForKey('y', 119) == 127); // 9th key
    CHECK_FALSE(ComputerKeyboardInput::noteForKey('h', 119).has_value());
}

TEST_CASE("Keyboard: 12-TET range starts at C4 and Z/X move by 12", "[control][keyboard]")
{
    Recorder rec;
    ComputerKeyboardInput kb(rec.sender());
    kb.configureForTuning(*Tet12::make());
    CHECK(kb.getBaseNote() == 60);
    CHECK(kb.getPageSize() == 12);

    press(kb, 'a');
    REQUIRE(rec.messages.size() == 1);
    CHECK(rec.messages[0].getNoteNumber() == 60);
    CHECK(rec.messages[0].getVelocity() == ComputerKeyboardInput::defaultVelocity);

    press(kb, 'x'); // releases held notes, then moves up
    CHECK(rec.noteOffs() == 1);
    CHECK(kb.getBaseNote() == 72);

    for (int i = 0; i < 20; ++i)
        press(kb, 'x');
    CHECK(kb.getBaseNote() == 120); // stays within 0..127
    for (int i = 0; i < 20; ++i)
        press(kb, 'z');
    CHECK(kb.getBaseNote() == 0);
}

TEST_CASE("Keyboard: Colundi pages of 17 cover the whole table", "[control][keyboard]")
{
    Recorder rec;
    ComputerKeyboardInput kb(rec.sender());
    const auto colundi = TuningRegistry::makeBuiltinColundi();
    kb.configureForTuning(*colundi);
    CHECK(kb.getPageSize() == 17);
    CHECK(kb.getBaseNote() % 17 == 0);

    while (kb.getBaseNote() > 0)
        press(kb, 'z');

    // Play every key on every page: each entry 0..127 sounds exactly once.
    std::vector<int> played;
    for (int page = 0; page < 8; ++page) // pages start at 0, 17, ..., 119
    {
        rec.messages.clear();
        for (const char* k = ComputerKeyboardInput::noteKeys; *k != 0; ++k)
        {
            press(kb, *k);
            kb.keyUp(static_cast<int>(*k));
        }
        for (const auto& m : rec.messages)
            if (m.isNoteOn())
                played.push_back(m.getNoteNumber());
        press(kb, 'x');
    }
    CHECK(kb.getBaseNote() == 119); // X on the last page stays there
    REQUIRE(played.size() == 128);
    for (int i = 0; i < 128; ++i)
        CHECK(played[static_cast<std::size_t>(i)] == i);
}

TEST_CASE("Keyboard: velocity stays within 1..127", "[control][keyboard]")
{
    Recorder rec;
    ComputerKeyboardInput kb(rec.sender());
    for (int i = 0; i < 10; ++i)
        press(kb, 'v');
    CHECK(kb.getVelocity() == 127);
    for (int i = 0; i < 10; ++i)
        press(kb, 'c');
    CHECK(kb.getVelocity() == 1);
    press(kb, 'v');
    CHECK(kb.getVelocity() == 21);
}

TEST_CASE("Keyboard: auto-repeat doesn't retrigger a held note", "[control][keyboard]")
{
    Recorder rec;
    ComputerKeyboardInput kb(rec.sender());
    press(kb, 's');
    press(kb, 's');
    press(kb, 's');
    CHECK(rec.noteOns() == 1);
    kb.keyUp('s');
    CHECK(rec.noteOffs() == 1);
    press(kb, 's');
    CHECK(rec.noteOns() == 2);
}

TEST_CASE("Keyboard: turning off, losing focus, or shifting releases held notes", "[control][keyboard]")
{
    Recorder rec;
    ComputerKeyboardInput kb(rec.sender());

    SECTION("turning off with M")
    {
        press(kb, 'a');
        press(kb, 'd');
        press(kb, 'm');
        CHECK_FALSE(kb.isEnabled());
        CHECK(rec.noteOffs() == 2);
        CHECK_FALSE(press(kb, 'a')); // off: note keys pass through
        CHECK(rec.noteOns() == 2);
        press(kb, 'm');
        CHECK(kb.isEnabled());
    }

    SECTION("losing focus")
    {
        press(kb, 'a');
        kb.releaseAll();
        CHECK(rec.noteOffs() == 1);
        CHECK(kb.heldKeyCodes().empty());
    }

    SECTION("shifting the range")
    {
        press(kb, 'a');
        press(kb, 'z');
        CHECK(rec.noteOffs() == 1);
    }

    SECTION("a released key after a shift sends nothing extra")
    {
        press(kb, 'a');
        press(kb, 'x');
        kb.keyUp('a');
        CHECK(rec.noteOffs() == 1);
    }
}
