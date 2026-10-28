#!/usr/bin/env sh
# Builds a game project against the engine. Called by the editor (F7 / F8)
# and usable by hand:
#
#   tools/build-game.sh <engine_root> <project.cwproj> module <output.so>  <source.c>...
#   tools/build-game.sh <engine_root> <project.cwproj> export <output_exe> <source.c>...
#
#   module  compiles the game sources with the ECS into a shared library the
#           editor and the player load at run time.
#   export  compiles a standalone game: the player with the module linked in
#           statically, plus a copy of the project file and its scenes next
#           to the executable, with its assets. The result runs without the editor or the
#           engine tree.
#
# CC selects the compiler (default cc); CW_CFLAGS adds flags.
set -eu
if [ $# -lt 5 ]; then
    echo "usage: $0 <engine_root> <project.cwproj> module|export <output> <source>..." >&2
    exit 2
fi
root=$1; project=$2; mode=$3; out=$4; shift 4
cc=${CC:-cc}
project_dir=$(cd "$(dirname "$project")" && pwd)
mkdir -p "$(dirname "$out")"
cflags="-std=c11 -O2 -Wall -Wextra -I$root/include ${CW_CFLAGS:-}"
engine_sources="$root/src/clockwork.c $root/src/cw_sim.c $root/src/cw_commands.c $root/src/cw_bytes.c $root/src/cw_snapshot.c $root/src/cw_file.c $root/src/cw_reflect.c $root/src/cw_scene.c $root/src/cw_game.c $root/src/cw_assets.c $root/src/cw_anim.c $root/src/cw_ui.c"
run() { echo "+ $*"; "$@"; }

case $mode in
module)
    # shellcheck disable=SC2086
    run $cc $cflags -shared -fPIC $engine_sources "$@" -o "$out" -lm
    echo "module: $out"
    ;;
export)
    sokol=$root/third_party/sokol
    render2d="$root/render2d/render2d.c $root/render2d/r2d_particles.c $root/render2d/r2d_effects.c $root/render2d/r2d_tunables.c $root/render2d/r2d_post_params.c $root/render2d/r2d_post.c $root/render2d/r2d_post_shaders.c $root/render2d/r2d_shadow.c $root/render2d/r2d_lighting.c $root/render2d/r2d_quality.c $root/render2d/r2d_image.c $root/render2d/r2d_material.c $root/render2d/r2d_sprite.c $root/render2d/r2d_sprite_shaders.c"
    objdir=$(dirname "$out")/obj
    mkdir -p "$objdir"
    if [ "$(uname -s)" = Darwin ]; then
        gui_flags="-DSOKOL_METAL"
        run $cc -std=c11 -w -O2 -DSOKOL_METAL -x objective-c -I"$sokol" -c "$root/examples/sokol_impl.c" -o "$objdir/sokol_impl.o"
        libs="-framework Cocoa -framework QuartzCore -framework Metal -framework MetalKit"
    else
        gui_flags="-DSOKOL_GLCORE"
        run $cc -std=c11 -w -O2 -DSOKOL_GLCORE -I"$root/third_party/x11" -I"$root/third_party/gl" -I"$sokol" -c "$root/examples/sokol_impl.c" -o "$objdir/sokol_impl.o"
        libs=""
        for lib in X11:libX11.so.6 Xi:libXi.so.6 Xcursor:libXcursor.so.1 GL:libGL.so.1; do
            name=${lib%%:*}; file=${lib#*:}
            if echo 'int main(void){return 0;}' | $cc -x c - -o /dev/null "-l$name" >/dev/null 2>&1; then libs="$libs -l$name"
            elif echo 'int main(void){return 0;}' | $cc -x c - -o /dev/null "-l:$file" >/dev/null 2>&1; then libs="$libs -l:$file"
            else echo "cannot link $name: install the runtime library $file" >&2; exit 1; fi
        done
        libs="$libs -lpthread -ldl"
    fi
    # shellcheck disable=SC2086
    run $cc $cflags $gui_flags -DCW_STATIC_GAME -I"$root/render2d" -I"$root/runtime" -I"$sokol" "$root/runtime/player.c" "$root/runtime/host_canvas.c" $render2d $engine_sources "$@" "$objdir/sokol_impl.o" -o "$out" -lm $libs
    outdir=$(dirname "$out")
    cp "$project" "$outdir/game.cwproj"   # the static player looks for game.cwproj next to itself
    if [ -d "$project_dir/scenes" ]; then rm -rf "$outdir/scenes"; cp -R "$project_dir/scenes" "$outdir/scenes"; fi
    [ -f "$project_dir/render.cfg" ] && cp "$project_dir/render.cfg" "$outdir/render.cfg"
    [ -f "$root/tunable.cfg" ] && cp "$root/tunable.cfg" "$outdir/tunable.cfg"
    # the assets: the manifest and every file it lists (images, materials, shaders, clips, prefabs...), same relative paths
    manifest=$(sed -n 's/^assets[[:space:]]\+\(.*\)$/\1/p' "$project" | head -n 1)
    manifest=${manifest:-assets.cwassets}
    copied=0
    if [ -f "$project_dir/$manifest" ]; then
        mkdir -p "$outdir/$(dirname "$manifest")"
        cp "$project_dir/$manifest" "$outdir/$manifest"
        sed -n 's/^asset [0-9a-fA-F]* [a-z]* "\([^"]*\)".*$/\1/p' "$project_dir/$manifest" | while IFS= read -r rel; do
            case $rel in *.c|*.h) continue ;; esac   # sources are compiled in
            if [ -f "$project_dir/$rel" ]; then
                mkdir -p "$outdir/$(dirname "$rel")"
                cp "$project_dir/$rel" "$outdir/$rel"
            fi
        done
        copied=$(sed -n 's/^asset .*$/x/p' "$project_dir/$manifest" | wc -l | tr -d ' ')
    fi
    echo "exported: $out (with game.cwproj, scenes/ and $copied assets)"
    ;;
*)
    echo "mode must be module or export" >&2
    exit 2
    ;;
esac
