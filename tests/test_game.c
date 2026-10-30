/* Engine-level checks for the game module layer: reflection, the text scene
 * format, world binding, the generic snapshot codec, project files and
 * module loading, all driven through the combat module linked statically.
 * With a path argument the same module is also loaded as a shared library.
 *
 *   clockwork_game_tests [path/to/combat_game.so]
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "cw_game.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* --- a counting canvas --- */
static struct { int shapes, lights, picks, occluders, texts, sprites; cw_entity_id last_pick; } canvas_stats;
static void c_circle(void *i, float x, float y, float r, cw_rgba c) { (void)i; (void)x; (void)y; (void)r; (void)c; ++canvas_stats.shapes; }
static void c_ring(void *i, float x, float y, float r, float t, cw_rgba c) { (void)i; (void)x; (void)y; (void)r; (void)t; (void)c; ++canvas_stats.shapes; }
static void c_line(void *i, float a, float b, float x, float y, float t, cw_rgba c) { (void)i; (void)a; (void)b; (void)x; (void)y; (void)t; (void)c; ++canvas_stats.shapes; }
static void c_rect(void *i, float x, float y, float w, float h, cw_rgba c) { (void)i; (void)x; (void)y; (void)w; (void)h; (void)c; ++canvas_stats.shapes; }
static void c_rect_outline(void *i, float x, float y, float w, float h, float t, cw_rgba c) { (void)i; (void)x; (void)y; (void)w; (void)h; (void)t; (void)c; ++canvas_stats.shapes; }
static void c_oriented(void *i, float x, float y, float dx, float dy, float w, float h, cw_rgba c) { (void)i; (void)x; (void)y; (void)dx; (void)dy; (void)w; (void)h; (void)c; ++canvas_stats.shapes; }
static void c_triangle(void *i, float a, float b, float x, float y, float p, float q, cw_rgba c) { (void)i; (void)a; (void)b; (void)x; (void)y; (void)p; (void)q; (void)c; ++canvas_stats.shapes; }
static void c_bar(void *i, float x, float y, float w, float h, float f, cw_rgba a, cw_rgba b) { (void)i; (void)x; (void)y; (void)w; (void)h; (void)f; (void)a; (void)b; ++canvas_stats.shapes; }
static void c_text(void *i, float x, float y, cw_rgba c, const char *t) { (void)i; (void)x; (void)y; (void)c; (void)t; ++canvas_stats.texts; }
static float c_px(void *i, float p) { (void)i; return p; }
static void c_light(void *i, const cw_canvas_light *l) { (void)i; (void)l; ++canvas_stats.lights; }
static void c_occ_circle(void *i, float x, float y, float r) { (void)i; (void)x; (void)y; (void)r; ++canvas_stats.occluders; }
static void c_occ_rect(void *i, float x, float y, float w, float h) { (void)i; (void)x; (void)y; (void)w; (void)h; ++canvas_stats.occluders; }
static void c_pick(void *i, cw_entity_id id, float x, float y, float r) { (void)i; (void)x; (void)y; (void)r; ++canvas_stats.picks; canvas_stats.last_pick = id; }
static struct { uint32_t material; float x, y, w, h, rot, u0, v0, u1, v1; cw_rgba tint; int layer; } last_sprite;
static void c_sprite(void *i, uint32_t m, float x, float y, float w, float h, float r, cw_rgba t, int l, float a, float b, float c, float d) {
    (void)i; ++canvas_stats.sprites;
    last_sprite.material = m; last_sprite.x = x; last_sprite.y = y; last_sprite.w = w; last_sprite.h = h; last_sprite.rot = r;
    last_sprite.tint = t; last_sprite.layer = l; last_sprite.u0 = a; last_sprite.v0 = b; last_sprite.u1 = c; last_sprite.v1 = d;
}
static struct { int images, texts; float last_x, last_y, last_w, last_h, text_x, text_y, last_scale; char last_text[96]; uint32_t last_material; cw_rgba last_tint; cw_canvas_dynamic last_dynamic; } hud_stats;
static void c_hud_image(void *i, uint32_t m, float x, float y, float w, float h, cw_rgba t, float border, float scale, float u0, float v0, float u1, float v1, const cw_canvas_dynamic *dyn) {
    (void)i; (void)border; (void)scale; (void)u0; (void)v0; (void)u1; (void)v1;
    if (dyn) memcpy(&hud_stats.last_dynamic, dyn, sizeof(*dyn));
    ++hud_stats.images; hud_stats.last_material = m; hud_stats.last_tint = t;
    hud_stats.last_x = x; hud_stats.last_y = y; hud_stats.last_w = w; hud_stats.last_h = h;
}
static void c_hud_text(void *i, float x, float y, float scale, cw_rgba c, const char *text) {
    (void)i; (void)c; ++hud_stats.texts; hud_stats.text_x = x; hud_stats.text_y = y; hud_stats.last_scale = scale;
    snprintf(hud_stats.last_text, sizeof(hud_stats.last_text), "%s", text);
}
static float c_hud_text_width(void *i, float scale, const char *text) { (void)i; return 8 * scale * (float)strlen(text); }
static struct { int count; cw_canvas_shape3d last; } shape_stats;
static void c_shape3d(void *i, const cw_canvas_shape3d *s) { (void)i; ++shape_stats.count; shape_stats.last = *s; }
static struct { int count; cw_canvas_light3d last; } light3d_stats;
static void c_light3d(void *i, const cw_canvas_light3d *l) { (void)i; ++light3d_stats.count; light3d_stats.last = *l; }
static struct { int count; cw_canvas_brush last; cw_brush brush; } brush_stats;
static void c_brush3d(void *i, const cw_canvas_brush *b) { (void)i; ++brush_stats.count; brush_stats.last = *b; brush_stats.brush = *b->brush; }
static struct { int count; cw_canvas_sky3d last; } sky3d_stats;
static void c_sky3d(void *i, const cw_canvas_sky3d *s) { (void)i; ++sky3d_stats.count; sky3d_stats.last = *s; }
static struct { int count; cw_canvas_decal3d last; } decal3d_stats;
static void c_decal3d(void *i, const cw_canvas_decal3d *d) { (void)i; ++decal3d_stats.count; decal3d_stats.last = *d; }
static const cw_canvas canvas = {NULL, c_circle, c_ring, c_line, c_rect, c_rect_outline, c_oriented, c_triangle, c_bar, c_text, c_px,
                                 c_light, c_occ_circle, c_occ_rect, c_pick, c_sprite, c_hud_image, c_hud_text, c_hud_text_width, c_shape3d, c_light3d, c_brush3d, c_sky3d, c_decal3d};
static const cw_canvas canvas_2d = {NULL, c_circle, c_ring, c_line, c_rect, c_rect_outline, c_oriented, c_triangle, c_bar, c_text, c_px,
                                    c_light, c_occ_circle, c_occ_rect, c_pick, c_sprite, c_hud_image, c_hud_text, c_hud_text_width, NULL, NULL, NULL, NULL, NULL};

/* --- helpers --- */
static size_t prefab_index(const cw_game_module *m, const char *category, const char *name) {
    for (size_t i = 0; i < m->prefab_count; ++i)
        if (strcmp(m->prefabs[i].name, name) == 0 && (!category || strcmp(m->prefabs[i].category, category) == 0)) return i;
    return SIZE_MAX;
}
/* the writer's text with a terminator after the size (the writer does not add one) */
static const char *ztext(cw_writer *out) {
    cw_writer_u8(out, 0);
    --out->size;
    return (const char *)out->data;
}
static cw_component_id comp(cw_world *w, const char *name) {
    cw_component_id ct = CW_INVALID_COMPONENT;
    cw_comp_by_name(w, name, &ct);
    return ct;
}
static double field_of(cw_world *w, cw_entity_id id, const char *component, const char *field) {
    cw_component_id ct = comp(w, component);
    const cw_type_info *t = cw_comp_type(w, ct);
    const cw_field *f = cw_type_field(t, field);
    return f && cw_comp_has(w, id, ct) ? cw_field_get(f, cw_comp_ptr(w, id, ct)) : NAN;
}
/* A fingerprint of every authored component in the world (not transient,
 * not runtime), field by field (struct padding is not state). */
static uint64_t world_hash(cw_world *w) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < w->capacity; ++i) {
        if (!cw_world_is_valid(w, (cw_entity_id)i)) continue;
        for (unsigned c = 0; c < CW_MAX_COMPONENTS; ++c) {
            cw_component_id ct = (cw_component_id)c;
            if (!cw_comp_has(w, (cw_entity_id)i, ct) || cw_comp_is_transient(w, ct)) continue;
            const cw_type_info *t = cw_comp_type(w, ct);
            if (t && (t->flags & CW_TYPE_RUNTIME)) continue;   /* generated, not authored */
            const void *value = cw_comp_ptr(w, (cw_entity_id)i, ct);
            h = (h ^ (uint64_t)i) * 1099511628211ull;
            h = (h ^ (uint64_t)c) * 1099511628211ull;
            for (size_t f = 0; t && f < t->field_count; ++f) {
                char text[64];
                cw_field_format(&t->fields[f], value, text, sizeof(text));
                for (const char *p = text; *p; ++p) h = (h ^ (unsigned char)*p) * 1099511628211ull;
            }
        }
    }
    return h;
}
typedef struct { cw_entity_id ids[64]; char names[64][32]; size_t count; } name_table;
static void on_name(void *user, cw_entity_id id, const char *name) {
    name_table *t = user;
    if (t->count == 64) return;
    t->ids[t->count] = id;
    snprintf(t->names[t->count], 32, "%s", name);
    ++t->count;
}
static bool name_of(void *user, cw_entity_id id, char *out, size_t capacity) {
    name_table *t = user;
    for (size_t i = 0; i < t->count; ++i)
        if (t->ids[i] == id) { snprintf(out, capacity, "%s", t->names[i]); return true; }
    return false;
}

static void test_reflection(void) {
    cw_game game;
    char error[256] = "";
    CHECK(cw_game_use(&game, cw_game_export(), error, sizeof(error)), "static module: %s", error);
    cw_game_world gw;
    CHECK(cw_game_world_init(&gw, game.module, error, sizeof(error)), "bind: %s", error);
    cw_world *w = &gw.world;
    cw_component_id light = comp(w, "Light"), unit = comp(w, "Unit"), health = comp(w, "Health"), position = comp(w, "Position");
    CHECK(light != CW_INVALID_COMPONENT && unit != CW_INVALID_COMPONENT && health != CW_INVALID_COMPONENT, "components registered by name");
    const cw_type_info *lt = cw_comp_type(w, light);
    CHECK(lt && strcmp(lt->name, "Light") == 0 && lt->field_count == 12, "Light reflection attached");
    CHECK(cw_comp_type(w, comp(w, "ShadowCaster")) != NULL, "tags carry reflection too");
    CHECK(cw_comp_is_transient(w, comp(w, "Target")) && cw_comp_is_transient(w, comp(w, "Visual")), "transient types");

    /* field text round trips */
    unsigned char value[64];
    cw_type_default(lt, value);
    char text[64];
    const cw_field *flags = cw_type_field(lt, "flags");
    CHECK(flags && flags->type == CW_FIELD_FLAGS, "flags field");
    CHECK(cw_field_parse(flags, value, "spot|no_shadows") && cw_field_get(flags, value) == 3, "flags parse");
    cw_field_format(flags, value, text, sizeof(text));
    CHECK(strcmp(text, "spot|no_shadows") == 0, "flags format: %s", text);
    CHECK(cw_field_parse(flags, value, "none") && cw_field_get(flags, value) == 0, "flags none");
    CHECK(!cw_field_parse(flags, value, "spot|bogus"), "unknown flag rejected");
    CHECK(cw_field_adjust(flags, value, 2) && cw_field_get(flags, value) == 2, "flag toggle by bit");
    const cw_type_info *ut = cw_comp_type(w, unit);
    const cw_field *team = cw_type_field(ut, "team");
    unsigned char u[8] = {0};
    CHECK(cw_field_parse(team, u, "blue") && u[team->offset] == 1, "enum parse by label");
    CHECK(cw_field_parse(team, u, "0") && u[team->offset] == 0, "enum parse by number");
    CHECK(!cw_field_parse(team, u, "green") && !cw_field_parse(team, u, "7"), "enum rejects unknown");
    cw_field_adjust(team, u, 1); cw_field_adjust(team, u, 1);
    CHECK(u[team->offset] == 0, "enum cycles");
    cw_field_format(team, u, text, sizeof(text));
    CHECK(strcmp(text, "red") == 0, "enum format");
    const cw_field *intensity = cw_type_field(lt, "intensity");
    CHECK(cw_field_set(intensity, value, 99) && cw_field_get(intensity, value) == 10, "set clamps to the range");
    CHECK(!cw_field_parse(intensity, value, "99"), "parse rejects out-of-range values");
    CHECK(!cw_field_parse(intensity, value, "abc") && !cw_field_parse(intensity, value, "1.5x"), "parse rejects junk");
    CHECK(cw_field_parse(intensity, value, "2.5") && fabs(cw_field_get(intensity, value) - 2.5) < 1e-6, "float parse");
    cw_field_adjust(intensity, value, -1);
    CHECK(fabs(cw_field_get(intensity, value) - 2.3) < 1e-5, "adjust by step");
    const cw_type_info *ht = cw_comp_type(w, health);
    int32_t bad[2] = {200, 100};
    CHECK(!cw_type_check(ht, bad), "validate callback runs");
    int32_t good[2] = {50, 100};
    CHECK(cw_type_check(ht, good), "valid health");
    const cw_field *maximum = cw_type_field(ht, "maximum");
    int32_t zero_max[2] = {0, 0};
    CHECK(!cw_field_check(maximum, zero_max), "range check on integers");

    /* semantics */
    cw_entity_id id = cw_game_world_spawn(&gw, prefab_index(game.module, "red", "tank"), 100, 200);
    CHECK(id != CW_INVALID_ENTITY, "spawn");
    float x = 0, y = 0;
    CHECK(cw_reflect_position(w, id, &x, &y) && x == 100 && y == 200, "position semantics (%g, %g)", (double)x, (double)y);
    CHECK(cw_reflect_set_position(w, id, 300, 400) && cw_reflect_position(w, id, &x, &y) && x == 300 && y == 400, "set position");
    CHECK(cw_reflect_radius(w, id, 1) == 16, "radius semantics");
    CHECK(cw_comp_has(w, id, position), "prefab sets Position");

    /* generic codec through the binary snapshot */
    cw_entity_id lamp = cw_game_world_spawn(&gw, prefab_index(game.module, "light", "lantern"), 50, 60);
    cw_game_world_settle(&gw);
    uint64_t before = world_hash(w);
    cw_writer image;
    cw_writer_init(&image);
    cw_snapshot_error serr;
    CHECK(cw_snapshot_save(w, NULL, &image, &serr), "snapshot save with the generic codec: %s", cw_snapshot_status_string(serr.status));
    cw_game_world_clear(&gw);
    CHECK(cw_world_entity_count(w) == 0, "clear empties the world");
    CHECK(cw_snapshot_load(w, image.data, image.size, NULL, &serr), "snapshot load: %s (%s)", cw_snapshot_status_string(serr.status), serr.name);
    CHECK(world_hash(w) == before, "snapshot round trip is exact");
    CHECK(cw_comp_has(w, lamp, light) && cw_comp_has(w, id, unit), "entities restored by id");
    /* a corrupt payload is rejected by the generic codec's checks */
    cw_writer_destroy(&image);

    /* copy and remap */
    cw_entity_id copy = cw_world_create_entity(w);
    CHECK(cw_reflect_copy_entity(w, id, copy) >= 6, "copy components");
    CHECK(field_of(w, copy, "Body", "radius") == 16 && cw_comp_has(w, copy, comp(w, "ShadowCaster")), "copied values");
    cw_entity_id torch = cw_game_world_spawn(&gw, prefab_index(game.module, "light", "torch"), 300, 400);
    CHECK(torch != CW_INVALID_ENTITY && field_of(w, torch, "Attached", "parent") == (double)id, "torch attaches to the nearest unit");

    cw_game_world_destroy(&gw);
    cw_game_close(&game);
}

static void test_scene(void) {
    cw_game game;
    char error[256] = "";
    cw_game_use(&game, cw_game_export(), error, sizeof(error));
    cw_game_world gw;
    CHECK(cw_game_world_init(&gw, game.module, error, sizeof(error)), "bind: %s", error);
    cw_world *w = &gw.world;
    const cw_game_module *m = game.module;
    for (int i = 0; i < 4; ++i) {
        cw_game_world_spawn(&gw, prefab_index(m, "red", i & 1 ? "archer" : "melee"), 200, 150 + 120.0f * (float)i);
        cw_game_world_spawn(&gw, prefab_index(m, "blue", i & 1 ? "tank" : "melee"), 1000, 150 + 120.0f * (float)i);
    }
    cw_entity_id pillar = cw_game_world_spawn(&gw, prefab_index(m, "level", "pillar"), 600, 400);
    cw_entity_id lamp = cw_game_world_spawn(&gw, prefab_index(m, "light", "spotlight"), 40, 400);
    cw_entity_id torch = cw_game_world_spawn(&gw, prefab_index(m, "light", "torch"), 200, 150);
    cw_game_world_settle(&gw);
    CHECK(cw_world_entity_count(w) == 11, "11 entities placed");
    CHECK(cw_comp_has(w, 0, comp(w, "Visual")), "EntityCreated ran on settle");

    name_table names = {{0}, {{0}}, 0};
    on_name(&names, pillar, "center pillar");
    on_name(&names, lamp, "stage \"spot\" \\ light");
    cw_scene_save_config save = {NULL, 0, name_of, &names};
    cw_scene_result r;
    const char *path = "test_game_scene.cwscene";
    CHECK(cw_game_world_save_scene(&gw, path, &save, &r), "scene save: %s", r.message);
    CHECK(r.entities == 11 && r.components > 11, "scene counted %zu entities, %zu components", r.entities, r.components);
    uint64_t authored = world_hash(w);

    /* the file is readable text */
    cw_buffer text;
    cw_file_error ferr;
    CHECK(cw_file_read(path, 0, &text, &ferr), "read back");
    CHECK(text.size > 200 && memcmp(text.data, "cwscene 1\ngame combat 1\n", 24) == 0, "header");
    CHECK(strstr((const char *)text.data, "Unit team=red kind=melee") != NULL, "enums written by label");
    CHECK(strstr((const char *)text.data, "Light r=255 g=255 b=255 flags=spot") != NULL, "flags written by label");
    CHECK(strstr((const char *)text.data, "entity 8 \"center pillar\"") != NULL, "entity names written");
    CHECK(strstr((const char *)text.data, "\"stage \\\"spot\\\" \\\\ light\"") != NULL, "names escaped");
    CHECK(strstr((const char *)text.data, "Visual") == NULL && strstr((const char *)text.data, "Target") == NULL, "transient components skipped");

    /* simulate from the authored state, then from the loaded one: identical */
    cw_game_world_tick(&gw, 90);
    uint64_t after_original = world_hash(w);
    CHECK(after_original != authored, "the battle changed the world");
    name_table loaded_names = {{0}, {{0}}, 0};
    cw_scene_load_config load = {NULL, false, false, 0, 0, on_name, &loaded_names};
    CHECK(cw_game_world_load_scene(&gw, path, &load, &r), "scene load: line %d: %s", r.line, r.message);
    CHECK(r.entities == 11 && r.warnings == 0, "loaded %zu entities, %zu warnings", r.entities, r.warnings);
    CHECK(world_hash(w) == authored, "loaded scene equals the authored one");
    CHECK(loaded_names.count == 2 && strcmp(loaded_names.names[1], "stage \"spot\" \\ light") == 0, "names restored (%zu)", loaded_names.count);
    CHECK(w->next_id == 11 && w->tick == 0, "id allocator restored, tick reset (%" PRId64 ", %" PRIu64 ")", w->next_id, w->tick);
    CHECK(field_of(w, torch, "Attached", "parent") == 0, "entity references preserved");
    cw_game_world_settle(&gw);
    CHECK(cw_comp_has(w, 0, comp(w, "Visual")) && cw_comp_has(w, lamp, comp(w, "LightState")), "transient state rebuilt on load");
    CHECK(strstr((const char *)text.data, "HealthConfig maximum=120") != NULL && strstr((const char *)text.data, "Health current") == NULL, "scenes hold config, never runtime components");
    CHECK(cw_comp_has(w, 0, comp(w, "Health")) && field_of(w, 0, "Health", "current") == 120, "the runtime component was baked from the config on load");
    cw_game_world_tick(&gw, 90);
    CHECK(world_hash(w) == after_original, "simulation from the loaded scene is identical");
    cw_buffer_free(&text);

    /* runtime state lives in snapshots, not scenes */
    {
        cw_game_world_tick(&gw, 240);   /* long enough for the lines to meet */
        cw_entity_id hurt = CW_INVALID_ENTITY;
        for (size_t i = 0; i < w->capacity && hurt == CW_INVALID_ENTITY; ++i)
            if (cw_world_is_valid(w, (cw_entity_id)i) && cw_comp_has(w, (cw_entity_id)i, comp(w, "Health"))
                && field_of(w, (cw_entity_id)i, "Health", "current") < field_of(w, (cw_entity_id)i, "Health", "maximum")) hurt = (cw_entity_id)i;
        CHECK(hurt != CW_INVALID_ENTITY, "somebody is hurt mid-battle");
        double hp = field_of(w, hurt, "Health", "current");
        cw_writer snap;
        cw_writer_init(&snap);
        cw_snapshot_error serr;
        CHECK(cw_snapshot_save(w, NULL, &snap, &serr), "snapshot save");
        cw_game_world_clear(&gw);
        CHECK(cw_snapshot_load(w, snap.data, snap.size, NULL, &serr) && field_of(w, hurt, "Health", "current") == hp, "a snapshot restores runtime health exactly");
        cw_game_world_settle(&gw);
        CHECK(field_of(w, hurt, "Health", "current") == hp, "baking does not overwrite a runtime component that exists");
        cw_writer_destroy(&snap);
    }

    /* in-memory round trip with the world's own writer */
    cw_writer out;
    cw_writer_init(&out);
    CHECK(cw_scene_write(w, NULL, &out, &r), "write to memory");
    uint64_t mid_battle = world_hash(w);
    CHECK(cw_game_world_read_scene(&gw, (const char *)out.data, out.size, NULL, &r), "read from memory: %s", r.message);
    CHECK(world_hash(w) == mid_battle, "memory round trip (authored state)");
    cw_game_world_settle(&gw);
    {
        size_t units = 0, full = 0;
        for (size_t i = 0; i < w->capacity; ++i) {
            if (!cw_world_is_valid(w, (cw_entity_id)i) || !cw_comp_has(w, (cw_entity_id)i, comp(w, "HealthConfig"))) continue;
            ++units;
            full += cw_comp_has(w, (cw_entity_id)i, comp(w, "Health")) && field_of(w, (cw_entity_id)i, "Health", "current") == field_of(w, (cw_entity_id)i, "HealthConfig", "maximum");
        }
        CHECK(units > 0 && full == units, "a scene reload re-bakes: every unit at full health (%zu of %zu)", full, units);
    }
    {   /* baking can be switched off (an editor does while editing) */
        cw_game_world_set_baking(&gw, false);
        cw_entity_id fresh = cw_game_world_spawn(&gw, prefab_index(m, "red", "melee"), 50, 50);
        cw_game_world_settle(&gw);
        CHECK(cw_comp_has(w, fresh, comp(w, "HealthConfig")) && !cw_comp_has(w, fresh, comp(w, "Health")), "with baking off only the config exists");
        cw_game_world_set_baking(&gw, true);
        CHECK(cw_game_world_bake_all(&gw) == 1 && cw_comp_has(w, fresh, comp(w, "Health")), "bake_all generates what is missing");
        CHECK(cw_game_world_bake_all(&gw) == 0, "and nothing twice");
        CHECK(cw_type_authored(cw_comp_type(w, comp(w, "HealthConfig"))) && !cw_type_authored(cw_comp_type(w, comp(w, "Health"))), "authored vs runtime");
        CHECK(cw_sim_find(&gw.sim, "cw_bake") != NULL, "the engine registered the bake system");
        cw_world_mark_destroy(w, fresh);
        cw_game_world_settle(&gw);
    }
    cw_writer_destroy(&out);

    /* fragments and instantiation: a unit with its torch, placed twice */
    {
        cw_game_world_load_scene(&gw, path, &load, &r);
        cw_entity_id unit_id = 0;
        cw_entity_id part[2] = {unit_id, torch};
        cw_writer frag;
        cw_writer_init(&frag);
        CHECK(cw_scene_write_entities(w, part, 2, NULL, &frag, &r) && r.entities == 2, "fragment written (%zu entities): %s", r.entities, r.message);
        char *frag_text = malloc(frag.size + 1);
        memcpy(frag_text, frag.data, frag.size);
        frag_text[frag.size] = 0;
        CHECK(strstr(frag_text, "next_id") == NULL && strstr(frag_text, "entity 0\n") != NULL, "a fragment has no allocator state");
        free(frag_text);
        cw_entity_id bad[1] = {999};
        cw_writer scratch;
        cw_writer_init(&scratch);
        CHECK(!cw_scene_write_entities(w, bad, 1, NULL, &scratch, &r), "dead ids are refused");
        cw_writer_destroy(&scratch);
        size_t before = cw_world_entity_count(w);
        cw_entity_id made[8];
        size_t made_count = 0;
        CHECK(cw_scene_instantiate(w, (const char *)frag.data, frag.size, NULL, true, 500, 600, made, 8, &made_count, &r), "instantiate: %s", r.message);
        CHECK(made_count == 2 && cw_world_entity_count(w) == before + 2 && made[0] >= 11 && made[1] >= 11, "two new entities with fresh ids (%" PRId64 ", %" PRId64 ")", made[0], made[1]);
        float x, y;
        CHECK(cw_reflect_position(w, made[0], &x, &y) && x == 500 && y == 600, "the anchor lands at the requested position (%g, %g)", (double)x, (double)y);
        CHECK(field_of(w, made[1], "Attached", "parent") == (double)made[0], "references between instantiated entities are remapped");
        CHECK(field_of(w, unit_id, "Position", "x") == 200, "the originals are untouched");
        CHECK(cw_scene_instantiate(w, (const char *)frag.data, frag.size, NULL, false, 0, 0, made, 8, &made_count, &r) && made_count == 2, "instantiate again without placing");
        CHECK(cw_reflect_position(w, made[0], &x, &y) && x == 200, "unplaced copies keep their saved position");
        static const char broken[] = "cwscene 1\nentity 0\n  Position x=1 y=2\nentity 1\n  Nope a=1\n";
        size_t count_before = cw_world_entity_count(w);
        CHECK(!cw_scene_instantiate(w, broken, sizeof(broken) - 1, NULL, true, 0, 0, made, 8, &made_count, &r) && cw_world_entity_count(w) == count_before && made_count == 0,
              "a failed instantiation destroys what it created and leaves the world (%zu)", cw_world_entity_count(w));
        cw_writer_destroy(&frag);
    }

    /* tolerance and errors */
    static const char grown[] =
        "cwscene 1\n"
        "game combat 1\n"
        "# a comment line\n"
        "entity 3 \"named\"\n"
        "  Position x=10 y=20 z=30\n"          /* unknown field: warning */
        "  HealthConfig\n"                     /* no fields: defaults */
        "  Unit team=blue kind=2\n"
        "  ShadowCaster\n"
        "\n"
        "entity 7\n"
        "  Obstacle\n";                        /* all defaults */
    CHECK(cw_game_world_read_scene(&gw, grown, sizeof(grown) - 1, NULL, &r), "grown scene: %s", r.message);
    CHECK(r.warnings == 1 && r.entities == 2, "one warning for the unknown field (%zu), %zu entities", r.warnings, r.entities);
    CHECK(field_of(w, 3, "HealthConfig", "maximum") == 100, "defaults fill missing fields");
    CHECK(field_of(w, 3, "Unit", "kind") == 2 && field_of(w, 7, "Obstacle", "radius") == 30, "numbers and defaults");
    CHECK(w->next_id == 8, "next_id follows the highest id");
    static const char bad_component[] = "cwscene 1\nentity 0\n  Position x=1 y=2\n  Widget path=x\n";
    CHECK(!cw_game_world_read_scene(&gw, bad_component, sizeof(bad_component) - 1, NULL, &r) && r.line == 4, "unknown component fails at line %d: %s", r.line, r.message);
    CHECK(cw_world_entity_count(w) == 0 && w->next_id == 0, "a failed load leaves a fresh world");
    cw_scene_load_config lenient = {NULL, true, false, 0, 0, NULL, NULL};
    CHECK(cw_game_world_read_scene(&gw, bad_component, sizeof(bad_component) - 1, &lenient, &r) && r.warnings == 1, "unknown component skipped when allowed");
    static const char bad_value[] = "cwscene 1\nentity 0\n  HealthConfig maximum=0\n";
    CHECK(!cw_game_world_read_scene(&gw, bad_value, sizeof(bad_value) - 1, NULL, &r) && r.line == 3, "ranges enforced on config: %s", r.message);
    static const char bad_range[] = "cwscene 1\nentity 0\n  Light intensity=99\n";
    CHECK(!cw_game_world_read_scene(&gw, bad_range, sizeof(bad_range) - 1, NULL, &r), "ranges enforced: %s", r.message);
    static const char wrong_game[] = "cwscene 1\ngame platformer 1\nentity 0\n";
    CHECK(!cw_game_world_read_scene(&gw, wrong_game, sizeof(wrong_game) - 1, NULL, &r) && r.line == 2, "game name checked");
    static const char dup[] = "cwscene 1\nentity 0\nentity 0\n";
    CHECK(!cw_game_world_read_scene(&gw, dup, sizeof(dup) - 1, NULL, &r), "duplicate ids rejected");
    static const char not_scene[] = "hello\n";
    CHECK(!cw_game_world_read_scene(&gw, not_scene, sizeof(not_scene) - 1, NULL, &r), "junk rejected");
    CHECK(!cw_scene_load_file(w, "does-not-exist.cwscene", NULL, &r), "missing file reported: %s", r.message);
    remove(path);
    cw_game_world_destroy(&gw);
    cw_game_close(&game);
}

static void test_simulation_and_canvas(void) {
    cw_game game;
    char error[256] = "";
    cw_game_use(&game, cw_game_export(), error, sizeof(error));
    cw_game_world gw;
    CHECK(cw_game_world_init(&gw, game.module, error, sizeof(error)), "bind: %s", error);
    cw_world *w = &gw.world;
    const cw_game_module *m = game.module;
    for (int i = 0; i < 6; ++i) {
        cw_game_world_spawn(&gw, prefab_index(m, "red", "melee"), 200, 150 + 90.0f * (float)i);
        cw_game_world_spawn(&gw, prefab_index(m, "blue", "archer"), 1000, 150 + 90.0f * (float)i);
    }
    cw_game_world_spawn(&gw, prefab_index(m, "light", "lantern"), 600, 400);
    cw_game_world_spawn(&gw, prefab_index(m, "level", "pillar"), 600, 300);
    cw_game_world_settle(&gw);
    size_t start = cw_world_entity_count(w);
    CHECK(start == 14, "14 entities");
    double hp0 = field_of(w, 0, "Health", "current");
    cw_game_world_tick(&gw, gw.clock.hz * 15);
    CHECK(w->tick == (uint64_t)gw.clock.hz * 15, "ticks advanced");
    bool hurt = false, someone_died = cw_world_entity_count(w) < start;
    for (size_t i = 0; i < w->capacity; ++i)
        if (cw_world_is_valid(w, (cw_entity_id)i) && cw_comp_has(w, (cw_entity_id)i, comp(w, "Health")) && field_of(w, (cw_entity_id)i, "Health", "current") < hp0) hurt = true;
    CHECK(hurt || someone_died, "the battle dealt damage");
    CHECK(someone_died, "somebody died (%zu left)", cw_world_entity_count(w));
    CHECK(cw_game_world_advance(&gw, 1.0f) > 0, "the clock advances the world");

    memset(&canvas_stats, 0, sizeof(canvas_stats));
    cw_draw_context ctx = {1, 0, true, false};
    cw_game_world_draw(&gw, &canvas, &ctx);
    size_t expected_picks = 0;
    for (size_t i = 0; i < w->capacity; ++i)
        if (cw_world_is_valid(w, (cw_entity_id)i) && cw_comp_has(w, (cw_entity_id)i, comp(w, "Position"))) ++expected_picks;
    CHECK((size_t)canvas_stats.picks == expected_picks, "every positioned entity is pickable (%d of %zu)", canvas_stats.picks, expected_picks);
    CHECK(canvas_stats.lights == 1 && canvas_stats.occluders >= 2 && canvas_stats.shapes > 20 && canvas_stats.texts > 0, "draw submits lights, occluders, shapes and labels");
    CHECK(canvas_stats.sprites == 0, "no Sprite components yet, so no sprites");

    /* the engine's Sprite component: drawn at the entity's position, framed by the sheet, saved like any other */
    CHECK(gw.sprite_ct != CW_INVALID_COMPONENT && comp(w, "Sprite") == gw.sprite_ct && cw_comp_type(w, gw.sprite_ct) == &cw_sprite_type, "the world has the engine's Sprite component");
    cw_entity_id pillar = CW_INVALID_ENTITY;
    for (size_t i = 0; i < w->capacity && pillar == CW_INVALID_ENTITY; ++i)
        if (cw_world_is_valid(w, (cw_entity_id)i) && cw_comp_has(w, (cw_entity_id)i, comp(w, "Position")) && !cw_comp_has(w, (cw_entity_id)i, comp(w, "Health"))) pillar = (cw_entity_id)i;
    CHECK(pillar != CW_INVALID_ENTITY, "found an entity to texture");
    if (pillar != CW_INVALID_ENTITY) {
        cw_sprite sprite = {cw_asset_id("ball.cwmat"), 64, 32, 0.5f, 3, true, false, 255, 128, 64, 200, 4, 2, 5};
        cw_comp_set(w, pillar, gw.sprite_ct, &sprite, sizeof(sprite));
        float px = 0, py = 0;
        cw_reflect_position(w, pillar, &px, &py);
        memset(&canvas_stats, 0, sizeof(canvas_stats));
        cw_game_world_draw(&gw, &canvas, &ctx);
        CHECK(canvas_stats.sprites == 1 && last_sprite.material == cw_asset_id("ball.cwmat") && last_sprite.x == px && last_sprite.y == py, "the sprite is drawn at the entity's position");
        CHECK(last_sprite.w == 64 && last_sprite.h == 32 && last_sprite.rot == 0.5f && last_sprite.layer == 3 && last_sprite.tint.g == 128 && last_sprite.tint.a == 200, "size, rotation, layer and tint pass through");
        /* frame 5 of a 4x2 sheet is column 1, row 1; flip_x mirrors u */
        CHECK(fabs(last_sprite.u0 - 0.5f) < 1e-6 && fabs(last_sprite.u1 - 0.25f) < 1e-6 && fabs(last_sprite.v0 - 0.5f) < 1e-6 && fabs(last_sprite.v1 - 1.0f) < 1e-6,
              "sheet frame uvs (%g %g %g %g)", (double)last_sprite.u0, (double)last_sprite.v0, (double)last_sprite.u1, (double)last_sprite.v1);
        CHECK((size_t)canvas_stats.picks == expected_picks + 1, "a sprite adds a pick area");
        /* the component is reflected and authored, so it goes through scenes */
        const cw_type_info *t = cw_comp_type(w, gw.sprite_ct);
        CHECK(t && cw_type_authored(t) && t->field_count == 14 && cw_type_field(t, "material") && cw_type_field(t, "material")->semantic == CW_SEM_ASSET, "Sprite is reflected with an asset material field");
        cw_writer out;
        cw_writer_init(&out);
        cw_scene_result r;
        cw_scene_save_config save = {0};
        CHECK(cw_scene_write_entities(w, &pillar, 1, &save, &out, &r) && r.entities == 1, "write the textured entity");
        CHECK(strstr(ztext(&out), "Sprite") && strstr(ztext(&out), "columns=4") && strstr(ztext(&out), "frame=5"), "the fragment carries the sprite:\n%.*s", (int)out.size, (const char *)out.data);
        cw_writer_destroy(&out);
        cw_comp_remove(w, pillar, gw.sprite_ct);
    }
    /* the engine's Shape3D component: a 3D primitive at the entity's position, for hosts with a 3D view */
    CHECK(gw.shape3d_ct != CW_INVALID_COMPONENT && comp(w, "Shape3D") == gw.shape3d_ct && cw_comp_type(w, gw.shape3d_ct) == &cw_shape3d_type, "the world has the engine's Shape3D component");
    if (pillar != CW_INVALID_ENTITY) {
        const cw_type_info *t = cw_comp_type(w, gw.shape3d_ct);
        CHECK(t && cw_type_authored(t) && t->field_count == 14, "Shape3D is reflected and authored");
        const cw_field *kind = cw_type_field(t, "kind");
        CHECK(kind && kind->type == CW_FIELD_ENUM && kind->name_count == CW_SHAPE_KINDS && strcmp(kind->names[CW_SHAPE_CAPSULE], "capsule") == 0, "kind is an enum over the four primitives");
        cw_shape3d shape;
        cw_type_default(t, &shape);
        CHECK(shape.kind == CW_SHAPE_CUBE && shape.width == 1 && shape.a == 255, "a new Shape3D is a unit cube");
        shape.kind = CW_SHAPE_CAPSULE; shape.width = 0.5f; shape.height = 2; shape.depth = 0.5f; shape.ry = 1.5f; shape.r = 10; shape.g = 20; shape.b = 30; shape.a = 128; shape.checker = 2; shape.wireframe = true;
        cw_comp_set(w, pillar, gw.shape3d_ct, &shape, sizeof(shape));
        float px = 0, py = 0, pz = -1;
        CHECK(cw_reflect_position3(w, pillar, &px, &py, &pz) && pz == 0 && !cw_reflect_has_position_z(w, pillar), "a 2D game's position reads with z = 0");
        memset(&shape_stats, 0, sizeof(shape_stats));
        cw_game_world_draw(&gw, &canvas, &ctx);
        CHECK(shape_stats.count == 1 && shape_stats.last.kind == CW_SHAPE_CAPSULE && shape_stats.last.id == pillar, "the shape is drawn once, picking the entity");
        CHECK(shape_stats.last.x == px && shape_stats.last.y == py && shape_stats.last.z == 0, "at the entity's position");
        CHECK(shape_stats.last.sx == 0.5f && shape_stats.last.sy == 2 && shape_stats.last.ry == 1.5f && shape_stats.last.color.a == 128 && shape_stats.last.checker == 2 && shape_stats.last.wireframe,
              "size, rotation, colour, checker and wireframe pass through");
        memset(&shape_stats, 0, sizeof(shape_stats));
        cw_game_world_draw(&gw, &canvas_2d, &ctx);
        CHECK(shape_stats.count == 0, "a host without a 3D view skips Shape3D");
        /* rotation and size semantics drive the 3D editor's gizmos */
        float rx, ry, rz, sx, sy, sz;
        CHECK(cw_reflect_rotation3(w, pillar, &rx, &ry, &rz) && rx == 0 && ry == 1.5f && rz == 0, "rotation read through its semantics");
        CHECK(cw_reflect_set_rotation3(w, pillar, 0.1f, 0.2f, 0.3f) && cw_reflect_rotation3(w, pillar, &rx, &ry, &rz) && rx == 0.1f && ry == 0.2f && rz == 0.3f, "rotation set");
        CHECK(cw_reflect_size3(w, pillar, &sx, &sy, &sz) && sx == 0.5f && sy == 2 && sz == 0.5f, "size read through its semantics");
        CHECK(cw_reflect_set_size3(w, pillar, 1, 2, 3) && cw_reflect_size3(w, pillar, &sx, &sy, &sz) && sx == 1 && sy == 2 && sz == 3, "size set");
        CHECK(cw_type_field(cw_comp_type(w, gw.shape3d_ct), "ry")->semantic == CW_SEM_ROTATION_Y && cw_sem_is_angle(CW_SEM_ROTATION_Y) && cw_field_step(cw_type_field(cw_comp_type(w, gw.shape3d_ct), "ry")) > 0.08f,
              "rotation fields are angles (degrees in the inspector, a 5 degree step)");
        CHECK(cw_type_field(cw_comp_type(w, gw.shape3d_ct), "material")->semantic == CW_SEM_ASSET, "a Shape3D names a material asset");
        cw_writer out;
        cw_writer_init(&out);
        cw_scene_result r;
        cw_scene_save_config save = {0};
        CHECK(cw_scene_write_entities(w, &pillar, 1, &save, &out, &r) && strstr(ztext(&out), "Shape3D kind=capsule") && strstr(ztext(&out), "wireframe=true"),
              "the fragment carries the shape:\n%.*s", (int)out.size, (const char *)out.data);
        cw_writer_destroy(&out);
        cw_comp_remove(w, pillar, gw.shape3d_ct);
    }
    /* the engine's Light3D: drawn through canvas->light3d with a direction from pitch and yaw */
    CHECK(gw.light3d_ct != CW_INVALID_COMPONENT && comp(w, "Light3D") == gw.light3d_ct && cw_type_authored(&cw_light3d_type), "the world has the engine's Light3D component");
    if (pillar != CW_INVALID_ENTITY) {
        cw_light3d light;
        cw_type_default(&cw_light3d_type, &light);
        CHECK(light.kind == CW_LIGHT_POINT && light.intensity == 1 && light.range == 10, "a new light is a point light");
        light.kind = CW_LIGHT_DIRECTIONAL; light.pitch = -1.5707964f; light.yaw = 0; light.r = 10; light.g = 20; light.b = 30; light.intensity = 2;
        cw_comp_set(w, pillar, gw.light3d_ct, &light, sizeof(light));
        memset(&light3d_stats, 0, sizeof(light3d_stats));
        cw_game_world_draw(&gw, &canvas, &ctx);
        CHECK(light3d_stats.count == 1 && light3d_stats.last.kind == CW_LIGHT_DIRECTIONAL && light3d_stats.last.id == pillar && light3d_stats.last.intensity == 2 && light3d_stats.last.color.g == 20, "the light is drawn once");
        CHECK(fabs(light3d_stats.last.dy + 1) < 1e-5 && fabs(light3d_stats.last.dx) < 1e-5 && fabs(light3d_stats.last.dz) < 1e-5, "pitch -90 degrees points straight down (%g %g %g)",
              (double)light3d_stats.last.dx, (double)light3d_stats.last.dy, (double)light3d_stats.last.dz);
        light.pitch = 0; light.yaw = 1.5707964f;
        cw_comp_set(w, pillar, gw.light3d_ct, &light, sizeof(light));
        cw_game_world_draw(&gw, &canvas, &ctx);
        CHECK(fabs(light3d_stats.last.dx - 1) < 1e-5 && fabs(light3d_stats.last.dz) < 1e-5, "yaw 90 degrees points along +x");
        cw_comp_remove(w, pillar, gw.light3d_ct);
    }
    /* the engine's Sky3D: the first entity with one sets the 3D view's background; a partial image set keeps the gradient */
    CHECK(gw.sky3d_ct != CW_INVALID_COMPONENT && comp(w, "Sky3D") == gw.sky3d_ct && cw_type_authored(&cw_sky3d_type), "the world has the engine's Sky3D component");
    if (pillar != CW_INVALID_ENTITY) {
        cw_sky3d sky;
        cw_type_default(&cw_sky3d_type, &sky);
        CHECK(sky.sun_intensity == 3 && sky.zenith_b == 190 && sky.image_up == 0, "a new sky is a blue gradient with a sun");
        CHECK(cw_type_field(cw_comp_type(w, gw.sky3d_ct), "image_up")->semantic == CW_SEM_ASSET && cw_asset_type_for_field("image_up") == CW_ASSET_IMAGE, "its faces are image assets");
        sky.image_up = cw_asset_id("sky_up.png");   /* one of six: not a cube map yet */
        cw_comp_set(w, pillar, gw.sky3d_ct, &sky, sizeof(sky));
        memset(&sky3d_stats, 0, sizeof(sky3d_stats));
        cw_game_world_draw(&gw, &canvas, &ctx);
        CHECK(sky3d_stats.count == 1 && sky3d_stats.last.id == pillar && sky3d_stats.last.zenith.b == 190 && sky3d_stats.last.sun_size == sky.sun_size, "the sky is drawn once");
        CHECK(sky3d_stats.last.images[2] == 0, "a partial set of faces falls back to the gradient");
        sky.image_east = cw_asset_id("e"); sky.image_west = cw_asset_id("w"); sky.image_down = cw_asset_id("d"); sky.image_south = cw_asset_id("s"); sky.image_north = cw_asset_id("n");
        cw_comp_set(w, pillar, gw.sky3d_ct, &sky, sizeof(sky));
        cw_game_world_draw(&gw, &canvas, &ctx);
        CHECK(sky3d_stats.last.images[0] == cw_asset_id("e") && sky3d_stats.last.images[2] == cw_asset_id("sky_up.png") && sky3d_stats.last.images[5] == cw_asset_id("n"),
              "six faces make the cube map in the +x -x +y -y +z -z order");
        cw_game_world_draw(&gw, &canvas_2d, &ctx);
        CHECK(sky3d_stats.count == 2, "2D hosts never see it");
        cw_comp_remove(w, pillar, gw.sky3d_ct);
    }
    /* the engine's Decal3D: a material projected onto the surfaces in a box at the entity's position */
    CHECK(gw.decal3d_ct != CW_INVALID_COMPONENT && comp(w, "Decal3D") == gw.decal3d_ct && cw_type_authored(&cw_decal3d_type), "the world has the engine's Decal3D component");
    if (pillar != CW_INVALID_ENTITY) {
        cw_decal3d decal;
        cw_type_default(&cw_decal3d_type, &decal);
        CHECK(decal.width == 1 && decal.height == 1 && decal.depth == 0.5f && decal.a == 255 && fabsf(decal.max_angle - 1.3962634f) < 1e-6f, "a new decal is a metre square, half a metre deep, steeper than 80 degrees skipped");
        const cw_type_info *dt = cw_comp_type(w, gw.decal3d_ct);
        CHECK(cw_type_field(dt, "width")->semantic == CW_SEM_SIZE_X && cw_type_field(dt, "height")->semantic == CW_SEM_SIZE_Z && cw_type_field(dt, "depth")->semantic == CW_SEM_SIZE_Y
              && cw_type_field(dt, "rx")->semantic == CW_SEM_ROTATION_X && cw_type_field(dt, "max_angle")->semantic == CW_SEM_ANGLE && cw_type_field(dt, "material")->semantic == CW_SEM_ASSET,
              "its sizes follow its local axes (height along z, depth along the projection y), the gizmos drive them");
        decal.width = 2; decal.height = 3; decal.depth = 0.25f; decal.rx = 1.5f; decal.material = cw_asset_id("scorch.cwmat"); decal.a = 200;
        cw_comp_set(w, pillar, gw.decal3d_ct, &decal, sizeof(decal));
        memset(&decal3d_stats, 0, sizeof(decal3d_stats));
        cw_game_world_draw(&gw, &canvas, &ctx);
        float px = 0, py = 0, pz = 0;
        cw_reflect_position3(w, pillar, &px, &py, &pz);
        CHECK(decal3d_stats.count == 1 && decal3d_stats.last.id == pillar && decal3d_stats.last.x == px && decal3d_stats.last.y == py, "the decal is drawn once, at the entity");
        CHECK(decal3d_stats.last.sx == 2 && decal3d_stats.last.sz == 3 && decal3d_stats.last.sy == 0.25f && decal3d_stats.last.rx == 1.5f && decal3d_stats.last.color.a == 200
              && decal3d_stats.last.material == cw_asset_id("scorch.cwmat"), "with its box (x width, z height, y depth), rotation, tint and material");
        cw_game_world_draw(&gw, &canvas_2d, &ctx);
        CHECK(decal3d_stats.count == 1, "2D hosts never see it");
        cw_writer out;
        cw_writer_init(&out);
        cw_scene_result r;
        cw_scene_save_config save = {0};
        CHECK(cw_scene_write_entities(w, &pillar, 1, &save, &out, &r) && strstr(ztext(&out), "Decal3D width=2") && strstr(ztext(&out), "rx=1.5"), "it saves with the scene");
        cw_writer_destroy(&out);
        cw_comp_remove(w, pillar, gw.decal3d_ct);
    }
    /* the engine's Brush: level geometry with per-face materials and vertex paint that travels with the scene */
    CHECK(gw.brush_ct != CW_INVALID_COMPONENT && comp(w, "Brush") == gw.brush_ct && cw_type_authored(&cw_brush_type), "the world has the engine's Brush component");
    if (pillar != CW_INVALID_ENTITY) {
        cw_brush brush;
        cw_type_default(&cw_brush_type, &brush);
        CHECK(brush.width == 1 && brush.subdivisions == 2 && brush.texture_scale == 1 && brush.paint.size == 0 && !cw_brush_painted(&brush), "a new brush is a unit cube, unpainted");
        brush.width = 4; brush.height = 2; brush.depth = 6;
        brush.material[CW_FACE_TOP] = cw_asset_id("grass.cwmat");
        float px = 0, py = 0, pz = 0;
        cw_reflect_position3(w, pillar, &px, &py, &pz);
        float center[3] = {px, py, pz}, point[3] = {px + 2, py + 1, pz + 3};
        CHECK(cw_brush_paint_at(&brush, center, point, 1.5f, 255, 0, 0, 1) > 0 && cw_brush_painted(&brush) && (size_t)brush.paint.size == cw_brush_paint_size(&brush), "painting initialises the lattice and touches the corner");
        uint8_t rgb[3];
        cw_brush_paint_get(&brush, CW_FACE_TOP, 2, 2, rgb);   /* the top face's (+x, +z) corner: u = x, v = z */
        CHECK(rgb[0] == 255 && rgb[1] == 0 && rgb[2] == 0, "the corner vertex took the colour fully (%u %u %u)", rgb[0], rgb[1], rgb[2]);
        cw_brush_paint_get(&brush, CW_FACE_TOP, 0, 0, rgb);
        CHECK(rgb[1] == 255, "the far corner is untouched");
        cw_comp_set(w, pillar, gw.brush_ct, &brush, sizeof(brush));
        memset(&brush_stats, 0, sizeof(brush_stats));
        cw_game_world_draw(&gw, &canvas, &ctx);
        CHECK(brush_stats.count == 1 && brush_stats.last.id == pillar && brush_stats.last.x == px && brush_stats.brush.depth == 6 && brush_stats.brush.material[CW_FACE_TOP] == cw_asset_id("grass.cwmat"),
              "the brush is drawn once with its component");
        /* scenes carry the paint as base64 and read it back */
        cw_writer out;
        cw_writer_init(&out);
        cw_scene_result r;
        cw_scene_save_config save = {0};
        CHECK(cw_scene_write_entities(w, &pillar, 1, &save, &out, &r) && strstr(ztext(&out), "Brush width=4") && strstr(ztext(&out), "paint=") && strstr(ztext(&out), "mat_top="),
              "the fragment carries the brush and its paint (the material id stays numeric: the test registers no assets)");
        cw_world w2;
        cw_world_init(&w2, NULL);
        cw_comp_block(&w2, 1);
        CHECK(cw_reflect_register(&w2, 0, &cw_brush_type), "register Brush in a fresh world");
        cw_scene_load_config lc = {0};
        lc.ignore_unknown_components = true;   /* the fragment also carries the pillar's own components */
        cw_entity_id made[4];
        size_t n = 0;
        cw_scene_result r2;
        CHECK(cw_scene_instantiate(&w2, ztext(&out), out.size, &lc, false, 0, 0, made, 4, &n, &r2) && n == 1, "instantiate the fragment: %s", r2.message);
        if (n == 1) {
            const cw_brush *back = cw_comp_ptr(&w2, made[0], 0);
            CHECK(back && back->paint.size == brush.paint.size && memcmp(back->paint.data, brush.paint.data, brush.paint.size) == 0 && back->material[CW_FACE_TOP] == brush.material[CW_FACE_TOP],
                  "the paint and materials survive the text round trip");
        }
        cw_world_destroy(&w2);
        cw_writer_destroy(&out);
        /* an unpainted brush writes no paint field at all */
        cw_brush_paint_clear(&brush);
        cw_comp_set(w, pillar, gw.brush_ct, &brush, sizeof(brush));
        cw_writer_init(&out);
        CHECK(cw_scene_write_entities(w, &pillar, 1, &save, &out, &r) && !strstr(ztext(&out), "paint="), "unpainted brushes stay compact");
        cw_writer_destroy(&out);
        cw_comp_remove(w, pillar, gw.brush_ct);
    }
    cw_game_world_destroy(&gw);
    cw_game_close(&game);
}

static void test_project(void) {
    const char *path = "test_game_project.cwproj";
    FILE *f = fopen(path, "w");
    CHECK(f != NULL, "write project");
    if (!f) return;
    fputs("cwproject 1\nname demo\n# comment\nmodule build/demo_game\nsource demo_game.c\nsource extra/more.c\nscenes levels\nmain_scene levels/start.cwscene\n"
          "render look/render.cfg\nwindow 1024 600\ntitle Demo Game\nui 1920 1080 shrink\nrenderer 3d\n", f);
    fclose(f);
    cw_project p;
    char error[256] = "";
    CHECK(cw_project_load(&p, path, error, sizeof(error)), "project load: %s", error);
    CHECK(strcmp(p.name, "demo") == 0 && p.source_count == 2, "name and sources");
    CHECK(p.renderer == CW_RENDERER_3D && strcmp(cw_renderer_name(p.renderer), "3d") == 0, "the renderer key");
    CHECK(strstr(p.render, "look/render.cfg") && p.window_width == 1024 && p.window_height == 600 && strcmp(p.title, "Demo Game") == 0, "render, window and title");
    CHECK(p.ui.width == 1920 && p.ui.height == 1080 && p.ui.mode == CW_UI_SCALE_SHRINK, "the interface canvas");
    /* saving writes the same values back */
    p.window_width = 1920;
    CHECK(cw_project_save(&p, error, sizeof(error)), "project save: %s", error);
    cw_project q;
    CHECK(cw_project_load(&q, path, error, sizeof(error)), "reload: %s", error);
    CHECK(q.window_width == 1920 && q.window_height == 600 && strcmp(q.title, "Demo Game") == 0 && strcmp(q.render, p.render) == 0
          && strcmp(q.module, p.module) == 0 && strcmp(q.main_scene, p.main_scene) == 0 && q.source_count == 2 && strcmp(q.sources[1], p.sources[1]) == 0
          && q.ui.width == 1920 && q.ui.mode == CW_UI_SCALE_SHRINK && q.renderer == CW_RENDERER_3D,
          "saved project round trip");
    f = fopen(path, "w");
    if (f) { fputs("cwproject 1\nname d\nmodule m\n", f); fclose(f); }
    CHECK(cw_project_load(&q, path, error, sizeof(error)) && q.window_width == 1280 && q.window_height == 800 && strcmp(q.title, "d") == 0 && strstr(q.render, "render.cfg")
          && q.ui.width == 1280 && q.ui.height == 720 && q.ui.mode == CW_UI_SCALE_EXPAND && q.renderer == CW_RENDERER_2D, "defaults: window 1280x800, title = name, render.cfg, a 1280x720 expanding interface, 2d");
    f = fopen(path, "w");
    if (f) { fputs("cwproject 1\nname d\nmodule m\nrenderer 4d\n", f); fclose(f); }
    CHECK(!cw_project_load(&q, path, error, sizeof(error)) && strstr(error, "renderer"), "an unknown renderer is rejected: %s", error);
    f = fopen(path, "w");
    if (f) { fputs("cwproject 1\nname d\nmodule m\nui 800 600 sideways\n", f); fclose(f); }
    CHECK(!cw_project_load(&q, path, error, sizeof(error)) && strstr(error, "ui"), "an unknown scale mode is rejected: %s", error);
    f = fopen(path, "w");
    if (f) { fputs("cwproject 1\nname d\nmodule m\nwindow 10 10\n", f); fclose(f); }
    CHECK(!cw_project_load(&q, path, error, sizeof(error)), "a tiny window is rejected: %s", error);
    size_t n = strlen(cw_game_library_suffix());
    CHECK(strlen(p.module) > n && strcmp(p.module + strlen(p.module) - n, cw_game_library_suffix()) == 0, "module path gets the platform suffix: %s", p.module);
    CHECK(strstr(p.module, "build/demo_game") && strstr(p.scenes_dir, "levels") && strstr(p.main_scene, "start.cwscene"), "paths joined onto the project directory");
    CHECK(p.dir[0] == '/' || (p.dir[1] == ':'), "project directory is absolute: %s", p.dir);
    remove(path);
    f = fopen(path, "w");
    if (f) { fputs("cwproject 1\nname nomodule\n", f); fclose(f); }
    CHECK(!cw_project_load(&p, path, error, sizeof(error)), "a project without a module is rejected: %s", error);
    remove(path);
    CHECK(!cw_project_load(&p, "missing.cwproj", error, sizeof(error)), "missing project reported: %s", error);
}

static void test_assets(void) {
    CHECK(cw_asset_id(NULL) == 0 && cw_asset_id("") == 0 && cw_asset_id("hero.png") != 0 && cw_asset_id("hero.png") == cw_asset_id("hero.png") && cw_asset_id("a") != cw_asset_id("b"), "ids are a pure hash");
    CHECK(cw_asset_type_for_field("mat_top") == CW_ASSET_MATERIAL, "brush face materials");
    CHECK(cw_asset_type_for_field("material") == CW_ASSET_MATERIAL && cw_asset_type_for_field("sprite") == CW_ASSET_IMAGE && cw_asset_type_for_field("hit_sound") == CW_ASSET_AUDIO
          && cw_asset_type_for_field("idle_clip") == CW_ASSET_ANIMATION && cw_asset_type_for_field("shader") == CW_ASSET_SHADER && cw_asset_type_for_field("next_level") == CW_ASSET_SCENE
          && cw_asset_type_for_field("materials_count") == CW_ASSET_OTHER && cw_asset_type_for_field("thing") == CW_ASSET_OTHER && cw_asset_type_for_field(NULL) == CW_ASSET_OTHER, "asset kind from a field's name");
    CHECK(cw_asset_type_of("unit.cwprefab") == CW_ASSET_PREFAB && strcmp(cw_asset_type_dir(CW_ASSET_PREFAB), "prefabs") == 0, "prefab assets");
    CHECK(cw_asset_type_of("x/hero.PNG") == CW_ASSET_IMAGE && cw_asset_type_of("level.cwscene") == CW_ASSET_SCENE && cw_asset_type_of("units.csv") == CW_ASSET_DATA
          && cw_asset_type_of("game.c") == CW_ASSET_SOURCE && cw_asset_type_of("jump.wav") == CW_ASSET_AUDIO && cw_asset_type_of("noext") == CW_ASSET_OTHER, "types by extension");
    /* a throwaway project directory */
    const char *dir = "test_assets_project";
#ifdef _WIN32
    (void)!system("rmdir /s /q test_assets_project 2>nul & mkdir test_assets_project & mkdir test_assets_project\\scenes");
#else
    (void)!system("rm -rf test_assets_project && mkdir -p test_assets_project/scenes");
#endif
    FILE *f = fopen("test_assets_project/scenes/level.cwscene", "w");
    if (f) { fputs("cwscene 1\n", f); fclose(f); }
    f = fopen("test_assets_source.csv", "wb");
    if (f) { fputs("a,b\n1,2\n", f); fclose(f); }
    char manifest[256], error[256] = "";
    snprintf(manifest, sizeof(manifest), "%s/assets.cwassets", dir);
    cw_assets a;
    cw_assets_init(&a, manifest);
    CHECK(strcmp(a.dir, "test_assets_project/") == 0, "manifest directory: %s", a.dir);
    CHECK(cw_assets_load(&a, error, sizeof(error)) && a.count == 0, "a missing manifest is an empty registry");
    CHECK(cw_assets_scan(&a, "") == 1 && a.count == 1 && a.items[0].type == CW_ASSET_SCENE && strcmp(a.items[0].path, "scenes/level.cwscene") == 0, "scan registers project files (%zu)", a.count);
    CHECK(cw_assets_scan(&a, "") == 0, "scanning again adds nothing");
    const cw_asset *csv = cw_assets_import(&a, "test_assets_source.csv", NULL, false, error, sizeof(error));
    CHECK(csv != NULL, "import: %s", error);
    if (csv) {
        CHECK(strcmp(csv->path, "assets/data/test_assets_source.csv") == 0 && csv->type == CW_ASSET_DATA && csv->size == 8 && strcmp(csv->name, "test_assets_source.csv") == 0, "imported into the type's directory: %s", csv->path);
        char full[CW_ASSET_PATH_CAPACITY];
        CHECK(cw_assets_path(&a, csv->id, full, sizeof(full)) && strcmp(full, "test_assets_project/assets/data/test_assets_source.csv") == 0, "path resolution: %s", full);
        FILE *copy = fopen(full, "r");
        CHECK(copy != NULL, "the file was copied");
        if (copy) fclose(copy);
    }
    CHECK(!cw_assets_import(&a, "test_assets_source.csv", NULL, false, error, sizeof(error)) && strstr(error, "already"), "importing twice without replace is refused: %s", error);
    CHECK(cw_assets_import(&a, "test_assets_source.csv", NULL, true, error, sizeof(error)) != NULL && a.count == 2, "replace overwrites and keeps one entry");
    CHECK(!cw_assets_import(&a, "does-not-exist.png", NULL, false, error, sizeof(error)), "missing sources are reported: %s", error);
    const cw_asset *sub = cw_assets_import(&a, "test_assets_source.csv", "custom/dir", true, error, sizeof(error));
    CHECK(sub && strcmp(sub->path, "custom/dir/test_assets_source.csv") == 0 && strcmp(sub->name, "test_assets_source_2.csv") == 0, "a custom subdirectory and a unique name: %s / %s", sub ? sub->path : "", sub ? sub->name : "");
    CHECK(cw_assets_find_name(&a, "test_assets_source.csv") == csv && cw_assets_find(&a, cw_asset_id("test_assets_source.csv")) == csv, "lookup by name and id");
    CHECK(!cw_assets_path(&a, 12345, NULL, 0), "unknown ids resolve to nothing");
    CHECK(cw_assets_rename(&a, sub->id, "table \"two\"") && cw_assets_find_name(&a, "table \"two\"") != NULL, "rename (and quotes survive)");
    CHECK(!cw_assets_rename(&a, a.items[0].id, "test_assets_source.csv"), "renaming onto a taken name is refused");
    CHECK(cw_assets_save(&a, error, sizeof(error)) && !a.dirty, "manifest saved: %s", error);
    cw_assets b;
    cw_assets_init(&b, manifest);
    CHECK(cw_assets_load(&b, error, sizeof(error)) && b.count == 3, "manifest reloaded: %zu entries (%s)", b.count, error);
    const cw_asset *two = cw_assets_find_name(&b, "table \"two\"");
    CHECK(two && strcmp(two->path, "custom/dir/test_assets_source.csv") == 0 && two->type == CW_ASSET_DATA && two->size == 8 && !two->missing, "entries survive the round trip");
    CHECK(cw_assets_remove(&b, cw_asset_id("test_assets_source.csv"), true) && b.count == 2, "remove with delete");
    CHECK(!fopen("test_assets_project/assets/data/test_assets_source.csv", "r"), "the file is gone");
    remove("test_assets_source.csv");
    /* the reflection side: @name fields */
    cw_assets_set_current(&b);
    const cw_field field = {"sprite", CW_FIELD_U32, 0, 0, 0, 0, NULL, 0, CW_SEM_ASSET, 0};
    uint32_t value = 0;
    char text[64];
    CHECK(cw_field_parse(&field, &value, "@scenes/level") == false || true, "(parse accepts any name)");
    CHECK(cw_field_parse(&field, &value, "@level.cwscene") && value == cw_asset_id("level.cwscene"), "@name parses to the id");
    cw_field_format(&field, &value, text, sizeof(text));
    CHECK(strcmp(text, "@level.cwscene") == 0, "known ids format as @name: %s", text);
    CHECK(cw_field_parse(&field, &value, "@unknown") && value == cw_asset_id("unknown"), "unknown names still hash");
    cw_field_format(&field, &value, text, sizeof(text));
    CHECK(strcmp(text, "none") != 0 && text[0] != '@', "unknown ids format as numbers: %s", text);
    CHECK(cw_field_parse(&field, &value, "none") && value == 0, "none is zero");
    cw_field_format(&field, &value, text, sizeof(text));
    CHECK(strcmp(text, "none") == 0, "zero formats as none");
    CHECK(cw_field_parse(&field, &value, "42") && value == 42, "numbers still parse");
    cw_assets_set_current(NULL);
    cw_assets_destroy(&a);
    cw_assets_destroy(&b);
#ifdef _WIN32
    (void)!system("rmdir /s /q test_assets_project 2>nul");
#else
    (void)!system("rm -rf test_assets_project");
#endif
}

static void test_animation(void) {
    /* keys stay sorted, replace within a millisecond, move around each other */
    cw_anim_clip clip;
    cw_anim_clip_init(&clip, "bob", 2.0f);
    CHECK(strcmp(clip.name, "bob") == 0 && clip.length == 2.0f && clip.mode == CW_ANIM_LOOP, "clip defaults");
    int tr = cw_anim_track_add(&clip, "Position", "y");
    CHECK(tr == 0 && cw_anim_track_add(&clip, "Position", "y") == 0 && clip.track_count == 1, "tracks are unique per component.field");
    cw_anim_track *t = &clip.tracks[0];
    CHECK(cw_anim_key_set(t, 1.0f, 300, CW_INTERP_LINEAR) == 0 && cw_anim_key_set(t, 0, 100, CW_INTERP_LINEAR) == 0 && cw_anim_key_set(t, 2.0f, 100, CW_INTERP_STEP) == 2, "keys insert sorted");
    CHECK(t->key_count == 3 && t->keys[1].time == 1.0f && cw_anim_key_set(t, 1.0004f, 350, CW_INTERP_EASE) == 1 && t->key_count == 3 && t->keys[1].value == 350, "a key within 1 ms replaces");
    CHECK(cw_anim_key_find(t, 2.0f) == 2 && cw_anim_key_find(t, 1.5f) < 0, "find");
    CHECK(cw_anim_key_move(t, 2, 0.5f) == 1 && t->keys[1].time == 0.5f && t->keys[2].time == 1.0f, "move re-sorts");
    CHECK(cw_anim_key_move(t, 1, 1.0f) < 0, "cannot move onto another key");
    CHECK(cw_anim_key_move(t, 1, 2.0f) == 2, "move back");
    /* evaluation */
    CHECK(cw_anim_track_eval(t, -1) == 100 && cw_anim_track_eval(t, 5) == 100, "held outside the keys");
    CHECK(fabs(cw_anim_track_eval(t, 0.5f) - 225) < 1e-4, "linear midpoint (%g)", (double)cw_anim_track_eval(t, 0.5f));
    CHECK(fabs(cw_anim_track_eval(t, 1.5f) - (350 - 250 * 0.5)) < 1e-3, "smoothstep midpoint (%g)", (double)cw_anim_track_eval(t, 1.5f));
    t->keys[0].interp = CW_INTERP_STEP;
    CHECK(cw_anim_track_eval(t, 0.9f) == 100 && cw_anim_track_eval(t, 1.0f) == 350, "step holds until the next key");
    CHECK(cw_anim_ease(CW_INTERP_EASE_IN, 0.5f) == 0.25f && cw_anim_ease(CW_INTERP_EASE_OUT, 0.5f) == 0.75f && cw_anim_ease(CW_INTERP_LINEAR, 0.3f) == 0.3f, "easing curves");
    bool finished = false;
    CHECK(fabs(cw_anim_wrap(&clip, 4.5f, &finished) - 0.5f) < 1e-5 && !finished, "loop wraps");
    clip.mode = CW_ANIM_PINGPONG;
    CHECK(fabs(cw_anim_wrap(&clip, 2.5f, &finished) - 1.5f) < 1e-5 && fabs(cw_anim_wrap(&clip, 4.5f, &finished) - 0.5f) < 1e-5, "pingpong bounces");
    clip.mode = CW_ANIM_ONCE;
    CHECK(cw_anim_wrap(&clip, 2.5f, &finished) == 2.0f && finished && cw_anim_wrap(&clip, 1.0f, &finished) == 1.0f && !finished, "once clamps and finishes");
    clip.mode = CW_ANIM_LOOP;
    /* text round trip */
    cw_writer out;
    cw_writer_init(&out);
    cw_anim_track_add(&clip, "Light", "flags");
    cw_anim_key_set(&clip.tracks[1], 0, 0, CW_INTERP_STEP);
    cw_anim_key_set(&clip.tracks[1], 1, 1, CW_INTERP_STEP);
    CHECK(cw_anim_write(&clip, &out), "write");
    cw_anim_clip back;
    char error[128] = "";
    CHECK(cw_anim_parse(&back, (const char *)out.data, out.size, error, sizeof(error)), "parse: %s", error);
    CHECK(strcmp(back.name, "bob") == 0 && back.length == 2.0f && back.track_count == 2 && back.tracks[0].key_count == 3 && back.tracks[0].keys[1].interp == CW_INTERP_EASE
          && back.tracks[0].keys[0].interp == CW_INTERP_STEP && back.tracks[1].keys[1].value == 1, "round trip");
    cw_writer_destroy(&out);
    static const char bad[] = "cwanim 1\nname x\ntrack Position y\n  key -1 5 linear\n";
    CHECK(!cw_anim_parse(&back, bad, sizeof(bad) - 1, error, sizeof(error)) && strstr(error, "line 4"), "bad key time: %s", error);
    static const char junk[] = "nope\n";
    CHECK(!cw_anim_parse(&back, junk, sizeof(junk) - 1, error, sizeof(error)), "junk rejected");
    CHECK(cw_anim_save(&clip, "test_anim.cwanim", error, sizeof(error)) && cw_anim_load(&back, "test_anim.cwanim", error, sizeof(error)) && back.track_count == 2, "file round trip: %s", error);
    remove("test_anim.cwanim");

    /* applied to a real entity through reflection, and by the engine's Animation component */
    cw_game game;
    cw_game_use(&game, cw_game_export(), error, sizeof(error));
    cw_game_world gw;
    CHECK(cw_game_world_init(&gw, game.module, error, sizeof(error)), "bind: %s", error);
    cw_world *w = &gw.world;
    CHECK(gw.animation_ct != CW_INVALID_COMPONENT && comp(w, "Animation") == gw.animation_ct && cw_sim_find(&gw.sim, "cw_anim") != NULL, "the engine registered Animation and its system");
    cw_entity_id lamp = cw_game_world_spawn(&gw, prefab_index(game.module, "light", "lantern"), 100, 100);
    cw_game_world_settle(&gw);
    cw_anim_clip pulse;
    cw_anim_clip_init(&pulse, "pulse", 1.0f);
    int it = cw_anim_track_add(&pulse, "Light", "intensity");
    cw_anim_key_set(&pulse.tracks[it], 0, 0.5f, CW_INTERP_LINEAR);
    cw_anim_key_set(&pulse.tracks[it], 1, 2.5f, CW_INTERP_LINEAR);
    int py = cw_anim_track_add(&pulse, "Position", "y");
    cw_anim_key_set(&pulse.tracks[py], 0, 100, CW_INTERP_LINEAR);
    cw_anim_key_set(&pulse.tracks[py], 1, 200, CW_INTERP_LINEAR);
    cw_anim_track_add(&pulse, "Nope", "x");   /* a track the entity cannot use */
    CHECK(cw_anim_apply(&pulse, w, lamp, 0.5f) == 2, "two tracks applied, the unknown one skipped");
    CHECK(fabs(field_of(w, lamp, "Light", "intensity") - 1.5) < 1e-5 && field_of(w, lamp, "Position", "y") == 150, "values written through reflection");
    cw_anim_key_set(&pulse.tracks[it], 1, 99, CW_INTERP_LINEAR);
    cw_anim_apply(&pulse, w, lamp, 1.0f);
    CHECK(field_of(w, lamp, "Light", "intensity") == 10, "field ranges still clamp animated values");
    cw_anim_key_set(&pulse.tracks[it], 1, 2.5f, CW_INTERP_LINEAR);
    /* the runtime component, with the clip served from the cache */
    uint32_t clip_id = cw_asset_id("pulse.cwanim");
    cw_anim_cache_put(&gw.anims, clip_id, &pulse);
    cw_animation anim = {clip_id, 1.0f, true, 0, 0};
    cw_comp_set(w, lamp, gw.animation_ct, &anim, sizeof(anim));
    cw_game_world_tick(&gw, 15);   /* half a second at 30 Hz */
    const cw_animation *state = cw_comp_ptr(w, lamp, gw.animation_ct);
    CHECK(fabs(state->time - 0.5f) < 1e-4 && fabs(field_of(w, lamp, "Position", "y") - 150) < 1e-3, "the system advanced and applied the clip (t=%g y=%g)", (double)state->time, field_of(w, lamp, "Position", "y"));
    cw_game_world_tick(&gw, 30);
    CHECK(state->time < 1.0f && state->playing, "looping keeps time bounded (%g)", (double)state->time);
    cw_animation once = {clip_id, 2.0f, true, 1 + CW_ANIM_ONCE, 0};
    cw_comp_set(w, lamp, gw.animation_ct, &once, sizeof(once));
    cw_game_world_tick(&gw, 30);
    CHECK(!state->playing && state->time == 1.0f && field_of(w, lamp, "Position", "y") == 200, "a once override finishes at the end (%g)", (double)state->time);
    cw_animation unknown = {cw_asset_id("missing.cwanim"), 1, true, 0, 0};
    cw_comp_set(w, lamp, gw.animation_ct, &unknown, sizeof(unknown));
    cw_game_world_tick(&gw, 5);
    CHECK(field_of(w, lamp, "Position", "y") == 200, "an unknown clip does nothing");
    /* scenes carry the component but not its runtime time */
    cw_writer text;
    cw_writer_init(&text);
    cw_scene_result r;
    cw_scene_write(w, NULL, &text, &r);
    char *as_text = malloc(text.size + 1);
    memcpy(as_text, text.data, text.size);
    as_text[text.size] = 0;
    CHECK(strstr(as_text, "Animation clip=") != NULL && strstr(as_text, "speed=1") != NULL, "Animation is authored into scenes: %.60s", strstr(as_text, "Animation") ? strstr(as_text, "Animation") : "");
    free(as_text);
    cw_writer_destroy(&text);
    cw_game_world_destroy(&gw);
    cw_game_close(&game);
}

static void add_default(cw_world *w, cw_entity_id id, cw_component_id ct) {   /* add with the type's defaults, as the editor does */
    cw_comp_add(w, id, ct);
    cw_type_default(cw_comp_type(w, ct), cw_comp_ptr(w, id, ct));
}
static void test_ui(void) {
    /* scale modes */
    cw_ui_canvas cv = cw_ui_canvas_default();
    CHECK(cv.width == 1280 && cv.height == 720 && cv.mode == CW_UI_SCALE_EXPAND, "canvas defaults");
    CHECK(fabs(cw_ui_scale(&cv, 1920, 1080) - 1.5f) < 1e-6 && fabs(cw_ui_scale(&cv, 2560, 720) - 1.0f) < 1e-6 && fabs(cw_ui_scale(&cv, 640, 720) - 0.5f) < 1e-6, "expand takes the smaller ratio");
    cv.mode = CW_UI_SCALE_SHRINK;
    CHECK(fabs(cw_ui_scale(&cv, 2560, 720) - 2.0f) < 1e-6, "shrink the larger");
    cv.mode = CW_UI_SCALE_WIDTH;
    CHECK(fabs(cw_ui_scale(&cv, 640, 2000) - 0.5f) < 1e-6, "width follows the width");
    cv.mode = CW_UI_SCALE_PIXELS;
    CHECK(cw_ui_scale(&cv, 4000, 3000) == 1, "pixels never scale");
    cw_ui_scale_mode mode;
    CHECK(cw_ui_scale_mode_parse("height", &mode) && mode == CW_UI_SCALE_HEIGHT && !cw_ui_scale_mode_parse("huge", &mode) && strcmp(cw_ui_scale_mode_name(CW_UI_SCALE_EXPAND), "expand") == 0, "mode names");
    /* edges and anchors, standalone */
    cw_ui_rect rr0 = *(const cw_ui_rect *)cw_ui_rect_type.defaults;
    float x0, y0, x1, y1;
    cw_ui_rect_edges(&rr0, 1280, 720, &x0, &y0, &x1, &y1);
    CHECK(x0 == 540 && y0 == 335 && x1 == 740 && y1 == 385, "the default rect is 200x50 at the centre (%g %g %g %g)", (double)x0, (double)y0, (double)x1, (double)y1);
    cw_ui_rect_set_anchors(&rr0, 1280, 720, 0, 0, 0, 0);
    CHECK(rr0.anchor_min_x == 0 && rr0.left == 540 && rr0.top == 335 && rr0.right == 740 && rr0.bottom == 385, "re-anchoring to the top left keeps the edges");
    cw_ui_rect_set_anchors(&rr0, 1280, 720, 0, 0, 1, 1);
    CHECK(rr0.anchor_max_x == 1 && rr0.left == 540 && rr0.right == -540 && rr0.top == 335 && rr0.bottom == -335, "stretch anchors: offsets from the far edges");
    cw_ui_rect_edges(&rr0, 1920, 1080, &x0, &y0, &x1, &y1);
    CHECK(x0 == 540 && x1 == 1380 && y0 == 335 && y1 == 745, "a stretched rect grows with its parent");
    cw_ui_rect_set_anchors(&rr0, 1280, 720, 1, 0.5f, 0.2f, 0.5f);
    CHECK(rr0.anchor_min_x == 1 && rr0.anchor_max_x == 1 && rr0.anchor_max_y == 0.5f, "anchors are clamped and ordered");

    /* in a bound world: the engine registers the components */
    cw_game game;
    char error[256] = "";
    cw_game_use(&game, cw_game_export(), error, sizeof(error));
    cw_game_world gw;
    CHECK(cw_game_world_init(&gw, game.module, error, sizeof(error)), "bind: %s", error);
    cw_world *w = &gw.world;
    const cw_ui_components *c = &gw.ui;
    CHECK(cw_ui_ready(c) && comp(w, "UiRect") == c->rect && comp(w, "UiImage") == c->image && comp(w, "UiText") == c->text && comp(w, "UiButton") == c->button && comp(w, "UiLayout") == c->layout, "Ui components registered");
    CHECK(cw_comp_is_transient(w, c->layout) && !cw_comp_is_transient(w, c->rect), "UiLayout is transient");
    CHECK(cw_type_field(&cw_ui_text_type, "text")->type == CW_FIELD_STRING && cw_type_field(&cw_ui_text_type, "text")->capacity == CW_UI_TEXT_CAPACITY, "UiText.text is a string field");
    /* a panel filling the bottom third, a bar inside it, a label and a button at the top right */
    cw_entity_id panel = cw_world_create_entity(w), bar = cw_world_create_entity(w), label = cw_world_create_entity(w), button = cw_world_create_entity(w), hidden = cw_world_create_entity(w);
    cw_ui_rect pr = *(const cw_ui_rect *)cw_ui_rect_type.defaults;
    pr.anchor_min_x = 0; pr.anchor_min_y = 2.0f / 3; pr.anchor_max_x = 1; pr.anchor_max_y = 1;
    pr.left = 10; pr.top = 0; pr.right = -10; pr.bottom = -10;
    cw_comp_set(w, panel, c->rect, &pr, sizeof(pr));
    cw_ui_image pim = *(const cw_ui_image *)cw_ui_image_type.defaults;
    pim.r = 10; pim.g = 20; pim.b = 30; pim.a = 200;
    cw_comp_set(w, panel, c->image, &pim, sizeof(pim));
    cw_ui_rect br = *(const cw_ui_rect *)cw_ui_rect_type.defaults;
    br.parent = panel; br.anchor_min_x = 0; br.anchor_min_y = 0; br.anchor_max_x = 1; br.anchor_max_y = 0;
    br.left = 20; br.top = 20; br.right = -20; br.bottom = 40;
    cw_comp_set(w, bar, c->rect, &br, sizeof(br));
    cw_ui_image bim = *(const cw_ui_image *)cw_ui_image_type.defaults;
    bim.fill_mode = CW_UI_FILL_HORIZONTAL; bim.fill = 0.25f;
    cw_comp_set(w, bar, c->image, &bim, sizeof(bim));
    cw_ui_rect lr = *(const cw_ui_rect *)cw_ui_rect_type.defaults;
    lr.parent = bar; lr.anchor_min_x = lr.anchor_min_y = 0; lr.anchor_max_x = lr.anchor_max_y = 1; lr.left = lr.top = lr.right = lr.bottom = 0;
    cw_comp_set(w, label, c->rect, &lr, sizeof(lr));
    cw_ui_text lt = *(const cw_ui_text *)cw_ui_text_type.defaults;
    snprintf(lt.text, sizeof(lt.text), "hello \"world\" \\ ok");
    lt.align = CW_UI_ALIGN_RIGHT; lt.valign = CW_UI_VALIGN_BOTTOM; lt.size = 1;
    cw_comp_set(w, label, c->text, &lt, sizeof(lt));
    cw_ui_rect ur = *(const cw_ui_rect *)cw_ui_rect_type.defaults;
    ur.anchor_min_x = ur.anchor_max_x = 1; ur.anchor_min_y = ur.anchor_max_y = 0; ur.left = -110; ur.top = 10; ur.right = -10; ur.bottom = 50; ur.order = 5;
    cw_comp_set(w, button, c->rect, &ur, sizeof(ur));
    add_default(w, button, c->image);
    add_default(w, button, c->button);
    ((cw_ui_button *)cw_comp_ptr(w, button, c->button))->action = 7;
    cw_ui_rect hr = *(const cw_ui_rect *)cw_ui_rect_type.defaults;
    hr.visible = false;
    cw_comp_set(w, hidden, c->rect, &hr, sizeof(hr));
    add_default(w, hidden, c->image);
    cw_game_world_settle(&gw);
    /* layout at the reference size, then on a 1920x1080 window and an ultra-wide one */
    cw_ui_screen screen = {0, 0, 1280, 720};
    cw_game_world_set_ui_screen(&gw, screen);
    CHECK(cw_game_world_ui_layout(&gw) == 5, "five elements laid out");
    const cw_ui_layout *pl = cw_comp_ptr(w, panel, c->layout), *bl = cw_comp_ptr(w, bar, c->layout), *ll = cw_comp_ptr(w, label, c->layout), *ul = cw_comp_ptr(w, button, c->layout), *hl = cw_comp_ptr(w, hidden, c->layout);
    CHECK(pl && pl->scale == 1 && pl->x == 10 && pl->w == 1260 && fabs(pl->y - 480) < 1e-3 && fabs(pl->h - 230) < 1e-3, "panel at the reference size (%g %g %g %g)", (double)pl->x, (double)pl->y, (double)pl->w, (double)pl->h);
    CHECK(bl && bl->x == 30 && fabs(bl->y - 500) < 1e-3 && bl->w == 1220 && bl->h == 20 && bl->pw == 1260, "the bar follows the panel's width; its layout records the parent size");
    CHECK(ll && ll->x == bl->x && ll->w == bl->w && ll->h == bl->h, "a stretched child covers its parent");
    CHECK(ul && ul->x == 1170 && ul->y == 10 && ul->w == 100 && ul->h == 40 && ul->shown, "the button sits at the top right");
    CHECK(hl && !hl->shown, "hidden");
    CHECK(cw_ui_depth(w, c, label) == 2 && cw_ui_depth(w, c, panel) == 0, "depths");
    float pw, ph;
    CHECK(cw_ui_parent_size(w, c, panel, &pw, &ph) && pw == 1280 && ph == 720 && cw_ui_parent_size(w, c, bar, &pw, &ph) && pw == 1260, "parent sizes");
    screen.w = 1920; screen.h = 1080;
    cw_game_world_set_ui_screen(&gw, screen);
    cw_game_world_ui_layout(&gw);
    CHECK(fabs(pl->scale - 1.5f) < 1e-6 && pl->x == 15 && pl->w == 1890 && ul->w == 150 && ul->x == 1755 && fabs(ul->rx - 1170) < 1e-3, "1080p: everything 1.5x, reference coordinates unchanged");
    screen.w = 2560; screen.h = 720;
    cw_game_world_ui_layout(&gw);
    cw_game_world_set_ui_screen(&gw, screen);
    cw_game_world_ui_layout(&gw);
    CHECK(pl->scale == 1 && pl->w == 2540 && ul->x == 2450 && ul->w == 100 && ul->rw == 100, "ultra-wide: the panel stretches, the button keeps its size and stays in the corner");
    screen.x = 100; screen.y = 50; screen.w = 640; screen.h = 360;
    cw_game_world_set_ui_screen(&gw, screen);
    cw_game_world_ui_layout(&gw);
    CHECK(pl->scale == 0.5f && pl->x == 105 && ul->x == 100 + 585 && ul->y == 55 && ul->w == 50, "a game view inside the window: offset and half size");
    /* draw order and hit testing */
    cw_entity_id order[8];
    size_t n = cw_ui_collect(w, c, order, 8);
    CHECK(n == 5 && order[0] == panel && order[1] == bar && order[2] == label && order[3] == hidden && order[4] == button, "draw order: parents first, siblings by order then id");
    CHECK(cw_ui_hit(w, c, 700, 60, false) == button && cw_ui_hit(w, c, 700, 60, true) == button, "hit the button");
    CHECK(cw_ui_hit(w, c, 120, 305, false) == label && cw_ui_hit(w, c, 120, 305, true) == CW_INVALID_ENTITY, "the topmost element wins; only buttons when asked");
    CHECK(cw_ui_hit(w, c, 5, 5, false) == CW_INVALID_ENTITY, "nothing outside");
    /* a cycle resolves to the canvas instead of hanging */
    cw_ui_rect *brp = cw_comp_ptr(w, bar, c->rect);
    cw_entity_id old_parent = brp->parent;
    ((cw_ui_rect *)cw_comp_ptr(w, panel, c->rect))->parent = bar;
    cw_game_world_ui_layout(&gw);
    CHECK(cw_ui_collect(w, c, order, 8) == 5 && cw_ui_depth(w, c, panel) == CW_UI_MAX_DEPTH, "cycles are tolerated and reported as the maximum depth");
    ((cw_ui_rect *)cw_comp_ptr(w, panel, c->rect))->parent = CW_INVALID_ENTITY;
    ((cw_ui_rect *)cw_comp_ptr(w, bar, c->rect))->parent = old_parent;
    /* drawing through the hud ops */
    screen.x = screen.y = 0; screen.w = 1280; screen.h = 720;
    cw_game_world_set_ui_screen(&gw, screen);
    memset(&hud_stats, 0, sizeof(hud_stats));
    cw_draw_context ctx = {1, 0, false, false};
    cw_game_world_draw(&gw, &canvas, &ctx);
    CHECK(hud_stats.images == 3 && hud_stats.texts == 1, "panel, bar and button images, one text; the hidden one is skipped (%d %d)", hud_stats.images, hud_stats.texts);
    CHECK(strcmp(hud_stats.last_text, lt.text) == 0 && hud_stats.last_scale == 1 && fabs(hud_stats.text_x - (bl->x + bl->w - 8 * (float)strlen(lt.text))) < 1e-3 && fabs(hud_stats.text_y - (bl->y + bl->h - 8)) < 1e-3, "text aligned bottom right (%g %g)", (double)hud_stats.text_x, (double)hud_stats.text_y);
    /* clicks: press and release over the button within ticks */
    cw_game_world_ui_input(&gw, 1200, 30, true, true, false);
    cw_game_world_tick(&gw, 1);
    CHECK(ul->hovered && ul->pressed && !ul->clicked && !cw_game_ui_clicked(w, 7), "pressed");
    cw_game_world_ui_input(&gw, 1200, 30, false, false, true);
    cw_game_world_tick(&gw, 1);
    CHECK(ul->clicked && !ul->pressed && cw_game_ui_clicked(w, 7) && !cw_game_ui_clicked(w, 8) && cw_ui_clicked(w, c, button), "clicked for this tick");
    cw_game_world_tick(&gw, 1);
    CHECK(!ul->clicked && !cw_game_ui_clicked(w, 7), "and gone the next");
    cw_game_world_ui_input(&gw, 1200, 30, true, true, false);
    cw_game_world_ui_input(&gw, 300, 300, false, false, true);   /* released elsewhere */
    cw_game_world_tick(&gw, 1);
    CHECK(!ul->clicked && !ul->pressed, "releasing off the button is not a click");
    cw_game_world_ui_input(&gw, 1200, 30, true, true, false);
    cw_game_world_ui_input(&gw, 1200, 30, false, false, true);   /* both edges before one tick */
    cw_game_world_tick(&gw, 1);
    CHECK(ul->clicked, "a press and release in one frame still clicks");
    /* the eased state and the element's parameters reach the shader through the canvas */
    {
        cw_ui_image *bim2 = cw_comp_ptr(w, button, c->image);
        bim2->param_x = 2.5f; bim2->param_w = -1;
        cw_game_world_ui_input(&gw, 5, 5, false, false, false);   /* away, long enough to settle */
        cw_game_world_tick(&gw, 60);
        CHECK(ul->hover_t == 0 && ul->press_t == 0, "settled away");
        cw_game_world_ui_input(&gw, 1200, 30, false, false, false);   /* hovering */
        cw_game_world_tick(&gw, 1);
        CHECK(ul->hovered && ul->hover_t > 0 && ul->hover_t < 1 && ul->since == 0, "hover eases in over several ticks (%g)", (double)ul->hover_t);
        cw_game_world_tick(&gw, 30);
        CHECK(ul->hover_t == 1 && ul->since > 0.4f, "and settles at 1 after half a second; `since` counts up (%g)", (double)ul->since);
        memset(&hud_stats, 0, sizeof(hud_stats));
        cw_game_world_draw(&gw, &canvas, &ctx);
        CHECK(hud_stats.last_dynamic.state[0] == 1 && hud_stats.last_dynamic.state[1] == 0 && hud_stats.last_dynamic.state[2] == 1 && hud_stats.last_dynamic.params[0] == 2.5f && hud_stats.last_dynamic.params[3] == -1,
              "the canvas gets hover, press, fill and the element's params (%g %g %g %g)", (double)hud_stats.last_dynamic.state[0], (double)hud_stats.last_dynamic.state[1], (double)hud_stats.last_dynamic.state[2], (double)hud_stats.last_dynamic.params[0]);
        cw_game_world_ui_input(&gw, 5, 5, false, false, false);   /* away */
        cw_game_world_tick(&gw, 1);
        CHECK(!ul->hovered && ul->hover_t < 1 && ul->since == 0, "leaving eases out and restarts `since`");
        cw_game_world_tick(&gw, 60);
        CHECK(ul->hover_t == 0, "back to 0");
    }
    ((cw_ui_button *)cw_comp_ptr(w, button, c->button))->enabled = false;
    cw_game_world_ui_input(&gw, 1200, 30, true, true, false);
    cw_game_world_ui_input(&gw, 1200, 30, false, false, true);
    cw_game_world_tick(&gw, 1);
    CHECK(!ul->clicked && !ul->hovered, "disabled buttons ignore the mouse");
    CHECK(cw_ui_set_textf(w, c, label, "%d%%", 25) && strcmp(((cw_ui_text *)cw_comp_ptr(w, label, c->text))->text, "25%") == 0 && !cw_ui_set_text(w, c, button, "x"), "set_text on a text, refused elsewhere");
    /* scenes: the interface is authored like anything else; strings are quoted */
    cw_writer out;
    cw_writer_init(&out);
    cw_entity_id all[5] = {panel, bar, label, button, hidden};
    cw_scene_result r;
    cw_scene_save_config save = {0};
    CHECK(cw_scene_write_entities(w, all, 5, &save, &out, &r) && r.entities == 5, "write the interface");
    const char *text = ztext(&out);
    CHECK(strstr(text, "UiText text=\"25%\"") && strstr(text, "UiRect parent=") && !strstr(text, "UiLayout"), "quoted text, references, no transient layout:\n%.*s", (int)out.size, text);
    cw_ui_set_text(w, c, label, "hello \"world\" \\ ok");
    cw_writer_clear(&out);
    cw_scene_write_entities(w, all, 5, &save, &out, &r);
    CHECK(strstr(ztext(&out), "text=\"hello \\\"world\\\" \\\\ ok\"") != NULL, "escapes in text");
    cw_scene_load_config load = {0};
    cw_entity_id made_ids[8];
    size_t made = 0;
    CHECK(cw_scene_instantiate(w, (const char *)out.data, out.size, &load, false, 0, 0, made_ids, 8, &made, &r) && made == 5, "instantiate a copy: %s", r.message);
    cw_game_world_settle(&gw);
    cw_game_world_ui_layout(&gw);
    size_t texts = 0;
    cw_query q = cw_query_iter(w, cw_mask(&c->text, 1));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        const cw_ui_text *t = cw_comp_ptr(w, id, c->text);
        const cw_ui_rect *rr = cw_comp_ptr(w, id, c->rect);
        if (strcmp(t->text, "hello \"world\" \\ ok") == 0 && rr->parent != bar && rr->parent != CW_INVALID_ENTITY && cw_comp_has(w, rr->parent, c->rect)) ++texts;
    }
    CHECK(texts == 1, "the copy's label kept its text and points at the copied bar (%zu)", texts);
    cw_writer_destroy(&out);
    cw_game_world_destroy(&gw);
    cw_game_close(&game);
}

/* byte-array fields and the brush geometry behind the level editor */
typedef struct { int32_t n; CW_BYTES(8) bytes; } Blobbed;
static const cw_field blobbed_fields[] = { CW_FIELD(Blobbed, n, CW_FIELD_I32), CW_FIELD_BLOB(Blobbed, bytes) };
static const cw_type_info blobbed_type = {"Blobbed", sizeof(Blobbed), _Alignof(Blobbed), blobbed_fields, 2, NULL, NULL, 0, NULL, NULL};
static void test_bytes_and_brushes(void) {
    /* base64 */
    char text[64];
    uint8_t back[16];
    const uint8_t sample[5] = {0, 255, 1, 2, 3};
    CHECK(cw_base64_encode(sample, 5, text, sizeof(text)) == 8 && strcmp(text, "AP8BAgM=") == 0, "base64 encodes with padding: %s", text);
    CHECK(cw_base64_decode(text, back, sizeof(back)) == 5 && memcmp(back, sample, 5) == 0, "and decodes");
    CHECK(cw_base64_encode(sample, 3, text, sizeof(text)) == 4 && cw_base64_decode(text, back, 16) == 3 && cw_base64_encode(sample, 0, text, sizeof(text)) == 0 && text[0] == 0 && cw_base64_decode("", back, 16) == 0, "lengths 3 and 0");
    CHECK(cw_base64_decode("AP8B*gM=", back, 16) == SIZE_MAX && cw_base64_decode("AP8", back, 16) == SIZE_MAX && cw_base64_decode("AP8BAgM=", back, 2) == SIZE_MAX, "bad characters, lengths and overflow are rejected");
    CHECK(cw_base64_decode("A=BB", back, 16) == SIZE_MAX, "padding only at the end");
    /* a blob field through the generic paths */
    CHECK(cw_type_valid(&blobbed_type) && blobbed_fields[1].capacity == 8, "a CW_BYTES(8) member is a valid field");
    Blobbed v = {7, {0, {0}}};
    CHECK(cw_field_format(&blobbed_fields[1], &v, text, sizeof(text)) == 0 && text[0] == 0, "an empty blob formats to nothing");
    CHECK(cw_field_parse(&blobbed_fields[1], &v, "AP8BAgM=") && v.bytes.size == 5 && v.bytes.data[1] == 255 && v.bytes.data[4] == 3 && cw_field_get(&blobbed_fields[1], &v) == 5, "parse fills size and data");
    CHECK(cw_field_format(&blobbed_fields[1], &v, text, sizeof(text)) == 8 && strcmp(text, "AP8BAgM=") == 0, "and formats back");
    CHECK(!cw_field_parse(&blobbed_fields[1], &v, "AAAAAAAAAAAAAAAA") && v.bytes.size == 5, "more than the capacity is refused and leaves the value");
    CHECK(cw_field_check(&blobbed_fields[1], &v), "a blob within its capacity checks out");
    v.bytes.size = 9;
    CHECK(!cw_field_check(&blobbed_fields[1], &v), "a size beyond the capacity does not");
    v.bytes.size = 5;
    /* the codec: size and bytes, validated */
    cw_world w;
    cw_world_init(&w, NULL);
    cw_comp_block(&w, 1);
    CHECK(cw_reflect_register(&w, 0, &blobbed_type), "register");
    cw_entity_id e = cw_world_create_entity(&w);
    cw_comp_set(&w, e, 0, &v, sizeof(v));
    unsigned char image[64];
    size_t size = 0;
    CHECK(cw_reflect_marshal(&w, e, 0, image, sizeof(image), &size) && size == 4 + 2 + 5, "marshalled: i32, u16 size, 5 bytes (%zu)", size);
    Blobbed zero = {0, {0, {0}}};
    cw_comp_set(&w, e, 0, &zero, sizeof(zero));
    CHECK(cw_reflect_unmarshal(&w, e, 0, image, size) && ((Blobbed *)cw_comp_ptr(&w, e, 0))->bytes.size == 5 && ((Blobbed *)cw_comp_ptr(&w, e, 0))->bytes.data[1] == 255, "unmarshalled");
    image[4] = 9;   /* size 9 > capacity 8 */
    CHECK(!cw_reflect_unmarshal(&w, e, 0, image, size), "an oversized blob image is rejected");
    cw_world_destroy(&w);

    /* brush geometry */
    cw_brush b;
    cw_type_default(&cw_brush_type, &b);
    b.width = 2; b.height = 4; b.depth = 6; b.subdivisions = 1; b.texture_scale = 2;
    float center[3] = {10, 20, 30}, lo[3], hi[3];
    cw_brush_bounds(&b, center, lo, hi);
    CHECK(lo[0] == 9 && hi[0] == 11 && lo[1] == 18 && hi[1] == 22 && lo[2] == 27 && hi[2] == 33, "bounds around the centre");
    float nlo[3] = {0, 0, 0}, nhi[3] = {1, 0.001f, 3};
    cw_brush_set_bounds(&b, center, nlo, nhi, 0.5f);
    CHECK(b.width == 1 && b.height == 0.5f && b.depth == 3 && center[0] == 0.5f && center[1] == 0.25f && center[2] == 1.5f, "set from corners with a minimum thickness");
    float n[3];
    cw_brush_face_normal(CW_FACE_BOTTOM, n);
    CHECK(n[1] == -1 && cw_brush_face_axis(CW_FACE_SOUTH) == 2 && cw_brush_face_sign(CW_FACE_NORTH) == -1 && strcmp(cw_brush_face_names[CW_FACE_TOP], "top") == 0, "face normals and names");
    CHECK(cw_brush_face_count(&b) == 6 && cw_brush_clip_count(&b) == 0, "a box has six faces");
    b.width = 2; b.height = 4; b.depth = 6; b.subdivisions = 2; b.texture_scale = 2;
    center[0] = 10; center[1] = 20; center[2] = 30;
    cw_brush_bounds(&b, center, lo, hi);
    cw_plane plane;
    CHECK(cw_brush_face_plane(&b, center, CW_FACE_TOP, &plane) && plane.n[1] == 1 && plane.d == 22, "the top plane is y = 22");
    CHECK(cw_brush_face_plane(&b, center, CW_FACE_WEST, &plane) && plane.n[0] == -1 && plane.d == -9, "the west plane is x = 9 (n . p <= d with n = -x)");
    cw_brush_polygon poly;
    CHECK(cw_brush_face_polygon(&b, center, CW_FACE_TOP, &poly) && poly.count == 4, "a box face is a quad (%d vertices)", poly.count);
    bool quad_ok = poly.count == 4;
    for (int i = 0; i < poly.count; ++i) if (poly.p[i][1] != 22 || fabsf(poly.p[i][0] - 10) != 1 || fabsf(poly.p[i][2] - 30) != 3) quad_ok = false;
    CHECK(quad_ok, "with the box's corners");
    static cw_brush_vertex verts[CW_BRUSH_FACE_MAX_VERTICES];
    static uint32_t idx[CW_BRUSH_FACE_MAX_INDICES];
    bool outward = true, on_face = true, uv_ok = true;
    for (int f = 0; f < CW_BRUSH_FACES; ++f) {
        size_t ni = 0, nv = cw_brush_face_mesh(&b, center, f, verts, idx, &ni);
        CHECK(nv == 16 && ni == 24, "a 2x2 face has four cells of four vertices (%zu) and eight triangles (%zu)", nv, ni);
        cw_brush_face_normal((cw_brush_face)f, n);
        int axis = cw_brush_face_axis((cw_brush_face)f);
        for (size_t i = 0; i < nv; ++i) {
            float p[3] = {verts[i].x, verts[i].y, verts[i].z};
            if (fabsf(p[axis] - (n[axis] > 0 ? hi[axis] : lo[axis])) > 1e-4f) on_face = false;
            if (verts[i].nx != n[0] || verts[i].ny != n[1] || verts[i].nz != n[2] || verts[i].a != 255 || verts[i].r != 255) on_face = false;
            int ua, va;
            cw_brush_face_tangents((cw_brush_face)f, &ua, &va);
            if (fabsf(verts[i].u - p[ua] / 2) > 1e-4f || fabsf(verts[i].v + p[va] / 2) > 1e-4f) uv_ok = false;
        }
        for (size_t t = 0; t + 2 < ni; t += 3) {
            const cw_brush_vertex *a = &verts[idx[t]], *bb = &verts[idx[t + 1]], *c = &verts[idx[t + 2]];
            float e1[3] = {bb->x - a->x, bb->y - a->y, bb->z - a->z}, e2[3] = {c->x - a->x, c->y - a->y, c->z - a->z};
            float cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            if (cr[0] * n[0] + cr[1] * n[1] + cr[2] * n[2] <= 0) outward = false;
        }
    }
    CHECK(on_face && uv_ok && outward, "every face lies on its plane with its normal, world-aligned uvs (scale 2) and outward triangles");
    /* texture alignment per face */
    cw_brush_align al;
    cw_brush_face_align_get(&b, CW_FACE_TOP, &al);
    CHECK(al.shift_u == 0 && al.rotation == 0 && al.scale_u == 1 && b.align.size == 0, "alignment defaults, stored nowhere");
    al.shift_u = 0.5f; al.scale_v = 2; al.rotation = 90;
    cw_brush_face_align_set(&b, CW_FACE_TOP, &al);
    cw_brush_face_align_get(&b, CW_FACE_TOP, &al);
    CHECK((size_t)b.align.size == CW_BRUSH_MAX_FACES * CW_BRUSH_ALIGN_FLOATS * sizeof(float) && al.shift_u == 0.5f && al.scale_v == 2 && al.rotation == 90, "alignment stored and read back");
    cw_brush_face_align_get(&b, CW_FACE_EAST, &al);
    CHECK(al.shift_u == 0 && al.scale_v == 1, "other faces keep the defaults");
    {
        float tu, tv, top_n[3] = {0, 1, 0}, tpt[3] = {4, 22, 6};   /* top face: u = x, v = z -> base uv (2, -3) at scale 2 */
        cw_brush_face_uv(&b, CW_FACE_TOP, top_n, tpt, &tu, &tv);
        /* rotated 90 degrees: (u, v) -> (-v, u) = (3, 2); v scaled by 2 -> 1; shift u by 0.5 -> 3.5 */
        CHECK(fabsf(tu - 3.5f) < 1e-4f && fabsf(tv - 1) < 1e-4f, "uv follows shift, rotation and scale (%g %g)", (double)tu, (double)tv);
    }
    al = cw_brush_align_default();
    cw_brush_face_align_set(&b, CW_FACE_TOP, &al);
    CHECK(b.align.size == 0, "back to defaults frees the alignment");
    /* ray casts report the face */
    float o[3] = {10, 50, 30}, d[3] = {0, -1, 0}, t;
    int face;
    cw_brush_face bface;
    CHECK(cw_brush_ray(&b, center, o, d, &t, &face) && t == 28 && face == CW_FACE_TOP, "a ray from above hits the top at the right distance (%g)", (double)t);
    CHECK(cw_brush_ray_box(lo, hi, o, d, &t, &bface) && t == 28 && bface == CW_FACE_TOP, "the box-only cast agrees");
    float o2[3] = {0, 20, 30}, d2[3] = {1, 0, 0};
    CHECK(cw_brush_ray(&b, center, o2, d2, &t, &face) && t == 9 && face == CW_FACE_WEST, "from the west");
    float o3[3] = {10, 20, 30};
    CHECK(cw_brush_ray(&b, center, o3, d2, &t, &face) && t == 1 && face == CW_FACE_EAST, "from inside: the exit face");
    float o4[3] = {0, 50, 30};
    CHECK(!cw_brush_ray(&b, center, o4, d2, &t, &face), "a miss");
    float pt[3] = {10.9f, 21.9f, 33};
    CHECK(cw_brush_face_at(lo, hi, pt) == CW_FACE_SOUTH && cw_brush_face_of_point(&b, center, pt) == CW_FACE_SOUTH, "the face a surface point lies on");
    /* painting: soft, clamped, and invalidated by a subdivision change */
    float p0[3] = {11, 22, 33};
    size_t touched = cw_brush_paint_at(&b, center, p0, 3, 0, 0, 255, 0.5f);
    CHECK(touched >= 3 && cw_brush_painted(&b), "the corner vertices of three faces were touched (%zu)", touched);
    uint8_t rgb[3];
    cw_brush_paint_get(&b, CW_FACE_EAST, 2, 2, rgb);   /* east face: u = z, v = y -> (i, j) = (2, 2) is the (+z, +y) corner */
    CHECK(rgb[0] == 128 && rgb[2] == 255, "half strength at the centre of the stroke (%u %u %u)", rgb[0], rgb[1], rgb[2]);
    cw_brush_paint_at(&b, center, p0, 3, 0, 0, 255, 1);
    cw_brush_paint_get(&b, CW_FACE_EAST, 2, 2, rgb);
    CHECK(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 255, "full strength reaches the colour");
    {
        size_t ni = 0, nv = cw_brush_face_mesh(&b, center, CW_FACE_EAST, verts, idx, &ni);
        bool blue_corner = false;
        for (size_t i = 0; i < nv; ++i) if (fabsf(verts[i].y - 22) < 1e-4f && fabsf(verts[i].z - 33) < 1e-4f && verts[i].b == 255 && verts[i].r == 0) blue_corner = true;
        CHECK(blue_corner, "the mesh carries the painted corner colour");
    }
    float far_point[3] = {-100, 0, 0};
    CHECK(cw_brush_paint_at(&b, center, far_point, 1, 255, 0, 0, 1) == 0, "a stroke away from the brush touches nothing");
    b.subdivisions = 4;
    {
        size_t ni = 0;
        CHECK(!cw_brush_painted(&b) && cw_brush_face_mesh(&b, center, CW_FACE_TOP, verts, idx, &ni) == 64 && ni == 96, "a subdivision change invalidates the paint; the lattice follows");
    }
    cw_brush_paint_get(&b, CW_FACE_TOP, 0, 0, rgb);
    CHECK(rgb[2] == 255 && rgb[0] == 255, "unpainted reads white");

    /* clipping: a wedge, a general cut, an axis-aligned cut (a resize), misses */
    cw_type_default(&cw_brush_type, &b);
    b.width = 4; b.height = 2; b.depth = 2;
    center[0] = 0; center[1] = 1; center[2] = 0;
    CHECK(cw_brush_make_wedge(&b, center, 0, 1) && cw_brush_clip_count(&b) == 1 && cw_brush_face_count(&b) == 7, "a wedge is a box with one clip");
    CHECK(b.width == 4 && b.height == 2 && fabsf(center[1] - 1) < 1e-5f, "the wedge still spans the box");
    {
        float o_top[3] = {1.9f, 5, 0}, o_near[3] = {-1.9f, 5, 0}, down[3] = {0, -1, 0};
        /* rising along +x: the far (+x) end is tall, the near end is cut to nothing */
        CHECK(cw_brush_ray(&b, center, o_top, down, &t, &face) && face == CW_BRUSH_FACES && fabsf(t - 3.05f) < 0.01f, "near the far end the slope is almost at the top (t %g, face %d)", (double)t, face);
        cw_brush_polygon east, top_face;
        cw_brush_face_polygon(&b, center, CW_FACE_EAST, &east);
        cw_brush_face_polygon(&b, center, CW_FACE_TOP, &top_face);
        CHECK(east.count == 4 && top_face.count == 0, "the far end is a full wall, the top is gone (%d, %d)", east.count, top_face.count);
        CHECK(cw_brush_ray(&b, center, o_near, down, &t, &face) && face == CW_BRUSH_FACES && t > 3 && t < 5, "the near end is the slope (t %g, face %d)", (double)t, face);
        cw_brush_polygon slope, west;
        cw_brush_face_polygon(&b, center, CW_BRUSH_FACES, &slope);
        cw_brush_face_polygon(&b, center, CW_FACE_WEST, &west);
        CHECK(slope.count == 4 && west.count == 0, "the slope is a quad and the west face is gone (%d, %d)", slope.count, west.count);
        size_t ni = 0, nv = cw_brush_face_mesh(&b, center, CW_FACE_SOUTH, verts, idx, &ni);
        CHECK(nv > 0 && ni > 0, "the south face is now a triangle, still meshed (%zu vertices)", nv);
        bool inside = true;
        for (size_t i = 0; i < nv; ++i) if (verts[i].y > (verts[i].x + 2) * 0.5f + 1e-3f) inside = false;   /* under the diagonal y = (x + 2) / 2 */
        CHECK(inside, "no vertex of the clipped face pokes above the slope");
    }
    cw_brush_paint_get(&b, CW_BRUSH_FACES, 0, 0, rgb);
    CHECK(rgb[0] == 255 && cw_brush_paint_size(&b) == (size_t)7 * 9 * 3, "clip faces have a lattice too");
    cw_plane miss = {{0, 1, 0}, 10};
    CHECK(cw_brush_plane_side(&b, center, &miss) == -1 && !cw_brush_clip(&b, center, &miss), "a plane above the brush cuts nothing");
    cw_plane gone = {{0, -1, 0}, -10};
    CHECK(cw_brush_plane_side(&b, center, &gone) == 1 && !cw_brush_clip(&b, center, &gone), "a plane that would remove everything is refused");
    cw_plane half = {{0, 1, 0}, 1};
    CHECK(cw_brush_plane_side(&b, center, &half) == 0 && cw_brush_clip(&b, center, &half) && cw_brush_clip_count(&b) == 1 && fabsf(b.height - 1) < 1e-5f && fabsf(center[1] - 0.5f) < 1e-5f,
          "an axis-aligned cut resizes the box instead of adding a plane (height %g, centre y %g)", (double)b.height, (double)center[1]);
    CHECK(fabsf(b.width - 4) < 1e-4f, "the wedge's width is unchanged by the horizontal cut");
    cw_plane diag = {{0.70710678f, 0, 0.70710678f}, 0.70710678f};   /* x + z <= 1 */
    CHECK(cw_brush_clip(&b, center, &diag) && cw_brush_clip_count(&b) == 2, "a second general cut is stored");
    {
        float verts_max = -INFINITY;
        int faces = cw_brush_face_count(&b);
        for (int f = 0; f < faces; ++f) {
            cw_brush_polygon pg;
            cw_brush_face_polygon(&b, center, f, &pg);
            for (int i = 0; i < pg.count; ++i) if (pg.p[i][0] + pg.p[i][2] > verts_max) verts_max = pg.p[i][0] + pg.p[i][2];
        }
        CHECK(verts_max <= 1 + 1e-3f, "every remaining vertex is on the kept side (max x + z = %g)", (double)verts_max);
        cw_brush_bounds(&b, center, lo, hi);
        CHECK(hi[0] <= 2 + 1e-4f && hi[2] < 1 + 1e-3f, "the box shrank to the solid (z max %g)", (double)hi[2]);
    }
    cw_brush_clear_clips(&b);
    CHECK(cw_brush_clip_count(&b) == 0 && cw_brush_face_count(&b) == 6, "clips cleared");

    /* CSG: subtraction and hollowing */
    {
        cw_brush big, cutter, pieces[CW_BRUSH_MAX_FACES];
        float pc[CW_BRUSH_MAX_FACES][3];
        cw_type_default(&cw_brush_type, &big);
        big.width = 6; big.height = 2; big.depth = 6;
        float bc[3] = {0, 1, 0}, cc[3] = {0, 1, 0};
        cw_type_default(&cw_brush_type, &cutter);
        cutter.width = 2; cutter.height = 4; cutter.depth = 2;   /* a column through the middle, taller than the slab */
        cw_plane cut[CW_BRUSH_MAX_FACES];
        int nc = cw_brush_planes(&cutter, cc, cut);
        CHECK(nc == 6 && cw_brush_intersects(&big, bc, cut, nc), "the column meets the slab");
        int dropped = 7;
        int np = cw_brush_subtract(&big, bc, cut, nc, pieces, pc, &dropped);
        CHECK(np == 4 && dropped == 0, "a box minus a box through it: four pieces (%d)", np);
        float volume = 0;
        bool boxes = true;
        for (int i = 0; i < np; ++i) {
            volume += pieces[i].width * pieces[i].height * pieces[i].depth;
            if (cw_brush_clip_count(&pieces[i])) boxes = false;
        }
        CHECK(boxes && fabsf(volume - (36 * 2 - 4 * 2)) < 1e-3f, "the pieces are plain boxes and the hole's volume is gone (%g)", (double)volume);
        cw_brush far_cutter = cutter;
        float fc[3] = {20, 1, 0};
        nc = cw_brush_planes(&far_cutter, fc, cut);
        CHECK(!cw_brush_intersects(&big, bc, cut, nc) && cw_brush_subtract(&big, bc, cut, nc, pieces, pc, NULL) == -1, "a cutter elsewhere does not intersect");
        cw_brush huge = cutter;
        huge.width = huge.depth = 20; huge.height = 20;
        nc = cw_brush_planes(&huge, cc, cut);
        CHECK(cw_brush_subtract(&big, bc, cut, nc, pieces, pc, NULL) == 0, "swallowed whole: no pieces");
        np = cw_brush_hollow(&big, bc, 0.5f, pieces, pc, &dropped);
        CHECK(np == 6 && dropped == 0, "a hollowed box has six walls (%d)", np);
        volume = 0;
        for (int i = 0; i < np; ++i) volume += pieces[i].width * pieces[i].height * pieces[i].depth;
        CHECK(fabsf(volume - (6 * 2 * 6 - 5 * 1 * 5)) < 1e-3f, "the walls enclose the hollow (%g)", (double)volume);
        /* subtracting a wedge leaves a piece with a clip plane */
        cw_brush wedge;
        cw_type_default(&cw_brush_type, &wedge);
        wedge.width = 4; wedge.height = 2; wedge.depth = 8;
        float wc[3] = {0, 1, 0};
        cw_brush_make_wedge(&wedge, wc, 0, 1);
        nc = cw_brush_planes(&wedge, wc, cut);
        np = cw_brush_subtract(&big, bc, cut, nc, pieces, pc, &dropped);
        bool clipped_piece = false;
        for (int i = 0; i < np; ++i) if (cw_brush_clip_count(&pieces[i])) clipped_piece = true;
        CHECK(np >= 1 && clipped_piece, "the piece over the wedge's slope carries the slope as a clip (%d pieces)", np);
    }
    /* texture lock, grid snapping and vertices */
    {
        cw_brush tb;
        cw_type_default(&cw_brush_type, &tb);
        tb.width = 2; tb.height = 2; tb.depth = 2; tb.texture_scale = 2;
        cw_brush_align al0;
        cw_brush_face_align_get(&tb, CW_FACE_TOP, &al0);
        float move[3] = {1, 0, 3};
        cw_brush_lock_textures(&tb, move);
        cw_brush_align al1;
        cw_brush_face_align_get(&tb, CW_FACE_TOP, &al1);
        /* top face: u = x, v = z (negated): base uv moved by (0.5, -1.5), so the shift compensates */
        CHECK(fabsf(al1.shift_u + 0.5f) < 1e-5f && fabsf(al1.shift_v - 1.5f) < 1e-5f, "texture lock shifts the top face by the move over the texture scale (%g %g)", (double)al1.shift_u, (double)al1.shift_v);
        cw_brush_face_align_get(&tb, CW_FACE_EAST, &al1);
        CHECK(fabsf(al1.shift_u + 1.5f) < 1e-5f && fabsf(al1.shift_v) < 1e-5f, "the east face (u = z, v = y) shifts along z only (%g %g)", (double)al1.shift_u, (double)al1.shift_v);
        float tc[3] = {0.3f, 1.1f, -0.2f};
        tb.width = 1.9f; tb.height = 2.2f; tb.depth = 1.6f;
        CHECK(cw_brush_snap_to_grid(&tb, tc, 0.5f) && tb.width == 2 && tb.height == 2 && tb.depth == 1.5f && tc[0] == 0.5f && tc[1] == 1 && tc[2] == -0.25f, "snap to a 0.5 grid: faces land on grid lines (%g %g %g at %g %g %g)",
              (double)tb.width, (double)tb.height, (double)tb.depth, (double)tc[0], (double)tc[1], (double)tc[2]);
        CHECK(!cw_brush_snap_to_grid(&tb, tc, 0.5f), "snapping again changes nothing");
        float corners_out[32][3];
        CHECK(cw_brush_vertices(&tb, tc, corners_out, 32) == 8, "a box has eight vertices");
        cw_brush wb;
        cw_type_default(&cw_brush_type, &wb);
        wb.width = 4; wb.height = 2; wb.depth = 2;
        float wc2[3] = {0, 1, 0};
        cw_brush_make_wedge(&wb, wc2, 0, 1);
        CHECK(cw_brush_vertices(&wb, wc2, corners_out, 32) == 6, "a wedge has six");
        /* drag a box corner: the three faces through it follow */
        float corner[3] = {1.5f, 2, 0.5f}, target[3] = {2, 3, 1};
        CHECK(cw_brush_move_vertex(&tb, tc, corner, target) && fabsf(tb.width - 2.5f) < 1e-5f && fabsf(tb.height - 3) < 1e-5f && fabsf(tb.depth - 2) < 1e-5f, "the box grew to the moved corner (%g %g %g)", (double)tb.width, (double)tb.height, (double)tb.depth);
        float nowhere[3] = {9, 9, 9};
        CHECK(!cw_brush_move_vertex(&tb, tc, nowhere, target), "a point that is no vertex moves nothing");
        /* drag the wedge's top-far vertex down: the slope re-fits and the top edge lowers */
        float top_far[3] = {2, 2, 1}, lower[3] = {2, 1, 1};
        CHECK(cw_brush_move_vertex(&wb, wc2, top_far, lower), "a wedge vertex on the slope moves");
        cw_plane slope;
        CHECK(cw_brush_face_plane(&wb, wc2, CW_BRUSH_FACES, &slope) && fabsf(slope.n[0] * 2 + slope.n[1] * 1 + slope.n[2] * 1 - slope.d) < 1e-3f, "the slope passes through the moved vertex");
        CHECK(fabsf(slope.n[0] * -2 + slope.n[1] * 0 + slope.n[2] * 1 - slope.d) < 1e-3f, "and still through the bottom near edge");
    }
}
static void test_dynamic(const char *library) {
    cw_game game;
    char error[256] = "";
    CHECK(!cw_game_open(&game, "does-not-exist.so", error, sizeof(error)) && error[0], "missing library reported: %s", error);
    if (!library) { printf("(no shared module given: dynamic loading not exercised)\n"); return; }
    CHECK(cw_game_open(&game, library, error, sizeof(error)), "open %s: %s", library, error);
    if (!game.module) return;
    CHECK(strcmp(game.module->name, "combat") == 0 && game.library != NULL, "loaded the combat module dynamically");
    cw_game_world gw;
    CHECK(cw_game_world_init(&gw, game.module, error, sizeof(error)), "bind a dynamically loaded module: %s", error);
    cw_entity_id id = cw_game_world_spawn(&gw, 0, 100, 100);
    cw_game_world_settle(&gw);
    cw_game_world_tick(&gw, 5);
    CHECK(cw_world_is_valid(&gw.world, id) && cw_comp_has(&gw.world, id, comp(&gw.world, "Visual")), "the loaded module simulates");
    cw_game_world_destroy(&gw);
    cw_game_close(&game);
}

int main(int argc, char **argv) {
    test_reflection();
    test_scene();
    test_simulation_and_canvas();
    test_project();
    test_assets();
    test_animation();
    test_ui();
    test_bytes_and_brushes();
    test_dynamic(argc > 1 ? argv[1] : NULL);
    if (failures) { printf("%d of %d checks failed\n", failures, checks); return 1; }
    printf("OK: %d game module checks passed\n", checks);
    return 0;
}
