/* cw_anim: keyframe animation of component fields.
 *
 * A clip animates fields of an entity's components over time: every track
 * names a component and a field (resolved through reflection when applied,
 * so a clip works on any entity carrying those components, in any game) and
 * holds keys sorted by time, each with a value and the interpolation from
 * it to the next key. Numeric fields interpolate; bools, enums and flags
 * step. Clips are text assets (`animations/<name>.cwanim`):
 *
 *   cwanim 1
 *   name walk
 *   length 1.5
 *   mode loop                     once | loop | pingpong
 *   track Position x
 *     key 0 100 linear            time value interpolation
 *     key 0.75 300 ease
 *     key 1.5 100 linear
 *   track Light intensity
 *     key 0 1.5 step
 *
 * At run time the engine gives every bound world an `Animation` component
 * (clip asset, speed, playing, loop override; `time` is runtime state) and
 * the cw_anim system, which advances playing animations each tick and
 * applies their clip, loaded from the project's assets and cached per
 * world. The editor's animation panel edits clips and previews them on the
 * selected entity without the component.
 */
#ifndef CW_ANIM_H
#define CW_ANIM_H

#include "cw_reflect.h"
#include "cw_assets.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CW_ANIM_FORMAT_VERSION 1
#define CW_ANIM_MAX_TRACKS 32
#define CW_ANIM_MAX_KEYS 256
#define CW_ANIM_NAME_CAPACITY 64

typedef enum { CW_INTERP_LINEAR, CW_INTERP_STEP, CW_INTERP_EASE, CW_INTERP_EASE_IN, CW_INTERP_EASE_OUT, CW_INTERP_COUNT } cw_interp;
typedef enum { CW_ANIM_ONCE, CW_ANIM_LOOP, CW_ANIM_PINGPONG, CW_ANIM_MODES } cw_anim_mode;

typedef struct { float time, value; cw_interp interp; } cw_anim_key;
typedef struct {
    char component[CW_ANIM_NAME_CAPACITY], field[CW_ANIM_NAME_CAPACITY];
    cw_anim_key keys[CW_ANIM_MAX_KEYS];
    size_t key_count;
} cw_anim_track;
typedef struct {
    char name[CW_ANIM_NAME_CAPACITY];
    float length;                 /* seconds; keys beyond it are kept but never reached */
    cw_anim_mode mode;
    cw_anim_track tracks[CW_ANIM_MAX_TRACKS];
    size_t track_count;
} cw_anim_clip;

const char *cw_interp_name(cw_interp interp);
bool cw_interp_parse(const char *text, cw_interp *out);
const char *cw_anim_mode_name(cw_anim_mode mode);
bool cw_anim_mode_parse(const char *text, cw_anim_mode *out);

/* --- authoring --- */
void cw_anim_clip_init(cw_anim_clip *clip, const char *name, float length);
int cw_anim_track_find(const cw_anim_clip *clip, const char *component, const char *field);
int cw_anim_track_add(cw_anim_clip *clip, const char *component, const char *field);   /* existing index when present, -1 when full */
bool cw_anim_track_remove(cw_anim_clip *clip, size_t index);
/* Sets the key at `time` (replacing one within 1 ms) and keeps the track
 * sorted; returns the key's index or -1 when the track is full. */
int cw_anim_key_set(cw_anim_track *track, float time, float value, cw_interp interp);
bool cw_anim_key_remove(cw_anim_track *track, size_t index);
int cw_anim_key_move(cw_anim_track *track, size_t index, float time);   /* returns the new index */
int cw_anim_key_find(const cw_anim_track *track, float time);           /* within 1 ms, else -1 */

/* --- evaluation --- */
float cw_anim_ease(cw_interp interp, float t);                          /* t in [0, 1] */
float cw_anim_track_eval(const cw_anim_track *track, float time);       /* holds the first/last key outside */
/* Maps an unbounded time onto the clip according to its mode; `finished`
 * is set for CW_ANIM_ONCE past the end. */
float cw_anim_wrap(const cw_anim_clip *clip, float time, bool *finished);
/* Writes every track's value at `time` into the entity's components
 * (through cw_field_set, so ranges are honoured); tracks whose component
 * or field the entity does not have are skipped. Returns tracks applied. */
size_t cw_anim_apply(const cw_anim_clip *clip, cw_world *w, cw_entity_id id, float time);
/* Can this field be animated (numeric, bool, enum or flags)? */
bool cw_anim_field_animatable(const cw_field *field);

/* --- text --- */
bool cw_anim_write(const cw_anim_clip *clip, cw_writer *out);
bool cw_anim_parse(cw_anim_clip *clip, const char *text, size_t size, char *error, size_t error_capacity);
bool cw_anim_save(const cw_anim_clip *clip, const char *path, char *error, size_t error_capacity);
bool cw_anim_load(cw_anim_clip *clip, const char *path, char *error, size_t error_capacity);

/* --- runtime ---
 * The Animation component the engine registers in bound worlds. */
typedef struct {
    uint32_t clip;      /* asset id of the .cwanim (CW_SEM_ASSET) */
    float speed;        /* playback rate */
    bool playing;
    uint8_t mode;       /* 0: as the clip says; else 1 + cw_anim_mode */
    float time;         /* runtime: position in the clip */
} cw_animation;
extern const cw_type_info cw_animation_type;
extern const char *const cw_anim_mode_override_names[4];
/* A per-world cache of loaded clips keyed by asset id. */
typedef struct { uint32_t id; bool missing; cw_anim_clip clip; } cw_anim_entry;
typedef struct { cw_anim_entry *items; size_t count, capacity; } cw_anim_cache;
void cw_anim_cache_init(cw_anim_cache *cache);
void cw_anim_cache_destroy(cw_anim_cache *cache);
void cw_anim_cache_clear(cw_anim_cache *cache);     /* after clips changed on disk */
/* Loads on first use through `assets`; NULL when unknown or unreadable. */
const cw_anim_clip *cw_anim_cache_get(cw_anim_cache *cache, const cw_assets *assets, uint32_t id);
/* Stores a clip directly (an editor previewing unsaved edits, or tests). */
void cw_anim_cache_put(cw_anim_cache *cache, uint32_t id, const cw_anim_clip *clip);
/* Advances one Animation component by dt and applies its clip; returns
 * whether a clip was applied. The host's system calls this per entity. */
bool cw_anim_step(cw_world *w, cw_entity_id id, cw_component_id animation_ct, cw_anim_cache *cache,
                  const cw_assets *assets, float dt);

#ifdef __cplusplus
}
#endif
#endif
