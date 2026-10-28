/* Orbit: a small gravity sandbox, the second game module.
 *
 * It exists to show that the editor and the player are generic: this game
 * shares nothing with combat but the engine. Its components, prefabs and
 * look come entirely from this file, and the same editor binary edits both.
 *
 * Bodies (stars, planets) attract ships; ships integrate velocity, wrap at
 * the level bounds and leave a fading trail.
 */
#include "cw_game.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define LEVEL_W 1600.0f
#define LEVEL_H 1000.0f
#define TICK_HZ 60
#define TRAIL_POINTS 24

typedef struct { float x, y; } Position;
typedef struct { float x, y; } Velocity;
typedef struct { float mass, radius; uint8_t r, g, b; bool fixed; } Body;   /* attracts ships; fixed bodies do not move */
typedef struct { float hull; uint8_t r, g, b; } Ship;                       /* hull: integrity; 0 destroys the ship */
typedef struct { Position points[TRAIL_POINTS]; uint8_t next; uint8_t count; } Trail;   /* transient */

enum { POSITION, VELOCITY, BODY, SHIP, TRAIL, COMPONENT_COUNT };

static const cw_field position_fields[] = {
    CW_FIELD_SEM(Position, x, CW_FIELD_F32, CW_SEM_POSITION_X),
    CW_FIELD_SEM(Position, y, CW_FIELD_F32, CW_SEM_POSITION_Y),
};
static const cw_field velocity_fields[] = {
    CW_FIELD_RANGE(Velocity, x, CW_FIELD_F32, -2000, 2000, 10),
    CW_FIELD_RANGE(Velocity, y, CW_FIELD_F32, -2000, 2000, 10),
};
static const cw_field body_fields[] = {
    CW_FIELD_RANGE(Body, mass, CW_FIELD_F32, 0, 1000000, 1000),
    CW_FIELD_RANGE_SEM(Body, radius, CW_FIELD_F32, 2, 300, 2, CW_SEM_RADIUS),
    CW_FIELD_SEM(Body, r, CW_FIELD_U8, CW_SEM_COLOR_R),
    CW_FIELD_SEM(Body, g, CW_FIELD_U8, CW_SEM_COLOR_G),
    CW_FIELD_SEM(Body, b, CW_FIELD_U8, CW_SEM_COLOR_B),
    CW_FIELD(Body, fixed, CW_FIELD_BOOL),
};
static const cw_field ship_fields[] = {
    CW_FIELD_RANGE(Ship, hull, CW_FIELD_F32, 0, 100, 5),
    CW_FIELD_SEM(Ship, r, CW_FIELD_U8, CW_SEM_COLOR_R),
    CW_FIELD_SEM(Ship, g, CW_FIELD_U8, CW_SEM_COLOR_G),
    CW_FIELD_SEM(Ship, b, CW_FIELD_U8, CW_SEM_COLOR_B),
};
static const cw_field trail_fields[] = { CW_FIELD(Trail, next, CW_FIELD_U8), CW_FIELD(Trail, count, CW_FIELD_U8) };
static const Body body_defaults = {20000, 30, 200, 180, 120, true};
static const Ship ship_defaults = {100, 120, 220, 255};
static const cw_type_info types[COMPONENT_COUNT] = {
    [POSITION] = CW_TYPE(Position, position_fields, NULL, NULL, 0),
    [VELOCITY] = CW_TYPE(Velocity, velocity_fields, NULL, NULL, 0),
    [BODY] = CW_TYPE(Body, body_fields, &body_defaults, NULL, 0),
    [SHIP] = CW_TYPE(Ship, ship_fields, &ship_defaults, NULL, 0),
    [TRAIL] = CW_TYPE(Trail, trail_fields, NULL, NULL, CW_TYPE_TRANSIENT),
};
static bool register_components(cw_world *w) {
    if (cw_comp_block(w, COMPONENT_COUNT) != 0) return false;
    for (unsigned i = 0; i < COMPONENT_COUNT; ++i)
        if (!cw_reflect_register(w, (cw_component_id)i, &types[i])) return false;
    return true;
}

/* --- systems --- */
static void sys_gravity(cw_world *w, cw_phase phase, float dt) {
    (void)phase;
    const cw_component_id moving[] = {POSITION, VELOCITY};
    const cw_component_id bodies[] = {POSITION, BODY};
    cw_query q = cw_query_iter(w, cw_mask(moving, 2));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        if (cw_comp_has(w, id, BODY) && ((Body *)cw_comp_ptr(w, id, BODY))->fixed) continue;
        const Position *p = cw_comp_ptr(w, id, POSITION);
        Velocity *v = cw_comp_ptr(w, id, VELOCITY);
        cw_query b = cw_query_iter(w, cw_mask(bodies, 2));
        cw_entity_id other;
        while (cw_query_next(&b, &other)) {
            if (other == id) continue;
            const Position *bp = cw_comp_ptr(w, other, POSITION);
            const Body *body = cw_comp_ptr(w, other, BODY);
            float dx = bp->x - p->x, dy = bp->y - p->y, d2 = dx * dx + dy * dy + 100.0f, d = sqrtf(d2);
            float a = body->mass / d2;
            v->x += dx / d * a * dt;
            v->y += dy / d * a * dt;
            if (cw_comp_has(w, id, SHIP) && d < body->radius) {   /* crashed into it */
                Ship *s = cw_comp_ptr(w, id, SHIP);
                s->hull -= 200.0f * dt;
            }
        }
    }
}
static void sys_integrate(cw_world *w, cw_phase phase, float dt) {
    (void)phase;
    const cw_component_id moving[] = {POSITION, VELOCITY};
    cw_query q = cw_query_iter(w, cw_mask(moving, 2));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        if (cw_comp_has(w, id, BODY) && ((Body *)cw_comp_ptr(w, id, BODY))->fixed) continue;
        Position *p = cw_comp_ptr(w, id, POSITION);
        const Velocity *v = cw_comp_ptr(w, id, VELOCITY);
        p->x += v->x * dt;
        p->y += v->y * dt;
        if (p->x < 0) p->x += LEVEL_W;
        if (p->x >= LEVEL_W) p->x -= LEVEL_W;
        if (p->y < 0) p->y += LEVEL_H;
        if (p->y >= LEVEL_H) p->y -= LEVEL_H;
        if (cw_comp_has(w, id, TRAIL)) {
            Trail *t = cw_comp_ptr(w, id, TRAIL);
            if (w->tick % 3 == 0) {
                t->points[t->next] = *p;
                t->next = (uint8_t)((t->next + 1) % TRAIL_POINTS);
                if (t->count < TRAIL_POINTS) ++t->count;
            }
        }
        if (cw_comp_has(w, id, SHIP) && ((Ship *)cw_comp_ptr(w, id, SHIP))->hull <= 0) cw_world_mark_destroy(w, id);
    }
}
static void sys_created(cw_world *w, cw_phase phase, float dt) {
    (void)phase; (void)dt;
    cw_id_view created = cw_world_created(w);
    for (size_t i = 0; i < created.count; ++i)
        if (cw_world_is_valid(w, created.data[i]) && cw_comp_has(w, created.data[i], SHIP)) cw_comp_add(w, created.data[i], TRAIL);
}
static bool register_systems(cw_sim *sim) {
    const cw_system_desc systems[] = {
        {"gravity", CW_PHASE(CW_UPDATE), sys_gravity, CW_SHARED, 0, false},
        {"integrate", CW_PHASE(CW_UPDATE), sys_integrate, CW_SHARED, 10, false},
        {"created", CW_PHASE(CW_ENTITY_CREATED), sys_created, CW_SHARED, 0, true},
    };
    for (size_t i = 0; i < sizeof(systems) / sizeof(systems[0]); ++i)
        if (!cw_sim_register(sim, systems[i])) return false;
    return true;
}

/* --- prefabs --- */
static bool spawn_body(cw_world *w, cw_entity_id id, Body body, float x, float y) {
    Position p = {x, y};
    Velocity v = {0, 0};
    return cw_comp_set(w, id, POSITION, &p, sizeof(p)) && cw_comp_set(w, id, VELOCITY, &v, sizeof(v))
        && cw_comp_set(w, id, BODY, &body, sizeof(body));
}
static bool spawn_star(cw_world *w, cw_entity_id id, float x, float y) {
    Body b = {120000, 48, 255, 220, 120, true};
    return spawn_body(w, id, b, x, y);
}
static bool spawn_planet(cw_world *w, cw_entity_id id, float x, float y) {
    Body b = {20000, 22, 120, 170, 255, false};
    if (!spawn_body(w, id, b, x, y)) return false;
    Velocity v = {0, 120};
    return cw_comp_set(w, id, VELOCITY, &v, sizeof(v));
}
static bool spawn_moon(cw_world *w, cw_entity_id id, float x, float y) {
    Body b = {3000, 10, 200, 200, 210, false};
    if (!spawn_body(w, id, b, x, y)) return false;
    Velocity v = {0, 180};
    return cw_comp_set(w, id, VELOCITY, &v, sizeof(v));
}
static bool spawn_ship(cw_world *w, cw_entity_id id, float x, float y) {
    Position p = {x, y};
    Velocity v = {0, 200};
    Ship s = ship_defaults;
    return cw_comp_set(w, id, POSITION, &p, sizeof(p)) && cw_comp_set(w, id, VELOCITY, &v, sizeof(v))
        && cw_comp_set(w, id, SHIP, &s, sizeof(s));
}
static const cw_prefab prefabs[] = {
    {"star", "bodies", spawn_star},
    {"planet", "bodies", spawn_planet},
    {"moon", "bodies", spawn_moon},
    {"ship", "ships", spawn_ship},
};

/* --- drawing --- */
static void draw(cw_world *w, const cw_canvas *c, const cw_draw_context *ctx) {
    c->rect(c->impl, 0, 0, LEVEL_W, LEVEL_H, cw_rgb(12, 12, 20));
    c->rect_outline(c->impl, 0, 0, LEVEL_W, LEVEL_H, c->px(c->impl, 1), cw_rgb(60, 60, 90));
    cw_entity_id id;
    const cw_component_id bodies[] = {POSITION, BODY};
    cw_query q = cw_query_iter(w, cw_mask(bodies, 2));
    while (cw_query_next(&q, &id)) {
        const Position *p = cw_comp_ptr(w, id, POSITION);
        const Body *b = cw_comp_ptr(w, id, BODY);
        cw_rgba col = cw_rgb(b->r, b->g, b->b);
        c->circle(c->impl, p->x, p->y, b->radius * 1.6f, cw_rgba_alpha(col, 30));
        c->circle(c->impl, p->x, p->y, b->radius, col);
        if (b->fixed) {
            cw_canvas_light l = {p->x, p->y, b->radius * 12, col, 2.0f, 2.0f, 1.0f, false, true, 0, 0};
            c->light(c->impl, &l);
        }
        if (ctx->editing) {
            char label[48];
            snprintf(label, sizeof(label), "mass %.0f%s", (double)b->mass, b->fixed ? " (fixed)" : "");
            c->text(c->impl, p->x + b->radius + c->px(c->impl, 4), p->y - c->px(c->impl, 6), cw_rgba_alpha(col, 200), label);
        }
        c->pick(c->impl, id, p->x, p->y, b->radius + 2);
    }
    const cw_component_id ships[] = {POSITION, SHIP};
    q = cw_query_iter(w, cw_mask(ships, 2));
    while (cw_query_next(&q, &id)) {
        const Position *p = cw_comp_ptr(w, id, POSITION);
        const Ship *s = cw_comp_ptr(w, id, SHIP);
        cw_rgba col = cw_rgb(s->r, s->g, s->b);
        if (cw_comp_has(w, id, TRAIL)) {
            const Trail *t = cw_comp_ptr(w, id, TRAIL);
            for (uint8_t i = 1; i < t->count; ++i) {
                const Position *a = &t->points[(t->next + TRAIL_POINTS - i) % TRAIL_POINTS];
                const Position *b = &t->points[(t->next + TRAIL_POINTS - i - 1) % TRAIL_POINTS];
                if (i + 1 >= t->count) break;
                float dx = a->x - b->x, dy = a->y - b->y;
                if (dx * dx + dy * dy > 200 * 200) continue;   /* wrapped */
                c->line(c->impl, a->x, a->y, b->x, b->y, c->px(c->impl, 1), cw_rgba_alpha(col, (uint8_t)(180 - i * 7)));
            }
        }
        float dx = 1, dy = 0;
        if (cw_comp_has(w, id, VELOCITY)) {
            const Velocity *v = cw_comp_ptr(w, id, VELOCITY);
            float len = sqrtf(v->x * v->x + v->y * v->y);
            if (len > 0.01f) { dx = v->x / len; dy = v->y / len; }
        }
        float r = 8;
        c->triangle(c->impl, p->x + dx * r * 1.4f, p->y + dy * r * 1.4f, p->x - dx * r - dy * r * 0.8f, p->y - dy * r + dx * r * 0.8f,
                    p->x - dx * r + dy * r * 0.8f, p->y - dy * r - dx * r * 0.8f, col);
        c->bar(c->impl, p->x - 10, p->y - 16, 20, 3, s->hull / 100.0f, cw_rgb(120, 230, 140), cw_rgba_make(0, 0, 0, 160));
        c->pick(c->impl, id, p->x, p->y, r + 2);
    }
}
static void describe(cw_world *w, cw_entity_id id, char *out, size_t capacity) {
    if (cw_comp_has(w, id, SHIP)) snprintf(out, capacity, "ship");
    else if (cw_comp_has(w, id, BODY)) snprintf(out, capacity, ((Body *)cw_comp_ptr(w, id, BODY))->fixed ? "star" : "planet");
    else snprintf(out, capacity, "entity");
}

static const cw_game_module module = {
    CW_GAME_ABI_VERSION, "orbit", 1, TICK_HZ, {0, 0, LEVEL_W, LEVEL_H},
    register_components, register_systems, NULL, NULL,
    prefabs, sizeof(prefabs) / sizeof(prefabs[0]), draw, describe,
};
CW_GAME_API const cw_game_module *cw_game_export(void) { return &module; }
