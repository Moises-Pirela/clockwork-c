/* cw_brush: Quake-style level geometry for 3D games.
 *
 * A brush is a convex solid: an axis-aligned box cut by up to
 * CW_BRUSH_MAX_CLIPS extra planes (a wedge is a box with one diagonal cut;
 * the editor's clip tool adds planes the way Quake editors do). Every face
 * - the six box faces and one per clip plane - has its own material and
 * texture alignment (shift, rotation, scale) over a world-aligned projection
 * (a face's uv comes from its world position along the two axes of the
 * plane closest to its normal, divided by the brush's texture scale, so
 * neighbouring brushes tile seamlessly), and a lattice of `subdivisions` x
 * `subdivisions` quads whose corners the editor's paint tool colours, kept
 * in a byte array so the paint travels with the scene (and its undo
 * history).
 *
 * The engine registers the Brush component (cw_game.h) in every bound
 * world and draws it at the entity's position through canvas->brush3d;
 * this header is the geometry the engine, the editor and the hosts share:
 * planes and face polygons, meshes, painting, clipping and ray casts.
 * Nothing here touches a world or a renderer. Positions handed to these
 * functions are world positions; the brush's own data is relative to its
 * centre (the entity's position).
 *
 * Box faces are numbered +x, -x, +y, -y, +z, -z (east, west, top, bottom,
 * south, north: z points south, towards a default camera); clip faces
 * follow as CW_BRUSH_FACES + i.
 */
#ifndef CW_BRUSH_H
#define CW_BRUSH_H

#include "cw_reflect.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CW_BRUSH_FACES 6
#define CW_BRUSH_MAX_CLIPS 4
#define CW_BRUSH_MAX_FACES (CW_BRUSH_FACES + CW_BRUSH_MAX_CLIPS)
#define CW_BRUSH_MAX_SUBDIVISIONS 8
#define CW_BRUSH_LATTICE (CW_BRUSH_MAX_SUBDIVISIONS + 1)
#define CW_BRUSH_PAINT_BYTES (CW_BRUSH_MAX_FACES * CW_BRUSH_LATTICE * CW_BRUSH_LATTICE * 3)   /* 2430: rgb per lattice vertex, face-major */
#define CW_BRUSH_ALIGN_FLOATS 5                                                              /* per face: shift u, shift v, rotation (degrees), scale u, scale v */
#define CW_BRUSH_MAX_POLYGON 16                                                              /* vertices of one face polygon */
/* a face mesh: every lattice cell clipped to the face polygon, fanned */
#define CW_BRUSH_FACE_MAX_VERTICES (CW_BRUSH_MAX_SUBDIVISIONS * CW_BRUSH_MAX_SUBDIVISIONS * CW_BRUSH_MAX_POLYGON)
#define CW_BRUSH_FACE_MAX_INDICES (CW_BRUSH_MAX_SUBDIVISIONS * CW_BRUSH_MAX_SUBDIVISIONS * (CW_BRUSH_MAX_POLYGON - 2) * 3)
typedef enum { CW_FACE_EAST, CW_FACE_WEST, CW_FACE_TOP, CW_FACE_BOTTOM, CW_FACE_SOUTH, CW_FACE_NORTH } cw_brush_face;

/* The Brush component. Sizes are the full extents around the entity's position. */
typedef struct {
    float width, height, depth;              /* CW_SEM_SIZE_X/Y/Z */
    uint32_t material[CW_BRUSH_MAX_FACES];   /* material assets per face (0: the flat tint) */
    float texture_scale;                     /* world units per texture repeat */
    uint8_t subdivisions;                    /* 1..CW_BRUSH_MAX_SUBDIVISIONS quads along each face edge (the paint lattice) */
    uint8_t r, g, b;                         /* tint */
    CW_BYTES(CW_BRUSH_MAX_CLIPS * 4 * sizeof(float)) clips;          /* clip planes (nx, ny, nz, d) relative to the centre; the solid is n.p <= d */
    CW_BYTES(CW_BRUSH_MAX_FACES * CW_BRUSH_ALIGN_FLOATS * sizeof(float)) align;   /* per-face texture alignment (empty: defaults) */
    CW_BYTES(CW_BRUSH_PAINT_BYTES) paint;    /* vertex colours: faces * (s+1)^2 rgb bytes, or empty (unpainted, white) */
} cw_brush;
extern const cw_type_info cw_brush_type;
extern const char *const cw_brush_face_names[CW_BRUSH_MAX_FACES];

/* A plane: the solid lies where n . p <= d. */
typedef struct { float n[3], d; } cw_plane;
/* Texture alignment of one face. */
typedef struct { float shift_u, shift_v, rotation, scale_u, scale_v; } cw_brush_align;
/* A face polygon in world space, counter-clockwise seen from outside. */
typedef struct { float p[CW_BRUSH_MAX_POLYGON][3]; int count; } cw_brush_polygon;

/* --- the solid --- */
void cw_brush_bounds(const cw_brush *b, const float center[3], float lo[3], float hi[3]);   /* the box (the clips only cut it) */
/* Sets the box from corners (the centre moves; sizes never drop below `min_size`). */
void cw_brush_set_bounds(cw_brush *b, float center[3], const float lo[3], const float hi[3], float min_size);
int cw_brush_clip_count(const cw_brush *b);
int cw_brush_face_count(const cw_brush *b);                   /* 6 + clips */
/* A face's plane in world space (false for a face index past the count). */
bool cw_brush_face_plane(const cw_brush *b, const float center[3], int face, cw_plane *out);
/* All planes (face order), in world space; returns the count. */
int cw_brush_planes(const cw_brush *b, const float center[3], cw_plane out[CW_BRUSH_MAX_FACES]);
/* The polygon of a face after every other plane has cut it; count 0 when the face was clipped away. */
bool cw_brush_face_polygon(const cw_brush *b, const float center[3], int face, cw_brush_polygon *out);
/* Cuts the brush by a world-space plane, keeping the solid's side (n . p <= d).
 * Returns false when the plane misses the brush (nothing to cut, or
 * everything would go) or the clips are full. The box then shrinks to the
 * remaining solid and the centre moves with it. */
bool cw_brush_clip(cw_brush *b, float center[3], const cw_plane *plane);
/* Which side of a plane a brush lies on: -1 entirely in the solid side, +1 entirely outside, 0 cut. */
int cw_brush_plane_side(const cw_brush *b, const float center[3], const cw_plane *plane);
/* A wedge: the box cut by the diagonal that rises along `axis` (0 x, 2 z)
 * in the direction of `sign`, from the bottom of one end to the top of the other. */
bool cw_brush_make_wedge(cw_brush *b, float center[3], int axis, float sign);
void cw_brush_clear_clips(cw_brush *b);
/* Replaces clip plane `index` (0-based) with a world-space plane. */
bool cw_brush_set_clip_plane(cw_brush *b, const float center[3], int index, const cw_plane *plane);

/* --- CSG ---
 * Subtraction: the pieces of `a` that lie outside the convex `cutter` (its
 * planes, world space) - one per cutter plane at most, each a brush with
 * its own centre, written to `pieces`/`centers` (CW_BRUSH_MAX_FACES of
 * each). Returns the piece count (0: `a` lies inside the cutter), or -1
 * when the solids do not intersect. Pieces that would need more clip planes
 * than a brush holds are counted in `dropped` (NULL allowed). */
int cw_brush_subtract(const cw_brush *a, const float center[3], const cw_plane *cutter, int cutter_count,
                      cw_brush *pieces, float (*centers)[3], int *dropped);
bool cw_brush_intersects(const cw_brush *a, const float center[3], const cw_plane *planes, int count);
/* Hollowing: the walls `thickness` thick around the inside of a brush (its
 * planes pushed inward, then subtracted). Same outputs as the subtraction. */
int cw_brush_hollow(const cw_brush *a, const float center[3], float thickness, cw_brush *pieces, float (*centers)[3], int *dropped);

/* --- keeping textures and vertices in place --- */
/* Texture lock: after the brush moved by `delta`, shifts every face's
 * alignment so its texture stays on the surface instead of sliding through
 * it (world-aligned projection would leave it behind). */
void cw_brush_lock_textures(cw_brush *b, const float delta[3]);
/* Snaps the solid to a grid: every box face plane to a multiple of
 * `spacing`, every clip plane re-fitted through its polygon's snapped
 * corners. Returns false when nothing moved. */
bool cw_brush_snap_to_grid(cw_brush *b, float center[3], float spacing);
/* The distinct vertices of the solid (corners shared by three or more
 * faces); returns the count written (at most `capacity`). */
int cw_brush_vertices(const cw_brush *b, const float center[3], float (*out)[3], int capacity);
/* Moves a vertex of the solid to `to`: the box faces through it move with it
 * and the clip planes through it are re-fitted around it (their other two
 * polygon corners stay). False when `from` is not a vertex. */
bool cw_brush_move_vertex(cw_brush *b, float center[3], const float from[3], const float to[3]);

/* --- faces --- */
int cw_brush_face_axis(cw_brush_face face);                  /* box faces: 0, 1, 2 */
float cw_brush_face_sign(cw_brush_face face);                 /* +1 or -1 */
void cw_brush_face_normal(cw_brush_face face, float n[3]);
/* The two world axes a face's texture and lattice run along: u then v, from
 * the axis plane closest to the normal (walls keep v vertical). */
void cw_brush_normal_tangents(const float n[3], int *u_axis, int *v_axis);
void cw_brush_face_tangents(cw_brush_face face, int *u_axis, int *v_axis);
/* The face of an axis-aligned box a surface point lies on (the nearest face plane). */
cw_brush_face cw_brush_face_at(const float lo[3], const float hi[3], const float point[3]);
/* The face whose plane a world point lies on, nearest plane wins; -1 for a brush without faces. */
int cw_brush_face_of_point(const cw_brush *b, const float center[3], const float point[3]);
/* Texture alignment: the stored values, or the defaults (no shift, no rotation, scale 1). */
cw_brush_align cw_brush_align_default(void);
void cw_brush_face_align_get(const cw_brush *b, int face, cw_brush_align *out);
void cw_brush_face_align_set(cw_brush *b, int face, const cw_brush_align *align);
/* The uv of a world point on a face (the projection, the brush's texture scale and the face's alignment). */
void cw_brush_face_uv(const cw_brush *b, int face, const float n[3], const float point[3], float *u, float *v);

/* --- meshes --- */
typedef struct {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
    uint8_t r, g, b, a;
} cw_brush_vertex;
/* The triangles of one face: its paint lattice cells clipped to the face
 * polygon, each fanned, with the face's normal, uv and the paint colour
 * (white when unpainted) times the tint. Returns the vertex count (0 for a
 * clipped-away face) and writes `*index_count` triangle indices,
 * counter-clockwise seen from outside. Buffers must hold
 * CW_BRUSH_FACE_MAX_VERTICES and CW_BRUSH_FACE_MAX_INDICES. */
size_t cw_brush_face_mesh(const cw_brush *b, const float center[3], int face, cw_brush_vertex *vertices, uint32_t *indices, size_t *index_count);

/* --- paint --- */
int cw_brush_subdivisions(const cw_brush *b);                 /* clamped to 1..max */
size_t cw_brush_paint_size(const cw_brush *b);                /* bytes the lattice colours take for this brush's faces and subdivisions */
bool cw_brush_painted(const cw_brush *b);                     /* the paint matches the faces and subdivisions */
/* Paints the lattice vertices within `radius` of a world point: their colour
 * moves towards rgb by `strength` (0..1), fading with distance. Initialises
 * the paint (white) when it does not match the lattice. Returns the
 * vertices touched. */
size_t cw_brush_paint_at(cw_brush *b, const float center[3], const float point[3], float radius, uint8_t r, uint8_t g, uint8_t bl, float strength);
void cw_brush_paint_clear(cw_brush *b);
/* Colour of one lattice vertex (white when unpainted). */
void cw_brush_paint_get(const cw_brush *b, int face, int i, int j, uint8_t rgb[3]);

/* --- ray casts --- */
/* Against the solid (every plane): the nearest entry, or the exit when the
 * origin is inside, with the face hit; t in units of the direction's length. */
bool cw_brush_ray(const cw_brush *b, const float center[3], const float origin[3], const float direction[3], float *t, int *face);
/* Against an axis-aligned box alone, with the box face hit. */
bool cw_brush_ray_box(const float lo[3], const float hi[3], const float origin[3], const float direction[3], float *t, cw_brush_face *face);

#ifdef __cplusplus
}
#endif
#endif
