#include "r2d_quality.h"
#include <string.h>

const char *r2d_tier_name(r2d_tier tier) {
    static const char *names[R2D_TIER_COUNT] = {"low", "medium", "high", "ultra"};
    return tier >= 0 && tier < R2D_TIER_COUNT ? names[tier] : "?";
}

void r2d_tier_defaults(r2d_tier tier, r2d_tier_settings *t) {
    if (!t) return;
    memset(t, 0, sizeof(*t));
    switch (tier) {
        case R2D_TIER_LOW:
            t->render_scale = 0.7f; t->hdr = false; t->msaa = 1;
            t->bloom_passes = 1; t->bloom_downscale = 4;
            t->allow_dof = false; t->dof_passes = 1;
            t->allow_motion_blur = false; t->motion_samples = 4;
            t->allow_lighting = true; t->allow_fog = false;
            t->lighting_downscale = 4; t->shadow_angles = 180; t->max_lights = 12; t->fog_octaves = 2;
            t->particle_scale = 0.35f;
            break;
        case R2D_TIER_MEDIUM:
            t->render_scale = 0.85f; t->hdr = true; t->msaa = 2;
            t->bloom_passes = 2; t->bloom_downscale = 4;
            t->allow_dof = true; t->dof_passes = 1;
            t->allow_motion_blur = true; t->motion_samples = 6;
            t->allow_lighting = true; t->allow_fog = true;
            t->lighting_downscale = 3; t->shadow_angles = 360; t->max_lights = 24; t->fog_octaves = 3;
            t->particle_scale = 0.6f;
            break;
        case R2D_TIER_ULTRA:
            t->render_scale = 1.0f; t->hdr = true; t->msaa = 8;
            t->bloom_passes = 4; t->bloom_downscale = 2;
            t->allow_dof = true; t->dof_passes = 3;
            t->allow_motion_blur = true; t->motion_samples = 16;
            t->allow_lighting = true; t->allow_fog = true;
            t->lighting_downscale = 1; t->shadow_angles = 1440; t->max_lights = 64; t->fog_octaves = 6;
            t->particle_scale = 1.5f;
            break;
        default:   /* high */
            t->render_scale = 1.0f; t->hdr = true; t->msaa = 4;
            t->bloom_passes = 3; t->bloom_downscale = 2;
            t->allow_dof = true; t->dof_passes = 2;
            t->allow_motion_blur = true; t->motion_samples = 8;
            t->allow_lighting = true; t->allow_fog = true;
            t->lighting_downscale = 2; t->shadow_angles = 720; t->max_lights = 48; t->fog_octaves = 4;
            t->particle_scale = 1.0f;
            break;
    }
}

void r2d_quality_init(r2d_quality_controller *c, r2d_tier start, r2d_tier min_tier, r2d_tier max_tier) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    if (min_tier > max_tier) { r2d_tier t = min_tier; min_tier = max_tier; max_tier = t; }
    c->min_tier = min_tier;
    c->max_tier = max_tier;
    c->tier = start < min_tier ? min_tier : (start > max_tier ? max_tier : start);
    c->warmup = 1.5f;
    c->cooldown = 0;
}
int r2d_quality_update(r2d_quality_controller *c, float frame_seconds, float target_fps) {
    if (!c || !(frame_seconds > 0) || !(target_fps > 0)) return 0;
    float ms = frame_seconds * 1000.0f;
    if (ms > 100.0f) ms = 100.0f;   /* a stall (load, resize) counts as one slow frame, not as a trend */
    float elapsed = frame_seconds > 0.05f ? 0.05f : frame_seconds;
    if (!c->warmed_up) {
        c->warmup -= frame_seconds;
        if (c->warmup > 0) return 0;
        c->warmed_up = true;
        c->ema_ms = ms;
    }
    c->ema_ms += (ms - c->ema_ms) * 0.1f;
    if (c->cooldown > 0) c->cooldown -= frame_seconds;
    float budget = 1000.0f / target_fps;
    if (c->ema_ms > budget * 1.15f) { c->over_seconds += elapsed; c->under_seconds = 0; }
    else if (c->ema_ms < budget * 0.6f) { c->under_seconds += elapsed; c->over_seconds = 0; }
    else { c->over_seconds = 0; c->under_seconds = 0; }
    if (c->cooldown > 0) return 0;
    if (c->over_seconds > 1.0f && c->tier > c->min_tier) {
        --c->tier;
        c->over_seconds = 0;
        c->cooldown = 3.0f;
        c->ema_ms = budget;   /* forget the old tier's cost */
        ++c->changes;
        return -1;
    }
    if (c->under_seconds > 6.0f && c->tier < c->max_tier) {
        ++c->tier;
        c->under_seconds = 0;
        c->cooldown = 6.0f;
        c->ema_ms = budget * 0.8f;
        ++c->changes;
        return 1;
    }
    return 0;
}
r2d_tier r2d_quality_pick_initial(bool hdr_supported, int width, int height) {
    int tier = R2D_TIER_HIGH;
    if (!hdr_supported) --tier;
    if ((long)width * (long)height > 2560L * 1440L) --tier;
    return tier < R2D_TIER_LOW ? R2D_TIER_LOW : (r2d_tier)tier;
}
