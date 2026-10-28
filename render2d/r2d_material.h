/* r2d_material: what a sprite is drawn with.
 *
 * A material names a texture (an image asset, by name), an effect (one of
 * the built-in sprite shader effects, or a custom GLSL fragment shader
 * asset), a blend mode, filtering and wrapping, a tint, uv tiling/offset
 * and four vec4 parameters whose meaning the effect defines (the editor
 * shows them with the labels from r2d_material_params). Materials are text
 * assets (`materials/<name>.cwmat`):
 *
 *   cwmaterial 1
 *   name hero
 *   texture hero.png              an image asset name ("" for plain white)
 *   effect flash                  sprite | flash | outline | dissolve | hue | wave | pixelate | glow |
 *                                 rounded | gradient | sweep | ring | pulse | scanlines | custom
 *   shader rim.cwshader           custom only: a shader asset
 *   blend alpha                   alpha | additive | multiply | opaque
 *   filter linear                 linear | nearest
 *   wrap clamp                    clamp | repeat
 *   tint 1 1 1 1
 *   tiling 1 1
 *   offset 0 0
 *   p0 1 1 1 0                    the effect's parameters
 *   p1 0 0 0 0
 *   ...
 *
 * This module is data only (no renderer dependency); r2d_sprite.h draws
 * with it. A custom shader (`*.cwshader`) is a GLSL fragment function
 *   vec4 material(vec4 base, vec2 uv, vec4 p0, vec4 p1, vec4 p2, vec4 p3, float time)
 * that r2d_sprite wraps into a full program; it compiles on the OpenGL
 * backend, the D3D11 and Metal backends fall back to the sprite effect.
 */
#ifndef R2D_MATERIAL_H
#define R2D_MATERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define R2D_MATERIAL_FORMAT_VERSION 1
#define R2D_MATERIAL_NAME_CAPACITY 96
#define R2D_MATERIAL_PARAMS 4

typedef enum { R2D_BLEND_ALPHA, R2D_BLEND_ADDITIVE, R2D_BLEND_MULTIPLY, R2D_BLEND_OPAQUE, R2D_BLEND_COUNT } r2d_blend;
typedef enum {
    R2D_EFFECT_SPRITE, R2D_EFFECT_FLASH, R2D_EFFECT_OUTLINE, R2D_EFFECT_DISSOLVE, R2D_EFFECT_HUE,
    R2D_EFFECT_WAVE, R2D_EFFECT_PIXELATE, R2D_EFFECT_GLOW,
    /* interface effects: they read the quad's pixel size and the element's state (r2d_sprite_dynamic) */
    R2D_EFFECT_ROUNDED, R2D_EFFECT_GRADIENT, R2D_EFFECT_SWEEP, R2D_EFFECT_RING, R2D_EFFECT_PULSE, R2D_EFFECT_SCANLINES,
    R2D_EFFECT_CUSTOM, R2D_EFFECT_COUNT
} r2d_effect;

typedef struct {
    char name[R2D_MATERIAL_NAME_CAPACITY];
    char texture[R2D_MATERIAL_NAME_CAPACITY];   /* image asset name, "" = white */
    char shader[R2D_MATERIAL_NAME_CAPACITY];    /* custom effect: shader asset name */
    r2d_effect effect;
    r2d_blend blend;
    bool nearest, repeat;
    float tint[4];
    float tiling[2], offset[2];
    float params[R2D_MATERIAL_PARAMS][4];
} r2d_material;

/* One editable parameter of an effect: a component of params[index]. */
typedef struct {
    r2d_effect effect;
    int index, component;
    const char *label;
    float min, max, def, step;
    bool color;                /* the first three components of params[index] are a colour */
} r2d_material_param;
const r2d_material_param *r2d_material_params(r2d_effect effect, size_t *count);
const char *r2d_effect_name(r2d_effect effect);
bool r2d_effect_parse(const char *text, r2d_effect *out);
const char *r2d_effect_help(r2d_effect effect);
const char *r2d_blend_name(r2d_blend blend);
bool r2d_blend_parse(const char *text, r2d_blend *out);

void r2d_material_defaults(r2d_material *m, const char *name);
void r2d_material_set_effect(r2d_material *m, r2d_effect effect);   /* resets the parameters to the effect's defaults */
bool r2d_material_equal(const r2d_material *a, const r2d_material *b);

size_t r2d_material_format(const r2d_material *m, char *out, size_t capacity);
bool r2d_material_parse(r2d_material *m, const char *text, size_t size, char *error, size_t error_capacity);
bool r2d_material_save(const r2d_material *m, const char *path, char *error, size_t error_capacity);
bool r2d_material_load(r2d_material *m, const char *path, char *error, size_t error_capacity);

#ifdef __cplusplus
}
#endif
#endif
