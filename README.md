# Lattice

A Session-view DAW in the spirit of Ableton Live, with deep Colundi-scale support and Push-style
and generic MIDI controller support. Plans and status are in [docs/](docs/README.md).

## Build (Windows)
Needs Visual Studio 2022 or newer with the "Desktop development with C++" workload (CMake comes
with it). Dependencies (JUCE, Catch2) are downloaded on the first configure into `.cpm-cache/`.
Keep the checkout at a short path: deep folders hit Windows' 260-character path limit during the
build.

```
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Presets: `windows-debug`, `windows-release`, and `windows-relwithdebinfo-rtchecks` (the everyday
development build: optimised, with the real-time checks on). The app is written to
`build/<preset>/Lattice_artefacts/<Config>/Lattice.exe`.

## Linux (through Docker)
With Docker Desktop running:

```
scripts\linux-test.ps1             # GCC release build + tests
scripts\linux-test.ps1 --rtsan     # Clang RealtimeSanitizer build + tests
scripts\linux-test.ps1 --tidy      # Clang build + tests + clang-tidy performance-* checks
scripts\linux-test.ps1 --shell     # a shell in the container
```

## Performance
See [docs/09-performance-charter.md](docs/09-performance-charter.md). `scripts\perf-check.ps1`
checks startup time on the reference machine (`docs/perf/reference-machine.md`).

## Options
| CMake option | Default | Meaning |
|---|---|---|
| `LATTICE_RT_CHECKS` | `AUTO` | Allocation hooks and audio-thread asserts in the app: `AUTO` = Debug only, `ON`, `OFF`. Always on in the tests |
| `LATTICE_ENABLE_ASIO` | `ON` on Windows | ASIO support. The Steinberg ASIO SDK (GPLv3 option) is downloaded at configure time; `LATTICE_ASIO_SDK_DIR` uses a local copy instead |
| `LATTICE_PROFILE` | `OFF` | Tracy profiler markers |
| `LATTICE_RTSAN` | `OFF` | Clang RealtimeSanitizer (Clang 20+) |
| `LATTICE_WARNINGS_AS_ERRORS` | `OFF` | CI turns it on |
