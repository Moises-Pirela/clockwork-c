/* cw_reflect: component reflection for Clockwork.
 *
 * C has no runtime type information, so a game describes each component it
 * registers with a cw_type_info: the fields (name, type, offset, range,
 * labels) and what some of them mean (position, radius, colour...). A world
 * binds to that description through cw_reflect_register, and from then on
 * every generic tool works on any game's components by name:
 *
 *   - the generic codec (cw_reflect_codec) encodes and validates components
 *     for snapshots without hand-written marshal/unmarshal callbacks,
 *   - the scene format (cw_scene.h) writes and reads components as text,
 *   - the editor shows and edits every field through cw_field_format /
 *     cw_field_parse / cw_field_adjust and moves entities with
 *     cw_reflect_set_position.
 *
 * Descriptions are static data owned by the game module and borrowed by the
 * world; they must outlive every world that uses them.
 */
#ifndef CW_REFLECT_H
#define CW_REFLECT_H

#include "clockwork.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CW_FIELD_BOOL,      /* bool */
    CW_FIELD_U8,        /* uint8_t */
    CW_FIELD_I32,       /* int32_t */
    CW_FIELD_U32,       /* uint32_t */
    CW_FIELD_F32,       /* float; must be finite */
    CW_FIELD_ENTITY,    /* cw_entity_id reference, CW_INVALID_ENTITY for none */
    CW_FIELD_ENUM,      /* uint8_t index into `names` */
    CW_FIELD_FLAGS,     /* uint8_t bit set; bit i is labelled names[i] */
    CW_FIELD_STRING,    /* char[capacity], NUL-terminated (CW_FIELD_STR); scenes quote it */
    CW_FIELD_TYPES
} cw_field_type;

/* What a field means to tools that do not know the game. */
typedef enum {
    CW_SEM_NONE,
    CW_SEM_POSITION_X,  /* world position; one component per entity should carry X and Y */
    CW_SEM_POSITION_Y,
    CW_SEM_RADIUS,      /* world-space size, used for gizmos and picking */
    CW_SEM_ANGLE,       /* radians, shown in degrees */
    CW_SEM_COLOR_R, CW_SEM_COLOR_G, CW_SEM_COLOR_B,
    CW_SEM_RUNTIME,     /* simulation state: saved and loaded, but not shown in the inspector */
    CW_SEM_ASSET        /* a CW_FIELD_U32 holding an asset id (cw_assets.h): scenes write it as @name,
                           the editor picks it from the project's assets; 0 means none */
} cw_field_semantic;

typedef struct {
    const char *name;
    cw_field_type type;
    size_t offset;
    float min, max;            /* inclusive range when min < max, otherwise unbounded */
    float step;                /* inspector increment; 0 selects a default for the type */
    const char *const *names;  /* enum values / flag bits */
    unsigned name_count;
    cw_field_semantic semantic;
    unsigned capacity;         /* CW_FIELD_STRING: the array size including the terminator */
} cw_field;

enum {
    CW_TYPE_TRANSIENT = 1,     /* runtime-only: not saved, not offered in the editor */
    CW_TYPE_INTERNAL = 2,      /* saved, but hidden from the inspector and the add menu */
    CW_TYPE_CONFIG = 4,        /* authoring data that generates a runtime component (see `generates`) */
    CW_TYPE_RUNTIME = 8        /* generated when the world runs: in snapshots, never in scenes; read-only in the editor */
};
/* Baking: a config component's `bake` fills the runtime component it
 * generates (already set to that type's defaults) from the config and
 * anything else on the entity. Hosts run it when entities enter a running
 * world (cw_game: the cw_bake system in CW_ENTITY_CREATED, and
 * cw_game_world_bake_all when play starts on an edited scene). */
typedef void (*cw_bake_fn)(cw_world *w, cw_entity_id id, const void *config, void *runtime);
struct cw_type_info {
    const char *name;          /* the registered component name */
    size_t size, alignment;
    const cw_field *fields;
    size_t field_count;
    const void *defaults;      /* value for new instances and missing fields; NULL means all zero */
    bool (*validate)(const void *value);   /* invariants across fields; NULL means none */
    unsigned flags;            /* CW_TYPE_* */
    const char *generates;     /* CW_TYPE_CONFIG: name of the runtime component it produces */
    cw_bake_fn bake;           /* CW_TYPE_CONFIG: how */
};

/* Field initialisers: CW_FIELD(T, member, type) plus optional range/step,
 * labels and semantics. */
#define CW_FIELD(T, m, ft)                      {#m, ft, offsetof(T, m), 0, 0, 0, NULL, 0, CW_SEM_NONE, 0}
#define CW_FIELD_RANGE(T, m, ft, lo, hi, st)    {#m, ft, offsetof(T, m), lo, hi, st, NULL, 0, CW_SEM_NONE, 0}
#define CW_FIELD_SEM(T, m, ft, sem)             {#m, ft, offsetof(T, m), 0, 0, 0, NULL, 0, sem, 0}
#define CW_FIELD_RANGE_SEM(T, m, ft, lo, hi, st, sem) {#m, ft, offsetof(T, m), lo, hi, st, NULL, 0, sem, 0}
#define CW_FIELD_NAMED(T, m, ft, labels, n)     {#m, ft, offsetof(T, m), 0, 0, 0, labels, n, CW_SEM_NONE, 0}
/* A fixed-capacity string member: char m[N]. */
#define CW_FIELD_STR(T, m)                      {#m, CW_FIELD_STRING, offsetof(T, m), 0, 0, 0, NULL, 0, CW_SEM_NONE, (unsigned)sizeof(((T *)0)->m)}
#define CW_TYPE(T, fields_array, defaults_ptr, validate_fn, type_flags) \
    {#T, sizeof(T), _Alignof(T), fields_array, sizeof(fields_array) / sizeof((fields_array)[0]), defaults_ptr, validate_fn, type_flags}
/* A config type: authored in the editor and scenes, baked into `runtime_name` at run time. */
#define CW_CONFIG_TYPE(T, fields_array, defaults_ptr, validate_fn, runtime_name, bake_fn) \
    {#T, sizeof(T), _Alignof(T), fields_array, sizeof(fields_array) / sizeof((fields_array)[0]), defaults_ptr, validate_fn, CW_TYPE_CONFIG, runtime_name, bake_fn}
/* A runtime type: generated, saved in snapshots, never authored. */
#define CW_RUNTIME_TYPE(T, fields_array, defaults_ptr, validate_fn) \
    {#T, sizeof(T), _Alignof(T), fields_array, sizeof(fields_array) / sizeof((fields_array)[0]), defaults_ptr, validate_fn, CW_TYPE_RUNTIME, NULL, NULL}
/* A component with no fields (a tag). */
#define CW_TAG_TYPE(name_string) {name_string, 0, 1, NULL, 0, NULL, NULL, 0, NULL, NULL}

/* Registers the component described by `type` at id `ct`, attaches the
 * description, installs the generic codec, and marks it transient when the
 * flags say so. Checks the description first (cw_type_valid). */
bool cw_reflect_register(cw_world *w, cw_component_id ct, const cw_type_info *type);
bool cw_type_valid(const cw_type_info *type);
const cw_field *cw_type_field(const cw_type_info *type, const char *name);
const char *cw_field_type_name(cw_field_type type);
size_t cw_field_size(cw_field_type type);

/* Generic codec: fields in declaration order, little-endian, validated on
 * load (ranges, finite floats, enum/flag labels, then `validate`). */
bool cw_reflect_marshal(cw_world *w, cw_entity_id id, cw_component_id ct,
                        void *data, size_t capacity, size_t *size);
bool cw_reflect_unmarshal(cw_world *w, cw_entity_id id, cw_component_id ct,
                          const void *data, size_t size);
cw_codec cw_reflect_codec(void);

/* Values. `value` points at a component instance (in a world or a local). */
void cw_type_default(const cw_type_info *type, void *value);
bool cw_type_check(const cw_type_info *type, const void *value);   /* ranges and validate */
bool cw_field_check(const cw_field *f, const void *value);
double cw_field_get(const cw_field *f, const void *value);         /* numeric view */
bool cw_field_set(const cw_field *f, void *value, double number);  /* clamped and rounded as needed */
/* Increment by the field's step (direction +1/-1); enums cycle, bools
 * toggle, flags toggle bit `direction - 1` when positive (bit 0 for -1). */
bool cw_field_adjust(const cw_field *f, void *value, int direction);
float cw_field_step(const cw_field *f);
/* Text form used by the scene format and the inspector: numbers, true/false,
 * enum labels, flags as a|b (or none), entities as integers. */
size_t cw_field_format(const cw_field *f, const void *value, char *out, size_t capacity);
bool cw_field_parse(const cw_field *f, void *value, const char *text);

/* Entity-level helpers driven by semantics. */
bool cw_reflect_position(cw_world *w, cw_entity_id id, float *x, float *y);
bool cw_reflect_set_position(cw_world *w, cw_entity_id id, float x, float y);
float cw_reflect_radius(cw_world *w, cw_entity_id id, float fallback);
/* Rewrites every CW_FIELD_ENTITY reference on `id` through `map`; returns
 * the number of references changed. */
typedef cw_entity_id (*cw_entity_map_fn)(cw_entity_id reference, void *user);
size_t cw_reflect_remap(cw_world *w, cw_entity_id id, cw_entity_map_fn map, void *user);
/* Copies every non-transient component of `from` onto `to` (both valid). */
size_t cw_reflect_copy_entity(cw_world *w, cw_entity_id from, cw_entity_id to);
/* Baking: runs every config component's bake on `id` whose runtime
 * component is missing; returns how many were generated. The runtime
 * component must be registered under the config's `generates` name. */
size_t cw_reflect_bake(cw_world *w, cw_entity_id id);
/* Does any registered type generate another (i.e. is there anything to bake)? */
bool cw_reflect_has_bakes(const cw_world *w);
/* Authored types are neither transient, internal nor runtime. */
static inline bool cw_type_authored(const cw_type_info *t) {
    return t && !(t->flags & (CW_TYPE_TRANSIENT | CW_TYPE_INTERNAL | CW_TYPE_RUNTIME));
}

#ifdef __cplusplus
}
#endif
#endif
