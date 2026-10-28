/* r2d_lighting: 2D lights with soft shadows, volumetric in-scatter and fog.
 *
 * Lights and shadow casters are submitted every frame in world space, like
 * pickables. When the post-process stack runs its `lighting` pass, the
 * renderer builds a polar shadow map per light on the CPU (the nearest
 * occluder distance for every angle around the light), renders every light
 * additively into an HDR lightmap with two outputs - direct light at the
 * surface, soft-shadowed, and volumetric light (the light integrated along
 * the ray from the light to the pixel up to the first occluder, which is what
 * makes shafts continue softly past shadow casters) - and composites the
 * scene as
 *
 *   lit  = scene * (ambient + direct)
 *   fog  = fog_color * (fog_ambient + direct * direct_in_fog + volumetric)
 *   out  = mix(lit, fog, density * noise(world, time)) + volumetric * haze
 *
 * where the fog noise is animated FBM in world space with wind. Every
 * parameter lives in the `[lighting]` and `[fog]` sections of tunable.cfg
 * (r2d_post_params.lighting / .fog). Lighting needs the post-process stack:
 * with the stack disabled the scene is drawn unlit.
 *
 * The shadow map builder is pure and usable on its own (and unit-tested).
 */
#ifndef R2D_LIGHTING_H
#define R2D_LIGHTING_H

#include "render2d.h"

#ifdef __cplusplus
extern "C" {
#endif

#define R2D_MAX_LIGHTS 64
#define R2D_MAX_OCCLUDERS 4096

typedef struct {
    r2d_vec2 position;       /* world */
    float radius;            /* world units; light reaches zero here */
    r2d_color color;         /* alpha is ignored */
    float intensity;         /* multiplier, may exceed 1 with HDR targets; 0 -> 1 */
    float falloff;           /* exponent of (1 - d/radius); 0 -> 2 */
    float volumetric;        /* in-scatter strength relative to the global tunable; 0 -> 1 */
    bool no_shadows;         /* ignore occluders for this light */
    bool spot;               /* cone light aimed at `direction` */
    float direction;         /* radians */
    float cone;              /* full cone width in radians */
    float cone_softness;     /* 0..1 fraction of the cone that fades out; 0 -> 0.3 */
} r2d_light_desc;

/* --- per-frame submission (world space) --- */
bool r2d_light(const r2d_light_desc *light);               /* false when the frame's light budget is spent */
void r2d_light_occluder_circle(r2d_vec2 center, float radius);
void r2d_light_occluder_segment(r2d_vec2 a, r2d_vec2 b);
void r2d_light_occluder_rect(r2d_rect rect);                /* four segments */
void r2d_light_occluder_polyline(const r2d_vec2 *points, size_t count, bool closed);

typedef struct {
    int lights, occluders, shadow_rays;   /* last frame; shadow_rays = angle samples tested */
    int lightmap_width, lightmap_height;
    double shadow_seconds;                /* CPU time spent building shadow maps */
} r2d_lighting_stats;
r2d_lighting_stats r2d_lighting_get_stats(void);

/* --- pure shadow map builder --- */
typedef struct { r2d_vec2 center; float radius; } r2d_circle;
typedef struct { r2d_vec2 a, b; } r2d_segment;
/* Fill out[0..angles-1] with the distance from `light` to the nearest
 * occluder in the direction of angle i * 2pi / angles (angle 0 = +x,
 * increasing towards +y), clamped to `radius`. Returns the number of
 * angle/occluder tests performed. */
size_t r2d_shadow_map_build(r2d_vec2 light, float radius, const r2d_circle *circles, size_t circle_count,
                            const r2d_segment *segments, size_t segment_count, float *out, int angles);
float r2d_shadow_map_sample(const float *map, int angles, float angle_rad);
/* Ray intersection helpers (t along the unit direction, or -1 when missed). */
float r2d_ray_circle(r2d_vec2 origin, r2d_vec2 direction, r2d_circle circle);
float r2d_ray_segment(r2d_vec2 origin, r2d_vec2 direction, r2d_segment segment);

#ifdef __cplusplus
}
#endif
#endif
