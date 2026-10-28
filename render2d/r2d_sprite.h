/* r2d_sprite: textured sprites drawn with materials.
 *
 * Sprites are world-space quads (centre, size, rotation) drawn with a
 * material (r2d_material.h): its texture, blend mode, filtering and effect.
 * They are batched per frame, sorted by layer (lower first, stable within
 * a layer) and drawn into the scene beneath the shape layer, so lights,
 * post-processing and picking work on them like on everything else. The
 * built-in effects live in one shader for every backend; a custom effect
 * compiles a GLSL fragment function at run time (r2d_shader) on the OpenGL
 * backend and falls back to the plain sprite effect elsewhere.
 *
 * Textures are created from decoded images (r2d_image.h); the host keeps
 * them and the materials in caches keyed by asset (runtime/host_canvas.c).
 */
#ifndef R2D_SPRITE_H
#define R2D_SPRITE_H

#include "render2d.h"
#include "r2d_image.h"
#include "r2d_material.h"
#include "sokol_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define R2D_MAX_SPRITES 16384

typedef struct {
    sg_image image;
    sg_view view;
    int width, height;
} r2d_texture;
bool r2d_texture_create(r2d_texture *texture, const r2d_image *image);
void r2d_texture_destroy(r2d_texture *texture);
const r2d_texture *r2d_texture_white(void);       /* 1x1 opaque white, owned by the renderer */

/* What a shader knows about the thing it draws besides the material: the
 * element's state (hover and press, smoothed 0..1; the fill amount; seconds
 * since the state last changed) and four numbers of its own (a UiImage's
 * param_x..w, animatable). Plain sprites pass NULL: all zero. */
typedef struct { float state[4]; float elem[4]; } r2d_sprite_dynamic;

/* A custom fragment program: `source` is the body of
 *   vec4 material(vec4 base, vec2 uv, vec4 p0, vec4 p1, vec4 p2, vec4 p3, float time)
 * plus any helper functions it needs. In scope: `tex` (sampler2D), `texel`
 * (vec2), `size` (the quad in pixels), `state` and `elem` (vec4, see
 * r2d_sprite_dynamic), and the helpers hash2, vnoise, hue_shift,
 * rounded_box (a signed distance) and angle01. On backends other than
 * OpenGL `valid` is false and `log` says why; a failed compile leaves the
 * driver's messages in `log`, with the source's own line numbers.
 * Zero-initialise the struct once; compiling again replaces the program. */
typedef struct {
    sg_shader shader;
    bool valid;
    char log[1024];
} r2d_shader;
bool r2d_shader_compile(r2d_shader *shader, const char *source);
void r2d_shader_destroy(r2d_shader *shader);
const char *r2d_shader_preamble(void);            /* what the function is wrapped into (for documentation) */
/* Starter shaders to create from: rim, button, ring, shine, hologram, bar. */
size_t r2d_shader_example_count(void);
const char *r2d_shader_example_name(size_t index);
const char *r2d_shader_example_source(size_t index);
const char *r2d_shader_example(void);             /* the first one */

/* Draw a sprite this frame. `texture` NULL draws white; `custom` is used
 * when the material's effect is custom and the shader is valid. `uv` is
 * the sub-rectangle of the texture in 0..1 (sprite sheets); NULL is the
 * whole texture. Rotation is in radians about the centre. */
void r2d_draw_sprite(const r2d_material *material, const r2d_texture *texture, const r2d_shader *custom,
                     r2d_vec2 center, r2d_vec2 size, float rotation, r2d_color tint, int layer, const r2d_rect *uv);

/* The same in the HUD (R2D_SPACE_HUD: logical pixels, origin top-left, a
 * game's interface below the host's), sorted by layer like world sprites. */
void r2d_draw_sprite_hud(const r2d_material *material, const r2d_texture *texture, const r2d_shader *custom,
                         r2d_vec2 center, r2d_vec2 size, float rotation, r2d_color tint, int layer, const r2d_rect *uv,
                         const r2d_sprite_dynamic *dynamic);
/* The same in screen space (logical pixels, origin top-left), drawn above
 * the screen-space UI: previews. */
void r2d_draw_sprite_screen(const r2d_material *material, const r2d_texture *texture, const r2d_shader *custom,
                            r2d_vec2 center, r2d_vec2 size, float rotation, r2d_color tint, const r2d_rect *uv,
                            const r2d_sprite_dynamic *dynamic);

typedef struct { int sprites, batches, pipelines; } r2d_sprite_stats;
r2d_sprite_stats r2d_sprite_get_stats(void);

#ifdef __cplusplus
}
#endif
#endif
