/* Stand-in for the system <GL/gl.h> so the Linux build does not need the
 * OpenGL development package (libgl1-mesa-dev / libgl-dev). Sokol's GL core
 * backend only uses core-profile OpenGL, which the official Khronos
 * glcorearb.h (bundled unchanged next to this file) declares completely:
 * types, enums, and, with GL_GLEXT_PROTOTYPES, the function prototypes.
 * The functions themselves are resolved at link time from the runtime
 * library libGL.so.1, which every OpenGL driver installation provides. */
#ifndef CW_BUNDLED_GL_H
#define CW_BUNDLED_GL_H
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include "glcorearb.h"
#endif
