#include "ui/device/DeviceDisplays.h"

#include "core/model/LiveSet.h"
#include "ui/theme/LatticeLookAndFeel.h"

#include <cmath>

namespace lattice::ui
{

// --- DeviceMirror ------------------------------------------------------------------------------------

DeviceMirror::DeviceMirror(const DeviceSpecs& specs, juce::ValueTree device)
    : node(std::move(device)), instance(specs.create(node[ids::deviceType].toString()))
{
    syncAll();
    node.addListener(this);
}

DeviceMirror::~DeviceMirror()
{
    node.removeListener(this);
}

float DeviceMirror::value(const juce::String& parameterId) const
{
    if (const auto* p = instance->findParameter(parameterId))
        return p->base();
    return 0.0f;
}

void DeviceMirror::prepare(double sampleRate)
{
    if (sampleRate > 0.0 && (!instance->isPrepared() || instance->preparedSampleRate() != sampleRate))
        instance->prepareDevice(sampleRate, 64);
}

void DeviceMirror::syncAll()
{
    for (int i = 0; i < instance->numParameters(); ++i)
    {
        auto& p = instance->parameter(i);
        p.setBase(static_cast<float>(LiveSet::deviceParameter(node, p.id(), p.spec().defaultValue)));
    }
}

void DeviceMirror::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (!tree.hasType(ids::params) || tree.getParent() != node)
        return;
    if (auto* p = instance->findParameter(property.toString()))
    {
        p->setBase(static_cast<float>(LiveSet::deviceParameter(node, p->id(), p->spec().defaultValue)));
        if (onChange)
            onChange();
    }
}

void DeviceMirror::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child)
{
    if (parent == node && child.hasType(ids::params))
    {
        syncAll();
        if (onChange)
            onChange();
    }
}

void DeviceMirror::valueTreeRedirected(juce::ValueTree&)
{
    syncAll();
    if (onChange)
        onChange();
}

// --- DeviceDisplay -----------------------------------------------------------------------------------

DeviceDisplay::DeviceDisplay()
{
    setOpaque(false);
}

void DeviceDisplay::invalidate()
{
    dirty = true;
    repaint();
}

juce::Rectangle<float> DeviceDisplay::contentArea() const
{
    const auto& t = themeOf(*this);
    return getLocalBounds().toFloat().reduced(t.stroke + 1.0f);
}

void DeviceDisplay::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(t.surfaceSunken);
    g.fillRoundedRectangle(bounds, t.radius);

    const auto area = contentArea();
    const float scale = juce::jmax(1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    const int w = juce::roundToInt(area.getWidth() * scale), h = juce::roundToInt(area.getHeight() * scale);
    if (w > 0 && h > 0)
    {
        if (cache.isNull() || cache.getWidth() != w || cache.getHeight() != h)
        {
            cache = juce::Image(juce::Image::ARGB, w, h, true);
            dirty = true;
        }
        if (dirty)
        {
            cache.clear(cache.getBounds());
            juce::Graphics cg(cache);
            cg.addTransform(juce::AffineTransform::translation(-area.getX(), -area.getY()).scaled(scale));
            drawContent(cg, area);
            dirty = false;
        }
        g.drawImage(cache, area);
        drawOverlay(g, area);
    }
    g.setColour(t.border);
    g.drawRoundedRectangle(bounds.reduced(0.5f), t.radius, t.stroke);
}

// --- Axes and curves ---------------------------------------------------------------------------------

float FrequencyAxis::xFor(double hz, juce::Rectangle<float> area)
{
    const double p = std::log(juce::jlimit(lowest, highest, hz) / lowest) / std::log(highest / lowest);
    return area.getX() + static_cast<float>(p) * area.getWidth();
}

double FrequencyAxis::hzAt(float x, juce::Rectangle<float> area)
{
    const double p = juce::jlimit(0.0, 1.0, static_cast<double>((x - area.getX()) / juce::jmax(1.0f, area.getWidth())));
    return lowest * std::pow(highest / lowest, p);
}

float yForDb(float db, juce::Rectangle<float> area, float dbLow, float dbHigh)
{
    return area.getBottom() - (db - dbLow) / (dbHigh - dbLow) * area.getHeight();
}

void drawFrequencyGrid(juce::Graphics& g, juce::Rectangle<float> area, const Theme& t, float dbLow, float dbHigh, float dbStep)
{
    const auto line = t.border.withAlpha(0.6f);
    const auto strong = t.border;
    for (double decade = 10.0; decade <= 10000.0; decade *= 10.0)
        for (const double m : {1.0, 2.0, 5.0})
        {
            const double hz = decade * m;
            if (hz < FrequencyAxis::lowest || hz > FrequencyAxis::highest)
                continue;
            const float x = FrequencyAxis::xFor(hz, area);
            g.setColour(m == 1.0 ? strong : line);
            g.drawVerticalLine(juce::roundToInt(x), area.getY(), area.getBottom());
        }
    if (dbStep > 0.0f)
        for (float db = std::ceil(dbLow / dbStep) * dbStep; db <= dbHigh; db += dbStep)
        {
            g.setColour(std::abs(db) < 0.01f ? strong : line);
            g.drawHorizontalLine(juce::roundToInt(yForDb(db, area, dbLow, dbHigh)), area.getX(), area.getRight());
        }
    g.setFont(t.font(t.fontSmall * 0.9f));
    g.setColour(t.textDim);
    for (const auto& [hz, text] : {std::pair{100.0, "100"}, std::pair{1000.0, "1k"}, std::pair{10000.0, "10k"}})
    {
        const float x = FrequencyAxis::xFor(hz, area);
        g.drawText(text, juce::Rectangle<float>(x + 2.0f, area.getBottom() - t.fontSmall - 2.0f, 40.0f, t.fontSmall),
                   juce::Justification::centredLeft, false);
    }
}

juce::Path curvePath(juce::Rectangle<float> area, int points, const std::function<float(float)>& fn)
{
    juce::Path p;
    points = juce::jmax(2, points);
    for (int i = 0; i < points; ++i)
    {
        const float x = static_cast<float>(i) / static_cast<float>(points - 1);
        const float v = juce::jlimit(-0.05f, 1.05f, fn(x));
        const juce::Point<float> pt{area.getX() + x * area.getWidth(), area.getBottom() - v * area.getHeight()};
        if (i == 0)
            p.startNewSubPath(pt);
        else
            p.lineTo(pt);
    }
    return p;
}

} // namespace lattice::ui
