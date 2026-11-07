#include "control/generic/SurfaceDefinition.h"

#include "LatticeBinaryData.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <regex>
#include <tuple>

namespace lattice::control::generic
{

namespace
{
/** A definition's mistake, with where it is; caught by parse(). */
struct Problem
{
    juce::String message;
};

[[noreturn]] void fail(const juce::String& path, const juce::String& message)
{
    throw Problem{path + ": " + message};
}

juce::String join(const juce::String& path, const juce::String& key)
{
    return path.isEmpty() ? key : path + "." + key;
}

juce::String at(const juce::String& path, int index)
{
    return path + "[" + juce::String(index) + "]";
}

const juce::var& field(const juce::var& object, const juce::String& key)
{
    if (const auto* o = object.getDynamicObject())
        return o->getProperty(juce::Identifier(key));
    static const juce::var none;
    return none;
}

bool has(const juce::var& object, const juce::String& key)
{
    const auto* o = object.getDynamicObject();
    return o != nullptr && o->hasProperty(juce::Identifier(key));
}

int integer(const juce::var& v, const juce::String& path, int low, int high)
{
    if (!(v.isInt() || v.isInt64() || (v.isDouble() && std::floor(static_cast<double>(v)) == static_cast<double>(v))))
        fail(path, "a whole number is needed");
    const int i = static_cast<int>(v);
    if (i < low || i > high)
        fail(path, juce::String(i) + " is outside " + juce::String(low) + " to " + juce::String(high));
    return i;
}

juce::String text(const juce::var& v, const juce::String& path)
{
    if (!v.isString())
        fail(path, "text is needed");
    return v.toString();
}

bool boolean(const juce::var& v, const juce::String& path)
{
    if (!v.isBool())
        fail(path, "true or false is needed");
    return static_cast<bool>(v);
}

juce::StringArray texts(const juce::var& v, const juce::String& path)
{
    juce::StringArray out;
    if (v.isString())
        out.add(v.toString());
    else if (const auto* a = v.getArray())
        for (int i = 0; i < a->size(); ++i)
            out.add(text(a->getReference(i), at(path, i)));
    else if (!v.isVoid())
        fail(path, "text or a list of text is needed");
    return out;
}

/** "F0 00 20 29 F7": hex bytes. */
std::vector<std::uint8_t> bytes(const juce::String& s, const juce::String& path)
{
    std::vector<std::uint8_t> out;
    for (const auto& token : juce::StringArray::fromTokens(s, " ", ""))
    {
        if (token.isEmpty())
            continue;
        if (token.length() > 2 || !token.containsOnly("0123456789abcdefABCDEF"))
            fail(path, "'" + token + "' isn't a hex byte");
        out.push_back(static_cast<std::uint8_t>(token.getHexValue32()));
    }
    return out;
}

juce::MidiMessage message(const juce::String& s, const juce::String& path)
{
    const auto b = bytes(s, path);
    if (b.empty())
        fail(path, "no bytes");
    if (b.front() == 0xf0)
    {
        if (b.back() != 0xf7)
            fail(path, "a sysex message must end with F7");
        return juce::MidiMessage::createSysExMessage(b.data() + 1, static_cast<int>(b.size()) - 2);
    }
    if (b.size() > 3)
        fail(path, "a message other than sysex has at most 3 bytes");
    return juce::MidiMessage(b.data(), static_cast<int>(b.size()));
}

ByteTemplate byteTemplate(const juce::String& s, const juce::String& path)
{
    using Kind = ByteTemplate::Token::Kind;
    static const std::vector<std::pair<juce::String, Kind>> placeholders{
        {"id", Kind::id},     {"r7", Kind::r7},     {"g7", Kind::g7},     {"b7", Kind::b7},     {"r8hi", Kind::r8hi},
        {"r8lo", Kind::r8lo}, {"g8hi", Kind::g8hi}, {"g8lo", Kind::g8lo}, {"b8hi", Kind::b8hi}, {"b8lo", Kind::b8lo},
        {"line", Kind::line}, {"text", Kind::text}};
    ByteTemplate t;
    for (const auto& token : juce::StringArray::fromTokens(s, " ", ""))
    {
        if (token.isEmpty())
            continue;
        if (token.startsWithChar('{'))
        {
            if (!token.endsWithChar('}'))
                fail(path, "'" + token + "' isn't closed");
            const auto name = token.substring(1, token.length() - 1);
            const auto it = std::find_if(placeholders.begin(), placeholders.end(), [&](const auto& p) { return p.first == name; });
            if (it == placeholders.end())
                fail(path, "no placeholder {" + name + "}");
            t.tokens.push_back({it->second, 0});
        }
        else
            for (const auto b : bytes(token, path))
                t.tokens.push_back({Kind::byte, b});
    }
    if (t.tokens.empty())
        fail(path, "no bytes");
    return t;
}

MidiAddress address(const juce::var& v, const juce::String& path, int defaultChannel, int number = -1)
{
    MidiAddress a;
    a.channel = has(v, "channel") ? integer(field(v, "channel"), join(path, "channel"), 1, 16) - 1 : defaultChannel;
    if (number >= 0)
    {
        a.type = has(v, "cc") ? MidiAddress::Type::cc : MidiAddress::Type::note;
        a.number = number;
        return a;
    }
    if (has(v, "note") == has(v, "cc"))
        fail(path, "give a note or a cc (one of them)");
    a.type = has(v, "cc") ? MidiAddress::Type::cc : MidiAddress::Type::note;
    a.number = integer(field(v, a.type == MidiAddress::Type::cc ? "cc" : "note"), join(path, a.type == MidiAddress::Type::cc ? "cc" : "note"), 0, 127);
    return a;
}

Aftertouch aftertouch(const juce::var& v, const juce::String& path)
{
    if (v.isVoid())
        return Aftertouch::none;
    const auto s = text(v, path);
    if (s == "none")
        return Aftertouch::none;
    if (s == "channel")
        return Aftertouch::channel;
    if (s == "polyphonic")
        return Aftertouch::polyphonic;
    fail(path, "'" + s + "': none, channel or polyphonic");
}

LedOutput led(const juce::var& v, const juce::String& path)
{
    LedOutput out;
    if (v.isVoid())
        return out;
    const auto kind = text(field(v, "kind"), join(path, "kind"));
    if (kind == "none")
        return out;
    if (has(v, "channel"))
        out.channel = integer(field(v, "channel"), join(path, "channel"), 1, 16) - 1;
    if (kind == "single")
    {
        out.kind = LedOutput::Kind::single;
        if (has(v, "on"))
            out.onValue = integer(field(v, "on"), join(path, "on"), 0, 127);
        if (has(v, "off"))
            out.offValue = integer(field(v, "off"), join(path, "off"), 0, 127);
        if (has(v, "blink"))
            out.blinkValue = integer(field(v, "blink"), join(path, "blink"), 0, 127);
    }
    else if (kind == "palette")
    {
        out.kind = LedOutput::Kind::palette;
        const auto& p = field(v, "palette");
        if (p.isString())
        {
            out.paletteName = p.toString();
            out.palette = namedPalette(out.paletteName);
            if (out.palette.empty())
                fail(join(path, "palette"), "no palette '" + out.paletteName + "' (novation, akai, or a list of colours)");
        }
        else if (const auto* a = p.getArray())
        {
            for (int i = 0; i < a->size(); ++i)
            {
                const auto c = text(a->getReference(i), at(join(path, "palette"), i));
                if (!c.startsWithChar('#') || c.length() != 7)
                    fail(at(join(path, "palette"), i), "'" + c + "' isn't #RRGGBB");
                out.palette.push_back(static_cast<std::uint32_t>(c.substring(1).getHexValue32()));
            }
        }
        else
            fail(join(path, "palette"), "a palette's name or a list of colours is needed");
        if (has(v, "blinkChannel"))
            out.blinkChannel = integer(field(v, "blinkChannel"), join(path, "blinkChannel"), 1, 16) - 1;
        if (has(v, "pulseChannel"))
            out.pulseChannel = integer(field(v, "pulseChannel"), join(path, "pulseChannel"), 1, 16) - 1;
    }
    else if (kind == "sysex")
    {
        out.kind = LedOutput::Kind::sysex;
        out.sysex = byteTemplate(text(field(v, "template"), join(path, "template")), join(path, "template"));
    }
    else
        fail(join(path, "kind"), "'" + kind + "': none, single, palette or sysex");
    return out;
}

Encoding encoding(const juce::var& v, const juce::String& path)
{
    if (v.isVoid())
        return Encoding::absolute;
    const auto s = text(v, path);
    if (const auto e = encodingFromName(s))
        return *e;
    fail(path, "'" + s + "': absolute, twosComplement, binaryOffset, signMagnitude, cc14 or nrpn14");
}

void readGrid(const juce::var& g, SurfaceDefinition& d)
{
    const juce::String path = "grid";
    auto& grid = d.grid;
    grid.rows = integer(field(g, "rows"), join(path, "rows"), 1, 16);
    grid.columns = integer(field(g, "columns"), join(path, "columns"), 1, 16);
    const auto kind = has(g, "message") ? text(field(g, "message"), join(path, "message")) : juce::String("note");
    if (kind != "note" && kind != "cc")
        fail(join(path, "message"), "'" + kind + "': note or cc");
    const int channel = has(g, "channel") ? integer(field(g, "channel"), join(path, "channel"), 1, 16) - 1 : 0;
    const auto type = kind == "cc" ? MidiAddress::Type::cc : MidiAddress::Type::note;
    if (const auto* table = field(g, "table").getArray())
    {
        // Rows from the top, each a list of numbers.
        if (table->size() != grid.rows)
            fail(join(path, "table"), "has " + juce::String(table->size()) + " rows, not " + juce::String(grid.rows));
        for (int r = 0; r < grid.rows; ++r)
        {
            const auto* row = table->getReference(r).getArray();
            const auto rowPath = at(join(path, "table"), r);
            if (row == nullptr || row->size() != grid.columns)
                fail(rowPath, "needs " + juce::String(grid.columns) + " numbers");
            for (int c = 0; c < grid.columns; ++c)
                grid.pads.push_back({type, channel, integer(row->getReference(c), at(rowPath, c), 0, 127)});
        }
    }
    else
    {
        // The top-left pad's number, then the steps to the next row down and the next column.
        const int first = integer(field(g, "first"), join(path, "first"), 0, 127);
        const int rowStep = integer(field(g, "rowStep"), join(path, "rowStep"), -127, 127);
        const int columnStep = has(g, "columnStep") ? integer(field(g, "columnStep"), join(path, "columnStep"), -127, 127) : 1;
        for (int r = 0; r < grid.rows; ++r)
            for (int c = 0; c < grid.columns; ++c)
            {
                const int n = first + r * rowStep + c * columnStep;
                if (n < 0 || n > 127)
                    fail(path, "the pad at row " + juce::String(r) + ", column " + juce::String(c) + " would be " + juce::String(n));
                grid.pads.push_back({type, channel, n});
            }
    }
    if (has(g, "velocity"))
        grid.velocity = boolean(field(g, "velocity"), join(path, "velocity"));
    grid.aftertouch = aftertouch(field(g, "aftertouch"), join(path, "aftertouch"));
    grid.led = led(field(g, "led"), join(path, "led"));
}

void readButtons(const juce::var& list, SurfaceDefinition& d)
{
    const auto* a = list.getArray();
    if (a == nullptr)
    {
        if (!list.isVoid())
            fail("buttons", "a list is needed");
        return;
    }
    for (int i = 0; i < a->size(); ++i)
    {
        const auto& b = a->getReference(i);
        const auto path = at("buttons", i);
        const auto roleText = text(field(b, "role"), join(path, "role"));
        const auto role = roleFromName(roleText);
        if (!role)
            fail(join(path, "role"), "no role '" + roleText + "'");
        LedKind ledKind = LedKind::none;
        LedOutput own;
        if (const auto& l = field(b, "led"); l.isString())
        {
            const auto s = l.toString();
            if (s == "single")
                ledKind = LedKind::single;
            else if (s == "rgb")
                ledKind = LedKind::rgb;
            else if (s != "none")
                fail(join(path, "led"), "'" + s + "': none, single, rgb, or a light of its own");
        }
        else if (!l.isVoid())
        {
            own = led(l, join(path, "led"));
            ledKind = own.kind == LedOutput::Kind::single ? LedKind::single : own.kind == LedOutput::Kind::none ? LedKind::none : LedKind::rgb;
        }
        // A numbered role's buttons can come as a list: "cc": [89, 79, ...], index 0 first.
        const auto& numbers = field(b, has(b, "cc") ? "cc" : "note");
        if (const auto* many = numbers.getArray())
        {
            if (!isNumbered(*role))
                fail(join(path, "role"), "'" + roleText + "' isn't numbered, so it takes one number");
            for (int k = 0; k < many->size(); ++k)
            {
                const int n = integer(many->getReference(k), at(join(path, has(b, "cc") ? "cc" : "note"), k), 0, 127);
                d.buttons.push_back({{*role, k}, address(b, path, 0, n), ledKind, own});
            }
            continue;
        }
        const int index = has(b, "index") ? integer(field(b, "index"), join(path, "index"), 0, 127) : 0;
        if (index > 0 && !isNumbered(*role))
            fail(join(path, "index"), "'" + roleText + "' isn't numbered");
        d.buttons.push_back({{*role, index}, address(b, path, 0), ledKind, own});
    }
}

void readEncoders(const juce::var& list, SurfaceDefinition& d)
{
    const auto* a = list.getArray();
    if (a == nullptr)
    {
        if (!list.isVoid())
            fail("encoders", "a list is needed");
        return;
    }
    for (int i = 0; i < a->size(); ++i)
    {
        const auto& e = a->getReference(i);
        const auto path = at("encoders", i);
        // "count": a row of them on numbers one after another (a touch note each too).
        const int count = has(e, "count") ? integer(field(e, "count"), join(path, "count"), 1, 64) : 1;
        SurfaceDefinition::Encoder first;
        first.encoding = encoding(field(e, "encoding"), join(path, "encoding"));
        if (first.encoding == Encoding::nrpn14)
        {
            first.nrpn = integer(field(e, "nrpn"), join(path, "nrpn"), 0, 16383);
            first.address = {MidiAddress::Type::cc, has(e, "channel") ? integer(field(e, "channel"), join(path, "channel"), 1, 16) - 1 : 0, 99};
        }
        else
        {
            first.address = address(e, path, 0);
            if (first.address.type != MidiAddress::Type::cc)
                fail(path, "an encoder sends a cc");
            if (first.encoding == Encoding::cc14)
                first.lsb = has(e, "lsb") ? integer(field(e, "lsb"), join(path, "lsb"), 0, 127) : first.address.number + 32;
        }
        if (has(e, "touch"))
            first.touch = address(field(e, "touch"), join(path, "touch"), first.address.channel);
        for (int k = 0; k < count; ++k)
        {
            auto enc = first;
            if (enc.encoding == Encoding::nrpn14)
                enc.nrpn += k;
            else
                enc.address.number += k;
            if (enc.lsb >= 0)
                enc.lsb += k;
            if (enc.touch)
                enc.touch->number += k;
            if (enc.address.number > 127 || enc.lsb > 127 || (enc.touch && enc.touch->number > 127) || enc.nrpn > 16383)
                fail(path, "the row of " + juce::String(count) + " runs past 127");
            d.encoders.push_back(enc);
        }
    }
}

/** No two controls may send the same message. */
void checkClashes(const SurfaceDefinition& d)
{
    std::map<std::tuple<int, int, int>, juce::String> taken;
    const auto claim = [&](const MidiAddress& a, const juce::String& what) {
        const auto key = std::tuple{static_cast<int>(a.type), a.channel, a.number};
        if (const auto it = taken.find(key); it != taken.end())
            fail(what, "sends the same message as " + it->second);
        taken[key] = what;
    };
    for (std::size_t i = 0; i < d.grid.pads.size(); ++i)
        claim(d.grid.pads[i], "grid pad " + juce::String(static_cast<int>(i)));
    for (std::size_t i = 0; i < d.buttons.size(); ++i)
        claim(d.buttons[i].address, "buttons (" + buttonName(d.buttons[i].id) + ")");
    for (std::size_t i = 0; i < d.encoders.size(); ++i)
    {
        const auto& e = d.encoders[i];
        if (e.encoding == Encoding::nrpn14)
            continue; // NRPNs share CCs 99, 98, 6 and 38 by design
        claim(e.address, at("encoders", static_cast<int>(i)));
        if (e.lsb >= 0)
            claim({MidiAddress::Type::cc, e.address.channel, e.lsb}, at("encoders", static_cast<int>(i)) + " (lsb)");
        if (e.touch)
            claim(*e.touch, at("encoders", static_cast<int>(i)) + " (touch)");
    }
    // A numbered role's buttons from 0 with no gaps, each once.
    for (int r = 0; r < static_cast<int>(Role::count); ++r)
    {
        std::vector<int> indices;
        for (const auto& b : d.buttons)
            if (static_cast<int>(b.id.role) == r)
                indices.push_back(b.id.index);
        std::sort(indices.begin(), indices.end());
        for (std::size_t k = 0; k < indices.size(); ++k)
            if (indices[k] != static_cast<int>(k))
                fail("buttons", juce::String("'") + roleName(static_cast<Role>(r)) + "' is numbered 0, 1, 2... once each");
    }
}

float distance(std::uint32_t a, std::uint32_t b)
{
    // Weighted RGB: close enough to how far apart two lights look.
    const auto ch = [](std::uint32_t c, int shift) { return static_cast<float>((c >> shift) & 0xffu); };
    const float dr = ch(a, 16) - ch(b, 16), dg = ch(a, 8) - ch(b, 8), db = ch(a, 0) - ch(b, 0);
    return 2.0f * dr * dr + 4.0f * dg * dg + 3.0f * db * db;
}
} // namespace

// --- ByteTemplate -----------------------------------------------------------------------------------

std::vector<std::uint8_t> ByteTemplate::fill(const Values& v) const
{
    using Kind = Token::Kind;
    const auto c8 = [&](int shift) { return static_cast<int>((v.rgb >> shift) & 0xffu); };
    std::vector<std::uint8_t> out;
    out.reserve(tokens.size() + static_cast<std::size_t>(v.width));
    for (const auto& t : tokens)
    {
        int b = 0;
        switch (t.kind)
        {
        case Kind::byte: out.push_back(t.value); continue;
        case Kind::id: b = v.id; break;
        case Kind::r7: b = c8(16) >> 1; break;
        case Kind::g7: b = c8(8) >> 1; break;
        case Kind::b7: b = c8(0) >> 1; break;
        case Kind::r8hi: b = c8(16) >> 7; break;
        case Kind::r8lo: b = c8(16); break;
        case Kind::g8hi: b = c8(8) >> 7; break;
        case Kind::g8lo: b = c8(8); break;
        case Kind::b8hi: b = c8(0) >> 7; break;
        case Kind::b8lo: b = c8(0); break;
        case Kind::line: b = v.line; break;
        case Kind::text:
            for (int i = 0; i < v.width; ++i)
            {
                const auto ch = i < v.text.length() ? v.text[i] : ' ';
                out.push_back(static_cast<std::uint8_t>(ch >= 32 && ch < 127 ? ch : '?'));
            }
            continue;
        }
        out.push_back(static_cast<std::uint8_t>(b & 0x7f));
    }
    return out;
}

// --- LedOutput --------------------------------------------------------------------------------------

int LedOutput::nearest(std::uint32_t rgb) const
{
    rgb &= 0xffffffu;
    if (rgb == 0 || palette.empty())
        return 0;
    int best = 0;
    float bestDistance = std::numeric_limits<float>::max();
    for (std::size_t i = 1; i < palette.size(); ++i)
        if (palette[i] != 0)
            if (const float d = distance(palette[i], rgb); d < bestDistance)
            {
                bestDistance = d;
                best = static_cast<int>(i);
            }
    return best;
}

std::vector<std::uint32_t> namedPalette(juce::StringRef name)
{
    // The first 64 entries of the 128-colour table Novation's Launchpads and Akai's APCs share
    // (from their programmer's references; approximate until checked on a device). The upper
    // half is left out: matching never needs it.
    static const std::vector<std::uint32_t> shared{
        0x000000, 0x1e1e1e, 0x7f7f7f, 0xffffff, 0xff4c4c, 0xff0000, 0x590000, 0x190000, 0xffbd6c, 0xff5400, 0x591d00,
        0x271b00, 0xffff4c, 0xffff00, 0x595900, 0x191900, 0x88ff4c, 0x54ff00, 0x1d5900, 0x142b00, 0x4cff4c, 0x00ff00,
        0x005900, 0x001900, 0x4cff5e, 0x00ff19, 0x00590d, 0x001902, 0x4cff88, 0x00ff55, 0x00591d, 0x001f12, 0x4cffb7,
        0x00ff99, 0x005935, 0x001912, 0x4cc3ff, 0x00a9ff, 0x004152, 0x001019, 0x4c88ff, 0x0055ff, 0x001d59, 0x000819,
        0x4c4cff, 0x0000ff, 0x000059, 0x000019, 0x874cff, 0x5400ff, 0x190064, 0x0f0030, 0xff4cff, 0xff00ff, 0x590059,
        0x190019, 0xff4c87, 0xff0054, 0x59001d, 0x220013, 0xff1500, 0x993500, 0x795100, 0x436400};
    if (name == juce::StringRef("novation") || name == juce::StringRef("akai"))
        return shared;
    return {};
}

// --- SurfaceDefinition ------------------------------------------------------------------------------

SurfaceCapabilities SurfaceDefinition::capabilities() const
{
    SurfaceCapabilities c;
    c.name = name;
    c.rows = grid.rows;
    c.columns = grid.columns;
    c.velocity = grid.velocity;
    c.aftertouch = grid.aftertouch;
    c.padLeds = grid.led.kind == LedOutput::Kind::none     ? LedKind::none
                : grid.led.kind == LedOutput::Kind::single ? LedKind::single
                                                           : LedKind::rgb;
    c.encoders = static_cast<int>(encoders.size());
    c.encodersRelative = !encoders.empty() && std::all_of(encoders.begin(), encoders.end(), [](const Encoder& e) { return isRelative(e.encoding); });
    c.encoderTouch = !encoders.empty() && std::all_of(encoders.begin(), encoders.end(), [](const Encoder& e) { return e.touch.has_value(); });
    if (encoderResolution > 0)
        c.encoderResolution = encoderResolution;
    for (const auto& b : buttons)
        c.buttons.push_back({b.id, b.ledKind});
    c.textLines = display.lines;
    c.textCharacters = display.characters;
    return c;
}

juce::Result SurfaceDefinition::parse(const juce::String& json, SurfaceDefinition& out)
{
    juce::var root;
    if (const auto r = juce::JSON::parse(json, root); r.failed())
        return juce::Result::fail("not JSON: " + r.getErrorMessage());
    try
    {
        if (root.getDynamicObject() == nullptr)
            fail("the file", "an object is needed");
        if (text(field(root, "lattice"), "lattice") != "surface")
            fail("lattice", "must be \"surface\"");
        if (const int version = integer(field(root, "version"), "version", 1, 1000); version != 1)
            fail("version", juce::String(version) + " is newer than this Lattice reads (1)");
        SurfaceDefinition d;
        d.name = text(field(root, "name"), "name");
        if (d.name.trim().isEmpty())
            fail("name", "is empty");
        d.manufacturer = has(root, "manufacturer") ? text(field(root, "manufacturer"), "manufacturer") : juce::String();
        d.verified = has(root, "verified") && boolean(field(root, "verified"), "verified");
        d.isTemplate = has(root, "template") && boolean(field(root, "template"), "template");
        d.source = has(root, "source") ? text(field(root, "source"), "source") : juce::String();
        d.notes = has(root, "notes") ? text(field(root, "notes"), "notes") : juce::String();
        if (has(root, "driver"))
        {
            d.driver = text(field(root, "driver"), "driver");
            if (!nativeDrivers().contains(d.driver))
                fail("driver", "no driver '" + d.driver + "' (there are: " + nativeDrivers().joinIntoString(", ") + ")");
        }

        const auto& ports = field(root, "ports");
        d.inputPorts = texts(field(ports, "input"), "ports.input");
        d.outputPorts = has(ports, "output") ? texts(field(ports, "output"), "ports.output") : d.inputPorts;
        if (d.inputPorts.isEmpty() && !d.isTemplate)
            fail("ports.input", "at least one port name pattern is needed (or \"template\": true)");
        for (const auto* list : {&d.inputPorts, &d.outputPorts})
            for (const auto& pattern : *list)
            {
                // std::regex throws on a bad pattern: say so here, not when a device appears.
                try
                {
                    std::regex test(pattern.toStdString());
                }
                catch (const std::regex_error&)
                {
                    fail("ports", "'" + pattern + "' isn't a regular expression");
                }
            }
        if (has(root, "identity"))
            d.identity = bytes(text(field(root, "identity"), "identity"), "identity");
        for (const auto& [key, list] : {std::pair{"init", &d.init}, std::pair{"exit", &d.exit}})
        {
            const auto messages = texts(field(root, key), key);
            for (int i = 0; i < messages.size(); ++i)
                list->push_back(message(messages[i], at(key, i)));
        }

        if (has(root, "grid"))
            readGrid(field(root, "grid"), d);
        const auto& leds = field(root, "leds");
        d.singleLed = led(field(leds, "single"), "leds.single");
        d.rgbLed = led(field(leds, "rgb"), "leds.rgb");
        readButtons(field(root, "buttons"), d);
        for (std::size_t i = 0; i < d.buttons.size(); ++i)
        {
            const auto& b = d.buttons[i];
            if (b.led.kind == LedOutput::Kind::none && b.ledKind != LedKind::none &&
                (b.ledKind == LedKind::single ? d.singleLed : d.rgbLed).kind == LedOutput::Kind::none)
                fail(at("buttons", static_cast<int>(i)), juce::String("its light needs leds.") + (b.ledKind == LedKind::single ? "single" : "rgb"));
        }
        readEncoders(field(root, "encoders"), d);
        if (has(root, "encoderResolution"))
            d.encoderResolution = integer(field(root, "encoderResolution"), "encoderResolution", 8, 10000);
        if (has(root, "display"))
        {
            const auto& disp = field(root, "display");
            d.display.lines = integer(field(disp, "lines"), "display.lines", 1, 8);
            d.display.characters = integer(field(disp, "characters"), "display.characters", 1, 128);
            d.display.line = byteTemplate(text(field(disp, "line"), "display.line"), "display.line");
        }
        if (d.driver.isEmpty() && d.grid.pads.empty() && d.buttons.empty() && d.encoders.empty())
            fail("the file", "no grid, buttons or encoders");
        checkClashes(d);
        out = std::move(d);
        return juce::Result::ok();
    }
    catch (const Problem& p)
    {
        return juce::Result::fail(p.message);
    }
}

// --- Writing ----------------------------------------------------------------------------------------

namespace
{
juce::String hexOf(const std::uint8_t* data, int size)
{
    juce::StringArray b;
    for (int i = 0; i < size; ++i)
        b.add(juce::String::toHexString(data[i]).toUpperCase().paddedLeft('0', 2));
    return b.joinIntoString(" ");
}

juce::String hexOf(const juce::MidiMessage& m)
{
    return hexOf(m.getRawData(), m.getRawDataSize());
}

juce::String templateText(const ByteTemplate& t)
{
    using Kind = ByteTemplate::Token::Kind;
    static const std::vector<std::pair<Kind, const char*>> names{
        {Kind::id, "{id}"},     {Kind::r7, "{r7}"},     {Kind::g7, "{g7}"},     {Kind::b7, "{b7}"},     {Kind::r8hi, "{r8hi}"}, {Kind::r8lo, "{r8lo}"},
        {Kind::g8hi, "{g8hi}"}, {Kind::g8lo, "{g8lo}"}, {Kind::b8hi, "{b8hi}"}, {Kind::b8lo, "{b8lo}"}, {Kind::line, "{line}"}, {Kind::text, "{text}"}};
    juce::StringArray out;
    for (const auto& token : t.tokens)
    {
        if (token.kind == Kind::byte)
        {
            out.add(hexOf(&token.value, 1));
            continue;
        }
        for (const auto& [k, n] : names)
            if (k == token.kind)
                out.add(n);
    }
    return out.joinIntoString(" ");
}

juce::var ledVar(const LedOutput& l)
{
    auto* o = new juce::DynamicObject();
    switch (l.kind)
    {
    case LedOutput::Kind::none: o->setProperty("kind", "none"); break;
    case LedOutput::Kind::single:
        o->setProperty("kind", "single");
        o->setProperty("on", l.onValue);
        o->setProperty("off", l.offValue);
        if (l.blinkValue >= 0)
            o->setProperty("blink", l.blinkValue);
        break;
    case LedOutput::Kind::palette:
    {
        o->setProperty("kind", "palette");
        if (l.paletteName.isNotEmpty())
            o->setProperty("palette", l.paletteName);
        else
        {
            juce::Array<juce::var> colours;
            for (const auto c : l.palette)
                colours.add("#" + juce::String::toHexString(static_cast<int>(c)).paddedLeft('0', 6));
            o->setProperty("palette", colours);
        }
        if (l.blinkChannel >= 0)
            o->setProperty("blinkChannel", l.blinkChannel + 1);
        if (l.pulseChannel >= 0)
            o->setProperty("pulseChannel", l.pulseChannel + 1);
        break;
    }
    case LedOutput::Kind::sysex:
        o->setProperty("kind", "sysex");
        o->setProperty("template", templateText(l.sysex));
        break;
    }
    if (l.kind != LedOutput::Kind::none && l.channel >= 0)
        o->setProperty("channel", l.channel + 1);
    return o;
}

void putAddress(juce::DynamicObject& o, const MidiAddress& a)
{
    o.setProperty(a.type == MidiAddress::Type::cc ? "cc" : "note", a.number);
    o.setProperty("channel", a.channel + 1);
}

juce::var textList(const juce::StringArray& s)
{
    juce::Array<juce::var> a;
    for (const auto& x : s)
        a.add(x);
    return a;
}
} // namespace

juce::String SurfaceDefinition::toJson() const
{
    auto* root = new juce::DynamicObject();
    const juce::var keep(root);
    root->setProperty("lattice", "surface");
    root->setProperty("version", 1);
    root->setProperty("name", name);
    if (manufacturer.isNotEmpty())
        root->setProperty("manufacturer", manufacturer);
    root->setProperty("verified", verified);
    if (isTemplate)
        root->setProperty("template", true);
    if (source.isNotEmpty())
        root->setProperty("source", source);
    if (notes.isNotEmpty())
        root->setProperty("notes", notes);
    if (driver.isNotEmpty())
        root->setProperty("driver", driver);
    if (!inputPorts.isEmpty() || !outputPorts.isEmpty())
    {
        auto* p = new juce::DynamicObject();
        p->setProperty("input", textList(inputPorts));
        p->setProperty("output", textList(outputPorts));
        root->setProperty("ports", p);
    }
    if (!identity.empty())
        root->setProperty("identity", hexOf(identity.data(), static_cast<int>(identity.size())));
    for (const auto& [key, list] : {std::pair{"init", &init}, std::pair{"exit", &exit}})
        if (!list->empty())
        {
            juce::Array<juce::var> a;
            for (const auto& m : *list)
                a.add(hexOf(m));
            root->setProperty(key, a);
        }
    if (!grid.pads.empty())
    {
        auto* g = new juce::DynamicObject();
        g->setProperty("rows", grid.rows);
        g->setProperty("columns", grid.columns);
        const auto& first = grid.pads.front();
        g->setProperty("message", first.type == MidiAddress::Type::cc ? "cc" : "note");
        g->setProperty("channel", first.channel + 1);
        // A formula when one fits every pad (one type and channel, even steps); else the table.
        const int columnStep = grid.columns > 1 ? grid.pads[1].number - first.number : 1;
        const int rowStep = grid.rows > 1 ? grid.pads[static_cast<std::size_t>(grid.columns)].number - first.number : 0;
        bool formula = true;
        bool oneChannel = true;
        for (int r = 0; r < grid.rows; ++r)
            for (int c = 0; c < grid.columns; ++c)
            {
                const auto& p = grid.pads[static_cast<std::size_t>(r * grid.columns + c)];
                oneChannel = oneChannel && p.type == first.type && p.channel == first.channel;
                formula = formula && p.number == first.number + r * rowStep + c * columnStep;
            }
        jassert(oneChannel); // the file has one message type and channel for the grid
        if (formula)
        {
            g->setProperty("first", first.number);
            g->setProperty("rowStep", rowStep);
            g->setProperty("columnStep", columnStep);
        }
        else
        {
            juce::Array<juce::var> table;
            for (int r = 0; r < grid.rows; ++r)
            {
                juce::Array<juce::var> row;
                for (int c = 0; c < grid.columns; ++c)
                    row.add(grid.pads[static_cast<std::size_t>(r * grid.columns + c)].number);
                table.add(row);
            }
            g->setProperty("table", table);
        }
        g->setProperty("velocity", grid.velocity);
        g->setProperty("aftertouch", grid.aftertouch == Aftertouch::channel      ? "channel"
                                     : grid.aftertouch == Aftertouch::polyphonic ? "polyphonic"
                                                                                 : "none");
        if (grid.led.kind != LedOutput::Kind::none)
            g->setProperty("led", ledVar(grid.led));
        root->setProperty("grid", g);
    }
    if (singleLed.kind != LedOutput::Kind::none || rgbLed.kind != LedOutput::Kind::none)
    {
        auto* l = new juce::DynamicObject();
        if (singleLed.kind != LedOutput::Kind::none)
            l->setProperty("single", ledVar(singleLed));
        if (rgbLed.kind != LedOutput::Kind::none)
            l->setProperty("rgb", ledVar(rgbLed));
        root->setProperty("leds", l);
    }
    if (!buttons.empty())
    {
        juce::Array<juce::var> list;
        for (const auto& b : buttons)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("role", roleName(b.id.role));
            if (isNumbered(b.id.role))
                o->setProperty("index", b.id.index);
            putAddress(*o, b.address);
            if (b.led.kind != LedOutput::Kind::none)
                o->setProperty("led", ledVar(b.led));
            else if (b.ledKind != LedKind::none)
                o->setProperty("led", b.ledKind == LedKind::single ? "single" : "rgb");
            list.add(o);
        }
        root->setProperty("buttons", list);
    }
    if (!encoders.empty())
    {
        juce::Array<juce::var> list;
        for (const auto& e : encoders)
        {
            auto* o = new juce::DynamicObject();
            if (e.encoding == Encoding::nrpn14)
            {
                o->setProperty("nrpn", e.nrpn);
                o->setProperty("channel", e.address.channel + 1);
            }
            else
                putAddress(*o, e.address);
            o->setProperty("encoding", encodingName(e.encoding));
            if (e.encoding == Encoding::cc14)
                o->setProperty("lsb", e.lsb);
            if (e.touch)
            {
                auto* t = new juce::DynamicObject();
                putAddress(*t, *e.touch);
                o->setProperty("touch", t);
            }
            list.add(o);
        }
        root->setProperty("encoders", list);
    }
    if (encoderResolution > 0)
        root->setProperty("encoderResolution", encoderResolution);
    if (display.lines > 0)
    {
        auto* d = new juce::DynamicObject();
        d->setProperty("lines", display.lines);
        d->setProperty("characters", display.characters);
        d->setProperty("line", templateText(display.line));
        root->setProperty("display", d);
    }
    return juce::JSON::toString(keep, false);
}

std::vector<SurfaceDefinition> SurfaceDefinition::included()
{
    std::vector<SurfaceDefinition> out;
    for (int i = 0; i < LatticeResources::namedResourceListSize; ++i)
    {
        if (!juce::String(LatticeResources::originalFilenames[i]).endsWithIgnoreCase(".surface.json"))
            continue;
        int size = 0;
        const auto* data = LatticeResources::getNamedResource(LatticeResources::namedResourceList[i], size);
        SurfaceDefinition d;
        if (data == nullptr || parse(juce::String::fromUTF8(data, size), d).failed())
        {
            jassertfalse; // a broken included definition: the tests catch this
            continue;
        }
        out.push_back(std::move(d));
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return out;
}

} // namespace lattice::control::generic
