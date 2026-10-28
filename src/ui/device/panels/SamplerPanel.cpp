// The Sampler's panel (docs/05-m4-ui.md, 4d): the waveform with its start, end, loop and slice
// markers to drag, zoom and scroll; the root frequency next to it as Hz and the matching 12-TET
// note (M3 hands-on finding 6), read from the file when it says; the interpolation quality; and the
// controls for the mode it's in.

#include "devices/instruments/Sampler.h"
#include "devices/samples/SamplePool.h"
#include "ui/device/DevicePanels.h"
#include "ui/device/panels/PanelKit.h"
#include "ui/theme/LatticeLookAndFeel.h"

#include <cmath>

namespace lattice::ui::panels
{

// --- Peaks -----------------------------------------------------------------------------------------------

/** A sample's peaks at several zoom levels (docs/05-m4-ui.md, "Rendering and performance"): the
    lowest and highest value of each run of frames, 64 frames a run, then 4 times longer at each level
    up, so a view of any zoom reads a few thousand values at most. Built in the background. */
struct WaveformPeaks
{
    static constexpr int baseFrames = 64;
    struct Level
    {
        int framesPerBin = baseFrames;
        std::vector<float> low, high;
    };
    std::vector<Level> levels;
    std::int64_t length = 0;

    static std::shared_ptr<const WaveformPeaks> build(const SampleData& sample, const std::function<bool()>& cancelled)
    {
        auto p = std::make_shared<WaveformPeaks>();
        p->length = sample.length;
        Level base;
        const auto bins = static_cast<std::size_t>((sample.length + baseFrames - 1) / baseFrames);
        base.low.assign(bins, 0.0f);
        base.high.assign(bins, 0.0f);
        for (std::size_t b = 0; b < bins; ++b)
        {
            if ((b & 4095) == 0 && cancelled())
                return nullptr;
            float lo = 0.0f, hi = 0.0f;
            const auto first = static_cast<std::int64_t>(b) * baseFrames;
            const auto last = std::min<std::int64_t>(sample.length, first + baseFrames);
            for (int c = 0; c < sample.numChannels; ++c)
            {
                const float* data = sample.channel(c);
                for (auto f = first; f < last; ++f)
                    lo = std::min(lo, data[f]), hi = std::max(hi, data[f]);
            }
            base.low[b] = lo;
            base.high[b] = hi;
        }
        p->levels.push_back(std::move(base));
        while (p->levels.back().low.size() > 512)
        {
            const auto& below = p->levels.back();
            Level up;
            up.framesPerBin = below.framesPerBin * 4;
            const std::size_t n = (below.low.size() + 3) / 4;
            up.low.assign(n, 0.0f);
            up.high.assign(n, 0.0f);
            for (std::size_t i = 0; i < below.low.size(); ++i)
            {
                up.low[i / 4] = std::min(up.low[i / 4], below.low[i]);
                up.high[i / 4] = std::max(up.high[i / 4], below.high[i]);
            }
            p->levels.push_back(std::move(up));
        }
        return p;
    }

    /** The lowest and highest value over frames [from, to). */
    std::pair<float, float> range(double from, double to) const
    {
        // The coarsest level that still gives at least one bin over the range.
        const Level* level = &levels.front();
        for (const auto& l : levels)
            if (l.framesPerBin <= (to - from))
                level = &l;
        const auto n = static_cast<std::int64_t>(level->low.size());
        auto b0 = juce::jlimit<std::int64_t>(0, n - 1, static_cast<std::int64_t>(from / level->framesPerBin));
        const auto b1 = juce::jlimit<std::int64_t>(b0, n - 1, static_cast<std::int64_t>(std::ceil(to / level->framesPerBin)) - 1);
        float lo = 0.0f, hi = 0.0f;
        for (; b0 <= b1; ++b0)
            lo = std::min(lo, level->low[static_cast<std::size_t>(b0)]), hi = std::max(hi, level->high[static_cast<std::size_t>(b0)]);
        return {lo, hi};
    }
};

namespace
{
juce::String noteNameFor(double hz)
{
    if (hz <= 0.0)
        return {};
    const double note = 69.0 + 12.0 * std::log2(hz / 440.0);
    const int nearest = juce::roundToInt(note);
    const int cents = juce::roundToInt((note - nearest) * 100.0);
    return juce::MidiMessage::getMidiNoteName(nearest, true, true, 4) +
           (cents == 0 ? juce::String() : (cents > 0 ? " +" : " ") + juce::String(cents) + " ct");
}

/** The waveform, with markers you drag: start and end, the loop (Classic mode), and the slices
    (Slice mode; in Manual slicing, double-click adds or removes one). The wheel zooms around the
    pointer, Shift+wheel scrolls, and dragging the empty part scrolls. */
class WaveformView final : public juce::Component, private juce::ValueTree::Listener
{
public:
    enum class Marker
    {
        none,
        start,
        end,
        loopStart,
        loopEnd,
        slice
    };

    WaveformView(DeviceContext& c, const juce::ValueTree& d) : context(c), device(d), mirror(c.specs, d)
    {
        setTitle("Waveform");
        setDescription("Drag the markers to set the start, end, loop and slices. The wheel zooms; Shift+wheel or a drag scrolls.");
        for (const auto* id : {"sample.start", "sample.end", "loop.on", "loop.start", "loop.length", "sample.mode", "slice.by"})
            values[id] = bind(context, device, id);
        for (auto& [id, b] : values)
            b->onChange = [this] { repaint(); };
        mirror.onChange = [this] { repaint(); };
        device.addListener(this);
        stateChanged();
    }
    ~WaveformView() override
    {
        device.removeListener(this);
        if (job != 0)
            context.jobs.cancel(job);
    }

    SamplePtr sample() const { return current; }
    bool hasPeaks() const { return peaks != nullptr; }
    juce::String fileName() const { return juce::File(path).getFileName(); }

    // --- View ---------------------------------------------------------------------------------------
    double viewStart() const { return start; }
    double viewEnd() const { return end > start ? end : static_cast<double>(length()); }
    std::int64_t length() const { return current != nullptr ? current->length : 0; }
    void zoom(double factor, double aroundFrame)
    {
        const double len = static_cast<double>(length());
        if (len <= 0.0)
            return;
        const double span = juce::jlimit(64.0, len, (viewEnd() - viewStart()) * factor);
        const double share = (aroundFrame - viewStart()) / juce::jmax(1.0, viewEnd() - viewStart());
        start = juce::jlimit(0.0, len - span, aroundFrame - share * span);
        end = start + span;
        repaint();
    }
    void scrollBy(double frames)
    {
        const double span = viewEnd() - viewStart(), len = static_cast<double>(length());
        start = juce::jlimit(0.0, juce::jmax(0.0, len - span), start + frames);
        end = start + span;
        repaint();
    }

    float xFor(double frame) const
    {
        const auto r = waveArea();
        return r.getX() + static_cast<float>((frame - viewStart()) / juce::jmax(1.0, viewEnd() - viewStart())) * r.getWidth();
    }
    double frameAt(float x) const
    {
        const auto r = waveArea();
        return viewStart() + (x - r.getX()) / juce::jmax(1.0f, r.getWidth()) * (viewEnd() - viewStart());
    }

    // --- Markers ------------------------------------------------------------------------------------
    double markerFrame(Marker m) const
    {
        const double len = static_cast<double>(length());
        const double s = value("sample.start") * len, e = value("sample.end") * len;
        const double loopStart = s + value("loop.start") * (e - s);
        switch (m)
        {
        case Marker::start: return s;
        case Marker::end: return e;
        case Marker::loopStart: return loopStart;
        case Marker::loopEnd: return std::min(e, loopStart + std::max(0.001f, value("loop.length")) * (e - s));
        default: return 0.0;
        }
    }
    bool showsLoop() const { return mode() == SamplerDevice::Mode::Classic && value("loop.on") > 0.5f; }
    SamplerDevice::Mode mode() const { return static_cast<SamplerDevice::Mode>(juce::roundToInt(value("sample.mode"))); }
    std::vector<std::int64_t> slices() const
    {
        const auto* s = const_cast<DeviceMirror&>(mirror).as<SamplerDevice>();
        return s != nullptr && mode() == SamplerDevice::Mode::Slice ? s->slicePoints() : std::vector<std::int64_t>{};
    }
    bool manualSlicing() const { return mode() == SamplerDevice::Mode::Slice && juce::roundToInt(value("slice.by")) == 2; }

    Marker markerAt(float x, int* sliceIndex = nullptr) const
    {
        const float reach = themeOf(*this).controlHeight * 0.35f;
        Marker best = Marker::none;
        float bestDistance = reach;
        const auto consider = [&](Marker m, double frame, int index) {
            if (const float d = std::abs(xFor(frame) - x); d < bestDistance)
            {
                best = m, bestDistance = d;
                if (sliceIndex != nullptr)
                    *sliceIndex = index;
            }
        };
        if (manualSlicing())
        {
            const auto points = manualPoints();
            for (int i = 0; i < static_cast<int>(points.size()); ++i)
                consider(Marker::slice, static_cast<double>(points[static_cast<std::size_t>(i)]), i);
        }
        if (showsLoop())
        {
            consider(Marker::loopStart, markerFrame(Marker::loopStart), -1);
            consider(Marker::loopEnd, markerFrame(Marker::loopEnd), -1);
        }
        consider(Marker::start, markerFrame(Marker::start), -1);
        consider(Marker::end, markerFrame(Marker::end), -1);
        return best;
    }

    /** Moves a marker to a frame, as one step of the current gesture. */
    void moveMarker(Marker m, double frame, int sliceIndex = -1)
    {
        const double len = static_cast<double>(juce::jmax<std::int64_t>(1, length()));
        const double s = markerFrame(Marker::start), e = markerFrame(Marker::end);
        switch (m)
        {
        case Marker::start:
            values["sample.start"]->setValue(static_cast<float>(juce::jlimit(0.0, e / len - 0.001, frame / len)));
            break;
        case Marker::end:
            values["sample.end"]->setValue(static_cast<float>(juce::jlimit(s / len + 0.001, 1.0, frame / len)));
            break;
        case Marker::loopStart:
        {
            // The loop's end stays where it is.
            const double loopEnd = markerFrame(Marker::loopEnd);
            const double span = juce::jmax(1.0, e - s);
            const double ls = juce::jlimit(s, loopEnd - 1.0, frame);
            values["loop.start"]->setValue(static_cast<float>((ls - s) / span));
            values["loop.length"]->setValue(static_cast<float>((loopEnd - ls) / span));
            break;
        }
        case Marker::loopEnd:
        {
            const double span = juce::jmax(1.0, e - s);
            values["loop.length"]->setValue(static_cast<float>((juce::jlimit(markerFrame(Marker::loopStart) + 1.0, e, frame) -
                                                                 markerFrame(Marker::loopStart)) / span));
            break;
        }
        case Marker::slice:
        {
            auto points = manualPoints();
            if (sliceIndex >= 0 && sliceIndex < static_cast<int>(points.size()))
            {
                points[static_cast<std::size_t>(sliceIndex)] = static_cast<std::int64_t>(juce::jlimit(0.0, len - 1.0, frame));
                setManualPoints(points, false);
            }
            break;
        }
        case Marker::none:
            break;
        }
    }

    std::vector<std::int64_t> manualPoints() const
    {
        std::vector<std::int64_t> out;
        for (const auto& token : juce::StringArray::fromTokens(device.getChildWithName(ids::deviceState)["slices"].toString(), ",", {}))
            if (token.trim().isNotEmpty())
                out.push_back(token.trim().getLargeIntValue());
        return out;
    }
    void setManualPoints(std::vector<std::int64_t> points, bool newStep)
    {
        std::sort(points.begin(), points.end());
        juce::StringArray text;
        for (const auto p : points)
            text.add(juce::String(p));
        if (newStep)
            context.undo().beginNewTransaction("Slices");
        device.getChildWithName(ids::deviceState).setProperty("slices", text.joinIntoString(","), context.set().undoManager());
    }

    // --- Mouse ------------------------------------------------------------------------------------------
    void mouseDown(const juce::MouseEvent& e) override
    {
        dragged = markerAt(e.position.x, &draggedSlice);
        lastX = e.position.x;
        if (dragged == Marker::none)
            return;
        context.undo().beginNewTransaction("Move marker");
        for (auto& [id, b] : values)
            b->beginGesture();
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragged != Marker::none)
            moveMarker(dragged, frameAt(e.position.x), draggedSlice);
        else
            scrollBy((lastX - e.position.x) / juce::jmax(1.0f, waveArea().getWidth()) * (viewEnd() - viewStart()));
        lastX = e.position.x;
    }
    void mouseUp(const juce::MouseEvent&) override
    {
        if (dragged != Marker::none)
            for (auto& [id, b] : values)
                b->endGesture();
        dragged = Marker::none;
    }
    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (!manualSlicing() || length() <= 0)
            return;
        int index = -1;
        auto points = manualPoints();
        if (markerAt(e.position.x, &index) == Marker::slice && index >= 0)
            points.erase(points.begin() + index);
        else
            points.push_back(static_cast<std::int64_t>(juce::jlimit(0.0, static_cast<double>(length() - 1), frameAt(e.position.x))));
        setManualPoints(points, true);
    }
    void mouseMove(const juce::MouseEvent& e) override
    {
        setMouseCursor(markerAt(e.position.x) != Marker::none ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const float delta = wheel.deltaY != 0.0f ? wheel.deltaY : wheel.deltaX;
        if (e.mods.isShiftDown() || wheel.deltaX != 0.0f)
            scrollBy(-delta * 0.25 * (viewEnd() - viewStart()));
        else
            zoom(delta > 0.0f ? 0.8 : 1.25, frameAt(e.position.x));
    }

    void paint(juce::Graphics& g) override
    {
        const auto& t = themeOf(*this);
        const auto bounds = getLocalBounds().toFloat();
        g.setColour(t.surfaceSunken);
        g.fillRoundedRectangle(bounds, t.radius);
        const auto area = waveArea();
        // The title line: the file, or what to do.
        g.setFont(t.font(t.fontSmall));
        g.setColour(path.isEmpty() ? t.textDim : t.text);
        const auto titleLine = bounds.reduced(t.padding * 0.5f, 0.0f).removeFromTop(t.fontSmall + t.spacing);
        g.drawText(path.isEmpty() ? juce::String("Drop a sample here") : fileName() + (current == nullptr ? (failed.isNotEmpty() ? "  (" + failed + ")" : "  (loading...)") : juce::String()),
                   titleLine, juce::Justification::centredLeft, true);
        if (current != nullptr && peaks != nullptr && length() > 0)
        {
            // Outside the region: dimmer.
            const float sx = xFor(markerFrame(Marker::start)), ex = xFor(markerFrame(Marker::end));
            const int columns = juce::roundToInt(area.getWidth());
            const double perColumn = (viewEnd() - viewStart()) / juce::jmax(1, columns);
            for (int c = 0; c < columns; ++c)
            {
                const double from = viewStart() + c * perColumn;
                if (from >= static_cast<double>(length()))
                    break;
                const auto [lo, hi] = peaks->range(from, from + juce::jmax(1.0, perColumn));
                const float x = area.getX() + static_cast<float>(c);
                g.setColour(x >= sx && x <= ex ? t.accent : t.textDisabled);
                g.drawVerticalLine(static_cast<int>(x), area.getCentreY() - juce::jmax(0.5f, hi * area.getHeight() * 0.5f),
                                   area.getCentreY() - lo * area.getHeight() * 0.5f + 0.5f);
            }
            // The loop: a band, its ends marked.
            if (showsLoop())
            {
                const float lx = xFor(markerFrame(Marker::loopStart)), rx = xFor(markerFrame(Marker::loopEnd));
                g.setColour(t.selection.withAlpha(0.18f));
                g.fillRect(juce::Rectangle<float>(lx, area.getY(), rx - lx, area.getHeight()));
                g.setColour(t.selection);
                g.drawVerticalLine(juce::roundToInt(lx), area.getY(), area.getBottom());
                g.drawVerticalLine(juce::roundToInt(rx), area.getY(), area.getBottom());
            }
            // Slices.
            g.setColour(t.text.withAlpha(0.7f));
            for (const auto frame : slices())
                g.drawVerticalLine(juce::roundToInt(xFor(static_cast<double>(frame))), area.getY(), area.getBottom());
            // Start and end: flags at the top.
            for (const auto m : {Marker::start, Marker::end})
            {
                const float x = xFor(markerFrame(m));
                g.setColour(t.text);
                g.drawVerticalLine(juce::roundToInt(x), area.getY(), area.getBottom());
                juce::Path flag;
                const float s = t.controlHeight * 0.35f;
                flag.addTriangle(x, area.getY(), m == Marker::start ? x + s : x - s, area.getY(), x, area.getY() + s);
                g.fillPath(flag);
            }
        }
        g.setColour(t.border);
        g.drawRoundedRectangle(bounds.reduced(0.5f), t.radius, t.stroke);
    }

private:
    juce::Rectangle<float> waveArea() const
    {
        const auto& t = themeOf(*this);
        return getLocalBounds().toFloat().reduced(t.padding * 0.5f).withTrimmedTop(t.fontSmall + t.spacing);
    }
    float value(const juce::String& id) const
    {
        const auto it = values.find(id);
        return it != values.end() ? it->second->value() : 0.0f;
    }

    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override
    {
        if (tree.hasType(ids::deviceState) && tree.getParent() == device && (property.toString() == "file" || property.toString() == "slices"))
            stateChanged();
    }
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child) override
    {
        if (parent == device && child.hasType(ids::deviceState))
            stateChanged();
    }

    /** The file or the slices changed: the copy follows (it computes the slices), and a new file is
        read (through the shared pool, so it's decoded once for the engine and here) and its peaks
        built in the background. */
    void stateChanged()
    {
        const auto state = device.getChildWithName(ids::deviceState);
        if (auto* s = mirror.as<SamplerDevice>())
        {
            auto copy = state.createCopy();
            s->setState(copy.isValid() ? copy : juce::ValueTree(ids::deviceState));
        }
        const auto file = state["file"].toString();
        if (file == path)
        {
            repaint();
            return;
        }
        path = file;
        current = nullptr;
        peaks = nullptr;
        failed.clear();
        start = end = 0.0;
        if (job != 0)
            context.jobs.cancel(job);
        repaint();
        if (path.isEmpty())
            return;
        juce::Component::SafePointer<WaveformView> safe(this);
        SamplePool::shared().request(juce::File(path), [safe, wanted = path](SamplePtr loaded, const juce::String& error) {
            if (safe == nullptr || safe->path != wanted)
                return;
            safe->sampleArrived(std::move(loaded), error);
        });
    }

    void sampleArrived(SamplePtr loaded, const juce::String& error)
    {
        current = std::move(loaded);
        failed = error;
        if (current == nullptr)
            return repaint();
        if (auto* s = mirror.as<SamplerDevice>(); s != nullptr && s->currentSample() != current)
            s->setSample(current);
        auto built = std::make_shared<std::shared_ptr<const WaveformPeaks>>();
        juce::Component::SafePointer<WaveformView> safe(this);
        job = context.jobs.submit(
            "Waveform of " + current->name,
            [sample = current, built](JobPool::Context& work) { *built = WaveformPeaks::build(*sample, [&work] { return work.isCancelled(); }); },
            [safe, built](bool cancelled) {
                if (safe == nullptr || cancelled)
                    return;
                safe->job = 0;
                safe->peaks = *built;
                safe->repaint();
            });
        repaint();
    }

    DeviceContext& context;
    juce::ValueTree device;
    DeviceMirror mirror;
    std::map<juce::String, std::unique_ptr<ValueBinding>> values;
    juce::String path, failed;
    SamplePtr current;
    std::shared_ptr<const WaveformPeaks> peaks;
    std::uint64_t job = 0;
    double start = 0.0, end = 0.0;
    Marker dragged = Marker::none;
    int draggedSlice = -1;
    float lastX = 0.0f;
};

/** The root, beside the waveform: its frequency (a number box), the 12-TET note it is, and what the
    file says, with a button to load another sample. */
class RootColumn final : public juce::Component
{
public:
    RootColumn(DeviceContext& c, const juce::ValueTree& d, WaveformView& w) : context(c), device(d), wave(w)
    {
        root = std::make_unique<LabelledBox>(bind(context, device, "sample.root"));
        root->setLabel("Root (Hz)");
        rootWatch = bind(context, device, "sample.root");
        rootWatch->onChange = [this] { repaint(); }; // the note under it follows
        addAndMakeVisible(*root);
        load.setWantsKeyboardFocus(false);
        load.setTooltip("Loads a sample file into this Sampler.");
        load.onClick = [this] {
            if (!context.chooseFile)
                return;
            juce::Component::SafePointer<RootColumn> safe(this);
            context.chooseFile("Load a sample", sampleFilePatterns, [safe](const juce::File& f) {
                if (safe != nullptr)
                    loadSampleInto(safe->context, safe->device, f);
            });
        };
        addAndMakeVisible(load);
    }

    /** "A4", "A4 +12 ct": the 12-TET note nearest the root. */
    juce::String noteText() const { return noteNameFor(root->box().binding().value()); }

    int preferredWidth() const { return juce::roundToInt(themeOf(*this).controlHeight * 4.0f); }

    void resized() override
    {
        const auto& t = themeOf(*this);
        auto r = getLocalBounds();
        root->setBounds(r.removeFromTop(root->preferredSize().getHeight()));
        load.setBounds(r.removeFromBottom(juce::roundToInt(t.controlHeight)));
    }

    void paint(juce::Graphics& g) override
    {
        const auto& t = themeOf(*this);
        auto r = getLocalBounds().withTrimmedTop(root->getBottom() + juce::roundToInt(t.spacing));
        g.setFont(juce::Font(t.font(t.fontLarge)).boldened());
        g.setColour(t.text);
        g.drawText(noteText(), r.removeFromTop(juce::roundToInt(t.fontLarge + t.spacing)), juce::Justification::centred, false);
        g.setFont(t.font(t.fontSmall));
        g.setColour(t.textDim);
        if (const auto s = wave.sample(); s != nullptr && s->suggestedRootHz > 0.0)
            g.drawFittedText("The file says " + noteNameFor(s->suggestedRootHz) + " (" + juce::String(s->suggestedRootHz, 1) + " Hz)",
                             r.removeFromTop(juce::roundToInt(t.fontSmall * 2.0f + 6.0f)), juce::Justification::centredTop, 2);
    }

private:
    DeviceContext& context;
    juce::ValueTree device;
    WaveformView& wave;
    std::unique_ptr<LabelledBox> root;
    std::unique_ptr<ValueBinding> rootWatch;
    juce::TextButton load{"Load..."};
};

class SamplerPanel final : public PanelRow
{
public:
    SamplerPanel(DeviceContext& c, const juce::ValueTree& d) : PanelRow(c, d)
    {
        auto view = std::make_unique<WaveformView>(context, node);
        wave = view.get();
        addDisplay(std::move(view), 2.4f);
        auto rootColumn = std::make_unique<RootColumn>(context, node, *wave);
        auto* rc = rootColumn.get();
        addPart(std::move(rootColumn), [rc](int) { return rc->preferredWidth(); });
        addSection("Sample", {"sample.mode", "sample.quality", "sample.tuning", "sample.transpose", "sample.detune", "sample.start", "sample.end"});
        // The mode's own controls.
        loop = addSection("Loop", {"loop.on", "loop.start", "loop.length", "loop.crossfade"});
        oneShot = addSection("One-Shot", {"oneshot.fadeIn", "oneshot.fadeOut"});
        slice = addSection("Slice", {"slice.by", "slice.sensitivity", "slice.division", "slice.tempo"});
        auto envelope = std::make_unique<EnvelopeEditor>(context, node, juce::StringArray{"amp.attack", "amp.decay", "amp.sustain", "amp.release"});
        addDisplay(std::move(envelope), 0.9f);
        addSection("Amp Env", {"amp.attack", "amp.decay", "amp.sustain", "amp.release", "amp.velocity"});
        addSection("Filter", {"filter.on", "filter.type", "filter.cutoff", "filter.resonance"});
        addSection("Voice", {"voice.polyphony", "voice.choke", "voice.spread", "out.volume", "out.pan"});
        modeBinding = bind(context, node, "sample.mode");
        modeBinding->onChange = [this] { showMode(); };
        showMode();
        wave->repaint();
    }

    bool reveal(const juce::String& id) override
    {
        // A mode's controls show in that mode only; revealing one shows it anyway.
        for (auto* s : {loop, oneShot, slice})
            if (controlFor(id) != nullptr && s->isParentOf(controlFor(id)) && !s->isVisible())
            {
                s->setVisible(true);
                resized();
                widthChanged();
            }
        return controlFor(id) != nullptr;
    }

private:
    void showMode()
    {
        const auto mode = static_cast<SamplerDevice::Mode>(juce::roundToInt(modeBinding->value()));
        loop->setVisible(mode == SamplerDevice::Mode::Classic);
        oneShot->setVisible(mode == SamplerDevice::Mode::OneShot);
        slice->setVisible(mode == SamplerDevice::Mode::Slice);
        resized();
        widthChanged();
        wave->repaint();
    }

    WaveformView* wave = nullptr;
    ParameterSection *loop = nullptr, *oneShot = nullptr, *slice = nullptr;
    std::unique_ptr<ValueBinding> modeBinding;
};
} // namespace

std::unique_ptr<DeviceBody> sampler(DeviceContext& c, const juce::ValueTree& d)
{
    return std::make_unique<SamplerPanel>(c, d);
}

} // namespace lattice::ui::panels
