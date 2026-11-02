#include "core/model/SetFiles.h"

#include "core/model/LiveSet.h"

#include <functional>
#include <map>

namespace lattice::setfiles
{
namespace
{
const juce::Identifier fileId{"file"};

void forEachStateXml(juce::XmlElement& xml, const std::function<void(juce::XmlElement&)>& f)
{
    if (xml.hasTagName(ids::deviceState.toString()))
        f(xml);
    for (auto* child : xml.getChildIterator())
        forEachStateXml(*child, f);
}

/** A file name not taken in `folder` by another file: "Kick.wav", "Kick 2.wav"... */
juce::File freeTarget(const juce::File& folder, const juce::File& source)
{
    auto target = folder.getChildFile(source.getFileName());
    for (int n = 2; target.exists() && !target.hasIdenticalContentTo(source); ++n)
        target = folder.getChildFile(source.getFileNameWithoutExtension() + " " + juce::String(n) + source.getFileExtension());
    return target;
}
} // namespace

std::vector<Reference> references(const juce::ValueTree& root)
{
    std::vector<Reference> out;
    std::function<void(const juce::ValueTree&)> walk = [&](const juce::ValueTree& node) {
        if (node.hasType(ids::deviceState))
            if (const auto path = node[fileId].toString(); path.isNotEmpty() && juce::File::isAbsolutePath(path))
                out.push_back({node, juce::File(path)});
        for (const auto& child : node)
            walk(child);
    };
    walk(root);
    return out;
}

std::vector<Reference> missing(const juce::ValueTree& root)
{
    auto all = references(root);
    std::erase_if(all, [](const Reference& r) { return r.file.existsAsFile(); });
    return all;
}

void makeRelative(juce::XmlElement& setXml, const juce::File& folder)
{
    forEachStateXml(setXml, [&](juce::XmlElement& state) {
        const auto path = state.getStringAttribute(fileId.toString());
        if (path.isEmpty() || !juce::File::isAbsolutePath(path))
            return;
        const juce::File file(path);
        if (file.isAChildOf(folder))
            state.setAttribute(fileId.toString(), file.getRelativePathFrom(folder).replaceCharacter('\\', '/'));
    });
}

void makeAbsolute(juce::XmlElement& setXml, const juce::File& folder)
{
    forEachStateXml(setXml, [&](juce::XmlElement& state) {
        const auto path = state.getStringAttribute(fileId.toString());
        if (path.isNotEmpty() && !juce::File::isAbsolutePath(path))
            state.setAttribute(fileId.toString(), folder.getChildFile(path).getFullPathName());
    });
}

CollectResult collect(LiveSet& set, const juce::File& folder)
{
    CollectResult result;
    const auto samples = folder.getChildFile("Samples");
    std::map<juce::String, juce::File> copies; // source path -> its copy
    for (auto& ref : references(set.state()))
    {
        if (ref.file.isAChildOf(folder))
            continue; // with the set already
        if (!ref.file.existsAsFile())
        {
            result.failed.add(ref.file.getFullPathName() + ": missing");
            continue;
        }
        auto it = copies.find(ref.file.getFullPathName());
        if (it == copies.end())
        {
            if (!samples.createDirectory())
            {
                result.failed.add(ref.file.getFullPathName() + ": can't create " + samples.getFullPathName());
                continue;
            }
            const auto target = freeTarget(samples, ref.file);
            if (!target.existsAsFile() && !ref.file.copyFileTo(target))
            {
                result.failed.add(ref.file.getFullPathName() + ": can't copy to " + target.getFullPathName());
                continue;
            }
            it = copies.emplace(ref.file.getFullPathName(), target).first;
            ++result.copied;
        }
        ref.state.setProperty(fileId, it->second.getFullPathName(), set.undoManager());
    }
    return result;
}

int relink(LiveSet& set, const juce::File& from, const juce::File& to)
{
    int changed = 0;
    for (auto& ref : references(set.state()))
        if (ref.file == from)
        {
            ref.state.setProperty(fileId, to.getFullPathName(), set.undoManager());
            ++changed;
        }
    return changed;
}

} // namespace lattice::setfiles
