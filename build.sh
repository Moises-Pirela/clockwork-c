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
engine_sources=(clockwork cw_sim cw_commands cw_bytes cw_snapshot cw_file cw_reflect cw_scene cw_game cw_assets cw_anim cw_ui)
objects=()
for unit in "${engine_sources[@]}"; do
    run "$compiler" "${cflags[@]}" -fPIC -c "$root/src/$unit.c" -o "$unit.o"
    objects+=("$unit.o")
done
rm -f libclockwork.a
run ar rcs libclockwork.a "${objects[@]}"
ldflags+=(-ldl)

if [ -f "$root/tests/test_clockwork.c" ]; then
    run "$compiler" "${cflags[@]}" "$root/tests/test_clockwork.c" -o clockwork_tests libclockwork.a "${ldflags[@]}"
    run ./clockwork_tests
fi

# The combat game as a module (what the editor and the player load), at the
# path its project names, then the engine-level checks through it.
combat_project=$root/games/combat/combat.cwproj
combat_module=$root/games/combat/build/combat_game.so
[ "$(uname -s)" = Darwin ] && combat_module=$root/games/combat/build/combat_game.dylib
mkdir -p "$(dirname "$combat_module")"
run "$compiler" "${cflags[@]}" -shared -fPIC "$root/games/combat/combat_game.c" "${objects[@]}" -o "$combat_module" "${ldflags[@]}"
run "$compiler" "${cflags[@]}" "$root/tests/test_game.c" "$root/games/combat/combat_game.c" -o clockwork_game_tests libclockwork.a "${ldflags[@]}"
run ./clockwork_game_tests "$combat_module" | tee game_tests.log
grep -q 'OK: [0-9]* game module checks passed' game_tests.log || { echo "game module tests produced unexpected output" >&2; exit 1; }
orbit_project=$root/games/orbit/orbit.cwproj
orbit_module=$root/games/orbit/build/orbit_game.so
[ "$(uname -s)" = Darwin ] && orbit_module=$root/games/orbit/build/orbit_game.dylib
mkdir -p "$(dirname "$orbit_module")"
run "$compiler" "${cflags[@]}" -shared -fPIC "$root/games/orbit/orbit_game.c" "${objects[@]}" -o "$orbit_module" "${ldflags[@]}"
run "$compiler" "${cflags[@]}" -I"$root/editor" -I"$root/render2d" "$root/tests/test_editor.c" "$root/editor/editor_core.c" "$root/editor/editor_settings.c" "$root/editor/editor_anim.c" "$root/render2d/r2d_tunables.c" -o clockwork_editor_tests libclockwork.a "${ldflags[@]}"
CC=$compiler run ./clockwork_editor_tests "$combat_project" "$root" "$orbit_project" | tee editor_tests.log
grep -q 'OK: [0-9]* editor checks passed' editor_tests.log || { echo "editor tests produced unexpected output" >&2; exit 1; }

run "$compiler" "${cflags[@]}" -fPIC -shared "$root/launcher/template/game.c" "${objects[@]}" -o starter_template.so "${ldflags[@]}"
run "$compiler" "${cflags[@]}" -I"$root/launcher" "$root/tests/test_launcher.c" "$root/launcher/launcher_core.c" -o clockwork_launcher_tests libclockwork.a "${ldflags[@]}"
CC=$compiler run ./clockwork_launcher_tests "$root" "$out" | tee launcher_tests.log
grep -q 'OK: [0-9]* launcher checks passed' launcher_tests.log || { echo "launcher tests produced unexpected output" >&2; exit 1; }

sokol=$(bash "$root/tools/prepare-sokol.sh")
run "$compiler" "${cflags[@]}" -I"$root/render2d" -I"$sokol" "$root/tests/test_render2d.c" -o clockwork_render2d_tests "${ldflags[@]}"
run ./clockwork_render2d_tests
run "$compiler" "${cflags[@]}" -I"$root/render2d" "$root/tests/test_image.c" "$root/render2d/r2d_image.c" "$root/render2d/r2d_material.c" -o clockwork_image_tests "${ldflags[@]}"
if command -v python3 >/dev/null 2>&1; then python3 "$root/tools/gen-test-images.py" test_images && run ./clockwork_image_tests test_images; else run ./clockwork_image_tests; fi
run "$compiler" "${cflags[@]}" -I"$root/runtime" -I"$root/render2d" -I"$sokol" "$root/tests/test_dock.c" "$root/runtime/host_dock.c" -o clockwork_dock_tests "${ldflags[@]}"
run ./clockwork_dock_tests
run "$compiler" "${cflags[@]}" -I"$root/render2d" -I"$sokol" "$root/tests/test_r2d_fx.c" "$root/render2d/r2d_particles.c" "$root/render2d/r2d_effects.c" -o clockwork_r2d_fx_tests "${ldflags[@]}"
run ./clockwork_r2d_fx_tests
run "$compiler" "${cflags[@]}" -I"$root/render2d" -I"$sokol" "$root/tests/test_r2d_post.c" "$root/render2d/r2d_post_params.c" "$root/render2d/r2d_tunables.c" "$root/render2d/r2d_shadow.c" "$root/render2d/r2d_quality.c" -o clockwork_r2d_post_tests "${ldflags[@]}"
run ./clockwork_r2d_post_tests

if [ -f "$root/examples/movement.c" ]; then
    run "$compiler" "${cflags[@]}" "$root/examples/movement.c" -o clockwork_movement libclockwork.a "${ldflags[@]}"
    run ./clockwork_movement | tee movement.log
    grep -q 'After 30 ticks: (3.00, 1.00)' movement.log || { echo "movement example produced unexpected output" >&2; exit 1; }
fi

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
    if [ -f "$root/examples/save_load.c" ]; then
        run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -I"$sokol" "$root/examples/save_load.c" -o clockwork_save_load \
            libclockwork.a libclockwork_sokol.a "${ldflags[@]}" "${gui_libraries[@]}"
        run ./clockwork_save_load --headless | tee save_load.log
        grep -q 'OK: save/load verified' save_load.log || { echo "save/load tool produced unexpected output" >&2; exit 1; }
    fi

    r2d_objects=()
    for unit in render2d r2d_particles r2d_effects r2d_tunables r2d_post_params r2d_post r2d_post_shaders r2d_shadow r2d_lighting r2d_quality r2d_image r2d_material r2d_sprite r2d_sprite_shaders; do
        run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -I"$root/render2d" -I"$sokol" -c "$root/render2d/$unit.c" -o "$unit.o"
        r2d_objects+=("$unit.o")
    done
    rm -f libclockwork_render2d.a
    run ar rcs libclockwork_render2d.a "${r2d_objects[@]}"
    run cp "$root/tunable.cfg" tunable.cfg
    run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -I"$root/render2d" -I"$root/runtime" -I"$sokol" -c "$root/runtime/host_canvas.c" -o host_canvas.o
    run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -I"$root/render2d" -I"$root/runtime" -I"$sokol" -c "$root/runtime/host_ui.c" -o host_ui.o
    run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -I"$root/render2d" -I"$root/runtime" -I"$sokol" -c "$root/runtime/host_dock.c" -o host_dock.o
    # the player: runs any module on a scene (exported games are this with the module linked in)
    run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -I"$root/render2d" -I"$root/runtime" -I"$sokol" "$root/runtime/player.c" host_canvas.o -o clockwork_player \
        libclockwork.a libclockwork_render2d.a libclockwork_sokol.a "${ldflags[@]}" "${gui_libraries[@]}"
    run ./clockwork_player --project "$combat_project" --headless 120 | tee player.log
    grep -q 'OK: combat ran 120 ticks headless' player.log || { echo "player produced unexpected output" >&2; exit 1; }
    run ./clockwork_player --project "$orbit_project" --headless 300 | tee player_orbit.log
    grep -q 'OK: orbit ran 300 ticks headless' player_orbit.log || { echo "player produced unexpected output for orbit" >&2; exit 1; }
    # the editor: opens projects, edits scenes, builds games
    run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -DCW_ENGINE_ROOT="\"$root\"" -I"$root/editor" -I"$root/render2d" -I"$root/runtime" -I"$sokol" \
        "$root/editor/editor_main.c" "$root/editor/editor_core.c" "$root/editor/editor_settings.c" "$root/editor/editor_anim.c" host_canvas.o host_ui.o host_dock.o -o clockwork_editor \
        libclockwork.a libclockwork_render2d.a libclockwork_sokol.a "${ldflags[@]}" "${gui_libraries[@]}"
    # the launcher: lists, creates, builds and opens projects
    run "$compiler" "${cflags[@]}" "${gui_flags[@]}" -DCW_ENGINE_ROOT="\"$root\"" -I"$root/launcher" -I"$root/render2d" -I"$root/runtime" -I"$sokol" \
        "$root/launcher/launcher_main.c" "$root/launcher/launcher_core.c" host_ui.o host_dock.o -o clockwork_launcher \
        libclockwork.a libclockwork_render2d.a libclockwork_sokol.a "${ldflags[@]}" "${gui_libraries[@]}"
fi

if [ -f "$root/examples/visualizer.c" ]; then
    run "$compiler" "${cflags[@]}" "$root/examples/visualizer.c" -o clockwork_visualizer libclockwork.a "${ldflags[@]}"
    run ./clockwork_visualizer --frames 300 | tail -n 8
fi

echo "Built and tested $out"
[ "$gui" = 1 ] && echo "Run $out/clockwork_launcher to create and open projects, or $out/clockwork_editor --project games/combat/combat.cwproj for the editor."
[ "$gui" = 1 ] && echo "Run $out/clockwork_player --project games/combat/combat.cwproj for the combat game (or games/orbit/orbit.cwproj)."
exit 0
