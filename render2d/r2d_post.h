/* r2d_post: a data- and config-driven post-process stack for render2d.
 *
 * When the stack is active the renderer draws the scene into an offscreen
 * (optionally HDR, MSAA-resolved) target instead of the window, then runs a
 * chain of fullscreen shader passes whose order and parameters come from
 * r2d_post_params, and finally writes the result to the swapchain. The
 * screen-space UI can be composited after the chain so it stays crisp.
 *
 * Passes:
 *   lighting              2D lights with soft shadows, volumetric light
 *                         and animated fog (r2d_lighting.h); composites
 *                         the lit scene, so it normally runs first
 *   bloom                 threshold -> separable gaussian blur at reduced
 *                         resolution -> additive combine
 *   dof                   depth of field for a 2D scene: a blurred copy is
 *                         mixed in by distance from a focus point or band
 *                         (tilt-shift); the focus can follow a world point
 *   motion_blur           directional + radial blur from the camera's own
 *                         motion (pan and zoom) between frames
 *   chromatic_aberration  radial RGB channel separation
 *   tonemap               exposure, reinhard / aces / filmic operators,
 *                         saturation, contrast, brightness, gamma
 *   vignette              darkening (or tinting) towards the edges
 *   shake                 trauma-driven camera shake applied to the whole
 *                         image (offset, rotation) with noise and decay
 *
 * Device tiers (r2d_quality.h): [quality] picks a tier or `auto`, and the
 * [tier_*] sections are budgets that clamp the cost knobs of everything
 * above; the artistic values stay as tuned.
 *
 * Every parameter is a tunable (see r2d_tunables.h): r2d_post_tunables()
 * exposes the table, r2d_post_load_config / r2d_post_save_config read and
 * write the `tunable.cfg` text format, r2d_post_reload_if_changed polls the
 * file for edits at runtime, and the struct can be edited directly between
 * frames. `stack.order` in the config decides which passes run and in which
 * sequence; each pass also has its own `enabled` switch.
 *
 * The renderer owns the stack: r2d_setup creates it unless
 * r2d_desc.no_post_process is set. The pure parts (parameter table, order
 * parsing, shake and motion vectors) are in r2d_post_params.c and need no
 * graphics backend; the passes themselves live in r2d_post.c.
 */
#ifndef R2D_POST_H
#define R2D_POST_H

#include "render2d.h"
#include "r2d_tunables.h"
#include "r2d_quality.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    R2D_PASS_LIGHTING,       /* lights, shadows, volumetric light and fog (see r2d_lighting.h) */
    R2D_PASS_BLOOM,
    R2D_PASS_DOF,
    R2D_PASS_MOTION_BLUR,
    R2D_PASS_CHROMATIC_ABERRATION,
    R2D_PASS_TONEMAP,
    R2D_PASS_VIGNETTE,
    R2D_PASS_SHAKE,
    R2D_PASS_COUNT
} r2d_pass;
const char *r2d_pass_name(r2d_pass pass);

typedef enum { R2D_TONEMAP_NONE, R2D_TONEMAP_REINHARD, R2D_TONEMAP_ACES, R2D_TONEMAP_FILMIC } r2d_tonemap_operator;
typedef enum { R2D_DOF_RADIAL, R2D_DOF_HORIZONTAL_BAND, R2D_DOF_VERTICAL_BAND } r2d_dof_shape;

typedef struct {
    struct {
        bool enabled;            /* the whole stack; off = draw straight to the window */
        bool hdr;                /* RGBA16F targets so bloom can exceed 1.0 */
        bool apply_to_ui;        /* post-process the screen-space layer too */
        float render_scale;      /* scene resolution relative to the window */
    } post;
    struct {
        int tier;                /* 0 auto, else 1 + r2d_tier */
        bool auto_adjust;        /* move between tiers from measured frame times */
        float target_fps;
        int min_tier, max_tier;  /* r2d_tier bounds for automatic changes */
    } quality;
    r2d_tier_settings tiers[R2D_TIER_COUNT];   /* [tier_low] .. [tier_ultra] */
    struct {
        char order[160];         /* comma-separated pass names, first runs first */
    } stack;
    struct {
        bool enabled;
        float threshold, soft_knee, intensity, radius;
        int passes, downscale;
    } bloom;
    struct {
        bool enabled;
        int shape;               /* r2d_dof_shape */
        float focus_x, focus_y;  /* screen fraction from the top-left, used unless a focus point is set */
        float focus_radius, falloff, blur_radius, max_blur;
        int blur_passes;
    } dof;
    struct {
        bool enabled;
        float strength, max_offset, zoom_strength;
        int samples;
    } motion_blur;
    struct {
        bool enabled;
        float strength, center_x, center_y, falloff;
    } chromatic_aberration;
    struct {
        bool enabled;
        int op;                  /* r2d_tonemap_operator */
        float exposure, gamma, saturation, contrast, brightness;
    } tonemap;
    struct {
        bool enabled;
        float radius, softness, intensity, roundness, color_r, color_g, color_b;
    } vignette;
    struct {
        bool enabled;
        float max_offset, max_angle, frequency, decay, exponent;
    } shake;
    struct {
        bool enabled;
        float ambient_r, ambient_g, ambient_b, ambient;   /* ambient color and intensity */
        float intensity, volumetric, haze;                /* global light, in-scatter and shaft-without-fog multipliers */
        float shadow_softness;                            /* penumbra width in angular steps */
        int shadow_angles, downscale, max_lights;         /* polar shadow map resolution, lightmap divisor, light budget */
        float falloff;                                    /* default falloff exponent */
    } lighting;
    struct {
        bool enabled;
        float density, color_r, color_g, color_b;
        float scale, speed_x, speed_y, contrast;          /* noise feature size (world units), wind, sharpening */
        int octaves;
        float ambient, direct;                            /* how much ambient and direct light the fog receives */
    } fog;
} r2d_post_params;

/* --- pure helpers (no graphics backend needed) --- */
void r2d_post_params_defaults(r2d_post_params *params);
r2d_tunable_set r2d_post_tunable_set(r2d_post_params *params);   /* binds the table to the struct */
const r2d_tunable *r2d_post_tunable_defs(size_t *count);
/* Parse params->stack.order into pass ids; unknown names are skipped and
 * counted in *unknown (may be NULL). Returns the number of passes. */
size_t r2d_post_parse_order(const char *order, r2d_pass *out, size_t capacity, size_t *unknown);
size_t r2d_post_format_order(const r2d_pass *passes, size_t count, char *out, size_t capacity);
/* Clamp the cost knobs of `params` to a tier's budget (pure). */
void r2d_tier_apply(const r2d_tier_settings *tier, r2d_post_params *params);
/* Shake state: trauma in 0..1, decays by `decay` per second; offset and
 * angle are derived from noise scaled by trauma^exponent. */
typedef struct { float trauma, time; uint32_t seed; } r2d_shake_state;
void r2d_shake_add(r2d_shake_state *s, float trauma);
void r2d_shake_update(r2d_shake_state *s, float dt, float decay);
void r2d_shake_sample(const r2d_shake_state *s, float max_offset, float max_angle, float frequency, float exponent,
                      r2d_vec2 *offset_px, float *angle);
/* Camera motion between two frames as a screen-space vector (pixels) and a
 * zoom ratio, which drive the motion blur pass. */
void r2d_motion_from_cameras(const r2d_camera *previous, const r2d_camera *current, r2d_vec2 viewport,
                             r2d_vec2 *delta_px, float *zoom_ratio);

/* --- the stack owned by the renderer --- */
r2d_post_params *r2d_post_params_ptr(void);           /* NULL when no stack was created */
bool r2d_post_active(void);                           /* stack exists, is valid, enabled and not bypassed */
/* Bypass: skip the whole stack (the scene goes straight to the window) from
 * the next frame on without changing the configured params; for tools that
 * preview the game's rendering settings and want a plain view too. */
void r2d_post_set_bypass(bool bypass);
bool r2d_post_bypassed(void);
bool r2d_post_load_config(const char *path, char *error, size_t error_capacity);   /* also sets the watched path */
bool r2d_post_save_config(const char *path);          /* NULL: the watched path */
bool r2d_post_reload_if_changed(void);                /* poll the watched file; true when reloaded */
const char *r2d_post_config_path(void);
void r2d_post_shake(float trauma);                    /* add trauma (0..1) to the shake pass */
float r2d_post_shake_trauma(void);
void r2d_post_set_focus(r2d_vec2 screen_point);       /* depth of field focus for this frame (screen space) */
void r2d_post_set_focus_world(r2d_vec2 world_point);
void r2d_post_clear_focus(void);                      /* back to the configured focus_x/focus_y */
typedef struct {
    bool active, hdr;
    int passes_run;              /* fullscreen passes executed last frame */
    int width, height;           /* scene target size in pixels */
    r2d_vec2 motion_px;          /* camera motion fed to the motion blur */
    float shake_trauma;
    r2d_tier tier;               /* tier in effect */
    bool tier_automatic;         /* chosen/adjusted automatically */
    float frame_ms;              /* smoothed frame time seen by the controller */
    int tier_changes;
} r2d_post_stats;
r2d_post_stats r2d_post_get_stats(void);
/* Quality tiers: the parameters actually used this frame (artistic settings
 * clamped to the tier), the tier in effect, and a way to force one. */
const r2d_post_params *r2d_post_effective_params(void);
r2d_tier r2d_post_current_tier(void);
void r2d_post_set_tier(int mode);            /* 0 = auto, 1 + r2d_tier = fixed; also updates params.quality.tier */
float r2d_post_particle_scale(void);         /* the tier's particle multiplier (1 without a stack) */

#ifdef __cplusplus
}
#endif
#endif
