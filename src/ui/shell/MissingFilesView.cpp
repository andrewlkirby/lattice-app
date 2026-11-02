#include "ui/shell/MissingFilesView.h"

#include "ui/theme/LatticeLookAndFeel.h"

#include <algorithm>

namespace lattice::ui
{

MissingFilesView::MissingFilesView(SessionController& s, const library::LibraryIndex& i, std::vector<setfiles::Reference> missing)
    : session(s), index(i)
{
    setTitle("Missing files");
    for (const auto& ref : missing)
        if (std::none_of(list.begin(), list.end(), [&](const Entry& e) { return e.file == ref.file; }))
            list.push_back({ref.file, {}});

    searchBtn.setTooltip("Looks for each file by name in the browser's places.");
    searchBtn.onClick = [this] { searchLibrary(); };
    locateBtn.setTooltip("Choose where the selected file is now.");
    locateBtn.onClick = [this] {
        if (locate && selected < static_cast<int>(list.size()))
            locate(selected);
    };
    closeBtn.onClick = [this] {
        if (onClose)
            onClose();
    };
    for (auto* b : {&searchBtn, &locateBtn, &closeBtn})
        addAndMakeVisible(*b);
    setSize(640, 360);
}

std::vector<juce::File> MissingFilesView::candidates(const library::LibraryIndex& index, const juce::File& missing)
{
    library::Query query;
    query.text = missing.getFileNameWithoutExtension();
    query.kind = static_cast<int>(library::Kind::sample);
    std::vector<library::Item> found;
    for (auto& item : index.find(query))
        if (item.file().getFileName().equalsIgnoreCase(missing.getFileName()) && item.file().existsAsFile())
            found.push_back(item);
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.modified > b.modified; });
    std::vector<juce::File> out;
    for (const auto& item : found)
        out.push_back(item.file());
    return out;
}

int MissingFilesView::searchLibrary()
{
    int foundCount = 0;
    session.undoManager().beginNewTransaction("Relink Files");
    for (auto& e : list)
    {
        if (e.foundAt != juce::File())
            continue;
        const auto found = candidates(index, e.file);
        if (found.empty())
            continue;
        setfiles::relink(session.set(), e.file, found.front());
        e.foundAt = found.front();
        ++foundCount;
    }
    repaint();
    return foundCount;
}

void MissingFilesView::relink(int entry, const juce::File& file)
{
    if (entry < 0 || entry >= static_cast<int>(list.size()) || !file.existsAsFile())
        return;
    session.undoManager().beginNewTransaction("Relink Files");
    const auto folder = file.getParentDirectory();
    for (int i = 0; i < static_cast<int>(list.size()); ++i)
    {
        auto& e = list[static_cast<std::size_t>(i)];
        const auto target = i == entry ? file : folder.getChildFile(e.file.getFileName());
        if ((i == entry || e.foundAt == juce::File()) && target.existsAsFile())
        {
            setfiles::relink(session.set(), e.file, target);
            e.foundAt = target;
        }
    }
    repaint();
}

int MissingFilesView::rowHeight() const
{
    return juce::roundToInt(themeOf(*this).fontNormal * 2.0f + themeOf(*this).spacing * 3.0f);
}

void MissingFilesView::resized()
{
    const auto& t = themeOf(*this);
    auto area = getLocalBounds().reduced(juce::roundToInt(t.padding * 2.0f));
    auto buttons = area.removeFromBottom(juce::roundToInt(t.controlHeight));
    area.removeFromBottom(juce::roundToInt(t.spacing * 2.0f));
    const int w = juce::roundToInt(t.controlHeight * 5.0f);
    closeBtn.setBounds(buttons.removeFromRight(w));
    buttons.removeFromRight(juce::roundToInt(t.spacing));
    searchBtn.setBounds(buttons.removeFromLeft(w));
    buttons.removeFromLeft(juce::roundToInt(t.spacing));
    locateBtn.setBounds(buttons.removeFromLeft(w));
    listArea = area.withTrimmedTop(juce::roundToInt(t.fontNormal * 2.0f + t.spacing * 2.0f));
}

void MissingFilesView::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    g.fillAll(t.surface);
    auto top = getLocalBounds().reduced(juce::roundToInt(t.padding * 2.0f)).removeFromTop(listArea.getY());
    g.setColour(t.text);
    g.setFont(t.font(t.fontNormal));
    const auto stillMissing = std::count_if(list.begin(), list.end(), [](const Entry& e) { return e.foundAt == juce::File(); });
    g.drawFittedText(stillMissing == 0 ? juce::String("Every file has been found.")
                                       : juce::String(stillMissing) + (stillMissing == 1 ? " file this set uses isn't" : " files this set uses aren't") +
                                             " where it was saved. Until they're found, those sounds are silent.",
                     top, juce::Justification::topLeft, 2);
    g.saveState();
    g.reduceClipRegion(listArea);
    const int h = rowHeight();
    auto row = listArea.withHeight(h);
    for (int i = 0; i < static_cast<int>(list.size()); ++i, row.translate(0, h))
    {
        const auto& e = list[static_cast<std::size_t>(i)];
        if (i == selected)
        {
            g.setColour(t.selection.withAlpha(0.35f));
            g.fillRect(row);
        }
        auto r = row.reduced(juce::roundToInt(t.padding), juce::roundToInt(t.spacing));
        g.setColour(t.text);
        g.setFont(t.font(t.fontNormal));
        g.drawText(e.file.getFileName(), r.removeFromTop(r.getHeight() / 2), juce::Justification::centredLeft, true);
        g.setFont(t.font(t.fontSmall));
        g.setColour(e.foundAt != juce::File() ? t.play : t.textDim);
        g.drawText(e.foundAt != juce::File() ? "Found: " + e.foundAt.getFullPathName() : "Was: " + e.file.getFullPathName(), r,
                   juce::Justification::centredLeft, true);
    }
    g.restoreState();
}

void MissingFilesView::mouseDown(const juce::MouseEvent& e)
{
    if (!listArea.contains(e.getPosition()))
        return;
    const int i = (e.y - listArea.getY()) / rowHeight();
    if (i < static_cast<int>(list.size()))
    {
        selected = i;
        repaint();
    }
}

} // namespace lattice::ui
