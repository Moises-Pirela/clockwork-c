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

sokol=$(bash "$root/tools/prepare-sokol.sh")
run "$compiler" "${cflags[@]}" -I"$root/render2d" -I"$sokol" "$root/tests/test_render2d.c" -o clockwork_render2d_tests "${ldflags[@]}"
run ./clockwork_render2d_tests
run "$compiler" "${cflags[@]}" -I"$root/render2d" -I"$sokol" "$root/tests/test_r2d_fx.c" "$root/render2d/r2d_particles.c" "$root/render2d/r2d_effects.c" -o clockwork_r2d_fx_tests "${ldflags[@]}"
run ./clockwork_r2d_fx_tests
run "$compiler" "${cflags[@]}" -I"$root/render2d" -I"$sokol" "$root/tests/test_r2d_post.c" "$root/render2d/r2d_post_params.c" "$root/render2d/r2d_tunables.c" "$root/render2d/r2d_shadow.c" "$root/render2d/r2d_quality.c" -o clockwork_r2d_post_tests "${ldflags[@]}"
run ./clockwork_r2d_post_tests

run "$compiler" "${cflags[@]}" "$root/examples/movement.c" -o clockwork_movement libclockwork.a "${ldflags[@]}"
run ./clockwork_movement | tee movement.log
grep -q 'After 30 ticks: (3.00, 1.00)' movement.log || { echo "movement example produced unexpected output" >&2; exit 1; }

if [ "$gui" = 1 ]; then
    sokol_flags=(-std=c11 -w -I"$sokol")
    gui_flags=()
    gui_libraries=()
    if [ "$(uname -s)" = Darwin ]; then
        sokol_flags+=(-DSOKOL_METAL -x objective-c)
        gui_flags+=(-DSOKOL_METAL)
        gui_libraries=(-framework Cocoa -framework QuartzCore -framework Metal -framework MetalKit)
    else
        # The X11 and OpenGL headers Sokol needs come bundled (third_party/x11,
        # third_party/gl), so no development package is needed; these -I must
        # precede any system include path.
        [ -f "$root/third_party/gl/GL/gl.h" ] || { echo "Bundled OpenGL header missing: third_party/gl/GL/gl.h" >&2; exit 1; }
        [ -f "$root/third_party/x11/X11/Xlib.h" ] || { echo "Bundled X11 header missing: third_party/x11/X11/Xlib.h" >&2; exit 1; }
        sokol_flags=(-std=c11 -w -I"$root/third_party/x11" -I"$root/third_party/gl" -I"$sokol" -DSOKOL_GLCORE)
        gui_flags+=(-DSOKOL_GLCORE)
        # Link each library through its development symlink (-lX) when present,
        # otherwise directly by runtime file name (-l:libX.so.N), which needs no
        # -dev package at all.
        link_library() {
            if echo 'int main(void){return 0;}' | "$compiler" -x c - -o /dev/null "-l$1" >/dev/null 2>&1; then
                gui_libraries+=("-l$1")
            elif echo 'int main(void){return 0;}' | "$compiler" -x c - -o /dev/null "-l:$2" >/dev/null 2>&1; then
                gui_libraries+=("-l:$2")
            else
                echo "Cannot link $1: neither lib$1.so nor $2 was found. The runtime library must be installed (any desktop has it), or use --no-gui." >&2
                exit 1
            fi
        }
        link_library X11 libX11.so.6
        link_library Xi libXi.so.6
        link_library Xcursor libXcursor.so.1
        link_library GL libGL.so.1
        gui_libraries+=(-lpthread -ldl)
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

    run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -I"$root/render2d" -I"$sokol" -c "$root/render2d/render2d.c" -o render2d.o
    run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -I"$root/render2d" -I"$sokol" -c "$root/render2d/r2d_particles.c" -o r2d_particles.o
    run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -I"$root/render2d" -I"$sokol" -c "$root/render2d/r2d_effects.c" -o r2d_effects.o
    run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -I"$root/render2d" -I"$sokol" -c "$root/render2d/r2d_tunables.c" -o r2d_tunables.o
    run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -I"$root/render2d" -I"$sokol" -c "$root/render2d/r2d_post_params.c" -o r2d_post_params.o
    run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -I"$root/render2d" -I"$sokol" -c "$root/render2d/r2d_post.c" -o r2d_post.o
    run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -I"$root/render2d" -I"$sokol" -c "$root/render2d/r2d_post_shaders.c" -o r2d_post_shaders.o
    run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -I"$root/render2d" -I"$sokol" -c "$root/render2d/r2d_shadow.c" -o r2d_shadow.o
    run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -I"$root/render2d" -I"$sokol" -c "$root/render2d/r2d_lighting.c" -o r2d_lighting.o
    run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -I"$root/render2d" -I"$sokol" -c "$root/render2d/r2d_quality.c" -o r2d_quality.o
    rm -f libclockwork_render2d.a
    run ar rcs libclockwork_render2d.a render2d.o r2d_particles.o r2d_effects.o r2d_tunables.o r2d_post_params.o r2d_post.o r2d_post_shaders.o r2d_shadow.o r2d_lighting.o r2d_quality.o
    run cp "$root/tunable.cfg" tunable.cfg
    run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -I"$root/render2d" -I"$sokol" "$root/examples/combat.c" -o clockwork_combat \
        libclockwork.a libclockwork_render2d.a libclockwork_sokol.a "${ldflags[@]}" "${gui_libraries[@]}"
    run ./clockwork_combat --headless combat_test.cwsn | tee combat.log
    grep -q 'OK: combat demo verified' combat.log || { echo "combat demo produced unexpected output" >&2; exit 1; }
fi

run "$compiler" "${cflags[@]}" "$root/examples/visualizer.c" -o clockwork_visualizer libclockwork.a "${ldflags[@]}"
run ./clockwork_visualizer --frames 300 | tail -n 8

echo "Built and tested $out"
echo "Run $out/clockwork_visualizer in a terminal for the interactive view."
[ "$gui" = 1 ] && echo "Run $out/clockwork_save_load for the interactive save/load tool."
[ "$gui" = 1 ] && echo "Run $out/clockwork_combat for the combat demo (render2d)."
exit 0
