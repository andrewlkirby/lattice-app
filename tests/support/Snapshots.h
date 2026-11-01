#pragma once

// Render snapshots compared with reference images (docs/05-m4-ui.md, Tests 5).
//
// A component (or part of it) is drawn with JUCE's software renderer, which draws the same pixels
// on every machine of a platform (the app's own renderer, Direct2D on Windows, depends on the
// GPU). The references are in tests/data/snapshots/<platform>/, one set per platform since each
// uses its own system font. A snapshot matches when at most 0.1% of its pixels differ by more
// than 32 (of 255) in any channel: room for a font rasteriser's update, not for a moved or
// recoloured element.
//
// LATTICE_UPDATE_SNAPSHOTS=1 writes the references from what's drawn (after a deliberate change:
// look at them before committing). A snapshot that doesn't match is written with a picture of
// its differences to LATTICE_SNAPSHOT_DIR (default: the temp folder's lattice-snapshot-failures).

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>

namespace lattice::test
{

/** Draws area of c (in its own coordinates) at the given zoom, with the software renderer. */
inline juce::Image snapshot(juce::Component& c, juce::Rectangle<int> area, float scale)
{
    juce::Image image(juce::Image::ARGB, juce::roundToInt(static_cast<float>(area.getWidth()) * scale),
                      juce::roundToInt(static_cast<float>(area.getHeight()) * scale), true, juce::SoftwareImageType());
    juce::Graphics g(image);
    g.addTransform(juce::AffineTransform::scale(scale));
    g.setOrigin(-area.getPosition());
    c.paintEntireComponent(g, true);
    return image;
}

inline juce::Image snapshot(juce::Component& c, float scale)
{
    return snapshot(c, c.getLocalBounds(), scale);
}

inline juce::File snapshotReferenceFolder()
{
#if JUCE_WINDOWS
    const char* platform = "windows";
#elif JUCE_MAC
    const char* platform = "macos";
#else
    const char* platform = "linux";
#endif
    return juce::File(LATTICE_SOURCE_DIR).getChildFile("tests/data/snapshots").getChildFile(platform);
}

inline bool writePng(const juce::Image& image, const juce::File& file)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::FileOutputStream out(file);
    juce::PNGImageFormat png;
    return out.openedOk() && png.writeImageToStream(image, out);
}

/** Empty if image matches the reference called name; otherwise what's wrong. */
inline juce::String compareWithReference(const juce::Image& image, const juce::String& name)
{
    const auto reference = snapshotReferenceFolder().getChildFile(name + ".png");
    if (juce::SystemStats::getEnvironmentVariable("LATTICE_UPDATE_SNAPSHOTS", {}).isNotEmpty())
        return writePng(image, reference) ? juce::String() : "could not write " + reference.getFullPathName();
    if (!reference.existsAsFile())
        return "no reference image " + reference.getFullPathName() + " (LATTICE_UPDATE_SNAPSHOTS=1 writes it)";

    const auto expected = juce::ImageFileFormat::loadFrom(reference).convertedToFormat(juce::Image::ARGB);
    juce::String problem;
    juce::Image differences;
    if (expected.getBounds() != image.getBounds())
    {
        problem = "the size is " + juce::String(image.getWidth()) + " x " + juce::String(image.getHeight()) + ", the reference's " +
                  juce::String(expected.getWidth()) + " x " + juce::String(expected.getHeight());
    }
    else
    {
        const juce::Image::BitmapData a(image, juce::Image::BitmapData::readOnly);
        const juce::Image::BitmapData b(expected, juce::Image::BitmapData::readOnly);
        differences = juce::Image(juce::Image::ARGB, image.getWidth(), image.getHeight(), true, juce::SoftwareImageType());
        juce::Image::BitmapData d(differences, juce::Image::BitmapData::writeOnly);
        int differing = 0, worst = 0;
        for (int y = 0; y < image.getHeight(); ++y)
            for (int x = 0; x < image.getWidth(); ++x)
            {
                const auto p = a.getPixelColour(x, y), q = b.getPixelColour(x, y);
                const int diff = std::max({std::abs(p.getRed() - q.getRed()), std::abs(p.getGreen() - q.getGreen()),
                                           std::abs(p.getBlue() - q.getBlue()), std::abs(p.getAlpha() - q.getAlpha())});
                worst = std::max(worst, diff);
                if (diff > 32)
                    ++differing;
                d.setPixelColour(x, y, diff > 32 ? juce::Colours::red : q.withMultipliedAlpha(0.25f));
            }
        const int allowed = image.getWidth() * image.getHeight() / 1000;
        if (differing > allowed)
            problem = juce::String(differing) + " pixels differ (at most " + juce::String(allowed) + " may), by up to " + juce::String(worst);
    }
    if (problem.isEmpty())
        return {};

    const auto dir = juce::File(juce::SystemStats::getEnvironmentVariable(
        "LATTICE_SNAPSHOT_DIR",
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lattice-snapshot-failures").getFullPathName()));
    writePng(image, dir.getChildFile(name + ".png"));
    if (differences.isValid())
        writePng(differences, dir.getChildFile(name + "-differences.png"));
    return name + ": " + problem + "; drawn: " + dir.getChildFile(name + ".png").getFullPathName();
}

} // namespace lattice::test
