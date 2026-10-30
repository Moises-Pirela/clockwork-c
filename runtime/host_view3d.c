#include "host_view3d.h"
#include <math.h>
#include <string.h>

static void underlay_prepare(void *user) {
    host_view3d *v = user;
    if (!v->ready || !v->began) return;
    if (v->grid) {
        float spacing = v->grid_spacing > 0 ? v->grid_spacing : 1;
        int half = v->grid_half_lines > 0 ? v->grid_half_lines : 50;
        r3d_draw_grid(spacing, half, r3d_rgba(255, 255, 255, 22), r3d_rgba(255, 255, 255, 60));
    }
    if (v->axes) r3d_draw_axes(r3d_v3(0, 0, 0), 1);
    r3d_render();
}
static void underlay_draw(void *user, const r2d_pass_info *info) {
    host_view3d *v = user;
    if (!v->ready || !v->began) return;
    r3d_composite((sg_pixel_format)info->color_format, (sg_pixel_format)info->depth_format, info->sample_count, info->viewport.x, info->viewport.y);
    v->began = false;
}
const char *host_view3d_preset_name(host_view3d_preset p) {
    static const char *const names[HOST_VIEW_PRESETS] = {"free", "top", "front", "right"};
    return p >= 0 && p < HOST_VIEW_PRESETS ? names[p] : "?";
}
bool host_view3d_orthographic(const host_view3d *v) { return v->ready && r3d_camera_ptr()->fov_y <= 0; }
void host_view3d_set_orthographic(host_view3d *v, bool orthographic) {
    if (!v->ready) return;
    r3d_camera *cam = r3d_camera_ptr();
    if (orthographic && cam->fov_y > 0) {
        v->perspective_fov = cam->fov_y;
        cam->ortho_height = 2 * r3d_camera_distance(cam) * tanf(cam->fov_y * 0.5f);   /* the target's plane keeps its size */
        cam->fov_y = 0;
    } else if (!orthographic && cam->fov_y <= 0) {
        cam->fov_y = v->perspective_fov > 0 ? v->perspective_fov : 50.0f * R3D_PI / 180.0f;
        float distance = cam->ortho_height * 0.5f / tanf(cam->fov_y * 0.5f);
        r3d_camera_set_orbit(cam, r3d_camera_yaw(cam), r3d_camera_pitch(cam), distance);
    }
    r3d_set_camera(cam);
}
void host_view3d_look(host_view3d *v, host_view3d_preset preset) {
    if (!v->ready) return;
    r3d_camera *cam = r3d_camera_ptr();
    float yaw = r3d_camera_yaw(cam), pitch = r3d_camera_pitch(cam);
    switch (preset) {
        case HOST_VIEW_TOP: yaw = 0; pitch = R3D_PI * 0.5f; break;   /* clamped just short of the pole, so the up vector holds */
        case HOST_VIEW_FRONT: yaw = 0; pitch = 0; break;
        case HOST_VIEW_RIGHT: yaw = R3D_PI * 0.5f; pitch = 0; break;
        default: yaw = 39.0f * R3D_PI / 180.0f; pitch = 25.0f * R3D_PI / 180.0f; break;
    }
    r3d_camera_set_orbit(cam, yaw, pitch, r3d_camera_distance(cam));
    r3d_set_camera(cam);
}
bool host_view3d_init(host_view3d *v) {
    memset(v, 0, sizeof(*v));
    v->drag_button = -1;
    v->grid_spacing = 1;
    v->grid_half_lines = 50;
    v->orbit_sensitivity = 0.006f;
    v->wheel_step = 0.9f;
    r3d_desc d;
    memset(&d, 0, sizeof(d));
    d.skip_gfx_setup = true;
    d.background = r3d_rgb(34, 37, 45);
    if (!r3d_setup(&d)) { r3d_shutdown(); return false; }   /* releases whatever setup managed to create */
    r2d_underlay u = {v, underlay_prepare, underlay_draw};
    r2d_set_underlay(&u);
    r3d_camera cam = r3d_camera_make(r3d_v3(9, 7, 11), r3d_v3(0, 0.5f, 0));
    r3d_set_camera(&cam);
    v->rect = r2d_rect_make(0, 0, r2d_viewport().x, r2d_viewport().y);
    v->ready = true;
    return true;
}
void host_view3d_destroy(host_view3d *v) {
    if (!v->ready) return;
    r2d_set_underlay(NULL);
    r3d_shutdown();
    memset(v, 0, sizeof(*v));
    v->drag_button = -1;
}
void host_view3d_begin(host_view3d *v, r2d_rect rect) {
    if (!v->ready) return;
    v->rect = rect;
    v->began = true;
    r3d_begin_frame(rect.x, rect.y, rect.w, rect.h, r2d_dpi_scale());
}
bool host_view3d_contains(const host_view3d *v, r2d_vec2 p) { return v->ready && r2d_rect_contains(v->rect, p); }
void host_view3d_controls(host_view3d *v, bool enabled) {
    if (!v->ready) return;
    if (!enabled) { v->drag_button = -1; v->dragging = false; return; }
    r2d_vec2 mouse = r2d_mouse();
    bool inside = host_view3d_contains(v, mouse) && !r2d_mouse_over_ui();
    r3d_camera *cam = r3d_camera_ptr();
    if (v->drag_button < 0 && inside) {
        if (r2d_mouse_pressed(R2D_MOUSE_RIGHT)) v->drag_button = R2D_MOUSE_RIGHT;
        else if (r2d_mouse_pressed(R2D_MOUSE_MIDDLE)) v->drag_button = R2D_MOUSE_MIDDLE;
        v->dragging = v->drag_button >= 0;
    }
    if (v->drag_button >= 0) {
        if (!r2d_mouse_down((r2d_mouse_button)v->drag_button)) { v->drag_button = -1; v->dragging = false; }
        else {
            r2d_vec2 d = r2d_mouse_delta();
            if (v->drag_button == R2D_MOUSE_RIGHT) {
                float s = v->orbit_sensitivity > 0 ? v->orbit_sensitivity : 0.006f;
                r3d_camera_orbit(cam, -d.x * s, d.y * s);   /* the scene follows the mouse */
            } else {
                r3d_camera_pan_screen(cam, d.x, d.y, v->rect.h);
            }
        }
    }
    float wheel = r2d_wheel();
    if (wheel != 0 && inside) {
        float step = v->wheel_step > 0 && v->wheel_step < 1 ? v->wheel_step : 0.9f;
        r3d_camera_dolly(cam, powf(step, wheel));
    }
    r3d_set_camera(cam);
}
bool host_view3d_world_bounds(cw_game_world *gw, r3d_vec3 *center, float *radius) {
    if (!gw || !gw->bound) return false;
    cw_world *w = &gw->world;
    r3d_vec3 lo = r3d_v3(INFINITY, INFINITY, INFINITY), hi = r3d_v3(-INFINITY, -INFINITY, -INFINITY);
    size_t n = 0;
    for (size_t i = 0; i < w->capacity; ++i) {
        cw_entity_id id = (cw_entity_id)i;
        float x, y, z;
        if (!cw_world_is_valid(w, id) || !cw_reflect_position3(w, id, &x, &y, &z)) continue;
        r3d_vec3 l = r3d_v3(x - 0.5f, y - 0.5f, z - 0.5f), h = r3d_v3(x + 0.5f, y + 0.5f, z + 0.5f);
        if (gw->shape3d_ct != CW_INVALID_COMPONENT && cw_comp_has(w, id, gw->shape3d_ct)) {
            cw_canvas_shape3d shape;
            cw_shape3d_to_canvas(cw_comp_ptr(w, id, gw->shape3d_ct), x, y, z, id, &shape);
            r3d_primitive p = host_view3d_primitive(&shape);
            r3d_primitive_bounds(&p, &l, &h);
            if (shape.kind == CW_SHAPE_PLANE) {   /* a huge ground plane should not push everything away */
                const float cap = 30;
                l = r3d_v3(r3d_maxf(l.x, x - cap), r3d_maxf(l.y, y - cap), r3d_maxf(l.z, z - cap));
                h = r3d_v3(r3d_minf(h.x, x + cap), r3d_minf(h.y, y + cap), r3d_minf(h.z, z + cap));
            }
        }
        lo = r3d_v3(r3d_minf(lo.x, l.x), r3d_minf(lo.y, l.y), r3d_minf(lo.z, l.z));
        hi = r3d_v3(r3d_maxf(hi.x, h.x), r3d_maxf(hi.y, h.y), r3d_maxf(hi.z, h.z));
        ++n;
    }
    if (!n) return false;
    if (center) *center = r3d_scale(r3d_add(lo, hi), 0.5f);
    /* the largest half extent rather than the box's diagonal: a flat level
     * fills the view instead of floating in the middle of it */
    r3d_vec3 half = r3d_scale(r3d_sub(hi, lo), 0.5f);
    if (radius) *radius = r3d_maxf(r3d_maxf(half.x, r3d_maxf(half.y, half.z)) * 1.15f, 0.5f);
    return true;
}
void host_view3d_frame_world(host_view3d *v, cw_game_world *gw) {
    if (!v->ready) return;
    r3d_vec3 center = r3d_v3(0, 0.5f, 0);
    float radius = 6;
    host_view3d_world_bounds(gw, &center, &radius);
    r3d_camera *cam = r3d_camera_ptr();
    r3d_camera_fit(cam, center, radius, v->rect.h > 0 ? v->rect.w / v->rect.h : 1);
    r3d_set_camera(cam);
}
bool host_view3d_ground_point(const host_view3d *v, r2d_vec2 p, float height, r3d_vec3 *out) {
    if (!v->ready) return false;
    return r3d_ray_ground(r3d_screen_ray(p.x, p.y), height, out);
}
r3d_primitive host_view3d_primitive(const cw_canvas_shape3d *s) {
    r3d_primitive_kind kind = R3D_CUBE;
    switch (s->kind) {
        case CW_SHAPE_SPHERE: kind = R3D_SPHERE; break;
        case CW_SHAPE_CAPSULE: kind = R3D_CAPSULE; break;
        case CW_SHAPE_CUBE: kind = R3D_CUBE; break;
        case CW_SHAPE_PLANE: kind = R3D_PLANE; break;
        default: break;
    }
    r3d_primitive p = r3d_primitive_make(kind, r3d_v3(s->x, s->y, s->z), r3d_v3(s->sx, s->sy, s->sz), host_view3d_color(s->color));
    p.rotation = r3d_v3(s->rx, s->ry, s->rz);
    p.checker = s->checker;
    p.wireframe = s->wireframe;
    p.id = s->id == CW_INVALID_ENTITY ? R3D_ID_NONE : (uint64_t)s->id;
    return p;
}
