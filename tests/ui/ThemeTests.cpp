// Theme tokens (docs/05-m4-ui.md, "Theme and scaling"): the built-in themes set every token and are
// readable, user themes load from JSON with sensible fallbacks, and the look and feel hands the
// theme to JUCE's own components.

#include "ui/theme/LatticeLookAndFeel.h"

#include <catch2/catch_test_macros.hpp>

using namespace lattice::ui;

namespace
{
const Theme* const builtIns[] = {&Theme::dark(), &Theme::light(), &Theme::highContrast()};
}

TEST_CASE("Themes: every built-in theme sets every token", "[ui][theme]")
{
    for (const auto* t : builtIns)
    {
        INFO(t->name);
#define CHECK_COLOUR(token, purpose) CHECK(t->token.getAlpha() == 0xff);
        LATTICE_THEME_COLOURS(CHECK_COLOUR)
#undef CHECK_COLOUR
#define CHECK_SIZE(token, purpose) CHECK(t->token > 0.0f);
        LATTICE_THEME_SIZES(CHECK_SIZE)
#undef CHECK_SIZE
        CHECK(Theme::builtIn(t->name) == t);
    }
    CHECK(Theme::builtIn("high contrast") == &Theme::highContrast()); // names ignore case
    CHECK(Theme::builtIn("Neon") == nullptr);
}

TEST_CASE("Themes: text is readable on every surface (WCAG contrast)", "[ui][theme]")
{
    // WCAG AA: 4.5 for text, 3 for large text and for controls; the high-contrast theme meets AAA (7).
    for (const auto* t : builtIns)
    {
        INFO(t->name);
        const bool high = t == &Theme::highContrast();
        for (const auto surface : {t->background, t->surface, t->surfaceRaised, t->surfaceSunken})
        {
            CHECK(contrastRatio(t->text, surface) >= 7.0);
            CHECK(contrastRatio(t->textDim, surface) >= (high ? 7.0 : 4.5));
            CHECK(contrastRatio(t->textDisabled, surface) >= 2.0); // dim but visible
        }
        CHECK(contrastRatio(t->accentText, t->accent) >= 4.5);
        // A control's value stands out from its unfilled track and from the panel.
        CHECK(contrastRatio(t->accent, t->surface) >= 3.0);
        CHECK(contrastRatio(t->focus, t->surface) >= 3.0);
    }
}

TEST_CASE("Themes: a theme survives a trip through JSON", "[ui][theme]")
{
    for (const auto* t : builtIns)
    {
        INFO(t->name);
        juce::StringArray problems;
        const auto json = juce::JSON::parse(juce::JSON::toString(t->toJson()));
        const auto back = Theme::fromJson(json, problems);
        CHECK(problems.isEmpty());
        CHECK(back == *t);
    }
}

TEST_CASE("Themes: a user theme changes some tokens; the rest come from its base", "[ui][theme]")
{
    juce::StringArray problems;
    const auto json = juce::JSON::parse(R"({ "name": "Warm", "base": "Light",
        "colours": { "accent": "#c04020", "focus": "#802040ff" }, "sizes": { "radius": 6 } })");
    const auto t = Theme::fromJson(json, problems);
    CHECK(problems.isEmpty());
    CHECK(t.name == "Warm");
    CHECK(t.accent == juce::Colour(0xffc04020));
    CHECK(t.focus == juce::Colour(0x802040ff)); // #aarrggbb
    CHECK(t.radius == 6.0f);
    CHECK(t.background == Theme::light().background);
    CHECK(t.fontNormal == Theme::light().fontNormal);

    // No base: Dark.
    const auto plain = Theme::fromJson(juce::JSON::parse(R"({ "colours": { "accent": "#00ff00" } })"), problems);
    CHECK(problems.isEmpty());
    CHECK(plain.surface == Theme::dark().surface);
    CHECK(plain.accent == juce::Colour(0xff00ff00));
}

TEST_CASE("Themes: mistakes in a user theme are reported and skipped", "[ui][theme]")
{
    juce::StringArray problems;
    const auto json = juce::JSON::parse(R"({ "base": "Neon", "colour": {},
        "colours": { "accent": "orange", "glow": "#ffffff", "text": "#123" },
        "sizes": { "radius": "big", "spacing": -2, "padding": 300, "gutter": 4, "stroke": 2 } })");
    const auto t = Theme::fromJson(json, problems);
    // The unknown base, the unknown key "colour", 2 bad colours and an unknown one, 3 bad sizes and
    // an unknown one.
    CHECK(problems.size() == 9);
    CHECK(t.accent == Theme::dark().accent);
    CHECK(t.text == Theme::dark().text);
    CHECK(t.radius == Theme::dark().radius);
    CHECK(t.stroke == 2.0f); // the one good value still applies

    problems.clear();
    CHECK(Theme::fromJson(juce::var("not an object"), problems) == Theme::dark());
    CHECK(problems.size() == 1);
}

TEST_CASE("Themes: the look and feel gives JUCE's components the theme, and widgets find it", "[ui][theme]")
{
    LatticeLookAndFeel laf(Theme::light());
    juce::Component c;
    CHECK(&themeOf(c) == &Theme::dark()); // not a Lattice look and feel: Dark
    c.setLookAndFeel(&laf);
    CHECK(themeOf(c) == Theme::light());
    CHECK(c.findColour(juce::PopupMenu::backgroundColourId) == Theme::light().surfaceRaised);
    CHECK(c.findColour(juce::PopupMenu::textColourId) == Theme::light().text);
    CHECK(c.findColour(juce::TextEditor::focusedOutlineColourId) == Theme::light().focus);

    laf.setTheme(Theme::highContrast());
    CHECK(themeOf(c) == Theme::highContrast());
    CHECK(c.findColour(juce::ResizableWindow::backgroundColourId) == Theme::highContrast().background);
    c.setLookAndFeel(nullptr);
}
