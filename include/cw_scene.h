/* cw_scene: the text scene format.
 *
 * A scene is what the editor edits and what a game loads at start: the
 * entities of a level with their components written field by field through
 * reflection (cw_reflect.h). It is line-oriented, diff-friendly and tolerant
 * of schema growth: a field missing from the file takes the component's
 * default, an unknown field is skipped and counted as a warning, while an
 * unknown component is an error unless the load config allows skipping it.
 *
 *   cwscene 1                      format version
 *   game combat 1                  module name and scene version (optional)
 *   tick 0                         simulation tick (optional)
 *   next_id 12                     id allocator (optional; at least max id + 1)
 *   entity 0 "red melee"           id, then an optional quoted display name
 *     Position x=200 y=150         one indented line per component
 *     Unit team=red kind=melee     enums by label, flags as a|b, bools as true/false
 *     ShadowCaster                 a tag component has no fields
 *     UiText text="hello world"    strings are quoted (\" and \\ escape)
 *   entity 1
 *     ...
 *
 * Entity ids are preserved (references between entities are plain ids), so a
 * scene loads into a fresh world: no entities, next_id zero, like a snapshot.
 * Transient components are never written. On any load error the world is
 * reset, so it is fresh again.
 *
 * Snapshots (clockwork.h) remain the binary runtime format for saved games;
 * scenes are the authored source.
 */
#ifndef CW_SCENE_H
#define CW_SCENE_H

#include "cw_reflect.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CW_SCENE_FORMAT_VERSION 1
#define CW_SCENE_NAME_CAPACITY 64
#define CW_SCENE_DEFAULT_MAX_SIZE ((size_t)64 << 20)

typedef struct {
    const char *game;              /* written to the header when set */
    uint32_t scene_version;
    /* Optional display names: return false for unnamed entities. */
    bool (*name_of)(void *user, cw_entity_id id, char *out, size_t capacity);
    void *user;
} cw_scene_save_config;

typedef struct {
    const char *game;              /* when set, the header's game name must match */
    bool ignore_unknown_components;/* skip components the world has not registered */
    bool keep_tick;                /* restore the saved tick (default: start at 0) */
    size_t max_entities;           /* 0 selects CW_SNAPSHOT_DEFAULT_MAX_ENTITIES */
    cw_entity_id max_entity_id;    /* 0 selects CW_SNAPSHOT_DEFAULT_MAX_ENTITY_ID */
    void (*on_name)(void *user, cw_entity_id id, const char *name);
    void *user;
} cw_scene_load_config;

typedef struct {
    bool ok;
    int line;                      /* where the problem or last warning was found; 0 when none */
    size_t entities, components, warnings;
    char message[160];
} cw_scene_result;

/* Text in memory. A NULL config selects the defaults. */
bool cw_scene_write(cw_world *w, const cw_scene_save_config *config, cw_writer *out, cw_scene_result *result);
/* A fragment: only the given entities, in that order (a prefab is one). */
bool cw_scene_write_entities(cw_world *w, const cw_entity_id *ids, size_t count, const cw_scene_save_config *config,
                             cw_writer *out, cw_scene_result *result);
/* Instantiates a scene or fragment into a live world: every entity in the
 * text becomes a new entity with a fresh id, references between them are
 * remapped (references to ids outside the text are kept as written), and
 * with `place` every position (CW_SEM_POSITION fields) is shifted so the
 * first entity lands at (x, y). The new ids are written to `out` (up to
 * `capacity`; `count` receives how many were created). On failure the
 * entities created so far are destroyed and the rest of the world is
 * untouched. next_id and tick in the text are ignored. */
bool cw_scene_instantiate(cw_world *w, const char *text, size_t size, const cw_scene_load_config *config,
                          bool place, float x, float y, cw_entity_id *out, size_t capacity, size_t *count,
                          cw_scene_result *result);
bool cw_scene_read(cw_world *w, const char *text, size_t size, const cw_scene_load_config *config,
                   cw_scene_result *result);
/* Files: saving writes a temporary and renames it over `path` (see cw_file_write). */
bool cw_scene_save_file(cw_world *w, const char *path, const cw_scene_save_config *config, cw_scene_result *result);
bool cw_scene_load_file(cw_world *w, const char *path, const cw_scene_load_config *config, cw_scene_result *result);

#ifdef __cplusplus
}
#endif
#endif
