#include "cw_brush.h"
#include <math.h>
#include <string.h>

#define EPS 1e-5f

const char *const cw_brush_face_names[CW_BRUSH_MAX_FACES] = {"east", "west", "top", "bottom", "south", "north", "clip 1", "clip 2", "clip 3", "clip 4"};
static const cw_field brush_fields[] = {
    {"width", CW_FIELD_F32, offsetof(cw_brush, width), 0.01f, 10000, 0.25f, NULL, 0, CW_SEM_SIZE_X, 0},
    {"height", CW_FIELD_F32, offsetof(cw_brush, height), 0.01f, 10000, 0.25f, NULL, 0, CW_SEM_SIZE_Y, 0},
    {"depth", CW_FIELD_F32, offsetof(cw_brush, depth), 0.01f, 10000, 0.25f, NULL, 0, CW_SEM_SIZE_Z, 0},
    {"mat_east", CW_FIELD_U32, offsetof(cw_brush, material[CW_FACE_EAST]), 0, 0, 0, NULL, 0, CW_SEM_ASSET, 0},
    {"mat_west", CW_FIELD_U32, offsetof(cw_brush, material[CW_FACE_WEST]), 0, 0, 0, NULL, 0, CW_SEM_ASSET, 0},
    {"mat_top", CW_FIELD_U32, offsetof(cw_brush, material[CW_FACE_TOP]), 0, 0, 0, NULL, 0, CW_SEM_ASSET, 0},
    {"mat_bottom", CW_FIELD_U32, offsetof(cw_brush, material[CW_FACE_BOTTOM]), 0, 0, 0, NULL, 0, CW_SEM_ASSET, 0},
    {"mat_south", CW_FIELD_U32, offsetof(cw_brush, material[CW_FACE_SOUTH]), 0, 0, 0, NULL, 0, CW_SEM_ASSET, 0},
    {"mat_north", CW_FIELD_U32, offsetof(cw_brush, material[CW_FACE_NORTH]), 0, 0, 0, NULL, 0, CW_SEM_ASSET, 0},
    {"mat_clip1", CW_FIELD_U32, offsetof(cw_brush, material[CW_BRUSH_FACES + 0]), 0, 0, 0, NULL, 0, CW_SEM_ASSET, 0},
    {"mat_clip2", CW_FIELD_U32, offsetof(cw_brush, material[CW_BRUSH_FACES + 1]), 0, 0, 0, NULL, 0, CW_SEM_ASSET, 0},
    {"mat_clip3", CW_FIELD_U32, offsetof(cw_brush, material[CW_BRUSH_FACES + 2]), 0, 0, 0, NULL, 0, CW_SEM_ASSET, 0},
    {"mat_clip4", CW_FIELD_U32, offsetof(cw_brush, material[CW_BRUSH_FACES + 3]), 0, 0, 0, NULL, 0, CW_SEM_ASSET, 0},
    CW_FIELD_RANGE(cw_brush, texture_scale, CW_FIELD_F32, 0.05f, 100, 0.25f),
    CW_FIELD_RANGE(cw_brush, subdivisions, CW_FIELD_U8, 1, CW_BRUSH_MAX_SUBDIVISIONS, 1),
    CW_FIELD_SEM(cw_brush, r, CW_FIELD_U8, CW_SEM_COLOR_R),
    CW_FIELD_SEM(cw_brush, g, CW_FIELD_U8, CW_SEM_COLOR_G),
    CW_FIELD_SEM(cw_brush, b, CW_FIELD_U8, CW_SEM_COLOR_B),
    CW_FIELD_BLOB(cw_brush, clips),
    CW_FIELD_BLOB(cw_brush, align),
    CW_FIELD_BLOB(cw_brush, paint),
};
static const cw_brush brush_defaults = {1, 1, 1, {0}, 1, 2, 255, 255, 255, {0, {0}}, {0, {0}}, {0, {0}}};
const cw_type_info cw_brush_type = {"Brush", sizeof(cw_brush), _Alignof(cw_brush), brush_fields, sizeof(brush_fields) / sizeof(brush_fields[0]), &brush_defaults, NULL, 0, NULL, NULL};

/* --- small vector helpers --- */
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
static float normalize3(float v[3]) {
    float len = sqrtf(dot3(v, v));
    if (len > 0) { v[0] /= len; v[1] /= len; v[2] /= len; }
    return len;
}
static int dominant_axis(const float n[3]) {
    float ax = fabsf(n[0]), ay = fabsf(n[1]), az = fabsf(n[2]);
    return ax >= ay && ax >= az ? 0 : (ay >= az ? 1 : 2);
}

/* --- the solid --- */
void cw_brush_bounds(const cw_brush *b, const float center[3], float lo[3], float hi[3]) {
    float half[3] = {fabsf(b->width) * 0.5f, fabsf(b->height) * 0.5f, fabsf(b->depth) * 0.5f};
    for (int i = 0; i < 3; ++i) { lo[i] = center[i] - half[i]; hi[i] = center[i] + half[i]; }
}
static void clip_plane_local(const cw_brush *b, int i, float n[3], float *d);
static void clip_plane_store(cw_brush *b, int i, const float n[3], float d);
static float dot3(const float a[3], const float b[3]);
void cw_brush_set_bounds(cw_brush *b, float center[3], const float lo[3], const float hi[3], float min_size) {
    float size[3], old[3] = {center[0], center[1], center[2]};
    for (int i = 0; i < 3; ++i) {
        float a = lo[i] < hi[i] ? lo[i] : hi[i], c = lo[i] < hi[i] ? hi[i] : lo[i];
        if (c - a < min_size) c = a + min_size;
        size[i] = c - a;
        center[i] = (a + c) * 0.5f;
    }
    b->width = size[0];
    b->height = size[1];
    b->depth = size[2];
    /* the clip planes stay where they are in the world (d_world = d_local + n . centre) */
    int clips = cw_brush_clip_count(b);
    for (int i = 0; i < clips; ++i) {
        float pn[3], d;
        clip_plane_local(b, i, pn, &d);
        d += dot3(pn, old) - dot3(pn, center);
        clip_plane_store(b, i, pn, d);
    }
}
int cw_brush_clip_count(const cw_brush *b) {
    int n = (int)(b->clips.size / (4 * sizeof(float)));
    return n > CW_BRUSH_MAX_CLIPS ? CW_BRUSH_MAX_CLIPS : n;
}
int cw_brush_face_count(const cw_brush *b) { return CW_BRUSH_FACES + cw_brush_clip_count(b); }
static void clip_plane_local(const cw_brush *b, int i, float n[3], float *d) {
    float v[4];
    memcpy(v, b->clips.data + (size_t)i * 4 * sizeof(float), sizeof(v));
    n[0] = v[0]; n[1] = v[1]; n[2] = v[2];
    *d = v[3];
}
static void clip_plane_store(cw_brush *b, int i, const float n[3], float d) {
    float v[4] = {n[0], n[1], n[2], d};
    memcpy(b->clips.data + (size_t)i * 4 * sizeof(float), v, sizeof(v));
}
bool cw_brush_face_plane(const cw_brush *b, const float center[3], int face, cw_plane *out) {
    if (face < 0 || face >= cw_brush_face_count(b)) return false;
    if (face < CW_BRUSH_FACES) {
        float half[3] = {fabsf(b->width) * 0.5f, fabsf(b->height) * 0.5f, fabsf(b->depth) * 0.5f};
        int axis = cw_brush_face_axis((cw_brush_face)face);
        float sign = cw_brush_face_sign((cw_brush_face)face);
        out->n[0] = out->n[1] = out->n[2] = 0;
        out->n[axis] = sign;
        out->d = sign * center[axis] + half[axis];
        return true;
    }
    float d;
    clip_plane_local(b, face - CW_BRUSH_FACES, out->n, &d);
    out->d = d + dot3(out->n, center);
    return true;
}
int cw_brush_planes(const cw_brush *b, const float center[3], cw_plane out[CW_BRUSH_MAX_FACES]) {
    int n = cw_brush_face_count(b);
    for (int i = 0; i < n; ++i) cw_brush_face_plane(b, center, i, &out[i]);
    return n;
}
/* Sutherland-Hodgman: keeps the part of a polygon with n . p <= d. */
static void clip_polygon(cw_brush_polygon *poly, const float n[3], float d) {
    cw_brush_polygon out;
    out.count = 0;
    for (int i = 0; i < poly->count; ++i) {
        const float *a = poly->p[i], *c = poly->p[(i + 1) % poly->count];
        float da = dot3(n, a) - d, dc = dot3(n, c) - d;
        bool ina = da <= EPS, inc = dc <= EPS;
        if (ina && out.count < CW_BRUSH_MAX_POLYGON) memcpy(out.p[out.count++], a, sizeof(float) * 3);
        if (ina != inc && fabsf(da - dc) > 1e-12f && out.count < CW_BRUSH_MAX_POLYGON) {
            float t = da / (da - dc);
            for (int k = 0; k < 3; ++k) out.p[out.count][k] = a[k] + (c[k] - a[k]) * t;
            ++out.count;
        }
    }
    /* drop duplicate consecutive points */
    cw_brush_polygon clean;
    clean.count = 0;
    for (int i = 0; i < out.count; ++i) {
        const float *prev = clean.count ? clean.p[clean.count - 1] : out.p[out.count - 1];
        float dx = out.p[i][0] - prev[0], dy = out.p[i][1] - prev[1], dz = out.p[i][2] - prev[2];
        if (clean.count && dx * dx + dy * dy + dz * dz < EPS * EPS) continue;
        memcpy(clean.p[clean.count++], out.p[i], sizeof(float) * 3);
    }
    if (clean.count > 1) {   /* the last against the first */
        float dx = clean.p[clean.count - 1][0] - clean.p[0][0], dy = clean.p[clean.count - 1][1] - clean.p[0][1], dz = clean.p[clean.count - 1][2] - clean.p[0][2];
        if (dx * dx + dy * dy + dz * dz < EPS * EPS) --clean.count;
    }
    *poly = clean;
}
static void polygon_normal(const cw_brush_polygon *poly, float n[3]) {   /* Newell */
    n[0] = n[1] = n[2] = 0;
    for (int i = 0; i < poly->count; ++i) {
        const float *a = poly->p[i], *b = poly->p[(i + 1) % poly->count];
        n[0] += (a[1] - b[1]) * (a[2] + b[2]);
        n[1] += (a[2] - b[2]) * (a[0] + b[0]);
        n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
}
static void tangent_basis(const float n[3], float t1[3], float t2[3]) {
    float helper[3] = {0, 0, 0};
    helper[fabsf(n[1]) < 0.9f ? 1 : 0] = 1;
    cross3(n, helper, t1);
    normalize3(t1);
    cross3(n, t1, t2);
}
bool cw_brush_face_polygon(const cw_brush *b, const float center[3], int face, cw_brush_polygon *out) {
    cw_plane planes[CW_BRUSH_MAX_FACES];
    int count = cw_brush_planes(b, center, planes);
    out->count = 0;
    if (face < 0 || face >= count) return false;
    const cw_plane *p = &planes[face];
    float extent = sqrtf(b->width * b->width + b->height * b->height + b->depth * b->depth) + 1;
    float p0[3], t1[3], t2[3];
    float offset = p->d - dot3(p->n, center);
    for (int k = 0; k < 3; ++k) p0[k] = center[k] + p->n[k] * offset;
    tangent_basis(p->n, t1, t2);
    static const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    out->count = 4;
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 3; ++k) out->p[i][k] = p0[k] + (t1[k] * corners[i][0] + t2[k] * corners[i][1]) * extent;
    for (int i = 0; i < count && out->count >= 3; ++i) {
        if (i == face) continue;
        clip_polygon(out, planes[i].n, planes[i].d);
    }
    if (out->count < 3) { out->count = 0; return true; }
    float n[3];
    polygon_normal(out, n);
    if (dot3(n, n) < EPS * EPS) { out->count = 0; return true; }   /* a sliver */
    if (dot3(n, p->n) < 0) {   /* counter-clockwise seen from outside */
        for (int i = 0; i < out->count / 2; ++i) {
            float tmp[3];
            memcpy(tmp, out->p[i], sizeof(tmp));
            memcpy(out->p[i], out->p[out->count - 1 - i], sizeof(tmp));
            memcpy(out->p[out->count - 1 - i], tmp, sizeof(tmp));
        }
    }
    return true;
}
/* Every vertex of the solid (the union of its face polygons); returns the count. */
static int solid_vertices(const cw_brush *b, const float center[3], float out[CW_BRUSH_MAX_FACES * CW_BRUSH_MAX_POLYGON][3]) {
    int n = 0, faces = cw_brush_face_count(b);
    for (int f = 0; f < faces; ++f) {
        cw_brush_polygon poly;
        cw_brush_face_polygon(b, center, f, &poly);
        for (int i = 0; i < poly.count; ++i) memcpy(out[n++], poly.p[i], sizeof(float) * 3);
    }
    return n;
}
int cw_brush_plane_side(const cw_brush *b, const float center[3], const cw_plane *plane) {
    float verts[CW_BRUSH_MAX_FACES * CW_BRUSH_MAX_POLYGON][3];
    int n = solid_vertices(b, center, verts);
    bool inside = false, outside = false;
    for (int i = 0; i < n; ++i) {
        float s = dot3(plane->n, verts[i]) - plane->d;
        if (s > 1e-3f) outside = true;
        else if (s < -1e-3f) inside = true;
    }
    if (inside && outside) return 0;
    return outside ? 1 : -1;
}
void cw_brush_clear_clips(cw_brush *b) { memset(&b->clips, 0, sizeof(b->clips)); }
/* After the planes changed: the box shrinks to the solid, the centre moves, the clips follow. */
static void shrink_to_solid(cw_brush *b, float center[3]) {
    float verts[CW_BRUSH_MAX_FACES * CW_BRUSH_MAX_POLYGON][3];
    int n = solid_vertices(b, center, verts);
    if (n < 4) return;
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) {
            if (verts[i][k] < lo[k]) lo[k] = verts[i][k];
            if (verts[i][k] > hi[k]) hi[k] = verts[i][k];
        }
    cw_brush_set_bounds(b, center, lo, hi, 0.01f);
}
bool cw_brush_clip(cw_brush *b, float center[3], const cw_plane *plane) {
    float n[3] = {plane->n[0], plane->n[1], plane->n[2]};
    if (normalize3(n) < EPS) return false;
    cw_plane p = {{n[0], n[1], n[2]}, plane->d / sqrtf(dot3(plane->n, plane->n))};
    if (cw_brush_plane_side(b, center, &p) != 0) return false;   /* misses or removes everything */
    int axis = dominant_axis(n);
    if (fabsf(n[axis]) > 1 - 1e-4f) {
        /* an axis-aligned cut is a resize of the box */
        float lo[3], hi[3];
        cw_brush_bounds(b, center, lo, hi);
        if (n[axis] > 0) hi[axis] = p.d; else lo[axis] = -p.d;
        if (hi[axis] - lo[axis] < 0.01f) return false;
        cw_brush_set_bounds(b, center, lo, hi, 0.01f);
        shrink_to_solid(b, center);
        return true;
    }
    int count = cw_brush_clip_count(b);
    if (count >= CW_BRUSH_MAX_CLIPS) return false;
    clip_plane_store(b, count, n, p.d - dot3(n, center));
    b->clips.size = (uint16_t)((count + 1) * 4 * sizeof(float));
    shrink_to_solid(b, center);
    return true;
}
bool cw_brush_make_wedge(cw_brush *b, float center[3], int axis, float sign) {
    if (axis != 0 && axis != 2) return false;
    sign = sign < 0 ? -1.0f : 1.0f;
    float half_u = (axis == 0 ? fabsf(b->width) : fabsf(b->depth)) * 0.5f, half_h = fabsf(b->height) * 0.5f;
    /* the diagonal from the bottom of the near end to the top of the far end; the solid keeps the bottom-far corner */
    float n[3] = {0, 0, 0};
    n[axis] = -half_h * sign;
    n[1] = half_u;
    cw_plane p;
    normalize3(n);
    memcpy(p.n, n, sizeof(n));
    p.d = dot3(n, center);   /* through the centre */
    return cw_brush_clip(b, center, &p);
}

bool cw_brush_set_clip_plane(cw_brush *b, const float center[3], int index, const cw_plane *plane) {
    if (index < 0 || index >= cw_brush_clip_count(b)) return false;
    float n[3] = {plane->n[0], plane->n[1], plane->n[2]};
    float len = normalize3(n);
    if (len < EPS) return false;
    clip_plane_store(b, index, n, plane->d / len - dot3(n, center));
    return true;
}

/* --- CSG --- */
bool cw_brush_intersects(const cw_brush *a, const float center[3], const cw_plane *planes, int count) {
    /* a is inside every plane's solid side at least partly: no plane puts it entirely outside */
    cw_brush work = *a;
    float c[3] = {center[0], center[1], center[2]};
    for (int i = 0; i < count; ++i) {
        int side = cw_brush_plane_side(&work, c, &planes[i]);
        if (side > 0) return false;
        if (side == 0 && !cw_brush_clip(&work, c, &planes[i])) return false;   /* could not keep the inside part */
    }
    return true;
}
int cw_brush_subtract(const cw_brush *a, const float center[3], const cw_plane *cutter, int cutter_count, cw_brush *pieces, float (*centers)[3], int *dropped) {
    if (dropped) *dropped = 0;
    if (!cw_brush_intersects(a, center, cutter, cutter_count)) return -1;
    cw_brush remaining = *a;
    float rc[3] = {center[0], center[1], center[2]};
    int count = 0;
    for (int i = 0; i < cutter_count; ++i) {
        int side = cw_brush_plane_side(&remaining, rc, &cutter[i]);
        if (side < 0) continue;   /* nothing of the remainder lies outside this plane */
        if (side > 0) {           /* the remainder lies outside the cutter: it is a piece, and the last */
            if (count < CW_BRUSH_MAX_FACES) { pieces[count] = remaining; memcpy(centers[count], rc, sizeof(rc)); ++count; }
            return count;
        }
        cw_brush piece = remaining;
        float pc[3] = {rc[0], rc[1], rc[2]};
        cw_plane outside = {{-cutter[i].n[0], -cutter[i].n[1], -cutter[i].n[2]}, -cutter[i].d};
        if (cw_brush_clip(&piece, pc, &outside)) {
            if (count < CW_BRUSH_MAX_FACES) { pieces[count] = piece; memcpy(centers[count], pc, sizeof(pc)); ++count; }
        } else if (dropped) ++*dropped;
        if (!cw_brush_clip(&remaining, rc, &cutter[i])) break;   /* the remainder could not take another plane: stop here */
    }
    return count;
}
int cw_brush_hollow(const cw_brush *a, const float center[3], float thickness, cw_brush *pieces, float (*centers)[3], int *dropped) {
    cw_plane inner[CW_BRUSH_MAX_FACES];
    int n = cw_brush_planes(a, center, inner);
    for (int i = 0; i < n; ++i) inner[i].d -= thickness;
    int count = cw_brush_subtract(a, center, inner, n, pieces, centers, dropped);
    return count < 0 ? 0 : count;
}

/* --- keeping textures and vertices in place --- */
void cw_brush_lock_textures(cw_brush *b, const float delta[3]) {
    if (fabsf(delta[0]) < EPS && fabsf(delta[1]) < EPS && fabsf(delta[2]) < EPS) return;
    float scale = b->texture_scale > 1e-4f ? b->texture_scale : 1;
    float zero[3] = {0, 0, 0};
    int faces = cw_brush_face_count(b);
    for (int f = 0; f < faces; ++f) {
        cw_plane p;
        if (!cw_brush_face_plane(b, zero, f, &p)) continue;
        int ua, va;
        cw_brush_normal_tangents(p.n, &ua, &va);
        /* the projection's shift for this move, through the face's rotation and scale */
        float du = delta[ua] / scale, dv = -delta[va] / scale;
        cw_brush_align a;
        cw_brush_face_align_get(b, f, &a);
        float rad = a.rotation * 3.14159265f / 180.0f, c = cosf(rad), s = sinf(rad);
        float ru = du * c - dv * s, rv = du * s + dv * c;
        a.shift_u -= ru / a.scale_u;
        a.shift_v -= rv / a.scale_v;
        cw_brush_face_align_set(b, f, &a);
    }
}
static float snap_value(float v, float spacing) { return spacing > 0 ? roundf(v / spacing) * spacing : v; }
static bool plane_from_points(const float a[3], const float b[3], const float c[3], const float like[3], cw_plane *out) {
    float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, ac[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]}, n[3];
    cross3(ab, ac, n);
    if (normalize3(n) < EPS) return false;
    if (dot3(n, like) < 0) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }   /* keep the orientation of the plane it replaces */
    memcpy(out->n, n, sizeof(n));
    out->d = dot3(n, a);
    return true;
}
bool cw_brush_snap_to_grid(cw_brush *b, float center[3], float spacing) {
    if (spacing <= 0) return false;
    cw_brush before = *b;
    float before_c[3] = {center[0], center[1], center[2]};
    /* the clip planes first, from the snapped corners of their polygons (the box is still where they were fitted) */
    int clips = cw_brush_clip_count(b);
    for (int i = 0; i < clips; ++i) {
        cw_brush_polygon poly;
        cw_plane p;
        if (!cw_brush_face_polygon(&before, before_c, CW_BRUSH_FACES + i, &poly) || poly.count < 3 || !cw_brush_face_plane(&before, before_c, CW_BRUSH_FACES + i, &p)) continue;
        float q[3][3];
        /* three corners spread around the polygon, snapped */
        int picks[3] = {0, poly.count / 3, (poly.count * 2) / 3};
        for (int k = 0; k < 3; ++k)
            for (int axis = 0; axis < 3; ++axis) q[k][axis] = snap_value(poly.p[picks[k]][axis], spacing);
        cw_plane fitted;
        if (plane_from_points(q[0], q[1], q[2], p.n, &fitted)) cw_brush_set_clip_plane(b, center, i, &fitted);
    }
    float lo[3], hi[3];
    cw_brush_bounds(b, center, lo, hi);
    for (int axis = 0; axis < 3; ++axis) {
        lo[axis] = snap_value(lo[axis], spacing);
        hi[axis] = snap_value(hi[axis], spacing);
        if (hi[axis] - lo[axis] < spacing * 0.5f) hi[axis] = lo[axis] + spacing;
    }
    cw_brush_set_bounds(b, center, lo, hi, 0.01f);
    return memcmp(&before, b, sizeof(*b)) != 0 || memcmp(before_c, center, sizeof(before_c)) != 0;
}
int cw_brush_vertices(const cw_brush *b, const float center[3], float (*out)[3], int capacity) {
    int n = 0, faces = cw_brush_face_count(b);
    for (int f = 0; f < faces; ++f) {
        cw_brush_polygon poly;
        cw_brush_face_polygon(b, center, f, &poly);
        for (int i = 0; i < poly.count; ++i) {
            bool seen = false;
            for (int k = 0; k < n && !seen; ++k) {
                float dx = out[k][0] - poly.p[i][0], dy = out[k][1] - poly.p[i][1], dz = out[k][2] - poly.p[i][2];
                seen = dx * dx + dy * dy + dz * dz < 1e-6f;
            }
            if (seen) continue;
            if (n >= capacity) return n;
            memcpy(out[n++], poly.p[i], sizeof(float) * 3);
        }
    }
    return n;
}
bool cw_brush_move_vertex(cw_brush *b, float center[3], const float from[3], const float to[3]) {
    cw_plane planes[CW_BRUSH_MAX_FACES];
    int n = cw_brush_planes(b, center, planes);
    bool on_any = false;
    cw_brush before = *b;
    float before_c[3] = {center[0], center[1], center[2]};
    /* the clip planes through the vertex: re-fitted through the moved vertex and its two polygon neighbours */
    for (int i = CW_BRUSH_FACES; i < n; ++i) {
        if (fabsf(dot3(planes[i].n, from) - planes[i].d) > 1e-3f) continue;
        cw_brush_polygon poly;
        if (!cw_brush_face_polygon(&before, before_c, i, &poly) || poly.count < 3) continue;
        int at = -1;
        for (int k = 0; k < poly.count && at < 0; ++k) {
            float dx = poly.p[k][0] - from[0], dy = poly.p[k][1] - from[1], dz = poly.p[k][2] - from[2];
            if (dx * dx + dy * dy + dz * dz < 1e-6f) at = k;
        }
        if (at < 0) continue;
        cw_plane fitted;
        if (plane_from_points(to, poly.p[(at + 1) % poly.count], poly.p[(at + poly.count - 1) % poly.count], planes[i].n, &fitted)) {
            cw_brush_set_clip_plane(b, center, i - CW_BRUSH_FACES, &fitted);
            on_any = true;
        }
    }
    /* the box faces through the vertex follow it along their axis */
    float lo[3], hi[3];
    cw_brush_bounds(b, center, lo, hi);
    for (int f = 0; f < CW_BRUSH_FACES; ++f) {
        int axis = cw_brush_face_axis((cw_brush_face)f);
        float plane = cw_brush_face_sign((cw_brush_face)f) > 0 ? hi[axis] : lo[axis];
        if (fabsf(from[axis] - plane) > 1e-3f) continue;
        if (cw_brush_face_sign((cw_brush_face)f) > 0) hi[axis] = to[axis] > lo[axis] + 0.05f ? to[axis] : lo[axis] + 0.05f;
        else lo[axis] = to[axis] < hi[axis] - 0.05f ? to[axis] : hi[axis] - 0.05f;
        on_any = true;
    }
    if (!on_any) return false;
    cw_brush_set_bounds(b, center, lo, hi, 0.05f);
    return true;
}

/* --- faces --- */
int cw_brush_face_axis(cw_brush_face face) { return (int)face / 2; }
float cw_brush_face_sign(cw_brush_face face) { return ((int)face & 1) ? -1.0f : 1.0f; }
void cw_brush_face_normal(cw_brush_face face, float n[3]) {
    n[0] = n[1] = n[2] = 0;
    n[cw_brush_face_axis(face)] = cw_brush_face_sign(face);
}
void cw_brush_normal_tangents(const float n[3], int *u_axis, int *v_axis) {
    /* textures run along the world axes: walls keep v vertical, the top and bottom use x and z */
    switch (dominant_axis(n)) {
        case 0: *u_axis = 2; *v_axis = 1; break;
        case 1: *u_axis = 0; *v_axis = 2; break;
        default: *u_axis = 0; *v_axis = 1; break;
    }
}
void cw_brush_face_tangents(cw_brush_face face, int *u_axis, int *v_axis) {
    float n[3];
    cw_brush_face_normal(face, n);
    cw_brush_normal_tangents(n, u_axis, v_axis);
}
cw_brush_face cw_brush_face_at(const float lo[3], const float hi[3], const float point[3]) {
    cw_brush_face best = CW_FACE_TOP;
    float best_d = INFINITY;
    for (int f = 0; f < CW_BRUSH_FACES; ++f) {
        int axis = f / 2;
        float plane = (f & 1) ? lo[axis] : hi[axis];
        float d = fabsf(point[axis] - plane);
        if (d < best_d) { best_d = d; best = (cw_brush_face)f; }
    }
    return best;
}
int cw_brush_face_of_point(const cw_brush *b, const float center[3], const float point[3]) {
    cw_plane planes[CW_BRUSH_MAX_FACES];
    int n = cw_brush_planes(b, center, planes), best = -1;
    float best_d = INFINITY;
    for (int i = 0; i < n; ++i) {
        cw_brush_polygon poly;
        cw_brush_face_polygon(b, center, i, &poly);
        if (!poly.count) continue;   /* clipped away */
        float d = fabsf(dot3(planes[i].n, point) - planes[i].d);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}
cw_brush_align cw_brush_align_default(void) { cw_brush_align a = {0, 0, 0, 1, 1}; return a; }
void cw_brush_face_align_get(const cw_brush *b, int face, cw_brush_align *out) {
    *out = cw_brush_align_default();
    size_t need = ((size_t)face + 1) * CW_BRUSH_ALIGN_FLOATS * sizeof(float);
    if (face < 0 || face >= CW_BRUSH_MAX_FACES || (size_t)b->align.size < need) return;
    float v[CW_BRUSH_ALIGN_FLOATS];
    memcpy(v, b->align.data + (size_t)face * CW_BRUSH_ALIGN_FLOATS * sizeof(float), sizeof(v));
    out->shift_u = v[0]; out->shift_v = v[1]; out->rotation = v[2];
    out->scale_u = fabsf(v[3]) > 1e-4f ? v[3] : 1; out->scale_v = fabsf(v[4]) > 1e-4f ? v[4] : 1;
}
void cw_brush_face_align_set(cw_brush *b, int face, const cw_brush_align *align) {
    if (face < 0 || face >= CW_BRUSH_MAX_FACES) return;
    if (!b->align.size) {   /* first use: every face at the defaults */
        cw_brush_align def = cw_brush_align_default();
        for (int f = 0; f < CW_BRUSH_MAX_FACES; ++f) {
            float v[CW_BRUSH_ALIGN_FLOATS] = {def.shift_u, def.shift_v, def.rotation, def.scale_u, def.scale_v};
            memcpy(b->align.data + (size_t)f * CW_BRUSH_ALIGN_FLOATS * sizeof(float), v, sizeof(v));
        }
        b->align.size = (uint16_t)(CW_BRUSH_MAX_FACES * CW_BRUSH_ALIGN_FLOATS * sizeof(float));
    }
    float v[CW_BRUSH_ALIGN_FLOATS] = {align->shift_u, align->shift_v, align->rotation, fabsf(align->scale_u) > 1e-4f ? align->scale_u : 1, fabsf(align->scale_v) > 1e-4f ? align->scale_v : 1};
    memcpy(b->align.data + (size_t)face * CW_BRUSH_ALIGN_FLOATS * sizeof(float), v, sizeof(v));
    /* back to empty when everything is default again */
    cw_brush_align def = cw_brush_align_default();
    bool all_default = true;
    for (int f = 0; f < CW_BRUSH_MAX_FACES && all_default; ++f) {
        cw_brush_align a;
        cw_brush_face_align_get(b, f, &a);
        if (memcmp(&a, &def, sizeof(a)) != 0) all_default = false;
    }
    if (all_default) memset(&b->align, 0, sizeof(b->align));
}
void cw_brush_face_uv(const cw_brush *b, int face, const float n[3], const float point[3], float *u, float *v) {
    int ua, va;
    cw_brush_normal_tangents(n, &ua, &va);
    float scale = b->texture_scale > 1e-4f ? b->texture_scale : 1;
    float u0 = point[ua] / scale, v0 = -point[va] / scale;   /* textures stand upright: v grows downwards in image space */
    cw_brush_align a;
    cw_brush_face_align_get(b, face, &a);
    float rad = a.rotation * 3.14159265f / 180.0f, c = cosf(rad), s = sinf(rad);
    float ru = u0 * c - v0 * s, rv = u0 * s + v0 * c;
    *u = ru / a.scale_u + a.shift_u;
    *v = rv / a.scale_v + a.shift_v;
}

/* --- the paint lattice --- */
int cw_brush_subdivisions(const cw_brush *b) {
    int s = b->subdivisions;
    return s < 1 ? 1 : (s > CW_BRUSH_MAX_SUBDIVISIONS ? CW_BRUSH_MAX_SUBDIVISIONS : s);
}
size_t cw_brush_paint_size(const cw_brush *b) {
    int s = cw_brush_subdivisions(b);
    return (size_t)cw_brush_face_count(b) * (size_t)(s + 1) * (size_t)(s + 1) * 3;
}
bool cw_brush_painted(const cw_brush *b) { return b->paint.size && (size_t)b->paint.size == cw_brush_paint_size(b); }
static size_t paint_index(int s, int face, int i, int j) {
    return ((size_t)face * (size_t)(s + 1) * (size_t)(s + 1) + (size_t)j * (size_t)(s + 1) + (size_t)i) * 3;
}
void cw_brush_paint_get(const cw_brush *b, int face, int i, int j, uint8_t rgb[3]) {
    int s = cw_brush_subdivisions(b);
    if (!cw_brush_painted(b) || face < 0 || face >= cw_brush_face_count(b) || i < 0 || j < 0 || i > s || j > s) { rgb[0] = rgb[1] = rgb[2] = 255; return; }
    memcpy(rgb, b->paint.data + paint_index(s, face, i, j), 3);
}
void cw_brush_paint_clear(cw_brush *b) { memset(&b->paint, 0, sizeof(b->paint)); }
/* The rectangle a face's lattice spans, in its (u, v) world axes: the box
 * face itself for box faces (stable under clipping), the polygon's bounds
 * for clip faces. False for a face with no area. */
typedef struct { cw_plane plane; int ua, va, na; float u0, u1, v0, v1; } face_frame;
static bool face_lattice_frame(const cw_brush *b, const float center[3], int face, const cw_brush_polygon *poly, face_frame *out) {
    if (!cw_brush_face_plane(b, center, face, &out->plane)) return false;
    cw_brush_normal_tangents(out->plane.n, &out->ua, &out->va);
    out->na = dominant_axis(out->plane.n);
    if (face < CW_BRUSH_FACES) {
        float lo[3], hi[3];
        cw_brush_bounds(b, center, lo, hi);
        out->u0 = lo[out->ua]; out->u1 = hi[out->ua];
        out->v0 = lo[out->va]; out->v1 = hi[out->va];
    } else {
        if (!poly || poly->count < 3) return false;
        out->u0 = out->v0 = INFINITY; out->u1 = out->v1 = -INFINITY;
        for (int i = 0; i < poly->count; ++i) {
            float u = poly->p[i][out->ua], v = poly->p[i][out->va];
            if (u < out->u0) out->u0 = u;
            if (u > out->u1) out->u1 = u;
            if (v < out->v0) out->v0 = v;
            if (v > out->v1) out->v1 = v;
        }
    }
    return out->u1 - out->u0 > EPS && out->v1 - out->v0 > EPS;
}
/* The world point of (u, v) on the frame's plane. */
static void frame_point(const face_frame *f, float u, float v, float out[3]) {
    out[f->ua] = u;
    out[f->va] = v;
    out[f->na] = (f->plane.d - f->plane.n[f->ua] * u - f->plane.n[f->va] * v) / f->plane.n[f->na];
}
static void lattice_point(const face_frame *f, int s, int i, int j, float out[3]) {
    frame_point(f, f->u0 + (f->u1 - f->u0) * (float)i / (float)s, f->v0 + (f->v1 - f->v0) * (float)j / (float)s, out);
}
static void vertex_colour(const cw_brush *b, bool painted, int s, int face, float fi, float fj, uint8_t rgb[3]) {
    if (!painted) { rgb[0] = rgb[1] = rgb[2] = 255; return; }
    int i0 = (int)fi, j0 = (int)fj;
    if (i0 >= s) i0 = s - 1;
    if (j0 >= s) j0 = s - 1;
    float tu = fi - (float)i0, tv = fj - (float)j0;
    const uint8_t *c00 = b->paint.data + paint_index(s, face, i0, j0), *c10 = b->paint.data + paint_index(s, face, i0 + 1, j0);
    const uint8_t *c01 = b->paint.data + paint_index(s, face, i0, j0 + 1), *c11 = b->paint.data + paint_index(s, face, i0 + 1, j0 + 1);
    for (int k = 0; k < 3; ++k) {
        float v = (c00[k] * (1 - tu) + c10[k] * tu) * (1 - tv) + (c01[k] * (1 - tu) + c11[k] * tu) * tv;
        rgb[k] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5f));
    }
}
size_t cw_brush_face_mesh(const cw_brush *b, const float center[3], int face, cw_brush_vertex *vertices, uint32_t *indices, size_t *index_count) {
    if (index_count) *index_count = 0;
    if (!b || !vertices || !indices) return 0;
    cw_brush_polygon poly;
    if (!cw_brush_face_polygon(b, center, face, &poly) || poly.count < 3) return 0;
    face_frame frame;
    if (!face_lattice_frame(b, center, face, &poly, &frame)) return 0;
    int s = cw_brush_subdivisions(b);
    bool painted = cw_brush_painted(b);
    /* the polygon's edge planes (inward): m . p >= m . a */
    float edge_n[CW_BRUSH_MAX_POLYGON][3], edge_d[CW_BRUSH_MAX_POLYGON];
    for (int e = 0; e < poly.count; ++e) {
        const float *a = poly.p[e], *c = poly.p[(e + 1) % poly.count];
        float dir[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]}, m[3];
        cross3(frame.plane.n, dir, m);
        normalize3(m);
        /* stored as a "solid" half-space for clip_polygon: -m . p <= -m . a */
        edge_n[e][0] = -m[0]; edge_n[e][1] = -m[1]; edge_n[e][2] = -m[2];
        edge_d[e] = -dot3(m, a);
    }
    size_t nv = 0, ni = 0;
    for (int j = 0; j < s; ++j)
        for (int i = 0; i < s; ++i) {
            cw_brush_polygon cell;
            cell.count = 4;
            lattice_point(&frame, s, i, j, cell.p[0]);
            lattice_point(&frame, s, i + 1, j, cell.p[1]);
            lattice_point(&frame, s, i + 1, j + 1, cell.p[2]);
            lattice_point(&frame, s, i, j + 1, cell.p[3]);
            for (int e = 0; e < poly.count && cell.count >= 3; ++e) clip_polygon(&cell, edge_n[e], edge_d[e]);
            if (cell.count < 3) continue;
            float cn[3];
            polygon_normal(&cell, cn);
            if (dot3(cn, cn) < EPS * EPS) continue;
            bool reverse = dot3(cn, frame.plane.n) < 0;
            if (nv + (size_t)cell.count > CW_BRUSH_FACE_MAX_VERTICES || ni + (size_t)(cell.count - 2) * 3 > CW_BRUSH_FACE_MAX_INDICES) break;
            uint32_t base = (uint32_t)nv;
            for (int k = 0; k < cell.count; ++k) {
                const float *p = cell.p[reverse ? cell.count - 1 - k : k];
                cw_brush_vertex *v = &vertices[nv++];
                v->x = p[0]; v->y = p[1]; v->z = p[2];
                v->nx = frame.plane.n[0]; v->ny = frame.plane.n[1]; v->nz = frame.plane.n[2];
                cw_brush_face_uv(b, face, frame.plane.n, p, &v->u, &v->v);
                float fi = (p[frame.ua] - frame.u0) / (frame.u1 - frame.u0) * (float)s, fj = (p[frame.va] - frame.v0) / (frame.v1 - frame.v0) * (float)s;
                uint8_t rgb[3];
                vertex_colour(b, painted, s, face, fi < 0 ? 0 : (fi > (float)s ? (float)s : fi), fj < 0 ? 0 : (fj > (float)s ? (float)s : fj), rgb);
                v->r = (uint8_t)(rgb[0] * b->r / 255);
                v->g = (uint8_t)(rgb[1] * b->g / 255);
                v->b = (uint8_t)(rgb[2] * b->b / 255);
                v->a = 255;
            }
            for (int k = 1; k + 1 < cell.count; ++k) {
                indices[ni++] = base;
                indices[ni++] = base + (uint32_t)k;
                indices[ni++] = base + (uint32_t)k + 1;
            }
        }
    if (index_count) *index_count = ni;
    return nv;
}
size_t cw_brush_paint_at(cw_brush *b, const float center[3], const float point[3], float radius, uint8_t r, uint8_t g, uint8_t bl, float strength) {
    if (!b || radius <= 0) return 0;
    int s = cw_brush_subdivisions(b), faces = cw_brush_face_count(b);
    if (!cw_brush_painted(b)) {
        b->paint.size = (uint16_t)cw_brush_paint_size(b);
        memset(b->paint.data, 255, sizeof(b->paint.data));
    }
    strength = strength < 0 ? 0 : (strength > 1 ? 1 : strength);
    float lo[3], hi[3];
    cw_brush_bounds(b, center, lo, hi);
    /* quick reject: the sphere against the box */
    float d2 = 0;
    for (int k = 0; k < 3; ++k) {
        float c = point[k] < lo[k] ? lo[k] : (point[k] > hi[k] ? hi[k] : point[k]);
        d2 += (point[k] - c) * (point[k] - c);
    }
    if (d2 > radius * radius) return 0;
    size_t touched = 0;
    uint8_t target[3] = {r, g, bl};
    for (int f = 0; f < faces; ++f) {
        cw_brush_polygon poly;
        face_frame frame;
        cw_brush_face_polygon(b, center, f, &poly);
        if (!face_lattice_frame(b, center, f, &poly, &frame)) continue;
        for (int j = 0; j <= s; ++j)
            for (int i = 0; i <= s; ++i) {
                float p[3];
                lattice_point(&frame, s, i, j, p);
                float dx = p[0] - point[0], dy = p[1] - point[1], dz = p[2] - point[2];
                float d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (d >= radius) continue;
                float w = strength * (1 - d / radius);
                uint8_t *c = b->paint.data + paint_index(s, f, i, j);
                for (int k = 0; k < 3; ++k) {
                    float v = c[k] + (target[k] - c[k]) * w;
                    c[k] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5f));
                }
                ++touched;
            }
    }
    return touched;
}

/* --- ray casts --- */
bool cw_brush_ray(const cw_brush *b, const float center[3], const float o[3], const float d[3], float *t, int *face) {
    cw_plane planes[CW_BRUSH_MAX_FACES];
    int n = cw_brush_planes(b, center, planes);
    float t_enter = -INFINITY, t_exit = INFINITY;
    int enter = -1, exit_face = -1;
    for (int i = 0; i < n; ++i) {
        float denom = dot3(planes[i].n, d), dist = planes[i].d - dot3(planes[i].n, o);
        if (fabsf(denom) < 1e-12f) {
            if (dist < 0) return false;   /* parallel and outside */
            continue;
        }
        float ti = dist / denom;
        if (denom < 0) { if (ti > t_enter) { t_enter = ti; enter = i; } }
        else if (ti < t_exit) { t_exit = ti; exit_face = i; }
        if (t_enter > t_exit) return false;
    }
    if (t_exit < 0) return false;
    bool inside = t_enter < 0;
    if (t) *t = inside ? t_exit : t_enter;
    if (face) *face = inside ? exit_face : enter;
    return true;
}
bool cw_brush_ray_box(const float lo[3], const float hi[3], const float o[3], const float d[3], float *t, cw_brush_face *face) {
    float tmin = -INFINITY, tmax = INFINITY;
    int enter_face = -1, exit_face = -1;
    for (int i = 0; i < 3; ++i) {
        if (fabsf(d[i]) < 1e-12f) {
            if (o[i] < lo[i] || o[i] > hi[i]) return false;
            continue;
        }
        float a = (lo[i] - o[i]) / d[i], b = (hi[i] - o[i]) / d[i];
        int fa = i * 2 + 1, fb = i * 2;   /* the low plane is the negative face, the high plane the positive one */
        if (a > b) { float s = a; a = b; b = s; int fs = fa; fa = fb; fb = fs; }
        if (a > tmin) { tmin = a; enter_face = fa; }
        if (b < tmax) { tmax = b; exit_face = fb; }
        if (tmin > tmax) return false;
    }
    if (tmax < 0) return false;
    bool inside = tmin < 0;
    if (t) *t = inside ? tmax : tmin;
    if (face) *face = (cw_brush_face)(inside ? exit_face : enter_face);
    return true;
}
