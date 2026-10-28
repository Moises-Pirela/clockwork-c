/* r2d_quality: device tiers for the post-process stack and lighting.
 *
 * A tier is a budget: how much resolution, how many blur passes, samples,
 * shadow rays, lights and particles a device class may spend. Four tiers
 * (low, medium, high, ultra) live in tunable.cfg as the sections
 * [tier_low] .. [tier_ultra]; [quality] selects one of them or `auto`.
 *
 * Tiers never replace the artistic settings: r2d_tier_apply clamps the cost
 * knobs of a parameter set to the tier's budget (fewer passes, larger
 * downscales, fewer samples and rays, lower render scale, HDR and optional
 * passes only where allowed) and leaves colors, thresholds and strengths
 * untouched, so a scene tuned on a high-end machine degrades gracefully.
 *
 * With `auto`, r2d_quality_pick_initial chooses a starting tier from what
 * the device supports, and the r2d_quality_controller moves it down when
 * the smoothed frame time stays over the budget and back up when there is
 * headroom for a while (with hysteresis and a cooldown, so it does not
 * oscillate). The controller is a plain value driven by frame times, usable
 * and testable without a window.
 */
#ifndef R2D_QUALITY_H
#define R2D_QUALITY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { R2D_TIER_LOW, R2D_TIER_MEDIUM, R2D_TIER_HIGH, R2D_TIER_ULTRA, R2D_TIER_COUNT } r2d_tier;
const char *r2d_tier_name(r2d_tier tier);

typedef struct {
    float render_scale;        /* scene resolution relative to the window (0.25 .. 1) */
    bool hdr;                  /* RGBA16F targets allowed */
    int msaa;                  /* window sample count to request (needs a restart to change) */
    int bloom_passes, bloom_downscale;
    bool allow_dof;
    int dof_passes;
    bool allow_motion_blur;
    int motion_samples;
    bool allow_lighting, allow_fog;
    int lighting_downscale, shadow_angles, max_lights, fog_octaves;
    float particle_scale;      /* multiplier for particle counts and emitter rates (apps read it) */
} r2d_tier_settings;
void r2d_tier_defaults(r2d_tier tier, r2d_tier_settings *out);

typedef struct {
    r2d_tier tier, min_tier, max_tier;
    float ema_ms;              /* smoothed frame time */
    float over_seconds;        /* time spent continuously over budget */
    float under_seconds;       /* time spent continuously with headroom */
    float cooldown;            /* seconds until the next change is allowed */
    int changes;               /* tier changes so far */
    bool warmed_up;            /* the first frames are ignored (shader compiles, loads) */
    float warmup;
} r2d_quality_controller;

void r2d_quality_init(r2d_quality_controller *c, r2d_tier start, r2d_tier min_tier, r2d_tier max_tier);
/* Feed one frame; returns -1 when the tier was lowered, +1 when raised, 0 otherwise. */
int r2d_quality_update(r2d_quality_controller *c, float frame_seconds, float target_fps);
/* Starting tier for `auto`: high when HDR targets are available, one tier
 * lower without them, and one lower again for very large framebuffers. */
r2d_tier r2d_quality_pick_initial(bool hdr_supported, int framebuffer_width, int framebuffer_height);

#ifdef __cplusplus
}
#endif
#endif
