#include "control/push/PushProtocol.h"

#include <algorithm>

namespace lattice::control::push
{

void encodeFrame(const juce::Image& image, std::uint8_t* out)
{
    std::fill(out, out + frameBytes, std::uint8_t{0});
    if (image.isValid())
    {
        const juce::Image::BitmapData pixels(image, juce::Image::BitmapData::readOnly);
        const int w = std::min(displayWidth, pixels.width), h = std::min(displayHeight, pixels.height);
        for (int y = 0; y < h; ++y)
        {
            auto* line = out + y * lineBytes;
            for (int x = 0; x < w; ++x)
            {
                const auto c = pixels.getPixelColour(x, y);
                const auto p = pixel((static_cast<std::uint32_t>(c.getRed()) << 16) | (static_cast<std::uint32_t>(c.getGreen()) << 8) |
                                     c.getBlue());
                line[x * 2] = static_cast<std::uint8_t>(p & 0xffu); // little-endian
                line[x * 2 + 1] = static_cast<std::uint8_t>(p >> 8);
            }
        }
    }
    // The whole buffer, filler too: every line starts on a multiple of four.
    for (int i = 0; i < frameBytes; ++i)
        out[i] ^= xorPattern[static_cast<std::size_t>(i & 3)];
}

std::vector<std::uint8_t> encodeFrame(const juce::Image& image)
{
    std::vector<std::uint8_t> out(frameBytes);
    encodeFrame(image, out.data());
    return out;
}

std::optional<PadPlace> padOf(int note) noexcept
{
    if (note < firstPadNote || note >= firstPadNote + 64)
        return std::nullopt;
    const int i = note - firstPadNote;
    return PadPlace{7 - i / 8, i % 8};
}

float stripPosition(int pitchBend) noexcept
{
    constexpr int middle = 8192, top = 16320;
    if (pitchBend >= middle)
        return 0.5f + 0.5f * static_cast<float>(std::min(pitchBend, top) - middle) / static_cast<float>(top - middle);
    return 0.5f * static_cast<float>(std::max(pitchBend, 0)) / static_cast<float>(middle);
}

const std::vector<Button>& buttons()
{
    static const std::vector<Button> all = [] {
        std::vector<Button> b;
        const auto add = [&](int cc, Role role, bool rgb, int index = 0) { b.push_back({cc, ButtonId{role, index}, rgb}); };
        for (int i = 0; i < 8; ++i)
            add(102 + i, Role::displayUpper, true, i);
        for (int i = 0; i < 8; ++i)
            add(20 + i, Role::displayLower, true, i);
        for (int i = 0; i < 8; ++i)
            add(43 - i, Role::scene, true, i); // scene 1, the top, is CC 43
        add(85, Role::play, true);
        add(86, Role::record, true);
        add(60, Role::mute, true);
        add(61, Role::solo, true);
        add(29, Role::stopClip, true);
        add(87, Role::newClip, false);
        add(88, Role::duplicate, false);
        add(90, Role::fixedLength, false);
        add(116, Role::quantize, false);
        add(117, Role::doubleLoop, false);
        add(118, Role::deleteClip, false);
        add(119, Role::undo, false);
        add(49, Role::shift, false);
        add(51, Role::modeSession, false);
        add(50, Role::modeNote, false);
        add(110, Role::modeDevice, false);
        add(112, Role::modeMix, false);
        add(111, Role::modeBrowse, false);
        add(113, Role::modeClip, false);
        add(46, Role::navUp, false);
        add(47, Role::navDown, false);
        add(44, Role::navLeft, false);
        add(45, Role::navRight, false);
        add(55, Role::octaveUp, false);
        add(54, Role::octaveDown, false);
        add(31, Role::layout, false);
        add(58, Role::scale, false);
        add(9, Role::metronome, false);
        add(3, Role::tapTempo, false);
        return b;
    }();
    return all;
}

const Button* buttonOn(int cc) noexcept
{
    static const auto table = [] {
        std::array<const Button*, 128> t{};
        for (const auto& b : buttons())
            t[static_cast<std::size_t>(b.cc)] = &b;
        return t;
    }();
    return cc >= 0 && cc < 128 ? table[static_cast<std::size_t>(cc)] : nullptr;
}

int ccOf(ButtonId id) noexcept
{
    for (const auto& b : buttons())
        if (b.id == id)
            return b.cc;
    return -1;
}

juce::MidiMessage command(std::uint8_t id, std::initializer_list<std::uint8_t> arguments)
{
    std::vector<std::uint8_t> body{0x00, 0x21, 0x1d, 0x01, 0x01, static_cast<std::uint8_t>(id & 0x7f)};
    for (const auto a : arguments)
        body.push_back(static_cast<std::uint8_t>(a & 0x7f));
    return juce::MidiMessage::createSysExMessage(body.data(), static_cast<int>(body.size()));
}

namespace
{
std::uint8_t low7(int v) noexcept { return static_cast<std::uint8_t>(v & 0x7f); }
std::uint8_t high1(int v) noexcept { return static_cast<std::uint8_t>((v >> 7) & 0x01); }
} // namespace

juce::MidiMessage setPaletteEntry(int index, std::uint32_t rgb, int white)
{
    const int r = static_cast<int>((rgb >> 16) & 0xffu), g = static_cast<int>((rgb >> 8) & 0xffu), b = static_cast<int>(rgb & 0xffu);
    const int w = std::clamp(white, 0, 255);
    return command(0x03, {low7(std::clamp(index, 0, 127)), low7(r), high1(r), low7(g), high1(g), low7(b), high1(b), low7(w), high1(w)});
}

juce::MidiMessage reapplyPalette()
{
    return command(0x05);
}

juce::MidiMessage setLedBrightness(int brightness)
{
    return command(0x06, {low7(std::clamp(brightness, 0, 127))});
}

juce::MidiMessage setDisplayBrightness(int brightness)
{
    const int b = std::clamp(brightness, 0, 255);
    return command(0x08, {low7(b), high1(b)});
}

juce::MidiMessage setMidiMode(MidiMode mode)
{
    return command(0x0a, {static_cast<std::uint8_t>(mode)});
}

juce::MidiMessage setAftertouchMode(bool polyphonic)
{
    return command(0x1e, {static_cast<std::uint8_t>(polyphonic ? 1 : 0)});
}

juce::MidiMessage setTouchStripConfiguration(std::uint8_t flags)
{
    return command(0x17, {flags});
}

juce::MidiMessage identityRequest()
{
    const std::uint8_t body[]{0x7e, 0x7f, 0x06, 0x01};
    return juce::MidiMessage::createSysExMessage(body, 4);
}

bool isPushIdentity(const juce::MidiMessage& reply) noexcept
{
    if (!reply.isSysEx() || reply.getSysExDataSize() < 9)
        return false;
    const auto* d = reply.getSysExData(); // after F0: 7E <id> 06 02 00 21 1D 67 32 ...
    return d[0] == 0x7e && d[2] == 0x06 && d[3] == 0x02 && d[4] == 0x00 && d[5] == 0x21 && d[6] == 0x1d && d[7] == 0x67 && d[8] == 0x32;
}

} // namespace lattice::control::push
