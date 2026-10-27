#ifndef CLOCKWORK_H
#define CLOCKWORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CW_CHUNK_SIZE 256
#define CW_MAX_COMPONENTS 192
#define CW_SIGNATURE_WORDS ((CW_MAX_COMPONENTS + 63) / 64)
#ifndef CW_POOL_RESERVE_ENTITIES
#define CW_POOL_RESERVE_ENTITIES ((size_t)1 << 24)
#endif

typedef int64_t cw_entity_id;
typedef uint8_t cw_component_id;
#define CW_INVALID_ENTITY INT64_C(-1)
#define CW_INVALID_COMPONENT UINT8_MAX

enum { CW_ACTIVE = 1, CW_DEAD = 2, CW_TO_DESTROY = 4 };
typedef struct { uint64_t words[CW_SIGNATURE_WORDS]; } cw_signature;
typedef struct {
    cw_entity_id id;
    uint8_t flags;
    cw_signature signature;
} cw_entity;

typedef struct cw_world cw_world;
typedef void (*cw_component_fn)(void *slot);
typedef struct {
    bool (*marshal)(cw_world *, cw_entity_id, cw_component_id,
                    void *data, size_t capacity, size_t *size);
    bool (*unmarshal)(cw_world *, cw_entity_id, cw_component_id,
                      const void *data, size_t size);
} cw_codec;

typedef struct {
    void *data;
    uint64_t *bitset;
    size_t elem_size, alignment, capacity, bitset_count;
    size_t reserved_bytes, committed_bytes;
    char *name;
    cw_component_fn clear, init;
    cw_codec codec;
    bool transient;
} cw_component_pool;

typedef struct { cw_entity_id *data; size_t count, capacity; } cw_id_list;
typedef struct { const cw_entity_id *data; size_t count; } cw_id_view;

struct cw_world {
    cw_entity *entities;
    size_t capacity;
    cw_id_list recycled_ids, pending_destroy, created;
    cw_component_pool pools[CW_MAX_COMPONENTS];
    cw_entity_id next_id, last_created;
    unsigned next_block;
    uint64_t tick;
    bool recycle_ids;
    void *user;
};

bool cw_sig_set(cw_signature *sig, cw_component_id ct);
bool cw_sig_clear(cw_signature *sig, cw_component_id ct);
bool cw_sig_has(cw_signature sig, cw_component_id ct);
cw_signature cw_mask(const cw_component_id *types, size_t count);
bool cw_entity_is_active(cw_entity entity);
bool cw_query_visible(cw_entity entity);

void cw_world_init(cw_world *w, void *user);
void cw_world_destroy(cw_world *w);
void cw_world_reset(cw_world *w);
cw_entity_id cw_world_create_entity(cw_world *w);
bool cw_world_restore_entity(cw_world *w, cw_entity_id id);
bool cw_world_rebuild_entity(cw_world *w, cw_entity_id id);
void cw_world_mark_destroy(cw_world *w, cw_entity_id id);
void cw_world_sweep(cw_world *w);
bool cw_world_is_valid(const cw_world *w, cw_entity_id id);
size_t cw_world_entity_count(const cw_world *w);
void cw_world_ensure_capacity(cw_world *w, cw_entity_id id);
void cw_world_reserve_entities(cw_world *w, size_t count);
cw_id_view cw_world_created(const cw_world *w);
cw_id_view cw_world_pending_destroy(const cw_world *w);

cw_component_id cw_comp_block(cw_world *w, size_t count);
cw_component_id cw_cid(cw_component_id base, unsigned offset);
bool cw_comp_register(cw_world *w, cw_component_id ct, size_t size,
                      size_t alignment, const char *name, cw_component_fn clear);
bool cw_comp_register_at(cw_world *w, cw_component_id base, unsigned offset,
                         size_t size, size_t alignment, const char *name,
                         cw_component_fn clear);
#define CW_REGISTER(w, ct, T) \
    cw_comp_register((w), (ct), sizeof(T), _Alignof(T), #T, NULL)
void *cw_comp_ptr(cw_world *w, cw_entity_id id, cw_component_id ct);
void *cw_comp_get(cw_world *w, cw_entity_id id, cw_component_id ct, size_t size);
#define CW_GET(w, id, ct, T) ((T *)cw_comp_get((w), (id), (ct), sizeof(T)))
bool cw_comp_set(cw_world *w, cw_entity_id id, cw_component_id ct,
                 const void *value, size_t size);
bool cw_comp_add(cw_world *w, cw_entity_id id, cw_component_id ct);
bool cw_comp_remove(cw_world *w, cw_entity_id id, cw_component_id ct);
bool cw_comp_has(const cw_world *w, cw_entity_id id, cw_component_id ct);
bool cw_comp_set_init(cw_world *w, cw_component_id ct, cw_component_fn init);
const char *cw_comp_name(const cw_world *w, cw_component_id ct);
bool cw_comp_by_name(const cw_world *w, const char *name, cw_component_id *ct);
size_t cw_registered_components(const cw_world *w, cw_component_id *out, size_t capacity);
bool cw_comp_set_codec(cw_world *w, cw_component_id ct, cw_codec codec);
bool cw_comp_set_transient(cw_world *w, cw_component_id ct, bool transient);
bool cw_comp_is_transient(const cw_world *w, cw_component_id ct);
bool cw_comp_marshal(cw_world *w, cw_entity_id id, cw_component_id ct,
                     void *data, size_t capacity, size_t *size);
bool cw_comp_unmarshal(cw_world *w, cw_entity_id id, cw_component_id ct,
                       const void *data, size_t size);

typedef struct {
    cw_world *world;
    cw_signature mask;
    size_t word_index, base;
    uint64_t current_word;
} cw_query;
cw_query cw_query_iter(cw_world *w, cw_signature mask);
bool cw_query_next(cw_query *query, cw_entity_id *id);
size_t cw_query_collect(cw_world *w, cw_signature mask, cw_entity_id *out, size_t capacity);

typedef enum {
    CW_INIT, CW_START, CW_STOP, CW_SHUTDOWN,
    CW_EARLY_UPDATE, CW_PHYSICS_UPDATE, CW_UPDATE, CW_LATE_UPDATE,
    CW_OVERLAY, CW_ENTITY_CREATED, CW_ENTITY_DESTROYED,
    CW_USER0, CW_USER1, CW_USER2, CW_USER3, CW_END_FRAME
} cw_phase;
typedef uint16_t cw_phase_mask;
#define CW_PHASE(phase) ((cw_phase_mask)(UINT16_C(1) << (phase)))
typedef enum { CW_SHARED, CW_HOST_ONLY } cw_authority;
typedef enum { CW_HOST, CW_CLIENT } cw_sim_role;
typedef void (*cw_sim_fn)(cw_world *, cw_phase, float dt);
typedef struct {
    const char *name;
    cw_phase_mask phases;
    cw_sim_fn fn;
    cw_authority authority;
    int order;
    bool run_when_paused;
} cw_system_desc;
typedef struct {
    char *name;
    cw_phase_mask phases;
    cw_sim_fn fn;
    cw_authority authority;
    int order;
    bool enabled, run_when_paused;
    uint64_t sequence;
} cw_system;
typedef struct {
    cw_system *systems;
    size_t count, capacity;
    cw_sim_role role;
    bool paused, sorted;
    uint64_t next_sequence;
    unsigned dispatch_depth;
} cw_sim;

void cw_sim_init(cw_sim *sim, cw_sim_role role);
void cw_sim_destroy(cw_sim *sim);
bool cw_sim_register(cw_sim *sim, cw_system_desc desc);
bool cw_sim_remove(cw_sim *sim, const char *name);
bool cw_sim_set_enabled(cw_sim *sim, const char *name, bool enabled);
const cw_system *cw_sim_find(const cw_sim *sim, const char *name);
const cw_system *cw_sim_systems(cw_sim *sim, size_t *count);
void cw_sim_dispatch(cw_sim *sim, cw_world *w, cw_phase phase, float dt);
void cw_sim_replay(cw_sim *sim, cw_world *w, float dt);
void cw_sim_tick(cw_sim *sim, cw_world *w, float dt);
void cw_sim_entity_lifecycle(cw_sim *sim, cw_world *w, float dt);

#define CW_DEFAULT_TICK_HZ 30
#define CW_MAX_FRAME_DT 0.25f
#define CW_MAX_STEPS_PER_FRAME 5
#define CW_MAX_CATCHUP_STEPS_PER_FRAME 1
#define CW_INPUT_BACKLOG_TARGET 1
typedef struct {
    int hz;
    float dt, accumulator;
    int max_steps, max_catchup, backlog_target;
} cw_clock;
cw_clock cw_clock_make(int hz);
int cw_clock_advance(cw_clock *clock, float frame_dt);
bool cw_clock_should_drain(const cw_clock *clock, int backlog, int taken, int drained);
float cw_clock_alpha(const cw_clock *clock);
void cw_clock_reset(cw_clock *clock);

#endif
