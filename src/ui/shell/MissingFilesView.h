#pragma once

#include "core/model/SetFiles.h"
#include "engine/SessionController.h"
#include "library/LibraryIndex.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace lattice::ui
{

/** The files a set names that aren't there (docs/05-m4-ui.md, 4g), shown after it opens.

    - Search Library looks each one up in the library index by its file name (the browser's places
      have been scanned, so this doesn't touch the disk) and relinks it to the one found (the
      newest, if there are several).
    - Locate... asks for the file by hand; other missing files in the same folder are looked for
      next to it.
    Relinking is one undo step per action. */
class MissingFilesView final : public juce::Component
{
public:
    MissingFilesView(SessionController& session, const library::LibraryIndex& index, std::vector<setfiles::Reference> missing);

    /** Files in the index with the missing file's name, the newest first. */
    static std::vector<juce::File> candidates(const library::LibraryIndex& index, const juce::File& missing);

    /** The missing files (each once), and what they've been relinked to (empty: still missing). */
    struct Entry
    {
        juce::File file;
        juce::File foundAt;
    };
    const std::vector<Entry>& entries() const noexcept { return list; }

    /** Relinks every file still missing that the index has. Returns how many it found. */
    int searchLibrary();
    /** Relinks one entry to `file`, and any other still missing whose name is in its folder. */
    void relink(int entry, const juce::File& file);

    /** "Locate..." for an entry (the shell provides the file dialog). */
    std::function<void(int entry)> locate;
    std::function<void()> onClose;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    int rowHeight() const;

    SessionController& session;
    const library::LibraryIndex& index;
    std::vector<Entry> list;
    int selected = 0;
    juce::TextButton searchBtn{"Search Library"}, locateBtn{"Locate..."}, closeBtn{"Done"};
    juce::Rectangle<int> listArea;
};

} // namespace lattice::ui
