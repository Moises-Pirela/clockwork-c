# Bundled OpenGL headers (Linux)

The Linux GUI build compiles Sokol's OpenGL backend against these headers
instead of the system `<GL/gl.h>`, so no OpenGL development package
(`libgl1-mesa-dev`, `libgl-dev`, `mesa-libGL-devel`, ...) is required. Only
the runtime library `libGL.so.1`, which ships with every OpenGL driver or
Mesa installation, is needed, and the build links it by file name.

- `GL/glcorearb.h` and `KHR/khrplatform.h` are the unmodified Khronos
  registry headers (MIT license, copyright The Khronos Group Inc.; see the
  notices in the files). Upstream: https://github.com/KhronosGroup/OpenGL-Registry
  and https://github.com/KhronosGroup/EGL-Registry.
- `GL/gl.h` is a small shim that includes `glcorearb.h` with prototypes
  enabled, because `sokol_app.h` and `sokol_gfx.h` include `<GL/gl.h>`.

Windows (Direct3D 11) and macOS (Metal) do not use these files.
