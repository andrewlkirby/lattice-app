#pragma once

#include "devices/framework/StudioDevice.h"
#include "devices/test/TuningTestSynth.h"

namespace lattice
{

/** The M1 TuningTestSynth as a device ("lattice.testSynth"): a plain sine-and-saw instrument with
    an exact fundamental, kept for tests and as a clean reference for hearing tunings. Hidden from
    the device list. Stereo out (the mono synth on both channels). */
class TestSynthDevice final : public StudioDevice
{
public:
    static constexpr const char* typeId = "lattice.testSynth";

    explicit TestSynthDevice(const TrackTuning& tuning);

    TuningTestSynth& synth() noexcept { return testSynth; }
    void releaseAllNotes() noexcept override { testSynth.allNotesOff(); }
    double getTailLengthSeconds() const override { return 0.5; }

protected:
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept override;

private:
    TuningTestSynth testSynth;
};

} // namespace lattice
