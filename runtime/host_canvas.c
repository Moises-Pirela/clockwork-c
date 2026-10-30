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

/* --- the 3D view: materials, shapes, lights, brushes --- */
const host_material3d *host_canvas_material3d(host_canvas *hc, uint32_t id) {
    if (!id) return NULL;
    for (size_t i = 0; i < hc->material3d_count; ++i) if (hc->materials3d[i].id == id) return &hc->materials3d[i];
    if (hc->material3d_count == hc->material3d_capacity) {
        hc->material3d_capacity = hc->material3d_capacity ? hc->material3d_capacity * 2 : 16;
        hc->materials3d = realloc(hc->materials3d, hc->material3d_capacity * sizeof(*hc->materials3d));
        if (!hc->materials3d) { fputs("host_canvas: out of memory\n", stderr); abort(); }
    }
    host_material3d *m = &hc->materials3d[hc->material3d_count++];
    memset(m, 0, sizeof(*m));
    m->id = id;
    m->material = r3d_material_default();
    const host_material *hm = host_canvas_material(hc, id);   /* the same .cwmat file, parsed once */
    if (!hm || hm->missing) { m->missing = true; snprintf(m->error, sizeof(m->error), "%s", hm ? hm->error : "unknown material asset"); return m; }
    const r2d_material *src = &hm->material;
    memcpy(m->material.tint, src->tint, sizeof(m->material.tint));
    memcpy(m->material.tiling, src->tiling, sizeof(m->material.tiling));
    memcpy(m->material.offset, src->offset, sizeof(m->material.offset));
    memcpy(m->material.specular, src->specular, sizeof(m->material.specular));
    memcpy(m->material.emissive, src->emissive, sizeof(m->material.emissive));
    m->material.shininess = src->shininess;
    m->material.unlit = src->unlit;
    m->material.nearest = src->nearest;
    m->material.repeat = src->repeat;
    if (hm->texture) {
        char path[CW_ASSET_PATH_CAPACITY];
        r2d_image img;
        if (!cw_assets_path(hc->assets, hm->texture, path, sizeof(path))) snprintf(m->error, sizeof(m->error), "unknown image asset");
        else if (!r2d_image_load(&img, path, m->error, sizeof(m->error))) { /* error set */ }
        else {
            if (!r3d_texture_create(&m->texture, img.width, img.height, img.pixels)) snprintf(m->error, sizeof(m->error), "cannot upload the texture");
            r2d_image_free(&img);
        }
        m->material.texture = m->texture;
    }
    return m;
}
static void c_shape3d(void *impl, const cw_canvas_shape3d *shape) {
    host_canvas *hc = impl;
    if (!hc->view3d || !hc->view3d->ready || !shape) return;
    if (hc->shape_count == hc->shape_capacity) {
        hc->shape_capacity = hc->shape_capacity ? hc->shape_capacity * 2 : 256;
        hc->shapes = realloc(hc->shapes, hc->shape_capacity * sizeof(*hc->shapes));
        if (!hc->shapes) { fputs("host_canvas: out of memory\n", stderr); abort(); }
    }
    hc->shapes[hc->shape_count++] = *shape;
    r3d_primitive p = host_view3d_primitive(shape);
    const host_material3d *m = shape->material ? host_canvas_material3d(hc, shape->material) : NULL;
    r3d_material missing;
    if (m && m->missing) {   /* a visible placeholder, like the sprites' magenta */
        missing = r3d_material_default();
        missing.tint[0] = 1; missing.tint[1] = 0; missing.tint[2] = 1;
        p.material = &missing;
    } else if (m) p.material = &m->material;
    if (!hc->pickable) p.id = R3D_ID_NONE;
    r3d_draw_primitive(&p);
}
r3d_decal host_canvas_decal(host_canvas *hc, const cw_canvas_decal3d *d) {
    r3d_decal r;
    memset(&r, 0, sizeof(r));
    r.position = r3d_v3(d->x, d->y, d->z);
    r.rotation = r3d_v3(d->rx, d->ry, d->rz);
    r.size = r3d_v3(d->sx, d->sy, d->sz);
    r.color = host_view3d_color(d->color);
    r.max_angle = d->max_angle;
    r.id = hc->pickable ? (uint64_t)d->id : R3D_ID_NONE;
    const host_material3d *m = d->material ? host_canvas_material3d(hc, d->material) : NULL;
    if (m && m->missing) {   /* the placeholder magenta, so a missing material is noticed */
        r.color = r3d_rgba(255, 0, 255, d->color.a);
        r.material = NULL;
    } else if (m) r.material = &m->material;
    return r;
}
static void c_decal3d(void *impl, const cw_canvas_decal3d *d) {
    host_canvas *hc = impl;
    if (!hc->view3d || !hc->view3d->ready || !d) return;
    if (hc->decal_count == hc->decal_capacity) {
        hc->decal_capacity = hc->decal_capacity ? hc->decal_capacity * 2 : 64;
        hc->decals = realloc(hc->decals, hc->decal_capacity * sizeof(*hc->decals));
        if (!hc->decals) { fputs("host_canvas: out of memory\n", stderr); abort(); }
    }
    hc->decals[hc->decal_count++] = *d;
    r3d_decal decal = host_canvas_decal(hc, d);
    r3d_draw_decal(&decal);
}
static void c_light3d(void *impl, const cw_canvas_light3d *l) {
    host_canvas *hc = impl;
    if (!hc->view3d || !hc->view3d->ready || !l) return;
    if (l->kind == CW_LIGHT_AMBIENT) { r3d_add_ambient(host_view3d_color(l->color), l->intensity); return; }
    r3d_light light;
    memset(&light, 0, sizeof(light));
    light.kind = l->kind == CW_LIGHT_DIRECTIONAL ? R3D_LIGHT_DIRECTIONAL : (l->kind == CW_LIGHT_SPOT ? R3D_LIGHT_SPOT : R3D_LIGHT_POINT);
    light.position = r3d_v3(l->x, l->y, l->z);
    light.direction = r3d_v3(l->dx, l->dy, l->dz);
    light.color = host_view3d_color(l->color);
    light.intensity = l->intensity;
    light.range = l->range;
    light.cone = l->cone;
    light.shadows = l->shadows;
    r3d_add_light(&light);
}
/* The sky's six images as one cube map; a failure (missing image, faces of
 * different sizes) leaves the gradient and is reported once. */
static void sky_cube(host_canvas *hc, const uint32_t ids[6]) {
    if (memcmp(ids, hc->sky_ids, sizeof(hc->sky_ids)) == 0) return;
    memcpy(hc->sky_ids, ids, sizeof(hc->sky_ids));
    r3d_texture_destroy(&hc->sky_cube);
    hc->sky_failed = false;
    hc->sky_error[0] = 0;
    if (!ids[0]) return;
    r2d_image faces[6];
    memset(faces, 0, sizeof(faces));
    int loaded = 0, size = 0;
    for (; loaded < 6; ++loaded) {
        char path[CW_ASSET_PATH_CAPACITY];
        if (!cw_assets_path(hc->assets, ids[loaded], path, sizeof(path))) { snprintf(hc->sky_error, sizeof(hc->sky_error), "sky face %d: unknown image asset", loaded); break; }
        if (!r2d_image_load(&faces[loaded], path, hc->sky_error, sizeof(hc->sky_error))) break;
        if (faces[loaded].width != faces[loaded].height || (size && faces[loaded].width != size)) {
            snprintf(hc->sky_error, sizeof(hc->sky_error), "sky faces must be squares of one size (face %d is %dx%d)", loaded, faces[loaded].width, faces[loaded].height);
            ++loaded;
            break;
        }
        size = faces[loaded].width;
    }
    if (loaded == 6) {
        const void *pixels[6];
        for (int i = 0; i < 6; ++i) pixels[i] = faces[i].pixels;
        if (!r3d_texture_create_cube(&hc->sky_cube, size, pixels)) snprintf(hc->sky_error, sizeof(hc->sky_error), "cannot upload the sky's cube map");
    }
    for (int i = 0; i < loaded; ++i) r2d_image_free(&faces[i]);
    if (hc->sky_error[0]) { hc->sky_failed = true; fprintf(stderr, "host_canvas: %s\n", hc->sky_error); }
}
static void c_sky3d(void *impl, const cw_canvas_sky3d *s) {
    host_canvas *hc = impl;
    if (!hc->view3d || !hc->view3d->ready || !s) return;
    sky_cube(hc, s->images);
    r3d_sky sky;
    memset(&sky, 0, sizeof(sky));
    sky.zenith = host_view3d_color(s->zenith);
    sky.horizon = host_view3d_color(s->horizon);
    sky.ground = host_view3d_color(s->ground);
    sky.sun_size = s->sun_size;
    sky.sun_intensity = s->sun_intensity;
    if (!hc->sky_failed) sky.cube = hc->sky_cube;
    r3d_set_sky(&sky);
}
static void c_brush3d(void *impl, const cw_canvas_brush *b) {
    host_canvas *hc = impl;
    if (!hc->view3d || !hc->view3d->ready || !b || !b->brush) return;
    float center[3] = {b->x, b->y, b->z};
    if (hc->pickable && b->id != CW_INVALID_ENTITY) {   /* the solid's planes, so clipped brushes pick exactly */
        cw_plane planes[CW_BRUSH_MAX_FACES];
        r3d_plane rp[CW_BRUSH_MAX_FACES];
        int n = cw_brush_planes(b->brush, center, planes);
        for (int i = 0; i < n; ++i) { rp[i].n = r3d_v3(planes[i].n[0], planes[i].n[1], planes[i].n[2]); rp[i].d = planes[i].d; }
        r3d_pick_convex((uint64_t)b->id, rp, n);
    }
    static cw_brush_vertex verts[CW_BRUSH_FACE_MAX_VERTICES];
    static uint32_t indices[CW_BRUSH_FACE_MAX_INDICES];
    static r3d_vertex out[CW_BRUSH_FACE_MAX_VERTICES];
    int faces = cw_brush_face_count(b->brush);
    for (int face = 0; face < faces; ++face) {
        size_t ni = 0, n = cw_brush_face_mesh(b->brush, center, face, verts, indices, &ni);
        if (!n || !ni) continue;
        for (size_t i = 0; i < n; ++i) {
            out[i].x = verts[i].x; out[i].y = verts[i].y; out[i].z = verts[i].z; out[i].cap = 0;
            out[i].nx = verts[i].nx; out[i].ny = verts[i].ny; out[i].nz = verts[i].nz;
            out[i].u = verts[i].u; out[i].v = verts[i].v;
            out[i].rgba[0] = verts[i].r; out[i].rgba[1] = verts[i].g; out[i].rgba[2] = verts[i].b; out[i].rgba[3] = verts[i].a;
        }
        uint32_t asset = b->brush->material[face];
        const host_material3d *m = asset ? host_canvas_material3d(hc, asset) : NULL;
        r3d_material missing;
        const r3d_material *use = NULL;
        if (m && m->missing) { missing = r3d_material_default(); missing.tint[0] = 1; missing.tint[1] = 0; missing.tint[2] = 1; use = &missing; }
        else if (m) use = &m->material;
        r3d_draw_mesh(out, (int)n, indices, (int)ni, use, r3d_rgb(255, 255, 255), false);
    }
}
void host_canvas_set_view3d(host_canvas *hc, host_view3d *view) {
    hc->view3d = view;
    hc->canvas.shape3d = view ? c_shape3d : NULL;
    hc->canvas.light3d = view ? c_light3d : NULL;
    hc->canvas.brush3d = view ? c_brush3d : NULL;
    hc->canvas.sky3d = view ? c_sky3d : NULL;
    hc->canvas.decal3d = view ? c_decal3d : NULL;
}
const cw_canvas_shape3d *host_canvas_find_shape(const host_canvas *hc, cw_entity_id id) {
    for (size_t i = hc->shape_count; i-- > 0;)
        if (hc->shapes[i].id == id) return &hc->shapes[i];
    return NULL;
}
const cw_canvas_decal3d *host_canvas_find_decal(const host_canvas *hc, cw_entity_id id) {
    for (size_t i = hc->decal_count; i-- > 0;)
        if (hc->decals[i].id == id) return &hc->decals[i];
    return NULL;
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
    for (size_t i = 0; i < hc->material3d_count; ++i) r3d_texture_destroy(&hc->materials3d[i].texture);
    hc->texture_count = hc->shader_count = hc->material_count = hc->material3d_count = 0;
    r3d_texture_destroy(&hc->sky_cube);
    memset(hc->sky_ids, 0, sizeof(hc->sky_ids));   /* the sky reloads its faces */
    hc->sky_failed = false;
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
    free(hc->shapes);
    free(hc->decals);
    free(hc->materials3d);
    memset(hc, 0, sizeof(*hc));
}
void host_canvas_begin(host_canvas *hc) {
    hc->pick_count = 0;
    hc->shape_count = 0;
    hc->decal_count = 0;
    if (hc->reload_pending) drop_caches(hc);
}
const host_pick *host_canvas_find(const host_canvas *hc, cw_entity_id id) {
    for (size_t i = hc->pick_count; i-- > 0;)
        if (hc->picks[i].id == id) return &hc->picks[i];
    return NULL;
}
