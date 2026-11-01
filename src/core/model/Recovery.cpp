#include "core/model/Recovery.h"

namespace lattice
{

RecoveryCopy::RecoveryCopy(juce::File folder) : dir(std::move(folder)) {}

bool RecoveryCopy::write(const juce::XmlElement& setXml, const juce::File& original) const
{
    if (!dir.createDirectory())
        return false;
    juce::TemporaryFile temp(setFile());
    if (!setXml.writeTo(temp.getFile()) || !temp.overwriteTargetFileWithTemporary())
        return false;
    juce::String text;
    text << "original=" << original.getFullPathName() << "\n" << "time=" << juce::Time::currentTimeMillis() << "\n";
    return infoFile().replaceWithText(text);
}

bool RecoveryCopy::exists() const
{
    return setFile().existsAsFile();
}

void RecoveryCopy::clear() const
{
    setFile().deleteFile();
    infoFile().deleteFile();
}

namespace
{
juce::String infoValue(const juce::File& info, const juce::String& key)
{
    for (const auto& line : juce::StringArray::fromLines(info.loadFileAsString()))
        if (line.startsWith(key + "="))
            return line.fromFirstOccurrenceOf("=", false, false);
    return {};
}
} // namespace

juce::File RecoveryCopy::original() const
{
    const auto path = infoValue(infoFile(), "original");
    return juce::File::isAbsolutePath(path) ? juce::File(path) : juce::File();
}

juce::Time RecoveryCopy::time() const
{
    const auto ms = infoValue(infoFile(), "time").getLargeIntValue();
    return ms > 0 ? juce::Time(ms) : setFile().getLastModificationTime();
}

} // namespace lattice
