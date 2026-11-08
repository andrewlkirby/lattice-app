#include "tuning/external/TuningExport.h"

#include <algorithm>
#include <cmath>

namespace lattice::tuning_export
{

namespace
{
constexpr int numNotes = TuningTable::numNotes;
/** MIDI note 0 in 12-TET at A = 440 Hz: the .tun format's default base. */
constexpr double note0Hz = 8.1757989156437073336;

double centsBetween(double from, double to)
{
    return 1200.0 * std::log2(to / from);
}

/** Every note's frequency, unmapped ones filled in: between mapped neighbours, log-interpolated;
    beyond the first or last mapped note, continuing the nearest step (12-TET's with one mapped
    note, and 12-TET itself with none). */
std::array<double, numNotes> filled(const TuningTable& table)
{
    std::array<double, numNotes> hz{};
    std::vector<int> mapped;
    for (int n = 0; n < numNotes; ++n)
        if (table.hz[static_cast<std::size_t>(n)] > 0.0)
            mapped.push_back(n);
    if (mapped.empty())
    {
        for (int n = 0; n < numNotes; ++n)
            hz[static_cast<std::size_t>(n)] = 440.0 * std::pow(2.0, (n - 69) / 12.0);
        return hz;
    }
    const auto at = [&](int n) { return table.hz[static_cast<std::size_t>(n)]; };
    const auto stepBetween = [&](int a, int b) { return std::pow(at(b) / at(a), 1.0 / (b - a)); };
    const double semitone = std::pow(2.0, 1.0 / 12.0);
    const double lowStep = mapped.size() > 1 ? stepBetween(mapped[0], mapped[1]) : semitone;
    const double highStep = mapped.size() > 1 ? stepBetween(mapped[mapped.size() - 2], mapped.back()) : semitone;
    for (int n = 0; n < numNotes; ++n)
    {
        double f = at(n);
        if (f <= 0.0)
        {
            if (n < mapped.front())
                f = at(mapped.front()) * std::pow(lowStep, n - mapped.front());
            else if (n > mapped.back())
                f = at(mapped.back()) * std::pow(highStep, n - mapped.back());
            else
            {
                const auto next = std::upper_bound(mapped.begin(), mapped.end(), n);
                const int b = *next, a = *(next - 1);
                f = at(a) * std::pow(stepBetween(a, b), n - a);
            }
        }
        hz[static_cast<std::size_t>(n)] = f;
    }
    return hz;
}

/** A name as a quoted .tun value can hold it. */
juce::String quoted(const juce::String& text)
{
    return "\"" + text.removeCharacters("\"\r\n") + "\"";
}

/** A one-line comment's text. */
juce::String oneLine(const juce::String& text)
{
    return text.replaceCharacters("\r\n", "  ").trim();
}
} // namespace

// --- .tun ------------------------------------------------------------------------------------------

juce::String toTun(const TuningTable& table)
{
    const auto hz = filled(table);
    juce::String out;
    out << "; AnaMark tuning file (version 2), written by Lattice\n";
    out << "; " << oneLine(table.name) << "\n";
    out << "[Scale Begin]\n";
    out << "Format= \"AnaMark-TUN\"\n";
    out << "FormatVersion= 200\n";
    out << "FormatSpecs= \"http://www.mark-henning.de/eternity/tuningspecs.html\"\n\n";
    out << "[Info]\n";
    out << "Name= " << quoted(table.name) << "\n";
    out << "Description= " << quoted("128 notes, each at its own frequency") << "\n\n";
    out << "[Tuning]\n";
    for (int n = 0; n < numNotes; ++n)
        out << "note " << n << "= " << juce::roundToInt(centsBetween(note0Hz, hz[static_cast<std::size_t>(n)])) << "\n";
    out << "\n[Exact Tuning]\n";
    out << "BaseFreq= " << juce::String(note0Hz, 16) << "\n";
    for (int n = 0; n < numNotes; ++n)
        out << "note " << n << "= " << juce::String(centsBetween(note0Hz, hz[static_cast<std::size_t>(n)]), 10) << "\n";
    out << "\n[Scale End]\n";
    return out;
}

// --- .scl + .kbm -----------------------------------------------------------------------------------

std::optional<ScalaFiles> toScala(const TuningTable& table)
{
    int first = -1, last = -1;
    for (int n = 0; n < numNotes; ++n)
        if (table.hz[static_cast<std::size_t>(n)] > 0.0)
        {
            if (first < 0)
                first = n;
            last = n;
        }
    if (first < 0 || last <= first || table.hz[static_cast<std::size_t>(last)] <= table.hz[static_cast<std::size_t>(first)])
        return std::nullopt;

    const auto hz = filled(table);
    const int steps = last - first;
    const double base = hz[static_cast<std::size_t>(first)];
    const auto name = oneLine(table.name);

    ScalaFiles files;
    auto& scl = files.scl;
    scl << "! " << name << ".scl\n";
    scl << "! Written by Lattice: one step per key from note " << first << " to note " << last
        << ", the period their ratio. Use it with the .kbm of the same name.\n";
    scl << "!\n";
    scl << name << "\n";
    scl << " " << steps << "\n";
    scl << "!\n";
    for (int k = 1; k <= steps; ++k)
        scl << " " << juce::String(centsBetween(base, hz[static_cast<std::size_t>(first + k)]), 10) << "\n";

    auto& kbm = files.kbm;
    kbm << "! " << name << ".kbm\n";
    kbm << "! Written by Lattice: one key per step of " << name << ".scl, note " << first << " at "
        << juce::String(base, 6) << " Hz.\n";
    kbm << "! Map size\n" << steps << "\n";
    kbm << "! First MIDI note number to retune\n" << first << "\n";
    kbm << "! Last MIDI note number to retune\n" << last << "\n";
    kbm << "! Middle note, where scale degree 0 is mapped\n" << first << "\n";
    kbm << "! Reference note for which the frequency is given\n" << first << "\n";
    kbm << "! Frequency to tune the above note to\n" << juce::String(base, 10) << "\n";
    kbm << "! Scale degree to consider as the formal octave\n" << steps << "\n";
    kbm << "! Mapping\n";
    for (int k = 0; k < steps; ++k)
        kbm << (table.hz[static_cast<std::size_t>(first + k)] > 0.0 ? juce::String(k) : juce::String("x")) << "\n";
    return files;
}

// --- MIDI Tuning Standard ---------------------------------------------------------------------------

std::array<std::uint8_t, 3> mtsBytes(double hz)
{
    constexpr std::array<std::uint8_t, 3> noChange{0x7f, 0x7f, 0x7f};
    if (!(hz > 0.0) || !std::isfinite(hz))
        return noChange;
    const double semitones = 69.0 + 12.0 * std::log2(hz / 440.0);
    if (semitones <= 0.0)
        return {0, 0, 0};
    int note = static_cast<int>(std::floor(semitones));
    int fraction = static_cast<int>(std::lround((semitones - note) * 16384.0));
    if (fraction >= 16384)
    {
        ++note;
        fraction = 0;
    }
    if (note > 127 || (note == 127 && fraction >= 16383))
        return noChange; // above the top, and 7F 7F 7F itself is reserved
    return {static_cast<std::uint8_t>(note), static_cast<std::uint8_t>(fraction >> 7), static_cast<std::uint8_t>(fraction & 0x7f)};
}

double hzFromMtsBytes(const std::array<std::uint8_t, 3>& bytes)
{
    if (bytes[0] == 0x7f && bytes[1] == 0x7f && bytes[2] == 0x7f)
        return 0.0;
    const double semitones = bytes[0] + ((bytes[1] << 7) | bytes[2]) / 16384.0;
    return 440.0 * std::pow(2.0, (semitones - 69.0) / 12.0);
}

std::vector<std::uint8_t> bulkDump(const TuningTable& table, std::uint8_t program, std::uint8_t device)
{
    std::vector<std::uint8_t> m{0xf0, 0x7e, static_cast<std::uint8_t>(device & 0x7f), 0x08, 0x01, static_cast<std::uint8_t>(program & 0x7f)};
    const auto name = table.name.toStdString();
    for (std::size_t i = 0; i < 16; ++i)
    {
        const char c = i < name.size() ? name[i] : ' ';
        m.push_back(c >= 0x20 && c < 0x7f ? static_cast<std::uint8_t>(c) : static_cast<std::uint8_t>('?'));
    }
    for (int n = 0; n < numNotes; ++n)
        for (const auto b : mtsBytes(table.hz[static_cast<std::size_t>(n)]))
            m.push_back(b);
    std::uint8_t sum = 0;
    for (std::size_t i = 1; i < m.size(); ++i)
        sum ^= m[i];
    m.push_back(sum & 0x7f);
    m.push_back(0xf7);
    return m;
}

std::vector<std::vector<std::uint8_t>> noteChanges(const TuningTable& table, const std::vector<int>& notes, std::uint8_t program,
                                                   std::uint8_t device)
{
    std::vector<std::vector<std::uint8_t>> messages;
    for (std::size_t start = 0; start < notes.size(); start += 127)
    {
        const auto count = std::min<std::size_t>(127, notes.size() - start);
        std::vector<std::uint8_t> m{0xf0, 0x7f, static_cast<std::uint8_t>(device & 0x7f), 0x08, 0x02, static_cast<std::uint8_t>(program & 0x7f),
                                    static_cast<std::uint8_t>(count)};
        for (std::size_t i = start; i < start + count; ++i)
        {
            const int note = juce::jlimit(0, numNotes - 1, notes[i]);
            m.push_back(static_cast<std::uint8_t>(note));
            for (const auto b : mtsBytes(table.hz[static_cast<std::size_t>(note)]))
                m.push_back(b);
        }
        m.push_back(0xf7);
        messages.push_back(std::move(m));
    }
    return messages;
}

juce::MemoryBlock toSyx(const std::vector<std::uint8_t>& message)
{
    return {message.data(), message.size()};
}

} // namespace lattice::tuning_export
