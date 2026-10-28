/* Combat: a two-team battle, packaged as a Clockwork game module.
 *
 * Units move toward the nearest enemy, attack when in range, lose health
 * and die; lights are entities too, animated by systems and rendered by the
 * host; obstacles are static geometry that units walk around.
 *
 * This file is the whole game: components (described with reflection so the
 * editor, the scene format and the snapshot codec understand them; Health is
 * a runtime component generated from the authored HealthConfig when the
 * world runs), systems,
 * the prefabs the editor can place, and how everything is drawn through the
 * host's canvas. It depends on the engine headers only: no renderer, no
 * window, no editor. It is compiled either as a shared library the editor
 * and player load at run time, or linked statically into an exported game.
 */
#include "cw_game.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_W 1200.0f
#define ARENA_H 800.0f
#define TICK_HZ 30
#define SCENE_VERSION 1
#define PI_F 3.14159265358979f

/* ===========================================================================
 * Components
 * ======================================================================== */
typedef struct { float x, y; } Position;
typedef struct { float x, y; } Velocity;
typedef struct { int32_t maximum; } HealthConfig;           /* authored: how much health a unit starts with */
typedef struct { int32_t current, maximum; } Health;         /* runtime: generated from HealthConfig when the world runs */
typedef struct { int32_t damage; float range, cooldown, timer; int32_t kills; } Attack;
typedef struct { uint8_t team, kind; } Unit;
typedef struct { float radius, speed; } Body;
typedef struct { cw_entity_id id; } Target;                 /* transient: rebuilt every tick */
typedef struct { Position previous; float flash; } Visual;  /* transient: render-only state */
typedef struct {
    uint8_t r, g, b, flags;
    float intensity, radius, falloff, volumetric;
    float direction, cone;          /* spot lights */
    float flicker, flicker_speed;   /* intensity wobble: amount (0..1) and rate */
} Light;
enum { LIGHT_SPOT = 1, LIGHT_NO_SHADOWS = 2 };
typedef struct { float intensity; } LightState;   /* transient: animated intensity */
typedef struct { float remaining, total; } Lifetime;
typedef struct { cw_entity_id parent; float offset_x, offset_y; bool follow_facing; } Attached;
typedef struct { float radius; } Obstacle;
typedef struct { uint8_t team; } Scoreboard;   /* on a UiText: the engine's interface shows this team's standing */

enum { POSITION, VELOCITY, HEALTH, ATTACK, UNIT, BODY, TARGET, VISUAL,
       LIGHT, LIGHT_STATE, LIFETIME, ATTACHED, OBSTACLE, SHADOW_CASTER, HEALTH_CONFIG, SCOREBOARD, COMPONENT_COUNT };
enum { TEAM_RED, TEAM_BLUE, TEAM_COUNT };
enum { KIND_MELEE, KIND_ARCHER, KIND_TANK, KIND_COUNT };
static const char *const team_names[TEAM_COUNT] = {"red", "blue"};
static const char *const kind_names[KIND_COUNT] = {"melee", "archer", "tank"};
static const char *const light_flag_names[] = {"spot", "no_shadows"};

/* --- reflection --- */
static const cw_field position_fields[] = {
    CW_FIELD_SEM(Position, x, CW_FIELD_F32, CW_SEM_POSITION_X),
    CW_FIELD_SEM(Position, y, CW_FIELD_F32, CW_SEM_POSITION_Y),
};
static const cw_field velocity_fields[] = {
    CW_FIELD_SEM(Velocity, x, CW_FIELD_F32, CW_SEM_RUNTIME),
    CW_FIELD_SEM(Velocity, y, CW_FIELD_F32, CW_SEM_RUNTIME),
};
static const cw_field health_config_fields[] = {
    CW_FIELD_RANGE(HealthConfig, maximum, CW_FIELD_I32, 1, 100000, 10),
};
static const cw_field health_fields[] = {
    CW_FIELD_RANGE(Health, current, CW_FIELD_I32, 0, 100000, 10),
    CW_FIELD_RANGE(Health, maximum, CW_FIELD_I32, 1, 100000, 10),
};
static bool health_valid(const void *value) {
    const Health *h = value;
    return h->current <= h->maximum;
}
/* config -> runtime: a unit enters the world at full health */
static void bake_health(cw_world *w, cw_entity_id id, const void *config, void *runtime) {
    (void)w; (void)id;
    const HealthConfig *c = config;
    Health *h = runtime;
    h->maximum = c->maximum;
    h->current = c->maximum;
}
static const cw_field attack_fields[] = {
    CW_FIELD_RANGE(Attack, damage, CW_FIELD_I32, 0, 100000, 1),
    CW_FIELD_RANGE(Attack, range, CW_FIELD_F32, 0, 2000, 10),
    CW_FIELD_RANGE(Attack, cooldown, CW_FIELD_F32, 0.05f, 10, 0.1f),
    CW_FIELD_RANGE_SEM(Attack, timer, CW_FIELD_F32, 0, 10, 0.1f, CW_SEM_RUNTIME),
    CW_FIELD_RANGE_SEM(Attack, kills, CW_FIELD_I32, 0, 1000000, 1, CW_SEM_RUNTIME),
};
static const cw_field unit_fields[] = {
    CW_FIELD_NAMED(Unit, team, CW_FIELD_ENUM, team_names, TEAM_COUNT),
    CW_FIELD_NAMED(Unit, kind, CW_FIELD_ENUM, kind_names, KIND_COUNT),
};
static const cw_field body_fields[] = {
    CW_FIELD_RANGE_SEM(Body, radius, CW_FIELD_F32, 1, 200, 1, CW_SEM_RADIUS),
    CW_FIELD_RANGE(Body, speed, CW_FIELD_F32, 0, 2000, 10),
};
static const cw_field target_fields[] = { CW_FIELD(Target, id, CW_FIELD_ENTITY) };
static const cw_field visual_fields[] = {
    CW_FIELD(Visual, previous.x, CW_FIELD_F32),
    CW_FIELD(Visual, previous.y, CW_FIELD_F32),
    CW_FIELD(Visual, flash, CW_FIELD_F32),
};
static const cw_field light_fields[] = {
    CW_FIELD_SEM(Light, r, CW_FIELD_U8, CW_SEM_COLOR_R),
    CW_FIELD_SEM(Light, g, CW_FIELD_U8, CW_SEM_COLOR_G),
    CW_FIELD_SEM(Light, b, CW_FIELD_U8, CW_SEM_COLOR_B),
    CW_FIELD_NAMED(Light, flags, CW_FIELD_FLAGS, light_flag_names, 2),
    CW_FIELD_RANGE(Light, intensity, CW_FIELD_F32, 0, 10, 0.2f),
    CW_FIELD_RANGE(Light, radius, CW_FIELD_F32, 20, 3000, 20),
    CW_FIELD_RANGE(Light, falloff, CW_FIELD_F32, 0.25f, 6, 0.25f),
    CW_FIELD_RANGE(Light, volumetric, CW_FIELD_F32, 0, 5, 0.2f),
    CW_FIELD_SEM(Light, direction, CW_FIELD_F32, CW_SEM_ANGLE),
    CW_FIELD_RANGE(Light, cone, CW_FIELD_F32, 0.1f, 6.2f, 0.15f),
    CW_FIELD_RANGE(Light, flicker, CW_FIELD_F32, 0, 1, 0.1f),
    CW_FIELD_RANGE(Light, flicker_speed, CW_FIELD_F32, 0, 60, 1),
};
static const Light light_defaults = {255, 255, 255, 0, 1.5f, 300, 2.0f, 1.0f, 0, 1.0f, 0, 6.0f};
static const cw_field light_state_fields[] = { CW_FIELD(LightState, intensity, CW_FIELD_F32) };
static const cw_field lifetime_fields[] = {
    CW_FIELD_RANGE(Lifetime, remaining, CW_FIELD_F32, -1, 3600, 0.1f),
    CW_FIELD_RANGE(Lifetime, total, CW_FIELD_F32, 0.01f, 3600, 0.1f),
};
static const Lifetime lifetime_defaults = {1, 1};
static const cw_field attached_fields[] = {
    CW_FIELD(Attached, parent, CW_FIELD_ENTITY),
    CW_FIELD_RANGE(Attached, offset_x, CW_FIELD_F32, -500, 500, 1),
    CW_FIELD_RANGE(Attached, offset_y, CW_FIELD_F32, -500, 500, 1),
    CW_FIELD(Attached, follow_facing, CW_FIELD_BOOL),
};
static const Attached attached_defaults = {CW_INVALID_ENTITY, 0, 0, true};
static const cw_field obstacle_fields[] = { CW_FIELD_RANGE_SEM(Obstacle, radius, CW_FIELD_F32, 1, 500, 2, CW_SEM_RADIUS) };
static const Obstacle obstacle_defaults = {30};
static const Health health_defaults = {100, 100};
static const HealthConfig health_config_defaults = {100};
static const Attack attack_defaults = {10, 30, 1, 0, 0};
static const Body body_defaults = {12, 80};

static const cw_field scoreboard_fields[] = {
    CW_FIELD_NAMED(Scoreboard, team, CW_FIELD_ENUM, team_names, TEAM_COUNT),
};
static const cw_type_info types[COMPONENT_COUNT] = {
    [POSITION] = CW_TYPE(Position, position_fields, NULL, NULL, 0),
    [VELOCITY] = CW_TYPE(Velocity, velocity_fields, NULL, NULL, CW_TYPE_INTERNAL),
    [HEALTH] = CW_RUNTIME_TYPE(Health, health_fields, &health_defaults, health_valid),
    [HEALTH_CONFIG] = CW_CONFIG_TYPE(HealthConfig, health_config_fields, &health_config_defaults, NULL, "Health", bake_health),
    [ATTACK] = CW_TYPE(Attack, attack_fields, &attack_defaults, NULL, 0),
    [UNIT] = CW_TYPE(Unit, unit_fields, NULL, NULL, 0),
    [BODY] = CW_TYPE(Body, body_fields, &body_defaults, NULL, 0),
    [TARGET] = CW_TYPE(Target, target_fields, NULL, NULL, CW_TYPE_TRANSIENT),
    [VISUAL] = CW_TYPE(Visual, visual_fields, NULL, NULL, CW_TYPE_TRANSIENT),
    [LIGHT] = CW_TYPE(Light, light_fields, &light_defaults, NULL, 0),
    [LIGHT_STATE] = CW_TYPE(LightState, light_state_fields, NULL, NULL, CW_TYPE_TRANSIENT),
    [LIFETIME] = CW_TYPE(Lifetime, lifetime_fields, &lifetime_defaults, NULL, 0),
    [ATTACHED] = CW_TYPE(Attached, attached_fields, &attached_defaults, NULL, 0),
    [OBSTACLE] = CW_TYPE(Obstacle, obstacle_fields, &obstacle_defaults, NULL, 0),
    [SHADOW_CASTER] = CW_TAG_TYPE("ShadowCaster"),
    [SCOREBOARD] = CW_TYPE(Scoreboard, scoreboard_fields, NULL, NULL, 0),
};
static bool register_components(cw_world *w) {
    if (cw_comp_block(w, COMPONENT_COUNT) != 0) return false;
    for (unsigned i = 0; i < COMPONENT_COUNT; ++i)
        if (!cw_reflect_register(w, (cw_component_id)i, &types[i])) return false;
    return true;
}

/* ===========================================================================
 * Per-world state (world->user): scratch space and battle statistics
 * ======================================================================== */
typedef struct {
    cw_entity_id *scratch;
    size_t scratch_capacity;
    uint64_t kills[TEAM_COUNT], deaths[TEAM_COUNT], damage_dealt;
} CombatState;
static CombatState *state_of(cw_world *w) { return w->user; }
static bool bind(cw_world *w) {
    CombatState *s = calloc(1, sizeof(*s));
    if (!s) return false;
    w->user = s;
    return true;
}
static void unbind(cw_world *w) {
    CombatState *s = state_of(w);
    if (!s) return;
    free(s->scratch);
    free(s);
    w->user = NULL;
}
static cw_entity_id *scratch_ids(cw_world *w, size_t needed) {
    CombatState *s = state_of(w);
    if (needed > s->scratch_capacity) {
        size_t cap = s->scratch_capacity ? s->scratch_capacity : 256;
        while (cap < needed) cap *= 2;
        s->scratch = realloc(s->scratch, cap * sizeof(*s->scratch));
        if (!s->scratch) { fputs("combat: out of memory\n", stderr); abort(); }
        s->scratch_capacity = cap;
    }
    return s->scratch;
}

/* ===========================================================================
 * Systems
 * ======================================================================== */
typedef struct {
    const char *name;
    int32_t health, damage;
    float range, cooldown, speed, radius;
} UnitKind;
static const UnitKind kinds[KIND_COUNT] = {
    {"melee", 120, 12, 30.0f, 0.8f, 90.0f, 12.0f},
    {"archer", 70, 8, 170.0f, 1.1f, 75.0f, 10.0f},
    {"tank", 300, 7, 34.0f, 1.0f, 55.0f, 16.0f},
};

static float dist2(const Position *a, const Position *b) {
    float dx = a->x - b->x, dy = a->y - b->y;
    return dx * dx + dy * dy;
}
static size_t collect_units(cw_world *w, cw_entity_id **ids) {
    const cw_component_id required[] = {UNIT, POSITION, HEALTH, BODY};
    cw_signature mask = cw_mask(required, 4);
    size_t n = cw_query_collect(w, mask, NULL, 0);
    *ids = scratch_ids(w, n ? n : 1);
    return cw_query_collect(w, mask, *ids, n);
}
/* EarlyUpdate: remember where everything was for render interpolation. */
static void sys_visual_begin(cw_world *w, cw_phase phase, float dt) {
    (void)phase;
    const cw_component_id required[] = {POSITION, VISUAL};
    cw_query q = cw_query_iter(w, cw_mask(required, 2));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        Visual *v = cw_comp_ptr(w, id, VISUAL);
        v->previous = *(Position *)cw_comp_ptr(w, id, POSITION);
        v->flash = v->flash > dt ? v->flash - dt : 0;
    }
}
/* EarlyUpdate: every armed unit targets the nearest living enemy (lowest id wins ties). */
static void sys_targeting(cw_world *w, cw_phase phase, float dt) {
    (void)phase; (void)dt;
    cw_entity_id *units;
    size_t n = collect_units(w, &units);
    for (size_t i = 0; i < n; ++i) {
        cw_entity_id id = units[i];
        if (!cw_comp_has(w, id, ATTACK)) continue;
        const Unit *u = cw_comp_ptr(w, id, UNIT);
        const Position *p = cw_comp_ptr(w, id, POSITION);
        Target t = {CW_INVALID_ENTITY};
        float best = INFINITY;
        for (size_t j = 0; j < n; ++j) {
            const Unit *other = cw_comp_ptr(w, units[j], UNIT);
            if (other->team == u->team) continue;
            float d = dist2(p, cw_comp_ptr(w, units[j], POSITION));
            if (d < best) { best = d; t.id = units[j]; }
        }
        cw_comp_set(w, id, TARGET, &t, sizeof(t));
    }
}
/* Update: approach the target until in range, keep bodies apart, stay in the arena. */
static void sys_movement(cw_world *w, cw_phase phase, float dt) {
    (void)phase;
    cw_entity_id *units;
    size_t n = collect_units(w, &units);
    const cw_component_id obstacle_parts[] = {OBSTACLE, POSITION};
    cw_signature obstacle_mask = cw_mask(obstacle_parts, 2);
    size_t n_obstacles = cw_query_collect(w, obstacle_mask, NULL, 0);
    cw_entity_id *all = scratch_ids(w, n + n_obstacles + 1);   /* units first, then obstacles */
    units = all;
    cw_entity_id *obstacles = all + n;
    cw_query_collect(w, obstacle_mask, obstacles, n_obstacles);
    for (size_t i = 0; i < n; ++i) {
        cw_entity_id id = units[i];
        Position *p = cw_comp_ptr(w, id, POSITION);
        const Body *b = cw_comp_ptr(w, id, BODY);
        Position before = *p;
        const Target *t = cw_comp_has(w, id, TARGET) ? cw_comp_ptr(w, id, TARGET) : NULL;
        if (t && cw_world_is_valid(w, t->id) && cw_comp_has(w, t->id, POSITION)) {
            const Position *tp = cw_comp_ptr(w, t->id, POSITION);
            float stop = 0;
            if (cw_comp_has(w, id, ATTACK)) {
                stop = ((Attack *)cw_comp_ptr(w, id, ATTACK))->range * 0.9f;
                if (cw_comp_has(w, t->id, BODY)) stop += ((Body *)cw_comp_ptr(w, t->id, BODY))->radius;
            }
            float dx = tp->x - p->x, dy = tp->y - p->y, d = sqrtf(dx * dx + dy * dy);
            if (d > stop && d > 0) {
                float step = b->speed * dt;
                if (step > d - stop) step = d - stop;
                p->x += dx / d * step;
                p->y += dy / d * step;
            }
        }
        for (size_t j = 0; j < n; ++j) {
            if (j == i) continue;
            Position *q = cw_comp_ptr(w, units[j], POSITION);
            float min = b->radius + ((Body *)cw_comp_ptr(w, units[j], BODY))->radius;
            float dx = p->x - q->x, dy = p->y - q->y, d2 = dx * dx + dy * dy;
            if (d2 >= min * min) continue;
            float d = sqrtf(d2);
            float push = d > 1e-4f ? (min - d) * 0.5f : min * 0.5f;
            float nx = d > 1e-4f ? dx / d : 1.0f, ny = d > 1e-4f ? dy / d : 0.0f;
            p->x += nx * push;
            p->y += ny * push;
        }
        for (size_t k = 0; k < n_obstacles; ++k) {   /* static obstacles push units out */
            const Position *op = cw_comp_ptr(w, obstacles[k], POSITION);
            float min = b->radius + ((Obstacle *)cw_comp_ptr(w, obstacles[k], OBSTACLE))->radius;
            float dx = p->x - op->x, dy = p->y - op->y, d2 = dx * dx + dy * dy;
            if (d2 >= min * min) continue;
            float d = sqrtf(d2);
            float nx = d > 1e-4f ? dx / d : 1.0f, ny = d > 1e-4f ? dy / d : 0.0f;
            p->x = op->x + nx * min;
            p->y = op->y + ny * min;
        }
        if (p->x < b->radius) p->x = b->radius;
        if (p->y < b->radius) p->y = b->radius;
        if (p->x > ARENA_W - b->radius) p->x = ARENA_W - b->radius;
        if (p->y > ARENA_H - b->radius) p->y = ARENA_H - b->radius;
        if (cw_comp_has(w, id, VELOCITY)) {
            Velocity *v = cw_comp_ptr(w, id, VELOCITY);
            v->x = dt > 0 ? (p->x - before.x) / dt : 0;
            v->y = dt > 0 ? (p->y - before.y) / dt : 0;
        }
    }
}
/* Damage is applied when the Update phase's command buffer is flushed, so
 * several attackers hitting one unit in the same tick accumulate in order. */
typedef struct { int32_t amount; cw_entity_id attacker; } Damage;
static void apply_damage(cw_world *w, cw_entity_id id, cw_component_id ct, void *component, const void *data, size_t size) {
    (void)ct;
    Health *h = component;
    const Damage *d = data;
    if (size != sizeof(*d) || h->current <= 0) return;
    h->current -= d->amount;
    state_of(w)->damage_dealt += (uint64_t)d->amount;
    if (cw_comp_has(w, id, VISUAL)) ((Visual *)cw_comp_ptr(w, id, VISUAL))->flash = 0.2f;
    if (h->current <= 0) {
        h->current = 0;
        if (cw_world_is_valid(w, d->attacker) && cw_comp_has(w, d->attacker, ATTACK))
            ((Attack *)cw_comp_ptr(w, d->attacker, ATTACK))->kills += 1;
    }
}
/* Update (after movement): attack the target when in range and off cooldown. */
static void sys_combat(cw_world *w, cw_phase phase, float dt) {
    (void)phase;
    const cw_component_id required[] = {ATTACK, TARGET, POSITION};
    cw_query q = cw_query_iter(w, cw_mask(required, 3));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        Attack *a = cw_comp_ptr(w, id, ATTACK);
        if (a->timer > 0) a->timer -= dt;
        const Target *t = cw_comp_ptr(w, id, TARGET);
        if (a->timer > 0 || !cw_world_is_valid(w, t->id) || !cw_comp_has(w, t->id, HEALTH)) continue;
        if (!cw_comp_has(w, t->id, POSITION)) continue;
        float reach = a->range;
        if (cw_comp_has(w, t->id, BODY)) reach += ((Body *)cw_comp_ptr(w, t->id, BODY))->radius;
        if (dist2(cw_comp_ptr(w, id, POSITION), cw_comp_ptr(w, t->id, POSITION)) > reach * reach) continue;
        a->timer += a->cooldown;
        Damage d = {a->damage, id};
        cw_commands_update(cw_world_commands(w), t->id, HEALTH, apply_damage, &d, sizeof(d));
    }
}
/* LateUpdate: anything at zero health is destroyed at the end of the tick. */
static void sys_death(cw_world *w, cw_phase phase, float dt) {
    (void)phase; (void)dt;
    const cw_component_id required[] = {HEALTH};
    cw_query q = cw_query_iter(w, cw_mask(required, 1));
    cw_entity_id id;
    while (cw_query_next(&q, &id))
        if (((Health *)cw_comp_ptr(w, id, HEALTH))->current <= 0) cw_world_mark_destroy(w, id);
}
/* Update: animate light intensity (flicker from the tick, fade with lifetime). */
static float flicker_noise(uint64_t tick, float dt, float speed, float phase) {
    float t = (float)tick * dt * speed + phase;
    return 0.5f * sinf(t * 1.7f) * sinf(t * 0.63f + 1.3f) + 0.35f * sinf(t * 3.1f + phase) + 0.15f * sinf(t * 7.9f);
}
static void sys_light_animate(cw_world *w, cw_phase phase, float dt) {
    (void)phase;
    const cw_component_id required[] = {LIGHT};
    cw_query q = cw_query_iter(w, cw_mask(required, 1));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        const Light *l = cw_comp_ptr(w, id, LIGHT);
        LightState st = {l->intensity};
        if (l->flicker > 0) st.intensity *= 1.0f + l->flicker * flicker_noise(w->tick, dt, l->flicker_speed, (float)(id % 17) * 0.7f);
        if (cw_comp_has(w, id, LIFETIME)) {
            const Lifetime *life = cw_comp_ptr(w, id, LIFETIME);
            st.intensity *= life->total > 0 ? fmaxf(life->remaining, 0) / life->total : 0;
        }
        if (st.intensity < 0) st.intensity = 0;
        cw_comp_set(w, id, LIGHT_STATE, &st, sizeof(st));
    }
}
/* Update: lifetimes run out, the entity goes. */
static void sys_lifetime(cw_world *w, cw_phase phase, float dt) {
    (void)phase;
    const cw_component_id required[] = {LIFETIME};
    cw_query q = cw_query_iter(w, cw_mask(required, 1));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        Lifetime *l = cw_comp_ptr(w, id, LIFETIME);
        l->remaining -= dt;
        if (l->remaining <= 0) cw_world_mark_destroy(w, id);
    }
}
/* LateUpdate (after movement): attached entities follow their parent and
 * inherit its facing; they are destroyed when the parent is gone. */
static void sys_attach(cw_world *w, cw_phase phase, float dt) {
    (void)phase; (void)dt;
    const cw_component_id required[] = {ATTACHED, POSITION};
    cw_query q = cw_query_iter(w, cw_mask(required, 2));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        const Attached *a = cw_comp_ptr(w, id, ATTACHED);
        if (!cw_world_is_valid(w, a->parent) || (w->entities[a->parent].flags & CW_TO_DESTROY) || !cw_comp_has(w, a->parent, POSITION)) {
            cw_world_mark_destroy(w, id);
            continue;
        }
        const Position *pp = cw_comp_ptr(w, a->parent, POSITION);
        Position *p = cw_comp_ptr(w, id, POSITION);
        p->x = pp->x + a->offset_x;
        p->y = pp->y + a->offset_y;
        if (a->follow_facing && cw_comp_has(w, id, LIGHT)) {
            Light *l = cw_comp_ptr(w, id, LIGHT);
            bool aimed = false;
            if (cw_comp_has(w, a->parent, TARGET)) {
                const Target *t = cw_comp_ptr(w, a->parent, TARGET);
                if (cw_world_is_valid(w, t->id) && cw_comp_has(w, t->id, POSITION)) {
                    const Position *tp = cw_comp_ptr(w, t->id, POSITION);
                    l->direction = atan2f(tp->y - pp->y, tp->x - pp->x);
                    aimed = true;
                }
            }
            if (!aimed && cw_comp_has(w, a->parent, VELOCITY)) {
                const Velocity *v = cw_comp_ptr(w, a->parent, VELOCITY);
                if (v->x * v->x + v->y * v->y > 1.0f) l->direction = atan2f(v->y, v->x);
            }
        }
    }
}
/* EntityDestroyed: battle statistics. */
static void sys_destroyed(cw_world *w, cw_phase phase, float dt) {
    (void)phase; (void)dt;
    CombatState *s = state_of(w);
    cw_id_view pending = cw_world_pending_destroy(w);
    for (size_t i = 0; i < pending.count; ++i) {
        cw_entity_id id = pending.data[i];
        if (!cw_comp_has(w, id, UNIT) || !cw_comp_has(w, id, HEALTH)) continue;
        if (((Health *)cw_comp_ptr(w, id, HEALTH))->current > 0) continue;   /* deleted, not killed */
        uint8_t team = ((Unit *)cw_comp_ptr(w, id, UNIT))->team;
        s->deaths[team] += 1;
        s->kills[team == TEAM_RED ? TEAM_BLUE : TEAM_RED] += 1;
    }
}
/* EntityCreated: spawned and loaded entities get their transient render state. */
static void sys_created(cw_world *w, cw_phase phase, float dt) {
    (void)phase; (void)dt;
    cw_id_view created = cw_world_created(w);
    for (size_t i = 0; i < created.count; ++i) {
        cw_entity_id id = created.data[i];
        if (!cw_world_is_valid(w, id) || !cw_comp_has(w, id, POSITION)) continue;
        if (!cw_comp_has(w, id, VISUAL)) {
            Visual v = {*(Position *)cw_comp_ptr(w, id, POSITION), 0};
            cw_comp_set(w, id, VISUAL, &v, sizeof(v));
        }
        if (cw_comp_has(w, id, LIGHT) && !cw_comp_has(w, id, LIGHT_STATE)) {
            LightState st = {((Light *)cw_comp_ptr(w, id, LIGHT))->intensity};
            cw_comp_set(w, id, LIGHT_STATE, &st, sizeof(st));
        }
    }
}
/* The interface: every UiText with a Scoreboard shows its team's units alive and kills. */
static void sys_scoreboard(cw_world *w, cw_phase phase, float dt) {
    (void)phase; (void)dt;
    cw_game_world *gw = cw_game_world_of(w);
    if (!gw) return;
    int alive[TEAM_COUNT] = {0, 0};
    cw_query units = cw_query_iter(w, cw_mask(&(const cw_component_id){UNIT}, 1));
    cw_entity_id id;
    while (cw_query_next(&units, &id)) {
        const Unit *u = cw_comp_ptr(w, id, UNIT);
        if (u->team < TEAM_COUNT) ++alive[u->team];
    }
    const CombatState *s = state_of(w);
    cw_query boards = cw_query_iter(w, cw_mask(&(const cw_component_id){SCOREBOARD}, 1));
    while (cw_query_next(&boards, &id)) {
        const Scoreboard *b = cw_comp_ptr(w, id, SCOREBOARD);
        uint8_t team = b->team < TEAM_COUNT ? b->team : TEAM_RED;
        cw_ui_set_textf(w, &gw->ui, id, "%s  %d alive  %" PRIu64 " kills", team_names[team], alive[team], s ? s->kills[team] : 0);
    }
}
static bool register_systems(cw_sim *sim) {
    const cw_system_desc systems[] = {
        {"scoreboard", CW_PHASE(CW_LATE_UPDATE), sys_scoreboard, CW_SHARED, 10, false},
        {"visual_begin", CW_PHASE(CW_EARLY_UPDATE), sys_visual_begin, CW_SHARED, -10, false},
        {"targeting", CW_PHASE(CW_EARLY_UPDATE), sys_targeting, CW_SHARED, 0, false},
        {"movement", CW_PHASE(CW_UPDATE), sys_movement, CW_SHARED, 0, false},
        {"combat", CW_PHASE(CW_UPDATE), sys_combat, CW_SHARED, 10, false},
        {"light_animate", CW_PHASE(CW_UPDATE), sys_light_animate, CW_SHARED, 20, false},
        {"lifetime", CW_PHASE(CW_UPDATE), sys_lifetime, CW_SHARED, 30, false},
        {"attach", CW_PHASE(CW_LATE_UPDATE), sys_attach, CW_SHARED, -10, false},
        {"death", CW_PHASE(CW_LATE_UPDATE), sys_death, CW_HOST_ONLY, 0, false},
        {"destroyed", CW_PHASE(CW_ENTITY_DESTROYED), sys_destroyed, CW_SHARED, 0, true},
        {"created", CW_PHASE(CW_ENTITY_CREATED), sys_created, CW_SHARED, 0, true},
    };
    for (size_t i = 0; i < sizeof(systems) / sizeof(systems[0]); ++i)
        if (!cw_sim_register(sim, systems[i])) return false;
    return true;
}

/* ===========================================================================
 * Prefabs: what the editor can place
 * ======================================================================== */
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static bool spawn_unit(cw_world *w, cw_entity_id id, uint8_t team, uint8_t kind, float x, float y) {
    const UnitKind *k = &kinds[kind];
    Position p = {clampf(x, k->radius, ARENA_W - k->radius), clampf(y, k->radius, ARENA_H - k->radius)};
    Velocity v = {0, 0};
    HealthConfig h = {k->health};
    Attack a = {k->damage, k->range, k->cooldown, 0, 0};
    Unit u = {team, kind};
    Body b = {k->radius, k->speed};
    return cw_comp_set(w, id, POSITION, &p, sizeof(p)) && cw_comp_set(w, id, VELOCITY, &v, sizeof(v))
        && cw_comp_set(w, id, HEALTH_CONFIG, &h, sizeof(h)) && cw_comp_set(w, id, ATTACK, &a, sizeof(a))
        && cw_comp_set(w, id, UNIT, &u, sizeof(u)) && cw_comp_set(w, id, BODY, &b, sizeof(b))
        && cw_comp_add(w, id, SHADOW_CASTER);
}
static bool spawn_red_melee(cw_world *w, cw_entity_id id, float x, float y) { return spawn_unit(w, id, TEAM_RED, KIND_MELEE, x, y); }
static bool spawn_red_archer(cw_world *w, cw_entity_id id, float x, float y) { return spawn_unit(w, id, TEAM_RED, KIND_ARCHER, x, y); }
static bool spawn_red_tank(cw_world *w, cw_entity_id id, float x, float y) { return spawn_unit(w, id, TEAM_RED, KIND_TANK, x, y); }
static bool spawn_blue_melee(cw_world *w, cw_entity_id id, float x, float y) { return spawn_unit(w, id, TEAM_BLUE, KIND_MELEE, x, y); }
static bool spawn_blue_archer(cw_world *w, cw_entity_id id, float x, float y) { return spawn_unit(w, id, TEAM_BLUE, KIND_ARCHER, x, y); }
static bool spawn_blue_tank(cw_world *w, cw_entity_id id, float x, float y) { return spawn_unit(w, id, TEAM_BLUE, KIND_TANK, x, y); }

static bool spawn_light(cw_world *w, cw_entity_id id, Light light, float x, float y) {
    Position p = {clampf(x, 0, ARENA_W), clampf(y, 0, ARENA_H)};
    return cw_comp_set(w, id, POSITION, &p, sizeof(p)) && cw_comp_set(w, id, LIGHT, &light, sizeof(light));
}
static bool spawn_lantern(cw_world *w, cw_entity_id id, float x, float y) {
    Light l = {255, 200, 130, 0, 1.6f, 320, 2.0f, 1.2f, 0, 1.0f, 0.25f, 6.0f};
    return spawn_light(w, id, l, x, y);
}
static bool spawn_cool_lamp(cw_world *w, cw_entity_id id, float x, float y) {
    Light l = {140, 200, 255, 0, 1.4f, 360, 2.0f, 1.0f, 0, 1.0f, 0, 6.0f};
    return spawn_light(w, id, l, x, y);
}
static bool spawn_spotlight(cw_world *w, cw_entity_id id, float x, float y) {
    Light l = {255, 255, 255, LIGHT_SPOT, 1.3f, 460, 1.5f, 1.4f, 0, 0.9f, 0, 6.0f};
    return spawn_light(w, id, l, x, y);
}
static bool spawn_green(cw_world *w, cw_entity_id id, float x, float y) {
    Light l = {120, 255, 150, 0, 1.3f, 260, 2.0f, 1.0f, 0, 1.0f, 0.5f, 12.0f};
    return spawn_light(w, id, l, x, y);
}
static bool spawn_base_light(cw_world *w, cw_entity_id id, float x, float y) {
    Light l = {255, 170, 110, 0, 2.2f, 680, 1.5f, 1.0f, 0, 1.0f, 0, 6.0f};
    return spawn_light(w, id, l, x, y);
}
static bool spawn_pillar(cw_world *w, cw_entity_id id, float x, float y) {
    Position p = {clampf(x, 0, ARENA_W), clampf(y, 0, ARENA_H)};
    Obstacle o = {34};
    return cw_comp_set(w, id, POSITION, &p, sizeof(p)) && cw_comp_set(w, id, OBSTACLE, &o, sizeof(o))
        && cw_comp_add(w, id, SHADOW_CASTER);
}
static bool spawn_torch(cw_world *w, cw_entity_id id, float x, float y) {
    /* a spot light that rides on the nearest unit, if one is close; otherwise a free spot */
    Light l = {255, 240, 200, LIGHT_SPOT, 1.6f, 380, 2.0f, 1.5f, 0, 1.1f, 0, 6.0f};
    if (!spawn_light(w, id, l, x, y)) return false;
    const cw_component_id parts[] = {UNIT, POSITION};
    cw_query q = cw_query_iter(w, cw_mask(parts, 2));
    cw_entity_id other, best = CW_INVALID_ENTITY;
    float best_d = 40 * 40;
    Position at = {x, y};
    while (cw_query_next(&q, &other)) {
        float d = dist2(&at, cw_comp_ptr(w, other, POSITION));
        if (d < best_d) { best_d = d; best = other; }
    }
    if (best == CW_INVALID_ENTITY) return true;
    Attached a = {best, 0, 0, true};
    return cw_comp_set(w, id, ATTACHED, &a, sizeof(a));
}
static const cw_prefab prefabs[] = {
    {"melee", "red", spawn_red_melee},
    {"archer", "red", spawn_red_archer},
    {"tank", "red", spawn_red_tank},
    {"melee", "blue", spawn_blue_melee},
    {"archer", "blue", spawn_blue_archer},
    {"tank", "blue", spawn_blue_tank},
    {"lantern", "light", spawn_lantern},
    {"cool lamp", "light", spawn_cool_lamp},
    {"spotlight", "light", spawn_spotlight},
    {"green", "light", spawn_green},
    {"base light", "light", spawn_base_light},
    {"torch", "light", spawn_torch},
    {"pillar", "level", spawn_pillar},
};

/* ===========================================================================
 * Drawing through the host's canvas
 * ======================================================================== */
static const cw_rgba team_colors[TEAM_COUNT] = {{232, 84, 70, 255}, {72, 144, 236, 255}};
static const cw_rgba team_dark[TEAM_COUNT] = {{120, 40, 36, 255}, {34, 70, 124, 255}};
static const cw_rgba white = {240, 242, 246, 255};

static Position render_position(cw_world *w, cw_entity_id id, const cw_draw_context *ctx) {
    const Position *p = cw_comp_ptr(w, id, POSITION);
    if (ctx->editing || ctx->paused || ctx->alpha >= 1 || !cw_comp_has(w, id, VISUAL)) return *p;
    const Visual *v = cw_comp_ptr(w, id, VISUAL);
    Position r = {v->previous.x + (p->x - v->previous.x) * ctx->alpha, v->previous.y + (p->y - v->previous.y) * ctx->alpha};
    return r;
}
static void facing_of(cw_world *w, cw_entity_id id, Position pos, float *dx, float *dy) {
    *dx = 1; *dy = 0;
    if (cw_comp_has(w, id, VELOCITY)) {
        const Velocity *v = cw_comp_ptr(w, id, VELOCITY);
        float len = sqrtf(v->x * v->x + v->y * v->y);
        if (len > 1.0f) { *dx = v->x / len; *dy = v->y / len; return; }
    }
    if (cw_comp_has(w, id, TARGET)) {
        const Target *t = cw_comp_ptr(w, id, TARGET);
        if (cw_world_is_valid(w, t->id) && cw_comp_has(w, t->id, POSITION)) {
            const Position *tp = cw_comp_ptr(w, t->id, POSITION);
            float len = sqrtf(dist2(tp, &pos));
            if (len > 0) { *dx = (tp->x - pos.x) / len; *dy = (tp->y - pos.y) / len; }
        }
    }
}
static cw_rgba mix(cw_rgba a, cw_rgba b, float t) {
    return cw_rgba_make((uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t),
                        (uint8_t)(a.b + (b.b - a.b) * t), (uint8_t)(a.a + (b.a - a.a) * t));
}
static void draw_unit(cw_world *w, cw_entity_id id, const cw_canvas *c, const cw_draw_context *ctx) {
    const Unit *u = cw_comp_ptr(w, id, UNIT);
    const Body *b = cw_comp_ptr(w, id, BODY);
    Position pos = render_position(w, id, ctx);
    float dx, dy;
    facing_of(w, id, pos, &dx, &dy);
    bool pending = (w->entities[id].flags & CW_TO_DESTROY) != 0;
    cw_rgba fill = team_colors[u->team], rim = team_dark[u->team];
    if (cw_comp_has(w, id, VISUAL) && ((Visual *)cw_comp_ptr(w, id, VISUAL))->flash > 0) fill = white;
    if (pending) fill = cw_rgba_alpha(fill, 90);
    float r = b->radius;
    switch (u->kind) {
        case KIND_ARCHER: {
            float sx = -dy, sy = dx;
            float tipx = pos.x + dx * r * 1.3f, tipy = pos.y + dy * r * 1.3f;
            float lx = pos.x - dx * r * 0.8f + sx * r, ly = pos.y - dy * r * 0.8f + sy * r;
            float rx = pos.x - dx * r * 0.8f - sx * r, ry = pos.y - dy * r * 0.8f - sy * r;
            c->triangle(c->impl, tipx, tipy, lx, ly, rx, ry, rim);
            c->triangle(c->impl, pos.x + (tipx - pos.x) * 0.7f, pos.y + (tipy - pos.y) * 0.7f,
                        pos.x + (lx - pos.x) * 0.7f, pos.y + (ly - pos.y) * 0.7f,
                        pos.x + (rx - pos.x) * 0.7f, pos.y + (ry - pos.y) * 0.7f, fill);
            break;
        }
        case KIND_TANK:
            c->oriented_rect(c->impl, pos.x, pos.y, dx, dy, r * 2, r * 1.7f, rim);
            c->oriented_rect(c->impl, pos.x, pos.y, dx, dy, r * 1.5f, r * 1.2f, fill);
            break;
        default:
            c->circle(c->impl, pos.x, pos.y, r, rim);
            c->circle(c->impl, pos.x, pos.y, r * 0.72f, fill);
            c->line(c->impl, pos.x, pos.y, pos.x + dx * r * 1.1f, pos.y + dy * r * 1.1f, c->px(c->impl, 2), fill);
            break;
    }
    if (cw_comp_has(w, id, HEALTH)) {
        const Health *h = cw_comp_ptr(w, id, HEALTH);
        float t = h->maximum ? (float)h->current / (float)h->maximum : 0;
        c->bar(c->impl, pos.x - r, pos.y - r - 7, r * 2, 4, t, mix(cw_rgb(220, 60, 50), cw_rgb(80, 210, 90), t), cw_rgba_make(0, 0, 0, 160));
    }
    if (ctx->editing) {
        char label[48];
        snprintf(label, sizeof(label), "%s %s", team_names[u->team], kinds[u->kind].name);
        c->text(c->impl, pos.x + r + c->px(c->impl, 4), pos.y - c->px(c->impl, 6), cw_rgba_alpha(white, 170), label);
        if (cw_comp_has(w, id, ATTACK))
            c->ring(c->impl, pos.x, pos.y, ((Attack *)cw_comp_ptr(w, id, ATTACK))->range, c->px(c->impl, 1), cw_rgba_alpha(fill, 40));
    }
    if (pending) {
        c->line(c->impl, pos.x - r, pos.y - r, pos.x + r, pos.y + r, c->px(c->impl, 2), white);
        c->line(c->impl, pos.x - r, pos.y + r, pos.x + r, pos.y - r, c->px(c->impl, 2), white);
    }
    if (!pending) c->pick(c->impl, id, pos.x, pos.y, r + 2);
    if (cw_comp_has(w, id, SHADOW_CASTER)) c->occluder_circle(c->impl, pos.x, pos.y, r * 0.9f);
}
static void draw(cw_world *w, const cw_canvas *c, const cw_draw_context *ctx) {
    cw_entity_id id;
    /* the arena */
    c->rect(c->impl, 0, 0, ARENA_W, ARENA_H, cw_rgb(33, 36, 44));
    c->rect_outline(c->impl, 0, 0, ARENA_W, ARENA_H, c->px(c->impl, 2), cw_rgb(90, 96, 110));
    c->occluder_rect(c->impl, 0, 0, ARENA_W, ARENA_H);
    /* obstacles */
    const cw_component_id obstacle_parts[] = {OBSTACLE, POSITION};
    cw_query obstacles = cw_query_iter(w, cw_mask(obstacle_parts, 2));
    while (cw_query_next(&obstacles, &id)) {
        const Position *p = cw_comp_ptr(w, id, POSITION);
        float r = ((Obstacle *)cw_comp_ptr(w, id, OBSTACLE))->radius;
        c->circle(c->impl, p->x, p->y, r, cw_rgb(58, 62, 72));
        c->circle(c->impl, p->x, p->y, r * 0.8f, cw_rgb(76, 80, 92));
        c->ring(c->impl, p->x, p->y, r, c->px(c->impl, 1.5f), cw_rgb(110, 116, 130));
        c->pick(c->impl, id, p->x, p->y, r);
        if (cw_comp_has(w, id, SHADOW_CASTER)) c->occluder_circle(c->impl, p->x, p->y, r);
    }
    /* units, including those about to be destroyed (invisible to queries) */
    const cw_component_id unit_parts[] = {POSITION, UNIT, BODY};
    cw_query units = cw_query_iter(w, cw_mask(unit_parts, 3));
    while (cw_query_next(&units, &id)) draw_unit(w, id, c, ctx);
    cw_id_view pending = cw_world_pending_destroy(w);
    for (size_t i = 0; i < pending.count; ++i)
        if (cw_comp_has(w, pending.data[i], UNIT) && cw_comp_has(w, pending.data[i], BODY) && cw_comp_has(w, pending.data[i], POSITION))
            draw_unit(w, pending.data[i], c, ctx);
    /* lights: submitted to the host's lighting, plus an icon while editing */
    const cw_component_id light_parts[] = {LIGHT, POSITION};
    cw_query lights = cw_query_iter(w, cw_mask(light_parts, 2));
    while (cw_query_next(&lights, &id)) {
        const Light *l = cw_comp_ptr(w, id, LIGHT);
        Position pos = render_position(w, id, ctx);
        cw_canvas_light d;
        memset(&d, 0, sizeof(d));
        d.x = pos.x; d.y = pos.y; d.radius = l->radius;
        d.color = cw_rgb(l->r, l->g, l->b);
        d.intensity = !ctx->editing && cw_comp_has(w, id, LIGHT_STATE) ? ((LightState *)cw_comp_ptr(w, id, LIGHT_STATE))->intensity : l->intensity;
        d.falloff = l->falloff; d.volumetric = l->volumetric;
        d.spot = (l->flags & LIGHT_SPOT) != 0; d.no_shadows = (l->flags & LIGHT_NO_SHADOWS) != 0;
        d.direction = l->direction; d.cone = l->cone;
        if (d.intensity > 0) c->light(c->impl, &d);
        if (cw_comp_has(w, id, LIFETIME) && !ctx->editing) continue;   /* flashes are not pickable */
        float r = c->px(c->impl, 7);
        cw_rgba col = cw_rgb(l->r, l->g, l->b);
        c->circle(c->impl, pos.x, pos.y, r, cw_rgba_alpha(col, 230));
        c->ring(c->impl, pos.x, pos.y, r * 1.6f, c->px(c->impl, 1), cw_rgba_alpha(col, 150));
        for (int k = 0; k < 8; ++k) {
            float a = (float)k * PI_F / 4.0f;
            c->line(c->impl, pos.x + cosf(a) * r * 1.9f, pos.y + sinf(a) * r * 1.9f,
                    pos.x + cosf(a) * r * 2.6f, pos.y + sinf(a) * r * 2.6f, c->px(c->impl, 1), cw_rgba_alpha(col, 160));
        }
        if (l->flags & LIGHT_SPOT)
            c->line(c->impl, pos.x, pos.y, pos.x + cosf(l->direction) * r * 4.5f, pos.y + sinf(l->direction) * r * 4.5f, c->px(c->impl, 2), cw_rgba_alpha(col, 200));
        if (ctx->editing) c->ring(c->impl, pos.x, pos.y, l->radius, c->px(c->impl, 1), cw_rgba_alpha(col, 50));
        c->pick(c->impl, id, pos.x, pos.y, r * 2.2f);
    }
}

static void describe(cw_world *w, cw_entity_id id, char *out, size_t capacity) {
    if (cw_comp_has(w, id, UNIT)) {
        const Unit *u = cw_comp_ptr(w, id, UNIT);
        snprintf(out, capacity, "%s %s", team_names[u->team], kinds[u->kind].name);
    } else if (cw_comp_has(w, id, LIGHT)) {
        const Light *l = cw_comp_ptr(w, id, LIGHT);
        snprintf(out, capacity, "%s%s", (l->flags & LIGHT_SPOT) ? "spot " : "", cw_comp_has(w, id, ATTACHED) ? "torch" : "light");
    } else if (cw_comp_has(w, id, OBSTACLE)) snprintf(out, capacity, "pillar");
    else snprintf(out, capacity, "entity");
}

/* ===========================================================================
 * The module table
 * ======================================================================== */
static const cw_game_module module = {
    CW_GAME_ABI_VERSION,
    "combat",
    SCENE_VERSION,
    TICK_HZ,
    {0, 0, ARENA_W, ARENA_H},
    register_components,
    register_systems,
    bind,
    unbind,
    prefabs,
    sizeof(prefabs) / sizeof(prefabs[0]),
    draw,
    describe,
};
CW_GAME_API const cw_game_module *cw_game_export(void) { return &module; }
