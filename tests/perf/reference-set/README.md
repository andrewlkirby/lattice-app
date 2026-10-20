# Performance reference set

The reference set from `docs/09-performance-charter.md` §1, built by `tests/perf/ReferenceSet.h`.
At M3 it is 32 tracks with clips in 8 scenes:

- 16 synth tracks (the Synth plus an EQ Eight; four of them duck under a sidechain compressor
  keyed from the first drum track);
- 8 drum tracks (short, noisy Synth hits);
- 8 bass tracks, still named "Sampler": a Synth stands in for the Sampler until the set can ship
  the samples it would play;
- every track sends to return A (Reverb) and every other track to return B (Delay); a Limiter
  sits on the master.

It grows with each milestone.

The `.lset` file is generated, not committed. To open the set in Lattice, write it out with the
engine benchmark and open it with File > Open (Ctrl+O):

    build\windows-release\tests\Release\lattice_engine_bench.exe --write-set tests\perf\reference-set\reference-m3.lset

The same program measures the engine on it (`--seconds N --block N --workers N`, where
`--workers -1` uses every physical core). `scripts/linux-test.sh --callgrind` counts its
instructions, with no workers, against `docs/perf/baselines/`.
