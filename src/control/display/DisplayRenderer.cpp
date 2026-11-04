#include "control/display/DisplayRenderer.h"

#include "ui/widgets/Meter.h"

#include <algorithm>
#include <cmath>

namespace lattice::control
{

namespace
{
// In parts of a cell's height (a Push cell: 120 x 112 between its two 24 px strips).
constexpr float stripShare = 24.0f / 112.0f;
constexpr float cellAspect = 112.0f / 120.0f; // height / width
// A knob's travel, as the UI's knobs draw it.
constexpr float arcFrom = juce::MathConstants<float>::pi * 1.25f;
constexpr float arcTo = juce::MathConstants<float>::pi * 2.75f;

juce::Colour rgbColour(std::uint32_t rgb, juce::Colour fallback)
{
    return rgb == 0 ? fallback : juce::Colour(0xff000000u | rgb);
}

void label(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r, float height, juce::Colour c,
           juce::Justification j = juce::Justification::centred)
{
    if (text.isEmpty() || r.getWidth() < 2.0f || r.getHeight() < 2.0f)
        return;
    g.setColour(c);
    g.setFont(juce::FontOptions(height));
    g.drawFittedText(text, r.toNearestInt(), j, 1, 0.7f);
}

void strip(juce::Graphics& g, const std::vector<DisplayButton>& buttons, const juce::String& title, juce::Rectangle<float> r,
           int across, const DisplayRenderer::Colours& c)
{
    if (r.isEmpty())
        return;
    const float font = r.getHeight() * 0.62f;
    if (buttons.empty())
    {
        label(g, title, r.reduced(r.getHeight() * 0.3f, 0.0f), font, c.text, juce::Justification::centredLeft);
        return;
    }
    const float w = r.getWidth() / static_cast<float>(std::max(1, across));
    for (std::size_t i = 0; i < buttons.size() && static_cast<int>(i) < across; ++i)
    {
        const auto& b = buttons[i];
        if (b.label.isEmpty())
            continue;
        auto box = juce::Rectangle<float>(r.getX() + static_cast<float>(i) * w, r.getY(), w, r.getHeight()).reduced(w * 0.03f, r.getHeight() * 0.1f);
        const auto fill = rgbColour(b.colour, c.accent);
        if (b.selected)
        {
            g.setColour(fill);
            g.fillRoundedRectangle(box, r.getHeight() * 0.12f);
        }
        else if (b.colour != 0)
        {
            g.setColour(fill);
            g.fillRect(box.removeFromBottom(std::max(1.0f, r.getHeight() * 0.08f)));
        }
        const auto ink = b.selected ? (fill.getPerceivedBrightness() > 0.6f ? juce::Colours::black : juce::Colours::white) : c.text;
        label(g, b.label, box.reduced(w * 0.04f, 0.0f), font, ink);
    }
}

void arc(juce::Graphics& g, juce::Point<float> centre, float radius, float width, float position, bool bipolar,
         juce::Colour track, juce::Colour value)
{
    juce::Path back;
    back.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, arcFrom, arcTo, true);
    g.setColour(track);
    g.strokePath(back, juce::PathStrokeType(width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    const float p = juce::jlimit(0.0f, 1.0f, position);
    const float from = bipolar ? (arcFrom + arcTo) * 0.5f : arcFrom;
    const float to = arcFrom + (arcTo - arcFrom) * p;
    g.setColour(value);
    if (std::abs(to - from) > 0.001f)
    {
        juce::Path fill;
        fill.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, std::min(from, to), std::max(from, to), true);
        g.strokePath(fill, juce::PathStrokeType(width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    // The pointer: where the value is, even at the start of its travel.
    const float angle = to - juce::MathConstants<float>::halfPi;
    g.drawLine(centre.x + std::cos(angle) * radius * 0.35f, centre.y + std::sin(angle) * radius * 0.35f,
               centre.x + std::cos(angle) * radius, centre.y + std::sin(angle) * radius, width * 0.7f);
}

void meter(juce::Graphics& g, juce::Rectangle<float> r, float level, const ui::Theme& theme, juce::Colour track)
{
    g.setColour(track);
    g.fillRect(r);
    const float l = juce::jlimit(0.0f, 1.0f, level);
    if (l <= 0.0f)
        return;
    // The mixer's meter colours, where its meters change them.
    using M = ui::Meter;
    g.setColour(l >= M::proportionForDb(0.0f)      ? theme.meterClip
                : l >= M::proportionForDb(M::highDb) ? theme.meterHigh
                : l >= M::proportionForDb(M::midDb)  ? theme.meterMid
                                                     : theme.meterLow);
    g.fillRect(r.withTop(r.getBottom() - r.getHeight() * l));
}

void cell(juce::Graphics& g, const DisplayCell& d, juce::Rectangle<float> r, bool large, const ui::Theme& theme,
          const DisplayRenderer::Colours& c)
{
    const auto accent = rgbColour(d.colour, c.accent);
    const float corner = r.getHeight() * 0.04f;
    if (large)
    {
        g.setColour(c.background.brighter(0.12f));
        g.fillRoundedRectangle(r.reduced(1.0f), corner);
        g.setColour(accent);
        g.drawRoundedRectangle(r.reduced(1.0f), corner, std::max(1.0f, r.getHeight() * 0.012f));
    }
    else if (d.selected)
    {
        g.setColour(c.selected);
        g.fillRoundedRectangle(r.reduced(1.0f), corner);
    }
    auto area = r.reduced(r.getWidth() * 0.05f, r.getHeight() * 0.04f);
    if (d.meter >= 0.0f)
    {
        const float w = std::max(2.0f, area.getWidth() * 0.06f);
        meter(g, area.removeFromRight(w).reduced(0.0f, area.getHeight() * 0.06f), d.meter, theme, c.track);
        area.removeFromRight(w * 0.5f);
    }
    const float line = area.getHeight() * (large ? 0.13f : 0.16f);
    if (d.caption.isNotEmpty())
    {
        auto cap = area.removeFromTop(line);
        if (d.colour != 0)
        {
            g.setColour(accent);
            g.fillRect(cap.removeFromBottom(std::max(1.0f, line * 0.12f)));
        }
        label(g, d.caption, cap, line * 0.78f, c.text);
    }
    label(g, d.name, area.removeFromTop(line), line * 0.78f, d.caption.isNotEmpty() ? c.dim : c.text);
    const auto valueRow = area.removeFromBottom(line * (large ? 1.3f : 1.0f));
    if (large && d.detail.isNotEmpty())
        label(g, d.detail, area.removeFromBottom(line), line * 0.66f, c.dim);
    label(g, d.value, valueRow, line * (large ? 1.05f : 0.82f), c.text);
    if (d.position >= 0.0f && area.getHeight() > 4.0f)
    {
        const float radius = std::min(area.getWidth(), area.getHeight()) * 0.42f;
        const float width = std::max(1.5f, radius * (large ? 0.14f : 0.2f));
        arc(g, area.getCentre(), radius - width * 0.5f, width, d.position, d.bipolar, c.track, accent);
    }
}
void list(juce::Graphics& g, const DisplayList& d, juce::Rectangle<float> r, float line, const DisplayRenderer::Colours& c)
{
    auto area = r.reduced(r.getWidth() * 0.03f, 0.0f);
    label(g, d.title, area.removeFromTop(line), line * 0.62f, c.dim, juce::Justification::centredLeft);
    const int fits = std::max(1, static_cast<int>(area.getHeight() / line));
    const int count = static_cast<int>(d.items.size());
    // The selection kept in the middle, the ends of the list at its edges.
    const int first = juce::jlimit(0, std::max(0, count - fits), d.selected - fits / 2);
    const auto accent = rgbColour(d.colour, c.accent);
    for (int i = first; i < count && i < first + fits; ++i)
    {
        const auto row = area.removeFromTop(line);
        auto ink = c.text;
        if (i == d.selected)
        {
            g.setColour(accent);
            g.fillRoundedRectangle(row.reduced(0.0f, line * 0.06f), line * 0.12f);
            ink = accent.getPerceivedBrightness() > 0.6f ? juce::Colours::black : juce::Colours::white;
        }
        label(g, d.items[static_cast<std::size_t>(i)], row.reduced(line * 0.25f, 0.0f), line * 0.62f, ink, juce::Justification::centredLeft);
    }
}
} // namespace

DisplayRenderer::Colours DisplayRenderer::coloursFor(const ui::Theme& theme)
{
    // A high-contrast theme: brighter text, no grey on grey.
    const bool high = theme.name == ui::Theme::highContrast().name;
    Colours c;
    c.background = juce::Colour(0xff0c0d10);
    c.text = high ? juce::Colours::white : juce::Colour(0xffe6e6e8);
    c.dim = high ? juce::Colour(0xffcfcfcf) : juce::Colour(0xff8c8e94);
    c.track = high ? juce::Colour(0xff5a5a5a) : juce::Colour(0xff2c2e34);
    c.accent = theme.accent;
    c.selected = high ? juce::Colour(0xff303030) : juce::Colour(0xff1f2128);
    return c;
}

float DisplayRenderer::heightFor(float width, int cells, int across)
{
    across = std::max(1, across);
    const int rows = std::max(1, (cells + across - 1) / across);
    const float cellHeight = width / static_cast<float>(across) * cellAspect;
    return cellHeight * (static_cast<float>(rows) + 2.0f * stripShare);
}

DisplayRenderer::Layout DisplayRenderer::layoutFor(juce::Rectangle<float> bounds, int cells, int across)
{
    Layout l;
    l.across = std::max(1, across);
    l.rows = cells > 0 ? (cells + l.across - 1) / l.across : 0;
    const float rows = static_cast<float>(std::max(1, l.rows));
    const float cellHeight = bounds.getHeight() / (rows + 2.0f * stripShare);
    auto area = bounds;
    l.upper = area.removeFromTop(cellHeight * stripShare);
    l.lower = area.removeFromBottom(cellHeight * stripShare);
    const float w = area.getWidth() / static_cast<float>(l.across);
    for (int i = 0; i < cells; ++i)
        l.cells.emplace_back(area.getX() + static_cast<float>(i % l.across) * w,
                             area.getY() + static_cast<float>(i / l.across) * cellHeight, w, cellHeight);
    return l;
}

void DisplayRenderer::draw(juce::Graphics& g, const DisplayFrame& frame, juce::Rectangle<float> bounds, int across,
                           const ui::Theme& theme)
{
    const auto c = coloursFor(theme);
    g.setColour(c.background);
    g.fillRect(bounds);
    const int cells = std::max(static_cast<int>(frame.cells.size()), across);
    const auto l = layoutFor(bounds, cells, across);
    strip(g, frame.upper, frame.title, l.upper, l.across, c);
    strip(g, frame.lower, {}, l.lower, l.across, c);

    if (!frame.lists.empty())
    {
        // The lists side by side across the middle, a column (a cell's width) at a time.
        const auto middle = bounds.withTop(l.upper.getBottom()).withBottom(l.lower.getY());
        const float column = middle.getWidth() / static_cast<float>(l.across);
        const float line = l.upper.getHeight() * 0.75f; // five rows or so on a Push
        float x = middle.getX();
        for (const auto& d : frame.lists)
        {
            const float w = column * static_cast<float>(std::max(1, d.columns));
            list(g, d, juce::Rectangle<float>(x, middle.getY(), std::min(w, middle.getRight() - x), middle.getHeight()), line, c);
            x += w;
            if (x >= middle.getRight())
                break;
        }
        return;
    }
    const bool anything = std::any_of(frame.cells.begin(), frame.cells.end(), [](const DisplayCell& d) { return d.name.isNotEmpty(); });
    if (!anything)
    {
        const auto middle = bounds.withTop(l.upper.getBottom()).withBottom(l.lower.getY());
        label(g, frame.message, middle, std::min(middle.getHeight() * 0.22f, l.upper.getHeight() * 0.9f), c.dim);
        return;
    }
    for (std::size_t i = 0; i < frame.cells.size() && i < l.cells.size(); ++i)
        if (static_cast<int>(i) != frame.detail)
            cell(g, frame.cells[i], l.cells[i], false, theme, c);

    if (frame.detail >= 0 && frame.detail < static_cast<int>(std::min(frame.cells.size(), l.cells.size())))
    {
        // Three cells wide (or as wide as a row is), centred on the touched one, in its row.
        const auto& at = l.cells[static_cast<std::size_t>(frame.detail)];
        const int span = std::min(3, l.across);
        const float w = at.getWidth() * static_cast<float>(span);
        const float x = juce::jlimit(bounds.getX(), bounds.getRight() - w, at.getCentreX() - w * 0.5f);
        const auto big = juce::Rectangle<float>(x, at.getY(), w, at.getHeight());
        g.setColour(c.background);
        g.fillRect(big);
        cell(g, frame.cells[static_cast<std::size_t>(frame.detail)], big, true, theme, c);
    }
}

juce::Image DisplayRenderer::render(const DisplayFrame& frame, int width, int height, int across, const ui::Theme& theme)
{
    juce::Image image(juce::Image::RGB, std::max(1, width), std::max(1, height), true, juce::SoftwareImageType());
    juce::Graphics g(image);
    draw(g, frame, image.getBounds().toFloat(), across, theme);
    return image;
}

} // namespace lattice::control
