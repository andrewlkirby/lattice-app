#include "control/surface/Surface.h"

#include <array>
#include <cmath>

namespace lattice::control
{

namespace
{
// Stable forever once released: definition files and settings refer to them.
constexpr std::array<const char*, static_cast<std::size_t>(Role::count)> roleNames{
    "play",        "record",      "shift",       "mode.session", "mode.note",    "mode.device", "mode.mix",
    "mode.browse", "mode.clip",   "nav.up",      "nav.down",     "nav.left",     "nav.right",   "scene",
    "trackSelect", "displayUpper", "displayLower", "octave.up",  "octave.down",  "layout",      "scale",
    "fixedLength", "duplicate",   "delete",      "undo",         "quantize",     "doubleLoop",  "new",
    "metronome",   "tapTempo",    "stopClip",    "mute",         "solo",         "stopAll"};
} // namespace

const char* roleName(Role role) noexcept
{
    const auto i = static_cast<std::size_t>(role);
    return i < roleNames.size() ? roleNames[i] : "";
}

std::optional<Role> roleFromName(juce::StringRef name)
{
    for (std::size_t i = 0; i < roleNames.size(); ++i)
        if (name == juce::StringRef(roleNames[i]))
            return static_cast<Role>(i);
    return std::nullopt;
}

bool isNumbered(Role role) noexcept
{
    return role == Role::scene || role == Role::trackSelect || role == Role::displayUpper || role == Role::displayLower;
}

juce::String buttonName(ButtonId id)
{
    juce::String name(roleName(id.role));
    if (isNumbered(id.role))
        name << "[" << id.index << "]";
    return name;
}

bool SurfaceCapabilities::has(ButtonId id) const noexcept
{
    return indexOf(id) >= 0;
}

bool SurfaceCapabilities::has(Role role) const noexcept
{
    for (const auto& b : buttons)
        if (b.id.role == role)
            return true;
    return false;
}

int SurfaceCapabilities::countOf(Role role) const noexcept
{
    int n = 0;
    while (has(ButtonId{role, n}))
        ++n;
    return n;
}

int SurfaceCapabilities::indexOf(ButtonId id) const noexcept
{
    for (std::size_t i = 0; i < buttons.size(); ++i)
        if (buttons[i].id == id)
            return static_cast<int>(i);
    return -1;
}

float animationLevel(LedState::Animation animation, double beat) noexcept
{
    switch (animation)
    {
    case LedState::Animation::solid:
        return 1.0f;
    case LedState::Animation::pulse:
    {
        // A raised cosine: full at the beat, down to a third half way through.
        const double phase = beat / pulseBeats - std::floor(beat / pulseBeats);
        return static_cast<float>(0.33 + 0.67 * 0.5 * (1.0 + std::cos(phase * juce::MathConstants<double>::twoPi)));
    }
    case LedState::Animation::blink:
    {
        const double phase = beat / blinkBeats - std::floor(beat / blinkBeats);
        return phase < 0.5 ? 1.0f : 0.0f;
    }
    }
    return 1.0f;
}

std::uint32_t dimmed(std::uint32_t rgb, float factor) noexcept
{
    const auto scale = [factor](std::uint32_t channel) {
        return static_cast<std::uint32_t>(juce::jlimit(0.0f, 255.0f, std::round(static_cast<float>(channel) * factor)));
    };
    const auto r = scale((rgb >> 16) & 0xffu), g = scale((rgb >> 8) & 0xffu), b = scale(rgb & 0xffu);
    const auto out = (r << 16) | (g << 8) | b;
    // A dimmed light is still on: never all the way to black.
    return out == 0 && rgb != 0 && factor > 0.0f ? 0x010101u : out;
}

LedFrame LedFrame::blank(const SurfaceCapabilities& caps)
{
    LedFrame f;
    f.pads.resize(static_cast<std::size_t>(caps.numPads()));
    f.buttons.resize(caps.buttons.size());
    return f;
}

LedFrame::Changes LedFrame::changesFrom(const LedFrame& before) const
{
    Changes c;
    const bool samePads = before.pads.size() == pads.size();
    for (std::size_t i = 0; i < pads.size(); ++i)
        if (!samePads || !(pads[i] == before.pads[i]))
            c.pads.push_back(static_cast<int>(i));
    const bool sameButtons = before.buttons.size() == buttons.size();
    for (std::size_t i = 0; i < buttons.size(); ++i)
        if (!sameButtons || !(buttons[i] == before.buttons[i]))
            c.buttons.push_back(static_cast<int>(i));
    return c;
}

bool LedFrame::anyAnimated() const noexcept
{
    for (const auto& p : pads)
        if (!p.isOff() && p.animation != LedState::Animation::solid)
            return true;
    for (const auto& b : buttons)
        if (!b.isOff() && b.animation != LedState::Animation::solid)
            return true;
    return false;
}

} // namespace lattice::control
