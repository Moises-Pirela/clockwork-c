#define _CRT_SECURE_NO_WARNINGS 1
#include "r2d_material.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const effect_names[R2D_EFFECT_COUNT] = {"sprite", "flash", "outline", "dissolve", "hue", "wave", "pixelate", "glow",
                                                           "rounded", "gradient", "sweep", "ring", "pulse", "scanlines", "custom"};
static const char *const effect_help[R2D_EFFECT_COUNT] = {
    "textured quad times tint",
    "blend the colour towards a flash colour (hit feedback)",
    "a coloured outline around the opaque part, `thickness` texels wide",
    "burn away by a noise threshold with a glowing edge",
    "rotate the hue, scale saturation and brightness",
    "wobble the texture coordinates over time",
    "snap the texture to a coarse grid",
    "add a coloured glow on top",
    "rounded corners and a border; brightens on hover, sinks when pressed",
    "a two-colour gradient along an angle, optionally scrolling or mirrored",
    "a band of light sweeping across, brighter while hovered",
    "a radial progress arc from the element's fill; the rest in the track colour",
    "a glowing border that breathes and flares on hover",
    "CRT scanlines that scroll and flicker; a tint on hover",
    "a custom GLSL fragment shader asset (OpenGL backend)",
};
static const char *const blend_names[R2D_BLEND_COUNT] = {"alpha", "additive", "multiply", "opaque"};
static const r2d_material_param params[] = {
    {R2D_EFFECT_FLASH, 0, 0, "flash r", 0, 1, 1, 0.05f, true}, {R2D_EFFECT_FLASH, 0, 1, "flash g", 0, 1, 1, 0.05f, true},
    {R2D_EFFECT_FLASH, 0, 2, "flash b", 0, 1, 1, 0.05f, true}, {R2D_EFFECT_FLASH, 0, 3, "amount", 0, 1, 0.5f, 0.05f, false},
    {R2D_EFFECT_OUTLINE, 0, 0, "outline r", 0, 1, 1, 0.05f, true}, {R2D_EFFECT_OUTLINE, 0, 1, "outline g", 0, 1, 1, 0.05f, true},
    {R2D_EFFECT_OUTLINE, 0, 2, "outline b", 0, 1, 1, 0.05f, true}, {R2D_EFFECT_OUTLINE, 0, 3, "outline a", 0, 1, 1, 0.05f, false},
    {R2D_EFFECT_OUTLINE, 1, 0, "thickness", 0.5f, 8, 1, 0.5f, false},
    {R2D_EFFECT_DISSOLVE, 0, 0, "threshold", 0, 1, 0.3f, 0.02f, false}, {R2D_EFFECT_DISSOLVE, 0, 1, "noise scale", 1, 64, 8, 1, false},
    {R2D_EFFECT_DISSOLVE, 0, 2, "edge width", 0, 0.5f, 0.08f, 0.01f, false},
    {R2D_EFFECT_DISSOLVE, 1, 0, "edge r", 0, 4, 1, 0.1f, true}, {R2D_EFFECT_DISSOLVE, 1, 1, "edge g", 0, 4, 0.6f, 0.1f, true}, {R2D_EFFECT_DISSOLVE, 1, 2, "edge b", 0, 4, 0.2f, 0.1f, true},
    {R2D_EFFECT_HUE, 0, 0, "hue shift", -180, 180, 0, 5, false}, {R2D_EFFECT_HUE, 0, 1, "saturation", 0, 4, 1, 0.1f, false}, {R2D_EFFECT_HUE, 0, 2, "brightness", 0, 4, 1, 0.1f, false},
    {R2D_EFFECT_WAVE, 0, 0, "frequency", 0, 40, 8, 0.5f, false}, {R2D_EFFECT_WAVE, 0, 1, "speed", -20, 20, 3, 0.5f, false},
    {R2D_EFFECT_WAVE, 0, 2, "amplitude", 0, 0.5f, 0.03f, 0.005f, false}, {R2D_EFFECT_WAVE, 0, 3, "vertical", 0, 1, 0, 1, false},
    {R2D_EFFECT_PIXELATE, 0, 0, "pixels", 2, 256, 16, 1, false},
    {R2D_EFFECT_GLOW, 0, 0, "glow r", 0, 4, 1, 0.1f, true}, {R2D_EFFECT_GLOW, 0, 1, "glow g", 0, 4, 0.8f, 0.1f, true},
    {R2D_EFFECT_GLOW, 0, 2, "glow b", 0, 4, 0.3f, 0.1f, true}, {R2D_EFFECT_GLOW, 0, 3, "strength", 0, 4, 0.5f, 0.1f, false},
    {R2D_EFFECT_ROUNDED, 0, 0, "radius", 0, 128, 8, 1, false}, {R2D_EFFECT_ROUNDED, 0, 1, "border", 0, 32, 2, 0.5f, false},
    {R2D_EFFECT_ROUNDED, 0, 2, "hover glow", 0, 1, 0.15f, 0.05f, false}, {R2D_EFFECT_ROUNDED, 0, 3, "softness", 0.5f, 4, 1, 0.25f, false},
    {R2D_EFFECT_ROUNDED, 1, 0, "border r", 0, 1, 0.9f, 0.05f, true}, {R2D_EFFECT_ROUNDED, 1, 1, "border g", 0, 1, 0.9f, 0.05f, true},
    {R2D_EFFECT_ROUNDED, 1, 2, "border b", 0, 1, 1, 0.05f, true}, {R2D_EFFECT_ROUNDED, 1, 3, "border a", 0, 1, 0.6f, 0.05f, false},
    {R2D_EFFECT_GRADIENT, 0, 0, "top r", 0, 2, 1, 0.05f, true}, {R2D_EFFECT_GRADIENT, 0, 1, "top g", 0, 2, 1, 0.05f, true}, {R2D_EFFECT_GRADIENT, 0, 2, "top b", 0, 2, 1, 0.05f, true},
    {R2D_EFFECT_GRADIENT, 1, 0, "bottom r", 0, 2, 0.5f, 0.05f, true}, {R2D_EFFECT_GRADIENT, 1, 1, "bottom g", 0, 2, 0.5f, 0.05f, true}, {R2D_EFFECT_GRADIENT, 1, 2, "bottom b", 0, 2, 0.6f, 0.05f, true},
    {R2D_EFFECT_GRADIENT, 2, 0, "angle", -180, 180, 90, 5, false}, {R2D_EFFECT_GRADIENT, 2, 1, "scroll", -5, 5, 0, 0.1f, false}, {R2D_EFFECT_GRADIENT, 2, 2, "mirror", 0, 1, 0, 1, false},
    {R2D_EFFECT_SWEEP, 0, 0, "speed", 0, 5, 0.5f, 0.05f, false}, {R2D_EFFECT_SWEEP, 0, 1, "width", 0.01f, 1, 0.12f, 0.01f, false},
    {R2D_EFFECT_SWEEP, 0, 2, "intensity", 0, 2, 0.4f, 0.05f, false}, {R2D_EFFECT_SWEEP, 0, 3, "angle", -180, 180, 30, 5, false},
    {R2D_EFFECT_SWEEP, 1, 0, "light r", 0, 2, 1, 0.05f, true}, {R2D_EFFECT_SWEEP, 1, 1, "light g", 0, 2, 0.95f, 0.05f, true},
    {R2D_EFFECT_SWEEP, 1, 2, "light b", 0, 2, 0.8f, 0.05f, true}, {R2D_EFFECT_SWEEP, 1, 3, "hover boost", 0, 4, 1, 0.1f, false},
    {R2D_EFFECT_RING, 0, 0, "thickness", 1, 64, 6, 1, false}, {R2D_EFFECT_RING, 0, 1, "start angle", -180, 180, 0, 5, false},
    {R2D_EFFECT_RING, 0, 2, "softness", 0.5f, 4, 1, 0.25f, false},
    {R2D_EFFECT_RING, 1, 0, "track r", 0, 1, 0.15f, 0.05f, true}, {R2D_EFFECT_RING, 1, 1, "track g", 0, 1, 0.16f, 0.05f, true},
    {R2D_EFFECT_RING, 1, 2, "track b", 0, 1, 0.2f, 0.05f, true}, {R2D_EFFECT_RING, 1, 3, "track a", 0, 1, 0.8f, 0.05f, false},
    {R2D_EFFECT_PULSE, 0, 0, "glow r", 0, 2, 0.4f, 0.05f, true}, {R2D_EFFECT_PULSE, 0, 1, "glow g", 0, 2, 0.8f, 0.05f, true},
    {R2D_EFFECT_PULSE, 0, 2, "glow b", 0, 2, 1, 0.05f, true}, {R2D_EFFECT_PULSE, 0, 3, "rate", 0, 5, 0.5f, 0.1f, false},
    {R2D_EFFECT_PULSE, 1, 0, "radius", 0, 128, 6, 1, false}, {R2D_EFFECT_PULSE, 1, 1, "width", 0.5f, 32, 4, 0.5f, false},
    {R2D_EFFECT_PULSE, 1, 2, "hover flare", 0, 4, 1, 0.1f, false}, {R2D_EFFECT_PULSE, 1, 3, "strength", 0, 4, 0.8f, 0.1f, false},
    {R2D_EFFECT_SCANLINES, 0, 0, "density", 0.05f, 4, 1, 0.05f, false}, {R2D_EFFECT_SCANLINES, 0, 1, "strength", 0, 1, 0.35f, 0.05f, false},
    {R2D_EFFECT_SCANLINES, 0, 2, "scroll", -200, 200, 20, 5, false}, {R2D_EFFECT_SCANLINES, 0, 3, "flicker", 0, 1, 0.2f, 0.05f, false},
    {R2D_EFFECT_SCANLINES, 1, 0, "hover r", 0, 1, 0.2f, 0.05f, true}, {R2D_EFFECT_SCANLINES, 1, 1, "hover g", 0, 1, 0.5f, 0.05f, true},
    {R2D_EFFECT_SCANLINES, 1, 2, "hover b", 0, 1, 0.6f, 0.05f, true}, {R2D_EFFECT_SCANLINES, 1, 3, "hover tint", 0, 2, 0.5f, 0.05f, false},
    {R2D_EFFECT_CUSTOM, 0, 0, "p0.x", -100, 100, 0, 0.1f, false}, {R2D_EFFECT_CUSTOM, 0, 1, "p0.y", -100, 100, 0, 0.1f, false},
    {R2D_EFFECT_CUSTOM, 0, 2, "p0.z", -100, 100, 0, 0.1f, false}, {R2D_EFFECT_CUSTOM, 0, 3, "p0.w", -100, 100, 0, 0.1f, false},
    {R2D_EFFECT_CUSTOM, 1, 0, "p1.x", -100, 100, 0, 0.1f, false}, {R2D_EFFECT_CUSTOM, 1, 1, "p1.y", -100, 100, 0, 0.1f, false},
    {R2D_EFFECT_CUSTOM, 1, 2, "p1.z", -100, 100, 0, 0.1f, false}, {R2D_EFFECT_CUSTOM, 1, 3, "p1.w", -100, 100, 0, 0.1f, false},
    {R2D_EFFECT_CUSTOM, 2, 0, "p2.x", -100, 100, 0, 0.1f, false}, {R2D_EFFECT_CUSTOM, 2, 1, "p2.y", -100, 100, 0, 0.1f, false},
    {R2D_EFFECT_CUSTOM, 2, 2, "p2.z", -100, 100, 0, 0.1f, false}, {R2D_EFFECT_CUSTOM, 2, 3, "p2.w", -100, 100, 0, 0.1f, false},
    {R2D_EFFECT_CUSTOM, 3, 0, "p3.x", -100, 100, 0, 0.1f, false}, {R2D_EFFECT_CUSTOM, 3, 1, "p3.y", -100, 100, 0, 0.1f, false},
    {R2D_EFFECT_CUSTOM, 3, 2, "p3.z", -100, 100, 0, 0.1f, false}, {R2D_EFFECT_CUSTOM, 3, 3, "p3.w", -100, 100, 0, 0.1f, false},
};
const r2d_material_param *r2d_material_params(r2d_effect effect, size_t *count) {
    size_t first = 0, n = 0, total = sizeof(params) / sizeof(params[0]);
    while (first < total && params[first].effect != effect) ++first;
    while (first + n < total && params[first + n].effect == effect) ++n;
    if (count) *count = n;
    return n ? &params[first] : NULL;
}
const char *r2d_effect_name(r2d_effect e) { return e >= 0 && e < R2D_EFFECT_COUNT ? effect_names[e] : "?"; }
const char *r2d_effect_help(r2d_effect e) { return e >= 0 && e < R2D_EFFECT_COUNT ? effect_help[e] : ""; }
bool r2d_effect_parse(const char *text, r2d_effect *out) {
    for (int i = 0; text && i < R2D_EFFECT_COUNT; ++i) if (strcmp(effect_names[i], text) == 0) { *out = (r2d_effect)i; return true; }
    return false;
}
const char *r2d_blend_name(r2d_blend b) { return b >= 0 && b < R2D_BLEND_COUNT ? blend_names[b] : "?"; }
bool r2d_blend_parse(const char *text, r2d_blend *out) {
    for (int i = 0; text && i < R2D_BLEND_COUNT; ++i) if (strcmp(blend_names[i], text) == 0) { *out = (r2d_blend)i; return true; }
    return false;
}
static void copy_name(char *dst, size_t capacity, const char *src) {
    size_t n = strlen(src ? src : "");
    if (n >= capacity) n = capacity - 1;
    memset(dst, 0, capacity);   /* keep the whole field deterministic */
    memcpy(dst, src ? src : "", n);
}
void r2d_material_set_effect(r2d_material *m, r2d_effect effect) {
    m->effect = effect >= 0 && effect < R2D_EFFECT_COUNT ? effect : R2D_EFFECT_SPRITE;
    memset(m->params, 0, sizeof(m->params));
    size_t count;
    const r2d_material_param *p = r2d_material_params(m->effect, &count);
    for (size_t i = 0; i < count; ++i) m->params[p[i].index][p[i].component] = p[i].def;
}
void r2d_material_defaults(r2d_material *m, const char *name) {
    memset(m, 0, sizeof(*m));
    copy_name(m->name, sizeof(m->name), name && *name ? name : "material");
    m->tint[0] = m->tint[1] = m->tint[2] = m->tint[3] = 1;
    m->tiling[0] = m->tiling[1] = 1;
    r2d_material_set_effect(m, R2D_EFFECT_SPRITE);
}
bool r2d_material_equal(const r2d_material *a, const r2d_material *b) {
    if (!a || !b) return false;
    return strcmp(a->name, b->name) == 0 && strcmp(a->texture, b->texture) == 0 && strcmp(a->shader, b->shader) == 0 &&
           a->effect == b->effect && a->blend == b->blend && a->nearest == b->nearest && a->repeat == b->repeat &&
           memcmp(a->tint, b->tint, sizeof(a->tint)) == 0 && memcmp(a->tiling, b->tiling, sizeof(a->tiling)) == 0 &&
           memcmp(a->offset, b->offset, sizeof(a->offset)) == 0 && memcmp(a->params, b->params, sizeof(a->params)) == 0;
}
size_t r2d_material_format(const r2d_material *m, char *out, size_t capacity) {
    int n = snprintf(out, capacity,
        "cwmaterial %d\nname %s\ntexture %s\neffect %s\nshader %s\nblend %s\nfilter %s\nwrap %s\ntint %g %g %g %g\ntiling %g %g\noffset %g %g\n",
        R2D_MATERIAL_FORMAT_VERSION, m->name, m->texture, r2d_effect_name(m->effect), m->shader, r2d_blend_name(m->blend),
        m->nearest ? "nearest" : "linear", m->repeat ? "repeat" : "clamp",
        (double)m->tint[0], (double)m->tint[1], (double)m->tint[2], (double)m->tint[3],
        (double)m->tiling[0], (double)m->tiling[1], (double)m->offset[0], (double)m->offset[1]);
    size_t len = n > 0 ? (size_t)n : 0;
    for (int i = 0; i < R2D_MATERIAL_PARAMS && len < capacity; ++i) {
        n = snprintf(out + len, capacity - len, "p%d %g %g %g %g\n", i, (double)m->params[i][0], (double)m->params[i][1], (double)m->params[i][2], (double)m->params[i][3]);
        if (n > 0) len += (size_t)n;
    }
    return len < capacity ? len : capacity - 1;
}
static bool fail(char *error, size_t capacity, int line, const char *message) {
    if (error && capacity) snprintf(error, capacity, "line %d: %s", line, message);
    return false;
}
static bool floats(const char *text, float *out, int count) {
    const char *p = text;
    for (int i = 0; i < count; ++i) {
        char *end;
        double v = strtod(p, &end);
        if (end == p || !isfinite(v)) return false;
        out[i] = (float)v;
        p = end;
    }
    return true;
}
bool r2d_material_parse(r2d_material *m, const char *text, size_t size, char *error, size_t error_capacity) {
    if (!m || (!text && size)) return fail(error, error_capacity, 0, "no material or text");
    r2d_material_defaults(m, "material");
    bool header = false;
    int line_no = 0;
    const char *p = text, *end = text + size;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        ++line_no;
        char line[320];
        if (len >= sizeof(line)) return fail(error, error_capacity, line_no, "line too long");
        memcpy(line, p, len);
        line[len] = 0;
        p += len + (nl ? 1 : 0);
        char *s = line;
        while (*s && isspace((unsigned char)*s)) ++s;
        char *e = s + strlen(s);
        while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
        if (!*s || *s == '#') continue;
        char *value = s;
        while (*value && !isspace((unsigned char)*value)) ++value;
        if (*value) { *value++ = 0; while (*value && isspace((unsigned char)*value)) ++value; }
        if (!header) {
            if (strcmp(s, "cwmaterial") != 0 || strcmp(value, "1") != 0) return fail(error, error_capacity, line_no, "not a material (expected 'cwmaterial 1')");
            header = true;
        } else if (strcmp(s, "name") == 0) copy_name(m->name, sizeof(m->name), value);
        else if (strcmp(s, "texture") == 0) copy_name(m->texture, sizeof(m->texture), value);
        else if (strcmp(s, "shader") == 0) copy_name(m->shader, sizeof(m->shader), value);
        else if (strcmp(s, "effect") == 0) { r2d_effect ef; if (!r2d_effect_parse(value, &ef)) return fail(error, error_capacity, line_no, "unknown effect"); m->effect = ef; }
        else if (strcmp(s, "blend") == 0) { if (!r2d_blend_parse(value, &m->blend)) return fail(error, error_capacity, line_no, "unknown blend mode"); }
        else if (strcmp(s, "filter") == 0) m->nearest = strcmp(value, "nearest") == 0;
        else if (strcmp(s, "wrap") == 0) m->repeat = strcmp(value, "repeat") == 0;
        else if (strcmp(s, "tint") == 0) { if (!floats(value, m->tint, 4)) return fail(error, error_capacity, line_no, "tint needs four numbers"); }
        else if (strcmp(s, "tiling") == 0) { if (!floats(value, m->tiling, 2)) return fail(error, error_capacity, line_no, "tiling needs two numbers"); }
        else if (strcmp(s, "offset") == 0) { if (!floats(value, m->offset, 2)) return fail(error, error_capacity, line_no, "offset needs two numbers"); }
        else if (s[0] == 'p' && s[1] >= '0' && s[1] < '0' + R2D_MATERIAL_PARAMS && !s[2]) { if (!floats(value, m->params[s[1] - '0'], 4)) return fail(error, error_capacity, line_no, "a parameter needs four numbers"); }
        else return fail(error, error_capacity, line_no, "unknown key");
    }
    if (!header) return fail(error, error_capacity, 0, "empty file");
    return true;
}
bool r2d_material_save(const r2d_material *m, const char *path, char *error, size_t error_capacity) {
    char text[2048];
    size_t len = r2d_material_format(m, text, sizeof(text));
    FILE *f = path ? fopen(path, "wb") : NULL;
    if (!f) { if (error && error_capacity) snprintf(error, error_capacity, "cannot write %s", path ? path : "(null)"); return false; }
    bool ok = fwrite(text, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (!ok && error && error_capacity) snprintf(error, error_capacity, "cannot write %s", path);
    return ok;
}
bool r2d_material_load(r2d_material *m, const char *path, char *error, size_t error_capacity) {
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) { if (error && error_capacity) snprintf(error, error_capacity, "cannot read %s", path ? path : "(null)"); return false; }
    char text[4096];
    size_t n = fread(text, 1, sizeof(text), f);
    fclose(f);
    return r2d_material_parse(m, text, n, error, error_capacity);
}
