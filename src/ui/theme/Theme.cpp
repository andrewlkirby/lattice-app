#include "ui/theme/Theme.h"

#include <cmath>

namespace lattice::ui
{
namespace
{
void setSizes(Theme& t)
{
    t.fontSmall = 11.0f;
    t.fontNormal = 13.0f;
    t.fontLarge = 16.0f;
    t.spacing = 4.0f;
    t.padding = 6.0f;
    t.radius = 3.0f;
    t.stroke = 1.0f;
    t.knobArc = 3.0f;
    t.controlHeight = 22.0f;
}

Theme makeDark()
{
    Theme t;
    t.name = "Dark";
    t.background = juce::Colour(0xff1d1f23);
    t.surface = juce::Colour(0xff2a2d33);
    t.surfaceRaised = juce::Colour(0xff363a42);
    t.surfaceSunken = juce::Colour(0xff17191c);
    t.border = juce::Colour(0xff474c55);
    t.text = juce::Colour(0xffe4e6e9);
    t.textDim = juce::Colour(0xffa2a8b0);
    t.textDisabled = juce::Colour(0xff656a73);
    t.accent = juce::Colour(0xfff2a33a);
    t.accentText = juce::Colour(0xff1d1f23);
    t.knobTrack = juce::Colour(0xff4a4f58);
    t.focus = juce::Colour(0xff6cb6ff);
    t.selection = juce::Colour(0xff7fb4ff);
    t.play = juce::Colour(0xff48d17a);
    t.queued = juce::Colour(0xffa3e8bd);
    t.record = juce::Colour(0xffff5a52);
    t.arm = juce::Colour(0xffff5a52);
    t.solo = juce::Colour(0xff4aa8ff);
    t.mute = juce::Colour(0xffffc94a);
    t.meterLow = juce::Colour(0xff3fcf72);
    t.meterMid = juce::Colour(0xffe6d84b);
    t.meterHigh = juce::Colour(0xffff9d3e);
    t.meterClip = juce::Colour(0xffff3b3b);
    t.warning = juce::Colour(0xffffb347);
    t.error = juce::Colour(0xffff6159);
    setSizes(t);
    return t;
}

Theme makeLight()
{
    Theme t;
    t.name = "Light";
    t.background = juce::Colour(0xffe6e7e9);
    t.surface = juce::Colour(0xfff4f4f5);
    t.surfaceRaised = juce::Colour(0xffffffff);
    t.surfaceSunken = juce::Colour(0xffd9dbde);
    t.border = juce::Colour(0xffa9aeb5);
    t.text = juce::Colour(0xff1c1f23);
    t.textDim = juce::Colour(0xff555b63);
    t.textDisabled = juce::Colour(0xff8f959c);
    t.accent = juce::Colour(0xffa8580a);
    t.accentText = juce::Colour(0xffffffff);
    t.knobTrack = juce::Colour(0xffbfc3c9);
    t.focus = juce::Colour(0xff2a72d4);
    t.selection = juce::Colour(0xff2a72d4);
    t.play = juce::Colour(0xff1e9650);
    t.queued = juce::Colour(0xff6cc58f);
    t.record = juce::Colour(0xffd93430);
    t.arm = juce::Colour(0xffd93430);
    t.solo = juce::Colour(0xff1c74d6);
    t.mute = juce::Colour(0xffd9a000);
    t.meterLow = juce::Colour(0xff25a85a);
    t.meterMid = juce::Colour(0xffc9b520);
    t.meterHigh = juce::Colour(0xffe07a1c);
    t.meterClip = juce::Colour(0xffe02a2a);
    t.warning = juce::Colour(0xffb86e00);
    t.error = juce::Colour(0xffc62828);
    setSizes(t);
    return t;
}

Theme makeHighContrast()
{
    Theme t;
    t.name = "High Contrast";
    t.background = juce::Colour(0xff000000);
    t.surface = juce::Colour(0xff0b0b0b);
    t.surfaceRaised = juce::Colour(0xff1c1c1c);
    t.surfaceSunken = juce::Colour(0xff000000);
    t.border = juce::Colour(0xffffffff);
    t.text = juce::Colour(0xffffffff);
    t.textDim = juce::Colour(0xffe2e2e2);
    t.textDisabled = juce::Colour(0xff9a9a9a);
    t.accent = juce::Colour(0xffffd400);
    t.accentText = juce::Colour(0xff000000);
    t.knobTrack = juce::Colour(0xff707070);
    t.focus = juce::Colour(0xff00e5ff);
    t.selection = juce::Colour(0xff00e5ff);
    t.play = juce::Colour(0xff00ff6a);
    t.queued = juce::Colour(0xffa6ffcb);
    t.record = juce::Colour(0xffff3b3b);
    t.arm = juce::Colour(0xffff3b3b);
    t.solo = juce::Colour(0xff3bb8ff);
    t.mute = juce::Colour(0xffffd400);
    t.meterLow = juce::Colour(0xff00ff6a);
    t.meterMid = juce::Colour(0xffffff3b);
    t.meterHigh = juce::Colour(0xffffa31a);
    t.meterClip = juce::Colour(0xffff3b3b);
    t.warning = juce::Colour(0xffffb000);
    t.error = juce::Colour(0xffff4d4d);
    setSizes(t);
    t.stroke = 1.5f;
    t.knobArc = 4.0f;
    return t;
}

bool parseColour(const juce::String& text, juce::Colour& out)
{
    auto hex = text.trim();
    if (!hex.startsWithChar('#'))
        return false;
    hex = hex.substring(1);
    if ((hex.length() != 6 && hex.length() != 8) || !hex.containsOnly("0123456789abcdefABCDEF"))
        return false;
    auto argb = static_cast<juce::uint32>(hex.getHexValue64());
    if (hex.length() == 6)
        argb |= 0xff000000u;
    out = juce::Colour(argb);
    return true;
}

juce::String colourText(juce::Colour c)
{
    return "#" + (c.getAlpha() == 0xff ? c.toDisplayString(false) : c.toDisplayString(true)).toLowerCase();
}

double linearChannel(juce::uint8 v)
{
    const double c = v / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(juce::Colour c)
{
    return 0.2126 * linearChannel(c.getRed()) + 0.7152 * linearChannel(c.getGreen()) + 0.0722 * linearChannel(c.getBlue());
}
} // namespace

const Theme& Theme::dark()
{
    static const Theme t = makeDark();
    return t;
}

const Theme& Theme::light()
{
    static const Theme t = makeLight();
    return t;
}

const Theme& Theme::highContrast()
{
    static const Theme t = makeHighContrast();
    return t;
}

const Theme* Theme::builtIn(const juce::String& themeName)
{
    for (const auto* t : {&dark(), &light(), &highContrast()})
        if (t->name.equalsIgnoreCase(themeName))
            return t;
    return nullptr;
}

Theme Theme::fromJson(const juce::var& json, juce::StringArray& problems)
{
    const auto* object = json.getDynamicObject();
    if (object == nullptr)
    {
        problems.add("The theme isn't a JSON object");
        return dark();
    }

    const auto baseName = json.getProperty("base", "Dark").toString();
    const auto* base = builtIn(baseName);
    if (base == nullptr)
    {
        problems.add("Unknown base theme \"" + baseName + "\"; using Dark");
        base = &dark();
    }
    Theme t = *base;
    t.name = json.getProperty("name", base->name + " (custom)").toString();

    for (const auto& key : object->getProperties())
        if (key.name.toString() != "name" && key.name.toString() != "base" && key.name.toString() != "colours" &&
            key.name.toString() != "sizes")
            problems.add("Unknown key \"" + key.name.toString() + "\"");

    if (const auto* colours = json.getProperty("colours", {}).getDynamicObject())
    {
        for (const auto& entry : colours->getProperties())
        {
            const auto key = entry.name.toString();
            juce::Colour* target = nullptr;
#define LATTICE_THEME_FIND_COLOUR(token, purpose)                                                   \
    if (key == #token)                                                                              \
        target = &t.token;
            LATTICE_THEME_COLOURS(LATTICE_THEME_FIND_COLOUR)
#undef LATTICE_THEME_FIND_COLOUR
            juce::Colour c;
            if (target == nullptr)
                problems.add("Unknown colour \"" + key + "\"");
            else if (!parseColour(entry.value.toString(), c))
                problems.add("Colour \"" + key + "\" isn't #rrggbb or #aarrggbb: \"" + entry.value.toString() + "\"");
            else
                *target = c;
        }
    }

    if (const auto* sizes = json.getProperty("sizes", {}).getDynamicObject())
    {
        for (const auto& entry : sizes->getProperties())
        {
            const auto key = entry.name.toString();
            float* target = nullptr;
#define LATTICE_THEME_FIND_SIZE(token, purpose)                                                     \
    if (key == #token)                                                                              \
        target = &t.token;
            LATTICE_THEME_SIZES(LATTICE_THEME_FIND_SIZE)
#undef LATTICE_THEME_FIND_SIZE
            const bool isNumber = entry.value.isDouble() || entry.value.isInt() || entry.value.isInt64();
            if (target == nullptr)
                problems.add("Unknown size \"" + key + "\"");
            else if (!isNumber || static_cast<double>(entry.value) < 0.0 || static_cast<double>(entry.value) > 200.0)
                problems.add("Size \"" + key + "\" isn't a number from 0 to 200");
            else
                *target = static_cast<float>(static_cast<double>(entry.value));
        }
    }
    return t;
}

juce::var Theme::toJson() const
{
    auto* colours = new juce::DynamicObject();
#define LATTICE_THEME_WRITE_COLOUR(token, purpose) colours->setProperty(#token, colourText(token));
    LATTICE_THEME_COLOURS(LATTICE_THEME_WRITE_COLOUR)
#undef LATTICE_THEME_WRITE_COLOUR
    auto* sizes = new juce::DynamicObject();
#define LATTICE_THEME_WRITE_SIZE(token, purpose) sizes->setProperty(#token, static_cast<double>(token));
    LATTICE_THEME_SIZES(LATTICE_THEME_WRITE_SIZE)
#undef LATTICE_THEME_WRITE_SIZE
    auto* root = new juce::DynamicObject();
    root->setProperty("name", name);
    root->setProperty("colours", juce::var(colours));
    root->setProperty("sizes", juce::var(sizes));
    return juce::var(root);
}

bool Theme::operator==(const Theme& other) const
{
    bool same = name == other.name;
#define LATTICE_THEME_COMPARE(token, purpose) same = same && token == other.token;
    LATTICE_THEME_COLOURS(LATTICE_THEME_COMPARE)
    LATTICE_THEME_SIZES(LATTICE_THEME_COMPARE)
#undef LATTICE_THEME_COMPARE
    return same;
}

double contrastRatio(juce::Colour a, juce::Colour b)
{
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

} // namespace lattice::ui
