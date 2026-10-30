/* editor_core: the editor's document model, independent of any window.
 *
 * An ed_document owns one project, the game module loaded from it, a world
 * bound to that module, the scene being edited (the world plus entity
 * display names), the selection, an undo history and play mode. Every
 * editing operation the window offers is a function here, so the whole
 * editor can be driven and tested headlessly.
 *
 * Undo is snapshot-based: after each operation the scene is written to text
 * (the same format that goes to disk) and pushed on a history; undo and
 * redo reload a neighbouring image. That is simple, always correct with
 * respect to the ECS (ids are preserved, so the selection survives), and
 * cheap at editor scales.
 *
 * Play mode runs the module's systems on the edited scene in place; stop
 * restores the scene from the image taken when play started, so nothing
 * that happens during play is ever saved by accident. While editing, baking
 * is off: config components are authored and runtime components do not
 * exist; play turns baking on and generates them (cw_game_world_bake_all),
 * and since scenes never contain runtime components, stop removes them.
 */
#ifndef EDITOR_CORE_H
#define EDITOR_CORE_H

#include "cw_game.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ED_UNDO_DEPTH 128
#define ED_STATUS_CAPACITY 200
#define ED_LOG_CAPACITY 8192

typedef struct {
    cw_entity_id id;
    char name[CW_SCENE_NAME_CAPACITY];
} ed_name;
typedef struct {
    char *text;
    size_t size;
    char label[48];
} ed_image;

typedef struct {
    cw_project project;
    bool project_open;
    cw_game game;                      /* the loaded module; game.module is NULL until built */
    cw_game_world gw;
    bool world_ready;
    cw_assets assets;                  /* the project's asset registry (cw_assets_current while the project is open) */
    char engine_root[CW_GAME_PATH_CAPACITY];
    char player_path[CW_GAME_PATH_CAPACITY];  /* the player executable F9 launches (CW_PLAYER overrides) */
    char scene_path[CW_GAME_PATH_CAPACITY];   /* "" while untitled */
    bool dirty;

    ed_name *names;
    size_t name_count, name_capacity;
    cw_entity_id *selection;           /* sorted, unique */
    size_t selection_count, selection_capacity;

    ed_image history[ED_UNDO_DEPTH];   /* history[0 .. history_count) in order; history[current] is the present */
    size_t history_count, current;

    bool playing, paused;
    bool no_history;                   /* edits apply but are not recorded (the animation editor's record mode) */
    bool texture_lock;                 /* 3D: brush textures move with their brushes (default on) */
    ed_image play_backup;
    uint64_t play_ticks;

    char status[ED_STATUS_CAPACITY];
    bool status_error;
    char log[ED_LOG_CAPACITY];         /* output of the last build */
    int last_build_status;             /* exit status of the last build, -1 before any */
} ed_document;

void ed_init(ed_document *d, const char *engine_root, const char *player_path);
void ed_shutdown(ed_document *d);

/* --- project and module --- */
bool ed_open_project(ed_document *d, const char *cwproj_path);   /* loads the module when it is built */
bool ed_load_module(ed_document *d);                             /* (re)loads project.module and rebinds, keeping the scene */
bool ed_module_built(const ed_document *d);
/* Runs tools/build-game.sh (or .ps1) for the project: "module" builds the
 * shared library the editor loads (and reloads it, keeping the scene),
 * "export" builds a standalone game executable with the module linked in
 * next to a copy of the project and its scenes. Output lands in d->log. */
bool ed_build(ed_document *d, const char *mode);
bool ed_run_player(ed_document *d);                              /* launches the player on the current scene */

/* --- project settings ---
 * Rendering settings are render2d tunables stored in the project's render
 * file (project.render, default <project>/render.cfg); the window edits
 * them live and saves them there. The document only knows the path and the
 * window settings, which live in the project file itself. */
void ed_render_path(const ed_document *d, char *out, size_t capacity);   /* "" without a project */
bool ed_render_settings_exist(const ed_document *d);
bool ed_set_window(ed_document *d, int width, int height, const char *title);   /* saves the project file */

/* --- assets ---
 * The manifest is loaded with the project; files already in the project
 * (scenes, sources, data) are registered by a scan at that point. Changes
 * are saved to the manifest immediately. */
const cw_asset *ed_import_asset(ed_document *d, const char *source_file, bool replace);
size_t ed_import_directory(ed_document *d, const char *directory, bool replace);   /* every file in it */
size_t ed_scan_assets(ed_document *d);                                              /* untracked project files */
bool ed_remove_asset(ed_document *d, uint32_t id, bool delete_file);
bool ed_rename_asset(ed_document *d, uint32_t id, const char *name);
bool ed_save_assets(ed_document *d);
/* Writes a file inside the project (relative path, directories created) and registers it;
 * an existing file is overwritten. Returns the asset. */
const cw_asset *ed_write_asset(ed_document *d, const char *relative_path, const void *data, size_t size);
/* Is the asset referenced by a CW_SEM_ASSET field of any entity? Counts the references. */
size_t ed_asset_references(ed_document *d, uint32_t id);

/* --- world inspection --- */
size_t ed_component_entities(const ed_document *d, cw_component_id ct);   /* entities carrying it */
void ed_select_with_component(ed_document *d, cw_component_id ct);
bool ed_set_system_enabled(ed_document *d, const char *name, bool enabled);

/* --- autosave: a copy next to the scene, never the scene itself --- */
void ed_autosave_path(const ed_document *d, char *out, size_t capacity);   /* "" while untitled */
bool ed_autosave(ed_document *d);

/* --- scenes --- */
void ed_new_scene(ed_document *d);
bool ed_load_scene(ed_document *d, const char *path);
bool ed_save_scene(ed_document *d, const char *path);            /* NULL: the current path */
/* Scene files in the project's scenes directory, sorted; returns the count. */
size_t ed_list_scenes(const ed_document *d, char (*names)[128], size_t capacity);
void ed_scene_file(const ed_document *d, const char *name, char *out, size_t capacity);   /* name -> full path */

/* --- entities --- */
cw_entity_id ed_place(ed_document *d, size_t prefab, float x, float y);
/* An empty entity at a position: it gets the module's position component
 * (the first registered type with CW_SEM_POSITION fields) and nothing else;
 * components are then added in the inspector. */
cw_entity_id ed_create_entity(ed_document *d, float x, float y);

/* --- the interface (cw_ui.h) ---
 * Interface elements are entities with the engine's Ui components; these
 * create the usual kinds with sensible defaults (a panel: a rect with a
 * translucent image; an image; a text; a button: an image with a UiButton
 * and a child text; a bar: a horizontally filled image), parented to
 * `parent` (a UiRect entity, or CW_INVALID_ENTITY for the canvas), centred
 * in it. Editing helpers work in reference pixels on the last layout
 * (ed_ui_layout: the window lays the interface out on the game view each
 * frame; tests on a fixed screen). */
typedef enum { ED_UI_PANEL, ED_UI_IMAGE, ED_UI_TEXT, ED_UI_BUTTON, ED_UI_BAR, ED_UI_KINDS } ed_ui_kind;
const char *ed_ui_kind_name(ed_ui_kind kind);
cw_entity_id ed_create_ui(ed_document *d, ed_ui_kind kind, cw_entity_id parent);
size_t ed_ui_layout(ed_document *d, cw_ui_screen screen);            /* lays out; returns the element count */
bool ed_is_ui(const ed_document *d, cw_entity_id id);                 /* carries a UiRect */
cw_entity_id ed_ui_hit(ed_document *d, float x, float y);            /* topmost element under a window point */
/* Moves the selected elements (those with a UiRect and no selected UI ancestor) by reference pixels, without recording. */
size_t ed_ui_move_selection(ed_document *d, float dx, float dy);
/* Drags edges of one element: each of left/top/right/bottom moves by the delta when its flag is set; without recording. */
bool ed_ui_resize(ed_document *d, cw_entity_id id, bool left, bool top, bool right, bool bottom, float dx, float dy);
/* Anchor presets keep the element where it is on screen. `preset` is 0..8
 * for the nine points (row-major from the top left), 9 stretch
 * horizontally, 10 vertically, 11 both. */
bool ed_ui_set_anchor_preset(ed_document *d, cw_entity_id id, int preset);
bool ed_ui_set_anchors(ed_document *d, cw_entity_id id, float min_x, float min_y, float max_x, float max_y);
/* Re-parents without moving the element on screen (CW_INVALID_ENTITY: the canvas). */
bool ed_ui_set_parent(ed_document *d, cw_entity_id id, cw_entity_id parent);
/* The project's interface canvas (reference resolution and scale mode); saves the project file. */
bool ed_set_ui_canvas(ed_document *d, float width, float height, cw_ui_scale_mode mode);

/* --- project prefabs ---
 * A prefab is a scene fragment (prefabs/<name>.cwprefab, registered as a
 * CW_ASSET_PREFAB asset) saved from the selection: one or more entities
 * with their components, names and the references between them. Placing
 * it instantiates copies with fresh ids at a position (the first saved
 * entity is the anchor). Instances keep no link to the prefab. */
const cw_asset *ed_save_prefab(ed_document *d, const char *name);      /* from the selection; overwrites an existing prefab of that name */
size_t ed_place_prefab(ed_document *d, uint32_t asset_id, float x, float y);   /* entities created; selects them */
bool ed_delete_prefab(ed_document *d, uint32_t asset_id);
/* Prefab assets in name order; returns the count written. */
size_t ed_list_prefabs(const ed_document *d, const cw_asset **out, size_t capacity);
size_t ed_delete_selection(ed_document *d);
size_t ed_duplicate_selection(ed_document *d, float dx, float dy);
size_t ed_duplicate_selection3(ed_document *d, float dx, float dy, float dz);   /* copies offset on three axes */
/* Moves without recording; call ed_commit when the drag ends. */
size_t ed_move_selection(ed_document *d, float dx, float dy);
/* The 3D forms (cw_reflect_position3): entities without a Z field move in x and y only. */
size_t ed_move_selection3(ed_document *d, float dx, float dy, float dz);
cw_entity_id ed_create_entity3(ed_document *d, float x, float y, float z);
/* Is the project viewed in 3D (its renderer key)? */
bool ed_is_3d(const ed_document *d);
/* Switches the project between the 2D and the 3D view (its `renderer` key,
 * saved into the .cwproj so the player follows); false without a project. */
bool ed_set_renderer(ed_document *d, cw_renderer_kind kind);
/* A decal entity (Decal3D at a three-axis position): `size` across and
 * `depth` along the projection, pitched/yawed so its projection runs against
 * `normal` (the surface it was clicked on; NULL: flat on the ground), with a
 * material asset (0: none). Records. */
cw_entity_id ed_create_decal(ed_document *d, const float point[3], const float normal[3], float size, float depth, uint32_t material);
bool ed_is_decal(const ed_document *d, cw_entity_id id);
/* The gizmos' transforms, without recording (commit when the drag ends):
 * rotation deltas add to the CW_SEM_ROTATION_* fields, scale factors
 * multiply the CW_SEM_SIZE_* fields (never below 0.01). */
size_t ed_rotate_selection(ed_document *d, float drx, float dry, float drz);
size_t ed_scale_selection(ed_document *d, float fx, float fy, float fz);

/* --- level brushes (cw_brush.h) ---
 * A brush entity carries the game's position component (the box centre)
 * and the engine's Brush. New brushes take `material` on every face (0:
 * none). Face edits move one face plane along its axis, keeping the
 * opposite face; painting colours the lattice vertices of every brush
 * within `radius` of a world point. */
cw_entity_id ed_create_brush(ed_document *d, const float lo[3], const float hi[3], uint32_t material);   /* records */
/* A wedge in that box: the slope rises along `axis` (0 x, 2 z) towards `sign`. */
cw_entity_id ed_create_wedge(ed_document *d, const float lo[3], const float hi[3], uint32_t material, int axis, float sign);
/* The clip tool: cuts every selected brush by a world plane (n . p = dist;
 * the front is where n . p > dist). Keeping both splits each cut brush into
 * two entities (the copy keeps the front). Returns the brushes cut; records. */
typedef enum { ED_CLIP_KEEP_BACK, ED_CLIP_KEEP_FRONT, ED_CLIP_KEEP_BOTH } ed_clip_mode;
size_t ed_clip_selection(ed_document *d, const float n[3], float dist, ed_clip_mode mode);
/* Per-face texture alignment (shift, rotation, scale); records. */
bool ed_brush_set_face_align(ed_document *d, cw_entity_id id, int face, const cw_brush_align *align);
/* CSG. Subtract: the selected brushes carve every other brush they touch
 * (the pieces become entities with the carved brush's components and name,
 * the carved brush and the cutters go); returns the pieces made. Hollow:
 * each selected brush becomes its walls. Both record. */
size_t ed_csg_subtract(ed_document *d);
size_t ed_csg_hollow(ed_document *d, float thickness);
/* Snaps the selected brushes' planes to the grid; records when anything moved. */
size_t ed_snap_brushes(ed_document *d, float spacing);
/* The vertex tool: moves a vertex of a brush (the faces through it follow); without recording. */
bool ed_brush_move_vertex(ed_document *d, cw_entity_id id, const float from[3], const float to[3]);
bool ed_is_brush(const ed_document *d, cw_entity_id id);
bool ed_brush_bounds(ed_document *d, cw_entity_id id, float lo[3], float hi[3]);
bool ed_brush_set_face(ed_document *d, cw_entity_id id, int face, float plane);                          /* without recording */
bool ed_brush_set_face_material(ed_document *d, cw_entity_id id, int face, uint32_t material);           /* face -1: every face; records */
size_t ed_paint(ed_document *d, const float point[3], float radius, uint8_t r, uint8_t g, uint8_t b, float strength);   /* without recording */
size_t ed_paint_clear_selection(ed_document *d);                                                          /* records */
bool ed_set_field(ed_document *d, cw_entity_id id, cw_component_id ct, const cw_field *field, const char *text);
bool ed_adjust_field(ed_document *d, cw_entity_id id, cw_component_id ct, const cw_field *field, int direction);
bool ed_add_component(ed_document *d, cw_entity_id id, cw_component_id ct);   /* runtime types only while playing */
bool ed_remove_component(ed_document *d, cw_entity_id id, cw_component_id ct);
bool ed_rename(ed_document *d, cw_entity_id id, const char *name);
const char *ed_name_of(const ed_document *d, cw_entity_id id);   /* "" when unnamed */
/* A label for lists: the name, else the first reflected component's name. */
void ed_label(const ed_document *d, cw_entity_id id, char *out, size_t capacity);
size_t ed_entity_count(const ed_document *d);
/* Entities in ascending id order; returns the count written. */
size_t ed_entities(const ed_document *d, cw_entity_id *out, size_t capacity);

/* --- selection --- */
void ed_select_set(ed_document *d, cw_entity_id id);
bool ed_select_add(ed_document *d, cw_entity_id id);
bool ed_select_remove(ed_document *d, cw_entity_id id);
void ed_select_toggle(ed_document *d, cw_entity_id id);
void ed_select_clear(ed_document *d);
void ed_select_all(ed_document *d);
bool ed_is_selected(const ed_document *d, cw_entity_id id);
cw_entity_id ed_primary(const ed_document *d);                   /* first selected, or CW_INVALID_ENTITY */

/* --- history --- */
void ed_commit(ed_document *d, const char *label);
bool ed_undo(ed_document *d);
bool ed_redo(ed_document *d);
bool ed_can_undo(const ed_document *d);
bool ed_can_redo(const ed_document *d);
const char *ed_undo_label(const ed_document *d);                 /* what undo would revert, or "" */

/* --- play mode --- */
bool ed_play(ed_document *d);
void ed_stop(ed_document *d);
void ed_toggle_pause(ed_document *d);
void ed_step(ed_document *d, int ticks);
int ed_frame(ed_document *d, float dt);                          /* advances when playing and not paused */

/* --- status --- */
#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
void ed_status(ed_document *d, bool error, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
#endif
