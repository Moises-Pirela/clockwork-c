/* Interactive save/load tool for Clockwork worlds (raylib + raygui).
 *
 * Buttons drive the public API only: populate and tick a world, mark and
 * sweep entities, save it to a file (visible-state serialization or a full
 * snapshot), destroy the world, load the file into a fresh world whose
 * schema is registered in the other order, verify it against what was
 * saved, peek at the file, and provoke failures (a codec-less component, a
 * corrupted file, an unwritable path) to watch the error reporting and the
 * "a failed save never touches the valid file" guarantee.
 *
 * Usage: clockwork_save_load [--headless] [path]
 *   default   opens the window (path defaults to clockwork_save.cwsn)
 *   --headless runs the same actions as a script, prints "OK: ..." and exits
 *              0 on success; this is what the build scripts run.
 */
#include "clockwork.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "raylib.h"
#include "raygui.h"

/* ------------------------------------------------------------------------- */
/* Schema: portable codecs built on the byte primitives                      */
/* ------------------------------------------------------------------------- */
typedef struct { float x, y; } Position;
typedef struct { float x, y; } Velocity;
typedef struct { int32_t current, maximum; } Health;
typedef struct { char text[24]; } Name;

static bool marshal_vec2(cw_world *w, cw_entity_id id, cw_component_id ct, void *data, size_t capacity, size_t *size) {
    const Position *p = cw_comp_ptr(w, id, ct);
    cw_writer out;
    cw_writer_init_fixed(&out, data, capacity);
    cw_writer_f32(&out, p->x);
    cw_writer_f32(&out, p->y);
    *size = out.size;
    return cw_writer_ok(&out);
}
static bool unmarshal_vec2(cw_world *w, cw_entity_id id, cw_component_id ct, const void *data, size_t size) {
    cw_reader in = cw_reader_make(data, size);
    Position p;
    if (!cw_reader_f32(&in, &p.x) || !cw_reader_f32(&in, &p.y) || !cw_reader_at_end(&in)) return false;
    *(Position *)cw_comp_ptr(w, id, ct) = p;
    return true;
}
static bool marshal_health(cw_world *w, cw_entity_id id, cw_component_id ct, void *data, size_t capacity, size_t *size) {
    const Health *h = cw_comp_ptr(w, id, ct);
    cw_writer out;
    cw_writer_init_fixed(&out, data, capacity);
    cw_writer_i32(&out, h->current);
    cw_writer_i32(&out, h->maximum);
    *size = out.size;
    return cw_writer_ok(&out);
}
static bool unmarshal_health(cw_world *w, cw_entity_id id, cw_component_id ct, const void *data, size_t size) {
    cw_reader in = cw_reader_make(data, size);
    Health h;
    if (!cw_reader_i32(&in, &h.current) || !cw_reader_i32(&in, &h.maximum) || !cw_reader_at_end(&in)) return false;
    if (h.current < 0 || h.current > h.maximum) return false;   /* codecs may validate */
    *(Health *)cw_comp_ptr(w, id, ct) = h;
    return true;
}
static bool marshal_name(cw_world *w, cw_entity_id id, cw_component_id ct, void *data, size_t capacity, size_t *size) {
    const Name *n = cw_comp_ptr(w, id, ct);
    cw_writer out;
    cw_writer_init_fixed(&out, data, capacity);
    cw_writer_u8(&out, (uint8_t)strlen(n->text));
    cw_writer_bytes(&out, n->text, strlen(n->text));
    *size = out.size;
    return cw_writer_ok(&out);
}
static bool unmarshal_name(cw_world *w, cw_entity_id id, cw_component_id ct, const void *data, size_t size) {
    cw_reader in = cw_reader_make(data, size);
    Name n = {{0}};
    uint8_t length;
    if (!cw_reader_u8(&in, &length) || length >= sizeof(n.text)) return false;
    if (!cw_reader_bytes(&in, n.text, length) || !cw_reader_at_end(&in)) return false;
    *(Name *)cw_comp_ptr(w, id, ct) = n;
    return true;
}
/* A tag carries no data: its codec writes nothing and accepts only nothing. */
static bool marshal_tag(cw_world *w, cw_entity_id id, cw_component_id ct, void *data, size_t capacity, size_t *size) {
    (void)w; (void)id; (void)ct; (void)data; (void)capacity;
    *size = 0;
    return true;
}
static bool unmarshal_tag(cw_world *w, cw_entity_id id, cw_component_id ct, const void *data, size_t size) {
    (void)w; (void)id; (void)ct; (void)data;
    return size == 0;
}

typedef struct { cw_component_id position, velocity, health, name, frozen, debug, unportable; } Schema;
/* Two registration orders; components are matched by name, so a file saved
 * with one loads with the other. */
static const Schema schema_a = {0, 1, 2, 3, 4, 5, 6}, schema_b = {40, 7, 191, 0, 64, 3, 100};
static bool register_schema(cw_world *w, const Schema *s) {
    cw_codec vec2 = {marshal_vec2, unmarshal_vec2}, health = {marshal_health, unmarshal_health};
    cw_codec name = {marshal_name, unmarshal_name}, tag = {marshal_tag, unmarshal_tag};
    return cw_comp_register(w, s->position, sizeof(Position), _Alignof(Position), "Position", NULL)
        && cw_comp_set_codec(w, s->position, vec2)
        && cw_comp_register(w, s->velocity, sizeof(Velocity), _Alignof(Velocity), "Velocity", NULL)
        && cw_comp_set_codec(w, s->velocity, vec2)
        && cw_comp_register(w, s->health, sizeof(Health), _Alignof(Health), "Health", NULL)
        && cw_comp_set_codec(w, s->health, health)
        && cw_comp_register(w, s->name, sizeof(Name), 1, "Name", NULL)
        && cw_comp_set_codec(w, s->name, name)
        && cw_comp_register(w, s->frozen, 0, 1, "Frozen", NULL)
        && cw_comp_set_codec(w, s->frozen, tag)
        && cw_comp_register(w, s->debug, sizeof(int64_t), _Alignof(int64_t), "DebugCounter", NULL)
        && cw_comp_set_transient(w, s->debug, true)          /* never saved */
        && cw_comp_register(w, s->unportable, sizeof(int), _Alignof(int), "Unportable", NULL);
        /* Unportable has no codec on purpose: saving an entity that carries it fails. */
}

/* ------------------------------------------------------------------------- */
/* Application state and actions (shared by the window and headless mode)   */
/* ------------------------------------------------------------------------- */
#define CANVAS_W 800.0f
#define CANVAS_H 360.0f
#define MAX_EXPECTED 4096
#define LOG_LINES 14
#define LOG_WIDTH 120

typedef struct {
    cw_entity_id id;
    bool pending, has_position, has_health, has_name, frozen;
    Position position;
    Health health;
    Name name;
} Expected;

typedef struct {
    cw_world world;
    cw_sim sim;
    const Schema *schema;
    unsigned generation;                 /* worlds created so far; alternates the schema */
    char path[512];
    bool full_snapshot;                  /* cw_snapshot_* instead of cw_world_(de)serialize */
    /* what the last save contained */
    Expected *expected;
    size_t expected_count;
    cw_entity_id expected_next_id;
    uint64_t expected_tick;
    bool expected_full, have_expected;
    cw_buffer saved_image;               /* bytes of the file after the last successful save */
    cw_snapshot_info info;
    bool have_info;
    /* feedback */
    char status[256];
    bool status_error;
    char log[LOG_LINES][LOG_WIDTH];
    unsigned log_next, log_count;
    uint32_t rng;
    bool failed;                         /* headless: any unexpected outcome */
} App;
static App app;

static uint32_t next_random(void) {
    app.rng = app.rng * 1664525u + 1013904223u;
    return app.rng >> 8;
}
static float random_float(float limit) { return (float)(next_random() % 10000) / 10000.0f * limit; }

static void logf_line(const char *fmt, ...);
static void set_status(bool error, const char *fmt, ...);
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
static void logf_line(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(app.log[app.log_next], LOG_WIDTH, fmt, args);
    va_end(args);
    fputs(app.log[app.log_next], stdout);
    fputc('\n', stdout);
    app.log_next = (app.log_next + 1) % LOG_LINES;
    if (app.log_count < LOG_LINES) ++app.log_count;
}
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void set_status(bool error, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(app.status, sizeof(app.status), fmt, args);
    va_end(args);
    app.status_error = error;
    if (error) app.failed = true;
    logf_line("%s%s", error ? "ERROR: " : "", app.status);
}
static void describe_error(const cw_file_error *e, char *out, size_t capacity) {
    if (e->status != CW_FILE_ERROR_SNAPSHOT) {
        if (e->errnum) snprintf(out, capacity, "%s (errno %d)", cw_file_status_string(e->status), e->errnum);
        else snprintf(out, capacity, "%s", cw_file_status_string(e->status));
        return;
    }
    const cw_snapshot_error *s = &e->snapshot;
    int n = s->offset ? snprintf(out, capacity, "%s at byte %zu", cw_snapshot_status_string(s->status), s->offset)
                      : snprintf(out, capacity, "%s", cw_snapshot_status_string(s->status));
    if (n < 0 || (size_t)n >= capacity) return;
    if (s->name[0]) n += snprintf(out + n, capacity - (size_t)n, ", component %s", s->name);
    if (n < 0 || (size_t)n >= capacity) return;
    if (s->entity != CW_INVALID_ENTITY) snprintf(out + n, capacity - (size_t)n, ", entity %" PRId64, s->entity);
}

/* Wrap-around movement so entities stay on the canvas. */
static void movement(cw_world *w, cw_phase phase, float dt) {
    (void)phase;
    const Schema *s = app.schema;
    const cw_component_id required[] = {s->position, s->velocity}, excluded[] = {s->frozen};
    cw_query q = cw_query_iter_filter(w, cw_filter_make(cw_mask(required, 2), cw_mask(NULL, 0), cw_mask(excluded, 1)));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        Position *p = cw_comp_ptr(w, id, s->position);
        const Velocity *v = cw_comp_ptr(w, id, s->velocity);
        p->x += v->x * dt;
        p->y += v->y * dt;
        if (p->x < 0) p->x += CANVAS_W; else if (p->x >= CANVAS_W) p->x -= CANVAS_W;
        if (p->y < 0) p->y += CANVAS_H; else if (p->y >= CANVAS_H) p->y -= CANVAS_H;
    }
}
static void world_up(void) {
    app.schema = app.generation % 2 == 0 ? &schema_a : &schema_b;
    ++app.generation;
    cw_world_init(&app.world, NULL);
    cw_sim_init(&app.sim, CW_HOST);
    if (!register_schema(&app.world, app.schema)) { set_status(true, "schema registration failed"); return; }
    cw_system_desc system = {0};
    system.name = "movement";
    system.phases = CW_PHASE(CW_UPDATE);
    system.fn = movement;
    cw_sim_register(&app.sim, system);
}
static void world_down(void) {
    cw_sim_destroy(&app.sim);
    cw_world_destroy(&app.world);
}
static bool world_is_fresh(void) {
    return cw_world_entity_count(&app.world) == 0 && app.world.next_id == 0 && app.world.recycled_ids.count == 0;
}
static size_t visible_count(void) { return cw_query_collect(&app.world, cw_mask(NULL, 0), NULL, 0); }

static void act_populate(int count) {
    const Schema *s = app.schema;
    for (int i = 0; i < count; ++i) {
        cw_entity_id id = cw_world_create_entity(&app.world);
        Position p = {random_float(CANVAS_W), random_float(CANVAS_H)};
        Velocity v = {random_float(120.0f) - 60.0f, random_float(120.0f) - 60.0f};
        Health h = {(int32_t)(next_random() % 101), 100};
        Name n = {{0}};
        int64_t debug = (int64_t)next_random();
        snprintf(n.text, sizeof(n.text), "unit-%" PRId64, id);
        cw_comp_set(&app.world, id, s->position, &p, sizeof(p));
        if (next_random() % 4) cw_comp_set(&app.world, id, s->velocity, &v, sizeof(v));
        if (next_random() % 3) cw_comp_set(&app.world, id, s->health, &h, sizeof(h));
        if (next_random() % 5) cw_comp_set(&app.world, id, s->name, &n, sizeof(n));
        if (next_random() % 6 == 0) cw_comp_add(&app.world, id, s->frozen);
        cw_comp_set(&app.world, id, s->debug, &debug, sizeof(debug));
    }
    set_status(false, "created %d entities (%zu visible, next id %" PRId64 ")", count, visible_count(), app.world.next_id);
}
static void act_tick(int ticks) {
    for (int i = 0; i < ticks; ++i) cw_sim_tick(&app.sim, &app.world, 1.0f / 30.0f);
    set_status(false, "ran %d ticks, tick is now %" PRIu64, ticks, app.world.tick);
}
static cw_entity_id pick_visible(void) {
    size_t n = visible_count();
    if (!n) return CW_INVALID_ENTITY;
    cw_entity_id *ids = malloc(n * sizeof(*ids));
    cw_query_collect(&app.world, cw_mask(NULL, 0), ids, n);
    cw_entity_id id = ids[next_random() % n];
    free(ids);
    return id;
}
static void act_mark_one(void) {
    cw_entity_id id = pick_visible();
    if (id == CW_INVALID_ENTITY) { set_status(true, "no visible entity to mark"); return; }
    cw_world_mark_destroy(&app.world, id);
    set_status(false, "entity %" PRId64 " marked for destruction (%zu pending; %s)", id, app.world.pending_destroy.count,
               app.full_snapshot ? "a full snapshot keeps it pending" : "excluded from serialization");
}
static void act_sweep(void) {
    size_t pending = app.world.pending_destroy.count;
    cw_world_sweep(&app.world);
    set_status(false, "swept %zu entities (%zu recycled ids)", pending, app.world.recycled_ids.count);
}
static void act_toggle_unportable(void) {
    const Schema *s = app.schema;
    cw_query q = cw_query_iter(&app.world, cw_mask(&s->unportable, 1));
    cw_entity_id id;
    if (cw_query_next(&q, &id)) {
        cw_comp_remove(&app.world, id, s->unportable);
        set_status(false, "removed the codec-less Unportable component from entity %" PRId64, id);
        return;
    }
    id = pick_visible();
    if (id == CW_INVALID_ENTITY) { set_status(true, "no visible entity to poison"); return; }
    cw_comp_add(&app.world, id, s->unportable);
    set_status(false, "entity %" PRId64 " now carries Unportable, which has no codec: saving will be refused", id);
}

static void capture(void) {
    const Schema *s = app.schema;
    app.expected_count = 0;
    app.expected_full = app.full_snapshot;
    for (size_t i = 0; i < app.world.capacity && app.expected_count < MAX_EXPECTED; ++i) {
        const cw_entity *e = &app.world.entities[i];
        bool pending = (e->flags & CW_TO_DESTROY) != 0;
        if (!cw_entity_is_active(*e) || (pending && !app.full_snapshot)) continue;
        Expected *x = &app.expected[app.expected_count++];
        memset(x, 0, sizeof(*x));
        x->id = e->id;
        x->pending = pending;
        if ((x->has_position = cw_comp_has(&app.world, e->id, s->position))) x->position = *(Position *)cw_comp_ptr(&app.world, e->id, s->position);
        if ((x->has_health = cw_comp_has(&app.world, e->id, s->health))) x->health = *(Health *)cw_comp_ptr(&app.world, e->id, s->health);
        if ((x->has_name = cw_comp_has(&app.world, e->id, s->name))) x->name = *(Name *)cw_comp_ptr(&app.world, e->id, s->name);
        x->frozen = cw_comp_has(&app.world, e->id, s->frozen);
    }
    app.expected_next_id = app.world.next_id;
    app.expected_tick = app.world.tick;
    app.have_expected = true;
}
static void refresh_file_info(void) {
    cw_file_error error;
    cw_buffer image;
    app.have_info = false;
    if (!cw_file_read(app.path, 0, &image, &error)) return;
    app.have_info = cw_snapshot_peek(image.data, image.size, &app.info, NULL);
    cw_buffer_free(&image);
}
/* After a failed save: is the file still exactly what the last good save wrote? */
static void report_file_untouched(void) {
    if (!app.saved_image.data) return;
    cw_file_error error;
    cw_buffer now;
    if (!cw_file_read(app.path, 0, &now, &error)) { set_status(true, "cannot re-read %s", app.path); return; }
    bool same = now.size == app.saved_image.size && memcmp(now.data, app.saved_image.data, now.size) == 0;
    cw_buffer_free(&now);
    if (same) logf_line("file untouched: %s still holds the previous %zu-byte image", app.path, app.saved_image.size);
    else set_status(true, "file changed after a failed save");
}
static void save_to(const char *path, bool expect_success) {
    cw_file_error error;
    cw_serialize_config serialize = {0};
    cw_snapshot_save_config snapshot = {0};
    serialize.user_version = snapshot.user_version = 1;
    bool ok = app.full_snapshot ? cw_snapshot_save_file(&app.world, path, &snapshot, &error)
                                : cw_world_serialize_file(&app.world, path, &serialize, &error);
    if (ok) {
        if (!expect_success) { set_status(true, "save to %s succeeded but was expected to fail", path); return; }
        capture();
        cw_buffer_free(&app.saved_image);
        cw_file_read(path, 0, &app.saved_image, &error);
        refresh_file_info();
        set_status(false, "%s: %zu entities, %zu bytes -> %s", app.full_snapshot ? "snapshot saved" : "serialized",
                   app.expected_count, app.saved_image.size, path);
        return;
    }
    char why[256];
    describe_error(&error, why, sizeof(why));
    if (expect_success) set_status(true, "save failed: %s", why);
    else { set_status(false, "save refused as expected: %s", why); }
    if (strcmp(path, app.path) == 0) report_file_untouched();
}
static void act_save(void) {
    const Schema *s = app.schema;
    bool poisoned = cw_query_collect(&app.world, cw_mask(&s->unportable, 1), NULL, 0) > 0;
    save_to(app.path, !poisoned);
}
static void act_save_bad_path(void) { save_to("no-such-directory/clockwork.cwsn", false); }
static void act_destroy_world(void) {
    size_t count = cw_world_entity_count(&app.world);
    world_down();
    world_up();
    set_status(false, "world with %zu entities destroyed; fresh world registered with schema order %s",
               count, app.schema == &schema_a ? "A (Position=0...)" : "B (Position=40...)");
}
static void act_load(bool expect_success) {
    cw_file_error error;
    cw_deserialize_config deserialize = cw_deserialize_defaults();
    cw_snapshot_load_config snapshot = cw_snapshot_load_defaults();
    deserialize.check_user_version = true;
    deserialize.user_version = 1;
    bool ok = app.full_snapshot ? cw_snapshot_load_file(&app.world, app.path, &snapshot, &error)
                                : cw_world_deserialize_file(&app.world, app.path, &deserialize, &error);
    if (ok) {
        if (!expect_success) { set_status(true, "load succeeded but was expected to fail"); return; }
        set_status(false, "loaded %zu entities from %s (tick %" PRIu64 ", next id %" PRId64 ", schema order %s)",
                   cw_world_entity_count(&app.world), app.path, app.world.tick, app.world.next_id,
                   app.schema == &schema_a ? "A" : "B");
        return;
    }
    char why[256];
    describe_error(&error, why, sizeof(why));
    if (expect_success) set_status(true, "load failed: %s", why);
    else set_status(false, "load refused as expected: %s", why);
    if (error.status == CW_FILE_ERROR_SNAPSHOT && error.snapshot.status == CW_SNAPSHOT_ERROR_WORLD_NOT_FRESH)
        logf_line("hint: loading needs a fresh world, press Destroy world first");
    else if (!world_is_fresh()) set_status(true, "world is not fresh after a failed load");
    else logf_line("world is still fresh after the failed load");
}
static bool verify(void) {
    const Schema *s = app.schema;
    cw_world *w = &app.world;
    if (!app.have_expected) { set_status(true, "nothing to verify against: save first"); return false; }
    if (cw_world_entity_count(w) != app.expected_count) {
        set_status(true, "verify: %zu entities, expected %zu", cw_world_entity_count(w), app.expected_count);
        return false;
    }
    if (w->next_id != app.expected_next_id || w->tick != app.expected_tick) {
        set_status(true, "verify: next id %" PRId64 "/tick %" PRIu64 ", expected %" PRId64 "/%" PRIu64,
                   w->next_id, w->tick, app.expected_next_id, app.expected_tick);
        return false;
    }
    for (size_t i = 0; i < app.expected_count; ++i) {
        const Expected *e = &app.expected[i];
        bool ok = cw_world_is_valid(w, e->id)
            && ((w->entities[e->id].flags & CW_TO_DESTROY) != 0) == e->pending
            && cw_comp_has(w, e->id, s->position) == e->has_position && cw_comp_has(w, e->id, s->health) == e->has_health
            && cw_comp_has(w, e->id, s->name) == e->has_name && cw_comp_has(w, e->id, s->frozen) == e->frozen
            && !cw_comp_has(w, e->id, s->debug)
            && (!e->has_position || memcmp(cw_comp_ptr(w, e->id, s->position), &e->position, sizeof(Position)) == 0)
            && (!e->has_health || memcmp(cw_comp_ptr(w, e->id, s->health), &e->health, sizeof(Health)) == 0)
            && (!e->has_name || strcmp(((Name *)cw_comp_ptr(w, e->id, s->name))->text, e->name.text) == 0);
        if (!ok) { set_status(true, "verify: entity %" PRId64 " differs from what was saved", e->id); return false; }
    }
    set_status(false, "verified: %zu entities, tick %" PRIu64 " and next id %" PRId64 " match the %s",
               app.expected_count, app.expected_tick, app.expected_next_id, app.expected_full ? "snapshot" : "serialization");
    return true;
}
static void act_peek(void) {
    refresh_file_info();
    if (!app.have_info) { set_status(true, "%s is not a readable snapshot", app.path); return; }
    set_status(false, "peek: format v%u, user version %u, %" PRIu64 " bytes, %" PRIu64 " entities, %u component types, tick %" PRIu64,
               app.info.format_version, app.info.user_version, app.info.size, app.info.entity_count,
               app.info.component_count, app.info.tick);
}
/* Offset of the length field of the "ENTS" section, or 0 when not found. */
static size_t entities_length_offset(const cw_buffer *image) {
    cw_reader r = cw_reader_make(image->data, image->size);
    uint32_t count, tag;
    uint64_t length;
    if (!cw_reader_skip(&r, 20) || !cw_reader_u32(&r, &count)) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!cw_reader_u32(&r, &tag)) return 0;
        size_t at = r.offset;
        if (!cw_reader_u64(&r, &length)) return 0;
        if (tag == ((uint32_t)'E' | (uint32_t)'N' << 8 | (uint32_t)'T' << 16 | (uint32_t)'S' << 24)) return at;
        if (!cw_reader_skip(&r, (size_t)length)) return 0;
    }
    return 0;
}
static void act_corrupt(void) {
    cw_file_error error;
    cw_buffer image;
    if (!cw_file_read(app.path, 0, &image, &error) || image.size < 64) { set_status(true, "no file to corrupt"); return; }
    /* Pick a byte whose change is guaranteed to be caught: the format
     * version, the total size, a section length, or a component name. */
    const char *what;
    size_t at;
    switch (next_random() % 4) {
        case 0: at = 4; what = "the format version"; break;
        case 1: at = 8; what = "the total size"; break;
        case 2: at = entities_length_offset(&image); what = "the entity section length"; break;
        default: at = 24 + 12 + 25 + 12 + 2 + 12; what = "a component name"; break;   /* first schema entry's name */
    }
    if (!at || at >= image.size) { at = 4; what = "the format version"; }
    image.data[at] ^= 0x20;                /* flips one bit; keeps names printable */
    bool ok = cw_file_write(app.path, image.data, image.size, &error);
    cw_buffer_free(&image);
    if (!ok) { set_status(true, "could not write the corrupted file"); return; }
    cw_buffer_free(&app.saved_image);      /* no longer a good image */
    refresh_file_info();
    set_status(false, "flipped byte %zu (%s) of %s: loading it now reports where it breaks", at, what, app.path);
}
static void act_create_after_load(void) {
    cw_entity_id id = cw_world_create_entity(&app.world);
    set_status(false, "created entity %" PRId64 "%s", id,
               app.have_expected && id == app.expected_next_id ? " - exactly where the saved world left off" : "");
}

/* ------------------------------------------------------------------------- */
/* Headless script: the button sequence as a regression test                 */
/* ------------------------------------------------------------------------- */
static int run_headless(void) {
    world_up();
    act_populate(10);
    act_tick(30);
    act_mark_one();
    if (visible_count() != 9) { set_status(true, "expected 9 visible entities"); return 1; }
    act_save();
    act_peek();
    act_destroy_world();
    act_load(true);
    if (!verify()) return 1;
    act_create_after_load();
    if (app.world.next_id != app.expected_next_id + 1) { set_status(true, "id allocation resumed wrongly"); return 1; }
    act_load(false);                       /* not fresh any more */
    act_toggle_unportable();
    act_save();                            /* refused, file untouched */
    act_toggle_unportable();
    act_save_bad_path();                   /* refused, file untouched */
    act_destroy_world();
    act_load(true);
    if (!verify()) return 1;
    act_corrupt();
    act_destroy_world();
    act_load(false);                       /* corrupt image: refused, world still fresh */
    app.full_snapshot = true;              /* the same story with full snapshots */
    act_populate(6);
    act_mark_one();
    act_save();
    act_destroy_world();
    act_load(true);
    if (!verify()) return 1;
    if (app.world.pending_destroy.count != 1) { set_status(true, "pending destruction not restored"); return 1; }
    world_down();
    remove(app.path);
    if (app.failed) return 1;
    printf("OK: save/load verified (serialization and snapshot round trips)\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Window                                                                    */
/* ------------------------------------------------------------------------- */
#define WINDOW_W 1100
#define WINDOW_H 700
#define PANEL_X 16
#define PANEL_W 236
#define CANVAS_X 272
#define CANVAS_Y 16

static bool button(int row, const char *text) {
    return GuiButton((Rectangle){PANEL_X, 16 + (float)row * 36, PANEL_W, 30}, text) != 0;
}
static void draw_world(void) {
    const Schema *s = app.schema;
    Rectangle canvas = {CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H};
    DrawRectangleRec(canvas, (Color){24, 26, 32, 255});
    DrawRectangleLinesEx(canvas, 1, GRAY);
    for (size_t i = 0; i < app.world.capacity; ++i) {
        const cw_entity *e = &app.world.entities[i];
        if (!cw_entity_is_active(*e) || !cw_comp_has(&app.world, e->id, s->position)) continue;
        const Position *p = cw_comp_ptr(&app.world, e->id, s->position);
        float x = canvas.x + p->x, y = canvas.y + p->y;
        bool pending = (e->flags & CW_TO_DESTROY) != 0;
        Color fill = GRAY;
        if (cw_comp_has(&app.world, e->id, s->health)) {
            const Health *h = cw_comp_ptr(&app.world, e->id, s->health);
            float t = h->maximum ? (float)h->current / (float)h->maximum : 0;
            fill = (Color){(unsigned char)(220 * (1 - t)), (unsigned char)(200 * t), 60, 255};
        }
        if (pending) { DrawLine((int)x - 8, (int)y - 8, (int)x + 8, (int)y + 8, RED); DrawLine((int)x - 8, (int)y + 8, (int)x + 8, (int)y - 8, RED); continue; }
        DrawCircle((int)x, (int)y, 7, fill);
        if (cw_comp_has(&app.world, e->id, s->frozen)) DrawCircleLines((int)x, (int)y, 10, SKYBLUE);
        if (cw_comp_has(&app.world, e->id, s->unportable)) DrawCircleLines((int)x, (int)y, 12, MAGENTA);
        const char *label = cw_comp_has(&app.world, e->id, s->name) ? ((Name *)cw_comp_ptr(&app.world, e->id, s->name))->text
                                                                    : TextFormat("#%" PRId64, e->id);
        DrawText(label, (int)x + 10, (int)y - 6, 10, LIGHTGRAY);
    }
    DrawText("circle: health (red..green)   ring: Frozen tag   magenta: Unportable   X: pending destruction",
             CANVAS_X, CANVAS_Y + (int)CANVAS_H + 6, 10, GRAY);
}
static void draw_panel_text(void) {
    int y = CANVAS_Y + (int)CANVAS_H + 26;
    const cw_world *w = &app.world;
    DrawText(TextFormat("world  %zu entities (%zu visible, %zu pending), tick %" PRIu64 ", next id %" PRId64 ", %zu recycled, schema %s",
                        cw_world_entity_count(w), visible_count(), w->pending_destroy.count, w->tick, w->next_id,
                        w->recycled_ids.count, app.schema == &schema_a ? "A" : "B"), CANVAS_X, y, 10, DARKGRAY);
    y += 16;
    if (app.have_info)
        DrawText(TextFormat("file   %s: %" PRIu64 " bytes, format v%u, user v%u, %" PRIu64 " entities, %u component types, tick %" PRIu64 ", next id %" PRId64 "%s",
                            app.path, app.info.size, app.info.format_version, app.info.user_version, app.info.entity_count,
                            app.info.component_count, app.info.tick, app.info.next_id, app.saved_image.data ? "" : " (modified)"),
                 CANVAS_X, y, 10, DARKGRAY);
    else DrawText(TextFormat("file   %s: no readable snapshot", app.path), CANVAS_X, y, 10, DARKGRAY);
    y += 16;
    if (app.have_expected)
        DrawText(TextFormat("saved  %zu entities, tick %" PRIu64 ", next id %" PRId64 " (%s)", app.expected_count,
                            app.expected_tick, app.expected_next_id, app.expected_full ? "full snapshot" : "visible state"),
                 CANVAS_X, y, 10, DARKGRAY);
    y += 22;
    DrawRectangle(CANVAS_X, y, (int)CANVAS_W, 22, app.status_error ? (Color){255, 225, 225, 255} : (Color){225, 245, 225, 255});
    DrawText(app.status, CANVAS_X + 6, y + 6, 10, app.status_error ? MAROON : DARKGREEN);
    y += 30;
    for (unsigned i = 0; i < app.log_count; ++i) {
        unsigned index = (app.log_next + LOG_LINES - app.log_count + i) % LOG_LINES;
        DrawText(app.log[index], CANVAS_X, y + (int)i * 13, 10, (Color){90, 90, 90, 255});
    }
}
static int run_window(void) {
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(WINDOW_W, WINDOW_H, "Clockwork save/load");
    SetTargetFPS(60);
    GuiSetStyle(DEFAULT, TEXT_SIZE, 12);
    world_up();
    set_status(false, "fresh world, schema order A; populate it, save, destroy, load, verify");
    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground(RAYWHITE);
        int row = 0;
        GuiLabel((Rectangle){PANEL_X, 16 + (float)row++ * 36, PANEL_W, 30}, "World");
        if (button(row++, "Populate +10") || IsKeyPressed(KEY_P)) act_populate(10);
        if (button(row++, "Tick x10") || IsKeyPressed(KEY_T)) act_tick(10);
        if (button(row++, "Mark one for destruction")) act_mark_one();
        if (button(row++, "Sweep")) act_sweep();
        if (button(row++, "Create entity")) act_create_after_load();
        if (button(row++, "Destroy world (fresh, other schema)") || IsKeyPressed(KEY_D)) act_destroy_world();
        GuiLabel((Rectangle){PANEL_X, 16 + (float)row++ * 36, PANEL_W, 30}, "File");
        GuiCheckBox((Rectangle){PANEL_X, 16 + (float)row++ * 36 + 6, 18, 18}, "Full snapshot (pending + recycled)", &app.full_snapshot);
        if (button(row++, "Save") || IsKeyPressed(KEY_S)) act_save();
        if (button(row++, "Load") || IsKeyPressed(KEY_L)) act_load(true);
        if (button(row++, "Verify against saved") || IsKeyPressed(KEY_V)) verify();
        if (button(row++, "Peek file")) act_peek();
        GuiLabel((Rectangle){PANEL_X, 16 + (float)row++ * 36, PANEL_W, 30}, "Provoke failures");
        if (button(row++, "Toggle codec-less component")) act_toggle_unportable();
        if (button(row++, "Save to unwritable path")) act_save_bad_path();
        if (button(row++, "Corrupt the file")) act_corrupt();
        draw_world();
        draw_panel_text();
        DrawText("keys: P populate  T tick  S save  D destroy  L load  V verify", PANEL_X, WINDOW_H - 20, 10, GRAY);
        EndDrawing();
        if (IsKeyPressed(KEY_F12)) TakeScreenshot("clockwork_save_load.png");
    }
    world_down();
    CloseWindow();
    return 0;
}

int main(int argc, char **argv) {
    bool headless = false;
    const char *path = "clockwork_save.cwsn";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--headless") == 0) headless = true;
        else path = argv[i];
    }
    snprintf(app.path, sizeof(app.path), "%s", path);
    app.rng = 0x2545f491u;
    app.expected = calloc(MAX_EXPECTED, sizeof(*app.expected));
    int result = headless ? run_headless() : run_window();
    cw_buffer_free(&app.saved_image);
    free(app.expected);
    return result;
}
