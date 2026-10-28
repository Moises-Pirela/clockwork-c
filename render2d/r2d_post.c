#include "r2d_post_internal.h"
#include "r2d_lighting_internal.h"
#include "r2d_post_shaders.h"
#include "sokol_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { KIND_BLIT, KIND_THRESHOLD, KIND_BLUR, KIND_BLOOM_COMBINE, KIND_DOF_COMBINE,
       KIND_MOTION_BLUR, KIND_CHROMATIC, KIND_TONEMAP, KIND_VIGNETTE, KIND_SHAKE, KIND_LIGHTING };

typedef struct {
    sg_image image;
    sg_view texture, attachment, resolve;
    int width, height, sample_count;
    sg_pixel_format format;
} target;

typedef struct { float v[8][4]; } uniforms;

static struct {
    bool created, valid, in_scene, targets_ready;
    bool bypass;                 /* draw straight to the window this frame, whatever the config says */
    r2d_post_params params;      /* as tuned (artistic) */
    r2d_post_params effective;   /* params clamped to the tier in effect; what the passes use */
    r2d_quality_controller controller;
    bool controller_ready, tier_automatic;
    r2d_tier tier;
    float frame_ms;              /* smoothed frame time for the stats */
    sg_shader shader;
    sg_pipeline pipeline_offscreen, pipeline_swapchain;
    sg_buffer vertices;
    sg_sampler sampler;
    sg_pixel_format swap_color, swap_depth, scene_format, pipeline_format;
    int swap_samples;
    bool hdr_supported, hdr_msaa_supported, origin_top_left, lighting_ok;
    r2d_camera camera;
    float dpi;
    target scene_msaa, scene, ping, pong, half_a, half_b;
    int width, height;
    float dt, time;
    r2d_vec2 viewport, motion_px;
    float zoom_ratio;
    r2d_shake_state shake;
    bool focus_set;
    r2d_vec2 focus_screen;
    char config_path[512];
    uint64_t config_stamp;
    float watch_timer;
    r2d_post_stats stats;
    int passes_this_frame;
} P;

/* ---------------------------------------------------------------------------
 * resources
 * ------------------------------------------------------------------------- */
static void destroy_target(target *t) {
    if (t->texture.id) sg_destroy_view(t->texture);
    if (t->attachment.id) sg_destroy_view(t->attachment);
    if (t->resolve.id) sg_destroy_view(t->resolve);
    if (t->image.id) sg_destroy_image(t->image);
    memset(t, 0, sizeof(*t));
}
static bool make_target(target *t, int width, int height, sg_pixel_format format, int sample_count, bool resolve, const char *label) {
    destroy_target(t);
    sg_image_desc img;
    memset(&img, 0, sizeof(img));
    img.width = width;
    img.height = height;
    img.pixel_format = format;
    img.sample_count = sample_count;
    img.usage.color_attachment = !resolve;
    img.usage.resolve_attachment = resolve;
    img.label = label;
    t->image = sg_make_image(&img);
    if (sg_query_image_state(t->image) != SG_RESOURCESTATE_VALID) { destroy_target(t); return false; }
    sg_view_desc view;
    if (resolve) {
        memset(&view, 0, sizeof(view));
        view.resolve_attachment.image = t->image;
        t->resolve = sg_make_view(&view);
    } else {
        memset(&view, 0, sizeof(view));
        view.color_attachment.image = t->image;
        t->attachment = sg_make_view(&view);
    }
    if (sample_count <= 1) {
        memset(&view, 0, sizeof(view));
        view.texture.image = t->image;
        t->texture = sg_make_view(&view);
    }
    t->width = width;
    t->height = height;
    t->sample_count = sample_count;
    t->format = format;
    return true;
}
static void destroy_targets(void) {
    destroy_target(&P.scene_msaa);
    destroy_target(&P.scene);
    destroy_target(&P.ping);
    destroy_target(&P.pong);
    destroy_target(&P.half_a);
    destroy_target(&P.half_b);
    P.width = P.height = 0;
}
static bool make_pipelines(sg_pixel_format offscreen_format) {
    if (P.pipeline_offscreen.id) sg_destroy_pipeline(P.pipeline_offscreen);
    sg_pipeline_desc pip;
    memset(&pip, 0, sizeof(pip));
    pip.shader = P.shader;
    pip.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
    pip.colors[0].pixel_format = offscreen_format;
    pip.depth.pixel_format = SG_PIXELFORMAT_NONE;
    pip.sample_count = 1;
    pip.label = "r2d-post-offscreen";
    P.pipeline_offscreen = sg_make_pipeline(&pip);
    P.pipeline_format = offscreen_format;
    if (!P.pipeline_swapchain.id) {
        pip.colors[0].pixel_format = P.swap_color;
        pip.depth.pixel_format = P.swap_depth;
        pip.depth.write_enabled = false;
        pip.sample_count = P.swap_samples;
        pip.label = "r2d-post-swapchain";
        P.pipeline_swapchain = sg_make_pipeline(&pip);
    }
    return sg_query_pipeline_state(P.pipeline_offscreen) == SG_RESOURCESTATE_VALID
        && sg_query_pipeline_state(P.pipeline_swapchain) == SG_RESOURCESTATE_VALID;
}
static sg_shader make_shader(void) {
    sg_shader_desc d;
    memset(&d, 0, sizeof(d));
    switch (sg_query_backend()) {
        case SG_BACKEND_GLCORE:
            d.vertex_func.source = r2d_post_vs_glsl;
            d.fragment_func.source = r2d_post_fs_glsl;
            break;
        case SG_BACKEND_D3D11:
            d.vertex_func.source = r2d_post_vs_hlsl;
            d.vertex_func.entry = "vs_main";
            d.fragment_func.source = r2d_post_fs_hlsl;
            d.fragment_func.entry = "fs_main";
            break;
        case SG_BACKEND_METAL_MACOS:
        case SG_BACKEND_METAL_IOS:
        case SG_BACKEND_METAL_SIMULATOR:
            d.vertex_func.source = r2d_post_vs_msl;
            d.vertex_func.entry = "vs_main";
            d.fragment_func.source = r2d_post_fs_msl;
            d.fragment_func.entry = "fs_main";
            break;
        default: {
            sg_shader none = {0};
            return none;
        }
    }
    d.attrs[0].glsl_name = "pos";
    d.attrs[0].hlsl_sem_name = "POSITION";
    d.attrs[0].hlsl_sem_index = 0;
    d.attrs[0].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    d.uniform_blocks[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.uniform_blocks[0].size = sizeof(uniforms);
    d.uniform_blocks[0].hlsl_register_b_n = 0;
    d.uniform_blocks[0].msl_buffer_n = 0;
    d.uniform_blocks[0].layout = SG_UNIFORMLAYOUT_STD140;
    d.uniform_blocks[0].glsl_uniforms[0].type = SG_UNIFORMTYPE_FLOAT4;
    d.uniform_blocks[0].glsl_uniforms[0].array_count = 8;
    d.uniform_blocks[0].glsl_uniforms[0].glsl_name = "params";
    for (int i = 0; i < 3; ++i) {
        d.views[i].texture.stage = SG_SHADERSTAGE_FRAGMENT;
        d.views[i].texture.image_type = SG_IMAGETYPE_2D;
        d.views[i].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
        d.views[i].texture.hlsl_register_t_n = (uint8_t)i;
        d.views[i].texture.msl_texture_n = (uint8_t)i;
    }
    d.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
    d.samplers[0].hlsl_register_s_n = 0;
    d.samplers[0].msl_sampler_n = 0;
    d.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.texture_sampler_pairs[0].view_slot = 0;
    d.texture_sampler_pairs[0].sampler_slot = 0;
    d.texture_sampler_pairs[0].glsl_name = "tex0";
    d.texture_sampler_pairs[1].stage = SG_SHADERSTAGE_FRAGMENT;
    d.texture_sampler_pairs[1].view_slot = 1;
    d.texture_sampler_pairs[1].sampler_slot = 0;
    d.texture_sampler_pairs[1].glsl_name = "tex1";
    d.texture_sampler_pairs[2].stage = SG_SHADERSTAGE_FRAGMENT;
    d.texture_sampler_pairs[2].view_slot = 2;
    d.texture_sampler_pairs[2].sampler_slot = 0;
    d.texture_sampler_pairs[2].glsl_name = "tex2";
    d.label = "r2d-post";
    return sg_make_shader(&d);
}

bool r2d_post__create(int swapchain_sample_count, sg_pixel_format swapchain_color, sg_pixel_format swapchain_depth) {
    memset(&P, 0, sizeof(P));
    r2d_post_params_defaults(&P.params);
    P.effective = P.params;
    P.tier = R2D_TIER_HIGH;
    P.swap_samples = swapchain_sample_count > 0 ? swapchain_sample_count : 1;
    P.swap_color = swapchain_color;
    P.swap_depth = swapchain_depth;
    P.origin_top_left = sg_query_features().origin_top_left;
    P.shake.seed = 0x51ed270bu;
    sg_pixelformat_info hdr = sg_query_pixelformat(SG_PIXELFORMAT_RGBA16F);
    P.hdr_supported = hdr.render && hdr.filter && hdr.blend;
    P.hdr_msaa_supported = P.hdr_supported && hdr.msaa;
    P.created = true;

    P.shader = make_shader();
    if (sg_query_shader_state(P.shader) != SG_RESOURCESTATE_VALID) {
        fputs("render2d: post-process shader failed to compile; the stack is disabled\n", stderr);
        return false;
    }
    static const float quad[12] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
    sg_buffer_desc vb;
    memset(&vb, 0, sizeof(vb));
    vb.data.ptr = quad;
    vb.data.size = sizeof(quad);
    vb.label = "r2d-post-quad";
    P.vertices = sg_make_buffer(&vb);
    sg_sampler_desc smp;
    memset(&smp, 0, sizeof(smp));
    smp.min_filter = smp.mag_filter = SG_FILTER_LINEAR;
    smp.wrap_u = smp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    smp.label = "r2d-post-sampler";
    P.sampler = sg_make_sampler(&smp);
    P.scene_format = r2d_post__scene_color_format();
    P.valid = make_pipelines(P.scene_format);
    P.lighting_ok = r2d_lighting__create();
    P.dpi = 1.0f;
    return P.valid;
}
void r2d_post__destroy(void) {
    if (!P.created) return;
    r2d_lighting__destroy();
    destroy_targets();
    if (P.pipeline_offscreen.id) sg_destroy_pipeline(P.pipeline_offscreen);
    if (P.pipeline_swapchain.id) sg_destroy_pipeline(P.pipeline_swapchain);
    if (P.sampler.id) sg_destroy_sampler(P.sampler);
    if (P.vertices.id) sg_destroy_buffer(P.vertices);
    if (P.shader.id) sg_destroy_shader(P.shader);
    memset(&P, 0, sizeof(P));
}
static bool want_hdr(void) {
    return P.effective.post.hdr && (P.swap_samples > 1 ? P.hdr_msaa_supported : P.hdr_supported);
}
/* Decide the tier for this frame and derive the effective parameters. */
static void apply_tier(float dt, int width, int height) {
    const r2d_post_params *p = &P.params;
    r2d_tier lo = (r2d_tier)(p->quality.min_tier < 0 ? 0 : (p->quality.min_tier >= R2D_TIER_COUNT ? R2D_TIER_COUNT - 1 : p->quality.min_tier));
    r2d_tier hi = (r2d_tier)(p->quality.max_tier < 0 ? 0 : (p->quality.max_tier >= R2D_TIER_COUNT ? R2D_TIER_COUNT - 1 : p->quality.max_tier));
    if (lo > hi) { r2d_tier t = lo; lo = hi; hi = t; }
    if (p->quality.tier > 0) {
        P.tier = (r2d_tier)(p->quality.tier - 1 >= R2D_TIER_COUNT ? R2D_TIER_COUNT - 1 : p->quality.tier - 1);
        P.tier_automatic = false;
        /* a later switch to auto continues from the fixed tier */
        r2d_quality_init(&P.controller, P.tier, lo, hi);
        P.controller_ready = true;
    } else {
        if (!P.controller_ready) {
            r2d_quality_init(&P.controller, r2d_quality_pick_initial(P.hdr_supported, width, height), lo, hi);
            P.controller_ready = true;
        }
        P.controller.min_tier = lo;
        P.controller.max_tier = hi;
        if (P.controller.tier < lo) P.controller.tier = lo;
        if (P.controller.tier > hi) P.controller.tier = hi;
        if (p->quality.auto_adjust) r2d_quality_update(&P.controller, dt, p->quality.target_fps);
        P.tier = P.controller.tier;
        P.tier_automatic = true;
    }
    P.effective = P.params;
    r2d_tier_apply(&P.params.tiers[P.tier], &P.effective);
}
sg_pixel_format r2d_post__scene_color_format(void) {
    return want_hdr() ? SG_PIXELFORMAT_RGBA16F : SG_PIXELFORMAT_RGBA8;
}
int r2d_post__scene_sample_count(void) { return P.swap_samples; }
bool r2d_post_active(void) { return P.created && P.valid && P.effective.post.enabled && !P.bypass; }
void r2d_post_set_bypass(bool bypass) { P.bypass = bypass; }
bool r2d_post_bypassed(void) { return P.bypass; }
bool r2d_post__scene_ready(void) { return r2d_post_active() && P.targets_ready; }

/* ---------------------------------------------------------------------------
 * frame
 * ------------------------------------------------------------------------- */
static bool ensure_targets(int width, int height);
void r2d_post__set_motion(const r2d_camera *previous, const r2d_camera *current, r2d_vec2 viewport) {
    if (P.created) r2d_motion_from_cameras(previous, current, viewport, &P.motion_px, &P.zoom_ratio);
}
void r2d_post__set_view(const r2d_camera *camera, r2d_vec2 viewport, float dpi) {
    if (!P.created || !camera) return;
    P.camera = *camera;
    P.viewport = viewport;
    P.dpi = dpi > 0 ? dpi : 1.0f;
}
void r2d_post__begin_frame(float dt, r2d_vec2 viewport, int width, int height) {
    if (!P.created) return;
    P.dt = dt;
    P.time += dt;
    P.viewport = viewport;
    P.frame_ms += ((dt > 0.25f ? 0.25f : dt) * 1000.0f - P.frame_ms) * 0.1f;
    apply_tier(dt, width, height);
    r2d_shake_update(&P.shake, dt, P.effective.shake.decay);
    P.focus_set = false;
    P.passes_this_frame = 0;
    P.targets_ready = false;
    if (P.valid && P.effective.post.enabled && !P.bypass && width > 0 && height > 0) {
        float scale = P.effective.post.render_scale;
        int sw = (int)((float)width * scale + 0.5f), sh = (int)((float)height * scale + 0.5f);
        if (sw < 1) sw = 1;
        if (sh < 1) sh = 1;
        if (ensure_targets(sw, sh)) P.targets_ready = true;
        else { P.valid = false; fputs("render2d: could not create post-process targets; the stack is disabled\n", stderr); }
    }
    if (!P.targets_ready) memset(&P.stats, 0, sizeof(P.stats));
}
static bool ensure_targets(int width, int height) {
    sg_pixel_format format = r2d_post__scene_color_format();
    int div = P.effective.bloom.downscale < 1 ? 1 : P.effective.bloom.downscale;
    int hw = width / div > 0 ? width / div : 1, hh = height / div > 0 ? height / div : 1;
    if (P.width == width && P.height == height && P.scene_format == format && P.half_a.width == hw && P.half_a.height == hh) return true;
    destroy_targets();
    P.scene_format = format;
    if (P.pipeline_format != format && !make_pipelines(format)) return false;
    bool ok = true;
    if (P.swap_samples > 1) {
        ok = ok && make_target(&P.scene_msaa, width, height, format, P.swap_samples, false, "r2d-scene-msaa");
        ok = ok && make_target(&P.scene, width, height, format, 1, true, "r2d-scene");
    } else {
        ok = ok && make_target(&P.scene, width, height, format, 1, false, "r2d-scene");
    }
    ok = ok && make_target(&P.ping, width, height, format, 1, false, "r2d-ping");
    ok = ok && make_target(&P.pong, width, height, format, 1, false, "r2d-pong");
    ok = ok && make_target(&P.half_a, hw, hh, format, 1, false, "r2d-half-a");
    ok = ok && make_target(&P.half_b, hw, hh, format, 1, false, "r2d-half-b");
    if (!ok) { destroy_targets(); return false; }
    P.width = width;
    P.height = height;
    return true;
}
bool r2d_post__begin_scene(sg_color clear) {
    if (!r2d_post_active() || !P.targets_ready) return false;
    sg_pass pass;
    memset(&pass, 0, sizeof(pass));
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value = clear;
    if (P.swap_samples > 1) {
        pass.attachments.colors[0] = P.scene_msaa.attachment;
        pass.attachments.resolves[0] = P.scene.resolve;
        pass.action.colors[0].store_action = SG_STOREACTION_DONTCARE;
    } else {
        pass.attachments.colors[0] = P.scene.attachment;
    }
    pass.label = "r2d-scene";
    sg_begin_pass(&pass);
    P.in_scene = true;
    return true;
}
void r2d_post__end_scene(void) {
    if (!P.in_scene) return;
    sg_end_pass();
    P.in_scene = false;
}

/* --- pass execution --- */
typedef struct { sg_view view; int width, height; } input;
static input input_of(const target *t) { input i = {t->texture, t->width, t->height}; return i; }
static void draw_fullscreen(bool to_swapchain, input src, sg_view aux, sg_view aux2, uniforms *u) {
    u->v[0][1] = src.width > 0 ? 1.0f / (float)src.width : 0;
    u->v[0][2] = src.height > 0 ? 1.0f / (float)src.height : 0;
    u->v[0][3] = P.origin_top_left ? 1.0f : 0.0f;
    sg_apply_pipeline(to_swapchain ? P.pipeline_swapchain : P.pipeline_offscreen);
    sg_bindings bind;
    memset(&bind, 0, sizeof(bind));
    bind.vertex_buffers[0] = P.vertices;
    bind.views[0] = src.view;
    bind.views[1] = aux.id ? aux : src.view;
    bind.views[2] = aux2.id ? aux2 : bind.views[1];
    bind.samplers[0] = P.sampler;
    sg_apply_bindings(&bind);
    sg_range range = {u, sizeof(*u)};
    sg_apply_uniforms(0, &range);
    sg_draw(0, 6, 1);
    ++P.passes_this_frame;
}
static void run_offscreen(target *dst, input src, sg_view aux, sg_view aux2, uniforms *u) {
    sg_pass pass;
    memset(&pass, 0, sizeof(pass));
    pass.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
    pass.attachments.colors[0] = dst->attachment;
    sg_begin_pass(&pass);
    draw_fullscreen(false, src, aux, aux2, u);
    sg_end_pass();
}
static void run_simple(target *dst, const target *src, int kind, const float p1[4]) {
    uniforms u;
    memset(&u, 0, sizeof(u));
    u.v[0][0] = (float)kind;
    if (p1) memcpy(u.v[1], p1, sizeof(u.v[1]));
    sg_view none = {0};
    run_offscreen(dst, input_of(src), none, none, &u);
}
/* Blur half_a in place through half_b, `passes` times with growing radius. */
static void blur_half(float radius, int passes) {
    for (int i = 0; i < passes; ++i) {
        float r = radius * (float)(i + 1);
        float h[4] = {r, 0, 0, 0}, v[4] = {0, r, 0, 0};
        run_simple(&P.half_b, &P.half_a, KIND_BLUR, h);
        run_simple(&P.half_a, &P.half_b, KIND_BLUR, v);
    }
}

static bool pass_enabled(r2d_pass pass) {
    const r2d_post_params *p = &P.effective;
    switch (pass) {
        case R2D_PASS_LIGHTING: return p->lighting.enabled && P.lighting_ok;
        case R2D_PASS_BLOOM: return p->bloom.enabled && p->bloom.intensity > 0;
        case R2D_PASS_DOF: return p->dof.enabled && p->dof.max_blur > 0;
        case R2D_PASS_MOTION_BLUR: {
            if (!p->motion_blur.enabled) return false;
            float len = r2d_length(P.motion_px) * p->motion_blur.strength;
            float zoom = fabsf(P.zoom_ratio - 1.0f) * p->motion_blur.zoom_strength;
            return len > 0.5f || zoom > 0.002f;
        }
        case R2D_PASS_CHROMATIC_ABERRATION: return p->chromatic_aberration.enabled && p->chromatic_aberration.strength > 0;
        case R2D_PASS_TONEMAP: return p->tonemap.enabled;
        case R2D_PASS_VIGNETTE: return p->vignette.enabled && p->vignette.intensity > 0;
        case R2D_PASS_SHAKE: return p->shake.enabled && P.shake.trauma > 0;
        default: return false;
    }
}
static float uv_y(float screen_fraction_from_top) { return P.origin_top_left ? screen_fraction_from_top : 1.0f - screen_fraction_from_top; }

void r2d_post__run(sg_swapchain swapchain) {
    sg_pass final;
    memset(&final, 0, sizeof(final));
    final.swapchain = swapchain;
    final.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
    final.label = "r2d-present";
    if (!r2d_post_active() || !P.targets_ready) {
        /* nothing was rendered offscreen: give the caller a cleared window pass */
        final.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        sg_begin_pass(&final);
        return;
    }
    const r2d_post_params *p = &P.effective;
    r2d_pass order[R2D_PASS_COUNT];
    size_t n = r2d_post_parse_order(p->stack.order, order, R2D_PASS_COUNT, NULL);
    r2d_pass active[R2D_PASS_COUNT];
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) if (pass_enabled(order[i])) active[count++] = order[i];

    float aspect = P.height > 0 ? (float)P.width / (float)P.height : 1.0f;
    const target *cur = &P.scene;
    target *free_slot = &P.ping;
    sg_view none = {0};
    for (size_t i = 0; i < count; ++i) {
        bool last = i + 1 == count;
        target *dst = free_slot;
        sg_view aux = none, aux2 = none;
        uniforms u;
        memset(&u, 0, sizeof(u));
        float *p1 = u.v[1], *p2 = u.v[2];
        int kind = KIND_BLIT;
        switch (active[i]) {
            case R2D_PASS_LIGHTING: {
                r2d_lightmap lm;
                if (!r2d_lighting__render(p, P.scene_format, P.width, P.height, &P.camera, P.viewport, P.dpi, &lm)) {
                    P.lighting_ok = false;
                    break;   /* a plain blit keeps the chain intact */
                }
                kind = KIND_LIGHTING;
                aux = lm.direct;
                aux2 = lm.volume;
                p1[0] = P.camera.center.x; p1[1] = P.camera.center.y; p1[2] = P.camera.zoom; p1[3] = P.camera.y_up ? 1.0f : 0.0f;
                p2[0] = P.viewport.x; p2[1] = P.viewport.y; p2[2] = P.time; p2[3] = p->fog.scale;
                u.v[3][0] = p->fog.enabled ? p->fog.density : 0.0f; u.v[3][1] = p->fog.contrast;
                u.v[3][2] = p->fog.speed_x; u.v[3][3] = p->fog.speed_y;
                u.v[4][0] = p->fog.color_r; u.v[4][1] = p->fog.color_g; u.v[4][2] = p->fog.color_b; u.v[4][3] = p->fog.ambient;
                u.v[5][0] = (float)p->fog.octaves; u.v[5][1] = p->fog.direct; u.v[5][2] = p->lighting.haze;
                u.v[5][3] = p->fog.enabled ? 1.0f : 0.0f;
                break;
            }
            case R2D_PASS_BLOOM: {
                float t[4] = {p->bloom.threshold, p->bloom.soft_knee, 0, 0};
                run_simple(&P.half_a, cur, KIND_THRESHOLD, t);
                blur_half(p->bloom.radius, p->bloom.passes);
                kind = KIND_BLOOM_COMBINE;
                aux = P.half_a.texture;
                p1[0] = p->bloom.intensity;
                break;
            }
            case R2D_PASS_DOF: {
                run_simple(&P.half_a, cur, KIND_BLIT, NULL);
                blur_half(p->dof.blur_radius, p->dof.blur_passes);
                kind = KIND_DOF_COMBINE;
                aux = P.half_a.texture;
                float fx = p->dof.focus_x, fy = p->dof.focus_y;
                if (P.focus_set && P.viewport.x > 0 && P.viewport.y > 0) {
                    fx = P.focus_screen.x / P.viewport.x;
                    fy = P.focus_screen.y / P.viewport.y;
                }
                p1[0] = fx; p1[1] = fy; p1[2] = p->dof.focus_radius; p1[3] = p->dof.falloff;
                p2[0] = p->dof.max_blur; p2[1] = (float)p->dof.shape; p2[2] = aspect;
                break;
            }
            case R2D_PASS_MOTION_BLUR: {
                kind = KIND_MOTION_BLUR;
                r2d_vec2 v = r2d_scale(P.motion_px, p->motion_blur.strength);
                float len = r2d_length(v);
                if (len > p->motion_blur.max_offset && len > 0) v = r2d_scale(v, p->motion_blur.max_offset / len);
                p1[0] = P.width > 0 ? v.x / (float)P.width : 0;
                p1[1] = P.height > 0 ? v.y / (float)P.height : 0;
                p1[2] = (float)p->motion_blur.samples;
                p1[3] = (P.zoom_ratio - 1.0f) * p->motion_blur.zoom_strength;
                p2[0] = 0.5f; p2[1] = 0.5f;
                break;
            }
            case R2D_PASS_CHROMATIC_ABERRATION:
                kind = KIND_CHROMATIC;
                p1[0] = p->chromatic_aberration.strength;
                p1[1] = p->chromatic_aberration.center_x;
                p1[2] = uv_y(p->chromatic_aberration.center_y);
                p1[3] = p->chromatic_aberration.falloff;
                break;
            case R2D_PASS_TONEMAP:
                kind = KIND_TONEMAP;
                p1[0] = (float)p->tonemap.op; p1[1] = p->tonemap.exposure; p1[2] = p->tonemap.gamma; p1[3] = p->tonemap.saturation;
                p2[0] = p->tonemap.contrast; p2[1] = p->tonemap.brightness;
                break;
            case R2D_PASS_VIGNETTE:
                kind = KIND_VIGNETTE;
                p1[0] = p->vignette.radius; p1[1] = p->vignette.softness; p1[2] = p->vignette.intensity; p1[3] = p->vignette.roundness;
                p2[0] = p->vignette.color_r; p2[1] = p->vignette.color_g; p2[2] = p->vignette.color_b; p2[3] = aspect;
                break;
            case R2D_PASS_SHAKE: {
                kind = KIND_SHAKE;
                r2d_vec2 off;
                float angle;
                r2d_shake_sample(&P.shake, p->shake.max_offset, p->shake.max_angle * 3.14159265f / 180.0f,
                                 p->shake.frequency, p->shake.exponent, &off, &angle);
                p1[0] = P.width > 0 ? off.x / (float)P.width : 0;
                p1[1] = (P.height > 0 ? off.y / (float)P.height : 0) * (P.origin_top_left ? 1.0f : -1.0f);
                p1[2] = angle;
                float edge = 2.0f * (fabsf(p1[0]) + fabsf(p1[1])) + fabsf(angle) * 0.6f * (aspect > 1 ? aspect : 1.0f / aspect);
                p1[3] = 1.0f + edge;
                break;
            }
            default: break;
        }
        u.v[0][0] = (float)kind;
        if (last) {
            sg_begin_pass(&final);
            draw_fullscreen(true, input_of(cur), aux, aux2, &u);
        } else {
            run_offscreen(dst, input_of(cur), aux, aux2, &u);
            cur = dst;
            free_slot = dst == &P.ping ? &P.pong : &P.ping;
        }
    }
    if (!count) {
        uniforms u;
        memset(&u, 0, sizeof(u));
        sg_begin_pass(&final);
        draw_fullscreen(true, input_of(cur), none, none, &u);
    }
    P.stats.active = true;
    P.stats.hdr = P.scene_format == SG_PIXELFORMAT_RGBA16F;
    P.stats.passes_run = P.passes_this_frame;
    P.stats.width = P.width;
    P.stats.height = P.height;
    P.stats.motion_px = P.motion_px;
    P.stats.shake_trauma = P.shake.trauma;
    P.stats.tier = P.tier;
    P.stats.tier_automatic = P.tier_automatic;
    P.stats.frame_ms = P.frame_ms;
    P.stats.tier_changes = P.controller.changes;
}

/* ---------------------------------------------------------------------------
 * public API
 * ------------------------------------------------------------------------- */
r2d_post_params *r2d_post_params_ptr(void) { return P.created ? &P.params : NULL; }
bool r2d_post_load_config(const char *path, char *error, size_t error_capacity) {
    if (error && error_capacity) error[0] = 0;
    if (!P.created || !path) return false;
    r2d_post_params fresh;
    r2d_post_params_defaults(&fresh);
    r2d_tunable_set fresh_set = r2d_post_tunable_set(&fresh);
    if (!r2d_tunables_load(&fresh_set, path, error, error_capacity)) return false;
    P.params = fresh;
    P.effective = fresh;
    r2d_tier_apply(&P.params.tiers[P.tier], &P.effective);
    snprintf(P.config_path, sizeof(P.config_path), "%s", path);
    P.config_stamp = 0;
    r2d_tunables_file_changed(P.config_path, &P.config_stamp);
    return true;
}
bool r2d_post_save_config(const char *path) {
    if (!P.created) return false;
    if (!path) path = P.config_path;
    if (!*path) return false;
    r2d_tunable_set set = r2d_post_tunable_set(&P.params);
    bool ok = r2d_tunables_save(&set, path, "render2d post-process tunables (edit while the program runs; it reloads automatically)");
    if (ok && strcmp(path, P.config_path) == 0) r2d_tunables_file_changed(P.config_path, &P.config_stamp);
    return ok;
}
bool r2d_post_reload_if_changed(void) {
    if (!P.created || !P.config_path[0]) return false;
    P.watch_timer += P.dt;
    if (P.watch_timer < 0.5f) return false;
    P.watch_timer = 0;
    if (!r2d_tunables_file_changed(P.config_path, &P.config_stamp)) return false;
    char error[256];
    r2d_post_params fresh;
    r2d_post_params_defaults(&fresh);
    r2d_tunable_set fresh_set = r2d_post_tunable_set(&fresh);
    if (!r2d_tunables_load(&fresh_set, P.config_path, error, sizeof(error))) return false;
    P.params = fresh;
    return true;
}
const char *r2d_post_config_path(void) { return P.config_path; }
void r2d_post_shake(float trauma) { if (P.created) r2d_shake_add(&P.shake, trauma); }
float r2d_post_shake_trauma(void) { return P.created ? P.shake.trauma : 0; }
void r2d_post_set_focus(r2d_vec2 screen_point) { P.focus_set = true; P.focus_screen = screen_point; }
void r2d_post_set_focus_world(r2d_vec2 world_point) { r2d_post_set_focus(r2d_world_to_screen(world_point)); }
void r2d_post_clear_focus(void) { P.focus_set = false; }
r2d_post_stats r2d_post_get_stats(void) { return P.stats; }
const r2d_post_params *r2d_post_effective_params(void) { return P.created ? &P.effective : NULL; }
r2d_tier r2d_post_current_tier(void) { return P.created ? P.tier : R2D_TIER_HIGH; }
void r2d_post_set_tier(int mode) {
    if (!P.created) return;
    P.params.quality.tier = mode < 0 ? 0 : (mode > R2D_TIER_COUNT ? R2D_TIER_COUNT : mode);
}
float r2d_post_particle_scale(void) { return P.created ? P.params.tiers[P.tier].particle_scale : 1.0f; }
