#include "r3d_mesh.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979f

static bool allocate(r3d_mesh *m, size_t vertices, size_t indices) {
    memset(m, 0, sizeof(*m));
    m->vertices = calloc(vertices, sizeof(*m->vertices));
    m->indices = calloc(indices, sizeof(*m->indices));
    if (!m->vertices || !m->indices) { r3d_mesh_free(m); return false; }
    m->vertex_count = vertices;
    m->index_count = indices;
    return true;
}
void r3d_mesh_free(r3d_mesh *m) {
    if (!m) return;
    free(m->vertices);
    free(m->indices);
    free(m->edges);
    memset(m, 0, sizeof(*m));
}
static void set_vertex(r3d_vertex *v, float x, float y, float z, float cap, float nx, float ny, float nz, float u, float t) {
    v->x = x; v->y = y; v->z = z; v->cap = cap;
    v->nx = nx; v->ny = ny; v->nz = nz;
    v->u = u; v->v = t;
    v->rgba[0] = v->rgba[1] = v->rgba[2] = v->rgba[3] = 255;
}

/* A stack of latitude rings (each `sectors + 1` vertices, the seam
 * duplicated for uvs) joined by quads; the sphere and the capsule share it. */
static void ring_vertices(r3d_vertex *out, int sectors, float lat, float cap, float v_coord) {
    float y = sinf(lat), r = cosf(lat);
    for (int j = 0; j <= sectors; ++j) {
        float lon = 2 * PI * (float)j / (float)sectors;
        float x = r * sinf(lon), z = r * cosf(lon);
        set_vertex(&out[j], x, y, z, cap, x, y, z, (float)j / (float)sectors, v_coord);
    }
}
static void join_rings(uint32_t *out, int ring_count, int sectors) {
    size_t n = 0;
    int stride = sectors + 1;
    for (int i = 0; i + 1 < ring_count; ++i)
        for (int j = 0; j < sectors; ++j) {
            uint32_t a = (uint32_t)(i * stride + j), b = (uint32_t)((i + 1) * stride + j);
            /* counter-clockwise seen from outside: the upper ring comes first */
            out[n++] = a; out[n++] = b; out[n++] = a + 1;
            out[n++] = a + 1; out[n++] = b; out[n++] = b + 1;
        }
}
bool r3d_mesh_sphere(r3d_mesh *m, int rings, int sectors) {
    if (!m || rings < 2 || sectors < 3) return false;
    int ring_count = rings + 1;
    if (!allocate(m, (size_t)ring_count * (size_t)(sectors + 1), (size_t)rings * (size_t)sectors * 6)) return false;
    for (int i = 0; i < ring_count; ++i) {
        float lat = PI * 0.5f - PI * (float)i / (float)rings;
        ring_vertices(m->vertices + (size_t)i * (size_t)(sectors + 1), sectors, lat, 0, (float)i / (float)rings);
    }
    join_rings(m->indices, ring_count, sectors);
    return true;
}
bool r3d_mesh_capsule(r3d_mesh *m, int rings, int sectors) {
    if (!m || rings < 1 || sectors < 3) return false;
    int half = rings + 1;                  /* rings per hemisphere, equator included */
    int ring_count = half * 2;             /* the two equators are distinct vertices (different cap signs) */
    if (!allocate(m, (size_t)ring_count * (size_t)(sectors + 1), (size_t)(ring_count - 1) * (size_t)sectors * 6)) return false;
    for (int i = 0; i < half; ++i) {
        float lat = PI * 0.5f - PI * 0.5f * (float)i / (float)rings;   /* pole -> equator */
        ring_vertices(m->vertices + (size_t)i * (size_t)(sectors + 1), sectors, lat, 1.0f, 0.25f * (float)i / (float)rings);
    }
    for (int i = 0; i < half; ++i) {
        float lat = -PI * 0.5f * (float)i / (float)rings;              /* equator -> pole */
        ring_vertices(m->vertices + (size_t)(half + i) * (size_t)(sectors + 1), sectors, lat, -1.0f, 0.75f + 0.25f * (float)i / (float)rings);
    }
    join_rings(m->indices, ring_count, sectors);
    return true;
}
/* A quad facing `n`, spanning tangent `t` and bitangent `b` (t x b = n), at distance `offset` along n. */
static void quad(r3d_vertex *v, uint32_t *idx, uint32_t base, const float n[3], const float t[3], const float b[3], float offset) {
    const float signs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int k = 0; k < 4; ++k) {
        float x = n[0] * offset + (t[0] * signs[k][0] + b[0] * signs[k][1]) * 0.5f;
        float y = n[1] * offset + (t[1] * signs[k][0] + b[1] * signs[k][1]) * 0.5f;
        float z = n[2] * offset + (t[2] * signs[k][0] + b[2] * signs[k][1]) * 0.5f;
        set_vertex(&v[k], x, y, z, 0, n[0], n[1], n[2], signs[k][0] * 0.5f + 0.5f, signs[k][1] * 0.5f + 0.5f);
    }
    idx[0] = base; idx[1] = base + 1; idx[2] = base + 2;
    idx[3] = base; idx[4] = base + 2; idx[5] = base + 3;
}
bool r3d_mesh_cube(r3d_mesh *m) {
    if (!m || !allocate(m, 24, 36)) return false;
    static const float faces[6][3][3] = {
        /* normal, tangent, bitangent with t x b = n */
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    };
    for (int f = 0; f < 6; ++f) quad(m->vertices + f * 4, m->indices + f * 6, (uint32_t)(f * 4), faces[f][0], faces[f][1], faces[f][2], 0.5f);
    return true;
}
bool r3d_mesh_plane(r3d_mesh *m) {
    if (!m || !allocate(m, 4, 6)) return false;
    static const float n[3] = {0, 1, 0}, t[3] = {0, 0, 1}, b[3] = {1, 0, 0};
    quad(m->vertices, m->indices, 0, n, t, b, 0);
    return true;
}

static int compare_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y);
}
bool r3d_mesh_build_edges(r3d_mesh *m) {
    if (!m || m->index_count < 3) return false;
    free(m->edges);
    m->edges = NULL;
    m->edge_index_count = 0;
    size_t n = m->index_count;   /* one edge per index */
    uint64_t *keys = malloc(n * sizeof(*keys));
    if (!keys) return false;
    for (size_t i = 0; i + 2 < m->index_count; i += 3) {
        for (int k = 0; k < 3; ++k) {
            uint32_t a = m->indices[i + k], b = m->indices[i + (k + 1) % 3];
            uint32_t lo = a < b ? a : b, hi = a < b ? b : a;
            keys[i + k] = ((uint64_t)lo << 32) | hi;
        }
    }
    qsort(keys, n, sizeof(*keys), compare_u64);
    size_t unique = 0;
    for (size_t i = 0; i < n; ++i)
        if (i == 0 || keys[i] != keys[i - 1]) keys[unique++] = keys[i];
    m->edges = malloc(unique * 2 * sizeof(*m->edges));
    if (!m->edges) { free(keys); return false; }
    for (size_t i = 0; i < unique; ++i) {
        m->edges[i * 2] = (uint32_t)(keys[i] >> 32);
        m->edges[i * 2 + 1] = (uint32_t)(keys[i] & 0xffffffffu);
    }
    m->edge_index_count = unique * 2;
    free(keys);
    return true;
}
