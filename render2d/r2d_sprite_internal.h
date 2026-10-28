/* Renderer-internal glue between render2d.c and r2d_sprite.c. */
#ifndef R2D_SPRITE_INTERNAL_H
#define R2D_SPRITE_INTERNAL_H
#include "r2d_sprite.h"

extern const char *const r2d_sprite_vs_glsl, *const r2d_sprite_vs_hlsl, *const r2d_sprite_vs_msl;
extern const char *const r2d_sprite_fs_glsl_preamble, *const r2d_sprite_fs_body, *const r2d_sprite_fs_glsl_main, *const r2d_sprite_fs_glsl_custom_main;
/* The complete fragment program for a backend (malloc'd): the built-in one,
 * or, with `custom` (OpenGL only), the user's material() wrapped. */
char *r2d_sprite__fragment_source(sg_backend backend, const char *custom);

/* A sokol_gfx logger that also keeps the last message (shader compile errors). */
void r2d_sprite__gfx_logger(const char *tag, uint32_t level, uint32_t item, const char *message, uint32_t line, const char *file, void *user);
bool r2d_sprite__setup(void);
void r2d_sprite__shutdown(void);
/* Draws this frame's sprites into the pass currently open, whose targets
 * have the given formats and sample count; then forgets them. */
void r2d_sprite__flush(sg_pixel_format color, sg_pixel_format depth, int sample_count,
                       const r2d_camera *camera, r2d_vec2 viewport, float time);
void r2d_sprite__discard(void);
/* Draws this frame's HUD sprites (before the HUD's shapes and text). */
void r2d_sprite__flush_hud(sg_pixel_format color, sg_pixel_format depth, int sample_count, r2d_vec2 viewport, float time);
/* Draws this frame's screen-space sprites into the window pass. */
void r2d_sprite__flush_screen(sg_pixel_format color, sg_pixel_format depth, int sample_count, r2d_vec2 viewport, float time);
#endif
