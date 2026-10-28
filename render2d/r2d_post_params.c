/* Pure part of the post-process stack: the parameter table, order parsing,
 * camera shake and camera-motion helpers. No graphics backend needed. */
#include "r2d_post.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *pass_names[R2D_PASS_COUNT] = {
    "lighting", "bloom", "dof", "motion_blur", "chromatic_aberration", "tonemap", "vignette", "shake"
};
const char *r2d_pass_name(r2d_pass pass) { return pass >= 0 && pass < R2D_PASS_COUNT ? pass_names[pass] : "?"; }

#define F(sec, name, field, def, min, max, step, help) \
    {sec, name, R2D_TUNE_FLOAT, offsetof(r2d_post_params, field), def, min, max, step, NULL, 0, help}
#define I(sec, name, field, def, min, max, help) \
    {sec, name, R2D_TUNE_INT, offsetof(r2d_post_params, field), def, min, max, 1, NULL, 0, help}
#define B(sec, name, field, def, help) \
    {sec, name, R2D_TUNE_BOOL, offsetof(r2d_post_params, field), def, 0, 0, 0, NULL, 0, help}
#define E(sec, name, field, def, choices, help) \
    {sec, name, R2D_TUNE_ENUM, offsetof(r2d_post_params, field), def, 0, 0, 0, choices, 0, help}
#define S(sec, name, field, help) \
    {sec, name, R2D_TUNE_STRING, offsetof(r2d_post_params, field), 0, 0, 0, 0, NULL, sizeof(((r2d_post_params *)0)->field), help}

static const r2d_tunable defs[] = {
    B("post", "enabled", post.enabled, 1, "run the post-process stack (off draws straight to the window)"),
    B("post", "hdr", post.hdr, 1, "render the scene into RGBA16F targets so bloom can exceed 1.0"),
    B("post", "apply_to_ui", post.apply_to_ui, 0, "post-process the screen-space UI layer as well"),
    F("post", "render_scale", post.render_scale, 1.0f, 0.25f, 1, 0.05f, "scene resolution relative to the window (tiers may lower it)"),

    E("quality", "tier", quality.tier, 0, "auto|low|medium|high|ultra", "device tier, or auto: pick from the device and adjust from frame times"),
    B("quality", "auto_adjust", quality.auto_adjust, 1, "with auto: lower the tier when over budget, raise it when there is headroom"),
    F("quality", "target_fps", quality.target_fps, 60, 15, 240, 5, "frame rate the automatic adjustment aims for"),
    E("quality", "min_tier", quality.min_tier, 0, "low|medium|high|ultra", "lowest tier the automatic adjustment may choose"),
    E("quality", "max_tier", quality.max_tier, 3, "low|medium|high|ultra", "highest tier the automatic adjustment may choose"),

    S("stack", "order", stack.order, "passes to run, first runs first: lighting, bloom, dof, motion_blur, chromatic_aberration, tonemap, vignette, shake"),

    B("lighting", "enabled", lighting.enabled, 1, "2D lights with soft shadows and volumetric in-scatter (needs the stack)"),
    F("lighting", "ambient_r", lighting.ambient_r, 0.55f, 0, 2, 0.05f, "ambient light color red"),
    F("lighting", "ambient_g", lighting.ambient_g, 0.6f, 0, 2, 0.05f, "ambient light color green"),
    F("lighting", "ambient_b", lighting.ambient_b, 0.75f, 0, 2, 0.05f, "ambient light color blue"),
    F("lighting", "ambient", lighting.ambient, 0.32f, 0, 3, 0.05f, "ambient intensity (1 with white color = unlit look)"),
    F("lighting", "intensity", lighting.intensity, 1.0f, 0, 5, 0.1f, "multiplier for every light"),
    F("lighting", "volumetric", lighting.volumetric, 1.0f, 0, 5, 0.1f, "multiplier for the in-scattered (shaft) light"),
    F("lighting", "haze", lighting.haze, 0.18f, 0, 1, 0.02f, "volumetric light visible even where there is no fog"),
    F("lighting", "shadow_softness", lighting.shadow_softness, 1.5f, 0, 8, 0.25f, "penumbra width in shadow map steps"),
    I("lighting", "shadow_angles", lighting.shadow_angles, 720, 64, 4096, "polar shadow map resolution per light"),
    I("lighting", "downscale", lighting.downscale, 2, 1, 8, "resolution divisor of the lightmap"),
    I("lighting", "max_lights", lighting.max_lights, 64, 1, 64, "lights rendered per frame (the first submitted win)"),
    F("lighting", "falloff", lighting.falloff, 2.0f, 0.25f, 6, 0.25f, "default falloff exponent of (1 - distance / radius)"),

    B("fog", "enabled", fog.enabled, 1, "animated fog lit by the lights"),
    F("fog", "density", fog.density, 0.3f, 0, 1, 0.05f, "overall fog amount"),
    F("fog", "color_r", fog.color_r, 0.62f, 0, 2, 0.05f, "fog color red"),
    F("fog", "color_g", fog.color_g, 0.66f, 0, 2, 0.05f, "fog color green"),
    F("fog", "color_b", fog.color_b, 0.74f, 0, 2, 0.05f, "fog color blue"),
    F("fog", "scale", fog.scale, 220, 10, 2000, 10, "size of the noise features in world units"),
    F("fog", "speed_x", fog.speed_x, 18, -200, 200, 2, "wind, world units per second"),
    F("fog", "speed_y", fog.speed_y, 6, -200, 200, 2, "wind, world units per second"),
    F("fog", "contrast", fog.contrast, 2.0f, 0.2f, 6, 0.1f, "sharpens the noise (1 = raw)"),
    I("fog", "octaves", fog.octaves, 4, 1, 6, "noise detail levels"),
    F("fog", "ambient", fog.ambient, 0.25f, 0, 3, 0.05f, "ambient light received by the fog"),
    F("fog", "direct", fog.direct, 0.6f, 0, 3, 0.05f, "direct light received by the fog"),

    B("bloom", "enabled", bloom.enabled, 1, "glow around bright pixels"),
    F("bloom", "threshold", bloom.threshold, 0.75f, 0, 4, 0.05f, "brightness where pixels start to bloom"),
    F("bloom", "soft_knee", bloom.soft_knee, 0.5f, 0, 1, 0.05f, "softness of the threshold transition"),
    F("bloom", "intensity", bloom.intensity, 0.9f, 0, 5, 0.05f, "strength of the added glow"),
    F("bloom", "radius", bloom.radius, 1.6f, 0.25f, 8, 0.25f, "blur radius in texels of the downscaled image"),
    I("bloom", "passes", bloom.passes, 3, 1, 8, "blur iterations (wider glow per pass)"),
    I("bloom", "downscale", bloom.downscale, 2, 1, 8, "resolution divisor of the bloom buffers"),

    B("dof", "enabled", dof.enabled, 0, "depth of field: blur away from a focus point or band"),
    E("dof", "shape", dof.shape, 0, "radial|horizontal_band|vertical_band", "focus region shape (bands give a tilt-shift look)"),
    F("dof", "focus_x", dof.focus_x, 0.5f, 0, 1, 0.05f, "focus center, fraction of the screen width from the left"),
    F("dof", "focus_y", dof.focus_y, 0.5f, 0, 1, 0.05f, "focus center, fraction of the screen height from the top"),
    F("dof", "focus_radius", dof.focus_radius, 0.2f, 0, 1.5f, 0.02f, "radius of the sharp region (screen fractions)"),
    F("dof", "falloff", dof.falloff, 0.35f, 0.01f, 1.5f, 0.02f, "distance over which sharp turns into blurred"),
    F("dof", "blur_radius", dof.blur_radius, 2.0f, 0.25f, 8, 0.25f, "blur radius in texels of the half-resolution copy"),
    I("dof", "blur_passes", dof.blur_passes, 2, 1, 6, "blur iterations"),
    F("dof", "max_blur", dof.max_blur, 1.0f, 0, 1, 0.05f, "how much of the blurred copy is mixed in at full distance"),

    B("motion_blur", "enabled", motion_blur.enabled, 1, "blur along the camera's motion between frames"),
    F("motion_blur", "strength", motion_blur.strength, 0.6f, 0, 4, 0.1f, "fraction of the per-frame camera movement to smear over"),
    F("motion_blur", "max_offset", motion_blur.max_offset, 48, 0, 400, 4, "longest smear in pixels"),
    F("motion_blur", "zoom_strength", motion_blur.zoom_strength, 4.0f, 0, 20, 0.5f, "radial blur per unit of zoom change"),
    I("motion_blur", "samples", motion_blur.samples, 8, 2, 32, "taps along the motion vector"),

    B("chromatic_aberration", "enabled", chromatic_aberration.enabled, 1, "separate the color channels towards the edges"),
    F("chromatic_aberration", "strength", chromatic_aberration.strength, 0.006f, 0, 0.05f, 0.001f, "channel offset at the edge, in screen fractions"),
    F("chromatic_aberration", "center_x", chromatic_aberration.center_x, 0.5f, 0, 1, 0.05f, "center of the effect, fraction from the left"),
    F("chromatic_aberration", "center_y", chromatic_aberration.center_y, 0.5f, 0, 1, 0.05f, "center of the effect, fraction from the top"),
    F("chromatic_aberration", "falloff", chromatic_aberration.falloff, 1.5f, 0.1f, 6, 0.1f, "exponent of the growth with distance from the center"),

    B("tonemap", "enabled", tonemap.enabled, 1, "exposure, tone curve and color grading"),
    E("tonemap", "operator", tonemap.op, 2, "none|reinhard|aces|filmic", "tone curve"),
    F("tonemap", "exposure", tonemap.exposure, 1.0f, 0, 8, 0.05f, "multiplier before the tone curve"),
    F("tonemap", "gamma", tonemap.gamma, 1.0f, 0.2f, 4, 0.05f, "output gamma (1 = unchanged)"),
    F("tonemap", "saturation", tonemap.saturation, 1.15f, 0, 3, 0.05f, "0 = grayscale, 1 = unchanged"),
    F("tonemap", "contrast", tonemap.contrast, 1.12f, 0, 3, 0.05f, "contrast around mid gray"),
    F("tonemap", "brightness", tonemap.brightness, 0.0f, -1, 1, 0.02f, "added after contrast"),

    B("vignette", "enabled", vignette.enabled, 1, "darken towards the edges"),
    F("vignette", "radius", vignette.radius, 1.25f, 0, 2, 0.05f, "distance from the center where the vignette is fully applied"),
    F("vignette", "softness", vignette.softness, 0.7f, 0.01f, 2, 0.05f, "width of the transition"),
    F("vignette", "intensity", vignette.intensity, 0.55f, 0, 1, 0.05f, "how strongly the edges are tinted"),
    F("vignette", "roundness", vignette.roundness, 0.5f, 0, 1, 0.1f, "0 follows the window shape, 1 is circular"),
    F("vignette", "color_r", vignette.color_r, 0, 0, 1, 0.05f, "tint color red"),
    F("vignette", "color_g", vignette.color_g, 0, 0, 1, 0.05f, "tint color green"),
    F("vignette", "color_b", vignette.color_b, 0.02f, 0, 1, 0.05f, "tint color blue"),

    B("shake", "enabled", shake.enabled, 1, "camera shake applied to the final image from accumulated trauma"),
    F("shake", "max_offset", shake.max_offset, 18, 0, 200, 1, "largest offset in pixels at full trauma"),
    F("shake", "max_angle", shake.max_angle, 1.5f, 0, 20, 0.1f, "largest rotation in degrees at full trauma"),
    F("shake", "frequency", shake.frequency, 24, 1, 120, 1, "shake speed in hertz"),
    F("shake", "decay", shake.decay, 1.8f, 0.1f, 20, 0.1f, "trauma lost per second"),
    F("shake", "exponent", shake.exponent, 2.0f, 1, 4, 0.25f, "shake = trauma ^ exponent (higher = more contrast between small and big hits)"),

#define TIER(sec, i, rs, hdr_, msaa_, bp, bd, dof_, dp, mb, ms, li, fo, ld, sa, ml, oc, ps) \
    F(sec, "render_scale", tiers[i].render_scale, rs, 0.25f, 1, 0.05f, "scene resolution relative to the window"), \
    B(sec, "hdr", tiers[i].hdr, hdr_, "RGBA16F targets allowed"), \
    I(sec, "msaa", tiers[i].msaa, msaa_, 1, 8, "window sample count to request at startup"), \
    I(sec, "bloom_passes", tiers[i].bloom_passes, bp, 1, 8, "bloom blur iterations at most"), \
    I(sec, "bloom_downscale", tiers[i].bloom_downscale, bd, 1, 8, "bloom buffers at least this much smaller"), \
    B(sec, "allow_dof", tiers[i].allow_dof, dof_, "depth of field allowed"), \
    I(sec, "dof_passes", tiers[i].dof_passes, dp, 1, 6, "depth of field blur iterations at most"), \
    B(sec, "allow_motion_blur", tiers[i].allow_motion_blur, mb, "motion blur allowed"), \
    I(sec, "motion_samples", tiers[i].motion_samples, ms, 2, 32, "motion blur taps at most"), \
    B(sec, "allow_lighting", tiers[i].allow_lighting, li, "lights and shadows allowed"), \
    B(sec, "allow_fog", tiers[i].allow_fog, fo, "fog allowed"), \
    I(sec, "lighting_downscale", tiers[i].lighting_downscale, ld, 1, 8, "lightmap at least this much smaller"), \
    I(sec, "shadow_angles", tiers[i].shadow_angles, sa, 64, 4096, "shadow map resolution at most"), \
    I(sec, "max_lights", tiers[i].max_lights, ml, 1, 64, "lights per frame at most"), \
    I(sec, "fog_octaves", tiers[i].fog_octaves, oc, 1, 6, "fog noise octaves at most"), \
    F(sec, "particle_scale", tiers[i].particle_scale, ps, 0, 4, 0.05f, "multiplier for particle counts and emitter rates")
    TIER("tier_low", R2D_TIER_LOW, 0.7f, 0, 1, 1, 4, 0, 1, 0, 4, 1, 0, 4, 180, 12, 2, 0.35f),
    TIER("tier_medium", R2D_TIER_MEDIUM, 0.85f, 1, 2, 2, 4, 1, 1, 1, 6, 1, 1, 3, 360, 24, 3, 0.6f),
    TIER("tier_high", R2D_TIER_HIGH, 1.0f, 1, 4, 3, 2, 1, 2, 1, 8, 1, 1, 2, 720, 48, 4, 1.0f),
    TIER("tier_ultra", R2D_TIER_ULTRA, 1.0f, 1, 8, 4, 2, 1, 3, 1, 16, 1, 1, 1, 1440, 64, 6, 1.5f),
#undef TIER
};
#undef F
#undef I
#undef B
#undef E
#undef S

const r2d_tunable *r2d_post_tunable_defs(size_t *count) {
    if (count) *count = sizeof(defs) / sizeof(defs[0]);
    return defs;
}
r2d_tunable_set r2d_post_tunable_set(r2d_post_params *params) {
    r2d_tunable_set set = {defs, sizeof(defs) / sizeof(defs[0]), params};
    return set;
}
void r2d_post_params_defaults(r2d_post_params *p) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
    r2d_tunable_set set = r2d_post_tunable_set(p);
    r2d_tunables_reset(&set);
    snprintf(p->stack.order, sizeof(p->stack.order), "lighting, bloom, dof, motion_blur, chromatic_aberration, tonemap, vignette, shake");
}

static int min_i(int a, int b) { return a < b ? a : b; }
static int max_i(int a, int b) { return a > b ? a : b; }
void r2d_tier_apply(const r2d_tier_settings *t, r2d_post_params *p) {
    if (!t || !p) return;
    if (p->post.render_scale > t->render_scale) p->post.render_scale = t->render_scale;
    if (p->post.render_scale < 0.25f) p->post.render_scale = 0.25f;
    p->post.hdr = p->post.hdr && t->hdr;
    p->bloom.passes = max_i(1, min_i(p->bloom.passes, t->bloom_passes));
    p->bloom.downscale = max_i(p->bloom.downscale, t->bloom_downscale);
    p->dof.enabled = p->dof.enabled && t->allow_dof;
    p->dof.blur_passes = max_i(1, min_i(p->dof.blur_passes, t->dof_passes));
    p->motion_blur.enabled = p->motion_blur.enabled && t->allow_motion_blur;
    p->motion_blur.samples = max_i(2, min_i(p->motion_blur.samples, t->motion_samples));
    p->lighting.enabled = p->lighting.enabled && t->allow_lighting;
    p->fog.enabled = p->fog.enabled && t->allow_fog;
    p->lighting.downscale = max_i(p->lighting.downscale, t->lighting_downscale);
    p->lighting.shadow_angles = max_i(64, min_i(p->lighting.shadow_angles, t->shadow_angles));
    p->lighting.max_lights = max_i(1, min_i(p->lighting.max_lights, t->max_lights));
    p->fog.octaves = max_i(1, min_i(p->fog.octaves, t->fog_octaves));
}

size_t r2d_post_parse_order(const char *order, r2d_pass *out, size_t capacity, size_t *unknown) {
    size_t n = 0, bad = 0;
    if (unknown) *unknown = 0;
    if (!order) return 0;
    const char *p = order;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) ++p;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',' && !isspace((unsigned char)*p)) ++p;
        size_t len = (size_t)(p - start);
        int found = -1;
        for (int i = 0; i < R2D_PASS_COUNT; ++i)
            if (strlen(pass_names[i]) == len && strncmp(pass_names[i], start, len) == 0) { found = i; break; }
        if (found < 0) { ++bad; continue; }
        bool duplicate = false;
        for (size_t i = 0; i < n; ++i) if (out && out[i] == (r2d_pass)found) duplicate = true;
        if (duplicate) continue;
        if (out && n < capacity) out[n] = (r2d_pass)found;
        ++n;
    }
    if (unknown) *unknown = bad;
    return n < capacity ? n : capacity;
}
size_t r2d_post_format_order(const r2d_pass *passes, size_t count, char *out, size_t capacity) {
    if (!out || !capacity) return 0;
    out[0] = 0;
    size_t used = 0;
    for (size_t i = 0; i < count && used < capacity; ++i) {
        int n = snprintf(out + used, capacity - used, "%s%s", i ? ", " : "", r2d_pass_name(passes[i]));
        if (n > 0) used += (size_t)n;
    }
    return used < capacity ? used : capacity - 1;
}

/* --- shake --- */
static float hash_noise(uint32_t seed, float t) {
    /* smooth value noise: interpolate hashed lattice values */
    float fl = floorf(t), fr = t - fl;
    uint32_t i = (uint32_t)(int32_t)fl;
    uint32_t a = (i * 747796405u + seed) ^ 0x9e3779b9u, b = ((i + 1) * 747796405u + seed) ^ 0x9e3779b9u;
    a ^= a >> 16; a *= 0x7feb352du; a ^= a >> 15;
    b ^= b >> 16; b *= 0x7feb352du; b ^= b >> 15;
    float va = (float)(a & 0xffff) / 32767.5f - 1.0f, vb = (float)(b & 0xffff) / 32767.5f - 1.0f;
    float s = fr * fr * (3.0f - 2.0f * fr);
    return va + (vb - va) * s;
}
void r2d_shake_add(r2d_shake_state *s, float trauma) {
    if (!s || !(trauma > 0)) return;
    s->trauma = r2d_clampf(s->trauma + trauma, 0, 1);
}
void r2d_shake_update(r2d_shake_state *s, float dt, float decay) {
    if (!s || !(dt > 0)) return;
    s->time += dt;
    s->trauma = r2d_clampf(s->trauma - decay * dt, 0, 1);
}
void r2d_shake_sample(const r2d_shake_state *s, float max_offset, float max_angle, float frequency, float exponent,
                      r2d_vec2 *offset_px, float *angle) {
    r2d_vec2 off = r2d_v2(0, 0);
    float ang = 0;
    if (s && s->trauma > 0) {
        float amount = powf(s->trauma, exponent > 0 ? exponent : 1.0f);
        float t = s->time * (frequency > 0 ? frequency : 1.0f);
        off.x = max_offset * amount * hash_noise(s->seed + 1, t);
        off.y = max_offset * amount * hash_noise(s->seed + 2, t);
        ang = max_angle * amount * hash_noise(s->seed + 3, t);
    }
    if (offset_px) *offset_px = off;
    if (angle) *angle = ang;
}

/* --- camera motion --- */
void r2d_motion_from_cameras(const r2d_camera *previous, const r2d_camera *current, r2d_vec2 viewport,
                             r2d_vec2 *delta_px, float *zoom_ratio) {
    r2d_vec2 d = r2d_v2(0, 0);
    float ratio = 1.0f;
    if (previous && current && previous->zoom > 0 && current->zoom > 0) {
        /* where the previously centered world point is now on screen */
        r2d_vec2 now = r2d_camera_world_to_screen(current, viewport, previous->center);
        d = r2d_sub(now, r2d_scale(viewport, 0.5f));
        ratio = current->zoom / previous->zoom;
    }
    if (delta_px) *delta_px = d;
    if (zoom_ratio) *zoom_ratio = ratio;
}
