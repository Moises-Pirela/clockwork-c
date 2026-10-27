# Vendored raylib

Subset of [raylib](https://www.raylib.com) (`RAYLIB_VERSION` in `src/raylib.h`,
zlib license, see `LICENSE`) plus `raygui.h` (zlib, same author), used only by
the interactive save/load tool in `examples/save_load.c`. The library itself
has no dependency on it.

Taken from the `raylib-sys` 5.5.1 crate source tree and trimmed to what a 2D
desktop build needs:

- `src/`: rcore, rshapes, rtextures, rtext, utils, rglfw and the desktop GLFW
  platform layer with their single-header dependencies in `src/external`.
  The audio and models modules and their dependencies are not included.
- `src/external/glfw`: GLFW `include` and `src` (zlib, see `LICENSE.md`);
  build files, docs and the Wayland protocol deps are not included, so
  Linux builds use the X11 backend (`-D_GLFW_X11`).

Build flags: `-DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_33`, plus `-D_GLFW_X11`
on Linux. Link with `m pthread dl rt X11` on Linux, `winmm gdi32 user32
shell32 opengl32` on Windows, and the Cocoa/IOKit/CoreVideo/OpenGL
frameworks on macOS. Linux needs the X11 development headers
(`libx11-dev libxrandr-dev libxcursor-dev libxinerama-dev libxi-dev
libxext-dev` on Debian/Ubuntu).

The only local modification is `src/raygui.h` line 324, whose raylib include
path was restored to the upstream `#include "raylib.h"`. `src/raygui_impl.c`
is ours: it compiles the raygui implementation as one translation unit.
