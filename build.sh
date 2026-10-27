#!/usr/bin/env bash
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
            printf 'Usage: %s [--compiler gcc|clang] [--release] [--sanitize] [--no-gui]\n' "$0"; exit 0 ;;
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
    sokol=$(bash "$root/tools/prepare-sokol.sh")
    sokol_flags=(-std=c11 -w -I"$sokol")
    gui_flags=()
    gui_libraries=(-lX11 -lXi -lXcursor -lGL -lpthread -ldl)
    if [ "$(uname -s)" = Darwin ]; then
        sokol_flags+=(-DSOKOL_METAL -x objective-c)
        gui_flags+=(-DSOKOL_METAL)
        gui_libraries=(-framework Cocoa -framework QuartzCore -framework Metal -framework MetalKit)
    else
        sokol_flags+=(-DSOKOL_GLCORE)
        gui_flags+=(-DSOKOL_GLCORE)
    fi
    [ "$release" = 1 ] && sokol_flags+=(-O2) || sokol_flags+=(-O1)
    [ -n "${CW_EXTRA_CFLAGS:-}" ] && sokol_flags+=($CW_EXTRA_CFLAGS)
    run "$compiler" "${sokol_flags[@]}" -c "$root/examples/sokol_impl.c" -o sokol_impl.o
    rm -f libclockwork_sokol.a
    run ar rcs libclockwork_sokol.a sokol_impl.o
    run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -I"$sokol" "$root/examples/save_load.c" -o clockwork_save_load \
        libclockwork.a libclockwork_sokol.a "${ldflags[@]}" "${gui_libraries[@]}"
    run ./clockwork_save_load --headless | tee save_load.log
    grep -q 'OK: save/load verified' save_load.log || { echo "save/load tool produced unexpected output" >&2; exit 1; }
fi

run "$compiler" "${cflags[@]}" "$root/examples/visualizer.c" -o clockwork_visualizer libclockwork.a "${ldflags[@]}"
run ./clockwork_visualizer --frames 300 | tail -n 8

echo "Built and tested $out"
echo "Run $out/clockwork_visualizer in a terminal for the interactive view."
[ "$gui" = 1 ] && echo "Run $out/clockwork_save_load for the interactive save/load tool."
exit 0
