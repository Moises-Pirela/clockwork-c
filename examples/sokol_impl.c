/* The single translation unit that carries the Sokol implementations used by
 * render2d and the Sokol-based tools. Every other file includes the headers
 * only. The backend is selected by the build: SOKOL_D3D11 on Windows,
 * SOKOL_METAL on macOS (compiled as Objective-C), SOKOL_GLCORE on Linux. */
#if !defined(SOKOL_D3D11) && !defined(SOKOL_METAL) && !defined(SOKOL_GLCORE) && !defined(SOKOL_GLES3)
#if defined(_WIN32)
#define SOKOL_D3D11
#elif defined(__APPLE__)
#define SOKOL_METAL
#else
#define SOKOL_GLCORE
#endif
#endif
#define SOKOL_NO_ENTRY
#define SOKOL_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "util/sokol_gl.h"
#include "util/sokol_debugtext.h"
