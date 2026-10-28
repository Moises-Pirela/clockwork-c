#include "r2d_sprite_internal.h"
#include "sokol_log.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float x, y, u, v, r, g, b, a; } vertex;
typedef struct {
    int layer;
    int kind;                 /* 0 world, 1 hud, 2 screen (see r2d_space) */
    size_t order;
    r2d_effect effect;
    r2d_blend blend;
    bool nearest, repeat;
    float params[R2D_MATERIAL_PARAMS][4];
    float tiling[2], offset[2];
    sg_view texture;
    int tex_w, tex_h;
    sg_shader shader;         /* custom, or 0 for the built-in program */
    float size[2];            /* the quad, in its space's units (pixels for hud and screen sprites) */
    r2d_sprite_dynamic dyn;
    vertex v[6];
} sprite;
typedef struct {
    sg_pixel_format color, depth;
    int samples;
    r2d_blend blend;
    uint32_t shader_id;
    sg_pipeline pipeline;
} pipeline_entry;
#define MAX_PIPELINES 48

static struct {
    bool ready;
    sg_shader shader;
    sg_buffer vertices[3];    /* per kind: a transient buffer may only be written before it is bound, so one per flush */
    sg_sampler samplers[4];   /* [nearest][repeat] */
    r2d_texture white;
    sprite *sprites;
    size_t count;
    pipeline_entry pipelines[MAX_PIPELINES];
    int pipeline_count;
    r2d_sprite_stats stats, pending;
    char gfx_log[1024];
} S;

/* --- textures --- */
bool r2d_texture_create(r2d_texture *t, const r2d_image *img) {
    if (!t) return false;
    memset(t, 0, sizeof(*t));
    if (!img || !img->pixels || img->width <= 0 || img->height <= 0) return false;
    sg_image_desc d;
    memset(&d, 0, sizeof(d));
    d.width = img->width;
    d.height = img->height;
    d.pixel_format = SG_PIXELFORMAT_RGBA8;
    d.data.ptr = img->pixels;
    d.data.size = (size_t)img->width * (size_t)img->height * 4;
    d.label = "r2d-sprite-texture";
    t->image = sg_make_image(&d);
    if (sg_query_image_state(t->image) != SG_RESOURCESTATE_VALID) { memset(t, 0, sizeof(*t)); return false; }
    sg_view_desc v;
    memset(&v, 0, sizeof(v));
    v.texture.image = t->image;
    t->view = sg_make_view(&v);
    t->width = img->width;
    t->height = img->height;
    return true;
}
void r2d_texture_destroy(r2d_texture *t) {
    if (!t) return;
    if (t->view.id) sg_destroy_view(t->view);
    if (t->image.id) sg_destroy_image(t->image);
    memset(t, 0, sizeof(*t));
}
const r2d_texture *r2d_texture_white(void) { return &S.white; }

/* --- shaders --- */
static void fill_shader_desc(sg_shader_desc *d, const char *fs) {
    memset(d, 0, sizeof(*d));
    d->fragment_func.source = fs;
    switch (sg_query_backend()) {
        case SG_BACKEND_GLCORE:
            d->vertex_func.source = r2d_sprite_vs_glsl;
            break;
        case SG_BACKEND_D3D11:
            d->vertex_func.source = r2d_sprite_vs_hlsl;
            d->vertex_func.entry = "vs_main";
            d->fragment_func.entry = "fs_main";
            break;
        default:
            d->vertex_func.source = r2d_sprite_vs_msl;
            d->vertex_func.entry = "vs_main";
            d->fragment_func.entry = "fs_main";
            break;
    }
    d->attrs[0].glsl_name = "pos";
    d->attrs[0].hlsl_sem_name = "POSITION";
    d->attrs[0].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    d->attrs[1].glsl_name = "uv_in";
    d->attrs[1].hlsl_sem_name = "TEXCOORD";
    d->attrs[1].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    d->attrs[2].glsl_name = "color_in";
    d->attrs[2].hlsl_sem_name = "COLOR";
    d->attrs[2].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    d->uniform_blocks[0].stage = SG_SHADERSTAGE_VERTEX;
    d->uniform_blocks[0].size = 64;
    d->uniform_blocks[0].hlsl_register_b_n = 0;
    d->uniform_blocks[0].msl_buffer_n = 0;
    d->uniform_blocks[0].layout = SG_UNIFORMLAYOUT_STD140;
    d->uniform_blocks[0].glsl_uniforms[0].type = SG_UNIFORMTYPE_MAT4;
    d->uniform_blocks[0].glsl_uniforms[0].glsl_name = "mvp";
    d->uniform_blocks[1].stage = SG_SHADERSTAGE_FRAGMENT;
    d->uniform_blocks[1].size = 9 * 16;
    d->uniform_blocks[1].hlsl_register_b_n = 0;
    d->uniform_blocks[1].msl_buffer_n = 0;
    d->uniform_blocks[1].layout = SG_UNIFORMLAYOUT_STD140;
    d->uniform_blocks[1].glsl_uniforms[0].type = SG_UNIFORMTYPE_FLOAT4;
    d->uniform_blocks[1].glsl_uniforms[0].array_count = 9;
    d->uniform_blocks[1].glsl_uniforms[0].glsl_name = "params";
    d->views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
    d->views[0].texture.image_type = SG_IMAGETYPE_2D;
    d->views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
    d->views[0].texture.hlsl_register_t_n = 0;
    d->views[0].texture.msl_texture_n = 0;
    d->samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d->samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
    d->samplers[0].hlsl_register_s_n = 0;
    d->samplers[0].msl_sampler_n = 0;
    d->texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d->texture_sampler_pairs[0].view_slot = 0;
    d->texture_sampler_pairs[0].sampler_slot = 0;
    d->texture_sampler_pairs[0].glsl_name = "tex";
    d->label = "r2d-sprite";
}
/* the last message sokol_gfx logged: compile errors end up here */
void r2d_sprite__gfx_logger(const char *tag, uint32_t level, uint32_t item, const char *message, uint32_t line, const char *file, void *user) {
    if (message && level <= 3) snprintf(S.gfx_log, sizeof(S.gfx_log), "%s", message);   /* the driver's log follows the error */
    slog_func(tag, level, item, message, line, file, user);
}
const char *r2d_shader_preamble(void) { return r2d_sprite_fs_glsl_preamble; }
const char *r2d_shader_example(void) { return r2d_shader_example_source(0); }
bool r2d_shader_compile(r2d_shader *sh, const char *source) {
    if (!sh) return false;
    r2d_shader_destroy(sh);
    if (sg_query_backend() != SG_BACKEND_GLCORE) {
        snprintf(sh->log, sizeof(sh->log), "custom shaders are GLSL and need the OpenGL backend; this backend draws the sprite effect instead");
        return false;
    }
    if (!source || !strstr(source, "material")) { snprintf(sh->log, sizeof(sh->log), "the shader must define vec4 material(vec4 base, vec2 uv, vec4 p0, vec4 p1, vec4 p2, vec4 p3, float time)"); return false; }
    char *full = r2d_sprite__fragment_source(SG_BACKEND_GLCORE, source);   /* with #line 1 so messages use the asset's own line numbers */
    if (!full) return false;
    sg_shader_desc d;
    fill_shader_desc(&d, full);
    d.label = "r2d-sprite-custom";
    S.gfx_log[0] = 0;
    sh->shader = sg_make_shader(&d);
    free(full);
    sh->valid = sg_query_shader_state(sh->shader) == SG_RESOURCESTATE_VALID;
    if (!sh->valid) {
        if (sh->shader.id) sg_destroy_shader(sh->shader);
        sh->shader.id = 0;
        snprintf(sh->log, sizeof(sh->log), "%s", S.gfx_log[0] ? S.gfx_log : "the shader did not compile (see the log output)");
    } else snprintf(sh->log, sizeof(sh->log), "compiled");
    return sh->valid;
}
void r2d_shader_destroy(r2d_shader *sh) {
    if (!sh) return;
    if (sh->shader.id) sg_destroy_shader(sh->shader);
    memset(sh, 0, sizeof(*sh));
}

/* --- setup --- */
bool r2d_sprite__setup(void) {
    memset(&S, 0, sizeof(S));
    sg_shader_desc d;
    char *full = r2d_sprite__fragment_source(sg_query_backend(), NULL);
    if (!full) return false;
    fill_shader_desc(&d, full);
    S.shader = sg_make_shader(&d);
    free(full);
    if (sg_query_shader_state(S.shader) != SG_RESOURCESTATE_VALID) { fputs("render2d: sprite shader failed to compile; sprites are disabled\n", stderr); return false; }
    sg_buffer_desc vb;
    memset(&vb, 0, sizeof(vb));
    vb.size = sizeof(vertex) * 6 * R2D_MAX_SPRITES;
    vb.usage.write_transient = true;
    static const char *const labels[3] = {"r2d-sprite-vertices", "r2d-sprite-vertices-hud", "r2d-sprite-vertices-screen"};
    for (int i = 0; i < 3; ++i) { vb.label = labels[i]; S.vertices[i] = sg_make_buffer(&vb); }
    for (int nearest = 0; nearest < 2; ++nearest)
        for (int repeat = 0; repeat < 2; ++repeat) {
            sg_sampler_desc sd;
            memset(&sd, 0, sizeof(sd));
            sd.min_filter = sd.mag_filter = nearest ? SG_FILTER_NEAREST : SG_FILTER_LINEAR;
            sd.wrap_u = sd.wrap_v = repeat ? SG_WRAP_REPEAT : SG_WRAP_CLAMP_TO_EDGE;
            S.samplers[nearest * 2 + repeat] = sg_make_sampler(&sd);
        }
    r2d_image white;
    r2d_image_solid(&white, 1, 1, 255, 255, 255, 255);
    r2d_texture_create(&S.white, &white);
    r2d_image_free(&white);
    S.sprites = malloc(sizeof(sprite) * R2D_MAX_SPRITES);
    if (!S.sprites) return false;
    S.ready = true;
    return true;
}
void r2d_sprite__shutdown(void) {
    if (!S.ready) return;
    for (int i = 0; i < S.pipeline_count; ++i) sg_destroy_pipeline(S.pipelines[i].pipeline);
    for (int i = 0; i < 4; ++i) sg_destroy_sampler(S.samplers[i]);
    r2d_texture_destroy(&S.white);
    for (int i = 0; i < 3; ++i) sg_destroy_buffer(S.vertices[i]);
    sg_destroy_shader(S.shader);
    free(S.sprites);
    memset(&S, 0, sizeof(S));
}
void r2d_sprite__discard(void) { S.count = 0; }
r2d_sprite_stats r2d_sprite_get_stats(void) { return S.stats; }

/* --- drawing --- */
static void push_sprite(const r2d_material *m, const r2d_texture *tex, const r2d_shader *custom,
                        r2d_vec2 center, r2d_vec2 size, float rotation, r2d_color tint, int layer, const r2d_rect *uv, int kind,
                        const r2d_sprite_dynamic *dyn) {
    if (!S.ready || !m || S.count >= R2D_MAX_SPRITES) return;
    if (!tex || !tex->view.id) tex = &S.white;
    sprite *s = &S.sprites[S.count];
    memset(s, 0, sizeof(*s));
    s->layer = layer;
    s->kind = kind;
    s->order = S.count;
    s->effect = m->effect;
    s->blend = m->blend;
    s->nearest = m->nearest;
    s->repeat = m->repeat;
    memcpy(s->params, m->params, sizeof(s->params));
    s->tiling[0] = m->tiling[0]; s->tiling[1] = m->tiling[1];
    s->offset[0] = m->offset[0]; s->offset[1] = m->offset[1];
    s->texture = tex->view;
    s->tex_w = tex->width;
    s->tex_h = tex->height;
    /* the size and the dynamic inputs are uniforms, so they split batches:
     * only the effects that read them carry them */
    bool reads_extras = m->effect >= R2D_EFFECT_ROUNDED;
    if (reads_extras) { s->size[0] = size.x; s->size[1] = size.y; }
    if (reads_extras && dyn) s->dyn = *dyn;
    if (m->effect == R2D_EFFECT_CUSTOM) {
        if (custom && custom->valid) s->shader = custom->shader;
        else s->effect = R2D_EFFECT_SPRITE;
    }
    float r = tint.r / 255.0f * m->tint[0], g = tint.g / 255.0f * m->tint[1], b = tint.b / 255.0f * m->tint[2], a = tint.a / 255.0f * m->tint[3];
    float c = cosf(rotation), sn = sinf(rotation), hw = size.x * 0.5f, hh = size.y * 0.5f;
    r2d_rect t = uv ? *uv : r2d_rect_make(0, 0, 1, 1);
    const float corners[4][2] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
    const float uvs[4][2] = {{t.x, t.y}, {t.x + t.w, t.y}, {t.x + t.w, t.y + t.h}, {t.x, t.y + t.h}};
    vertex q[4];
    for (int i = 0; i < 4; ++i) {
        q[i].x = center.x + corners[i][0] * c - corners[i][1] * sn;
        q[i].y = center.y + corners[i][0] * sn + corners[i][1] * c;
        q[i].u = uvs[i][0]; q[i].v = uvs[i][1];
        q[i].r = r; q[i].g = g; q[i].b = b; q[i].a = a;
    }
    s->v[0] = q[0]; s->v[1] = q[1]; s->v[2] = q[2];
    s->v[3] = q[0]; s->v[4] = q[2]; s->v[5] = q[3];
    ++S.count;
}
void r2d_draw_sprite(const r2d_material *m, const r2d_texture *tex, const r2d_shader *custom,
                     r2d_vec2 center, r2d_vec2 size, float rotation, r2d_color tint, int layer, const r2d_rect *uv) {
    push_sprite(m, tex, custom, center, size, rotation, tint, layer, uv, 0, NULL);
}
void r2d_draw_sprite_hud(const r2d_material *m, const r2d_texture *tex, const r2d_shader *custom,
                         r2d_vec2 center, r2d_vec2 size, float rotation, r2d_color tint, int layer, const r2d_rect *uv, const r2d_sprite_dynamic *dyn) {
    push_sprite(m, tex, custom, center, size, rotation, tint, layer, uv, 1, dyn);
}
void r2d_draw_sprite_screen(const r2d_material *m, const r2d_texture *tex, const r2d_shader *custom,
                            r2d_vec2 center, r2d_vec2 size, float rotation, r2d_color tint, const r2d_rect *uv, const r2d_sprite_dynamic *dyn) {
    push_sprite(m, tex, custom, center, size, rotation, tint, 0, uv, 2, dyn);
}
static int compare_sprites(const void *a, const void *b) {
    const sprite *x = a, *y = b;
    if (x->layer != y->layer) return x->layer < y->layer ? -1 : 1;
    return x->order < y->order ? -1 : (x->order > y->order);
}
static sg_pipeline pipeline_for(sg_pixel_format color, sg_pixel_format depth, int samples, r2d_blend blend, sg_shader shader) {
    for (int i = 0; i < S.pipeline_count; ++i) {
        pipeline_entry *e = &S.pipelines[i];
        if (e->color == color && e->depth == depth && e->samples == samples && e->blend == blend && e->shader_id == shader.id) return e->pipeline;
    }
    if (S.pipeline_count == MAX_PIPELINES) { sg_destroy_pipeline(S.pipelines[0].pipeline); memmove(S.pipelines, S.pipelines + 1, sizeof(S.pipelines[0]) * (MAX_PIPELINES - 1)); --S.pipeline_count; }
    sg_pipeline_desc p;
    memset(&p, 0, sizeof(p));
    p.shader = shader;
    p.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
    p.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT2;
    p.layout.attrs[2].format = SG_VERTEXFORMAT_FLOAT4;
    p.colors[0].pixel_format = color;
    p.depth.pixel_format = depth;
    p.depth.write_enabled = false;
    p.sample_count = samples;
    p.colors[0].blend.enabled = blend != R2D_BLEND_OPAQUE;
    switch (blend) {
        case R2D_BLEND_ADDITIVE:
            p.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
            p.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE;
            p.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ZERO;
            p.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
            break;
        case R2D_BLEND_MULTIPLY:
            p.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_DST_COLOR;
            p.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            p.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ZERO;
            p.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
            break;
        default:
            p.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
            p.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            p.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
            p.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;
    }
    p.label = "r2d-sprite";
    pipeline_entry *e = &S.pipelines[S.pipeline_count++];
    e->color = color; e->depth = depth; e->samples = samples; e->blend = blend; e->shader_id = shader.id;
    e->pipeline = sg_make_pipeline(&p);
    return e->pipeline;
}
/* Draws the sprites of one kind (world, hud or screen) and removes them from the list. */
static void flush_kind(int kind, sg_pixel_format color, sg_pixel_format depth, int samples, const float *mvp, float time) {
    size_t n = 0;
    for (size_t i = 0; i < S.count; ++i) if (S.sprites[i].kind == kind) ++n;
    if (!n) return;
    /* the ones to draw first, sorted; the others stay behind them */
    sprite *kept = malloc(sizeof(sprite) * S.count);
    vertex *verts = malloc(sizeof(vertex) * 6 * n);
    if (!kept || !verts) { free(kept); free(verts); S.count = 0; return; }
    size_t d = 0, k = 0;
    for (size_t i = 0; i < S.count; ++i) { if (S.sprites[i].kind == kind) S.sprites[d++] = S.sprites[i]; else kept[k++] = S.sprites[i]; }
    qsort(S.sprites, d, sizeof(sprite), compare_sprites);
    for (size_t i = 0; i < d; ++i) memcpy(verts + i * 6, S.sprites[i].v, sizeof(S.sprites[i].v));
    sg_write_buffer_desc wb;
    memset(&wb, 0, sizeof(wb));
    wb.src.data.ptr = verts;
    wb.src.data.size = sizeof(vertex) * 6 * d;
    wb.dst.buffer = S.vertices[kind];
    sg_write_buffer_transient(&wb);
    free(verts);
    sg_range mvp_range = {mvp, 64};
    size_t count = d, start = 0;
    while (start < count) {
        const sprite *a = &S.sprites[start];
        size_t end = start + 1;
        while (end < S.count) {
            const sprite *b = &S.sprites[end];
            if (b->blend != a->blend || b->shader.id != a->shader.id || b->texture.id != a->texture.id || b->nearest != a->nearest || b->repeat != a->repeat
                || b->effect != a->effect || memcmp(b->params, a->params, sizeof(a->params)) != 0 || memcmp(b->tiling, a->tiling, sizeof(a->tiling)) != 0
                || memcmp(b->offset, a->offset, sizeof(a->offset)) != 0 || memcmp(b->size, a->size, sizeof(a->size)) != 0
                || memcmp(&b->dyn, &a->dyn, sizeof(a->dyn)) != 0) break;
            ++end;
        }
        sg_shader shader = a->shader.id ? a->shader : S.shader;
        sg_apply_pipeline(pipeline_for(color, depth, samples, a->blend, shader));
        sg_bindings bind;
        memset(&bind, 0, sizeof(bind));
        bind.vertex_buffers[0] = S.vertices[kind];
        bind.views[0] = a->texture;
        bind.samplers[0] = S.samplers[(a->nearest ? 2 : 0) + (a->repeat ? 1 : 0)];
        sg_apply_bindings(&bind);
        sg_apply_uniforms(0, &mvp_range);
        float params[9][4];
        memset(params, 0, sizeof(params));
        params[0][0] = (float)a->effect; params[0][1] = time;
        params[0][2] = a->tex_w > 0 ? 1.0f / (float)a->tex_w : 0; params[0][3] = a->tex_h > 0 ? 1.0f / (float)a->tex_h : 0;
        memcpy(params[1], a->params, sizeof(a->params));
        params[5][0] = a->tiling[0]; params[5][1] = a->tiling[1]; params[5][2] = a->offset[0]; params[5][3] = a->offset[1];
        params[6][0] = a->size[0]; params[6][1] = a->size[1]; params[6][2] = a->dyn.state[0]; params[6][3] = a->dyn.state[1];
        params[7][0] = a->dyn.state[2]; params[7][1] = a->dyn.state[3];
        memcpy(params[8], a->dyn.elem, sizeof(a->dyn.elem));
        sg_range prange = {params, sizeof(params)};
        sg_apply_uniforms(1, &prange);
        sg_draw((int)(start * 6), (int)((end - start) * 6), 1);
        ++S.pending.batches;
        start = end;
    }
    memcpy(S.sprites, kept, sizeof(sprite) * k);
    S.count = k;
    free(kept);
}
void r2d_sprite__flush(sg_pixel_format color, sg_pixel_format depth, int samples, const r2d_camera *cam, r2d_vec2 viewport, float time) {
    if (!S.ready) return;
    S.pending.sprites = (int)S.count;
    S.pending.batches = 0;
    /* world -> clip: screen = viewport/2 + (world - centre) * zoom (y down), clip = screen / viewport * 2 - 1 */
    float zoom = cam->zoom, vw = viewport.x > 0 ? viewport.x : 1, vh = viewport.y > 0 ? viewport.y : 1;
    float sx = 2.0f * zoom / vw, sy = (cam->y_up ? 2.0f : -2.0f) * zoom / vh;
    float tx = -cam->center.x * sx, ty = -cam->center.y * sy;
    float mvp[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, 1, 0, tx, ty, 0, 1};
    flush_kind(0, color, depth, samples, mvp, time);
    S.pending.pipelines = S.pipeline_count;
    S.stats = S.pending;
}
static void flush_pixels(int kind, sg_pixel_format color, sg_pixel_format depth, int samples, r2d_vec2 viewport, float time) {
    float vw = viewport.x > 0 ? viewport.x : 1, vh = viewport.y > 0 ? viewport.y : 1;
    float mvp[16] = {2.0f / vw, 0, 0, 0, 0, -2.0f / vh, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};
    flush_kind(kind, color, depth, samples, mvp, time);
    S.stats.batches = S.pending.batches;
}
void r2d_sprite__flush_hud(sg_pixel_format color, sg_pixel_format depth, int samples, r2d_vec2 viewport, float time) {
    if (!S.ready) return;
    flush_pixels(1, color, depth, samples, viewport, time);
}
void r2d_sprite__flush_screen(sg_pixel_format color, sg_pixel_format depth, int samples, r2d_vec2 viewport, float time) {
    if (!S.ready) return;
    flush_pixels(2, color, depth, samples, viewport, time);
    S.count = 0;
}
