/* host_view3d: a render3d view inside a render2d window, for the desktop
 * hosts (the player and the editor of 3D projects).
 *
 * The view owns the 3D camera and a rectangle of the window. Each frame the
 * host tells it where the rectangle is (host_view3d_begin), the engine draws
 * the world's Shape3D primitives, Brush geometry and Light3D lights through
 * the canvas (host_canvas forwards them to render3d with their materials),
 * and render2d's underlay hook renders the 3D target and composites it
 * beneath the 2D layers - so render2d's grid, gizmos, text and panels draw
 * over the 3D image exactly as they do over a 2D level.
 *
 * Controls (host_view3d_controls, from render2d's input): right drag
 * orbits around the target, middle drag pans, the wheel dollies; the view
 * only reacts while the mouse is inside its rectangle and not over the UI,
 * or while a drag it started is in progress. */
#ifndef HOST_VIEW3D_H
#define HOST_VIEW3D_H

#include "cw_game.h"
#include "render2d.h"
#include "render3d.h"

typedef struct {
    bool ready;
    r2d_rect rect;             /* the view in window pixels (logical) */
    bool grid, axes;           /* editor helpers drawn each frame */
    float grid_spacing;        /* world units; zero -> 1 */
    int grid_half_lines;       /* zero -> 50 */
    float orbit_sensitivity;   /* radians per pixel; zero -> 0.006 */
    float wheel_step;          /* dolly factor per notch; zero -> 0.9 */
    int drag_button;           /* -1 when idle, else the R2D_MOUSE_* button dragging */
    bool dragging;             /* a drag started inside the view is in progress */
    bool began;                /* host_view3d_begin ran this frame: the underlay renders and composites (a host switched to 2D leaves the old image alone) */
    float perspective_fov;     /* the field of view to return to after an orthographic spell */
} host_view3d;
/* Where the camera looks from: free (wherever it was), or straight down,
 * from the front (+z, looking along -z) or from the right (+x). */
typedef enum { HOST_VIEW_FREE, HOST_VIEW_TOP, HOST_VIEW_FRONT, HOST_VIEW_RIGHT, HOST_VIEW_PRESETS } host_view3d_preset;
const char *host_view3d_preset_name(host_view3d_preset preset);

bool host_view3d_init(host_view3d *v);      /* after r2d_setup; installs render2d's underlay */
void host_view3d_destroy(host_view3d *v);
/* Once per frame, before the module draws: the rectangle the view fills. */
void host_view3d_begin(host_view3d *v, r2d_rect rect);
/* Camera controls from this frame's render2d input (call after begin). */
void host_view3d_controls(host_view3d *v, bool enabled);
bool host_view3d_contains(const host_view3d *v, r2d_vec2 window_point);
/* Frames every entity that carries a Shape3D (or a position), or a default
 * area when the world is empty. */
void host_view3d_frame_world(host_view3d *v, cw_game_world *gw);
/* The bounding sphere host_view3d_frame_world uses; false when nothing is placed. */
bool host_view3d_world_bounds(cw_game_world *gw, r3d_vec3 *center, float *radius);
/* Orthographic or perspective projection, keeping what the target shows at its size. */
void host_view3d_set_orthographic(host_view3d *v, bool orthographic);
bool host_view3d_orthographic(const host_view3d *v);
/* Turns the camera around its target to a preset direction (the distance stays). */
void host_view3d_look(host_view3d *v, host_view3d_preset preset);
/* A mouse position -> the point on the ground plane (y = height) under it. */
bool host_view3d_ground_point(const host_view3d *v, r2d_vec2 window_point, float height, r3d_vec3 *out);

/* Conversions between the engine's shapes and render3d's. */
r3d_primitive host_view3d_primitive(const cw_canvas_shape3d *shape);
static inline r3d_color host_view3d_color(cw_rgba c) { return r3d_rgba(c.r, c.g, c.b, c.a); }

#endif
