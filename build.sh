#!/usr/bin/env bash
# Linux counterpart of build.ps1: compiles the library directly (no CMake),
# archives it, then builds and runs the tests, the movement and save/load
# examples and the visualizer in headless verification mode.
#
#   ./build.sh [--compiler gcc|clang] [--release] [--sanitize] [--no-gui]
#
# --no-gui skips the interactive save/load tool, which builds the vendored
# raylib and needs the X11 development headers (libx11-dev libxrandr-dev
# libxcursor-dev libxinerama-dev libxi-dev libxext-dev). CW_EXTRA_CFLAGS adds
# flags to every compile (e.g. -I for headers in an unusual place).
# Output goes to build/<compiler>-<debug|release>[-asan]/.
set -euo pipefail

compiler=gcc
release=0
sanitize=0
gui=1
while [ $# -gt 0 ]; do
    case "$1" in
        --compiler)
            [ $# -ge 2 ] || { echo "--compiler needs gcc or clang" >&2; exit 2; }
            compiler=$2; shift ;;
        --compiler=*) compiler=${1#*=} ;;
        --release) release=1 ;;
        --sanitize) sanitize=1 ;;
        --no-gui) gui=0 ;;
        -h|--help)
            sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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
# shellcheck disable=SC2206
[ -n "${CW_EXTRA_CFLAGS:-}" ] && cflags+=($CW_EXTRA_CFLAGS)
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
run "$compiler" "${cflags[@]}" -c "$root/src/cw_bytes.c" -o cw_bytes.o
run "$compiler" "${cflags[@]}" -c "$root/src/cw_snapshot.c" -o cw_snapshot.o
run "$compiler" "${cflags[@]}" -c "$root/src/cw_file.c" -o cw_file.o
rm -f libclockwork.a
run ar rcs libclockwork.a clockwork.o cw_sim.o cw_commands.o cw_bytes.o cw_snapshot.o cw_file.o

run "$compiler" "${cflags[@]}" "$root/tests/test_clockwork.c" -o clockwork_tests libclockwork.a "${ldflags[@]}"
run ./clockwork_tests

run "$compiler" "${cflags[@]}" "$root/examples/movement.c" -o clockwork_movement libclockwork.a "${ldflags[@]}"
run ./clockwork_movement | tee movement.log
grep -q 'After 30 ticks: (3.00, 1.00)' movement.log || { echo "movement example produced unexpected output" >&2; exit 1; }

if [ "$gui" = 1 ]; then
    # The save/load tool: vendored raylib (2D desktop subset) + raygui, X11 backend.
    raylib=$root/third_party/raylib/src
    rlflags=(-std=gnu99 -w -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_33 -D_GLFW_X11
             -I"$raylib" -I"$raylib/external/glfw/include")
    [ "$release" = 1 ] && rlflags+=(-O2) || rlflags+=(-O1)
    [ -n "${CW_EXTRA_CFLAGS:-}" ] && rlflags+=($CW_EXTRA_CFLAGS)
    rlobjs=()
    for unit in rcore rshapes rtextures rtext utils rglfw raygui_impl; do
        run "$compiler" "${rlflags[@]}" -c "$raylib/$unit.c" -o "raylib_$unit.o"
        rlobjs+=("raylib_$unit.o")
    done
    rm -f libclockwork_raylib.a
    run ar rcs libclockwork_raylib.a "${rlobjs[@]}"
    run "$compiler" "${cflags[@]}" -I"$raylib" "$root/examples/save_load.c" -o clockwork_save_load \
        libclockwork.a libclockwork_raylib.a "${ldflags[@]}" -lpthread -ldl -lrt -lX11
    run ./clockwork_save_load --headless | tee save_load.log
    grep -q 'OK: save/load verified' save_load.log || { echo "save/load tool produced unexpected output" >&2; exit 1; }
fi

run "$compiler" "${cflags[@]}" "$root/examples/visualizer.c" -o clockwork_visualizer libclockwork.a "${ldflags[@]}"
run ./clockwork_visualizer --frames 300 | tail -n 8

echo "Built and tested $out"
echo "Run $out/clockwork_visualizer in a terminal for the interactive view."
[ "$gui" = 1 ] && echo "Run $out/clockwork_save_load for the interactive save/load tool."
