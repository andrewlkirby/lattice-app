# Linux build-and-test image for Lattice (docs/01-m0-skeleton.md, "Linux through Docker").
# Used by scripts/linux-test.{sh,ps1} and by CI, so local and CI runs match.
FROM ubuntu:24.04

ARG LLVM_VERSION=21
ENV DEBIAN_FRONTEND=noninteractive

# Toolchains and tools. Clang comes from apt.llvm.org: RTSan needs Clang 20 or newer.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates gnupg wget lsb-release software-properties-common \
    && wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/noble/ llvm-toolchain-noble-${LLVM_VERSION} main" \
        > /etc/apt/sources.list.d/llvm.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build ccache git pkg-config valgrind python3 \
        clang-${LLVM_VERSION} clang-tidy-${LLVM_VERSION} lld-${LLVM_VERSION} llvm-${LLVM_VERSION} \
        libclang-rt-${LLVM_VERSION}-dev \
        # JUCE's Linux dependencies (no WebKit or curl: JUCE_WEB_BROWSER=0, JUCE_USE_CURL=0)
        libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev \
        libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev libxcomposite-dev \
        libgl1-mesa-dev \
        # Test support: virtual display for UI tests, and a German locale for M1's
        # locale-independence test (comma decimal separator).
        xvfb xauth locales \
    && sed -i 's/^# *\(de_DE.UTF-8\)/\1/; s/^# *\(en_US.UTF-8\)/\1/' /etc/locale.gen && locale-gen \
    && rm -rf /var/lib/apt/lists/*

# Unversioned names for the presets (linux-rtsan uses clang/clang++); LTO needs llvm-ar/llvm-ranlib.
RUN for tool in clang clang++ clang-tidy run-clang-tidy ld.lld llvm-ar llvm-ranlib; do \
        ln -sf "/usr/bin/${tool}-${LLVM_VERSION}" "/usr/local/bin/${tool}"; \
    done

ENV LANG=en_US.UTF-8 \
    CCACHE_DIR=/cache/ccache \
    CCACHE_MAXSIZE=5G \
    CPM_SOURCE_CACHE=/cache/cpm

WORKDIR /src
