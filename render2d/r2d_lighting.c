#include "r2d_lighting_internal.h"
#include "r2d_post_shaders.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define R2D_L_TAU 6.28318530717959f

typedef struct {
    sg_image image;
    sg_view texture, attachment;
    int width, height;
    sg_pixel_format format;
} lm_target;

static struct {
    bool created, valid;
    r2d_light_desc lights[R2D_MAX_LIGHTS];
    int light_count, lights_dropped;
    r2d_circle circles[R2D_MAX_OCCLUDERS];
    r2d_segment segments[R2D_MAX_OCCLUDERS];
    int circle_count, segment_count;
    sg_shader shader;
    sg_pipeline pipeline;
    sg_pixel_format pipeline_format;
    sg_buffer quad;
    sg_sampler shadow_sampler;
    sg_image shadow_image;
    sg_view shadow_view;
    int shadow_angles;
    float *shadow_data;
    r2d_circle *px_circles;
    r2d_segment *px_segments;
    lm_target direct, volume;
    r2d_lighting_stats stats;
} L;

/* ---------------------------------------------------------------------------
 * submission
 * ------------------------------------------------------------------------- */
bool r2d_light(const r2d_light_desc *light) {
    if (!light || !(light->radius > 0)) return false;
    if (L.light_count >= R2D_MAX_LIGHTS) { ++L.lights_dropped; return false; }
    L.lights[L.light_count++] = *light;
    return true;
}
void r2d_light_occluder_circle(r2d_vec2 center, float radius) {
    if (!(radius > 0) || L.circle_count >= R2D_MAX_OCCLUDERS) return;
    L.circles[L.circle_count].center = center;
    L.circles[L.circle_count].radius = radius;
    ++L.circle_count;
}
void r2d_light_occluder_segment(r2d_vec2 a, r2d_vec2 b) {
    if (L.segment_count >= R2D_MAX_OCCLUDERS) return;
    L.segments[L.segment_count].a = a;
    L.segments[L.segment_count].b = b;
    ++L.segment_count;
}
void r2d_light_occluder_rect(r2d_rect r) {
    r2d_vec2 p[4] = {r2d_v2(r.x, r.y), r2d_v2(r.x + r.w, r.y), r2d_v2(r.x + r.w, r.y + r.h), r2d_v2(r.x, r.y + r.h)};
    r2d_light_occluder_polyline(p, 4, true);
}
void r2d_light_occluder_polyline(const r2d_vec2 *points, size_t count, bool closed) {
    if (!points || count < 2) return;
    for (size_t i = 0; i + 1 < count; ++i) r2d_light_occluder_segment(points[i], points[i + 1]);
    if (closed) r2d_light_occluder_segment(points[count - 1], points[0]);
}
r2d_lighting_stats r2d_lighting_get_stats(void) { return L.stats; }
void r2d_lighting__end_frame(void) {
    L.light_count = 0;
    L.lights_dropped = 0;
    L.circle_count = 0;
    L.segment_count = 0;
}

/* ---------------------------------------------------------------------------
 * resources
 * ------------------------------------------------------------------------- */
static void destroy_target(lm_target *t) {
    if (t->texture.id) sg_destroy_view(t->texture);
    if (t->attachment.id) sg_destroy_view(t->attachment);
    if (t->image.id) sg_destroy_image(t->image);
    memset(t, 0, sizeof(*t));
}
static bool make_target(lm_target *t, int width, int height, sg_pixel_format format, const char *label) {
    destroy_target(t);
    sg_image_desc img;
    memset(&img, 0, sizeof(img));
    img.width = width;
    img.height = height;
    img.pixel_format = format;
    img.sample_count = 1;
    img.usage.color_attachment = true;
    img.label = label;
    t->image = sg_make_image(&img);
    if (sg_query_image_state(t->image) != SG_RESOURCESTATE_VALID) { destroy_target(t); return false; }
    sg_view_desc view;
    memset(&view, 0, sizeof(view));
    view.color_attachment.image = t->image;
    t->attachment = sg_make_view(&view);
    memset(&view, 0, sizeof(view));
    view.texture.image = t->image;
    t->texture = sg_make_view(&view);
    t->width = width;
    t->height = height;
    t->format = format;
    return true;
}
static sg_shader make_shader(void) {
    sg_shader_desc d;
    memset(&d, 0, sizeof(d));
    switch (sg_query_backend()) {
        case SG_BACKEND_GLCORE:
            d.vertex_func.source = r2d_light_vs_glsl;
            d.fragment_func.source = r2d_light_fs_glsl;
            break;
        case SG_BACKEND_D3D11:
            d.vertex_func.source = r2d_light_vs_hlsl;
            d.vertex_func.entry = "vs_main";
            d.fragment_func.source = r2d_light_fs_hlsl;
            d.fragment_func.entry = "fs_main";
            break;
        case SG_BACKEND_METAL_MACOS:
        case SG_BACKEND_METAL_IOS:
        case SG_BACKEND_METAL_SIMULATOR:
            d.vertex_func.source = r2d_light_vs_msl;
            d.vertex_func.entry = "vs_main";
            d.fragment_func.source = r2d_light_fs_msl;
            d.fragment_func.entry = "fs_main";
            break;
        default: {
            sg_shader none = {0};
            return none;
        }
    }
    d.attrs[0].glsl_name = "pos";
    d.attrs[0].hlsl_sem_name = "POSITION";
    d.attrs[0].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    d.uniform_blocks[0].stage = SG_SHADERSTAGE_VERTEX;
    d.uniform_blocks[0].size = 16;
    d.uniform_blocks[0].hlsl_register_b_n = 0;
    d.uniform_blocks[0].msl_buffer_n = 0;
    d.uniform_blocks[0].layout = SG_UNIFORMLAYOUT_STD140;
    d.uniform_blocks[0].glsl_uniforms[0].type = SG_UNIFORMTYPE_FLOAT4;
    d.uniform_blocks[0].glsl_uniforms[0].array_count = 1;
    d.uniform_blocks[0].glsl_uniforms[0].glsl_name = "vs_params";
    d.uniform_blocks[1].stage = SG_SHADERSTAGE_FRAGMENT;
    d.uniform_blocks[1].size = 5 * 16;
    d.uniform_blocks[1].hlsl_register_b_n = 0;
    d.uniform_blocks[1].msl_buffer_n = 0;
    d.uniform_blocks[1].layout = SG_UNIFORMLAYOUT_STD140;
    d.uniform_blocks[1].glsl_uniforms[0].type = SG_UNIFORMTYPE_FLOAT4;
    d.uniform_blocks[1].glsl_uniforms[0].array_count = 5;
    d.uniform_blocks[1].glsl_uniforms[0].glsl_name = "fs_params";
    d.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
    d.views[0].texture.image_type = SG_IMAGETYPE_2D;
    d.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_UNFILTERABLE_FLOAT;
    d.views[0].texture.hlsl_register_t_n = 0;
    d.views[0].texture.msl_texture_n = 0;
    d.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.samplers[0].sampler_type = SG_SAMPLERTYPE_NONFILTERING;
    d.samplers[0].hlsl_register_s_n = 0;
    d.samplers[0].msl_sampler_n = 0;
    d.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.texture_sampler_pairs[0].view_slot = 0;
    d.texture_sampler_pairs[0].sampler_slot = 0;
    d.texture_sampler_pairs[0].glsl_name = "shadow_tex";
    d.label = "r2d-light";
    return sg_make_shader(&d);
}
static bool make_pipeline(sg_pixel_format format) {
    if (L.pipeline.id) sg_destroy_pipeline(L.pipeline);
    sg_pipeline_desc pip;
    memset(&pip, 0, sizeof(pip));
    pip.shader = L.shader;
    pip.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
    pip.color_count = 2;
    for (int i = 0; i < 2; ++i) {
        pip.colors[i].pixel_format = format;
        pip.colors[i].blend.enabled = true;
        pip.colors[i].blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
        pip.colors[i].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE;
        pip.colors[i].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
        pip.colors[i].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
    }
    pip.depth.pixel_format = SG_PIXELFORMAT_NONE;
    pip.sample_count = 1;
    pip.label = "r2d-light";
    L.pipeline = sg_make_pipeline(&pip);
    L.pipeline_format = format;
    return sg_query_pipeline_state(L.pipeline) == SG_RESOURCESTATE_VALID;
}
static bool make_shadow_image(int angles) {
    if (L.shadow_view.id) sg_destroy_view(L.shadow_view);
    if (L.shadow_image.id) sg_destroy_image(L.shadow_image);
    L.shadow_view.id = 0;
    L.shadow_image.id = 0;
    free(L.shadow_data);
    L.shadow_data = calloc((size_t)angles * R2D_MAX_LIGHTS, sizeof(float));
    if (!L.shadow_data) return false;
    sg_image_desc img;
    memset(&img, 0, sizeof(img));
    img.width = angles;
    img.height = R2D_MAX_LIGHTS;
    img.pixel_format = SG_PIXELFORMAT_R32F;
    img.usage.write_transient = true;
    img.label = "r2d-shadow-maps";
    L.shadow_image = sg_make_image(&img);
    if (sg_query_image_state(L.shadow_image) != SG_RESOURCESTATE_VALID) return false;
    sg_view_desc view;
    memset(&view, 0, sizeof(view));
    view.texture.image = L.shadow_image;
    L.shadow_view = sg_make_view(&view);
    L.shadow_angles = angles;
    return true;
}
bool r2d_lighting__create(void) {
    memset(&L, 0, sizeof(L));
    L.created = true;
    if (!sg_query_pixelformat(SG_PIXELFORMAT_R32F).sample) {
        fputs("render2d: R32F textures unsupported; lighting is disabled\n", stderr);
        return false;
    }
    L.shader = make_shader();
    if (sg_query_shader_state(L.shader) != SG_RESOURCESTATE_VALID) {
        fputs("render2d: light shader failed to compile; lighting is disabled\n", stderr);
        return false;
    }
    static const float quad[12] = {0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1};
    sg_buffer_desc vb;
    memset(&vb, 0, sizeof(vb));
    vb.data.ptr = quad;
    vb.data.size = sizeof(quad);
    vb.label = "r2d-light-quad";
    L.quad = sg_make_buffer(&vb);
    sg_sampler_desc smp;
    memset(&smp, 0, sizeof(smp));
    smp.min_filter = smp.mag_filter = SG_FILTER_NEAREST;
    smp.wrap_u = SG_WRAP_REPEAT;            /* the angle wraps around */
    smp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    smp.label = "r2d-shadow-sampler";
    L.shadow_sampler = sg_make_sampler(&smp);
    L.px_circles = calloc(R2D_MAX_OCCLUDERS, sizeof(*L.px_circles));
    L.px_segments = calloc(R2D_MAX_OCCLUDERS, sizeof(*L.px_segments));
    L.valid = L.px_circles && L.px_segments && make_shadow_image(720);
    return L.valid;
}
void r2d_lighting__destroy(void) {
    if (!L.created) return;
    destroy_target(&L.direct);
    destroy_target(&L.volume);
    if (L.shadow_view.id) sg_destroy_view(L.shadow_view);
    if (L.shadow_image.id) sg_destroy_image(L.shadow_image);
    if (L.pipeline.id) sg_destroy_pipeline(L.pipeline);
    if (L.shadow_sampler.id) sg_destroy_sampler(L.shadow_sampler);
    if (L.quad.id) sg_destroy_buffer(L.quad);
    if (L.shader.id) sg_destroy_shader(L.shader);
    free(L.shadow_data);
    free(L.px_circles);
    free(L.px_segments);
    memset(&L, 0, sizeof(L));
}

/* ---------------------------------------------------------------------------
 * the light pass
 * ------------------------------------------------------------------------- */
static double now_seconds(void) {
    return (double)clock() / (double)CLOCKS_PER_SEC;
}
bool r2d_lighting__render(const r2d_post_params *p, sg_pixel_format format, int scene_w, int scene_h,
                          const r2d_camera *camera, r2d_vec2 viewport, float dpi, r2d_lightmap *out) {
    if (!L.created || !L.valid || !p || !camera || scene_w < 1 || scene_h < 1) return false;
    int div = p->lighting.downscale < 1 ? 1 : p->lighting.downscale;
    int w = scene_w / div > 0 ? scene_w / div : 1, h = scene_h / div > 0 ? scene_h / div : 1;
    if (L.direct.width != w || L.direct.height != h || L.direct.format != format) {
        if (!make_target(&L.direct, w, h, format, "r2d-light-direct") || !make_target(&L.volume, w, h, format, "r2d-light-volume")) {
            L.valid = false;
            return false;
        }
    }
    if (L.pipeline_format != format || !L.pipeline.id) {
        if (!make_pipeline(format)) { L.valid = false; return false; }
    }
    int angles = p->lighting.shadow_angles < 64 ? 64 : (p->lighting.shadow_angles > 4096 ? 4096 : p->lighting.shadow_angles);
    if (angles != L.shadow_angles && !make_shadow_image(angles)) { L.valid = false; return false; }

    /* world -> lightmap pixels (y down): logical screen pixels scaled by the
     * actual scene target size (dpi and render scale) and the lightmap divisor */
    (void)dpi;
    float scale = viewport.x > 0 ? (float)scene_w / viewport.x / (float)div : 1.0f;
    int nl = L.light_count;
    if (p->lighting.max_lights >= 1 && nl > p->lighting.max_lights) nl = p->lighting.max_lights;
    for (int i = 0; i < L.circle_count; ++i) {
        r2d_vec2 c = r2d_scale(r2d_camera_world_to_screen(camera, viewport, L.circles[i].center), scale);
        L.px_circles[i].center = c;
        L.px_circles[i].radius = L.circles[i].radius * camera->zoom * scale;
    }
    for (int i = 0; i < L.segment_count; ++i) {
        L.px_segments[i].a = r2d_scale(r2d_camera_world_to_screen(camera, viewport, L.segments[i].a), scale);
        L.px_segments[i].b = r2d_scale(r2d_camera_world_to_screen(camera, viewport, L.segments[i].b), scale);
    }
    /* shadow maps */
    double t0 = now_seconds();
    size_t rays = 0;
    for (int i = 0; i < nl; ++i) {
        const r2d_light_desc *ld = &L.lights[i];
        float *row = L.shadow_data + (size_t)i * (size_t)angles;
        r2d_vec2 c = r2d_scale(r2d_camera_world_to_screen(camera, viewport, ld->position), scale);
        float r = ld->radius * camera->zoom * scale;
        if (ld->no_shadows) { for (int a = 0; a < angles; ++a) row[a] = r; continue; }
        rays += r2d_shadow_map_build(c, r, L.px_circles, (size_t)L.circle_count, L.px_segments, (size_t)L.segment_count, row, angles);
    }
    L.stats.shadow_seconds = now_seconds() - t0;
    L.stats.lights = nl;
    L.stats.occluders = L.circle_count + L.segment_count;
    L.stats.shadow_rays = (int)rays;
    L.stats.lightmap_width = w;
    L.stats.lightmap_height = h;
    if (nl > 0) {
        sg_write_image_desc wr;
        memset(&wr, 0, sizeof(wr));
        wr.src.data.ptr = L.shadow_data;
        wr.src.data.size = (size_t)angles * R2D_MAX_LIGHTS * sizeof(float);
        wr.dst.image = L.shadow_image;
        sg_write_image_transient(&wr);
    }

    /* the pass: ambient in the direct target, nothing in the volumetric one */
    sg_pass pass;
    memset(&pass, 0, sizeof(pass));
    pass.attachments.colors[0] = L.direct.attachment;
    pass.attachments.colors[1] = L.volume.attachment;
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value.r = p->lighting.ambient_r * p->lighting.ambient;
    pass.action.colors[0].clear_value.g = p->lighting.ambient_g * p->lighting.ambient;
    pass.action.colors[0].clear_value.b = p->lighting.ambient_b * p->lighting.ambient;
    pass.action.colors[0].clear_value.a = 1.0f;
    pass.action.colors[1].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[1].clear_value.a = 1.0f;
    pass.label = "r2d-lights";
    sg_begin_pass(&pass);
    if (nl > 0) {
        sg_apply_pipeline(L.pipeline);
        sg_bindings bind;
        memset(&bind, 0, sizeof(bind));
        bind.vertex_buffers[0] = L.quad;
        bind.views[0] = L.shadow_view;
        bind.samplers[0] = L.shadow_sampler;
        sg_apply_bindings(&bind);
        bool flip = sg_query_features().origin_top_left;
        for (int i = 0; i < nl; ++i) {
            const r2d_light_desc *ld = &L.lights[i];
            r2d_vec2 c = r2d_scale(r2d_camera_world_to_screen(camera, viewport, ld->position), scale);
            float r = ld->radius * camera->zoom * scale;
            if (c.x + r < 0 || c.y + r < 0 || c.x - r > (float)w || c.y - r > (float)h) continue;
            float x0 = (c.x - r) / (float)w * 2.0f - 1.0f, x1 = (c.x + r) / (float)w * 2.0f - 1.0f;
            float y0 = 1.0f - (c.y + r) / (float)h * 2.0f, y1 = 1.0f - (c.y - r) / (float)h * 2.0f;
            float vs[4] = {x0, y0, x1, y1};
            float intensity = (ld->intensity > 0 ? ld->intensity : 1.0f) * p->lighting.intensity;
            float fs[5][4];
            memset(fs, 0, sizeof(fs));
            fs[0][0] = c.x; fs[0][1] = c.y; fs[0][2] = r; fs[0][3] = ((float)i + 0.5f) / (float)R2D_MAX_LIGHTS;
            fs[1][0] = ld->color.r / 255.0f * intensity;
            fs[1][1] = ld->color.g / 255.0f * intensity;
            fs[1][2] = ld->color.b / 255.0f * intensity;
            fs[1][3] = ld->falloff > 0 ? ld->falloff : p->lighting.falloff;
            fs[2][0] = (ld->volumetric > 0 ? ld->volumetric : 1.0f) * p->lighting.volumetric;
            fs[2][1] = p->lighting.shadow_softness;
            fs[2][2] = (float)angles;
            fs[2][3] = flip ? 1.0f : 0.0f;
            fs[3][0] = (float)w; fs[3][1] = (float)h;
            /* spot direction is given in world orientation; the lightmap is y down */
            fs[3][2] = camera->y_up ? -ld->direction : ld->direction;
            fs[3][3] = ld->cone;
            fs[4][0] = ld->cone_softness > 0 ? ld->cone_softness : 0.3f;
            fs[4][1] = ld->spot ? 1.0f : 0.0f;
            fs[4][2] = ld->no_shadows ? 0.0f : 1.0f;
            sg_range vs_range = {vs, sizeof(vs)}, fs_range = {fs, sizeof(fs)};
            sg_apply_uniforms(0, &vs_range);
            sg_apply_uniforms(1, &fs_range);
            sg_draw(0, 6, 1);
        }
    }
    sg_end_pass();
    if (out) {
        out->direct = L.direct.texture;
        out->volume = L.volume.texture;
        out->width = w;
        out->height = h;
    }
    return true;
}
