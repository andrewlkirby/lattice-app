#pragma once

#include "core/SettledShared.h"
#include "devices/dsp/Envelope.h"
#include "devices/dsp/SincResampler.h"
#include "devices/dsp/Svf.h"
#include "devices/framework/StudioDevice.h"
#include "devices/samples/SamplePool.h"
#include "tuning/TrackTuning.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace lattice
{

/** The built-in sampler, "lattice.sampler" (docs/04-m3-devices.md, "Sampler", like Live's Simpler).

    - Modes: Classic (pitched, ADSR, loop with crossfade), One-Shot (plays through, ignores note-off,
      fades), Slice (by transient, beat grid or manual points; slice i on note 36 + i).
    - Pitch: the sample has a root frequency in Hz. Playback rate = hzAt(note) / rootHz, so a 440 Hz
      sample with its root at 440 Hz plays the tuning's exact frequencies, Colundi included. With
      "Tuning: Fixed" it ignores the track tuning (12-TET from C4), which Drum Rack pads use.
    - Interpolation: windowed sinc (SincKernelBank: 64 taps at unity, band-limited when pitched up)
      or "Draft" cubic Hermite.
    - The file is decoded on a background thread by the SamplePool (shared between devices). Until
      it arrives the sampler is silent; then it is swapped in. A new sample replaces the old after a
      3 ms fade of whatever was playing; the old one stays alive until the audio thread has let go.

    STATE: `file` (the sample's path) and `slices` (manual slice points, frames, comma-separated). */
class SamplerDevice final : public StudioDevice
{
public:
    static constexpr const char* typeId = "lattice.sampler";
    static constexpr int maxVoices = 32;
    static constexpr int voiceSlots = maxVoices + 4;
    static constexpr int controlBlock = 64;
    static constexpr int firstSliceNote = 36;
    static constexpr double declickSeconds = 0.001;
    static constexpr double stealFadeSeconds = 0.003;

    enum class Mode : std::uint8_t
    {
        Classic,
        OneShot,
        Slice
    };

    enum class Quality : std::uint8_t
    {
        Sinc,
        Draft
    };

    enum class SliceBy : std::uint8_t
    {
        Transient,
        Beat,
        Manual
    };

    /** Parameter indices, in declaration order. (In a struct, so the names don't fill the
        class scope.) */
    struct Param
    {
        enum : std::uint8_t
        {
            root, transpose, detune, tuningMode, start, end, mode, quality,
            loopOn, loopStart, loopLength, loopCrossfade,
            fadeIn, fadeOut,
            ampAttack, ampDecay, ampSustain, ampRelease, ampVelocity,
            filterOn, filterType, filterCutoff, filterResonance,
            sliceBy, sliceSensitivity, sliceDivision, sliceTempo,
            polyphony, choke, spread,
            outVolume, outPan,
            numParams
        };
    };

    SamplerDevice(const TrackTuning& tuning, SamplePool& pool);
    ~SamplerDevice() override;

    void releaseAllNotes() noexcept override;
    void choke() noexcept override;
    void reset() noexcept override;
    double getTailLengthSeconds() const override { return 2.0; }

    /** Message thread: plays this sample from now on (tests and generated samples; normally the
        STATE's file is loaded through the pool). */
    void setSample(SamplePtr sample);
    /** Message thread: the sample published last (null while none or still loading). */
    SamplePtr currentSample() const;
    /** Message thread: true while a file is being loaded. */
    bool isLoading() const noexcept { return loadingFile.isNotEmpty(); }
    juce::String lastError() const { return error; }

    int activeVoiceCount() const noexcept;
    /** The slice start frames in effect for the current sample and settings (tests, editors). */
    std::vector<std::int64_t> slicePoints() const;

protected:
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept override;
    void stateChanged(const juce::ValueTree& state) override;

private:
    /** What the audio thread plays from: the sample and the manual slice points. */
    struct Source
    {
        SamplePtr sample;
        std::vector<std::int64_t> manualSlices;
    };
    using SourceVersion = SettledShared<Source>::Version;

    struct Voice
    {
        bool active = false;
        bool gate = false;
        bool stealing = false;
        bool oneShot = false;
        int note = -1;
        float velocity = 0.0f;
        std::uint64_t age = 0;
        double position = 0.0;   // frames into the sample
        double rate = 1.0;       // frames per output sample
        double regionStart = 0.0;
        double regionEnd = 0.0;
        bool loop = false;
        double loopStart = 0.0;
        double loopEnd = 0.0;
        bool pitched = true;     // follows notes (not a slice)
        float panL = 0.70710678f;
        float panR = 0.70710678f;
        std::int64_t played = 0; // output samples since the start (fade in)
        float killGain = 1.0f;
        float killStep = 0.0f;
        dsp::Adsr amp;
        dsp::Svf svf[2];
    };

    void handleEvent(const TimedMidi& e) noexcept LATTICE_NONBLOCKING;
    void noteOn(int note, int velocity) noexcept LATTICE_NONBLOCKING;
    void noteOff(int note) noexcept LATTICE_NONBLOCKING;
    Voice* allocateVoice() noexcept LATTICE_NONBLOCKING;
    void render(float* left, float* right, int numSamples) noexcept LATTICE_NONBLOCKING;
    void renderVoice(Voice& v, float* left, float* right, int numSamples) noexcept LATTICE_NONBLOCKING;
    double rateFor(const Voice& v) const noexcept LATTICE_NONBLOCKING;
    /** Slice boundaries for the current settings into `out` (fixed capacity); returns the count. */
    int computeSlices(const Source& source, std::int64_t* out, int capacity) const noexcept LATTICE_NONBLOCKING;
    void loadFile(const juce::String& path);

    const TrackTuning& tuning;
    SamplePool& pool;
    SettledShared<Source> sources;
    const SourceVersion* active = nullptr;
    bool switching = false;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    juce::String loadingFile;
    juce::String loadedFile;
    juce::String error;
    std::vector<std::int64_t> manualSlices; // message thread copy

    std::array<Voice, voiceSlots> voices;
    std::uint64_t noteCounter = 0;
    std::uint32_t randomSeed = 0x6d2b79f5u;
    float bend = 0.0f;
    bool sustainDown = false;
    std::array<bool, 128> sustained{};
    std::array<std::int64_t, 512> sliceScratch{};

    float outGainLeft = 0.5f;
    float outGainRight = 0.5f;
    float gainStep = 0.001f;
};

} // namespace lattice
