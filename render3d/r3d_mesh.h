/* r3d_mesh: CPU-side meshes of the primitives render3d draws. Pure code
 * (no graphics API), so the generators are unit-tested headlessly.
 *
 * Every mesh is a unit shape the renderer scales with the model matrix:
 *   sphere   radius 1 around the origin
 *   capsule  radius 1, hemispheres meeting at y = 0; `cap` is +1 on the
 *            upper hemisphere and -1 on the lower one so the vertex shader
 *            can push the caps apart by half the capsule's height
 *   cube     -0.5 .. 0.5 on every axis, 24 vertices with per-face normals
 *   plane    1 x 1 on the y = 0 plane, normal +y
 * Vertices carry a normal, a uv and a colour.
 */
#ifndef R3D_MESH_H
#define R3D_MESH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct r3d_vertex {
    float x, y, z, cap;    /* position and the capsule cap sign (0 on other meshes) */
    float nx, ny, nz;
    float u, v;
    uint8_t rgba[4];       /* vertex colour, white on the generated meshes */
} r3d_vertex;

typedef struct {
    r3d_vertex *vertices;
    uint32_t *indices;     /* triangles */
    uint32_t *edges;       /* unique edges as line index pairs (r3d_mesh_build_edges) */
    size_t vertex_count, index_count, edge_index_count;
} r3d_mesh;

bool r3d_mesh_sphere(r3d_mesh *m, int rings, int sectors);
bool r3d_mesh_capsule(r3d_mesh *m, int rings, int sectors);   /* rings per hemisphere */
bool r3d_mesh_cube(r3d_mesh *m);
bool r3d_mesh_plane(r3d_mesh *m);
/* Fills `edges` with every triangle edge once (no duplicates, either direction). */
bool r3d_mesh_build_edges(r3d_mesh *m);
void r3d_mesh_free(r3d_mesh *m);

#ifdef __cplusplus
}
#endif
#endif
