/* A starter game module, created by the launcher. Everything a game needs
 * lives in this file: components described with reflection (so the editor
 * can place and edit them and scenes can store them), systems, the prefabs
 * that appear in the editor's palette, and how entities are drawn through
 * the host's canvas. Build it with the editor (F7) or tools/build-game.sh.
 *
 * Ideas to grow from here: add fields (they appear in the inspector and in
 * scenes automatically), add components and prefabs, add systems in other
 * phases (CW_EARLY_UPDATE, CW_LATE_UPDATE, CW_ENTITY_CREATED...), use
 * cw_commands for deferred changes, and lights through canvas->light.
 * Separate authored data from runtime state with CW_CONFIG_TYPE (edited in
 * the editor, stored in scenes) and CW_RUNTIME_TYPE (generated from it by a
 * bake function when the world runs; never in scenes): see HealthConfig ->
 * Health in games/combat/combat_game.c.
 * Textured entities need no drawing code: the engine's Sprite component
 * (a material asset, edited in the editor's material panel) is drawn by the
 * host; the starter scene's ball carries one over its circle. */
#include "cw_game.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define LEVEL_W 960.0f
#define LEVEL_H 540.0f
#define TICK_HZ 60

/* --- components --- */
typedef struct { float x, y; } Position;
typedef struct { float radius; uint8_t r, g, b, kind; uint32_t sprite; } Shape;   /* kind: circle or square; sprite: an imported asset (id) */
typedef struct { float vx, vy; bool bounce; } Mover;             /* moves every tick; bounces off the level bounds */

enum { POSITION, SHAPE, MOVER, COMPONENT_COUNT };
static const char *const shape_kinds[] = {"circle", "square"};

static const cw_field position_fields[] = {
    CW_FIELD_SEM(Position, x, CW_FIELD_F32, CW_SEM_POSITION_X),
    CW_FIELD_SEM(Position, y, CW_FIELD_F32, CW_SEM_POSITION_Y),
};
static const cw_field shape_fields[] = {
    CW_FIELD_RANGE_SEM(Shape, radius, CW_FIELD_F32, 2, 200, 2, CW_SEM_RADIUS),
    CW_FIELD_SEM(Shape, r, CW_FIELD_U8, CW_SEM_COLOR_R),
    CW_FIELD_SEM(Shape, g, CW_FIELD_U8, CW_SEM_COLOR_G),
    CW_FIELD_SEM(Shape, b, CW_FIELD_U8, CW_SEM_COLOR_B),
    CW_FIELD_NAMED(Shape, kind, CW_FIELD_ENUM, shape_kinds, 2),
    /* an asset reference: the editor offers the project's assets, scenes store @name,
     * and cw_assets_path(cw_assets_of(w), s->sprite, ...) resolves it at run time */
    CW_FIELD_SEM(Shape, sprite, CW_FIELD_U32, CW_SEM_ASSET),
};
static const cw_field mover_fields[] = {
    CW_FIELD_RANGE(Mover, vx, CW_FIELD_F32, -1000, 1000, 10),
    CW_FIELD_RANGE(Mover, vy, CW_FIELD_F32, -1000, 1000, 10),
    CW_FIELD(Mover, bounce, CW_FIELD_BOOL),
};
static const Shape shape_defaults = {16, 120, 200, 255, 0, 0};
static const Mover mover_defaults = {120, 80, true};
static const cw_type_info types[COMPONENT_COUNT] = {
    [POSITION] = CW_TYPE(Position, position_fields, NULL, NULL, 0),
    [SHAPE] = CW_TYPE(Shape, shape_fields, &shape_defaults, NULL, 0),
    [MOVER] = CW_TYPE(Mover, mover_fields, &mover_defaults, NULL, 0),
};
static bool register_components(cw_world *w) {
    if (cw_comp_block(w, COMPONENT_COUNT) != 0) return false;
    for (unsigned i = 0; i < COMPONENT_COUNT; ++i)
        if (!cw_reflect_register(w, (cw_component_id)i, &types[i])) return false;
    return true;
}

/* --- systems --- */
static void sys_move(cw_world *w, cw_phase phase, float dt) {
    (void)phase;
    const cw_component_id parts[] = {POSITION, MOVER};
    cw_query q = cw_query_iter(w, cw_mask(parts, 2));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        Position *p = cw_comp_ptr(w, id, POSITION);
        Mover *m = cw_comp_ptr(w, id, MOVER);
        float r = cw_comp_has(w, id, SHAPE) ? ((Shape *)cw_comp_ptr(w, id, SHAPE))->radius : 0;
        p->x += m->vx * dt;
        p->y += m->vy * dt;
        if (!m->bounce) continue;
        if (p->x < r) { p->x = r; m->vx = fabsf(m->vx); }
        if (p->x > LEVEL_W - r) { p->x = LEVEL_W - r; m->vx = -fabsf(m->vx); }
        if (p->y < r) { p->y = r; m->vy = fabsf(m->vy); }
        if (p->y > LEVEL_H - r) { p->y = LEVEL_H - r; m->vy = -fabsf(m->vy); }
    }
}
/* The interface: the scene's "bounce" button (UiButton action 1, built in
 * the editor's scene panel) reverses every mover; the engine lays the
 * elements out, draws them and tracks the clicks. */
enum { ACTION_BOUNCE = 1 };
static void sys_interface(cw_world *w, cw_phase phase, float dt) {
    (void)phase; (void)dt;
    if (!cw_game_ui_clicked(w, ACTION_BOUNCE)) return;
    cw_query q = cw_query_iter(w, cw_mask(&(const cw_component_id){MOVER}, 1));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        Mover *m = cw_comp_ptr(w, id, MOVER);
        m->vx = -m->vx;
        m->vy = -m->vy;
    }
}
static bool register_systems(cw_sim *sim) {
    const cw_system_desc systems[] = {
        {"move", CW_PHASE(CW_UPDATE), sys_move, CW_SHARED, 0, false},
        {"interface", CW_PHASE(CW_UPDATE), sys_interface, CW_SHARED, 1, false},
    };
    for (size_t i = 0; i < sizeof(systems) / sizeof(systems[0]); ++i)
        if (!cw_sim_register(sim, systems[i])) return false;
    return true;
}

/* --- prefabs --- */
static bool spawn_ball(cw_world *w, cw_entity_id id, float x, float y) {
    Position p = {x, y};
    Shape s = shape_defaults;
    Mover m = mover_defaults;
    return cw_comp_set(w, id, POSITION, &p, sizeof(p)) && cw_comp_set(w, id, SHAPE, &s, sizeof(s))
        && cw_comp_set(w, id, MOVER, &m, sizeof(m));
}
static bool spawn_block(cw_world *w, cw_entity_id id, float x, float y) {
    Position p = {x, y};
    Shape s = {24, 230, 160, 90, 1, 0};
    return cw_comp_set(w, id, POSITION, &p, sizeof(p)) && cw_comp_set(w, id, SHAPE, &s, sizeof(s));
}
static const cw_prefab prefabs[] = {
    {"ball", "things", spawn_ball},
    {"block", "things", spawn_block},
};

/* --- drawing --- */
static void draw(cw_world *w, const cw_canvas *c, const cw_draw_context *ctx) {
    c->rect(c->impl, 0, 0, LEVEL_W, LEVEL_H, cw_rgb(30, 32, 40));
    c->rect_outline(c->impl, 0, 0, LEVEL_W, LEVEL_H, c->px(c->impl, 2), cw_rgb(90, 96, 110));
    const cw_component_id parts[] = {POSITION, SHAPE};
    cw_query q = cw_query_iter(w, cw_mask(parts, 2));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        const Position *p = cw_comp_ptr(w, id, POSITION);
        const Shape *s = cw_comp_ptr(w, id, SHAPE);
        cw_rgba col = cw_rgb(s->r, s->g, s->b);
        if (s->kind == 1) c->rect(c->impl, p->x - s->radius, p->y - s->radius, s->radius * 2, s->radius * 2, col);
        else c->circle(c->impl, p->x, p->y, s->radius, col);
        if (ctx->editing && cw_comp_has(w, id, MOVER)) {
            const Mover *m = cw_comp_ptr(w, id, MOVER);
            c->line(c->impl, p->x, p->y, p->x + m->vx * 0.25f, p->y + m->vy * 0.25f, c->px(c->impl, 1), cw_rgba_alpha(col, 160));
        }
        const cw_asset *sprite = cw_assets_find(cw_assets_of(w), s->sprite);
        if (sprite && ctx->editing)   /* the asset this shape refers to; a renderer with images would draw it */
            c->text(c->impl, p->x + s->radius + c->px(c->impl, 4), p->y - c->px(c->impl, 6), cw_rgba_alpha(col, 200), sprite->name);
        c->pick(c->impl, id, p->x, p->y, s->radius + 2);
    }
}
static void describe(cw_world *w, cw_entity_id id, char *out, size_t capacity) {
    if (!cw_comp_has(w, id, SHAPE)) { snprintf(out, capacity, "entity"); return; }
    const Shape *s = cw_comp_ptr(w, id, SHAPE);
    snprintf(out, capacity, "%s%s", cw_comp_has(w, id, MOVER) ? "moving " : "", shape_kinds[s->kind < 2 ? s->kind : 0]);
}

/* --- the module table --- */
static const cw_game_module module = {
    CW_GAME_ABI_VERSION,
    "starter",                 /* the game's name: stored in scenes, checked on load */
    1,                         /* scene version: bump on incompatible schema changes */
    TICK_HZ,
    {0, 0, LEVEL_W, LEVEL_H},
    register_components, register_systems,
    NULL, NULL,                /* bind/unbind: per-world state, none needed here */
    prefabs, sizeof(prefabs) / sizeof(prefabs[0]),
    draw, describe,
};
CW_GAME_API const cw_game_module *cw_game_export(void) { return &module; }
