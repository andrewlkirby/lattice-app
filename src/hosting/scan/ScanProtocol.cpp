#include "hosting/scan/ScanProtocol.h"

namespace lattice::hosting::scan
{

namespace
{
constexpr auto midiInputsAttribute = "latticeMidiIn";
constexpr auto midiOutputsAttribute = "latticeMidiOut";
constexpr auto mpeAttribute = "latticeMpe";

juce::String fields(const juce::StringArray& parts)
{
    juce::StringArray escaped;
    for (const auto& p : parts)
        escaped.add(escape(p));
    return escaped.joinIntoString("\t");
}
} // namespace

std::unique_ptr<juce::XmlElement> ScannedPlugin::toXml() const
{
    auto xml = description.createXml();
    xml->setAttribute(midiInputsAttribute, midiInputs);
    xml->setAttribute(midiOutputsAttribute, midiOutputs);
    xml->setAttribute(mpeAttribute, mpe);
    return xml;
}

std::optional<ScannedPlugin> ScannedPlugin::fromXml(const juce::XmlElement& xml)
{
    ScannedPlugin p;
    if (!p.description.loadFromXml(xml))
        return std::nullopt;
    p.midiInputs = xml.getIntAttribute(midiInputsAttribute, -1);
    p.midiOutputs = xml.getIntAttribute(midiOutputsAttribute, -1);
    p.mpe = xml.getIntAttribute(mpeAttribute, -1);
    return p;
}

juce::String escape(const juce::String& text)
{
    if (!text.containsAnyOf("\\\t\n\r"))
        return text;
    juce::String out;
    out.preallocateBytes(text.getNumBytesAsUTF8() + 8);
    for (auto p = text.getCharPointer(); !p.isEmpty(); ++p)
    {
        switch (*p)
        {
        case '\\': out << "\\\\"; break;
        case '\t': out << "\\t"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        default: out += *p; break;
        }
    }
    return out;
}

juce::String unescape(const juce::String& text)
{
    if (!text.containsChar('\\'))
        return text;
    juce::String out;
    for (auto p = text.getCharPointer(); !p.isEmpty(); ++p)
    {
        if (*p != '\\')
        {
            out += *p;
            continue;
        }
        ++p;
        if (p.isEmpty())
            break;
        switch (*p)
        {
        case 't': out << "\t"; break;
        case 'n': out << "\n"; break;
        case 'r': out << "\r"; break;
        default: out += *p; break;
        }
    }
    return out;
}

juce::String writeJobs(const std::vector<Job>& jobs)
{
    juce::String text;
    for (const auto& j : jobs)
        text << fields({j.format, j.identifier}) << "\n";
    return text;
}

std::vector<Job> readJobs(const juce::String& text)
{
    std::vector<Job> jobs;
    for (const auto& line : juce::StringArray::fromLines(text))
    {
        if (line.isEmpty())
            continue;
        const auto tab = line.indexOfChar('\t');
        if (tab <= 0)
            continue;
        jobs.push_back({unescape(line.substring(0, tab)), unescape(line.substring(tab + 1))});
    }
    return jobs;
}

juce::String beginLine(int job) { return "begin\t" + juce::String(job); }
juce::String endLine(int job) { return "end\t" + juce::String(job); }
juce::String doneLine() { return "done"; }
juce::String failLine(int job, const juce::String& why) { return "fail\t" + juce::String(job) + "\t" + escape(why); }
juce::String crashLine(int job, const juce::String& how) { return "crash\t" + juce::String(job) + "\t" + escape(how); }

juce::String pluginLine(int job, const ScannedPlugin& plugin)
{
    const auto xml = plugin.toXml()->toString(juce::XmlElement::TextFormat().singleLine().withoutHeader());
    return "plugin\t" + juce::String(job) + "\t" + escape(xml);
}

std::optional<Line> parseLine(const juce::String& text)
{
    const auto parts = juce::StringArray::fromTokens(text, "\t", {});
    if (parts.isEmpty())
        return std::nullopt;
    Line line;
    const auto& word = parts[0];
    if (word == "done")
    {
        line.type = Line::Type::done;
        return line;
    }
    if (parts.size() < 2 || !parts[1].containsOnly("0123456789"))
        return std::nullopt;
    line.job = parts[1].getIntValue();
    const auto rest = parts.size() > 2 ? unescape(parts[2]) : juce::String();
    if (word == "begin")
        line.type = Line::Type::begin;
    else if (word == "end")
        line.type = Line::Type::end;
    else if (word == "fail")
        line.type = Line::Type::fail;
    else if (word == "crash")
        line.type = Line::Type::crash;
    else if (word == "plugin")
    {
        line.type = Line::Type::plugin;
        const auto xml = juce::parseXML(rest);
        if (xml == nullptr)
            return std::nullopt;
        line.plugin = ScannedPlugin::fromXml(*xml);
        if (!line.plugin)
            return std::nullopt;
    }
    else
        return std::nullopt;
    line.text = rest;
    return line;
}

} // namespace lattice::hosting::scan
