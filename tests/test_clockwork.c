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

static uint32_t rng_state = UINT32_C(0x1a2b3c4d);
static uint32_t next_random(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static void test_randomized_reference_model(void) {
    enum { N = 600, STEPS = 12000 };
    cw_world w;
    bool alive[N] = {0}, pending[N] = {0}, present[N] = {0};
    int64_t values[N] = {0};
    cw_world_init(&w, NULL);
    CHECK(CW_REGISTER(&w, 191, int64_t));
    for (int step = 0; step < STEPS; ++step) {
        unsigned id = next_random() % N;
        switch (next_random() % 7) {
            case 0:
                CHECK(cw_world_restore_entity(&w, id) == !alive[id]);
                alive[id] = true; pending[id] = false;
                break;
            case 1:
                if (alive[id]) {
                    values[id] = step;
                    set_value(&w, id, 191, values[id]);
                    present[id] = true;
                }
                break;
            case 2:
                CHECK(cw_comp_remove(&w, id, 191) == (alive[id] && present[id]));
                present[id] = false;
                break;
            case 3:
                cw_world_mark_destroy(&w, id);
                if (alive[id]) pending[id] = true;
                break;
            case 4:
                cw_world_sweep(&w);
                for (unsigned i = 0; i < N; ++i)
                    if (pending[i]) alive[i] = pending[i] = present[i] = false;
                break;
            case 5:
                CHECK(cw_world_rebuild_entity(&w, id) == alive[id]);
                if (alive[id]) present[id] = pending[id] = false;
                break;
            default: break;
        }
        if (step % 37 == 0) {
            cw_entity_id actual[N];
            size_t n = cw_query_collect(&w, value_mask(191), actual, N), expected = 0, live = 0;
            for (unsigned i = 0; i < N; ++i) {
                live += alive[i];
                CHECK(cw_world_is_valid(&w, i) == alive[i]);
                CHECK(cw_comp_has(&w, i, 191) == (alive[i] && present[i]));
                if (alive[i] && present[i]) CHECK(*CW_GET(&w, i, 191, int64_t) == values[i]);
                if (alive[i] && present[i] && !pending[i]) {
                    CHECK(expected < n && actual[expected] == i);
                    ++expected;
                }
            }
            CHECK(expected == n && cw_world_entity_count(&w) == live);
        }
    }
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
    RUN(test_alignment_and_tags);
    RUN(test_codecs);
    RUN(test_scheduler);
    RUN(test_tick_lifecycle);
    RUN(test_scheduler_mutation_guard);
    RUN(test_clock);
    RUN(test_randomized_reference_model);
    printf("%u tests passed\n", tests);
    return 0;
}
