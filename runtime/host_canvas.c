#include "host_canvas.h"
#include "render2d.h"
#include "r2d_lighting.h"
#include "r2d_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static r2d_color color(cw_rgba c) { return r2d_rgba(c.r, c.g, c.b, c.a); }
static void c_circle(void *impl, float x, float y, float r, cw_rgba c) { (void)impl; r2d_draw_circle(r2d_v2(x, y), r, color(c)); }
static void c_ring(void *impl, float x, float y, float r, float t, cw_rgba c) { (void)impl; r2d_draw_ring(r2d_v2(x, y), r, t, color(c)); }
static void c_line(void *impl, float x0, float y0, float x1, float y1, float t, cw_rgba c) { (void)impl; r2d_draw_line(r2d_v2(x0, y0), r2d_v2(x1, y1), t, color(c)); }
static void c_rect(void *impl, float x, float y, float w, float h, cw_rgba c) { (void)impl; r2d_draw_rect(r2d_rect_make(x, y, w, h), color(c)); }
static void c_rect_outline(void *impl, float x, float y, float w, float h, float t, cw_rgba c) { (void)impl; r2d_draw_rect_outline(r2d_rect_make(x, y, w, h), t, color(c)); }
static void c_oriented(void *impl, float cx, float cy, float dx, float dy, float w, float h, cw_rgba c) {
    (void)impl;
    r2d_draw_oriented_rect(r2d_v2(cx, cy), r2d_v2(dx, dy), w, h, color(c));
}
static void c_triangle(void *impl, float x0, float y0, float x1, float y1, float x2, float y2, cw_rgba c) {
    (void)impl;
    r2d_draw_triangle(r2d_v2(x0, y0), r2d_v2(x1, y1), r2d_v2(x2, y2), color(c));
}
static void c_bar(void *impl, float x, float y, float w, float h, float f, cw_rgba fill, cw_rgba back) {
    (void)impl;
    r2d_draw_bar(r2d_rect_make(x, y, w, h), f, color(fill), color(back));
}
static void c_text(void *impl, float x, float y, cw_rgba c, const char *text) { (void)impl; r2d_draw_text(r2d_v2(x, y), color(c), text); }
static float c_px(void *impl, float pixels) { (void)impl; return r2d_px(pixels); }
static void c_light(void *impl, const cw_canvas_light *l) {
    host_canvas *hc = impl;
    if (!hc->lights) return;
    r2d_light_desc d;
    memset(&d, 0, sizeof(d));
    d.position = r2d_v2(l->x, l->y);
    d.radius = l->radius;
    d.color = color(l->color);
    d.intensity = l->intensity;
    d.falloff = l->falloff;
    d.volumetric = l->volumetric;
    d.no_shadows = l->no_shadows;
    d.spot = l->spot;
    d.direction = l->direction;
    d.cone = l->cone;
    d.cone_softness = 0.4f;
    r2d_light(&d);
}
static void c_occluder_circle(void *impl, float x, float y, float r) {
    host_canvas *hc = impl;
    if (hc->lights) r2d_light_occluder_circle(r2d_v2(x, y), r);
}
static void c_occluder_rect(void *impl, float x, float y, float w, float h) {
    host_canvas *hc = impl;
    if (hc->lights) r2d_light_occluder_rect(r2d_rect_make(x, y, w, h));
}
static void c_pick(void *impl, cw_entity_id id, float x, float y, float r) {
    host_canvas *hc = impl;
    if (hc->pick_count == hc->pick_capacity) {
        hc->pick_capacity = hc->pick_capacity ? hc->pick_capacity * 2 : 256;
        hc->picks = realloc(hc->picks, hc->pick_capacity * sizeof(*hc->picks));
        if (!hc->picks) { fputs("host_canvas: out of memory\n", stderr); abort(); }
    }
    host_pick p = {id, x, y, r};
    hc->picks[hc->pick_count++] = p;
    if (hc->pickable) r2d_pick_circle((uint64_t)id, r2d_v2(x, y), r);
}

/* --- asset-backed resources --- */
static host_texture *texture_entry(host_canvas *hc, uint32_t id) {
    for (size_t i = 0; i < hc->texture_count; ++i) if (hc->textures[i].id == id) return &hc->textures[i];
    if (hc->texture_count == hc->texture_capacity) {
        hc->texture_capacity = hc->texture_capacity ? hc->texture_capacity * 2 : 16;
        hc->textures = realloc(hc->textures, hc->texture_capacity * sizeof(*hc->textures));
        if (!hc->textures) { fputs("host_canvas: out of memory\n", stderr); abort(); }
    }
    host_texture *t = &hc->textures[hc->texture_count++];
    memset(t, 0, sizeof(*t));
    t->id = id;
    char path[CW_ASSET_PATH_CAPACITY];
    r2d_image img;
    if (!cw_assets_path(hc->assets, id, path, sizeof(path))) { t->missing = true; snprintf(t->error, sizeof(t->error), "unknown image asset"); return t; }
    if (!r2d_image_load(&img, path, t->error, sizeof(t->error)) || !r2d_texture_create(&t->texture, &img)) { t->missing = true; r2d_image_free(&img); return t; }
    r2d_image_free(&img);
    return t;
}
const host_texture *host_canvas_texture(host_canvas *hc, uint32_t id) { return id ? texture_entry(hc, id) : NULL; }
const host_shader *host_canvas_shader(host_canvas *hc, uint32_t id) {
    if (!id) return NULL;
    for (size_t i = 0; i < hc->shader_count; ++i) if (hc->shaders[i].id == id) return &hc->shaders[i];
    if (hc->shader_count == hc->shader_capacity) {
        hc->shader_capacity = hc->shader_capacity ? hc->shader_capacity * 2 : 8;
        hc->shaders = realloc(hc->shaders, hc->shader_capacity * sizeof(*hc->shaders));
        if (!hc->shaders) { fputs("host_canvas: out of memory\n", stderr); abort(); }
    }
    host_shader *sh = &hc->shaders[hc->shader_count++];
    memset(sh, 0, sizeof(*sh));
    sh->id = id;
    char path[CW_ASSET_PATH_CAPACITY];
    cw_buffer text;
    cw_file_error ferr;
    if (!cw_assets_path(hc->assets, id, path, sizeof(path)) || !cw_file_read(path, 1 << 20, &text, &ferr)) {
        sh->missing = true;
        snprintf(sh->shader.log, sizeof(sh->shader.log), "cannot read the shader asset");
        return sh;
    }
    char *source = malloc(text.size + 1);
    if (source) { memcpy(source, text.data, text.size); source[text.size] = 0; r2d_shader_compile(&sh->shader, source); free(source); }
    cw_buffer_free(&text);
    return sh;
}
const host_material *host_canvas_material(host_canvas *hc, uint32_t id) {
    if (!id) return NULL;
    for (size_t i = 0; i < hc->material_count; ++i) if (hc->materials[i].id == id) return &hc->materials[i];
    if (hc->material_count == hc->material_capacity) {
        hc->material_capacity = hc->material_capacity ? hc->material_capacity * 2 : 16;
        hc->materials = realloc(hc->materials, hc->material_capacity * sizeof(*hc->materials));
        if (!hc->materials) { fputs("host_canvas: out of memory\n", stderr); abort(); }
    }
    host_material *m = &hc->materials[hc->material_count++];
    memset(m, 0, sizeof(*m));
    m->id = id;
    char path[CW_ASSET_PATH_CAPACITY];
    if (!cw_assets_path(hc->assets, id, path, sizeof(path)) || !r2d_material_load(&m->material, path, m->error, sizeof(m->error))) { m->missing = true; if (!m->error[0]) snprintf(m->error, sizeof(m->error), "unknown material asset"); return m; }
    m->texture = m->material.texture[0] ? cw_asset_id(m->material.texture) : 0;
    m->shader = m->material.shader[0] ? cw_asset_id(m->material.shader) : 0;
    return m;
}
void host_canvas_set_assets(host_canvas *hc, const cw_assets *assets) { hc->assets = assets; host_canvas_reload(hc); }
static void drop_caches(host_canvas *hc) {
    for (size_t i = 0; i < hc->texture_count; ++i) r2d_texture_destroy(&hc->textures[i].texture);
    for (size_t i = 0; i < hc->shader_count; ++i) r2d_shader_destroy(&hc->shaders[i].shader);
    hc->texture_count = hc->shader_count = hc->material_count = 0;
    hc->reload_pending = false;
}
void host_canvas_reload(host_canvas *hc) { hc->reload_pending = true; }
void host_canvas_draw_material(host_canvas *hc, const r2d_material *m, r2d_vec2 center, r2d_vec2 size, float rotation, int layer) {
    const host_texture *t = m->texture[0] ? host_canvas_texture(hc, cw_asset_id(m->texture)) : NULL;
    const host_shader *sh = m->effect == R2D_EFFECT_CUSTOM && m->shader[0] ? host_canvas_shader(hc, cw_asset_id(m->shader)) : NULL;
    r2d_draw_sprite(m, t && !t->missing ? &t->texture : NULL, sh ? &sh->shader : NULL, center, size, rotation, r2d_rgb(255, 255, 255), layer, NULL);
}
static void c_sprite(void *impl, uint32_t material_id, float x, float y, float w, float h, float rotation, cw_rgba tint, int layer, float u0, float v0, float u1, float v1) {
    host_canvas *hc = impl;
    const host_material *m = host_canvas_material(hc, material_id);
    r2d_rect uv = r2d_rect_make(u0, v0, u1 - u0, v1 - v0);
    if (!m || m->missing) {   /* a visible placeholder: magenta, so missing materials are noticed */
        r2d_material fallback;
        r2d_material_defaults(&fallback, "missing");
        fallback.tint[0] = 1; fallback.tint[1] = 0; fallback.tint[2] = 1; fallback.tint[3] = 0.8f;
        r2d_draw_sprite(&fallback, NULL, NULL, r2d_v2(x, y), r2d_v2(w, h), rotation, color(tint), layer, &uv);
        return;
    }
    const host_texture *t = m->texture ? host_canvas_texture(hc, m->texture) : NULL;
    const host_shader *sh = m->material.effect == R2D_EFFECT_CUSTOM && m->shader ? host_canvas_shader(hc, m->shader) : NULL;
    r2d_draw_sprite(&m->material, t && !t->missing ? &t->texture : NULL, sh ? &sh->shader : NULL, r2d_v2(x, y), r2d_v2(w, h), rotation, color(tint), layer, &uv);
}
/* --- the interface layer --- */
static void hud_quad(const r2d_material *m, const r2d_texture *t, const r2d_shader *sh, float x, float y, float w, float h, r2d_color tint, r2d_rect uv, const r2d_sprite_dynamic *dyn) {
    r2d_draw_sprite_hud(m, t, sh, r2d_v2(x + w * 0.5f, y + h * 0.5f), r2d_v2(w, h), 0, tint, 0, &uv, dyn);
}
static void c_hud_image(void *impl, uint32_t material_id, float x, float y, float w, float h, cw_rgba tint, float border, float scale,
                        float u0, float v0, float u1, float v1, const cw_canvas_dynamic *dynamic) {
    r2d_sprite_dynamic dyn;
    memset(&dyn, 0, sizeof(dyn));
    if (dynamic) { memcpy(dyn.state, dynamic->state, sizeof(dyn.state)); memcpy(dyn.elem, dynamic->params, sizeof(dyn.elem)); }
    host_canvas *hc = impl;
    r2d_material flat;
    const r2d_material *m = NULL;
    const r2d_texture *t = NULL;
    const r2d_shader *sh = NULL;
    const host_material *hm = material_id ? host_canvas_material(hc, material_id) : NULL;
    if (hm && !hm->missing) {
        m = &hm->material;
        const host_texture *ht = hm->texture ? host_canvas_texture(hc, hm->texture) : NULL;
        if (ht && !ht->missing) t = &ht->texture;
        const host_shader *hs = hm->material.effect == R2D_EFFECT_CUSTOM && hm->shader ? host_canvas_shader(hc, hm->shader) : NULL;
        if (hs) sh = &hs->shader;
    } else {
        r2d_material_defaults(&flat, material_id ? "missing" : "flat");
        if (material_id) { flat.tint[0] = 1; flat.tint[1] = 0; flat.tint[2] = 1; }   /* a missing material shows magenta */
        m = &flat;
    }
    r2d_color c = color(tint);
    if (border > 0 && t && t->width > 0 && t->height > 0 && u0 == 0 && v0 == 0 && u1 == 1 && v1 == 1) {
        /* nine slices: corners keep their size (border * scale), edges stretch along, the middle both ways */
        float bu = border / (float)t->width, bv = border / (float)t->height;
        if (bu > 0.5f) bu = 0.5f;
        if (bv > 0.5f) bv = 0.5f;
        float bs = border * scale;
        if (bs * 2 > w) bs = w * 0.5f;
        if (bs * 2 > h) bs = h * 0.5f;
        float xs[4] = {x, x + bs, x + w - bs, x + w}, ys[4] = {y, y + bs, y + h - bs, y + h};
        float us[4] = {0, bu, 1 - bu, 1}, vs[4] = {0, bv, 1 - bv, 1};
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i) {
                float qw = xs[i + 1] - xs[i], qh = ys[j + 1] - ys[j];
                if (qw <= 0 || qh <= 0) continue;
                hud_quad(m, t, sh, xs[i], ys[j], qw, qh, c, r2d_rect_make(us[i], vs[j], us[i + 1] - us[i], vs[j + 1] - vs[j]), &dyn);
            }
        return;
    }
    hud_quad(m, t, sh, x, y, w, h, c, r2d_rect_make(u0, v0, u1 - u0, v1 - v0), &dyn);
}
static void c_hud_text(void *impl, float x, float y, float scale, cw_rgba c, const char *text) {
    (void)impl;
    r2d_space previous = r2d_get_space();
    r2d_set_space(R2D_SPACE_HUD);
    r2d_draw_text_scaled(r2d_v2(x, y), color(c), scale, text);
    r2d_set_space(previous);
}
static float c_hud_text_width(void *impl, float scale, const char *text) { (void)impl; return r2d_text_width_scaled(scale, text); }

void host_canvas_init(host_canvas *hc) {
    memset(hc, 0, sizeof(*hc));
    hc->canvas.impl = hc;
    hc->canvas.circle = c_circle;
    hc->canvas.ring = c_ring;
    hc->canvas.line = c_line;
    hc->canvas.rect = c_rect;
    hc->canvas.rect_outline = c_rect_outline;
    hc->canvas.oriented_rect = c_oriented;
    hc->canvas.triangle = c_triangle;
    hc->canvas.bar = c_bar;
    hc->canvas.text = c_text;
    hc->canvas.px = c_px;
    hc->canvas.light = c_light;
    hc->canvas.occluder_circle = c_occluder_circle;
    hc->canvas.occluder_rect = c_occluder_rect;
    hc->canvas.pick = c_pick;
    hc->canvas.sprite = c_sprite;
    hc->canvas.hud_image = c_hud_image;
    hc->canvas.hud_text = c_hud_text;
    hc->canvas.hud_text_width = c_hud_text_width;
    hc->lights = true;
    hc->pickable = true;
}
void host_canvas_destroy(host_canvas *hc) {
    drop_caches(hc);
    free(hc->materials);
    free(hc->textures);
    free(hc->shaders);
    free(hc->picks);
    memset(hc, 0, sizeof(*hc));
}
void host_canvas_begin(host_canvas *hc) {
    hc->pick_count = 0;
    if (hc->reload_pending) drop_caches(hc);
}
const host_pick *host_canvas_find(const host_canvas *hc, cw_entity_id id) {
    for (size_t i = hc->pick_count; i-- > 0;)
        if (hc->picks[i].id == id) return &hc->picks[i];
    return NULL;
}
