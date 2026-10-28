/* Interface between render2d.c and the post-process stack. Not for apps. */
#ifndef R2D_POST_INTERNAL_H
#define R2D_POST_INTERNAL_H

#include "r2d_post.h"
#include "sokol_gfx.h"

bool r2d_post__create(int swapchain_sample_count, sg_pixel_format swapchain_color, sg_pixel_format swapchain_depth);
void r2d_post__destroy(void);
/* Formats the scene-layer sokol-gl / debugtext contexts must be created with. */
sg_pixel_format r2d_post__scene_color_format(void);
int r2d_post__scene_sample_count(void);
/* Per frame, before anything is drawn: updates shake and camera motion and
 * (re)creates the targets for the framebuffer size. */
void r2d_post__begin_frame(float dt, r2d_vec2 viewport, int width, int height);
/* Camera motion between the previous and the current frame (for motion blur). */
void r2d_post__set_motion(const r2d_camera *previous, const r2d_camera *current, r2d_vec2 viewport);
/* The camera and viewport the scene was drawn with (for lighting and fog). */
void r2d_post__set_view(const r2d_camera *camera, r2d_vec2 viewport, float dpi);
/* True when this frame's scene goes through the stack (decided in begin_frame). */
bool r2d_post__scene_ready(void);
/* Begins the offscreen scene pass; false when the stack is inactive, in
 * which case the caller renders to the window. */
bool r2d_post__begin_scene(sg_color clear);
void r2d_post__end_scene(void);
/* Runs the chain; the last pass is drawn inside a swapchain pass that is
 * left open so the caller can composite overlays before ending it. */
void r2d_post__run(sg_swapchain swapchain);

#endif
