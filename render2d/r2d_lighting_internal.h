/* Interface between the post-process stack and the lighting pass. Not for apps. */
#ifndef R2D_LIGHTING_INTERNAL_H
#define R2D_LIGHTING_INTERNAL_H

#include "r2d_lighting.h"
#include "r2d_post.h"
#include "sokol_gfx.h"

typedef struct {
    sg_view direct, volume;      /* lightmap textures for the composite */
    int width, height;
} r2d_lightmap;

bool r2d_lighting__create(void);
void r2d_lighting__destroy(void);
/* Renders this frame's lights into the lightmap (ambient-cleared). `format`
 * is the scene target format, `scene_w/h` its size in pixels. Returns false
 * when the lighting resources are unavailable. */
bool r2d_lighting__render(const r2d_post_params *params, sg_pixel_format format, int scene_w, int scene_h,
                          const r2d_camera *camera, r2d_vec2 viewport, float dpi, r2d_lightmap *out);
/* Forget this frame's lights and occluders (called at the end of every frame). */
void r2d_lighting__end_frame(void);

#endif
