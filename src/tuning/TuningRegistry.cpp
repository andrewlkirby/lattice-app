#include "tuning/TuningRegistry.h"

#include "core/RealtimeCheck.h"
#include "tuning/Tet12.h"
#include "tuning/parsers/ColundiTextParser.h"
#include "tuning/parsers/ScalaParser.h"
#include "tuning/parsers/TunParser.h"

#include <LatticeBinaryData.h>

#include <algorithm>

namespace lattice
{

TuningRegistry::TuningRegistry()
{
    add(Tet12::make(440.0));
    if (auto colundi = makeBuiltinColundi())
        add(std::move(colundi));
}

TuningPtr TuningRegistry::makeBuiltinColundi()
{
    const auto text = juce::String::fromUTF8(LatticeResources::colundi_scale_txt,
                                             LatticeResources::colundi_scale_txtSize);
    ColundiTextParser::Options options;
    options.id = colundiId;
    options.name = "Colundi";
    options.sourceDescription = "Built in (colundi_scale.txt)";
    options.periodRatio = colundiPeriodRatio;
    options.keyboardPageSize = 17;

    auto result = ColundiTextParser::parse(text, options);
    jassert(result.ok()); // the embedded file is covered by tests
    return result.table;
}

TuningPtr TuningRegistry::findById(const juce::String& id) const
{
    for (const auto* list : {&tunings, &replaced})
        for (const auto& t : *list)
            if (t->id == id)
                return t;
    return nullptr;
}

TuningPtr TuningRegistry::defaultTuning() const
{
    return findById(Tet12::builtinId);
}

void TuningRegistry::add(TuningPtr tuning)
{
    std::erase_if(replaced, [&](const TuningPtr& t) { return t->id == tuning->id; }); // coming back
    // A newer version of a file in the list takes its place.
    const auto sameFile = std::find_if(tunings.begin(), tunings.end(), [&](const TuningPtr& t) {
        return juce::File::isAbsolutePath(tuning->sourceDescription) && t->sourceDescription == tuning->sourceDescription;
    });
    if (sameFile != tunings.end())
    {
        replaced.push_back(*sameFile);
        *sameFile = std::move(tuning);
    }
    else
        tunings.push_back(std::move(tuning));
    listeners.call([](Listener& l) { l.tuningsChanged(); });
}

TuningLoadResult TuningRegistry::loadFile(const juce::File& file)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    if (!file.existsAsFile())
        return TuningLoadResult::failure("File not found: " + file.getFullPathName());

    const auto text = file.loadFileAsString();
    const auto ext = file.getFileExtension().toLowerCase();
    juce::String kbmText;
    if (ext == ".scl")
    {
        const auto kbm = file.withFileExtension(".kbm");
        if (kbm.existsAsFile())
            kbmText = kbm.loadFileAsString();
    }

    const auto id = "user." + juce::String::toHexString(static_cast<juce::int64>((text + "\n--kbm--\n" + kbmText).hashCode64()));
    if (auto existing = findById(id))
    {
        if (std::find(tunings.begin(), tunings.end(), existing) == tunings.end())
            add(existing); // the file went back to an older version
        return TuningLoadResult::success(existing);
    }

    TuningLoadResult result;
    if (ext == ".scl")
    {
        result = ScalaParser::parse(text, kbmText, id, {}, file.getFullPathName());
    }
    else if (ext == ".txt")
    {
        ColundiTextParser::Options options;
        options.id = id;
        options.name = file.getFileNameWithoutExtension();
        options.sourceDescription = file.getFullPathName();
        result = ColundiTextParser::parse(text, options);
    }
    else if (ext == ".tun")
    {
        result = TunParser::parse(text, id, file.getFileNameWithoutExtension(), file.getFullPathName());
    }
    else
    {
        return TuningLoadResult::failure("Unsupported tuning file type '" + ext + "' (use .scl, .tun or .txt).");
    }

    if (result.ok())
        add(result.table);
    return result;
}

int TuningRegistry::loadFolder(const juce::File& folder, juce::StringArray* errors)
{
    int added = 0;
    for (const auto& entry : juce::RangedDirectoryIterator(folder, false, "*.scl;*.tun;*.txt"))
    {
        const auto before = tunings.size();
        const auto result = loadFile(entry.getFile());
        if (!result.ok() && errors != nullptr)
            errors->add(entry.getFile().getFileName() + ": " + result.error);
        if (tunings.size() > before)
            ++added;
    }
    return added;
}

} // namespace lattice
