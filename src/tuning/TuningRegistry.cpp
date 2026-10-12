#include "tuning/TuningRegistry.h"

#include "core/RealtimeCheck.h"
#include "tuning/Tet12.h"
#include "tuning/parsers/ColundiTextParser.h"
#include "tuning/parsers/ScalaParser.h"

#include <LatticeBinaryData.h>

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
    for (const auto& t : tunings)
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
        return TuningLoadResult::success(existing);

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
    else
    {
        return TuningLoadResult::failure("Unsupported tuning file type '" + ext + "' (use .scl or .txt).");
    }

    if (result.ok())
        add(result.table);
    return result;
}

int TuningRegistry::loadFolder(const juce::File& folder, juce::StringArray* errors)
{
    int added = 0;
    for (const auto& entry : juce::RangedDirectoryIterator(folder, false, "*.scl;*.txt"))
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
