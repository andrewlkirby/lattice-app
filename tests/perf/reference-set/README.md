# Performance reference set

The reference set from `docs/09-performance-charter.md` §1. At M2 it is 32 tracks of test-synth
placeholders with clips in 8 scenes, built by `tests/perf/ReferenceSet.h`. It grows with each
milestone.

The `.lset` file is generated, not committed. To open the set in Lattice, write it out with the
engine benchmark and open it with File > Open (Ctrl+O):

    build\windows-release\tests\Release\lattice_engine_bench.exe --write-set tests\perf\reference-set\reference-m2.lset

The same program measures the engine on it (`--seconds N --block N`), and
`scripts/linux-test.sh --callgrind` counts its instructions against `docs/perf/baselines/`.
