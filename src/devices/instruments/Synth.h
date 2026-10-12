#pragma once

#include "devices/dsp/Envelope.h"
#include "devices/dsp/Ladder.h"
#include "devices/dsp/Lfo.h"
#include "devices/dsp/Svf.h"
#include "devices/dsp/Wavetable.h"
#include "devices/framework/StudioDevice.h"
#include "tuning/TrackTuning.h"

#include <array>
#include <cstdint>

namespace lattice
{

/** The built-in subtractive / wavetable synth, "lattice.synth" (docs/04-m3-devices.md, "Synth").

    - Two oscillators (sine, triangle, saw, pulse with PWM, wavetable), each band-limited through
      mipmapped tables; a sub-oscillator and noise; hard sync of oscillator 2 with BLEP correction.
    - Unison of 1-8 copies per oscillator, detuned in cents and spread across the stereo field.
    - A filter per voice: TPT state-variable (low-pass, high-pass, band-pass, notch; 12 or 24 dB) or
      the nonlinear ladder. Key tracking works on the note's frequency in Hz, so it is right in any
      tuning.
    - Two envelopes, two LFOs (free or tempo-synced; per voice), and an 8-slot modulation matrix.
    - 1-32 voices; poly, mono or legato; glide.
    - Every voice keeps its pitch as a fractional note (note, glide, modulation and bend in table
      steps) and gets its frequency from TrackTuning::hzAt once per control block, so glide and
      vibrato move along the active tuning, Colundi included. Increments are interpolated across the
      block, so pitch changes are smooth.

    Voice stealing takes a released voice first, then the oldest, and fades the stolen voice out
    over 3 ms in a spare voice slot while the new note starts at once.

    Audio thread: no allocation, no locks. */
class SynthDevice final : public StudioDevice
{
public:
    static constexpr const char* typeId = "lattice.synth";
    static constexpr int maxPolyphony = 32;
    static constexpr int spareVoices = 4; // where stolen voices fade out
    static constexpr int voiceSlots = maxPolyphony + spareVoices;
    static constexpr int maxUnison = 8;
    static constexpr int controlBlock = 32;
    static constexpr int numModSlots = 8;
    static constexpr double stealFadeSeconds = 0.003;

    enum class Shape : std::uint8_t
    {
        Sine,
        Triangle,
        Saw,
        Pulse,
        Wavetable
    };

    enum class FilterType : std::uint8_t
    {
        LowPass12,
        LowPass24,
        HighPass12,
        HighPass24,
        BandPass,
        Notch,
        Ladder
    };

    enum class ModSource : std::uint8_t
    {
        Off,
        Env2,
        Lfo1,
        Lfo2,
        Velocity,
        Aftertouch,
        ModWheel,
        Key,
        AmpEnv,
        Random
    };

    enum class ModDest : std::uint8_t
    {
        Off,
        Pitch,
        Osc1Pitch,
        Osc2Pitch,
        Osc1Pw,
        Osc2Pw,
        Osc1Position,
        Osc2Position,
        Osc1Level,
        Osc2Level,
        Cutoff,
        Resonance,
        Pan,
        Amp,
        Lfo1Rate,
        Lfo2Rate,
        NoiseLevel,
        SubLevel,
        count
    };

    enum class VoiceMode : std::uint8_t
    {
        Poly,
        Mono,
        Legato
    };

    /** Parameter indices, in declaration order (the IDs are in Synth.cpp). */
    enum Param : int
    {
        osc1Shape, osc1Level, osc1Transpose, osc1Detune, osc1Pw, osc1Table, osc1Position,
        osc2Shape, osc2Level, osc2Transpose, osc2Detune, osc2Pw, osc2Table, osc2Position, osc2Sync,
        subLevel, subOctave, noiseLevel, noiseColour,
        unisonVoices, unisonDetune, unisonSpread,
        filterType, filterCutoff, filterResonance, filterDrive, filterKeytrack, filterEnvAmount, filterVelocity,
        ampAttack, ampDecay, ampSustain, ampRelease, ampVelocity,
        env2Attack, env2Decay, env2Sustain, env2Release,
        lfo1Shape, lfo1Rate, lfo1Sync, lfo1SyncRate, lfo1Retrigger,
        lfo2Shape, lfo2Rate, lfo2Sync, lfo2SyncRate, lfo2Retrigger,
        mod1Source, mod1Dest, mod1Amount,
        mod2Source, mod2Dest, mod2Amount,
        mod3Source, mod3Dest, mod3Amount,
        mod4Source, mod4Dest, mod4Amount,
        mod5Source, mod5Dest, mod5Amount,
        mod6Source, mod6Dest, mod6Amount,
        mod7Source, mod7Dest, mod7Amount,
        mod8Source, mod8Dest, mod8Amount,
        voiceMode, polyphony, glideTime,
        outVolume, outPan,
        numParams
    };

    explicit SynthDevice(const TrackTuning& tuning);

    void releaseAllNotes() noexcept override;
    void reset() noexcept override;
    double getTailLengthSeconds() const override { return 2.0; }

    /** Sounding voices, including ones fading out (tests). */
    int activeVoiceCount() const noexcept;

protected:
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept override;

private:
    struct OscCopy
    {
        std::uint32_t phase = 0;
        std::uint32_t inc = 0;
        float gainLeft = 0.7071f;
        float gainRight = 0.7071f;
        float blep = 0.0f; // osc 2 with sync: the BLEP correction still due on the next sample
    };

    struct Voice
    {
        int note = -1;
        int channel = 0;
        float velocity = 0.0f; // 0..1
        bool gate = false;     // key (or sustain pedal) held
        bool keyDown = false;
        bool stealing = false; // fading out after being stolen
        std::uint64_t age = 0;

        double pitch = 60.0;       // fractional note, glide included
        double pitchTarget = 60.0;
        double glideRate = 0.0;    // notes per sample (0 = jump)
        float pressure = 0.0f;
        float random = 0.0f;

        dsp::Adsr amp;
        dsp::Adsr env2;
        dsp::Lfo lfo1;
        dsp::Lfo lfo2;
        std::array<OscCopy, maxUnison> osc1{};
        std::array<OscCopy, maxUnison> osc2{};
        std::uint32_t subPhase = 0;
        std::uint32_t subInc = 0;
        float noiseState = 0.0f;
        std::uint32_t noiseSeed = 1;
        dsp::Svf svf[2];
        dsp::LadderFilter ladder[2];
        float filterG = -1.0f; // last coefficient, for interpolation; < 0 = not set yet
        float lastPan = 0.0f;
        bool started = false; // increments are set (no interpolation from zero)

        bool isActive() const noexcept { return amp.isActive(); }
    };

    void handleEvent(const TimedMidi& e, const DeviceTimeInfo& time) noexcept LATTICE_NONBLOCKING;
    void noteOn(int note, int velocity, int channel) noexcept LATTICE_NONBLOCKING;
    void noteOff(int note, int channel) noexcept LATTICE_NONBLOCKING;
    void startVoice(Voice& v, int note, float velocity, int channel, bool retrigger) noexcept LATTICE_NONBLOCKING;
    Voice* allocateVoice() noexcept LATTICE_NONBLOCKING;
    void setSustain(bool down) noexcept LATTICE_NONBLOCKING;
    void renderSegment(float* left, float* right, int numSamples, const DeviceTimeInfo& time,
                       int offset) noexcept LATTICE_NONBLOCKING;
    void renderVoice(Voice& v, float* left, float* right, int numSamples, const DeviceTimeInfo& time,
                     int offset) noexcept LATTICE_NONBLOCKING;
    void readPatch() noexcept LATTICE_NONBLOCKING;
    int heldNoteForMono() const noexcept;

    const TrackTuning& tuning;
    const dsp::WaveformLibrary& waves;
    std::array<Voice, voiceSlots> voices;
    std::uint64_t noteCounter = 0;
    float bend = 0.0f;
    float modWheel = 0.0f;
    float channelPressure = 0.0f;
    bool sustainDown = false;
    double lastPitch = -1.0; // for glide in poly mode
    double freeLfoPhase[2] = {0.0, 0.0};
    std::uint32_t randomSeed = 0x2545f491u;

    // Mono and legato: the keys held, in the order they were pressed.
    std::array<std::int16_t, 128> monoStack{};
    int monoCount = 0;

    // Output
    float outGainLeft = 0.5f;
    float outGainRight = 0.5f;
    float targetGainLeft = 0.5f;
    float targetGainRight = 0.5f;
    float gainStep = 0.001f;

    /** Parameter values, read once per block. */
    struct PatchValues
    {
        Shape shape[2]{};
        float level[2]{};
        float transpose[2]{};
        float detune[2]{};
        float pw[2]{};
        int table[2]{};
        float position[2]{};
        bool sync = false;
        float sub = 0.0f;
        int subOctave = 1;
        float noise = 0.0f;
        float noiseColour = 0.5f;
        int unison = 1;
        float unisonDetune = 0.0f;
        float unisonSpread = 0.0f;
        FilterType filter = FilterType::LowPass24;
        float cutoff = 2000.0f;
        float resonance = 0.0f;
        float drive = 1.0f;
        float keytrack = 0.0f;
        float envAmount = 0.0f;
        float filterVelocity = 0.0f;
        dsp::Adsr::Settings ampEnv;
        dsp::Adsr::Settings modEnv;
        float ampVelocity = 0.5f;
        dsp::LfoShape lfoShape[2]{};
        double lfoRate[2]{};
        bool lfoSync[2]{};
        double lfoBeats[2]{};
        bool lfoRetrigger[2]{};
        ModSource modSource[numModSlots]{};
        ModDest modDest[numModSlots]{};
        float modAmount[numModSlots]{};
        VoiceMode mode = VoiceMode::Poly;
        int polyphony = 16;
        double glideSeconds = 0.0;
        // Derived: unison detune ratios and pan gains per copy
        std::array<double, maxUnison> unisonRatio{};
        std::array<float, maxUnison> unisonLeft{};
        std::array<float, maxUnison> unisonRight{};
        float unisonNormal = 1.0f;
    } patch;
};

} // namespace lattice
