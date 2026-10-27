#!/usr/bin/env bash
# Linux counterpart of build.ps1: compiles the library directly (no CMake),
# archives it, then builds and runs the tests, the movement example and the
# visualizer in headless verification mode.
#
#   ./build.sh [--compiler gcc|clang] [--release] [--sanitize]
#
# Output goes to build/<compiler>-<debug|release>[-asan]/.
set -euo pipefail

compiler=gcc
release=0
sanitize=0
while [ $# -gt 0 ]; do
    case "$1" in
        --compiler)
            [ $# -ge 2 ] || { echo "--compiler needs gcc or clang" >&2; exit 2; }
            compiler=$2; shift ;;
        --compiler=*) compiler=${1#*=} ;;
        --release) release=1 ;;
        --sanitize) sanitize=1 ;;
        -h|--help)
            sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
case "$compiler" in
    gcc|clang) ;;
    *) echo "--compiler must be gcc or clang" >&2; exit 2 ;;
esac
if ! command -v "$compiler" >/dev/null 2>&1; then
    echo "$compiler was not found. Install it (e.g. apt install $compiler) or pick the other compiler with --compiler." >&2
    exit 1
fi
command -v ar >/dev/null 2>&1 || { echo "ar (binutils) is required." >&2; exit 1; }

root=$(cd "$(dirname "$0")" && pwd)
configuration=debug; [ "$release" = 1 ] && configuration=release
suffix=""; [ "$sanitize" = 1 ] && suffix=-asan
out=$root/build/$compiler-$configuration$suffix
mkdir -p "$out"

cflags=(-std=c11 -Wall -Wextra -Wpedantic -Werror -I"$root/include")
if [ "$release" = 1 ]; then cflags+=(-O2 -DNDEBUG); else cflags+=(-O0 -g); fi
ldflags=(-lm)
if [ "$sanitize" = 1 ]; then
    cflags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    ldflags+=(-fsanitize=address,undefined)
fi

run() { echo "+ $*"; "$@"; }
cd "$out"
run "$compiler" "${cflags[@]}" -c "$root/src/clockwork.c" -o clockwork.o
run "$compiler" "${cflags[@]}" -c "$root/src/cw_sim.c" -o cw_sim.o
run "$compiler" "${cflags[@]}" -c "$root/src/cw_commands.c" -o cw_commands.o
rm -f libclockwork.a
run ar rcs libclockwork.a clockwork.o cw_sim.o cw_commands.o

run "$compiler" "${cflags[@]}" "$root/tests/test_clockwork.c" -o clockwork_tests libclockwork.a "${ldflags[@]}"
run ./clockwork_tests

run "$compiler" "${cflags[@]}" "$root/examples/movement.c" -o clockwork_movement libclockwork.a "${ldflags[@]}"
run ./clockwork_movement | tee movement.log
grep -q 'After 30 ticks: (3.00, 1.00)' movement.log || { echo "movement example produced unexpected output" >&2; exit 1; }

run "$compiler" "${cflags[@]}" "$root/examples/visualizer.c" -o clockwork_visualizer libclockwork.a "${ldflags[@]}"
run ./clockwork_visualizer --frames 300 | tail -n 8

echo "Built and tested $out"
echo "Run $out/clockwork_visualizer in a terminal for the interactive view."
