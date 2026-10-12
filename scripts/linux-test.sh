#!/usr/bin/env bash
# Build and test Lattice on Linux inside Docker (docs/01-m0-skeleton.md, "Linux through Docker").
#
#   scripts/linux-test.sh [options]
#     --debug       linux-debug preset (default: linux-release)
#     --rtcheck     linux-relwithdebinfo-rtchecks preset
#     --rtsan       linux-rtsan preset (Clang + RealtimeSanitizer)
#     --tsan        linux-tsan preset (Clang + ThreadSanitizer)
#     --clang       build with Clang instead of GCC
#     --tidy        also run the clang-tidy performance-* checks (implies --clang)
#     --all-tests   include the heavy tags ([slow] [bench] [stress] [hardware] [timing])
#     --callgrind   also count the engine benchmark's instructions and compare them with
#                   docs/perf/baselines/engine-callgrind.txt (fails if more than 5% higher)
#     --update-baseline  like --callgrind, but write the count as the new baseline
#     --shell       open a shell in the container instead of building
#
# Run it from Linux or WSL; scripts/linux-test.ps1 does the same from Windows. Inside the container
# the script is re-invoked with --inside. Builds live in a named volume (fast, and never written to
# the source tree); ccache and the CPM download cache share another named volume.
# Budget: at most 4 cores and 4 GB of RAM.
set -euo pipefail

IMAGE=lattice-linux:latest
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

inside=0
preset=linux-release
clang=0
tidy=0
all_tests=0
callgrind=0
update_baseline=0
shell=0
for arg in "$@"; do
    case "$arg" in
        --inside) inside=1 ;;
        --debug) preset=linux-debug ;;
        --rtcheck) preset=linux-relwithdebinfo-rtchecks ;;
        --rtsan) preset=linux-rtsan ;;
        --tsan) preset=linux-tsan ;;
        --clang) clang=1 ;;
        --tidy) tidy=1; clang=1 ;;
        --all-tests) all_tests=1 ;;
        --callgrind) callgrind=1 ;;
        --update-baseline) callgrind=1; update_baseline=1 ;;
        --shell) shell=1 ;;
        -h|--help) sed -n '2,21p' "$0"; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; exit 2 ;;
    esac
done

if [[ $inside -eq 0 ]]; then
    # Git Bash on Windows: stop MSYS from rewriting container paths like /src into Windows paths,
    # and hand Docker a Windows-style path for the bind mount.
    export MSYS_NO_PATHCONV=1
    if command -v cygpath >/dev/null 2>&1; then
        REPO_ROOT="$(cygpath -m "$REPO_ROOT")"
    fi
    docker build --quiet -t "$IMAGE" -f "$REPO_ROOT/docker/linux.Dockerfile" "$REPO_ROOT/docker" >/dev/null
    tty_flags=()
    [[ -t 0 && -t 1 ]] && tty_flags=(-it)
    # ThreadSanitizer re-executes itself with address randomisation off (personality syscall),
    # which Docker's default seccomp profile blocks. Only the TSan run relaxes it.
    security=()
    [[ $preset == linux-tsan ]] && security=(--security-opt seccomp=unconfined)
    # Forward the warnings-as-errors switch, so local runs can match CI exactly.
    run=(docker run --rm "${tty_flags[@]}" "${security[@]}" --cpus 4 --memory 4g
         -e "LATTICE_WARNINGS_AS_ERRORS=${LATTICE_WARNINGS_AS_ERRORS:-OFF}"
         -v "$REPO_ROOT:/src"
         -v lattice-cache:/cache
         -v lattice-build:/build
         "$IMAGE")
    if [[ $shell -eq 1 ]]; then
        exec "${run[@]}" bash
    fi
    exec "${run[@]}" bash /src/scripts/linux-test.sh --inside "$@"
fi

# ---- Inside the container ----------------------------------------------------------------------
start=$(date +%s)
build_dir="/build/${preset}"
extra=()
if [[ $clang -eq 1 && $preset != linux-rtsan && $preset != linux-tsan ]]; then
    build_dir+="-clang"
    extra+=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
fi
# The app needs no window in these runs, but it must still build and link on Linux.
extra+=(-DLATTICE_WARNINGS_AS_ERRORS="${LATTICE_WARNINGS_AS_ERRORS:-OFF}")

echo "== Configure ($preset -> $build_dir)"
cmake --preset "$preset" -S /src -B "$build_dir" "${extra[@]}" >/dev/null

echo "== Build"
cmake --build "$build_dir" --parallel 4

echo "== Test"
ctest_args=(--test-dir "$build_dir" --output-on-failure --no-tests=error)
if [[ $all_tests -eq 0 ]]; then
    if [[ $preset == linux-debug ]]; then
        ctest_args+=(-LE "slow|bench|stress|hardware|timing|heavy") # unoptimised DSP measurements: Release runs them
    else
        ctest_args+=(-LE "slow|bench|stress|hardware|timing")
    fi
fi
xvfb-run -a ctest "${ctest_args[@]}"

if [[ $tidy -eq 1 ]]; then
    echo "== clang-tidy (performance-*)"
    run-clang-tidy -quiet -j 4 -p "$build_dir" "^/src/(src|tests)/.*\.cpp$"
fi

if [[ $callgrind -eq 1 ]]; then
    # Instruction counts don't depend on how fast or busy the machine is (charter §2), so they can
    # catch slowdowns on shared CI machines. Only the benchmark's render loop is counted.
    echo "== Instruction count (callgrind: engine benchmark, reference set, 2 s at 64-sample blocks)"
    [[ $preset == linux-release && $clang -eq 0 ]] || echo "warning: the baseline is for the GCC linux-release build"
    baseline_file=/src/docs/perf/baselines/engine-callgrind.txt
    out=/tmp/engine.callgrind
    xvfb-run -a valgrind --tool=callgrind --callgrind-out-file="$out" --collect-atstart=no \
        --toggle-collect='*renderReferenceSet*' \
        "$build_dir/tests/lattice_engine_bench" --seconds 2 --block 64 >/dev/null 2>/tmp/valgrind.log
    count=$(callgrind_annotate "$out" 2>/dev/null | awk '/PROGRAM TOTALS/ { gsub(",", "", $1); print $1; exit }')
    [[ -n $count ]] || { cat /tmp/valgrind.log; echo "callgrind produced no count" >&2; exit 1; }
    echo "Instructions: $count"
    if [[ $update_baseline -eq 1 ]]; then
        {
            echo "# Instructions in the engine benchmark's render loop (lattice_engine_bench --seconds 2"
            echo "# --block 64: the M2 reference set), GCC linux-release in docker/linux.Dockerfile."
            echo "# Written by scripts/linux-test.sh --update-baseline; update it in the same commit as a"
            echo "# deliberate change, with the reason (charter §3)."
            echo "$count"
        } > "$baseline_file"
        echo "Baseline updated: $baseline_file"
    elif [[ -f $baseline_file ]]; then
        baseline=$(grep -v '^#' "$baseline_file" | head -1)
        python3 - "$count" "$baseline" <<'PY'
import sys
count, baseline = int(sys.argv[1]), int(sys.argv[2])
change = count / baseline - 1.0
print(f"Baseline: {baseline}  change: {change * 100:+.2f}%")
if change > 0.05:
    print("FAIL: more than 5% above the baseline (charter §3)")
    sys.exit(1)
if change < -0.05:
    print("Note: more than 5% below the baseline; consider --update-baseline")
PY
    else
        echo "No baseline yet ($baseline_file); run with --update-baseline"
    fi
fi

ccache --show-stats --verbose 2>/dev/null | grep -E "Hits|Misses" | head -3 || true
echo "== Done in $(( $(date +%s) - start )) s"
