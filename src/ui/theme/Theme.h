#pragma once

// Theme tokens (docs/05-m4-ui.md, "Theme and scaling"): every colour, font size, spacing and corner
// radius the UI draws with comes from a Theme. Nothing is hard-coded inside components.

#include <juce_graphics/juce_graphics.h>

namespace lattice::ui
{

// X(name, purpose). The names are also the keys of a theme's JSON file, so they never change once
// released (a user's theme file refers to them).
#define LATTICE_THEME_COLOURS(X)                                                                   \
    X(background, "behind everything")                                                             \
    X(surface, "panels: the browser, device panels, mixer strips")                                 \
    X(surfaceRaised, "things on a panel: buttons, clip slots, headers")                            \
    X(surfaceSunken, "wells: number boxes, meters, text fields")                                   \
    X(border, "outlines and dividers")                                                             \
    X(text, "main text")                                                                           \
    X(textDim, "labels and secondary text")                                                        \
    X(textDisabled, "text of controls that are off")                                               \
    X(accent, "a control's value: knob arcs, fader fills, selected items")                         \
    X(accentText, "text on the accent colour")                                                     \
    X(knobTrack, "the unfilled part of a knob's arc or a fader's track")                           \
    X(focus, "the keyboard focus ring")                                                            \
    X(selection, "selected clips, tracks and notes")                                               \
    X(play, "playing clips, the play button")                                                      \
    X(queued, "clips waiting for their launch")                                                    \
    X(record, "recording, the record button")                                                      \
    X(arm, "armed tracks")                                                                         \
    X(solo, "solo")                                                                                \
    X(mute, "a track's on switch (lit when the track is on)")                                      \
    X(meterLow, "meters below -18 dB")                                                             \
    X(meterMid, "meters from -18 to -6 dB")                                                        \
    X(meterHigh, "meters from -6 to 0 dB")                                                         \
    X(meterClip, "meters over 0 dB, and the clip light")                                           \
    X(warning, "warnings")                                                                         \
    X(error, "errors")

// X(name, purpose). In logical pixels at 100% zoom; the UI zoom scales everything.
#define LATTICE_THEME_SIZES(X)                                                                     \
    X(fontSmall, "labels under and over controls")                                                 \
    X(fontNormal, "most text")                                                                     \
    X(fontLarge, "headings")                                                                       \
    X(spacing, "the gap between controls")                                                         \
    X(padding, "inside panels")                                                                    \
    X(radius, "corner radius of boxes and buttons")                                                \
    X(stroke, "outline width")                                                                     \
    X(knobArc, "width of a knob's value arc")                                                      \
    X(controlHeight, "height of buttons, number boxes and menus")

struct Theme
{
#define LATTICE_THEME_DECLARE_COLOUR(token, purpose) juce::Colour token;
    LATTICE_THEME_COLOURS(LATTICE_THEME_DECLARE_COLOUR)
#undef LATTICE_THEME_DECLARE_COLOUR
#define LATTICE_THEME_DECLARE_SIZE(token, purpose) float token = 0.0f;
    LATTICE_THEME_SIZES(LATTICE_THEME_DECLARE_SIZE)
#undef LATTICE_THEME_DECLARE_SIZE

    juce::String name;

    // --- Built-in themes ---------------------------------------------------------------------------
    static const Theme& dark();         // the default
    static const Theme& light();
    static const Theme& highContrast();
    /** The built-in theme with this name ("Dark", "Light", "High Contrast"), or nullptr. */
    static const Theme* builtIn(const juce::String& themeName);

    // --- User themes (JSON) ------------------------------------------------------------------------
    /** Reads a user theme:
        { "name": "...", "base": "Dark", "colours": { "accent": "#f2a33a", ... }, "sizes": { ... } }
        Tokens not in the file come from the base theme (Dark if none), so an old file still works
        after new tokens are added. Colours are "#rrggbb" or "#aarrggbb". Unknown keys and bad
        values are reported in `problems` and skipped. */
    static Theme fromJson(const juce::var& json, juce::StringArray& problems);
    /** Every token, in the format fromJson() reads. */
    juce::var toJson() const;

    bool operator==(const Theme& other) const;

    juce::FontOptions font(float size) const { return juce::FontOptions(size); }
};

/** WCAG 2 contrast ratio between two colours (1 to 21), for the readability checks. */
double contrastRatio(juce::Colour a, juce::Colour b);

} // namespace lattice::ui
