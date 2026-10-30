/* Headless tests for the pure parts of render3d: matrices, the camera,
 * the primitive meshes and the ray casts picking relies on. No window or
 * graphics backend is needed. */
#include "render3d.h"
#include "r3d_mesh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define EXPECT(cond) do { ++checks; if (!(cond)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
static bool near_f(float a, float b) { return fabsf(a - b) <= 1e-3f * (1.0f + fabsf(b)); }
#define NEAR(a, b) near_f((float)(a), (float)(b))
#define NEAR_V(a, b) (NEAR((a).x, (b).x) && NEAR((a).y, (b).y) && NEAR((a).z, (b).z))

static bool mat_near(r3d_mat4 a, r3d_mat4 b) {
    for (int i = 0; i < 16; ++i) if (!near_f(a.m[i], b.m[i])) return false;
    return true;
}

static void test_vectors(void) {
    r3d_vec3 a = r3d_v3(1, 2, 3), b = r3d_v3(-2, 0.5f, 4);
    EXPECT(NEAR_V(r3d_add(a, b), r3d_v3(-1, 2.5f, 7)));
    EXPECT(NEAR_V(r3d_sub(a, b), r3d_v3(3, 1.5f, -1)));
    EXPECT(NEAR(r3d_dot(a, b), -2 + 1 + 12));
    EXPECT(NEAR_V(r3d_cross(r3d_v3(1, 0, 0), r3d_v3(0, 1, 0)), r3d_v3(0, 0, 1)));
    EXPECT(NEAR(r3d_length(r3d_v3(3, 4, 12)), 13));
    EXPECT(NEAR(r3d_length(r3d_normalize(a)), 1));
    EXPECT(NEAR_V(r3d_normalize(r3d_v3(0, 0, 0)), r3d_v3(0, 0, 0)));
    EXPECT(NEAR_V(r3d_lerp(a, b, 0.5f), r3d_v3(-0.5f, 1.25f, 3.5f)));
}
static void test_matrices(void) {
    r3d_mat4 id = r3d_mat4_identity();
    EXPECT(mat_near(r3d_mat4_multiply(id, id), id));
    r3d_mat4 t = r3d_mat4_translation(r3d_v3(1, 2, 3)), s = r3d_mat4_scaling(r3d_v3(2, 2, 2));
    /* translation * scale: scale first */
    r3d_vec3 p = r3d_mat4_transform_point(&(r3d_mat4){{0}}, r3d_v3(0, 0, 0));
    (void)p;
    r3d_mat4 ts = r3d_mat4_multiply(t, s);
    EXPECT(NEAR_V(r3d_mat4_transform_point(&ts, r3d_v3(1, 1, 1)), r3d_v3(3, 4, 5)));
    EXPECT(NEAR_V(r3d_mat4_transform_vector(&ts, r3d_v3(1, 1, 1)), r3d_v3(2, 2, 2)));   /* vectors ignore translation */
    /* rotations: a quarter turn about each axis, right-handed */
    r3d_mat4 rx = r3d_mat4_rotation_x(R3D_PI / 2), ry = r3d_mat4_rotation_y(R3D_PI / 2), rz = r3d_mat4_rotation_z(R3D_PI / 2);
    EXPECT(NEAR_V(r3d_mat4_transform_vector(&rx, r3d_v3(0, 1, 0)), r3d_v3(0, 0, 1)));
    EXPECT(NEAR_V(r3d_mat4_transform_vector(&ry, r3d_v3(0, 0, 1)), r3d_v3(1, 0, 0)));
    EXPECT(NEAR_V(r3d_mat4_transform_vector(&rz, r3d_v3(1, 0, 0)), r3d_v3(0, 1, 0)));
    /* euler order: roll, then pitch, then yaw */
    r3d_mat4 e = r3d_mat4_rotation(r3d_v3(0.3f, 0.7f, -0.2f));
    r3d_mat4 expected = r3d_mat4_multiply(r3d_mat4_rotation_y(0.7f), r3d_mat4_multiply(r3d_mat4_rotation_x(0.3f), r3d_mat4_rotation_z(-0.2f)));
    EXPECT(mat_near(e, expected));
    /* inverse round trip on a full trs */
    r3d_mat4 trs = r3d_mat4_trs(r3d_v3(3, -1, 2), r3d_v3(0.4f, 1.1f, -0.6f), r3d_v3(2, 0.5f, 3)), inv;
    EXPECT(r3d_mat4_inverse(&trs, &inv));
    EXPECT(mat_near(r3d_mat4_multiply(trs, inv), id));
    r3d_vec3 q = r3d_v3(0.3f, -0.8f, 1.7f);
    EXPECT(NEAR_V(r3d_mat4_transform_point(&inv, r3d_mat4_transform_point(&trs, q)), q));
    r3d_mat4 singular = r3d_mat4_scaling(r3d_v3(1, 0, 1));
    EXPECT(!r3d_mat4_inverse(&singular, &inv));
    EXPECT(mat_near(r3d_mat4_transpose(r3d_mat4_transpose(trs)), trs));
    EXPECT(NEAR(r3d_mat4_at(&t, 0, 3), 1) && NEAR(r3d_mat4_at(&t, 2, 3), 3));
}
static void test_look_at_and_projection(void) {
    r3d_mat4 view = r3d_mat4_look_at(r3d_v3(0, 0, 10), r3d_v3(0, 0, 0), r3d_v3(0, 1, 0));
    /* the target lands on the -z axis in view space, 10 away */
    EXPECT(NEAR_V(r3d_mat4_transform_point(&view, r3d_v3(0, 0, 0)), r3d_v3(0, 0, -10)));
    /* +x stays to the right, +y up */
    EXPECT(NEAR_V(r3d_mat4_transform_point(&view, r3d_v3(1, 2, 0)), r3d_v3(1, 2, -10)));
    /* the eye is at the origin of view space */
    EXPECT(NEAR_V(r3d_mat4_transform_point(&view, r3d_v3(0, 0, 10)), r3d_v3(0, 0, 0)));
    /* a degenerate up vector still gives a valid basis */
    r3d_mat4 v2 = r3d_mat4_look_at(r3d_v3(0, 10, 0), r3d_v3(0, 0, 0), r3d_v3(0, 1, 0));
    EXPECT(NEAR(r3d_length(r3d_mat4_transform_vector(&v2, r3d_v3(1, 0, 0))), 1));
    for (int zero_to_one = 0; zero_to_one < 2; ++zero_to_one) {
        r3d_mat4 proj = r3d_mat4_perspective(R3D_PI / 2, 1, 1, 100, zero_to_one != 0);
        r3d_vec4 n = r3d_mat4_transform(&proj, (r3d_vec4){0, 0, -1, 1}), f = r3d_mat4_transform(&proj, (r3d_vec4){0, 0, -100, 1});
        EXPECT(NEAR(n.z / n.w, zero_to_one ? 0 : -1));
        EXPECT(NEAR(f.z / f.w, 1));
        /* a 90 degree fov: a point at x = z lands on the right clip edge */
        r3d_vec4 e = r3d_mat4_transform(&proj, (r3d_vec4){5, 0, -5, 1});
        EXPECT(NEAR(e.x / e.w, 1));
        r3d_mat4 ortho = r3d_mat4_orthographic(-2, 2, -1, 1, 1, 100, zero_to_one != 0);
        r3d_vec4 o = r3d_mat4_transform(&ortho, (r3d_vec4){2, -1, -1, 1});
        EXPECT(NEAR(o.x, 1) && NEAR(o.y, -1) && NEAR(o.z, zero_to_one ? 0 : -1) && NEAR(o.w, 1));
    }
}
static void test_camera(void) {
    r3d_camera c = r3d_camera_make(r3d_v3(0, 0, 10), r3d_v3(0, 0, 0));
    EXPECT(NEAR(r3d_camera_distance(&c), 10));
    EXPECT(NEAR(r3d_camera_yaw(&c), 0) && NEAR(r3d_camera_pitch(&c), 0));
    EXPECT(NEAR_V(r3d_camera_forward(&c), r3d_v3(0, 0, -1)));
    EXPECT(NEAR_V(r3d_camera_right(&c), r3d_v3(1, 0, 0)));
    EXPECT(NEAR_V(r3d_camera_up(&c), r3d_v3(0, 1, 0)));
    /* orbit a quarter turn: the camera moves to +x, still looking at the target */
    r3d_camera_orbit(&c, R3D_PI / 2, 0);
    EXPECT(NEAR_V(c.position, r3d_v3(10, 0, 0)));
    EXPECT(NEAR_V(c.target, r3d_v3(0, 0, 0)));
    /* pitch is clamped short of the poles */
    r3d_camera_orbit(&c, 0, 10);
    EXPECT(r3d_camera_pitch(&c) < R3D_PI / 2 && r3d_camera_pitch(&c) > 1.5f);
    EXPECT(NEAR(r3d_camera_distance(&c), 10));
    /* dolly halves the distance, never below the minimum */
    r3d_camera_dolly(&c, 0.5f);
    EXPECT(NEAR(r3d_camera_distance(&c), 5));
    r3d_camera_dolly(&c, 0.0001f);
    EXPECT(NEAR(r3d_camera_distance(&c), c.min_distance));
    /* pan moves both ends */
    c = r3d_camera_make(r3d_v3(0, 0, 10), r3d_v3(0, 0, 0));
    r3d_camera_pan(&c, 1, 2);
    EXPECT(NEAR_V(c.target, r3d_v3(1, 2, 0)) && NEAR_V(c.position, r3d_v3(1, 2, 10)));
    /* a screen drag to the right moves the view so the content follows the mouse: the target moves left */
    c = r3d_camera_make(r3d_v3(0, 0, 10), r3d_v3(0, 0, 0));
    r3d_camera_pan_screen(&c, 100, 0, 600);
    EXPECT(c.target.x < 0 && NEAR(c.target.y, 0));
    float upp = r3d_camera_units_per_pixel(&c, 600);
    EXPECT(NEAR(c.target.x, -100 * upp));
    /* fit: the sphere fits the narrower side of the view */
    c = r3d_camera_make(r3d_v3(0, 5, 10), r3d_v3(0, 0, 0));
    r3d_camera_fit(&c, r3d_v3(1, 2, 3), 4, 16.0f / 9.0f);
    EXPECT(NEAR_V(c.target, r3d_v3(1, 2, 3)));
    float d = r3d_camera_distance(&c);
    EXPECT(d > 4 && d < 20);
    EXPECT(NEAR(d, 4 / sinf(c.fov_y * 0.5f) * 1.05f));
    /* the direction was kept */
    EXPECT(NEAR(r3d_camera_yaw(&c), 0) && NEAR(r3d_camera_pitch(&c), atan2f(5, 10)));
    /* orthographic: projection has no perspective divide */
    c.fov_y = 0;
    c.ortho_height = 10;
    r3d_mat4 p = r3d_camera_projection(&c, 2, false);
    r3d_vec4 q = r3d_mat4_transform(&p, (r3d_vec4){10, 5, -3, 1});
    EXPECT(NEAR(q.x, 1) && NEAR(q.y, 1) && NEAR(q.w, 1));
}
static void test_project_and_rays(void) {
    r3d_camera c = r3d_camera_make(r3d_v3(0, 0, 10), r3d_v3(0, 0, 0));
    float vw = 800, vh = 600;
    r3d_mat4 vp = r3d_camera_view_projection(&c, vw / vh, false);
    float sx, sy, depth;
    /* the target projects to the viewport centre */
    EXPECT(r3d_project(&vp, vw, vh, r3d_v3(0, 0, 0), &sx, &sy, &depth));
    EXPECT(NEAR(sx, 400) && NEAR(sy, 300));
    /* +y goes up the screen (smaller sy), +x to the right */
    EXPECT(r3d_project(&vp, vw, vh, r3d_v3(1, 1, 0), &sx, &sy, NULL));
    EXPECT(sx > 400 && sy < 300);
    /* behind the camera */
    EXPECT(!r3d_project(&vp, vw, vh, r3d_v3(0, 0, 20), &sx, &sy, NULL));
    /* the ray through the centre goes straight at the target */
    r3d_ray r = r3d_camera_ray(&c, vw, vh, 400, 300);
    EXPECT(NEAR_V(r.direction, r3d_v3(0, 0, -1)));
    EXPECT(NEAR(r.origin.x, 0) && NEAR(r.origin.y, 0));
    /* project then unproject round trip for an off-centre point */
    r3d_vec3 world = r3d_v3(2, -1.5f, 3);
    EXPECT(r3d_project(&vp, vw, vh, world, &sx, &sy, NULL));
    r = r3d_camera_ray(&c, vw, vh, sx, sy);
    /* the world point lies on the ray: the distance from the ray is ~0 */
    r3d_vec3 to = r3d_sub(world, r.origin);
    float along = r3d_dot(to, r.direction);
    r3d_vec3 closest = r3d_add(r.origin, r3d_scale(r.direction, along));
    EXPECT(r3d_distance(closest, world) < 1e-2f);
    EXPECT(along > 0);
    /* planes and lines (gizmo drags) */
    float tp;
    r3d_ray diag = {r3d_v3(0, 5, 5), r3d_v3(0, -1, -1)};
    EXPECT(r3d_ray_plane(diag, r3d_v3(0, 0, 0), r3d_v3(0, 1, 0), &tp) && NEAR(tp, 5));
    EXPECT(!r3d_ray_plane(diag, r3d_v3(0, 0, 0), r3d_v3(1, 0, 0), &tp));              /* parallel */
    r3d_ray back = {r3d_v3(0, 5, 5), r3d_v3(0, 1, 1)};
    EXPECT(!r3d_ray_plane(back, r3d_v3(0, 0, 0), r3d_v3(0, 1, 0), &tp));              /* behind */
    float sp;
    r3d_ray across = {r3d_v3(3, 2, 10), r3d_v3(0, 0, -1)};                             /* passes over x = 3 on the x axis line */
    EXPECT(r3d_ray_line_closest(across, r3d_v3(0, 0, 0), r3d_v3(1, 0, 0), &sp) && NEAR(sp, 3));
    EXPECT(r3d_ray_line_closest(across, r3d_v3(1, 0, 0), r3d_v3(2, 0, 0), &sp) && NEAR(sp, 1));   /* origin and scale of the line matter */
    r3d_ray parallel_line = {r3d_v3(0, 1, 0), r3d_v3(1, 0, 0)};
    EXPECT(!r3d_ray_line_closest(parallel_line, r3d_v3(0, 0, 0), r3d_v3(1, 0, 0), &sp));
    /* convex solids: a unit-ish box as six planes */
    r3d_plane box[6] = {{{1, 0, 0}, 1}, {{-1, 0, 0}, 1}, {{0, 1, 0}, 2}, {{0, -1, 0}, 0}, {{0, 0, 1}, 1}, {{0, 0, -1}, 1}};
    float tc;
    int plane;
    r3d_ray from_above = {r3d_v3(0.5f, 10, 0), r3d_v3(0, -1, 0)};
    EXPECT(r3d_ray_convex(from_above, box, 6, &tc, &plane) && NEAR(tc, 8) && plane == 2);
    r3d_ray from_inside = {r3d_v3(0, 1, 0), r3d_v3(1, 0, 0)};
    EXPECT(r3d_ray_convex(from_inside, box, 6, &tc, &plane) && NEAR(tc, 1) && plane == 0);
    r3d_ray beside = {r3d_v3(5, 10, 0), r3d_v3(0, -1, 0)};
    EXPECT(!r3d_ray_convex(beside, box, 6, &tc, &plane));
    r3d_ray away_from = {r3d_v3(0, 10, 0), r3d_v3(0, 1, 0)};
    EXPECT(!r3d_ray_convex(away_from, box, 6, &tc, &plane));
    /* a wedge: the box plus a diagonal cut (x + y <= 1) */
    r3d_plane wedge[7] = {{{1, 0, 0}, 1}, {{-1, 0, 0}, 1}, {{0, 1, 0}, 2}, {{0, -1, 0}, 0}, {{0, 0, 1}, 1}, {{0, 0, -1}, 1}, {{0.70710678f, 0.70710678f, 0}, 0.70710678f}};
    EXPECT(r3d_ray_convex(from_above, wedge, 7, &tc, &plane) && NEAR(tc, 9.5f) && plane == 6);   /* at x = 0.5 the slope is at y = 0.5 */
    /* ground intersection */
    r3d_vec3 g;
    r3d_ray down = {r3d_v3(1, 5, 2), r3d_v3(0, -1, 0)};
    EXPECT(r3d_ray_ground(down, 0, &g) && NEAR_V(g, r3d_v3(1, 0, 2)));
    r3d_ray up = {r3d_v3(1, 5, 2), r3d_v3(0, 1, 0)};
    EXPECT(!r3d_ray_ground(up, 0, &g));
    r3d_ray flat = {r3d_v3(1, 5, 2), r3d_v3(1, 0, 0)};
    EXPECT(!r3d_ray_ground(flat, 0, &g));
}
static void test_ray_casts(void) {
    float t;
    r3d_ray r = {r3d_v3(0, 0, 10), r3d_v3(0, 0, -1)};
    EXPECT(r3d_ray_sphere(r, r3d_v3(0, 0, 0), 2, &t) && NEAR(t, 8));
    EXPECT(!r3d_ray_sphere(r, r3d_v3(5, 0, 0), 2, &t));
    /* from inside: the exit point */
    r3d_ray inside = {r3d_v3(0, 0, 0), r3d_v3(0, 0, -1)};
    EXPECT(r3d_ray_sphere(inside, r3d_v3(0, 0, 0), 2, &t) && NEAR(t, 2));
    /* behind */
    r3d_ray away = {r3d_v3(0, 0, 10), r3d_v3(0, 0, 1)};
    EXPECT(!r3d_ray_sphere(away, r3d_v3(0, 0, 0), 2, &t));
    /* an unnormalized direction reports t in its units */
    r3d_ray fast = {r3d_v3(0, 0, 10), r3d_v3(0, 0, -2)};
    EXPECT(r3d_ray_sphere(fast, r3d_v3(0, 0, 0), 2, &t) && NEAR(t, 4));

    EXPECT(r3d_ray_aabb(r, r3d_v3(-1, -1, -1), r3d_v3(1, 1, 1), &t) && NEAR(t, 9));
    r3d_ray miss = {r3d_v3(3, 0, 10), r3d_v3(0, 0, -1)};
    EXPECT(!r3d_ray_aabb(miss, r3d_v3(-1, -1, -1), r3d_v3(1, 1, 1), &t));
    r3d_ray parallel_inside = {r3d_v3(0.5f, 0.5f, 10), r3d_v3(0, 0, -1)};
    EXPECT(r3d_ray_aabb(parallel_inside, r3d_v3(-1, -1, -1), r3d_v3(1, 1, 1), &t) && NEAR(t, 9));

    /* capsule along y from -1 to 1, radius 0.5 */
    r3d_vec3 a = r3d_v3(0, -1, 0), b = r3d_v3(0, 1, 0);
    EXPECT(r3d_ray_capsule(r, a, b, 0.5f, &t) && NEAR(t, 9.5f));               /* the cylinder part */
    r3d_ray cap = {r3d_v3(0, 1.2f, 10), r3d_v3(0, 0, -1)};
    EXPECT(r3d_ray_capsule(cap, a, b, 0.5f, &t) && NEAR(t, 10 - sqrtf(0.25f - 0.04f)));   /* the upper cap */
    r3d_ray past = {r3d_v3(0, 1.6f, 10), r3d_v3(0, 0, -1)};
    EXPECT(!r3d_ray_capsule(past, a, b, 0.5f, &t));
    r3d_ray side_miss = {r3d_v3(0.6f, 0, 10), r3d_v3(0, 0, -1)};
    EXPECT(!r3d_ray_capsule(side_miss, a, b, 0.5f, &t));
    r3d_ray along_axis = {r3d_v3(0, 10, 0), r3d_v3(0, -1, 0)};
    EXPECT(r3d_ray_capsule(along_axis, a, b, 0.5f, &t) && NEAR(t, 8.5f));
    /* a degenerate capsule is a sphere */
    EXPECT(r3d_ray_capsule(r, r3d_v3(0, 0, 0), r3d_v3(0, 0, 0), 2, &t) && NEAR(t, 8));

    /* primitives with transforms */
    r3d_primitive sphere = r3d_primitive_make(R3D_SPHERE, r3d_v3(0, 0, 0), r3d_v3(2, 2, 2), r3d_rgb(255, 255, 255));
    EXPECT(r3d_ray_primitive(r, &sphere, &t) && NEAR(t, 8));
    r3d_primitive cube = r3d_primitive_make(R3D_CUBE, r3d_v3(0, 0, 0), r3d_v3(2, 4, 6), r3d_rgb(255, 255, 255));
    EXPECT(r3d_ray_primitive(r, &cube, &t) && NEAR(t, 7));
    r3d_ray from_side = {r3d_v3(10, 0, 0), r3d_v3(-1, 0, 0)};
    EXPECT(r3d_ray_primitive(from_side, &cube, &t) && NEAR(t, 9));
    /* rotate the cube a quarter turn about y: its 6-long side now faces x */
    cube.rotation = r3d_v3(0, R3D_PI / 2, 0);
    EXPECT(r3d_ray_primitive(from_side, &cube, &t) && NEAR(t, 7));
    EXPECT(r3d_ray_primitive(r, &cube, &t) && NEAR(t, 9));
    r3d_primitive plane = r3d_primitive_make(R3D_PLANE, r3d_v3(0, 0, 0), r3d_v3(4, 1, 4), r3d_rgb(255, 255, 255));
    r3d_ray down = {r3d_v3(1, 5, 1), r3d_v3(0, -1, 0)};
    EXPECT(r3d_ray_primitive(down, &plane, &t) && NEAR(t, 5));
    r3d_ray off = {r3d_v3(3, 5, 1), r3d_v3(0, -1, 0)};
    EXPECT(!r3d_ray_primitive(off, &plane, &t));
    EXPECT(!r3d_ray_primitive(r, &plane, &t));   /* edge-on */
    /* a plane stood up (pitched a quarter turn) faces z */
    plane.rotation = r3d_v3(R3D_PI / 2, 0, 0);
    EXPECT(r3d_ray_primitive(r, &plane, &t) && NEAR(t, 10));
    r3d_primitive capsule = r3d_primitive_make(R3D_CAPSULE, r3d_v3(0, 0, 0), r3d_v3(0.5f, 2, 0.5f), r3d_rgb(255, 255, 255));
    r3d_vec3 ca, cb;
    r3d_capsule_segment(&capsule, &ca, &cb);
    EXPECT(NEAR_V(ca, r3d_v3(0, -1, 0)) && NEAR_V(cb, r3d_v3(0, 1, 0)));
    EXPECT(r3d_ray_primitive(cap, &capsule, &t));
    capsule.rotation = r3d_v3(0, 0, R3D_PI / 2);   /* rolled: lies along x */
    r3d_capsule_segment(&capsule, &ca, &cb);
    EXPECT(NEAR(fabsf(ca.x), 1) && NEAR(ca.y, 0));
    EXPECT(!r3d_ray_primitive(cap, &capsule, &t));
    /* radii and matrices */
    EXPECT(NEAR(r3d_primitive_radius(&sphere), 2));
    EXPECT(NEAR(r3d_primitive_radius(&capsule), 1.5f));
    EXPECT(NEAR(r3d_primitive_radius(&cube), 0.5f * sqrtf(4 + 16 + 36)));
    r3d_mat4 m = r3d_primitive_matrix(&sphere);
    EXPECT(NEAR_V(r3d_mat4_transform_point(&m, r3d_v3(1, 0, 0)), r3d_v3(2, 0, 0)));
    EXPECT(NEAR_V(r3d_primitive_scale(&plane), r3d_v3(4, 1, 4)));
    /* bounding boxes */
    r3d_vec3 lo, hi;
    r3d_primitive_bounds(&sphere, &lo, &hi);
    EXPECT(NEAR_V(lo, r3d_v3(-2, -2, -2)) && NEAR_V(hi, r3d_v3(2, 2, 2)));
    capsule.rotation = r3d_v3(0, 0, 0);
    r3d_primitive_bounds(&capsule, &lo, &hi);
    EXPECT(NEAR_V(lo, r3d_v3(-0.5f, -1.5f, -0.5f)) && NEAR_V(hi, r3d_v3(0.5f, 1.5f, 0.5f)));
    cube.rotation = r3d_v3(0, 0, 0);
    cube.position = r3d_v3(10, 0, 0);
    r3d_primitive_bounds(&cube, &lo, &hi);
    EXPECT(NEAR_V(lo, r3d_v3(9, -2, -3)) && NEAR_V(hi, r3d_v3(11, 2, 3)));
    cube.rotation = r3d_v3(0, R3D_PI / 2, 0);   /* a quarter turn swaps x and z extents */
    r3d_primitive_bounds(&cube, &lo, &hi);
    EXPECT(NEAR_V(lo, r3d_v3(7, -2, -1)) && NEAR_V(hi, r3d_v3(13, 2, 1)));
    plane.rotation = r3d_v3(0, 0, 0);
    r3d_primitive_bounds(&plane, &lo, &hi);
    EXPECT(NEAR_V(lo, r3d_v3(-2, 0, -2)) && NEAR_V(hi, r3d_v3(2, 0, 2)));
}

/* every triangle faces away from the origin (counter-clockwise seen from outside) */
static bool outward(const r3d_mesh *m, bool closed) {
    for (size_t i = 0; i + 2 < m->index_count; i += 3) {
        const r3d_vertex *a = &m->vertices[m->indices[i]], *b = &m->vertices[m->indices[i + 1]], *c = &m->vertices[m->indices[i + 2]];
        r3d_vec3 pa = r3d_v3(a->x, a->y, a->z), pb = r3d_v3(b->x, b->y, b->z), pc = r3d_v3(c->x, c->y, c->z);
        r3d_vec3 n = r3d_cross(r3d_sub(pb, pa), r3d_sub(pc, pa));
        if (r3d_length(n) < 1e-6f) continue;   /* degenerate at the poles */
        r3d_vec3 ref = closed ? r3d_scale(r3d_add(pa, r3d_add(pb, pc)), 1.0f / 3) : r3d_v3(a->nx, a->ny, a->nz);
        if (r3d_dot(n, ref) <= 0) return false;
    }
    return true;
}
static bool indices_in_range(const r3d_mesh *m) {
    for (size_t i = 0; i < m->index_count; ++i) if (m->indices[i] >= m->vertex_count) return false;
    for (size_t i = 0; i < m->edge_index_count; ++i) if (m->edges[i] >= m->vertex_count) return false;
    return true;
}
static void test_meshes(void) {
    r3d_mesh m;
    EXPECT(r3d_mesh_sphere(&m, 8, 12));
    EXPECT(m.vertex_count == 9 * 13 && m.index_count == 8 * 12 * 6);
    bool unit = true, normals = true;
    for (size_t i = 0; i < m.vertex_count; ++i) {
        r3d_vec3 p = r3d_v3(m.vertices[i].x, m.vertices[i].y, m.vertices[i].z);
        if (!NEAR(r3d_length(p), 1)) unit = false;
        if (!NEAR_V(p, r3d_v3(m.vertices[i].nx, m.vertices[i].ny, m.vertices[i].nz))) normals = false;
        if (m.vertices[i].cap != 0) unit = false;
    }
    EXPECT(unit && normals);
    EXPECT(outward(&m, true));
    EXPECT(r3d_mesh_build_edges(&m) && indices_in_range(&m));
    /* every edge once: a closed quad mesh has E = V_unique... check no duplicates and a sane count */
    EXPECT(m.edge_index_count > 0 && m.edge_index_count % 2 == 0 && m.edge_index_count < m.index_count * 2);
    bool dup = false;
    for (size_t i = 0; i + 1 < m.edge_index_count / 2 && !dup; ++i)
        for (size_t j = i + 1; j < m.edge_index_count / 2; ++j)
            if (m.edges[i * 2] == m.edges[j * 2] && m.edges[i * 2 + 1] == m.edges[j * 2 + 1]) { dup = true; break; }
    EXPECT(!dup);
    r3d_mesh_free(&m);
    EXPECT(!r3d_mesh_sphere(&m, 1, 12));

    EXPECT(r3d_mesh_capsule(&m, 4, 12));
    EXPECT(m.vertex_count == 10 * 13);
    bool caps_ok = true, top_half = true;
    int plus = 0, minus = 0;
    for (size_t i = 0; i < m.vertex_count; ++i) {
        const r3d_vertex *v = &m.vertices[i];
        if (v->cap == 1) { ++plus; if (v->y < -1e-5f) top_half = false; }
        else if (v->cap == -1) { ++minus; if (v->y > 1e-5f) top_half = false; }
        else caps_ok = false;
        if (!NEAR(r3d_length(r3d_v3(v->x, v->y, v->z)), 1)) caps_ok = false;
    }
    EXPECT(caps_ok && top_half && plus == minus && plus == 5 * 13);
    EXPECT(outward(&m, true));
    EXPECT(r3d_mesh_build_edges(&m) && indices_in_range(&m));
    r3d_mesh_free(&m);

    EXPECT(r3d_mesh_cube(&m));
    EXPECT(m.vertex_count == 24 && m.index_count == 36);
    bool corners = true;
    for (size_t i = 0; i < m.vertex_count; ++i) {
        const r3d_vertex *v = &m.vertices[i];
        if (!NEAR(fabsf(v->x), 0.5f) || !NEAR(fabsf(v->y), 0.5f) || !NEAR(fabsf(v->z), 0.5f)) corners = false;
        /* the normal points along the face's axis and agrees with the position */
        r3d_vec3 n = r3d_v3(v->nx, v->ny, v->nz);
        if (!NEAR(r3d_length(n), 1) || r3d_dot(n, r3d_v3(v->x, v->y, v->z)) <= 0.49f) corners = false;
    }
    EXPECT(corners);
    EXPECT(outward(&m, true));
    EXPECT(r3d_mesh_build_edges(&m) && m.edge_index_count == 2 * 6 * 5 && indices_in_range(&m));   /* faces share no vertices: 4 sides + a diagonal each */
    r3d_mesh_free(&m);

    EXPECT(r3d_mesh_plane(&m));
    EXPECT(m.vertex_count == 4 && m.index_count == 6);
    bool flat = true;
    for (size_t i = 0; i < m.vertex_count; ++i)
        if (!NEAR(m.vertices[i].y, 0) || !NEAR(m.vertices[i].ny, 1) || !NEAR(fabsf(m.vertices[i].x), 0.5f) || !NEAR(fabsf(m.vertices[i].z), 0.5f)) flat = false;
    EXPECT(flat);
    EXPECT(outward(&m, false));   /* faces +y */
    EXPECT(r3d_mesh_build_edges(&m) && m.edge_index_count == 10);
    r3d_mesh_free(&m);
    EXPECT(m.vertices == NULL && m.vertex_count == 0);
}

int main(void) {
    EXPECT(R3D_SHADOW_LAYERS == R3D_MAX_SPOT_SHADOWS + 6 * R3D_MAX_POINT_SHADOWS && R3D_SHADOW_LAYERS <= 16);   /* the fragment shader's local_matrices array */
    test_vectors();
    test_matrices();
    test_look_at_and_projection();
    test_camera();
    test_project_and_rays();
    test_ray_casts();
    test_meshes();
    if (failures) { printf("%d of %d render3d checks failed\n", failures, checks); return 1; }
    printf("OK: %d render3d checks passed\n", checks);
    return 0;
}
