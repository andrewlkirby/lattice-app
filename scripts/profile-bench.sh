#!/usr/bin/env bash
# Profiles the device benchmark with callgrind inside the Linux container.
#
#   scripts/profile-bench.sh "<benchmark name filter>" [function to count] [source file to annotate]
#
# Builds lattice_bench (RelWithDebInfo, no LTO, so functions and lines stay visible) in the
# container's build volume, counts only the given function (default: every device's
# processInChain, so table building at startup is left out), and prints the hottest functions and,
# if asked, the hottest lines of one source file.
set -euo pipefail

if [[ "${1:-}" != --inside ]]; then
    REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    export MSYS_NO_PATHCONV=1
    if command -v cygpath >/dev/null 2>&1; then
        REPO_ROOT="$(cygpath -m "$REPO_ROOT")"
    fi
    docker build --quiet -t lattice-linux:latest -f "$REPO_ROOT/docker/linux.Dockerfile" "$REPO_ROOT/docker" >/dev/null
    exec docker run --rm --cpus 4 --memory 4g -v "$REPO_ROOT:/src" -v lattice-cache:/cache -v lattice-build:/build \
        lattice-linux:latest bash /src/scripts/profile-bench.sh --inside "$@"
fi
shift
filter="${1:-}"
function="${2:-*processInChain*}"
source_file="${3:-}"
build_dir=/build/profile
cmake --preset linux-release -S /src -B "$build_dir" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -DNDEBUG" -DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO=OFF \
    -DLATTICE_BUILD_APP=OFF >/dev/null
cmake --build "$build_dir" --parallel 4 --target lattice_bench >/dev/null
out=/tmp/bench.callgrind
# Over budget is not an error here.
valgrind --tool=callgrind --callgrind-out-file="$out" --collect-atstart=no --toggle-collect="$function" \
    "$build_dir/tests/lattice_bench" --seconds 0.5 --filter "$filter" 2>/dev/null || true
callgrind_annotate "$out" | sed -n '/file:function/,/^$/p' | cut -c1-180 | head -30
if [[ -n $source_file ]]; then
    echo "== Hottest lines of $source_file"
    callgrind_annotate --auto=yes "$out" |
        awk -v f="$source_file" '/^-- (Auto-annotated|User-annotated) source:/ { on = index($0, f) > 0 } on && $1 ~ /^[0-9,]+$/ { print }' |
        sort -t'(' -k2 -rn | head -40 | cut -c1-200
fi
