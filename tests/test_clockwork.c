#include "clockwork.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)
#define RUN(test) do { test(); ++tests; printf("PASS %s\n", #test); } while (0)

static unsigned clears, inits;
static void init_value(void *slot) { *(int64_t *)slot = 7; ++inits; }
static void clear_value(void *slot) { *(int64_t *)slot = 0; ++clears; }
static void register_value(cw_world *w, cw_component_id ct) {
    CHECK(cw_comp_register(w, ct, sizeof(int64_t), _Alignof(int64_t), "Value", clear_value));
}
static void set_value(cw_world *w, cw_entity_id id, cw_component_id ct, int64_t value) {
    CHECK(cw_comp_set(w, id, ct, &value, sizeof(value)));
}
static cw_signature value_mask(cw_component_id ct) { return cw_mask(&ct, 1); }
static cw_signature no_mask(void) { return cw_mask(NULL, 0); }

static void test_entity_lifetime(void) {
    cw_world w;
    int user = 42;
    cw_world_init(&w, &user);
    CHECK(w.user == &user && w.capacity == CW_CHUNK_SIZE);
    CHECK(w.last_created == CW_INVALID_ENTITY && !w.recycle_ids);
    CHECK(!cw_world_is_valid(&w, 0));
    CHECK(!cw_world_is_valid(&w, -1));
    CHECK(!cw_world_is_valid(&w, INT64_MAX));
    cw_entity_id a = cw_world_create_entity(&w), b = cw_world_create_entity(&w);
    CHECK(a == 0 && b == 1 && cw_world_entity_count(&w) == 2);
    cw_id_view created = cw_world_created(&w);
    CHECK(created.count == 2 && created.data[0] == a && created.data[1] == b);
    cw_world_mark_destroy(&w, a);
    cw_world_mark_destroy(&w, a);
    cw_world_mark_destroy(&w, -1);
    CHECK(cw_world_pending_destroy(&w).count == 1);
    CHECK(cw_world_is_valid(&w, a) && cw_world_entity_count(&w) == 2);
    cw_entity_id out[2];
    CHECK(cw_query_collect(&w, (cw_signature){0}, out, 2) == 1 && out[0] == b);
    cw_world_sweep(&w);
    CHECK(!cw_world_is_valid(&w, a) && cw_world_entity_count(&w) == 1);
    CHECK(cw_world_create_entity(&w) == 2);
    cw_world_destroy(&w);
    CHECK(w.entities == NULL && w.capacity == 0);
    cw_world_destroy(&w);
}

static void test_registry_and_bounds(void) {
    cw_world w;
    cw_world_init(&w, NULL);
    CHECK(cw_comp_block(&w, 0) == CW_INVALID_COMPONENT);
    CHECK(cw_comp_block(&w, 64) == 0);
    CHECK(cw_comp_block(&w, 128) == 64);
    CHECK(cw_comp_block(&w, 1) == CW_INVALID_COMPONENT);
    CHECK(cw_cid(128, 63) == 191);
    CHECK(cw_cid(128, UINT32_MAX) == CW_INVALID_COMPONENT);
    CHECK(cw_cid(CW_INVALID_COMPONENT, 0) == CW_INVALID_COMPONENT);
    CHECK(cw_comp_register_at(&w, 128, 63, sizeof(int64_t), _Alignof(int64_t), "high", NULL));
    CHECK(!cw_comp_register_at(&w, 128, 64, 8, 8, "bad", NULL));
    CHECK(!cw_comp_register(&w, 191, 8, 8, "duplicate", NULL));
    CHECK(!cw_comp_register(&w, 0, 8, 3, "bad", NULL));
    CHECK(!cw_comp_register(&w, 0, 3, 2, "bad", NULL));
    CHECK(!cw_comp_register(&w, CW_INVALID_COMPONENT, 8, 8, "bad", NULL));
    char name[] = "OwnedName";
    CHECK(cw_comp_register(&w, 0, 8, 8, name, NULL));
    name[0] = 'X';
    CHECK(strcmp(cw_comp_name(&w, 0), "OwnedName") == 0);
    cw_component_id ct = CW_INVALID_COMPONENT;
    CHECK(cw_comp_by_name(&w, "OwnedName", &ct) && ct == 0);
    CHECK(!cw_comp_by_name(&w, "ownedname", &ct));
    cw_component_id types[1];
    CHECK(cw_registered_components(&w, types, 1) == 2 && types[0] == 0);
    CHECK(cw_registered_components(&w, NULL, 0) == 2);
    CHECK(cw_comp_set_transient(&w, 191, true) && cw_comp_is_transient(&w, 191));
    CHECK(cw_comp_set_transient(&w, 191, false) && !cw_comp_is_transient(&w, 191));
    CHECK(!cw_comp_set_transient(&w, 190, true));
    cw_signature mask = {0};
    CHECK(cw_sig_set(&mask, 191) && cw_sig_has(mask, 191));
    CHECK(cw_sig_clear(&mask, 191) && !cw_sig_has(mask, 191));
    CHECK(!cw_sig_set(&mask, CW_INVALID_COMPONENT));
    CHECK(!cw_sig_has(mask, CW_INVALID_COMPONENT));
    cw_world_destroy(&w);
}

static void test_components_and_callbacks(void) {
    cw_world w;
    clears = inits = 0;
    cw_world_init(&w, NULL);
    register_value(&w, 0);
    CHECK(cw_comp_set_init(&w, 0, init_value));
    cw_entity_id id = cw_world_create_entity(&w);
    CHECK(CW_GET(&w, id, 0, int64_t) != NULL && !cw_comp_has(&w, id, 0));
    CHECK(cw_comp_add(&w, id, 0));
    CHECK(*CW_GET(&w, id, 0, int64_t) == 7 && inits == 1);
    set_value(&w, id, 0, 9);
    CHECK(cw_comp_add(&w, id, 0));
    CHECK(*CW_GET(&w, id, 0, int64_t) == 9 && inits == 1 && clears == 0);
    CHECK(CW_GET(&w, id, 0, int32_t) == NULL);
    CHECK(CW_GET(&w, -1, 0, int64_t) == NULL);
    CHECK(!cw_comp_add(&w, id, 191));
    CHECK(!cw_comp_add(&w, 10, 0));
    CHECK(!cw_comp_set(&w, id, 0, NULL, sizeof(int64_t)));
    CHECK(cw_comp_remove(&w, id, 0) && clears == 1);
    CHECK(!cw_comp_remove(&w, id, 0) && clears == 1);
    CHECK(cw_comp_add(&w, id, 0) && inits == 2);
    cw_world_mark_destroy(&w, id);
    CHECK(cw_comp_has(&w, id, 0) && *CW_GET(&w, id, 0, int64_t) == 7);
    CHECK(cw_query_collect(&w, value_mask(0), NULL, 0) == 0);
    cw_world_sweep(&w);
    CHECK(clears == 2 && CW_GET(&w, id, 0, int64_t) == NULL);
    cw_world_destroy(&w);
    CHECK(clears == 2);
}

static void test_queries_across_words(void) {
    cw_world w;
    cw_world_init(&w, NULL);
    const cw_component_id types[] = {0, 63, 64, 127, 128, 191};
    for (size_t t = 0; t < sizeof(types); ++t) CHECK(CW_REGISTER(&w, types[t], int64_t));
    for (int i = 0; i < 600; ++i) {
        cw_entity_id id = cw_world_create_entity(&w);
        for (size_t t = 0; t < sizeof(types); ++t)
            if (i % (int)(t + 2) == 0) set_value(&w, id, types[t], i);
    }
    cw_signature mask = cw_mask(types, sizeof(types));
    cw_entity_id ids[3] = {-9, -9, -9};
    CHECK(cw_query_collect(&w, mask, ids, 3) == 2);
    CHECK(ids[0] == 0 && ids[1] == 420 && ids[2] == -9);
    CHECK(cw_query_collect(&w, value_mask(190), NULL, 0) == 0);
    CHECK(cw_query_collect(&w, (cw_signature){0}, NULL, 0) == 600);
    cw_query q = cw_query_iter(&w, value_mask(0));
    cw_entity_id id;
    CHECK(cw_query_next(&q, &id) && id == 0);
    CHECK(cw_comp_remove(&w, 2, 0));
    cw_world_mark_destroy(&w, 4);
    CHECK(cw_query_next(&q, &id) && id == 6);
    cw_world_sweep(&w);
    size_t count = 2;
    while (cw_query_next(&q, &id)) { CHECK(id % 2 == 0); ++count; }
    CHECK(count == 298 && id == CW_INVALID_ENTITY);
    cw_world_destroy(&w);
}

static void test_restoration_and_rebuild(void) {
    cw_world w;
    clears = 0;
    cw_world_init(&w, NULL);
    register_value(&w, 0);
    cw_entity_id id = cw_world_create_entity(&w);
    set_value(&w, id, 0, 19);
    cw_world_mark_destroy(&w, id);
    CHECK(!cw_world_restore_entity(&w, id));
    CHECK(cw_world_pending_destroy(&w).count == 0);
    CHECK(*CW_GET(&w, id, 0, int64_t) == 19 && clears == 0);
    cw_world_mark_destroy(&w, id);
    CHECK(cw_world_rebuild_entity(&w, id));
    CHECK(!cw_comp_has(&w, id, 0) && clears == 1 && w.pending_destroy.count == 0);
    CHECK(w.created.count == 2 && cw_world_is_valid(&w, id));
    set_value(&w, id, 0, 41);
    cw_world_mark_destroy(&w, id);
    cw_world_sweep(&w);
    CHECK(clears == 2 && cw_world_restore_entity(&w, id));
    CHECK(!cw_comp_has(&w, id, 0) && cw_world_create_entity(&w) != id);
    CHECK(cw_world_restore_entity(&w, 900));
    CHECK(cw_world_create_entity(&w) == 901);
    CHECK(!cw_world_restore_entity(&w, -1));
    CHECK(!cw_world_rebuild_entity(&w, 800));
    cw_world_destroy(&w);
}

static void test_recycling_and_reset(void) {
    cw_world w;
    clears = 0;
    cw_world_init(&w, NULL);
    register_value(&w, 0);
    w.recycle_ids = true;
    cw_entity_id a = cw_world_create_entity(&w), b = cw_world_create_entity(&w);
    set_value(&w, a, 0, 1);
    set_value(&w, b, 0, 2);
    cw_world_mark_destroy(&w, a);
    cw_world_mark_destroy(&w, b);
    cw_world_sweep(&w);
    CHECK(clears == 2 && w.recycled_ids.count == 2);
    CHECK(cw_world_restore_entity(&w, a));
    CHECK(w.recycled_ids.count == 1 && cw_world_create_entity(&w) == b);
    CHECK(!cw_comp_has(&w, b, 0));
    set_value(&w, a, 0, 11);
    set_value(&w, b, 0, 12);
    w.tick = 7;
    CHECK(cw_comp_block(&w, 10) == 0);
    void *data = w.pools[0].data;
    cw_world_reset(&w);
    CHECK(clears == 4 && w.tick == 0 && w.next_id == 0 && w.next_block == 10);
    CHECK(w.created.count == 0 && w.pending_destroy.count == 0 && w.recycled_ids.count == 0);
    CHECK(cw_world_entity_count(&w) == 0 && w.pools[0].data == data);
    CHECK(cw_query_collect(&w, value_mask(0), NULL, 0) == 0);
    CHECK(cw_world_create_entity(&w) == 0);
    CHECK(CW_GET(&w, 0, 0, int64_t) && *CW_GET(&w, 0, 0, int64_t) == 0);
    cw_world_destroy(&w);
}

static void test_batch_reservation(void) {
    cw_world w;
    cw_world_init(&w, NULL);
    CHECK(CW_REGISTER(&w, 0, int64_t));
    cw_entity_id a = cw_world_create_entity(&w);
    set_value(&w, a, 0, 123456);
    int64_t *original = CW_GET(&w, a, 0, int64_t);
    cw_world_reserve_entities(&w, 18000);
    size_t capacity = w.pools[0].capacity;
    CHECK(capacity >= (size_t)w.next_id + 18000);
    for (int i = 0; i < 18000; ++i) {
        cw_entity_id e = cw_world_create_entity(&w);
        CHECK(e != a && w.pools[0].capacity == capacity);
        cw_world_mark_destroy(&w, e);
    }
    cw_world_sweep(&w);
    CHECK(*original == 123456 && original == CW_GET(&w, a, 0, int64_t));
    cw_entity_id last = w.next_id;
    cw_world_reserve_entities(&w, 18000);
    CHECK(original == CW_GET(&w, a, 0, int64_t));
    CHECK(cw_world_create_entity(&w) == last && !cw_world_is_valid(&w, a + 1));
    CHECK(CW_REGISTER(&w, 191, int64_t));
    set_value(&w, last, 191, 987);
    CHECK(*CW_GET(&w, last, 191, int64_t) == 987);
    cw_world_destroy(&w);
}

static void test_memory_stats(void) {
    cw_world w;
    CHECK(cw_world_memory(NULL).total == 0);
    cw_world_init(&w, NULL);
    cw_memory_stats m = cw_world_memory(&w);
    CHECK(m.entities == CW_CHUNK_SIZE * sizeof(cw_entity) && m.pool_committed == 0);
    CHECK(m.bitsets == (CW_CHUNK_SIZE / 64) * sizeof(uint64_t) && m.queries == 0 && m.lists == 0);
    CHECK(m.total == m.entities + m.bitsets);
    CHECK(CW_REGISTER(&w, 0, int64_t));
    m = cw_world_memory(&w);
    CHECK(m.pool_committed >= CW_CHUNK_SIZE * sizeof(int64_t) && m.pool_reserved >= m.pool_committed);
    CHECK(m.bitsets == 2 * (CW_CHUNK_SIZE / 64) * sizeof(uint64_t));
    cw_cached_query *q = cw_query_create(&w, cw_filter_all(value_mask(0)));
    size_t before = cw_world_memory(&w).queries;
    CHECK(before >= (CW_CHUNK_SIZE / 64) * sizeof(uint64_t) + sizeof(void *));
    cw_world_reserve_entities(&w, 10000);
    m = cw_world_memory(&w);
    CHECK(m.entities == w.capacity * sizeof(cw_entity) && m.queries > before);
    CHECK(m.pool_committed >= w.capacity * sizeof(int64_t) && m.lists >= 10000 * sizeof(cw_entity_id));
    CHECK(m.total == m.entities + m.pool_committed + m.bitsets + m.queries + m.lists);
    cw_query_destroy(q);
    CHECK(cw_world_memory(&w).queries < m.queries);
    cw_world_destroy(&w);
    CHECK(cw_world_memory(&w).total == 0);
}

static void test_alignment_and_tags(void) {
    cw_world w;
    cw_world_init(&w, NULL);
    CHECK(cw_comp_register(&w, 0, 64, 64, "Aligned", NULL));
    CHECK(cw_comp_register(&w, 1, 0, 1, "Tag", NULL));
    for (int i = 0; i < 600; ++i) {
        cw_entity_id id = cw_world_create_entity(&w);
        CHECK(cw_comp_add(&w, id, 0));
        CHECK((uintptr_t)cw_comp_ptr(&w, id, 0) % 64 == 0);
        CHECK(cw_comp_set(&w, id, 1, NULL, 0));
    }
    CHECK(cw_query_collect(&w, value_mask(1), NULL, 0) == 600);
    cw_world_destroy(&w);
}

static bool marshal_value(cw_world *w, cw_entity_id id, cw_component_id ct,
                          void *data, size_t capacity, size_t *size) {
    char buffer[64];
    int n = snprintf(buffer, sizeof(buffer), "v=%" PRId64, *CW_GET(w, id, ct, int64_t));
    if (n < 0) return false;
    *size = (size_t)n;
    if (!data) return true;
    if (capacity < *size) return false;
    memcpy(data, buffer, *size);
    return true;
}
static bool unmarshal_value(cw_world *w, cw_entity_id id, cw_component_id ct,
                            const void *data, size_t size) {
    if (!data || size != 4 || memcmp(data, "v=42", 4) != 0) return false;
    *CW_GET(w, id, ct, int64_t) = 42;
    return true;
}
static void test_codecs(void) {
    cw_world w;
    cw_world_init(&w, NULL);
    CHECK(CW_REGISTER(&w, 0, int64_t));
    cw_entity_id id = cw_world_create_entity(&w);
    size_t size = 9;
    CHECK(!cw_comp_marshal(&w, id, 0, NULL, 0, &size) && size == 0);
    cw_codec codec = {marshal_value, unmarshal_value};
    CHECK(cw_comp_set_codec(&w, 0, codec));
    CHECK(!cw_comp_unmarshal(&w, id, 0, "bad", 3) && !cw_comp_has(&w, id, 0));
    CHECK(cw_comp_unmarshal(&w, id, 0, "v=42", 4));
    CHECK(cw_comp_has(&w, id, 0) && *CW_GET(&w, id, 0, int64_t) == 42);
    CHECK(cw_comp_marshal(&w, id, 0, NULL, 0, &size) && size == 4);
    char data[4];
    CHECK(!cw_comp_marshal(&w, id, 0, data, 3, &size));
    CHECK(cw_comp_marshal(&w, id, 0, data, 4, &size));
    CHECK(memcmp(data, "v=42", 4) == 0);
    CHECK(!cw_comp_unmarshal(&w, id, 0, "bad", 3));
    CHECK(*CW_GET(&w, id, 0, int64_t) == 42);
    CHECK(!cw_comp_unmarshal(&w, -1, 0, "v=42", 4));
    cw_world_destroy(&w);
}

typedef struct { int events[128]; size_t count; cw_sim *sim; cw_entity_id victim; } trace;
static void record(cw_world *w, int event) {
    trace *t = w->user;
    CHECK(t->count < 128);
    t->events[t->count++] = event;
}
static void system_a(cw_world *w, cw_phase phase, float dt) { (void)phase; (void)dt; record(w, 1); }
static void system_b(cw_world *w, cw_phase phase, float dt) { (void)phase; (void)dt; record(w, 2); }
static void system_c(cw_world *w, cw_phase phase, float dt) { (void)phase; (void)dt; record(w, 3); }
static cw_system_desc system_desc(const char *name, cw_sim_fn fn, int order, cw_authority authority) {
    cw_system_desc d = {0};
    d.name = name;
    d.fn = fn;
    d.order = order;
    d.authority = authority;
    d.phases = CW_PHASE(CW_UPDATE);
    return d;
}
static void expect_trace(trace *t, const int *events, size_t count) {
    CHECK(t->count == count);
    for (size_t i = 0; i < count; ++i) CHECK(t->events[i] == events[i]);
    t->count = 0;
}
static void test_scheduler(void) {
    cw_world w;
    cw_sim s;
    trace t = {0};
    cw_world_init(&w, &t);
    cw_sim_init(&s, CW_HOST);
    CHECK(cw_sim_register(&s, system_desc("a", system_a, 10, CW_SHARED)));
    CHECK(cw_sim_register(&s, system_desc("b", system_b, -10, CW_HOST_ONLY)));
    CHECK(cw_sim_register(&s, system_desc("c", system_c, 10, CW_SHARED)));
    cw_sim_dispatch(&s, &w, CW_UPDATE, 0);
    expect_trace(&t, (int[]){2, 1, 3}, 3);
    CHECK(cw_sim_remove(&s, "b"));
    CHECK(cw_sim_register(&s, system_desc("b", system_b, 10, CW_HOST_ONLY)));
    cw_sim_dispatch(&s, &w, CW_UPDATE, 0);
    expect_trace(&t, (int[]){1, 3, 2}, 3);
    CHECK(cw_sim_set_enabled(&s, "a", false));
    CHECK(cw_sim_register(&s, system_desc("a", system_b, 10, CW_SHARED)));
    CHECK(s.count == 3);
    cw_sim_dispatch(&s, &w, CW_UPDATE, 0);
    expect_trace(&t, (int[]){2, 3, 2}, 3);
    s.role = CW_CLIENT;
    cw_sim_dispatch(&s, &w, CW_UPDATE, 0);
    expect_trace(&t, (int[]){2, 3}, 2);
    s.paused = true;
    cw_sim_dispatch(&s, &w, CW_UPDATE, 0);
    expect_trace(&t, NULL, 0);
    cw_sim_replay(&s, &w, 0);
    expect_trace(&t, (int[]){2, 3}, 2);
    cw_system_desc desc = system_desc("c", system_c, 10, CW_SHARED);
    desc.run_when_paused = true;
    CHECK(cw_sim_register(&s, desc));
    cw_sim_dispatch(&s, &w, CW_UPDATE, 0);
    expect_trace(&t, (int[]){3}, 1);
    CHECK(cw_sim_set_enabled(&s, "c", false));
    cw_sim_replay(&s, &w, 0);
    expect_trace(&t, (int[]){2}, 1);
    size_t count;
    CHECK(cw_sim_systems(&s, &count) != NULL && count == 3);
    CHECK(cw_sim_find(&s, "c") && !cw_sim_find(&s, "c")->enabled);
    CHECK(!cw_sim_remove(&s, "missing") && !cw_sim_set_enabled(&s, "missing", true));
    cw_sim_destroy(&s);
    cw_world_destroy(&w);
}

static void lifecycle_system(cw_world *w, cw_phase phase, float dt) {
    (void)dt;
    trace *t = w->user;
    record(w, (int)phase);
    if (phase == CW_ENTITY_DESTROYED) {
        CHECK(cw_world_is_valid(w, t->victim));
        CHECK(cw_world_pending_destroy(w).data[0] == t->victim);
        CHECK(*CW_GET(w, t->victim, 0, int64_t) == 88);
        CHECK(cw_query_collect(w, value_mask(0), NULL, 0) == 0);
    } else if (phase == CW_ENTITY_CREATED) {
        CHECK(!cw_world_is_valid(w, t->victim));
        CHECK(cw_world_created(w).count == 1);
    }
}
static void test_tick_lifecycle(void) {
    cw_world w;
    cw_sim s;
    trace t = {0};
    cw_world_init(&w, &t);
    cw_sim_init(&s, CW_HOST);
    CHECK(CW_REGISTER(&w, 0, int64_t));
    t.victim = cw_world_create_entity(&w);
    set_value(&w, t.victim, 0, 88);
    cw_world_mark_destroy(&w, t.victim);
    cw_system_desc desc = system_desc("lifecycle", lifecycle_system, 0, CW_SHARED);
    desc.phases = UINT16_MAX;
    CHECK(cw_sim_register(&s, desc));
    cw_sim_tick(&s, &w, 1.0f / 30.0f);
    expect_trace(&t, (int[]){CW_EARLY_UPDATE, CW_PHYSICS_UPDATE, CW_UPDATE,
        CW_LATE_UPDATE, CW_END_FRAME, CW_ENTITY_DESTROYED, CW_ENTITY_CREATED}, 7);
    CHECK(w.tick == 1 && w.created.count == 0 && w.pending_destroy.count == 0);
    s.paused = true;
    cw_sim_tick(&s, &w, 0);
    expect_trace(&t, NULL, 0);
    CHECK(w.tick == 2);
    cw_sim_dispatch(&s, &w, CW_START, 0);
    cw_sim_dispatch(&s, &w, CW_OVERLAY, 0);
    cw_sim_dispatch(&s, &w, CW_USER0, 0);
    expect_trace(&t, (int[]){CW_START, CW_OVERLAY, CW_USER0}, 3);
    cw_sim_destroy(&s);
    cw_world_destroy(&w);
}

static void mutate_scheduler(cw_world *w, cw_phase phase, float dt) {
    (void)phase; (void)dt;
    trace *t = w->user;
    CHECK(!cw_sim_register(t->sim, system_desc("new", system_a, 0, CW_SHARED)));
    CHECK(!cw_sim_remove(t->sim, "self"));
    CHECK(cw_sim_set_enabled(t->sim, "later", false));
    record(w, 9);
}
static void test_scheduler_mutation_guard(void) {
    cw_world w;
    cw_sim s;
    trace t = {0};
    t.sim = &s;
    cw_world_init(&w, &t);
    cw_sim_init(&s, CW_HOST);
    CHECK(cw_sim_register(&s, system_desc("self", mutate_scheduler, 0, CW_SHARED)));
    CHECK(cw_sim_register(&s, system_desc("later", system_b, 1, CW_SHARED)));
    cw_sim_tick(&s, &w, 0);
    expect_trace(&t, (int[]){9}, 1);
    CHECK(s.dispatch_depth == 0 && s.count == 2);
    cw_sim_destroy(&s);
    cw_world_destroy(&w);
}

static void test_clock(void) {
    cw_clock c = cw_clock_make(4);
    CHECK(c.hz == 4 && c.dt == 0.25f);
    CHECK(cw_clock_advance(&c, 0.125f) == 0 && cw_clock_alpha(&c) == 0.5f);
    CHECK(cw_clock_advance(&c, 0.125f) == 1 && cw_clock_alpha(&c) == 0);
    CHECK(cw_clock_advance(&c, 100) == 1);
    CHECK(cw_clock_advance(&c, -1) == 0 && cw_clock_alpha(&c) == 0);
    CHECK(cw_clock_advance(&c, NAN) == 0);
    c = cw_clock_make(60);
    CHECK(cw_clock_advance(&c, 1) == CW_MAX_STEPS_PER_FRAME && c.accumulator == 0);
    CHECK(cw_clock_should_drain(&c, 2, 1, 0));
    CHECK(!cw_clock_should_drain(&c, 1, 1, 0));
    CHECK(!cw_clock_should_drain(&c, 2, 5, 0));
    CHECK(!cw_clock_should_drain(&c, 2, 1, 1));
    CHECK(!cw_clock_should_drain(&c, 2, -1, 0));
    c.accumulator = c.dt * 2;
    CHECK(cw_clock_alpha(&c) == 1);
    cw_clock_reset(&c);
    CHECK(c.accumulator == 0);
    CHECK(cw_clock_make(0).hz == 1);
}

static cw_signature mask2(cw_component_id a, cw_component_id b) {
    cw_component_id types[] = {a, b};
    return cw_mask(types, 2);
}
static cw_filter filter3(cw_signature all, cw_signature any, cw_signature none) {
    return cw_filter_make(all, any, none);
}
/* Cross-check a cached query against an ad-hoc evaluation of the same filter. */
static void check_cached_matches_adhoc(cw_cached_query *q) {
    cw_world *w = cw_query_world(q);
    cw_entity_id *expected = malloc(w->capacity * sizeof(*expected));
    cw_entity_id *actual = malloc(w->capacity * sizeof(*actual));
    CHECK(expected && actual);
    size_t n = cw_query_collect_filter(w, cw_query_filter(q), expected, w->capacity);
    CHECK(n <= w->capacity && cw_query_count(q) == n);
    CHECK(cw_query_gather(q, actual, w->capacity) == n);
    CHECK(memcmp(expected, actual, n * sizeof(*actual)) == 0);
    cw_query it = cw_query_begin(q);
    cw_entity_id id;
    size_t i = 0;
    while (cw_query_next(&it, &id)) { CHECK(i < n && actual[i] == id); ++i; }
    CHECK(i == n && id == CW_INVALID_ENTITY);
    for (cw_entity_id e = 0; (size_t)e < w->capacity; ++e)
        CHECK(cw_query_contains(q, e) == cw_filter_matches(cw_query_filter(q), w->entities[e]));
    free(expected);
    free(actual);
}

static void test_filters(void) {
    cw_world w;
    cw_world_init(&w, NULL);
    enum { A = 0, B = 63, C = 64, D = 191 };
    const cw_component_id types[] = {A, B, C, D};
    for (size_t t = 0; t < 4; ++t) CHECK(CW_REGISTER(&w, types[t], int64_t));
    /* entity i carries component t when bit t of i is set */
    for (int i = 0; i < 16; ++i) {
        cw_entity_id id = cw_world_create_entity(&w);
        for (size_t t = 0; t < 4; ++t) if (i & (1 << t)) set_value(&w, id, types[t], i);
    }
    cw_entity_id out[16];
    cw_filter f = filter3(value_mask(A), no_mask(), no_mask());
    CHECK(cw_query_collect_filter(&w, f, out, 16) == 8 && out[0] == 1 && out[7] == 15);
    f = filter3(no_mask(), mask2(B, C), no_mask());
    CHECK(cw_query_collect_filter(&w, f, out, 16) == 12 && out[0] == 2 && out[1] == 3);
    f = filter3(no_mask(), no_mask(), mask2(B, C));
    CHECK(cw_query_collect_filter(&w, f, out, 16) == 4);
    CHECK(out[0] == 0 && out[1] == 1 && out[2] == 8 && out[3] == 9);
    f = filter3(value_mask(A), mask2(B, C), value_mask(D));
    CHECK(cw_query_collect_filter(&w, f, out, 16) == 3);
    CHECK(out[0] == 3 && out[1] == 5 && out[2] == 7);
    CHECK(cw_filter_matches(f, w.entities[3]) && !cw_filter_matches(f, w.entities[11]));
    cw_world_mark_destroy(&w, 3);
    CHECK(!cw_filter_matches(f, w.entities[3]));
    CHECK(cw_query_collect_filter(&w, f, NULL, 0) == 2);
    cw_component_id unregistered = 100;
    f = filter3(value_mask(unregistered), no_mask(), no_mask());
    CHECK(cw_query_collect_filter(&w, f, NULL, 0) == 0);
    f = filter3(no_mask(), no_mask(), value_mask(unregistered));
    CHECK(cw_query_collect_filter(&w, f, NULL, 0) == 15);
    f = filter3(no_mask(), value_mask(unregistered), no_mask());
    CHECK(cw_query_collect_filter(&w, f, NULL, 0) == 0);
    CHECK(cw_filter_valid(cw_filter_all(value_mask(A))));
    CHECK(!cw_filter_valid(filter3(value_mask(A), no_mask(), value_mask(A))));
    CHECK(!cw_filter_valid(filter3(no_mask(), value_mask(A), mask2(A, B))));
    CHECK(cw_filter_valid(filter3(no_mask(), mask2(A, B), value_mask(A))));
    CHECK(cw_filter_equal(cw_filter_all(value_mask(A)), filter3(value_mask(A), no_mask(), no_mask())));
    CHECK(!cw_filter_equal(cw_filter_all(value_mask(A)), filter3(no_mask(), value_mask(A), no_mask())));
    cw_query legacy = cw_query_iter(&w, value_mask(A));
    cw_entity_id id;
    CHECK(cw_query_next(&legacy, &id) && id == 1);
    CHECK(!cw_query_next(NULL, &id) && id == CW_INVALID_ENTITY);
    CHECK(!cw_query_next(&legacy, NULL));
    cw_world_destroy(&w);
}

static void test_cached_query_ownership(void) {
    cw_world w;
    cw_world_init(&w, NULL);
    CHECK(CW_REGISTER(&w, 0, int64_t));
    CHECK(cw_query_create(NULL, cw_filter_all(value_mask(0))) == NULL);
    CHECK(cw_query_create(&w, filter3(value_mask(0), no_mask(), value_mask(0))) == NULL);
    cw_cached_query *a = cw_query_create(&w, cw_filter_all(value_mask(0)));
    cw_cached_query *b = cw_query_create(&w, cw_filter_all(value_mask(0)));
    cw_cached_query *c = cw_query_create(&w, cw_filter_all(no_mask()));
    CHECK(a && a == b && c && c != a);
    CHECK(cw_query_refs(a) == 2 && cw_query_refs(c) == 1 && w.queries.handle_count == 2);
    CHECK(w.queries.cache_count == 2);
    CHECK(cw_query_world(a) == &w && cw_filter_equal(cw_query_filter(a), cw_filter_all(value_mask(0))));
    cw_query_destroy(b);
    CHECK(cw_query_refs(a) == 1 && w.queries.handle_count == 2);
    cw_query_destroy(a);
    CHECK(w.queries.handle_count == 1 && w.queries.handles[0] == c && w.queries.cache_count == 1);
    cw_query_destroy(NULL);
    CHECK(cw_query_count(NULL) == 0 && cw_query_gather(NULL, NULL, 0) == 0);
    CHECK(!cw_query_contains(NULL, 0) && cw_query_world(NULL) == NULL && cw_query_refs(NULL) == 0);
    size_t words = 7;
    CHECK(cw_query_bits(NULL, &words) == NULL && words == 0);
    cw_query it = cw_query_begin(NULL);
    cw_entity_id id;
    CHECK(!cw_query_next(&it, &id));
    /* reset keeps queries but empties them; destroy releases the rest */
    cw_world_create_entity(&w);
    CHECK(cw_query_count(c) == 1);
    cw_world_reset(&w);
    CHECK(w.queries.handle_count == 1 && cw_query_count(c) == 0);
    cw_world_create_entity(&w);
    CHECK(cw_query_count(c) == 1);
    cw_world_destroy(&w);
    CHECK(w.queries.handle_count == 0 && w.queries.handles == NULL && w.queries.caches == NULL && w.visible == NULL);
}

static void test_cached_query_lifecycle(void) {
    cw_world w;
    cw_world_init(&w, NULL);
    enum { A = 0, B = 1, LATE = 190 };
    CHECK(CW_REGISTER(&w, A, int64_t) && CW_REGISTER(&w, B, int64_t));
    cw_cached_query *all = cw_query_create(&w, cw_filter_all(no_mask()));
    cw_cached_query *with_a = cw_query_create(&w, cw_filter_all(value_mask(A)));
    cw_cached_query *a_not_b = cw_query_create(&w, filter3(value_mask(A), no_mask(), value_mask(B)));
    cw_cached_query *a_or_b = cw_query_create(&w, filter3(no_mask(), mask2(A, B), no_mask()));
    cw_cached_query *late = cw_query_create(&w, cw_filter_all(value_mask(LATE)));
    cw_cached_query *not_late = cw_query_create(&w, filter3(no_mask(), no_mask(), value_mask(LATE)));
    cw_cached_query *queries[] = {all, with_a, a_not_b, a_or_b, late, not_late};
    const size_t nq = sizeof(queries) / sizeof(*queries);
#define CHECK_ALL() do { for (size_t qi = 0; qi < nq; ++qi) check_cached_matches_adhoc(queries[qi]); } while (0)
    CHECK_ALL();

    /* creation */
    cw_entity_id e0 = cw_world_create_entity(&w), e1 = cw_world_create_entity(&w);
    CHECK(cw_query_count(all) == 2 && cw_query_count(with_a) == 0 && cw_query_count(not_late) == 2);
    CHECK_ALL();
    /* component membership */
    set_value(&w, e0, A, 1);
    CHECK(cw_query_count(with_a) == 1 && cw_query_count(a_not_b) == 1 && cw_query_count(a_or_b) == 1);
    set_value(&w, e0, A, 2);   /* re-set is not a membership change */
    CHECK(cw_query_count(with_a) == 1);
    CHECK(cw_comp_add(&w, e0, B));
    CHECK(cw_query_count(a_not_b) == 0 && cw_query_count(a_or_b) == 1);
    CHECK(cw_comp_add(&w, e1, B));
    CHECK(cw_query_count(a_or_b) == 2 && cw_query_count(with_a) == 1);
    CHECK(cw_comp_remove(&w, e0, B));
    CHECK(cw_query_count(a_not_b) == 1 && cw_query_contains(a_not_b, e0));
    CHECK_ALL();
    /* deferred destruction, restoration of a pending entity, sweeping */
    cw_world_mark_destroy(&w, e0);
    CHECK(cw_query_count(all) == 1 && cw_query_count(with_a) == 0 && !cw_query_contains(all, e0));
    CHECK_ALL();
    CHECK(!cw_world_restore_entity(&w, e0));
    CHECK(cw_query_count(all) == 2 && cw_query_count(with_a) == 1 && cw_query_contains(a_not_b, e0));
    CHECK_ALL();
    cw_world_mark_destroy(&w, e0);
    cw_comp_remove(&w, e0, A);   /* membership change on an invisible entity */
    CHECK(cw_query_count(with_a) == 0);
    cw_world_sweep(&w);
    CHECK(cw_query_count(all) == 1 && cw_query_count(with_a) == 0 && cw_query_count(a_or_b) == 1);
    CHECK_ALL();
    /* restoration of a swept entity and rebuild */
    CHECK(cw_world_restore_entity(&w, e0));
    CHECK(cw_query_count(all) == 2 && cw_query_count(with_a) == 0);
    set_value(&w, e0, A, 5);
    cw_world_mark_destroy(&w, e0);
    CHECK(cw_world_rebuild_entity(&w, e0));
    CHECK(cw_query_count(all) == 2 && cw_query_count(with_a) == 0 && cw_query_count(a_or_b) == 1);
    set_value(&w, e1, A, 6);
    CHECK(cw_world_rebuild_entity(&w, e1));
    CHECK(cw_query_count(with_a) == 0 && cw_query_count(a_or_b) == 0 && cw_query_count(all) == 2);
    CHECK_ALL();
    /* late component registration */
    CHECK(cw_query_count(late) == 0 && cw_query_count(not_late) == 2);
    CHECK(CW_REGISTER(&w, LATE, int64_t));
    CHECK(cw_query_count(late) == 0 && cw_query_count(not_late) == 2);
    set_value(&w, e1, LATE, 1);
    CHECK(cw_query_count(late) == 1 && cw_query_count(not_late) == 1 && cw_query_contains(late, e1));
    CHECK_ALL();
    /* pool growth: push the world well past its initial capacity */
    size_t capacity = w.capacity;
    cw_world_reserve_entities(&w, 3000);
    CHECK(w.capacity > capacity && cw_query_bits(all, &capacity) && capacity == w.capacity / 64);
    CHECK(cw_query_count(all) == 2);
    for (int i = 0; i < 5000; ++i) {
        cw_entity_id id = cw_world_create_entity(&w);
        if (i % 3 == 0) set_value(&w, id, A, i);
        if (i % 5 == 0) set_value(&w, id, B, i);
    }
    CHECK(cw_query_count(all) == 5002 && cw_query_count(with_a) == 1667);
    CHECK(cw_query_count(a_or_b) == 1667 + 1000 - 334);
    CHECK_ALL();
    CHECK(cw_world_restore_entity(&w, 70000));
    CHECK(w.capacity > 70000 && cw_query_count(all) == 5003 && cw_query_contains(all, 70000));
    set_value(&w, 70000, A, 1);
    CHECK(cw_query_contains(with_a, 70000) && cw_query_contains(a_not_b, 70000));
    CHECK_ALL();
    /* recycled ids are born clean */
    w.recycle_ids = true;
    cw_world_mark_destroy(&w, 70000);
    cw_world_sweep(&w);
    CHECK(cw_world_create_entity(&w) == 70000);
    CHECK(cw_query_contains(all, 70000) && !cw_query_contains(with_a, 70000));
    /* reset empties everything and the caches keep working afterwards */
    cw_world_reset(&w);
    for (size_t qi = 0; qi < nq; ++qi) CHECK(cw_query_count(queries[qi]) == 0);
    CHECK_ALL();
    CHECK(cw_world_create_entity(&w) == 0);
    set_value(&w, 0, A, 1);
    CHECK(cw_query_count(all) == 1 && cw_query_count(with_a) == 1 && cw_query_count(a_or_b) == 1);
    CHECK_ALL();
    /* a query created late sees the current state immediately */
    cw_cached_query *fresh = cw_query_create(&w, filter3(value_mask(A), no_mask(), value_mask(B)));
    CHECK(fresh && cw_query_count(fresh) == 1 && cw_query_contains(fresh, 0));
    cw_query_destroy(fresh);
#undef CHECK_ALL
    for (size_t qi = 0; qi < nq; ++qi) cw_query_destroy(queries[qi]);
    CHECK(w.queries.handle_count == 0 && w.queries.cache_count == 0);
    cw_world_destroy(&w);
}

typedef struct { cw_cached_query *query; size_t visited; cw_entity_id last; } each_state;
static bool each_visit(cw_world *w, cw_entity_id id, void *user) {
    each_state *s = user;
    ++s->visited;
    s->last = id;
    if (id == 4) {
        cw_comp_remove(w, 6, 0);               /* ahead of the cursor: skipped */
        cw_world_mark_destroy(w, 8);           /* ahead of the cursor: skipped */
        CHECK(cw_comp_add(w, 1, 0));           /* behind the cursor: not revisited */
        CHECK(cw_comp_add(w, 10, 0));          /* ahead of the cursor: visited */
        cw_world_sweep(w);
        cw_entity_id created = cw_world_create_entity(w);
        CHECK(cw_comp_add(w, created, 0));     /* ahead: visited */
        CHECK(cw_world_rebuild_entity(w, 12)); /* loses the component: skipped */
        CHECK(cw_world_restore_entity(w, 8));  /* revived without components: skipped */
    }
    return id != 5000;
}
static void test_cached_query_mutation_during_iteration(void) {
    cw_world w;
    cw_world_init(&w, NULL);
    CHECK(CW_REGISTER(&w, 0, int64_t));
    for (int i = 0; i < 20; ++i) {
        cw_entity_id id = cw_world_create_entity(&w);
        if (i % 2 == 0) set_value(&w, id, 0, i);
    }
    cw_cached_query *q = cw_query_create(&w, cw_filter_all(value_mask(0)));
    CHECK(cw_query_count(q) == 10);
    each_state s = {q, 0, -1};
    size_t n = cw_query_each(q, each_visit, &s);
    /* 0 2 4 | 10 14 16 18 20 ; 6, 8 (destroyed), 12 (rebuilt) skipped, 1 not revisited */
    CHECK(n == 8 && s.visited == 8 && s.last == 20);
    CHECK(cw_query_count(q) == 9 && cw_query_contains(q, 1) && cw_query_contains(q, 20));
    CHECK(!cw_query_contains(q, 6) && !cw_query_contains(q, 8) && !cw_query_contains(q, 12));
    check_cached_matches_adhoc(q);
    /* early stop and live growth from inside the walk */
    cw_query it = cw_query_begin(q);
    cw_entity_id id, visited = 0;
    while (cw_query_next(&it, &id)) {
        ++visited;
        if (id == 0) {
            cw_entity_id far = cw_world_create_entity(&w);
            CHECK(far < 64);
            CHECK(cw_world_restore_entity(&w, 5000) && cw_comp_add(&w, 5000, 0));
            cw_cached_query *nested = cw_query_create(&w, cw_filter_all(value_mask(0)));
            CHECK(nested == q && cw_query_refs(q) == 2);
            cw_query_destroy(nested);
        }
        if (id == 5000) break;
    }
    CHECK(visited == 10 && id == 5000);
    /* the callback stops at 5000, so the entity created inside the walk (5001) is never reached */
    each_state stop = {q, 0, -1};
    CHECK(cw_query_each(q, each_visit, &stop) == 10 && stop.last == 5000);
    CHECK(cw_query_contains(q, 5001) && cw_query_count(q) == 11);
    check_cached_matches_adhoc(q);
    cw_query_destroy(q);
    cw_world_destroy(&w);
}

static void test_query_access_and_conflicts(void) {
    cw_world w;
    cw_world_init(&w, NULL);
    enum { POS = 0, VEL = 1, HEALTH = 2, PLAYER = 3, DEAD = 4 };
    for (cw_component_id ct = POS; ct <= DEAD; ++ct) CHECK(CW_REGISTER(&w, ct, int64_t));
    cw_filter mover = cw_filter_all(mask2(POS, VEL));
    /* default access: reads all | any, writes nothing */
    cw_cached_query *renderer = cw_query_create(&w, mover);
    cw_access a = cw_query_access(renderer);
    CHECK(cw_sig_has(a.reads, POS) && cw_sig_has(a.reads, VEL) && cw_sig_empty(a.writes));
    CHECK(cw_access_equal(a, cw_access_default(mover)));
    CHECK(cw_access_equal(cw_query_access(NULL), cw_access_make(no_mask(), no_mask())));
    cw_filter with_any = filter3(value_mask(POS), mask2(VEL, HEALTH), no_mask());
    a = cw_access_default(with_any);
    CHECK(cw_sig_has(a.reads, POS) && cw_sig_has(a.reads, VEL) && cw_sig_has(a.reads, HEALTH));
    /* explicit access: same filter shares the cache but is a distinct handle */
    cw_cached_query *physics = cw_query_create_access(&w, mover,
        cw_access_make(value_mask(VEL), value_mask(POS)));
    CHECK(physics && physics != renderer && cw_query_shares_cache(physics, renderer));
    CHECK(w.queries.handle_count == 2 && w.queries.cache_count == 1);
    CHECK(cw_query_bits(physics, NULL) == cw_query_bits(renderer, NULL));
    CHECK(cw_query_create_access(&w, mover, cw_access_make(value_mask(VEL), value_mask(POS))) == physics);
    CHECK(cw_query_refs(physics) == 2 && cw_query_refs(renderer) == 1);
    cw_query_destroy(physics);
    a = cw_query_access(physics);
    CHECK(cw_sig_has(a.reads, VEL) && !cw_sig_has(a.reads, POS) && cw_sig_has(a.writes, POS));
    CHECK(cw_sig_has(cw_access_components(a), POS) && cw_sig_has(cw_access_components(a), VEL));
    /* the shared cache is maintained for both handles */
    cw_entity_id e = cw_world_create_entity(&w);
    set_value(&w, e, POS, 1);
    set_value(&w, e, VEL, 1);
    CHECK(cw_query_count(physics) == 1 && cw_query_count(renderer) == 1);
    cw_query_destroy(renderer);
    CHECK(w.queries.handle_count == 1 && w.queries.cache_count == 1 && cw_query_count(physics) == 1);
    cw_world_mark_destroy(&w, e);
    CHECK(cw_query_count(physics) == 0);
    /* reads may not name an excluded component; writes may (adding it) */
    cw_filter alive = filter3(value_mask(HEALTH), no_mask(), value_mask(DEAD));
    CHECK(!cw_access_valid(alive, cw_access_read(value_mask(DEAD))));
    CHECK(cw_query_create_access(&w, alive, cw_access_read(value_mask(DEAD))) == NULL);
    cw_cached_query *reaper = cw_query_create_access(&w, alive,
        cw_access_make(value_mask(HEALTH), value_mask(DEAD)));
    CHECK(reaper && cw_sig_has(cw_query_access(reaper).writes, DEAD));
    /* access-level conflicts */
    cw_access read_pos = cw_access_read(value_mask(POS));
    cw_access write_pos = cw_access_write(value_mask(POS));
    cw_access read_vel = cw_access_read(value_mask(VEL));
    CHECK(cw_sig_empty(cw_access_conflicts(read_pos, read_pos)));
    CHECK(cw_sig_empty(cw_access_conflicts(read_pos, read_vel)));
    CHECK(cw_sig_has(cw_access_conflicts(read_pos, write_pos), POS));
    CHECK(cw_sig_has(cw_access_conflicts(write_pos, read_pos), POS));
    CHECK(cw_sig_has(cw_access_conflicts(write_pos, write_pos), POS));
    CHECK(cw_sig_empty(cw_access_conflicts(write_pos, read_vel)));
    cw_signature both = cw_access_conflicts(cw_access_make(value_mask(VEL), value_mask(POS)),
                                            cw_access_make(value_mask(POS), value_mask(VEL)));
    CHECK(cw_sig_has(both, POS) && cw_sig_has(both, VEL));
    /* query-level conflicts respect filter disjointness */
    cw_cached_query *player_move = cw_query_create_access(&w,
        filter3(mask2(POS, PLAYER), no_mask(), no_mask()), write_pos);
    cw_cached_query *npc_move = cw_query_create_access(&w,
        filter3(value_mask(POS), no_mask(), value_mask(PLAYER)), write_pos);
    cw_cached_query *any_move = cw_query_create_access(&w, cw_filter_all(value_mask(POS)), write_pos);
    cw_cached_query *any_read = cw_query_create_access(&w, cw_filter_all(value_mask(POS)), read_pos);
    CHECK(cw_filter_disjoint(cw_query_filter(player_move), cw_query_filter(npc_move)));
    CHECK(!cw_filter_disjoint(cw_query_filter(player_move), cw_query_filter(any_move)));
    CHECK(cw_filter_disjoint(filter3(no_mask(), mask2(POS, VEL), no_mask()),
                             filter3(no_mask(), no_mask(), mask2(POS, VEL))));
    CHECK(!cw_filter_disjoint(filter3(no_mask(), mask2(POS, VEL), no_mask()),
                              filter3(no_mask(), no_mask(), value_mask(POS))));
    CHECK(cw_sig_empty(cw_query_conflicts(player_move, npc_move)));
    CHECK(cw_sig_has(cw_query_conflicts(player_move, any_move), POS));
    CHECK(cw_sig_has(cw_query_conflicts(npc_move, any_read), POS));
    CHECK(cw_sig_has(cw_query_conflicts(any_move, any_move), POS));
    CHECK(cw_sig_empty(cw_query_conflicts(any_read, any_read)));
    CHECK(cw_sig_empty(cw_query_conflicts(any_read, reaper)));
    CHECK(cw_sig_empty(cw_query_conflicts(NULL, any_move)));
    CHECK(cw_query_shares_cache(any_move, any_read) && !cw_query_shares_cache(any_move, npc_move));
    cw_world other;
    cw_world_init(&other, NULL);
    CHECK(CW_REGISTER(&other, POS, int64_t));
    cw_cached_query *foreign = cw_query_create_access(&other, cw_filter_all(value_mask(POS)), write_pos);
    CHECK(cw_sig_empty(cw_query_conflicts(any_move, foreign)));
    cw_world_destroy(&other);
    /* releasing handles in any order keeps shared caches alive until the last user */
    cw_query_destroy(any_move);   /* caches: mover, alive, player, npc, pos */
    CHECK(w.queries.cache_count == 5 && cw_query_count(any_read) == 0);
    cw_world_create_entity(&w);
    set_value(&w, 1, POS, 1);
    CHECK(cw_query_count(any_read) == 1);
    cw_query_destroy(any_read);
    CHECK(w.queries.cache_count == 4);
    cw_world_destroy(&w);
    CHECK(w.queries.handle_count == 0 && w.queries.cache_count == 0);
}

static size_t observer_walk(cw_observer *o, cw_change kind, cw_entity_id *out, size_t capacity) {
    cw_query it = cw_observer_iter(o, kind);
    cw_entity_id id;
    size_t n = 0;
    while (cw_query_next(&it, &id)) { if (n < capacity) out[n] = id; ++n; }
    return n;
}
static void test_change_observers(void) {
    cw_world w;
    cw_world_init(&w, NULL);
    enum { VALUE = 0, OTHER = 1, LATE = 150 };
    CHECK(CW_REGISTER(&w, VALUE, int64_t) && CW_REGISTER(&w, OTHER, int64_t));
    CHECK(cw_observer_create(NULL, VALUE) == NULL && cw_observer_create(&w, CW_INVALID_COMPONENT) == NULL);
    cw_observer *a = cw_observer_create(&w, VALUE);
    cw_observer *b = cw_observer_create(&w, VALUE);     /* independent consumer */
    cw_observer *other = cw_observer_create(&w, OTHER);
    cw_observer *late = cw_observer_create(&w, LATE);   /* component registered later */
    CHECK(a && b && other && late && w.observers.count == 4);
    CHECK(cw_observer_world(a) == &w && cw_observer_component(a) == VALUE);
    CHECK(cw_observer_component(NULL) == CW_INVALID_COMPONENT && cw_observer_world(NULL) == NULL);
    cw_entity_id ids[8];

    /* additions count as modifications; nothing is observed until observe() */
    cw_entity_id e0 = cw_world_create_entity(&w), e1 = cw_world_create_entity(&w);
    set_value(&w, e0, VALUE, 1);
    CHECK(cw_comp_add(&w, e1, VALUE));
    CHECK(cw_observer_pending(a, CW_CHANGE_ADDED) == 2 && cw_observer_pending(a, CW_CHANGE_MODIFIED) == 2);
    CHECK(cw_observer_pending(a, CW_CHANGE_REMOVED) == 0 && cw_observer_count(a, CW_CHANGE_ADDED) == 0);
    CHECK(cw_observer_pending(other, CW_CHANGE_ADDED) == 0 && cw_observer_pending(late, CW_CHANGE_ADDED) == 0);
    CHECK(observer_walk(a, CW_CHANGE_ADDED, ids, 8) == 0);
    cw_observer_observe(a);
    CHECK(cw_observer_observations(a) == 1 && cw_observer_observations(b) == 0);
    CHECK(cw_observer_count(a, CW_CHANGE_ADDED) == 2 && cw_observer_pending(a, CW_CHANGE_ADDED) == 0);
    CHECK(cw_observer_has(a, CW_CHANGE_ADDED, e0) && cw_observer_has(a, CW_CHANGE_MODIFIED, e1));
    CHECK(!cw_observer_has(a, CW_CHANGE_REMOVED, e0) && !cw_observer_has(a, CW_CHANGE_ADDED, -1));
    CHECK(observer_walk(a, CW_CHANGE_ADDED, ids, 8) == 2 && ids[0] == e0 && ids[1] == e1);
    CHECK(cw_observer_gather(a, CW_CHANGE_MODIFIED, ids, 8) == 2 && ids[1] == e1);
    /* b has its own cursor: a's observation did not consume b's changes */
    CHECK(cw_observer_pending(b, CW_CHANGE_ADDED) == 2 && cw_observer_count(b, CW_CHANGE_ADDED) == 0);

    /* modifications: set on an existing component, write, touch; get is invisible */
    set_value(&w, e0, VALUE, 2);
    CHECK(cw_observer_pending(a, CW_CHANGE_ADDED) == 0 && cw_observer_pending(a, CW_CHANGE_MODIFIED) == 1);
    set_value(&w, e0, VALUE, 3);    /* same entity twice is one entry */
    CHECK(cw_observer_pending(a, CW_CHANGE_MODIFIED) == 1);
    *CW_GET(&w, e1, VALUE, int64_t) = 50;
    CHECK(cw_observer_pending(a, CW_CHANGE_MODIFIED) == 1);
    CHECK(CW_WRITE(&w, e1, VALUE, int64_t) != NULL && *CW_GET(&w, e1, VALUE, int64_t) == 50);
    CHECK(cw_observer_pending(a, CW_CHANGE_MODIFIED) == 2);
    CHECK(CW_WRITE(&w, e1, OTHER, int64_t) != NULL);       /* pointer to an absent component: no event */
    CHECK(cw_observer_pending(other, CW_CHANGE_MODIFIED) == 0);
    CHECK(!cw_comp_touch(&w, e1, OTHER) && !cw_comp_touch(&w, 99, VALUE) && !cw_comp_touch(&w, e1, LATE));
    CHECK(cw_comp_add(&w, e1, OTHER) && cw_comp_touch(&w, e1, OTHER));
    CHECK(cw_observer_pending(other, CW_CHANGE_ADDED) == 1 && cw_observer_pending(other, CW_CHANGE_MODIFIED) == 1);
    cw_observer_observe(a);
    CHECK(cw_observer_count(a, CW_CHANGE_ADDED) == 0 && cw_observer_count(a, CW_CHANGE_MODIFIED) == 2);
    CHECK(observer_walk(a, CW_CHANGE_MODIFIED, ids, 8) == 2);
    /* the observed set is stable while new changes accumulate */
    set_value(&w, e0, VALUE, 4);
    CHECK(cw_observer_count(a, CW_CHANGE_MODIFIED) == 2 && cw_observer_pending(a, CW_CHANGE_MODIFIED) == 1);

    /* removal, deferred destruction and sweep */
    CHECK(cw_comp_remove(&w, e1, VALUE));
    CHECK(cw_observer_pending(a, CW_CHANGE_REMOVED) == 1);
    cw_world_mark_destroy(&w, e0);
    CHECK(cw_observer_pending(a, CW_CHANGE_REMOVED) == 1);
    cw_observer_observe(a);
    /* e0 is pending destruction: modified raw set has it, the walk skips it */
    CHECK(cw_observer_count(a, CW_CHANGE_MODIFIED) == 1 && cw_observer_has(a, CW_CHANGE_MODIFIED, e0));
    CHECK(observer_walk(a, CW_CHANGE_MODIFIED, ids, 8) == 0);
    CHECK(observer_walk(a, CW_CHANGE_REMOVED, ids, 8) == 1 && ids[0] == e1);
    cw_world_sweep(&w);
    CHECK(cw_observer_pending(a, CW_CHANGE_REMOVED) == 1 && cw_observer_pending(other, CW_CHANGE_REMOVED) == 0);
    cw_observer_observe(a);
    CHECK(observer_walk(a, CW_CHANGE_REMOVED, ids, 8) == 1 && ids[0] == e0 && !cw_world_is_valid(&w, e0));
    /* b still holds everything since the beginning */
    CHECK(cw_observer_pending(b, CW_CHANGE_ADDED) == 2 && cw_observer_pending(b, CW_CHANGE_MODIFIED) == 2);
    CHECK(cw_observer_pending(b, CW_CHANGE_REMOVED) == 2);
    cw_observer_observe(b);
    /* raw sets keep both events; walks only yield what currently exists */
    CHECK(cw_observer_has(b, CW_CHANGE_ADDED, e0) && cw_observer_has(b, CW_CHANGE_REMOVED, e0));
    CHECK(observer_walk(b, CW_CHANGE_ADDED, ids, 8) == 0 && observer_walk(b, CW_CHANGE_REMOVED, ids, 8) == 2);

    /* rebuild removes, restore does not add, recycled ids are fresh events */
    w.recycle_ids = true;
    set_value(&w, e1, VALUE, 5);
    cw_observer_observe(a);
    CHECK(cw_world_rebuild_entity(&w, e1));
    CHECK(cw_observer_pending(a, CW_CHANGE_REMOVED) == 1 && cw_observer_pending(other, CW_CHANGE_REMOVED) == 1);
    cw_world_mark_destroy(&w, e1);
    CHECK(!cw_world_restore_entity(&w, e1) && cw_observer_pending(a, CW_CHANGE_ADDED) == 0);
    set_value(&w, e1, VALUE, 6);
    cw_world_mark_destroy(&w, e1);
    cw_world_sweep(&w);
    CHECK(cw_world_create_entity(&w) == e1);
    set_value(&w, e1, VALUE, 7);
    cw_observer_observe(a);
    CHECK(cw_observer_has(a, CW_CHANGE_ADDED, e1) && cw_observer_has(a, CW_CHANGE_REMOVED, e1));
    CHECK(observer_walk(a, CW_CHANGE_ADDED, ids, 8) == 1 && ids[0] == e1);

    /* late registration, unmarshal, and growth past the initial capacity */
    CHECK(CW_REGISTER(&w, LATE, int64_t));
    cw_codec codec = {marshal_value, unmarshal_value};
    CHECK(cw_comp_set_codec(&w, LATE, codec));
    CHECK(cw_comp_unmarshal(&w, e1, LATE, "v=42", 4));
    CHECK(cw_comp_unmarshal(&w, e1, LATE, "v=42", 4));
    CHECK(cw_observer_pending(late, CW_CHANGE_ADDED) == 1 && cw_observer_pending(late, CW_CHANGE_MODIFIED) == 1);
    CHECK(cw_world_restore_entity(&w, 5000));
    set_value(&w, 5000, LATE, 1);
    cw_observer_observe(late);
    size_t words = 0;
    CHECK(cw_observer_bits(late, CW_CHANGE_ADDED, &words) && words == w.capacity / 64);
    CHECK(cw_observer_count(late, CW_CHANGE_ADDED) == 2 && cw_observer_has(late, CW_CHANGE_ADDED, 5000));
    CHECK(cw_observer_bits(NULL, CW_CHANGE_ADDED, &words) == NULL && words == 0);
    CHECK(cw_observer_count(late, CW_CHANGE_KINDS) == 0 && !cw_observer_has(late, CW_CHANGE_KINDS, 5000));
    cw_query bad = cw_observer_iter(late, CW_CHANGE_KINDS);
    cw_entity_id id;
    CHECK(!cw_query_next(&bad, &id));

    /* reset shows up as removals; observers survive and memory is accounted */
    cw_observer_observe(a);
    cw_world_reset(&w);
    CHECK(w.observers.count == 4 && cw_observer_pending(a, CW_CHANGE_REMOVED) == 1);
    CHECK(cw_observer_pending(late, CW_CHANGE_REMOVED) == 2);
    cw_memory_stats m = cw_world_memory(&w);
    CHECK(m.observers >= 4 * 6 * (w.capacity / 64) * sizeof(uint64_t) && m.total >= m.observers);
    cw_observer_destroy(b);
    CHECK(w.observers.count == 3 && cw_world_memory(&w).observers < m.observers);
    cw_observer_destroy(NULL);
    cw_world_create_entity(&w);
    set_value(&w, 0, VALUE, 1);
    CHECK(cw_observer_pending(a, CW_CHANGE_ADDED) == 1);
    cw_observer_destroy(a);
    cw_world_destroy(&w);
    CHECK(w.observers.count == 0 && w.observers.items == NULL);
}

static void test_query_helpers_and_checked_access(void) {
    cw_world w;
    cw_world_init(&w, NULL);
    enum { POS = 0, VEL = 1, TAG = 2 };
    CHECK(CW_REGISTER(&w, POS, int64_t) && CW_REGISTER(&w, VEL, int64_t) && CW_REGISTER(&w, TAG, int64_t));
    cw_filter mover = filter3(value_mask(POS), value_mask(VEL), value_mask(TAG));
    cw_cached_query *q = cw_query_create_access(&w, mover, cw_access_make(value_mask(VEL), value_mask(POS)));
    cw_observer *o = cw_observer_create(&w, POS);
    cw_entity_id id = 99;

    /* empty: O(1) emptiness, first/single report nothing */
    CHECK(cw_query_is_empty(q) && cw_query_is_empty(NULL));
    CHECK(!cw_query_first(q, &id) && id == CW_INVALID_ENTITY);
    CHECK(cw_query_single(q, &id) == CW_SINGLE_NONE && id == CW_INVALID_ENTITY);
    CHECK(cw_query_single(NULL, NULL) == CW_SINGLE_NONE && !cw_query_first(q, NULL));
    CHECK(!cw_query_first_filter(&w, mover, &id) && id == CW_INVALID_ENTITY);
    CHECK(cw_query_single_filter(&w, mover, &id) == CW_SINGLE_NONE && cw_query_count_filter(&w, mover, 10) == 0);

    /* one match */
    cw_entity_id a = cw_world_create_entity(&w);
    set_value(&w, a, POS, 1);
    set_value(&w, a, VEL, 2);
    CHECK(!cw_query_is_empty(q) && cw_query_first(q, &id) && id == a);
    CHECK(cw_query_single(q, &id) == CW_SINGLE_ONE && id == a);
    CHECK(cw_query_first_filter(&w, mover, &id) && id == a);
    CHECK(cw_query_single_filter(&w, mover, &id) == CW_SINGLE_ONE && id == a);
    CHECK(cw_query_single_filter(&w, mover, NULL) == CW_SINGLE_ONE);

    /* many: single reports the first match, counting stops at the limit */
    cw_world_reserve_entities(&w, 1000);
    for (int i = 0; i < 1000; ++i) {
        cw_entity_id e = cw_world_create_entity(&w);
        set_value(&w, e, POS, i);
        set_value(&w, e, VEL, i);
        if (i % 10 == 0) set_value(&w, e, TAG, i);
    }
    CHECK(cw_query_count(q) == 901 && cw_query_single(q, &id) == CW_SINGLE_MANY && id == a);
    CHECK(cw_query_single_filter(&w, mover, &id) == CW_SINGLE_MANY && id == a);
    CHECK(cw_query_count_filter(&w, mover, 5) == 5 && cw_query_count_filter(&w, mover, 0) == 0);
    CHECK(cw_query_count_filter(&w, mover, SIZE_MAX) == 901);
    cw_world_mark_destroy(&w, a);
    CHECK(cw_query_first(q, &id) && id == 2 && cw_query_single(q, NULL) == CW_SINGLE_MANY);

    /* matches() on every iterator flavour */
    cw_query cached = cw_query_begin(q), adhoc = cw_query_iter_filter(&w, mover);
    CHECK(cw_query_matches(&cached, 2) && cw_query_matches(&adhoc, 2));
    CHECK(!cw_query_matches(&cached, a) && !cw_query_matches(&adhoc, a));       /* pending destroy */
    CHECK(!cw_query_matches(&cached, 11) && !cw_query_matches(&adhoc, 11));     /* tagged */
    CHECK(!cw_query_matches(&cached, -1) && !cw_query_matches(&adhoc, 1 << 20) && !cw_query_matches(NULL, 2));
    cw_observer_observe(o);
    cw_query added = cw_observer_iter(o, CW_CHANGE_ADDED), removed = cw_observer_iter(o, CW_CHANGE_REMOVED);
    CHECK(cw_query_matches(&added, 2) && !cw_query_matches(&added, a) && !cw_query_matches(&removed, 2));
    CHECK(cw_comp_remove(&w, 3, POS));
    cw_observer_observe(o);
    removed = cw_observer_iter(o, CW_CHANGE_REMOVED);
    added = cw_observer_iter(o, CW_CHANGE_ADDED);
    CHECK(cw_query_matches(&removed, 3) && !cw_query_matches(&added, 3));

    /* checked access: entity must be in the query, carry the component, and the component be declared */
    CHECK(CW_QUERY_GET(q, 2, VEL, int64_t) && *CW_QUERY_GET(q, 2, VEL, int64_t) == 1);
    CHECK(CW_QUERY_GET(q, 2, POS, int64_t) != NULL);            /* writes imply read access */
    CHECK(CW_QUERY_GET(q, 2, TAG, int64_t) == NULL);            /* undeclared (and excluded) */
    CHECK(CW_QUERY_GET(q, a, VEL, int64_t) == NULL);            /* not in the query */
    CHECK(CW_QUERY_GET(q, 11, VEL, int64_t) == NULL);           /* not in the query */
    CHECK(CW_QUERY_GET(q, 2, VEL, int32_t) == NULL);            /* wrong size */
    CHECK(CW_QUERY_GET(NULL, 2, VEL, int64_t) == NULL);
    CHECK(CW_QUERY_WRITE(q, 2, VEL, int64_t) == NULL);          /* declared read only */
    CHECK(CW_QUERY_WRITE(q, a, POS, int64_t) == NULL);
    CHECK(cw_observer_pending(o, CW_CHANGE_MODIFIED) == 0);
    int64_t *pos = CW_QUERY_WRITE(q, 2, POS, int64_t);
    CHECK(pos && pos == CW_GET(&w, 2, POS, int64_t));
    *pos = 77;
    CHECK(cw_observer_pending(o, CW_CHANGE_MODIFIED) == 1 && *CW_GET(&w, 2, POS, int64_t) == 77);
    /* an `any` component that this entity lacks yields NULL rather than a dangling slot */
    cw_cached_query *either = cw_query_create_access(&w, filter3(no_mask(), mask2(POS, VEL), no_mask()),
                                                     cw_access_read(mask2(POS, VEL)));
    cw_entity_id only_vel = cw_world_create_entity(&w);
    set_value(&w, only_vel, VEL, 5);
    CHECK(cw_query_contains(either, only_vel) && CW_QUERY_GET(either, only_vel, VEL, int64_t));
    CHECK(CW_QUERY_GET(either, only_vel, POS, int64_t) == NULL && CW_GET(&w, only_vel, POS, int64_t) != NULL);

    /* observers take part in conflict detection as readers */
    cw_access oa = cw_observer_access(o);
    CHECK(cw_sig_has(oa.reads, POS) && cw_sig_empty(oa.writes) && cw_sig_empty(cw_observer_access(NULL).reads));
    CHECK(cw_sig_has(cw_observer_conflicts(o, q), POS));         /* q writes POS */
    CHECK(cw_sig_empty(cw_observer_conflicts(o, either)));       /* either only reads */
    cw_cached_query *tagger = cw_query_create_access(&w, filter3(no_mask(), no_mask(), value_mask(POS)),
                                                     cw_access_write(value_mask(POS)));
    CHECK(cw_sig_has(cw_observer_conflicts(o, tagger), POS));    /* adding POS anywhere is an event */
    CHECK(cw_sig_empty(cw_observer_conflicts(NULL, q)) && cw_sig_empty(cw_observer_conflicts(o, NULL)));
    cw_access combined = cw_access_union(cw_query_access(q), cw_observer_access(o));
    CHECK(cw_sig_has(combined.reads, VEL) && cw_sig_has(combined.reads, POS) && cw_sig_has(combined.writes, POS));
    CHECK(cw_sig_has(cw_access_conflicts(combined, cw_query_access(tagger)), POS));
    cw_world other;
    cw_world_init(&other, NULL);
    cw_observer *foreign = cw_observer_create(&other, POS);
    CHECK(cw_sig_empty(cw_observer_conflicts(foreign, q)));
    cw_world_destroy(&other);
    cw_query_destroy(tagger);
    cw_query_destroy(either);
    cw_query_destroy(q);
    cw_observer_destroy(o);
    cw_world_destroy(&w);
}

typedef struct { int64_t hp; } Health;
static void apply_damage(cw_world *w, cw_entity_id id, cw_component_id ct, void *component,
                         const void *data, size_t size) {
    (void)w; (void)id; (void)ct;
    CHECK(size == sizeof(int64_t) && ((uintptr_t)data % 16) == 0);
    ((Health *)component)->hp -= *(const int64_t *)data;
}
static unsigned calls;
static void count_call(cw_world *w, const void *data, size_t size) {
    (void)w;
    CHECK(size == 3 && memcmp(data, "abc", 3) == 0);
    ++calls;
}
static void record_while_flushing(cw_world *w, const void *data, size_t size) {
    (void)data; (void)size;
    cw_commands *c = w->user;
    CHECK(!cw_commands_add(c, 0, 0) && cw_commands_create(c) == CW_INVALID_ENTITY);
    CHECK(cw_commands_flush(c, w).applied == 0);
    cw_commands_clear(c);   /* ignored while flushing */
    CHECK(c->count > 0);
    ++calls;
}
static void test_command_buffer(void) {
    cw_world w;
    cw_world_init(&w, NULL);
    enum { HEALTH = 0, TAG = 1, LATE = 2 };
    CHECK(cw_comp_register(&w, HEALTH, sizeof(Health), _Alignof(Health), "Health", NULL));
    CHECK(cw_comp_register(&w, TAG, 0, 1, "Tag", NULL));
    cw_commands c;
    cw_commands_init(&c);
    CHECK(cw_commands_is_empty(&c) && cw_commands_count(&c) == 0 && cw_commands_is_empty(NULL));
    CHECK(cw_commands_flush(&c, &w).applied == 0 && cw_commands_flush(NULL, &w).applied == 0);
    CHECK(cw_commands_created(&c).count == 0 && cw_commands_created(NULL).data == NULL);
    cw_cached_query *hurt = cw_query_create(&w, cw_filter_all(value_mask(HEALTH)));
    cw_observer *o = cw_observer_create(&w, HEALTH);

    /* record during iteration: nothing changes until the flush, then FIFO order applies */
    for (int i = 0; i < 5; ++i) {
        Health h = {100};
        cw_entity_id id = cw_world_create_entity(&w);
        CHECK(CW_COMMANDS_SET(&c, id, HEALTH, &h));   /* deferred: not yet in the query */
    }
    CHECK(cw_query_count(hurt) == 0 && cw_commands_count(&c) == 5);
    cw_flush_result r = cw_commands_flush(&c, &w);
    CHECK(r.applied == 5 && r.failed == 0 && cw_query_count(hurt) == 5 && cw_commands_is_empty(&c));
    cw_observer_observe(o);
    CHECK(cw_observer_count(o, CW_CHANGE_ADDED) == 5);
    int64_t damage = 30;
    cw_query it = cw_query_begin(hurt);
    cw_entity_id id;
    while (cw_query_next(&it, &id)) {
        CHECK(cw_commands_update(&c, id, HEALTH, apply_damage, &damage, sizeof(damage)));
        if (id == 1) CHECK(cw_commands_update(&c, id, HEALTH, apply_damage, &damage, sizeof(damage)));
        if (id == 2) CHECK(cw_commands_remove(&c, id, HEALTH));
        if (id == 3) CHECK(cw_commands_destroy_entity(&c, id));
        if (id == 4) { Health h = {1}; CHECK(CW_COMMANDS_SET(&c, id, HEALTH, &h)); }
        CHECK(cw_commands_add(&c, id, TAG));
    }
    CHECK(CW_GET(&w, 0, HEALTH, Health)->hp == 100 && cw_query_count(hurt) == 5);
    r = cw_commands_flush(&c, &w);
    CHECK(r.applied == 14 && r.failed == 0);
    CHECK(CW_GET(&w, 0, HEALTH, Health)->hp == 70 && CW_GET(&w, 1, HEALTH, Health)->hp == 40);
    CHECK(!cw_comp_has(&w, 2, HEALTH) && cw_comp_has(&w, 2, TAG));
    CHECK(cw_world_is_valid(&w, 3) && (w.entities[3].flags & CW_TO_DESTROY) && !cw_query_contains(hurt, 3));
    CHECK(CW_GET(&w, 4, HEALTH, Health)->hp == 1);   /* update then set: order preserved */
    cw_observer_observe(o);
    CHECK(cw_observer_count(o, CW_CHANGE_MODIFIED) == 5 && cw_observer_count(o, CW_CHANGE_REMOVED) == 1);

    /* pending entities: create and populate in one buffer, then read back the real ids */
    cw_entity_id p0 = cw_commands_create(&c), p1 = cw_commands_create(&c);
    CHECK(cw_entity_is_pending(p0) && cw_entity_is_pending(p1) && p0 != p1 && !cw_entity_is_pending(CW_INVALID_ENTITY));
    CHECK(!cw_entity_is_pending(0) && !cw_commands_add(&c, p1 - 1, TAG));   /* not created by this buffer */
    Health h = {50};
    CHECK(CW_COMMANDS_SET(&c, p0, HEALTH, &h) && cw_commands_add(&c, p1, TAG));
    CHECK(cw_commands_update(&c, p0, HEALTH, apply_damage, &damage, sizeof(damage)));
    CHECK(cw_commands_destroy_entity(&c, p1));
    r = cw_commands_flush(&c, &w);
    CHECK(r.applied == 6 && r.failed == 0);
    cw_id_view created = cw_commands_created(&c);
    CHECK(created.count == 2 && created.data[0] == 5 && created.data[1] == 6);
    CHECK(CW_GET(&w, 5, HEALTH, Health)->hp == 20 && cw_comp_has(&w, 6, TAG));
    CHECK(w.entities[6].flags & CW_TO_DESTROY);
    CHECK(cw_commands_created(&c).count == 2);   /* kept until the next flush or clear */
    cw_commands_clear(&c);
    CHECK(cw_commands_created(&c).count == 0);

    /* failures are counted, not fatal: stale entity, unregistered component, size mismatch */
    cw_world_sweep(&w);
    CHECK(cw_commands_update(&c, 3, HEALTH, apply_damage, &damage, sizeof(damage)));
    CHECK(cw_commands_destroy_entity(&c, 3) && cw_commands_remove(&c, 0, LATE));
    CHECK(cw_commands_set(&c, 0, HEALTH, &damage, sizeof(int32_t)) && cw_commands_add(&c, 0, LATE));
    CHECK(cw_commands_update(&c, 2, HEALTH, apply_damage, &damage, sizeof(damage)));  /* removed earlier */
    CHECK(cw_commands_set(&c, 0, TAG, NULL, 0) && cw_commands_call(&c, count_call, "abc", 3));
    r = cw_commands_flush(&c, &w);
    CHECK(r.applied == 2 && r.failed == 6 && calls == 1);
    CHECK(!cw_commands_set(&c, 0, HEALTH, NULL, 4) && !cw_commands_update(&c, 0, HEALTH, NULL, NULL, 0));
    CHECK(!cw_commands_call(&c, NULL, NULL, 0) && !cw_commands_add(&c, -1, TAG));
    CHECK(!cw_commands_add(&c, 0, CW_INVALID_COMPONENT) && !cw_commands_add(NULL, 0, TAG));
    CHECK(cw_commands_is_empty(&c));

    /* reserve makes recording allocation-free; the stream stays 16-byte aligned */
    cw_commands_reserve(&c, 128 * 1024);
    size_t capacity = c.capacity;
    unsigned char *data = c.data;
    CHECK(capacity >= 128 * 1024 && ((uintptr_t)data % 16) == 0);
    for (int i = 0; i < 500; ++i) {
        Health value = {i};
        CHECK(CW_COMMANDS_SET(&c, 0, HEALTH, &value) && cw_commands_update(&c, 0, HEALTH, apply_damage, &damage, 8));
        CHECK(cw_commands_call(&c, count_call, "abc", 3));
    }
    CHECK(c.capacity == capacity && c.data == data && cw_commands_count(&c) == 1500);
    cw_commands_reserve(&c, 10);   /* never shrinks */
    CHECK(c.capacity == capacity);
    r = cw_commands_flush(&c, &w);
    CHECK(r.applied == 1500 && calls == 501 && CW_GET(&w, 0, HEALTH, Health)->hp == 499 - 30);
    /* callbacks may not record into the buffer being flushed */
    w.user = &c;
    CHECK(cw_commands_call(&c, record_while_flushing, NULL, 0));
    CHECK(cw_commands_flush(&c, &w).applied == 1 && calls == 502 && cw_commands_is_empty(&c));
    cw_commands_destroy(&c);
    cw_commands_destroy(&c);
    cw_commands_destroy(NULL);
    cw_query_destroy(hurt);
    cw_observer_destroy(o);
    cw_world_destroy(&w);
}

static void deferred_damage_system(cw_world *w, cw_phase phase, float dt) {
    (void)dt;
    trace *t = w->user;
    int64_t damage = 10;
    if (phase == CW_EARLY_UPDATE) {
        /* record against every entity with health; nothing applies until the phase ends */
        cw_query it = cw_query_iter(w, value_mask(0));
        cw_entity_id id;
        while (cw_query_next(&it, &id))
            CHECK(cw_commands_update(cw_world_commands(w), id, 0, apply_damage, &damage, sizeof(damage)));
        CHECK(CW_GET(w, t->victim, 0, Health)->hp == 100);
        cw_entity_id spawned = cw_commands_create(cw_world_commands(w));
        Health h = {5};
        CHECK(CW_COMMANDS_SET(cw_world_commands(w), spawned, 0, &h));
        record(w, 1);
    } else if (phase == CW_UPDATE) {
        CHECK(CW_GET(w, t->victim, 0, Health)->hp == 90);   /* flushed between phases */
        CHECK(cw_commands_is_empty(cw_world_commands(w)));
        cw_id_view created = cw_commands_created(cw_world_commands(w));
        if (t->count == 1) {   /* a full tick: EARLY_UPDATE spawned one entity */
            CHECK(created.count == 1 && CW_GET(w, created.data[0], 0, Health)->hp == 5);
            CHECK(cw_commands_destroy_entity(cw_world_commands(w), created.data[0]));
        } else {               /* replay runs CW_UPDATE alone: nothing was created */
            CHECK(created.count == 0);
            CHECK(cw_commands_destroy_entity(cw_world_commands(w), 1));   /* swept id: fails quietly */
        }
        record(w, 2);
    } else if (phase == CW_ENTITY_DESTROYED) {
        CHECK(cw_world_pending_destroy(w).count == 1);
        record(w, 3);
    }
}
static void test_command_buffer_in_sim(void) {
    cw_world w;
    cw_sim s;
    trace t = {0};
    cw_world_init(&w, &t);
    cw_sim_init(&s, CW_HOST);
    CHECK(cw_comp_register(&w, 0, sizeof(Health), _Alignof(Health), "Health", NULL));
    t.victim = cw_world_create_entity(&w);
    Health h = {100};
    CHECK(cw_comp_set(&w, t.victim, 0, &h, sizeof(h)));
    cw_system_desc desc = system_desc("damage", deferred_damage_system, 0, CW_SHARED);
    desc.phases = CW_PHASE(CW_EARLY_UPDATE) | CW_PHASE(CW_UPDATE) | CW_PHASE(CW_ENTITY_DESTROYED);
    CHECK(cw_sim_register(&s, desc));
    CHECK(cw_world_commands(&w) == &w.commands && cw_world_commands(NULL) == NULL);
    CHECK(cw_commands_last_flush(&w.commands).applied == 0 && cw_commands_last_flush(NULL).failed == 0);
    cw_sim_tick(&s, &w, 0);
    expect_trace(&t, (int[]){1, 2, 3}, 3);
    /* the last non-empty flush was the UPDATE phase's destroy; later empty flushes kept it */
    CHECK(cw_commands_last_flush(&w.commands).applied == 1 && cw_commands_last_flush(&w.commands).failed == 0);
    CHECK(cw_world_entity_count(&w) == 1 && CW_GET(&w, t.victim, 0, Health)->hp == 90);
    cw_sim_replay(&s, &w, 0);   /* replay flushes too */
    expect_trace(&t, (int[]){2}, 1);
    CHECK(cw_commands_last_flush(&w.commands).applied == 0 && cw_commands_last_flush(&w.commands).failed == 1);
    CHECK(cw_world_pending_destroy(&w).count == 0);   /* the destroy of a swept id failed quietly */
    /* reset discards what is queued; destroy releases the buffer */
    CHECK(cw_commands_add(cw_world_commands(&w), t.victim, 0));
    cw_memory_stats m = cw_world_memory(&w);
    CHECK(m.commands > 0 && m.total >= m.commands);
    cw_world_reset(&w);
    CHECK(cw_commands_is_empty(cw_world_commands(&w)) && cw_world_memory(&w).commands == m.commands);
    cw_sim_destroy(&s);
    cw_world_destroy(&w);
    CHECK(w.commands.data == NULL && w.commands.capacity == 0);
}

static uint32_t rng_state = UINT32_C(0x1a2b3c4d);
static uint32_t next_random(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static void test_randomized_reference_model(void) {
    enum { N = 600, STEPS = 12000, TAG = 5, QUERIES = 5 };
    cw_world w;
    bool alive[N] = {0}, pending[N] = {0}, present[N] = {0}, tagged[N] = {0};
    bool ev_added[N] = {0}, ev_modified[N] = {0}, ev_removed[N] = {0};
    int64_t values[N] = {0};
    cw_world_init(&w, NULL);
    CHECK(CW_REGISTER(&w, 191, int64_t));
    /* value | tag | value & !tag | value or tag | !value & !tag */
    cw_cached_query *queries[QUERIES] = {
        cw_query_create(&w, cw_filter_all(value_mask(191))),
        cw_query_create(&w, cw_filter_all(value_mask(TAG))),
        cw_query_create(&w, filter3(value_mask(191), no_mask(), value_mask(TAG))),
        cw_query_create(&w, filter3(no_mask(), mask2(191, TAG), no_mask())),
        cw_query_create(&w, filter3(no_mask(), no_mask(), mask2(191, TAG))),
    };
    cw_observer *observer = cw_observer_create(&w, 191), *idle = cw_observer_create(&w, 191);
    CHECK(observer && idle);
    for (int step = 0; step < STEPS; ++step) {
        unsigned id = next_random() % N;
        if (step == STEPS / 3) CHECK(cw_comp_register(&w, TAG, 0, 1, "Tag", NULL));
        if (step == STEPS / 2) {
            cw_query_destroy(queries[0]);
            queries[0] = cw_query_create(&w, cw_filter_all(value_mask(191)));
        }
        switch (next_random() % 10) {
            case 0:
                CHECK(cw_world_restore_entity(&w, id) == !alive[id]);
                alive[id] = true; pending[id] = false;
                break;
            case 1:
                if (alive[id]) {
                    values[id] = step;
                    if (step & 1) set_value(&w, id, 191, values[id]);
                    else {
                        CHECK(cw_comp_add(&w, id, 191));
                        *CW_WRITE(&w, id, 191, int64_t) = values[id];
                    }
                    if (!present[id]) ev_added[id] = true;
                    ev_modified[id] = true;
                    present[id] = true;
                }
                break;
            case 2:
                CHECK(cw_comp_remove(&w, id, 191) == (alive[id] && present[id]));
                if (alive[id] && present[id]) ev_removed[id] = true;
                present[id] = false;
                break;
            case 3:
                cw_world_mark_destroy(&w, id);
                if (alive[id]) pending[id] = true;
                break;
            case 4:
                cw_world_sweep(&w);
                for (unsigned i = 0; i < N; ++i) {
                    if (!pending[i]) continue;
                    if (present[i]) ev_removed[i] = true;
                    alive[i] = pending[i] = present[i] = tagged[i] = false;
                }
                break;
            case 5:
                CHECK(cw_world_rebuild_entity(&w, id) == alive[id]);
                if (alive[id] && present[id]) ev_removed[id] = true;
                if (alive[id]) present[id] = pending[id] = tagged[id] = false;
                break;
            case 8:
                if (alive[id] && present[id] && (step & 2)) {
                    CHECK(cw_comp_touch(&w, id, 191));
                    ev_modified[id] = true;
                }
                break;
            case 6:
                if (step >= STEPS / 3) {
                    CHECK(cw_comp_add(&w, id, TAG) == alive[id]);
                    if (alive[id]) tagged[id] = true;
                }
                break;
            case 7:
                if (step >= STEPS / 3) {
                    CHECK(cw_comp_remove(&w, id, TAG) == (alive[id] && tagged[id]));
                    tagged[id] = false;
                }
                break;
            default: break;
        }
        if (step % 37 == 0) {
            cw_entity_id actual[N];
            size_t n = cw_query_collect(&w, value_mask(191), actual, N), expected = 0, live = 0;
            size_t counts[QUERIES] = {0};
            for (unsigned i = 0; i < N; ++i) {
                live += alive[i];
                CHECK(cw_world_is_valid(&w, i) == alive[i]);
                CHECK(cw_comp_has(&w, i, 191) == (alive[i] && present[i]));
                if (alive[i] && present[i]) CHECK(*CW_GET(&w, i, 191, int64_t) == values[i]);
                if (alive[i] && present[i] && !pending[i]) {
                    CHECK(expected < n && actual[expected] == i);
                    ++expected;
                }
                bool visible = alive[i] && !pending[i];
                bool match[QUERIES] = {
                    visible && present[i], visible && tagged[i], visible && present[i] && !tagged[i],
                    visible && (present[i] || tagged[i]), visible && !present[i] && !tagged[i],
                };
                for (int qi = 0; qi < QUERIES; ++qi) {
                    CHECK(cw_query_contains(queries[qi], i) == match[qi]);
                    counts[qi] += match[qi];
                }
            }
            CHECK(expected == n && cw_world_entity_count(&w) == live);
            for (int qi = 0; qi < QUERIES; ++qi) {
                CHECK(cw_query_count(queries[qi]) == counts[qi]);
                CHECK(cw_query_gather(queries[qi], actual, N) == counts[qi]);
                check_cached_matches_adhoc(queries[qi]);
            }
            /* observed change sets match the events since the previous observation */
            size_t pending_before[3] = {cw_observer_pending(observer, CW_CHANGE_ADDED),
                cw_observer_pending(observer, CW_CHANGE_MODIFIED), cw_observer_pending(observer, CW_CHANGE_REMOVED)};
            cw_observer_observe(observer);
            size_t ev_counts[3] = {0}, walked[3] = {0};
            for (unsigned i = 0; i < N; ++i) {
                CHECK(cw_observer_has(observer, CW_CHANGE_ADDED, i) == ev_added[i]);
                CHECK(cw_observer_has(observer, CW_CHANGE_MODIFIED, i) == ev_modified[i]);
                CHECK(cw_observer_has(observer, CW_CHANGE_REMOVED, i) == ev_removed[i]);
                ev_counts[0] += ev_added[i]; ev_counts[1] += ev_modified[i]; ev_counts[2] += ev_removed[i];
                bool readable = alive[i] && !pending[i] && present[i];
                walked[0] += ev_added[i] && readable;
                walked[1] += ev_modified[i] && readable;
                walked[2] += ev_removed[i];
                ev_added[i] = ev_modified[i] = ev_removed[i] = false;
            }
            for (int k = 0; k < 3; ++k) {
                CHECK(cw_observer_count(observer, (cw_change)k) == ev_counts[k] && pending_before[k] == ev_counts[k]);
                CHECK(cw_observer_gather(observer, (cw_change)k, NULL, 0) == ev_counts[k]);
                CHECK(observer_walk(observer, (cw_change)k, actual, N) == walked[k]);
                CHECK(cw_observer_pending(observer, (cw_change)k) == 0);
            }
            /* the idle consumer keeps accumulating; walks never yield unreadable ids */
            CHECK(cw_observer_pending(idle, CW_CHANGE_ADDED) >= ev_counts[0]);
            cw_query it = cw_observer_iter(observer, CW_CHANGE_MODIFIED);
            cw_entity_id mid;
            while (cw_query_next(&it, &mid)) CHECK(*CW_GET(&w, mid, 191, int64_t) == values[mid]);
        }
    }
    cw_observer_observe(idle);
    CHECK(cw_observer_observations(idle) == 1 && cw_observer_observations(observer) == STEPS / 37 + 1);
    cw_observer_destroy(observer);
    cw_observer_destroy(idle);
    for (int qi = 0; qi < QUERIES; ++qi) cw_query_destroy(queries[qi]);
    cw_world_destroy(&w);
}

int main(void) {
    unsigned tests = 0;
    RUN(test_entity_lifetime);
    RUN(test_registry_and_bounds);
    RUN(test_components_and_callbacks);
    RUN(test_queries_across_words);
    RUN(test_restoration_and_rebuild);
    RUN(test_recycling_and_reset);
    RUN(test_batch_reservation);
    RUN(test_memory_stats);
    RUN(test_alignment_and_tags);
    RUN(test_codecs);
    RUN(test_scheduler);
    RUN(test_tick_lifecycle);
    RUN(test_scheduler_mutation_guard);
    RUN(test_clock);
    RUN(test_filters);
    RUN(test_cached_query_ownership);
    RUN(test_cached_query_lifecycle);
    RUN(test_cached_query_mutation_during_iteration);
    RUN(test_query_access_and_conflicts);
    RUN(test_change_observers);
    RUN(test_query_helpers_and_checked_access);
    RUN(test_command_buffer);
    RUN(test_command_buffer_in_sim);
    RUN(test_randomized_reference_model);
    printf("%u tests passed\n", tests);
    return 0;
}
