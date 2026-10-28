/* Combat demo: a small two-team battle built on Clockwork and render2d.
 *
 * Units move toward the nearest enemy, attack when in range, lose health
 * and die. The window lets you spawn units (by clicking or in waves),
 * select them (click, shift-click, box drag), inspect and edit them, drag
 * them around, delete them, pause and single-step the simulation, change
 * its speed, and save/load the whole world as a Clockwork snapshot.
 *
 * Usage:
 *   clockwork_combat [path]            interactive window (snapshot file = path)
 *   clockwork_combat --headless [path] run the simulation, edits and a
 *                                      save/load round trip without a window
 *   clockwork_combat --frames N        render N frames and exit (smoke test)
 *   clockwork_combat --config FILE     post-process tunables (default tunable.cfg)
 *
 * Keys: Space pause   N step   [ ] speed   Q select tool   1/2 place red/blue   3 place light
 *       K kind   W wave   L torch on/off for selected units   Delete remove selection   H heal   T swap team
 *       F5 save   F9 load   F fit arena   Home reset camera   F11 fullscreen   Ctrl+Q quit
 *       P post-process on/off   F6 reload tunables   F7 tuning panel   F8 save tunables
 *       right/middle drag or arrow keys pan   wheel or +/- zoom   Esc deselect
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "clockwork.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * Simulation: components, codecs, systems
 * ======================================================================== */
typedef struct { float x, y; } Position;
typedef struct { float x, y; } Velocity;
typedef struct { int32_t current, maximum; } Health;
typedef struct { int32_t damage; float range, cooldown, timer; int32_t kills; } Attack;
typedef struct { uint8_t team, kind; } Unit;
typedef struct { float radius, speed; } Body;
typedef struct { cw_entity_id id; } Target;                 /* transient: rebuilt every tick */
typedef struct {                                           /* transient: render-only state */
    Position previous;           /* where the unit was at the start of the tick (interpolation) */
    float flash;                 /* hit flash timer */
    uint32_t smoke, dust;        /* particle emitters attached by the window, 0 when none */
} Visual;

/* Lights are entities too: a Light is rendered by the window at the entity's
 * Position, animated by the light systems, saved in snapshots, selectable
 * and editable like a unit. */
enum { LIGHT_SPOT = 1, LIGHT_NO_SHADOWS = 2 };
typedef struct {
    uint8_t r, g, b, flags;
    float intensity, radius, falloff, volumetric;
    float direction, cone;          /* spot lights */
    float flicker, flicker_speed;   /* intensity wobble: amount (0..1) and rate */
} Light;
typedef struct { float intensity; } LightState;   /* transient: animated intensity for rendering */
typedef struct { float remaining, total; } Lifetime;   /* the entity is destroyed when it runs out */
typedef struct { cw_entity_id parent; float offset_x, offset_y; uint8_t follow_facing; } Attached;
typedef struct { float radius; } Obstacle;        /* static level geometry: units walk around it */

enum { POSITION, VELOCITY, HEALTH, ATTACK, UNIT, BODY, TARGET, VISUAL,
       LIGHT, LIGHT_STATE, LIFETIME, ATTACHED, OBSTACLE, SHADOW_CASTER, COMPONENT_COUNT };
enum { TEAM_RED, TEAM_BLUE, TEAM_COUNT };
enum { KIND_MELEE, KIND_ARCHER, KIND_TANK, KIND_COUNT };

#define ARENA_W 1200.0f
#define ARENA_H 800.0f

#define TICK_HZ 30
#define SNAPSHOT_VERSION 1

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
static const char *team_names[TEAM_COUNT] = {"red", "blue"};

/* --- codecs: explicit little-endian encodings, independent of host layout --- */
#define BEGIN_MARSHAL(T, var) \
    const T *var = cw_comp_ptr(w, id, ct); cw_writer out; cw_writer_init_fixed(&out, data, capacity)
#define END_MARSHAL() *size = out.size; return cw_writer_ok(&out)
#define BEGIN_UNMARSHAL(T, var) cw_reader in = cw_reader_make(data, size); T var; memset(&var, 0, sizeof(var))
#define END_UNMARSHAL(T, var) \
    do { if (!cw_reader_at_end(&in)) return false; *(T *)cw_comp_ptr(w, id, ct) = var; return true; } while (0)

static bool marshal_vec2(cw_world *w, cw_entity_id id, cw_component_id ct, void *data, size_t capacity, size_t *size) {
    BEGIN_MARSHAL(Position, p);
    cw_writer_f32(&out, p->x);
    cw_writer_f32(&out, p->y);
    END_MARSHAL();
}
static bool unmarshal_vec2(cw_world *w, cw_entity_id id, cw_component_id ct, const void *data, size_t size) {
    BEGIN_UNMARSHAL(Position, p);
    cw_reader_f32(&in, &p.x);
    cw_reader_f32(&in, &p.y);
    if (!isfinite(p.x) || !isfinite(p.y)) return false;
    END_UNMARSHAL(Position, p);
}
static bool marshal_health(cw_world *w, cw_entity_id id, cw_component_id ct, void *data, size_t capacity, size_t *size) {
    BEGIN_MARSHAL(Health, h);
    cw_writer_i32(&out, h->current);
    cw_writer_i32(&out, h->maximum);
    END_MARSHAL();
}
static bool unmarshal_health(cw_world *w, cw_entity_id id, cw_component_id ct, const void *data, size_t size) {
    BEGIN_UNMARSHAL(Health, h);
    cw_reader_i32(&in, &h.current);
    cw_reader_i32(&in, &h.maximum);
    if (h.maximum <= 0 || h.current < 0 || h.current > h.maximum) return false;
    END_UNMARSHAL(Health, h);
}
static bool marshal_attack(cw_world *w, cw_entity_id id, cw_component_id ct, void *data, size_t capacity, size_t *size) {
    BEGIN_MARSHAL(Attack, a);
    cw_writer_i32(&out, a->damage);
    cw_writer_f32(&out, a->range);
    cw_writer_f32(&out, a->cooldown);
    cw_writer_f32(&out, a->timer);
    cw_writer_i32(&out, a->kills);
    END_MARSHAL();
}
static bool unmarshal_attack(cw_world *w, cw_entity_id id, cw_component_id ct, const void *data, size_t size) {
    BEGIN_UNMARSHAL(Attack, a);
    cw_reader_i32(&in, &a.damage);
    cw_reader_f32(&in, &a.range);
    cw_reader_f32(&in, &a.cooldown);
    cw_reader_f32(&in, &a.timer);
    cw_reader_i32(&in, &a.kills);
    if (a.damage < 0 || !(a.range >= 0) || !(a.cooldown > 0) || !isfinite(a.timer) || a.kills < 0) return false;
    END_UNMARSHAL(Attack, a);
}
static bool marshal_unit(cw_world *w, cw_entity_id id, cw_component_id ct, void *data, size_t capacity, size_t *size) {
    BEGIN_MARSHAL(Unit, u);
    cw_writer_u8(&out, u->team);
    cw_writer_u8(&out, u->kind);
    END_MARSHAL();
}
static bool unmarshal_unit(cw_world *w, cw_entity_id id, cw_component_id ct, const void *data, size_t size) {
    BEGIN_UNMARSHAL(Unit, u);
    cw_reader_u8(&in, &u.team);
    cw_reader_u8(&in, &u.kind);
    if (u.team >= TEAM_COUNT || u.kind >= KIND_COUNT) return false;
    END_UNMARSHAL(Unit, u);
}
static bool marshal_body(cw_world *w, cw_entity_id id, cw_component_id ct, void *data, size_t capacity, size_t *size) {
    BEGIN_MARSHAL(Body, b);
    cw_writer_f32(&out, b->radius);
    cw_writer_f32(&out, b->speed);
    END_MARSHAL();
}
static bool unmarshal_body(cw_world *w, cw_entity_id id, cw_component_id ct, const void *data, size_t size) {
    BEGIN_UNMARSHAL(Body, b);
    cw_reader_f32(&in, &b.radius);
    cw_reader_f32(&in, &b.speed);
    if (!(b.radius > 0) || !(b.speed >= 0)) return false;
    END_UNMARSHAL(Body, b);
}

static bool marshal_light(cw_world *w, cw_entity_id id, cw_component_id ct, void *data, size_t capacity, size_t *size) {
    BEGIN_MARSHAL(Light, l);
    cw_writer_u8(&out, l->r); cw_writer_u8(&out, l->g); cw_writer_u8(&out, l->b); cw_writer_u8(&out, l->flags);
    cw_writer_f32(&out, l->intensity); cw_writer_f32(&out, l->radius); cw_writer_f32(&out, l->falloff); cw_writer_f32(&out, l->volumetric);
    cw_writer_f32(&out, l->direction); cw_writer_f32(&out, l->cone); cw_writer_f32(&out, l->flicker); cw_writer_f32(&out, l->flicker_speed);
    END_MARSHAL();
}
static bool unmarshal_light(cw_world *w, cw_entity_id id, cw_component_id ct, const void *data, size_t size) {
    BEGIN_UNMARSHAL(Light, l);
    cw_reader_u8(&in, &l.r); cw_reader_u8(&in, &l.g); cw_reader_u8(&in, &l.b); cw_reader_u8(&in, &l.flags);
    cw_reader_f32(&in, &l.intensity); cw_reader_f32(&in, &l.radius); cw_reader_f32(&in, &l.falloff); cw_reader_f32(&in, &l.volumetric);
    cw_reader_f32(&in, &l.direction); cw_reader_f32(&in, &l.cone); cw_reader_f32(&in, &l.flicker); cw_reader_f32(&in, &l.flicker_speed);
    if (!(l.intensity >= 0) || !(l.radius > 0) || !(l.falloff >= 0) || !(l.volumetric >= 0) || !isfinite(l.direction)
        || !(l.cone >= 0) || !(l.flicker >= 0 && l.flicker <= 1) || !(l.flicker_speed >= 0)) return false;
    END_UNMARSHAL(Light, l);
}
static bool marshal_lifetime(cw_world *w, cw_entity_id id, cw_component_id ct, void *data, size_t capacity, size_t *size) {
    BEGIN_MARSHAL(Lifetime, l);
    cw_writer_f32(&out, l->remaining);
    cw_writer_f32(&out, l->total);
    END_MARSHAL();
}
static bool unmarshal_lifetime(cw_world *w, cw_entity_id id, cw_component_id ct, const void *data, size_t size) {
    BEGIN_UNMARSHAL(Lifetime, l);
    cw_reader_f32(&in, &l.remaining);
    cw_reader_f32(&in, &l.total);
    if (!isfinite(l.remaining) || !(l.total > 0)) return false;
    END_UNMARSHAL(Lifetime, l);
}
static bool marshal_attached(cw_world *w, cw_entity_id id, cw_component_id ct, void *data, size_t capacity, size_t *size) {
    BEGIN_MARSHAL(Attached, a);
    cw_writer_i64(&out, a->parent);
    cw_writer_f32(&out, a->offset_x);
    cw_writer_f32(&out, a->offset_y);
    cw_writer_u8(&out, a->follow_facing);
    END_MARSHAL();
}
static bool unmarshal_attached(cw_world *w, cw_entity_id id, cw_component_id ct, const void *data, size_t size) {
    BEGIN_UNMARSHAL(Attached, a);
    cw_reader_i64(&in, &a.parent);
    cw_reader_f32(&in, &a.offset_x);
    cw_reader_f32(&in, &a.offset_y);
    cw_reader_u8(&in, &a.follow_facing);
    if (!isfinite(a.offset_x) || !isfinite(a.offset_y)) return false;
    END_UNMARSHAL(Attached, a);
}
static bool marshal_obstacle(cw_world *w, cw_entity_id id, cw_component_id ct, void *data, size_t capacity, size_t *size) {
    BEGIN_MARSHAL(Obstacle, o);
    cw_writer_f32(&out, o->radius);
    END_MARSHAL();
}
static bool unmarshal_obstacle(cw_world *w, cw_entity_id id, cw_component_id ct, const void *data, size_t size) {
    BEGIN_UNMARSHAL(Obstacle, o);
    cw_reader_f32(&in, &o.radius);
    if (!(o.radius > 0)) return false;
    END_UNMARSHAL(Obstacle, o);
}
static bool marshal_tag(cw_world *w, cw_entity_id id, cw_component_id ct, void *data, size_t capacity, size_t *size) {
    (void)w; (void)id; (void)ct; (void)data; (void)capacity;
    *size = 0;
    return true;
}
static bool unmarshal_tag(cw_world *w, cw_entity_id id, cw_component_id ct, const void *data, size_t size) {
    (void)w; (void)id; (void)ct; (void)data;
    return size == 0;
}

static void visual_clear(void *slot);
static bool register_schema(cw_world *w) {
    const cw_codec vec2 = {marshal_vec2, unmarshal_vec2}, health = {marshal_health, unmarshal_health};
    const cw_codec attack = {marshal_attack, unmarshal_attack}, unit = {marshal_unit, unmarshal_unit};
    const cw_codec body = {marshal_body, unmarshal_body}, light = {marshal_light, unmarshal_light};
    const cw_codec lifetime = {marshal_lifetime, unmarshal_lifetime}, attached = {marshal_attached, unmarshal_attached};
    const cw_codec obstacle = {marshal_obstacle, unmarshal_obstacle}, tag = {marshal_tag, unmarshal_tag};
    return cw_comp_block(w, COMPONENT_COUNT) == 0
        && CW_REGISTER(w, POSITION, Position) && cw_comp_set_codec(w, POSITION, vec2)
        && CW_REGISTER(w, VELOCITY, Velocity) && cw_comp_set_codec(w, VELOCITY, vec2)
        && CW_REGISTER(w, HEALTH, Health) && cw_comp_set_codec(w, HEALTH, health)
        && CW_REGISTER(w, ATTACK, Attack) && cw_comp_set_codec(w, ATTACK, attack)
        && CW_REGISTER(w, UNIT, Unit) && cw_comp_set_codec(w, UNIT, unit)
        && CW_REGISTER(w, BODY, Body) && cw_comp_set_codec(w, BODY, body)
        && CW_REGISTER(w, TARGET, Target) && cw_comp_set_transient(w, TARGET, true)
        && cw_comp_register(w, VISUAL, sizeof(Visual), _Alignof(Visual), "Visual", visual_clear)
        && cw_comp_set_transient(w, VISUAL, true)
        && CW_REGISTER(w, LIGHT, Light) && cw_comp_set_codec(w, LIGHT, light)
        && CW_REGISTER(w, LIGHT_STATE, LightState) && cw_comp_set_transient(w, LIGHT_STATE, true)
        && CW_REGISTER(w, LIFETIME, Lifetime) && cw_comp_set_codec(w, LIFETIME, lifetime)
        && CW_REGISTER(w, ATTACHED, Attached) && cw_comp_set_codec(w, ATTACHED, attached)
        && CW_REGISTER(w, OBSTACLE, Obstacle) && cw_comp_set_codec(w, OBSTACLE, obstacle)
        && cw_comp_register(w, SHADOW_CASTER, 0, 1, "ShadowCaster", NULL) && cw_comp_set_codec(w, SHADOW_CASTER, tag);
}

/* --- application-side hooks the systems report into (through world->user) ---
 * The simulation knows nothing about rendering: it reports combat events to
 * optional listener callbacks, which the window turns into particles and
 * effects and the headless check leaves unset. */
typedef struct SimHooks SimHooks;
struct SimHooks {
    cw_entity_id *scratch;
    size_t scratch_capacity;
    uint64_t kills[TEAM_COUNT], deaths[TEAM_COUNT], damage_dealt;
    void *listener;
    void (*on_attack)(void *listener, Position from, Position to, uint8_t team, uint8_t kind);
    void (*on_damage)(void *listener, Position at, int32_t amount, uint8_t team, Position from);
    void (*on_death)(void *listener, Position at, uint8_t team, float radius, bool killed);
    void (*on_visual_release)(void *listener, Visual *visual);   /* the component is being cleared */
};
static SimHooks *active_hooks;   /* for the component cleanup callback, which gets no world */

static SimHooks *hooks_of(cw_world *w) { return w->user; }
static cw_entity_id *scratch_ids(SimHooks *h, size_t needed) {
    if (needed > h->scratch_capacity) {
        size_t cap = h->scratch_capacity ? h->scratch_capacity : 256;
        while (cap < needed) cap *= 2;
        h->scratch = realloc(h->scratch, cap * sizeof(*h->scratch));
        if (!h->scratch) { fputs("out of memory\n", stderr); abort(); }
        h->scratch_capacity = cap;
    }
    return h->scratch;
}
/* Cleanup callback for Visual: lets the window release attached emitters
 * when a unit dies, is deleted, or the world is torn down for a load. */
static void visual_clear(void *slot) {
    Visual *v = slot;
    if (active_hooks && active_hooks->on_visual_release) active_hooks->on_visual_release(active_hooks->listener, v);
    memset(v, 0, sizeof(*v));
}

static float dist2(const Position *a, const Position *b) {
    float dx = a->x - b->x, dy = a->y - b->y;
    return dx * dx + dy * dy;
}
static size_t collect_units(cw_world *w, cw_entity_id **ids) {
    const cw_component_id required[] = {UNIT, POSITION, HEALTH, BODY};
    cw_signature mask = cw_mask(required, 4);
    size_t n = cw_query_collect(w, mask, NULL, 0);
    *ids = scratch_ids(hooks_of(w), n ? n : 1);
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
    /* obstacles are gathered once; the scratch buffer holds units first, then obstacles */
    const cw_component_id obstacle_parts[] = {OBSTACLE, POSITION};
    cw_signature obstacle_mask = cw_mask(obstacle_parts, 2);
    size_t n_obstacles = cw_query_collect(w, obstacle_mask, NULL, 0);
    cw_entity_id *all = scratch_ids(hooks_of(w), n + n_obstacles + 1);
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
    SimHooks *hooks = hooks_of(w);
    hooks->damage_dealt += (uint64_t)d->amount;
    if (cw_comp_has(w, id, VISUAL)) ((Visual *)cw_comp_ptr(w, id, VISUAL))->flash = 0.2f;
    if (hooks->on_damage && cw_comp_has(w, id, POSITION)) {
        Position at = *(Position *)cw_comp_ptr(w, id, POSITION), from = at;
        uint8_t team = cw_comp_has(w, id, UNIT) ? ((Unit *)cw_comp_ptr(w, id, UNIT))->team : 0;
        if (cw_world_is_valid(w, d->attacker) && cw_comp_has(w, d->attacker, POSITION))
            from = *(Position *)cw_comp_ptr(w, d->attacker, POSITION);
        hooks->on_damage(hooks->listener, at, d->amount, team, from);
    }
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
        SimHooks *hooks = hooks_of(w);
        if (hooks->on_attack && cw_comp_has(w, id, UNIT)) {
            const Unit *u = cw_comp_ptr(w, id, UNIT);
            hooks->on_attack(hooks->listener, *(Position *)cw_comp_ptr(w, id, POSITION),
                             *(Position *)cw_comp_ptr(w, t->id, POSITION), u->team, u->kind);
        }
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
static void sys_destroyed(cw_world *w, cw_phase phase, float dt) {
    (void)phase; (void)dt;
    SimHooks *hooks = hooks_of(w);
    cw_id_view pending = cw_world_pending_destroy(w);
    for (size_t i = 0; i < pending.count; ++i) {
        cw_entity_id id = pending.data[i];
        if (!cw_comp_has(w, id, UNIT) || !cw_comp_has(w, id, POSITION)) continue;
        const Unit *u = cw_comp_ptr(w, id, UNIT);
        const Position *p = cw_comp_ptr(w, id, POSITION);
        bool killed = cw_comp_has(w, id, HEALTH) && ((Health *)cw_comp_ptr(w, id, HEALTH))->current <= 0;
        if (killed) {
            hooks->deaths[u->team] += 1;
            hooks->kills[u->team == TEAM_RED ? TEAM_BLUE : TEAM_RED] += 1;
        }
        float radius = cw_comp_has(w, id, BODY) ? ((Body *)cw_comp_ptr(w, id, BODY))->radius : 10;
        if (hooks->on_death) hooks->on_death(hooks->listener, *p, u->team, radius, killed);
    }
}
/* EntityCreated: spawned and loaded entities get their transient render state. */
static void sys_created(cw_world *w, cw_phase phase, float dt) {
    (void)phase; (void)dt;
    cw_id_view created = cw_world_created(w);
    for (size_t i = 0; i < created.count; ++i) {
        cw_entity_id id = created.data[i];
        if (!cw_world_is_valid(w, id) || !cw_comp_has(w, id, POSITION) || cw_comp_has(w, id, VISUAL)) continue;
        Visual v = {*(Position *)cw_comp_ptr(w, id, POSITION), 0, 0, 0};
        cw_comp_set(w, id, VISUAL, &v, sizeof(v));
    }
}
static bool register_systems(cw_sim *sim) {
    const cw_system_desc systems[] = {
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
 * Application state and actions (shared by the window and the headless check)
 * ======================================================================== */
typedef struct {
    cw_world world;
    cw_sim sim;
    cw_clock clock;
    SimHooks hooks;
    bool world_ready;
    bool paused;
    float speed;
    uint64_t steps_total;
    char path[512];
    char status[160];
    bool status_error, failed;
    uint32_t rng;
    cw_snapshot_info last_saved;
    bool have_saved;
} App;
static App app;

static uint32_t next_random(void) {
    app.rng ^= app.rng << 13;
    app.rng ^= app.rng >> 17;
    app.rng ^= app.rng << 5;
    return app.rng;
}
static float random_range(float lo, float hi) {
    return lo + (float)(next_random() & 0xffffff) / (float)0x1000000 * (hi - lo);
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
    printf("%s%s\n", error ? "ERROR: " : "", app.status);
    fflush(stdout);
}
static void describe_error(const cw_file_error *e, char *out, size_t capacity) {
    if (e->status != CW_FILE_ERROR_SNAPSHOT) {
        snprintf(out, capacity, "%s%s%s", cw_file_status_string(e->status), e->errnum ? ": " : "",
                 e->errnum ? strerror(e->errnum) : "");
        return;
    }
    const cw_snapshot_error *s = &e->snapshot;
    int n = snprintf(out, capacity, "%s at byte %zu", cw_snapshot_status_string(s->status), s->offset);
    if (n > 0 && (size_t)n < capacity && s->name[0]) snprintf(out + n, capacity - (size_t)n, " (%s)", s->name);
}

static void world_up(void) {
    active_hooks = &app.hooks;
    cw_world_init(&app.world, &app.hooks);
    cw_sim_init(&app.sim, CW_HOST);
    if (!register_schema(&app.world) || !register_systems(&app.sim)) {
        set_status(true, "schema or system registration failed");
        return;
    }
    app.world_ready = true;
}
static void world_down(void) {
    if (!app.world_ready) return;
    cw_sim_destroy(&app.sim);
    cw_world_destroy(&app.world);
    app.world_ready = false;
}
/* Apply pending creations and destructions without advancing time, so that
 * edits made while paused are visible immediately. */
static void settle(void) {
    if (app.world.pending_destroy.count || app.world.created.count)
        cw_sim_entity_lifecycle(&app.sim, &app.world, 0);
}

static cw_entity_id spawn_unit(uint8_t team, uint8_t kind, float x, float y) {
    const UnitKind *k = &kinds[kind];
    cw_world *w = &app.world;
    cw_entity_id id = cw_world_create_entity(w);
    Position p = {x < k->radius ? k->radius : (x > ARENA_W - k->radius ? ARENA_W - k->radius : x),
                  y < k->radius ? k->radius : (y > ARENA_H - k->radius ? ARENA_H - k->radius : y)};
    Velocity v = {0, 0};
    Health h = {k->health, k->health};
    Attack a = {k->damage, k->range, k->cooldown, 0, 0};
    Unit u = {team, kind};
    Body b = {k->radius, k->speed};
    cw_comp_set(w, id, POSITION, &p, sizeof(p));
    cw_comp_set(w, id, VELOCITY, &v, sizeof(v));
    cw_comp_set(w, id, HEALTH, &h, sizeof(h));
    cw_comp_set(w, id, ATTACK, &a, sizeof(a));
    cw_comp_set(w, id, UNIT, &u, sizeof(u));
    cw_comp_set(w, id, BODY, &b, sizeof(b));
    cw_comp_add(w, id, SHADOW_CASTER);
    return id;
}
static Light light_make(uint8_t r, uint8_t g, uint8_t b, float intensity, float radius) {
    Light l;
    memset(&l, 0, sizeof(l));
    l.r = r; l.g = g; l.b = b;
    l.intensity = intensity; l.radius = radius; l.falloff = 2.0f; l.volumetric = 1.0f;
    l.cone = 1.0f; l.flicker_speed = 6.0f;
    return l;
}
static cw_entity_id spawn_light(Light light, float x, float y) {
    cw_world *w = &app.world;
    cw_entity_id id = cw_world_create_entity(w);
    Position p = {x, y};
    cw_comp_set(w, id, POSITION, &p, sizeof(p));
    cw_comp_set(w, id, LIGHT, &light, sizeof(light));
    LightState st = {light.intensity};
    cw_comp_set(w, id, LIGHT_STATE, &st, sizeof(st));
    return id;
}
static cw_entity_id spawn_flash(Light light, float x, float y, float seconds) {
    cw_entity_id id = spawn_light(light, x, y);
    Lifetime life = {seconds, seconds};
    cw_comp_set(&app.world, id, LIFETIME, &life, sizeof(life));
    return id;
}
/* A spot torch riding on `parent`, aimed where it faces; returns the light. */
static cw_entity_id spawn_torch(cw_entity_id parent, uint8_t r, uint8_t g, uint8_t b) {
    cw_world *w = &app.world;
    if (!cw_world_is_valid(w, parent) || !cw_comp_has(w, parent, POSITION)) return CW_INVALID_ENTITY;
    const Position *pp = cw_comp_ptr(w, parent, POSITION);
    Light l = light_make(r, g, b, 1.6f, 380);
    l.flags = LIGHT_SPOT; l.cone = 1.1f; l.volumetric = 1.5f;
    cw_entity_id id = spawn_light(l, pp->x, pp->y);
    Attached a = {parent, 0, 0, 1};
    cw_comp_set(w, id, ATTACHED, &a, sizeof(a));
    return id;
}
static cw_entity_id spawn_obstacle(float x, float y, float radius) {
    cw_world *w = &app.world;
    cw_entity_id id = cw_world_create_entity(w);
    Position p = {x, y};
    Obstacle o = {radius};
    cw_comp_set(w, id, POSITION, &p, sizeof(p));
    cw_comp_set(w, id, OBSTACLE, &o, sizeof(o));
    cw_comp_add(w, id, SHADOW_CASTER);
    return id;
}
/* Find the torch attached to an entity, if any. */
static cw_entity_id torch_of(cw_entity_id parent) {
    const cw_component_id required[] = {ATTACHED, LIGHT};
    cw_query q = cw_query_iter(&app.world, cw_mask(required, 2));
    cw_entity_id id;
    while (cw_query_next(&q, &id))
        if (((Attached *)cw_comp_ptr(&app.world, id, ATTACHED))->parent == parent) return id;
    return CW_INVALID_ENTITY;
}
/* The level: pillars that cast shadows and the arena's fixed lights. */
static void act_build_level(void) {
    static const float pillar[][3] = {{330, 210, 34}, {330, 590, 34}, {600, 400, 46}, {870, 210, 34}, {870, 590, 34}};
    for (size_t i = 0; i < sizeof(pillar) / sizeof(pillar[0]); ++i) spawn_obstacle(pillar[i][0], pillar[i][1], pillar[i][2]);
    Light warm = light_make(255, 170, 110, 2.2f, 680), cool = light_make(120, 180, 255, 2.2f, 680);
    warm.falloff = cool.falloff = 1.5f;
    spawn_light(warm, 40, ARENA_H * 0.5f);
    spawn_light(cool, ARENA_W - 40, ARENA_H * 0.5f);
    const float corners[4][2] = {{60, 60}, {ARENA_W - 60, 60}, {60, ARENA_H - 60}, {ARENA_W - 60, ARENA_H - 60}};
    for (int i = 0; i < 4; ++i) {
        Light lantern = light_make(255, 210, 140, 1.6f, 300);
        lantern.flicker = 0.25f; lantern.flicker_speed = 5.0f + (float)i; lantern.volumetric = 1.3f;
        spawn_light(lantern, corners[i][0], corners[i][1]);
    }
    settle();
    set_status(false, "level built: 5 pillars, 2 base lights, 4 lanterns (%zu entities)", cw_world_entity_count(&app.world));
}
static void act_spawn_wave(int per_team) {
    for (int t = 0; t < TEAM_COUNT; ++t) {
        for (int i = 0; i < per_team; ++i) {
            uint8_t kind = (uint8_t)(next_random() % KIND_COUNT);
            float x = t == TEAM_RED ? random_range(40, ARENA_W * 0.3f) : random_range(ARENA_W * 0.7f, ARENA_W - 40);
            spawn_unit((uint8_t)t, kind, x, random_range(40, ARENA_H - 40));
        }
    }
    settle();
    set_status(false, "spawned %d units per team (%zu entities)", per_team, cw_world_entity_count(&app.world));
}
static size_t count_team(uint8_t team) {
    const cw_component_id required[] = {UNIT};
    cw_query q = cw_query_iter(&app.world, cw_mask(required, 1));
    cw_entity_id id;
    size_t n = 0;
    while (cw_query_next(&q, &id)) n += ((Unit *)cw_comp_ptr(&app.world, id, UNIT))->team == team;
    return n;
}
static void act_step(int ticks) {
    for (int i = 0; i < ticks; ++i) cw_sim_tick(&app.sim, &app.world, app.clock.dt);
    app.steps_total += (uint64_t)ticks;
}
static void act_delete(const cw_entity_id *ids, size_t count) {
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!cw_world_is_valid(&app.world, ids[i])) continue;
        cw_world_mark_destroy(&app.world, ids[i]);
        ++n;
    }
    settle();
    if (n) set_status(false, "deleted %zu entities", n);
}
static void act_clear(void) {
    size_t n = cw_world_entity_count(&app.world);
    for (size_t i = 0; i < app.world.capacity; ++i)
        if (cw_entity_is_active(app.world.entities[i])) cw_world_mark_destroy(&app.world, app.world.entities[i].id);
    settle();
    set_status(false, "removed all %zu entities", n);
}

/* Edits: every operation applies to a list of entities and is clamped to keep
 * components valid (the codecs reject invalid values on load). */
typedef enum { EDIT_HEALTH, EDIT_MAX_HEALTH, EDIT_DAMAGE, EDIT_RANGE, EDIT_SPEED, EDIT_HEAL, EDIT_SWAP_TEAM, EDIT_KIND } EditOp;
static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
static size_t act_edit(const cw_entity_id *ids, size_t count, EditOp op, float amount) {
    cw_world *w = &app.world;
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        cw_entity_id id = ids[i];
        if (!cw_world_is_valid(w, id)) continue;
        Health *h = cw_comp_has(w, id, HEALTH) ? cw_comp_ptr(w, id, HEALTH) : NULL;
        Attack *a = cw_comp_has(w, id, ATTACK) ? cw_comp_ptr(w, id, ATTACK) : NULL;
        Body *b = cw_comp_has(w, id, BODY) ? cw_comp_ptr(w, id, BODY) : NULL;
        Unit *u = cw_comp_has(w, id, UNIT) ? cw_comp_ptr(w, id, UNIT) : NULL;
        switch (op) {
            case EDIT_HEALTH:
                if (h) {
                    h->current = clamp_i32(h->current + (int32_t)amount, 0, h->maximum);
                    if (h->current == 0) cw_world_mark_destroy(w, id);   /* an explicit kill */
                    ++n;
                }
                break;
            case EDIT_MAX_HEALTH:
                if (h) {
                    h->maximum = clamp_i32(h->maximum + (int32_t)amount, 10, 100000);
                    h->current = clamp_i32(h->current, 0, h->maximum);
                    ++n;
                }
                break;
            case EDIT_DAMAGE: if (a) { a->damage = clamp_i32(a->damage + (int32_t)amount, 0, 100000); ++n; } break;
            case EDIT_RANGE: if (a) { a->range = fmaxf(10.0f, fminf(a->range + amount, 2000.0f)); ++n; } break;
            case EDIT_SPEED: if (b) { b->speed = fmaxf(0.0f, fminf(b->speed + amount, 2000.0f)); ++n; } break;
            case EDIT_HEAL: if (h) { h->current = h->maximum; ++n; } break;
            case EDIT_SWAP_TEAM: if (u) { u->team = u->team == TEAM_RED ? TEAM_BLUE : TEAM_RED; ++n; } break;
            case EDIT_KIND:
                if (u && h && a && b) {
                    const UnitKind *k = &kinds[u->kind = (uint8_t)((u->kind + 1) % KIND_COUNT)];
                    float fraction = h->maximum > 0 ? (float)h->current / (float)h->maximum : 1.0f;
                    h->maximum = k->health;
                    h->current = clamp_i32((int32_t)(fraction * (float)k->health + 0.5f), 0, k->health);
                    a->damage = k->damage; a->range = k->range; a->cooldown = k->cooldown;
                    b->radius = k->radius; b->speed = k->speed;
                    ++n;
                }
                break;
        }
        if (h) cw_comp_touch(w, id, HEALTH);
    }
    if (n && op == EDIT_HEALTH && amount < 0) settle();
    return n;
}
static size_t act_move(const cw_entity_id *ids, size_t count, float dx, float dy) {
    cw_world *w = &app.world;
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!cw_world_is_valid(w, ids[i]) || !cw_comp_has(w, ids[i], POSITION)) continue;
        Position *p = cw_comp_ptr(w, ids[i], POSITION);
        float r = cw_comp_has(w, ids[i], BODY) ? ((Body *)cw_comp_ptr(w, ids[i], BODY))->radius : 0;
        p->x = fmaxf(r, fminf(p->x + dx, ARENA_W - r));
        p->y = fmaxf(r, fminf(p->y + dy, ARENA_H - r));
        if (cw_comp_has(w, ids[i], VISUAL)) ((Visual *)cw_comp_ptr(w, ids[i], VISUAL))->previous = *p;
        ++n;
    }
    return n;
}

static bool act_save(void) {
    cw_snapshot_save_config config = {0};
    cw_file_error error;
    config.user_version = SNAPSHOT_VERSION;
    if (!cw_snapshot_save_file(&app.world, app.path, &config, &error)) {
        char why[160];
        describe_error(&error, why, sizeof(why));
        set_status(true, "save failed: %s", why);
        return false;
    }
    cw_buffer image;
    app.have_saved = cw_file_read(app.path, 0, &image, &error)
        && cw_snapshot_peek(image.data, image.size, &app.last_saved, NULL);
    cw_buffer_free(&image);
    set_status(false, "saved %zu entities at tick %" PRIu64 " to %s (%" PRIu64 " bytes)",
               cw_world_entity_count(&app.world), app.world.tick, app.path, app.have_saved ? app.last_saved.size : 0);
    return true;
}
/* Loading needs a fresh world: tear the current one down, register the same
 * schema, load, then run the lifecycle once so loaded entities get their
 * transient render state through the EntityCreated system. */
static bool act_load(void) {
    cw_snapshot_load_config config = cw_snapshot_load_defaults();
    cw_file_error error;
    world_down();
    world_up();
    if (!app.world_ready) return false;
    if (!cw_snapshot_load_file(&app.world, app.path, &config, &error)) {
        char why[160];
        describe_error(&error, why, sizeof(why));
        set_status(true, "load failed: %s (world is empty)", why);
        return false;
    }
    cw_sim_entity_lifecycle(&app.sim, &app.world, 0);
    cw_clock_reset(&app.clock);
    set_status(false, "loaded %zu entities at tick %" PRIu64 " from %s", cw_world_entity_count(&app.world),
               app.world.tick, app.path);
    return true;
}
static void app_init(const char *path) {
    memset(&app, 0, sizeof(app));
    snprintf(app.path, sizeof(app.path), "%s", path);
    app.rng = 0x9e3779b9u;
    app.speed = 1.0f;
    app.clock = cw_clock_make(TICK_HZ);
    world_up();
}
static void app_shutdown(void) {
    world_down();
    free(app.hooks.scratch);
    app.hooks.scratch = NULL;
    app.hooks.scratch_capacity = 0;
}

/* ===========================================================================
 * Headless self-check: combat, editing, deletion, pause/step, snapshot
 * round trip and deterministic continuation after a load.
 * ======================================================================== */
typedef struct { cw_entity_id id; Position p; Health h; Attack a; Unit u; Body b; } Record;
static size_t capture(cw_world *w, Record *out, size_t capacity) {
    size_t n = 0;
    for (size_t i = 0; i < w->capacity && n < capacity; ++i) {
        const cw_entity *e = &w->entities[i];
        if (!cw_entity_is_active(*e) || !cw_comp_has(w, e->id, UNIT)) continue;
        Record *r = &out[n++];
        r->id = e->id;
        r->p = *(Position *)cw_comp_ptr(w, e->id, POSITION);
        r->h = *(Health *)cw_comp_ptr(w, e->id, HEALTH);
        r->a = *(Attack *)cw_comp_ptr(w, e->id, ATTACK);
        r->u = *(Unit *)cw_comp_ptr(w, e->id, UNIT);
        r->b = *(Body *)cw_comp_ptr(w, e->id, BODY);
    }
    return n;
}
static bool records_equal(const Record *a, size_t na, const Record *b, size_t nb) {
    if (na != nb) return false;
    for (size_t i = 0; i < na; ++i)
        if (memcmp(&a[i], &b[i], sizeof(Record)) != 0) return false;
    return true;
}
#define CHECK(cond, ...) do { if (!(cond)) { set_status(true, __VA_ARGS__); return 1; } } while (0)
static int run_headless(void) {
    enum { MAX_RECORDS = 512 };
    Record *before = calloc(MAX_RECORDS, sizeof(*before)), *after = calloc(MAX_RECORDS, sizeof(*after));
    if (!before || !after) { free(before); free(after); return 1; }

    /* A deterministic battle line. */
    for (int i = 0; i < 6; ++i) {
        spawn_unit(TEAM_RED, (uint8_t)(i % KIND_COUNT), 200, 150 + 90.0f * (float)i);
        spawn_unit(TEAM_BLUE, (uint8_t)((i + 1) % KIND_COUNT), ARENA_W - 200, 150 + 90.0f * (float)i);
    }
    settle();
    CHECK(cw_world_entity_count(&app.world) == 12, "expected 12 units");
    CHECK(cw_comp_has(&app.world, 0, VISUAL), "EntityCreated did not add render state");

    /* Pause semantics: no tick, no movement; a step moves. */
    Position p0 = *(Position *)cw_comp_ptr(&app.world, 0, POSITION);
    app.paused = true;
    CHECK(memcmp(&p0, cw_comp_ptr(&app.world, 0, POSITION), sizeof(p0)) == 0, "paused world moved");
    act_step(1);
    CHECK(app.world.tick == 1, "single step did not advance the tick");
    CHECK(memcmp(&p0, cw_comp_ptr(&app.world, 0, POSITION), sizeof(p0)) != 0, "single step did not move the unit");

    /* Run the battle until blood is drawn. */
    act_step(TICK_HZ * 15);
    CHECK(app.hooks.damage_dealt > 0, "no damage was dealt");
    CHECK(app.hooks.deaths[TEAM_RED] + app.hooks.deaths[TEAM_BLUE] > 0, "nobody died");
    CHECK(count_team(TEAM_RED) + count_team(TEAM_BLUE) == cw_world_entity_count(&app.world), "team counts disagree");
    set_status(false, "after %" PRIu64 " ticks: %zu red, %zu blue, %" PRIu64 " damage, %" PRIu64 "/%" PRIu64 " kills",
               app.world.tick, count_team(TEAM_RED), count_team(TEAM_BLUE), app.hooks.damage_dealt,
               app.hooks.kills[TEAM_RED], app.hooks.kills[TEAM_BLUE]);

    /* Editing and deletion through the same actions the UI uses. */
    cw_entity_id first = CW_INVALID_ENTITY;
    for (size_t i = 0; i < app.world.capacity; ++i)
        if (cw_world_is_valid(&app.world, (cw_entity_id)i)) { first = (cw_entity_id)i; break; }
    CHECK(first != CW_INVALID_ENTITY, "no unit left to edit");
    Health *h = cw_comp_ptr(&app.world, first, HEALTH);
    int32_t maximum = h->maximum;
    act_edit(&first, 1, EDIT_MAX_HEALTH, 50);
    CHECK(h->maximum == maximum + 50, "max health edit failed");
    act_edit(&first, 1, EDIT_HEAL, 0);
    CHECK(h->current == h->maximum, "heal failed");
    act_edit(&first, 1, EDIT_HEALTH, -20);
    CHECK(h->current == h->maximum - 20, "health edit failed");
    act_edit(&first, 1, EDIT_HEALTH, -100000);
    CHECK(!cw_world_is_valid(&app.world, first), "a unit edited to zero health should be destroyed");
    cw_entity_id spawned = spawn_unit(TEAM_BLUE, KIND_TANK, 600, 400);
    settle();
    cw_entity_id moved[1] = {spawned};
    act_move(moved, 1, -10000, 0);
    CHECK(((Position *)cw_comp_ptr(&app.world, spawned, POSITION))->x == kinds[KIND_TANK].radius, "move was not clamped to the arena");
    act_edit(moved, 1, EDIT_SWAP_TEAM, 0);
    CHECK(((Unit *)cw_comp_ptr(&app.world, spawned, UNIT))->team == TEAM_RED, "team swap failed");
    act_delete(moved, 1);
    CHECK(!cw_world_is_valid(&app.world, spawned), "delete failed");

    /* Lights are entities: a lamp, a flash that expires, a torch that follows
     * its unit and dies with it, a pillar that units walk around. */
    size_t before_lights = cw_world_entity_count(&app.world);
    Light lamp = light_make(255, 200, 130, 1.5f, 300);
    lamp.flicker = 0.3f;
    cw_entity_id lamp_id = spawn_light(lamp, 400, 400);
    cw_entity_id flash_id = spawn_flash(light_make(255, 255, 255, 2, 100), 500, 400, 0.1f);
    cw_entity_id carrier = spawn_unit(TEAM_RED, KIND_MELEE, 300, 700);
    cw_entity_id torch_id = spawn_torch(carrier, 255, 255, 255);
    cw_entity_id pillar_id = spawn_obstacle(640, 400, 40);
    settle();
    CHECK(cw_world_entity_count(&app.world) == before_lights + 5, "light entities were not created");
    CHECK(torch_of(carrier) == torch_id, "torch lookup failed");
    act_step(1);
    CHECK(cw_comp_has(&app.world, lamp_id, LIGHT_STATE), "light animation did not run");
    float lit = ((LightState *)cw_comp_ptr(&app.world, lamp_id, LIGHT_STATE))->intensity;
    CHECK(lit > 0.9f && lit < 2.1f && lit != 1.5f, "flicker did not animate the lamp");
    CHECK(cw_world_is_valid(&app.world, flash_id), "flash expired too early");
    act_step(4);
    CHECK(!cw_world_is_valid(&app.world, flash_id), "flash did not expire");
    {
        const Position *cp = cw_comp_ptr(&app.world, carrier, POSITION), *tp = cw_comp_ptr(&app.world, torch_id, POSITION);
        CHECK(cp->x == tp->x && cp->y == tp->y, "torch does not follow its unit");
        CHECK(((Light *)cw_comp_ptr(&app.world, torch_id, LIGHT))->flags & LIGHT_SPOT, "torch is not a spot light");
    }
    {
        const Position *pp = cw_comp_ptr(&app.world, pillar_id, POSITION);
        cw_entity_id walker = spawn_unit(TEAM_BLUE, KIND_MELEE, 640, 400);   /* right on the pillar */
        settle();
        act_step(1);
        const Position *wp = cw_comp_ptr(&app.world, walker, POSITION);
        float dx = wp->x - pp->x, dy = wp->y - pp->y;
        CHECK(sqrtf(dx * dx + dy * dy) >= 40 + kinds[KIND_MELEE].radius - 0.01f, "unit was not pushed out of the pillar");
        cw_entity_id wid[1] = {walker};
        act_delete(wid, 1);
    }
    Light lamp_saved = *(Light *)cw_comp_ptr(&app.world, lamp_id, LIGHT);

    /* Snapshot: save, keep simulating, load into a fresh world, simulate the
     * same number of ticks and expect bit-identical state. */
    CHECK(act_save(), "save failed");
    CHECK(app.have_saved && app.last_saved.user_version == SNAPSHOT_VERSION, "snapshot header not readable");
    size_t saved_entities = cw_world_entity_count(&app.world);
    uint64_t saved_tick = app.world.tick;
    act_step(45);
    size_t n_before = capture(&app.world, before, MAX_RECORDS);
    CHECK(act_load(), "load failed");
    CHECK(cw_world_entity_count(&app.world) == saved_entities && app.world.tick == saved_tick, "loaded world differs from what was saved");
    for (size_t i = 0; i < app.world.capacity; ++i) {
        if (!cw_world_is_valid(&app.world, (cw_entity_id)i)) continue;
        CHECK(cw_comp_has(&app.world, (cw_entity_id)i, VISUAL), "loaded entity %zu has no render state", i);
        CHECK(!cw_comp_has(&app.world, (cw_entity_id)i, TARGET), "transient Target was loaded");
    }
    CHECK(cw_world_is_valid(&app.world, lamp_id) && cw_comp_has(&app.world, lamp_id, LIGHT), "lamp did not survive the snapshot");
    CHECK(memcmp(cw_comp_ptr(&app.world, lamp_id, LIGHT), &lamp_saved, sizeof(Light)) == 0, "lamp light differs after load");
    CHECK(cw_comp_has(&app.world, pillar_id, OBSTACLE) && cw_comp_has(&app.world, pillar_id, SHADOW_CASTER), "pillar did not survive the snapshot");
    CHECK(torch_of(carrier) == torch_id && cw_comp_has(&app.world, torch_id, ATTACHED), "torch attachment did not survive the snapshot");
    act_step(45);
    size_t n_after = capture(&app.world, after, MAX_RECORDS);
    CHECK(records_equal(before, n_before, after, n_after), "simulation after load diverged from the original");
    {
        const Position *cp = cw_comp_ptr(&app.world, carrier, POSITION), *tp = cw_comp_ptr(&app.world, torch_id, POSITION);
        CHECK(cp->x == tp->x && cp->y == tp->y, "torch lost its unit after the load");
        cw_entity_id cid[1] = {carrier};
        act_delete(cid, 1);
        act_step(1);
        CHECK(!cw_world_is_valid(&app.world, torch_id), "torch outlived its unit");
    }
    set_status(false, "snapshot round trip: %zu units, identical state after 45 more ticks", n_after);

    /* A corrupt file is rejected and leaves an empty, usable world. */
    cw_buffer image;
    cw_file_error error;
    CHECK(cw_file_read(app.path, 0, &image, &error), "cannot re-read the snapshot");
    image.data[8] ^= 0xff;   /* the total size in the header */
    CHECK(cw_file_write(app.path, image.data, image.size, &error), "cannot write the corrupted snapshot");
    cw_buffer_free(&image);
    bool loaded = act_load();
    app.failed = false;   /* the load error above was expected */
    CHECK(!loaded, "a corrupted snapshot was accepted");
    CHECK(cw_world_entity_count(&app.world) == 0, "failed load left entities behind");
    act_spawn_wave(2);
    CHECK(cw_world_entity_count(&app.world) == 4, "world unusable after a failed load");
    act_step(10);

    remove(app.path);
    free(before);
    free(after);
    if (app.failed) return 1;
    printf("OK: combat demo verified (movement, damage, death, lights, editing, deletion, pause/step, snapshot round trip)\n");
    return 0;
}

/* ===========================================================================
 * Window: rendering and interaction on top of render2d
 * ======================================================================== */
#include "render2d.h"
#include "r2d_particles.h"
#include "r2d_effects.h"
#include "r2d_post.h"
#include "r2d_lighting.h"
#include "sokol_log.h"

#define WINDOW_W 1280
#define WINDOW_H 800
#define PANEL_W 258.0f
#define STATUS_H 60.0f

typedef enum { TOOL_SELECT, TOOL_PLACE_RED, TOOL_PLACE_BLUE, TOOL_PLACE_LIGHT } Tool;
/* lamp presets for the light placement tool */
static const Light lamp_kinds[] = {
    {255, 200, 130, 0, 1.6f, 320, 2.0f, 1.2f, 0, 1.0f, 0.2f, 6.0f},            /* warm lantern */
    {140, 200, 255, 0, 1.4f, 360, 2.0f, 1.0f, 0, 1.0f, 0, 6.0f},               /* cool lamp */
    {255, 255, 255, LIGHT_SPOT, 1.3f, 460, 1.5f, 1.4f, 0, 0.9f, 0, 6.0f},      /* spotlight */
    {120, 255, 150, 0, 1.3f, 260, 2.0f, 1.0f, 0, 1.0f, 0.5f, 12.0f},           /* flickering green */
};
#define LAMP_KIND_COUNT ((int)(sizeof(lamp_kinds) / sizeof(lamp_kinds[0])))
static const char *lamp_names[] = {"lantern", "cool lamp", "spotlight", "green"};
static struct {
    bool ready, failed;
    unsigned frames, max_frames;
    Tool tool;
    uint8_t place_kind;
    float place_cooldown;
    bool show_ranges, show_grid, follow_selection, show_fx;
    float fps;
    r2d_particles *particles;
    r2d_effects *effects;
    char config_path[512];       /* tunable.cfg for the post-process stack */
    bool show_lights;            /* map light entities and shadow casters to the renderer */
    struct { r2d_vec2 pos; r2d_color color; float radius, life; } flash_queue[64];   /* to spawn next frame */
    int flash_count;
    bool tuning_open;            /* the post-process tuning panel */
    int tuning_section;
} ui;

static const r2d_color team_colors[TEAM_COUNT] = {{232, 84, 70, 255}, {72, 144, 236, 255}};
static const r2d_color team_dark[TEAM_COUNT] = {{120, 40, 36, 255}, {34, 70, 124, 255}};
static const r2d_color white = {240, 242, 246, 255};
static const r2d_color yellow = {255, 214, 80, 255};

static r2d_rect arena_rect(void) { return r2d_rect_make(0, 0, ARENA_W, ARENA_H); }

/* --- particle presets and combat event listeners --- */
#define PI_F 3.14159265358979f
static float angle_between(Position from, Position to) { return atan2f(to.y - from.y, to.x - from.x); }

static r2d_particle_desc preset_sparks(uint8_t team) {
    r2d_particle_desc d = r2d_particle_desc_default();
    d.shape = R2D_PARTICLE_STREAK;
    d.lifetime = r2d_range_make(0.15f, 0.4f);
    d.speed = r2d_range_make(120, 280);
    d.spread = 1.6f;                       /* a cone, aimed away from the attacker */
    d.size = r2d_range_make(1.2f, 2.2f);
    d.size_end_scale = 0.3f;
    d.color_start = white;
    d.color_end = r2d_with_alpha(team_colors[team], 0);
    d.gravity = r2d_v2(0, 260);
    d.drag = 3.0f;
    d.streak_length = 0.04f;
    return d;
}
static r2d_particle_desc preset_explosion(uint8_t team) {
    r2d_particle_desc d = r2d_particle_desc_default();
    d.shape = R2D_PARTICLE_CIRCLE;
    d.lifetime = r2d_range_make(0.35f, 0.8f);
    d.speed = r2d_range_make(30, 170);
    d.size = r2d_range_make(3, 7);
    d.size_end_scale = 0;
    d.color_start = r2d_color_mix(team_colors[team], white, 0.5f);
    d.color_end = r2d_with_alpha(team_dark[team], 0);
    d.drag = 2.5f;
    d.emit_radius = 4;
    return d;
}
static r2d_particle_desc preset_debris(uint8_t team) {
    r2d_particle_desc d = r2d_particle_desc_default();
    d.shape = R2D_PARTICLE_SQUARE;
    d.lifetime = r2d_range_make(0.6f, 1.1f);
    d.speed = r2d_range_make(60, 220);
    d.direction = -PI_F / 2;               /* mostly upwards */
    d.spread = 2.4f;
    d.size = r2d_range_make(1.5f, 3.0f);
    d.size_end_scale = 0.6f;
    d.color_start = team_colors[team];
    d.color_end = r2d_with_alpha(team_dark[team], 0);
    d.gravity = r2d_v2(0, 320);
    d.drag = 0.8f;
    d.spin = r2d_range_make(-14, 14);
    return d;
}
static r2d_particle_desc preset_smoke(void) {
    r2d_particle_desc d = r2d_particle_desc_default();
    d.lifetime = r2d_range_make(0.8f, 1.5f);
    d.speed = r2d_range_make(8, 22);
    d.direction = -PI_F / 2;
    d.spread = 0.9f;
    d.size = r2d_range_make(2.5f, 4.5f);
    d.size_end_scale = 2.6f;
    d.color_start = r2d_rgba(110, 110, 118, 150);
    d.color_end = r2d_rgba(70, 70, 76, 0);
    d.fade_in = 0.15f;
    d.gravity = r2d_v2(0, -18);
    d.emit_radius = 4;
    d.inherit_velocity = 0.4f;
    d.rate = 16;
    return d;
}
static r2d_particle_desc preset_dust(void) {
    r2d_particle_desc d = r2d_particle_desc_default();
    d.lifetime = r2d_range_make(0.35f, 0.7f);
    d.speed = r2d_range_make(8, 30);
    d.spread = 1.2f;                       /* direction is set to trail the mover */
    d.size = r2d_range_make(2, 4);
    d.size_end_scale = 2.0f;
    d.color_start = r2d_rgba(160, 145, 118, 110);
    d.color_end = r2d_rgba(160, 145, 118, 0);
    d.emit_radius = 6;
    d.rate = 0;
    return d;
}
static r2d_particle_desc preset_sparkle(r2d_color color) {
    r2d_particle_desc d = r2d_particle_desc_default();
    d.shape = R2D_PARTICLE_RING;
    d.lifetime = r2d_range_make(0.3f, 0.6f);
    d.speed = r2d_range_make(30, 90);
    d.size = r2d_range_make(1, 2);
    d.size_end_scale = 3.0f;
    d.color_start = r2d_with_alpha(color, 220);
    d.color_end = r2d_with_alpha(color, 0);
    d.drag = 2.0f;
    return d;
}

/* --- lights: every light is an entity; the window only maps components to the renderer --- */
static r2d_vec2 render_position(cw_entity_id id, const Position *p);
/* Flash lights requested by the simulation listeners are spawned as
 * short-lived light entities at the start of the next frame (the listeners
 * run inside the tick, where creating entities would be awkward). */
static void flash_light(r2d_vec2 pos, r2d_color color, float radius, float life) {
    if (ui.flash_count >= 64) return;
    ui.flash_queue[ui.flash_count].pos = pos;
    ui.flash_queue[ui.flash_count].color = color;
    ui.flash_queue[ui.flash_count].radius = radius;
    ui.flash_queue[ui.flash_count].life = life;
    ++ui.flash_count;
}
static void spawn_queued_flashes(void) {
    for (int i = 0; i < ui.flash_count; ++i) {
        Light l = light_make(ui.flash_queue[i].color.r, ui.flash_queue[i].color.g, ui.flash_queue[i].color.b, 2.2f, ui.flash_queue[i].radius);
        l.volumetric = 2.0f;
        spawn_flash(l, ui.flash_queue[i].pos.x, ui.flash_queue[i].pos.y, ui.flash_queue[i].life);
    }
    if (ui.flash_count) settle();
    ui.flash_count = 0;
}
static r2d_color light_color(const Light *l) { return r2d_rgb(l->r, l->g, l->b); }
static void submit_lights(void) {
    if (!ui.show_lights) return;
    cw_world *w = &app.world;
    cw_entity_id id;
    /* shadow casters: the arena walls plus every ShadowCaster entity */
    r2d_light_occluder_rect(arena_rect());
    const cw_component_id caster_parts[] = {SHADOW_CASTER, POSITION};
    cw_query casters = cw_query_iter(w, cw_mask(caster_parts, 2));
    while (cw_query_next(&casters, &id)) {
        float radius = cw_comp_has(w, id, BODY) ? ((Body *)cw_comp_ptr(w, id, BODY))->radius * 0.9f
                     : cw_comp_has(w, id, OBSTACLE) ? ((Obstacle *)cw_comp_ptr(w, id, OBSTACLE))->radius : 0;
        if (radius > 0) r2d_light_occluder_circle(render_position(id, cw_comp_ptr(w, id, POSITION)), radius);
    }
    /* lights */
    const cw_component_id light_parts[] = {LIGHT, POSITION};
    cw_query lights = cw_query_iter(w, cw_mask(light_parts, 2));
    while (cw_query_next(&lights, &id)) {
        const Light *l = cw_comp_ptr(w, id, LIGHT);
        r2d_light_desc d;
        memset(&d, 0, sizeof(d));
        d.position = render_position(id, cw_comp_ptr(w, id, POSITION));
        d.radius = l->radius;
        d.color = light_color(l);
        d.intensity = cw_comp_has(w, id, LIGHT_STATE) ? ((LightState *)cw_comp_ptr(w, id, LIGHT_STATE))->intensity : l->intensity;
        d.falloff = l->falloff;
        d.volumetric = l->volumetric;
        d.no_shadows = (l->flags & LIGHT_NO_SHADOWS) != 0;
        d.spot = (l->flags & LIGHT_SPOT) != 0;
        d.direction = l->direction;
        d.cone = l->cone;
        d.cone_softness = 0.4f;
        if (d.intensity > 0) r2d_light(&d);
    }
    /* the placement cursor previews what it would place */
    if (ui.tool != TOOL_SELECT && !r2d_mouse_over_ui()) {
        r2d_light_desc d;
        memset(&d, 0, sizeof(d));
        d.position = r2d_mouse_world();
        d.no_shadows = true;
        if (ui.tool == TOOL_PLACE_LIGHT) {
            const Light *l = &lamp_kinds[ui.place_kind % LAMP_KIND_COUNT];
            d.radius = l->radius; d.color = light_color(l); d.intensity = l->intensity * 0.7f; d.falloff = l->falloff;
            d.spot = (l->flags & LIGHT_SPOT) != 0; d.direction = l->direction; d.cone = l->cone; d.cone_softness = 0.4f;
        } else {
            d.radius = 160; d.color = team_colors[ui.tool == TOOL_PLACE_RED ? TEAM_RED : TEAM_BLUE]; d.intensity = 1.2f;
        }
        r2d_light(&d);
    }
}
static void draw_obstacles(void) {
    const cw_component_id parts[] = {OBSTACLE, POSITION};
    cw_query q = cw_query_iter(&app.world, cw_mask(parts, 2));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        const Position *p = cw_comp_ptr(&app.world, id, POSITION);
        float r = ((Obstacle *)cw_comp_ptr(&app.world, id, OBSTACLE))->radius;
        r2d_vec2 c = r2d_v2(p->x, p->y);
        r2d_draw_circle(c, r, r2d_rgb(58, 62, 72));
        r2d_draw_circle(c, r * 0.8f, r2d_rgb(76, 80, 92));
        r2d_draw_ring(c, r, r2d_px(1.5f), r2d_rgb(110, 116, 130));
        if (r2d_is_selected((uint64_t)id)) r2d_draw_ring(c, r + r2d_px(4), r2d_px(2), yellow);
        r2d_pick_circle((uint64_t)id, c, r);
    }
}
/* Light icons: a small sun glyph, pickable like a unit. */
static void draw_light_icons(uint64_t hovered) {
    const cw_component_id parts[] = {LIGHT, POSITION};
    cw_query q = cw_query_iter(&app.world, cw_mask(parts, 2));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        if (cw_comp_has(&app.world, id, LIFETIME)) continue;   /* flashes are not editable */
        const Light *l = cw_comp_ptr(&app.world, id, LIGHT);
        r2d_vec2 c = render_position(id, cw_comp_ptr(&app.world, id, POSITION));
        float r = r2d_px(7);
        bool selected = r2d_is_selected((uint64_t)id), hover = hovered == (uint64_t)id;
        r2d_color col = light_color(l);
        r2d_draw_circle(c, r, r2d_with_alpha(col, 230));
        r2d_draw_ring(c, r * 1.6f, r2d_px(1), r2d_with_alpha(col, 150));
        for (int k = 0; k < 8; ++k) {
            float a = (float)k * PI_F / 4.0f;
            r2d_draw_line(r2d_add(c, r2d_scale(r2d_v2(cosf(a), sinf(a)), r * 1.9f)),
                          r2d_add(c, r2d_scale(r2d_v2(cosf(a), sinf(a)), r * 2.6f)), r2d_px(1), r2d_with_alpha(col, 160));
        }
        if (l->flags & LIGHT_SPOT) {
            r2d_vec2 dir = r2d_v2(cosf(l->direction), sinf(l->direction));
            r2d_draw_line(c, r2d_add(c, r2d_scale(dir, r * 4.5f)), r2d_px(2), r2d_with_alpha(col, 200));
        }
        if (selected) {
            r2d_draw_ring(c, r * 3.0f, r2d_px(2), yellow);
            r2d_draw_ring(c, l->radius, r2d_px(1), r2d_with_alpha(col, 90));   /* reach */
        } else if (hover) r2d_draw_ring(c, r * 3.0f, r2d_px(1.5f), r2d_with_alpha(white, 170));
        if (selected || hover) r2d_draw_textf(r2d_add(c, r2d_v2(r * 3.2f, -r2d_px(6))), r2d_with_alpha(white, 200), "#%" PRId64 " light", id);
        r2d_pick_circle((uint64_t)id, c, r * 2.2f);
    }
}

/* Particle counts and emitter rates follow the quality tier. */
static int scaled(int count) {
    float n = (float)count * r2d_post_particle_scale();
    return n < 1 ? 1 : (int)(n + 0.5f);
}
static void fx_attack(void *listener, Position from, Position to, uint8_t team, uint8_t kind) {
    (void)listener;
    if (!ui.show_fx) return;
    r2d_vec2 a = r2d_v2(from.x, from.y), b = r2d_v2(to.x, to.y);
    r2d_color c = r2d_color_mix(team_colors[team], white, 0.35f);
    switch (kind) {
        case KIND_ARCHER:   /* an arrow: a beam that fades, plus a puff at the bow */
            r2d_effects_beam(ui.effects, r2d_lerp(a, b, 0.15f), b, 2.0f, r2d_with_alpha(c, 210), 0.22f);
            break;
        case KIND_TANK:     /* a heavy slam: ring pulse at the target */
            r2d_effects_ring(ui.effects, b, 4, 26, 3, r2d_with_alpha(c, 200), 0.3f, R2D_EASE_OUT_CUBIC);
            break;
        default: {          /* a slash arc in front of the attacker */
            float facing = angle_between(from, to);
            r2d_effects_arc(ui.effects, a, kinds[kind].radius + 9, 3.5f, facing - 0.9f, 1.8f, r2d_with_alpha(c, 230), 0.25f);
            break;
        }
    }
}
static void fx_damage(void *listener, Position at, int32_t amount, uint8_t team, Position from) {
    (void)listener;
    if (!ui.show_fx) return;
    char text[16];
    snprintf(text, sizeof(text), "-%" PRId32, amount);
    r2d_effects_text(ui.effects, r2d_v2(at.x, at.y - 16), text, white, 0.8f, r2d_v2(0, -28));
    flash_light(r2d_v2(at.x, at.y), r2d_color_mix(team_colors[team], white, 0.6f), 90, 0.18f);
    r2d_particle_desc sparks = preset_sparks(team);
    sparks.direction = angle_between(from, at);   /* sparks fly away from the attacker */
    r2d_particles_burst(ui.particles, &sparks, r2d_v2(at.x, at.y), scaled(3 + amount / 3));
}
static void fx_death(void *listener, Position at, uint8_t team, float radius, bool killed) {
    (void)listener;
    if (!ui.show_fx) return;
    r2d_vec2 p = r2d_v2(at.x, at.y);
    r2d_color c = team_colors[team];
    r2d_effects_ring(ui.effects, p, radius, radius * 3.2f, 2.5f, c, 0.5f, R2D_EASE_OUT);
    if (!killed) return;   /* deleted by the user: just the ring */
    flash_light(p, r2d_color_mix(c, white, 0.5f), radius * 14, 0.45f);
    r2d_effects_disc(ui.effects, p, radius * 1.8f, r2d_with_alpha(white, 160), 0.25f);
    r2d_particle_desc explosion = preset_explosion(team), debris = preset_debris(team);
    r2d_particles_burst(ui.particles, &explosion, p, scaled((int)(radius * 1.6f)));
    r2d_particles_burst(ui.particles, &debris, p, scaled((int)(radius * 0.9f)));
    const r2d_post_params *post = r2d_post_params_ptr();
    if (r2d_post_active() && post && post->shake.enabled) r2d_post_shake(0.25f + radius * 0.015f);
    else r2d_effects_shake(ui.effects, 2.0f + radius * 0.25f, 28, 0.35f);
}
static void fx_visual_release(void *listener, Visual *v) {
    (void)listener;
    if (!ui.particles) return;
    r2d_emitter smoke = {v->smoke}, dust = {v->dust};
    r2d_emitter_stop(ui.particles, smoke);
    r2d_emitter_stop(ui.particles, dust);
}
static void fx_spawn(r2d_vec2 at, uint8_t team, float radius) {
    if (!ui.show_fx) return;
    r2d_particle_desc sparkle = preset_sparkle(team_colors[team]);
    r2d_particles_burst(ui.particles, &sparkle, at, scaled(14));
    r2d_effects_ring(ui.effects, at, radius * 2.5f, radius + 2, 2, r2d_with_alpha(team_colors[team], 220), 0.35f, R2D_EASE_OUT);
}
/* Emitters that follow a unit: smoke when badly hurt, dust when a tank rolls. */
static void update_attached_emitters(cw_entity_id id, r2d_vec2 pos) {
    cw_world *w = &app.world;
    if (!cw_comp_has(w, id, VISUAL)) return;
    Visual *v = cw_comp_ptr(w, id, VISUAL);
    r2d_emitter smoke = {v->smoke}, dust = {v->dust};
    bool hurt = false;
    if (cw_comp_has(w, id, HEALTH)) {
        const Health *h = cw_comp_ptr(w, id, HEALTH);
        hurt = h->current > 0 && h->maximum > 0 && (float)h->current / (float)h->maximum < 0.35f;
    }
    if (hurt && ui.show_fx) {
        if (!r2d_emitter_alive(ui.particles, smoke)) {
            r2d_particle_desc d = preset_smoke();
            d.rate *= r2d_post_particle_scale();
            smoke = r2d_emitter_start(ui.particles, &d, pos, 0);
            v->smoke = smoke.id;
        }
        r2d_emitter_move(ui.particles, smoke, pos);
    } else if (v->smoke) {
        r2d_emitter_stop(ui.particles, smoke);
        v->smoke = 0;
    }
    float speed = 0;
    r2d_vec2 vel = r2d_v2(0, 0);
    if (cw_comp_has(w, id, VELOCITY) && cw_comp_has(w, id, UNIT) && ((Unit *)cw_comp_ptr(w, id, UNIT))->kind == KIND_TANK) {
        const Velocity *vv = cw_comp_ptr(w, id, VELOCITY);
        vel = r2d_v2(vv->x, vv->y);
        speed = r2d_length(vel);
    }
    if (speed > 5 && ui.show_fx && !app.paused) {
        if (!r2d_emitter_alive(ui.particles, dust)) {
            r2d_particle_desc d = preset_dust();
            dust = r2d_emitter_start(ui.particles, &d, pos, 0);
            v->dust = dust.id;
        }
        r2d_emitter_move(ui.particles, dust, pos);
        r2d_emitter_set_rate(ui.particles, dust, 0.25f * speed * r2d_post_particle_scale());
        r2d_emitter_set_direction(ui.particles, dust, atan2f(-vel.y, -vel.x));
    } else if (v->dust) {
        r2d_emitter_stop(ui.particles, dust);
        v->dust = 0;
    }
}

static r2d_rect world_area(void) {
    r2d_vec2 v = r2d_viewport();
    return r2d_rect_make(PANEL_W + 24, 0, v.x - PANEL_W - 24, v.y - STATUS_H);
}
static void fit_arena(void) { r2d_camera_fit_rect_in(world_area(), arena_rect(), 24); }
static bool keep_alive(uint64_t id, void *user) {
    (void)user;
    return cw_world_is_valid(&app.world, (cw_entity_id)id)
        && !(app.world.entities[(size_t)id].flags & CW_TO_DESTROY);
}
/* Copy of the selection as entity ids (the actions may mutate the selection). */
static size_t selected_ids(cw_entity_id **out) {
    static cw_entity_id *buffer;
    static size_t capacity;
    size_t n;
    const uint64_t *ids = r2d_selection(&n);
    if (n > capacity) {
        capacity = n * 2;
        buffer = realloc(buffer, capacity * sizeof(*buffer));
        if (!buffer) { fputs("out of memory\n", stderr); abort(); }
    }
    for (size_t i = 0; i < n; ++i) buffer[i] = (cw_entity_id)ids[i];
    *out = buffer;
    return n;
}
static void toggle_pause(void) {
    app.paused = !app.paused;
    set_status(false, app.paused ? "paused at tick %" PRIu64 " (N steps one tick)" : "running at tick %" PRIu64, app.world.tick);
}
static void single_step(void) {
    app.paused = true;
    act_step(1);
    cw_clock_reset(&app.clock);
    set_status(false, "stepped to tick %" PRIu64, app.world.tick);
}
static void change_speed(int direction) {
    static const float speeds[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
    int index = 2;
    for (int i = 0; i < 5; ++i) if (fabsf(speeds[i] - app.speed) < 0.01f) index = i;
    index += direction;
    if (index < 0) index = 0;
    if (index > 4) index = 4;
    app.speed = speeds[index];
    set_status(false, "speed x%.2g", (double)app.speed);
}
static void delete_selection(void) {
    cw_entity_id *ids;
    size_t n = selected_ids(&ids);
    if (!n) { set_status(true, "nothing selected to delete"); return; }
    act_delete(ids, n);
    r2d_select_clear();
}
static void edit_selection(EditOp op, float amount, const char *what) {
    cw_entity_id *ids;
    size_t n = selected_ids(&ids);
    if (!n) { set_status(true, "nothing selected to edit"); return; }
    size_t changed = act_edit(ids, n, op, amount);
    if (op == EDIT_HEAL && ui.show_fx) {
        r2d_particle_desc d = preset_sparkle(r2d_rgb(120, 230, 140));
        d.direction = -PI_F / 2;
        d.spread = 1.2f;
        for (size_t i = 0; i < n; ++i)
            if (cw_world_is_valid(&app.world, ids[i]) && cw_comp_has(&app.world, ids[i], POSITION)) {
                const Position *p = cw_comp_ptr(&app.world, ids[i], POSITION);
                r2d_particles_burst(ui.particles, &d, r2d_v2(p->x, p->y), scaled(12));
            }
    }
    set_status(false, "%s on %zu unit%s", what, changed, changed == 1 ? "" : "s");
}

static void save_with_fx(void) {
    if (act_save()) r2d_effects_flash(ui.effects, r2d_rgba(120, 200, 255, 70), 0.35f);
}
static void load_with_fx(void) {
    r2d_select_clear();
    r2d_particles_clear(ui.particles);      /* the old world's emitters are gone with it */
    r2d_effects_clear(ui.effects);
    if (act_load()) r2d_effects_flash(ui.effects, r2d_rgba(255, 255, 255, 110), 0.4f);
    else r2d_effects_flash(ui.effects, r2d_rgba(255, 80, 60, 110), 0.4f);
}
static void advance_simulation(void) {
    if (app.paused) { settle(); return; }
    int steps = cw_clock_advance(&app.clock, r2d_dt() * app.speed);
    if (steps) act_step(steps);
}

static r2d_vec2 render_position(cw_entity_id id, const Position *p) {
    if (app.paused || !cw_comp_has(&app.world, id, VISUAL)) return r2d_v2(p->x, p->y);
    const Visual *v = cw_comp_ptr(&app.world, id, VISUAL);
    float alpha = cw_clock_alpha(&app.clock);
    return r2d_lerp(r2d_v2(v->previous.x, v->previous.y), r2d_v2(p->x, p->y), alpha);
}
static r2d_vec2 facing_of(cw_entity_id id, r2d_vec2 pos) {
    cw_world *w = &app.world;
    if (cw_comp_has(w, id, VELOCITY)) {
        const Velocity *v = cw_comp_ptr(w, id, VELOCITY);
        if (v->x * v->x + v->y * v->y > 1.0f) return r2d_normalize(r2d_v2(v->x, v->y));
    }
    if (cw_comp_has(w, id, TARGET)) {
        const Target *t = cw_comp_ptr(w, id, TARGET);
        if (cw_world_is_valid(w, t->id) && cw_comp_has(w, t->id, POSITION)) {
            const Position *tp = cw_comp_ptr(w, t->id, POSITION);
            return r2d_normalize(r2d_sub(r2d_v2(tp->x, tp->y), pos));
        }
    }
    return r2d_v2(1, 0);
}
static void draw_unit(cw_entity_id id, uint64_t hovered) {
    cw_world *w = &app.world;
    const Position *p = cw_comp_ptr(w, id, POSITION);
    const Unit *u = cw_comp_ptr(w, id, UNIT);
    const Body *b = cw_comp_ptr(w, id, BODY);
    r2d_vec2 pos = render_position(id, p);
    r2d_vec2 dir = facing_of(id, pos);
    bool selected = r2d_is_selected((uint64_t)id), hover = hovered == (uint64_t)id;
    bool pending = (w->entities[id].flags & CW_TO_DESTROY) != 0;
    r2d_color fill = team_colors[u->team], rim = team_dark[u->team];
    if (cw_comp_has(w, id, VISUAL) && ((Visual *)cw_comp_ptr(w, id, VISUAL))->flash > 0) fill = white;
    if (pending) fill = r2d_with_alpha(fill, 90);

    if (selected && ui.show_ranges && cw_comp_has(w, id, ATTACK)) {
        const Attack *a = cw_comp_ptr(w, id, ATTACK);
        r2d_draw_circle(pos, a->range, r2d_with_alpha(fill, 22));
        r2d_draw_ring(pos, a->range, r2d_px(1), r2d_with_alpha(fill, 110));
    }
    if (selected && cw_comp_has(w, id, TARGET)) {
        const Target *t = cw_comp_ptr(w, id, TARGET);
        if (cw_world_is_valid(w, t->id) && cw_comp_has(w, t->id, POSITION)) {
            r2d_vec2 tp = render_position(t->id, cw_comp_ptr(w, t->id, POSITION));
            r2d_draw_line(pos, tp, r2d_px(1), r2d_with_alpha(yellow, 140));
        }
    }
    if (selected) r2d_draw_ring(pos, b->radius + r2d_px(4), r2d_px(2), yellow);
    else if (hover) r2d_draw_ring(pos, b->radius + r2d_px(3), r2d_px(1.5f), r2d_with_alpha(white, 170));

    switch (u->kind) {
        case KIND_ARCHER: {
            r2d_vec2 side = r2d_v2(-dir.y, dir.x);
            r2d_vec2 tip = r2d_add(pos, r2d_scale(dir, b->radius * 1.3f));
            r2d_vec2 l = r2d_add(r2d_sub(pos, r2d_scale(dir, b->radius * 0.8f)), r2d_scale(side, b->radius));
            r2d_vec2 r = r2d_sub(r2d_sub(pos, r2d_scale(dir, b->radius * 0.8f)), r2d_scale(side, b->radius));
            r2d_draw_triangle(tip, l, r, rim);
            r2d_draw_triangle(r2d_lerp(pos, tip, 0.7f), r2d_lerp(pos, l, 0.7f), r2d_lerp(pos, r, 0.7f), fill);
            break;
        }
        case KIND_TANK:
            r2d_draw_oriented_rect(pos, dir, b->radius * 2, b->radius * 1.7f, rim);
            r2d_draw_oriented_rect(pos, dir, b->radius * 1.5f, b->radius * 1.2f, fill);
            break;
        default:
            r2d_draw_circle(pos, b->radius, rim);
            r2d_draw_circle(pos, b->radius * 0.72f, fill);
            r2d_draw_line(pos, r2d_add(pos, r2d_scale(dir, b->radius * 1.1f)), r2d_px(2), fill);
            break;
    }
    if (cw_comp_has(w, id, HEALTH)) {
        const Health *h = cw_comp_ptr(w, id, HEALTH);
        float t = h->maximum ? (float)h->current / (float)h->maximum : 0;
        r2d_rect bar = r2d_rect_make(pos.x - b->radius, pos.y - b->radius - 7, b->radius * 2, 4);
        r2d_draw_bar(bar, t, r2d_color_mix(r2d_rgb(220, 60, 50), r2d_rgb(80, 210, 90), t), r2d_rgba(0, 0, 0, 160));
    }
    if (pending) {
        float s = b->radius;
        r2d_draw_line(r2d_v2(pos.x - s, pos.y - s), r2d_v2(pos.x + s, pos.y + s), r2d_px(2), white);
        r2d_draw_line(r2d_v2(pos.x - s, pos.y + s), r2d_v2(pos.x + s, pos.y - s), r2d_px(2), white);
    }
    if (selected || hover || r2d_camera_ptr()->zoom > 1.8f)
        r2d_draw_textf(r2d_add(pos, r2d_v2(b->radius + r2d_px(4), -r2d_px(6))), r2d_with_alpha(white, 200), "#%" PRId64, id);
    r2d_pick_circle((uint64_t)id, pos, b->radius + 2);
    if (!pending) update_attached_emitters(id, pos);
}
static void draw_world(uint64_t hovered) {
    r2d_set_space(R2D_SPACE_WORLD);
    r2d_draw_rect(arena_rect(), r2d_rgb(33, 36, 44));
    if (ui.show_grid) r2d_draw_grid(arena_rect(), 50, 24, r2d_rgba(255, 255, 255, 16));
    r2d_draw_rect_outline(arena_rect(), r2d_px(2), r2d_rgb(90, 96, 110));
    draw_obstacles();
    const cw_component_id required[] = {POSITION, UNIT, BODY};
    cw_query q = cw_query_iter(&app.world, cw_mask(required, 3));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) draw_unit(id, hovered);
    /* entities pending destruction are invisible to queries but still drawn */
    cw_id_view pending = cw_world_pending_destroy(&app.world);
    for (size_t i = 0; i < pending.count; ++i)
        if (cw_comp_has(&app.world, pending.data[i], UNIT) && cw_comp_has(&app.world, pending.data[i], BODY))
            draw_unit(pending.data[i], hovered);
    if (ui.show_fx) {
        r2d_particles_draw(ui.particles);
        r2d_effects_draw(ui.effects, R2D_SPACE_WORLD);
    }
    draw_light_icons(hovered);
    submit_lights();
    if (ui.tool != TOOL_SELECT && ui.tool != TOOL_PLACE_LIGHT && !r2d_mouse_over_ui()) {
        const UnitKind *k = &kinds[ui.place_kind];
        r2d_color c = team_colors[ui.tool == TOOL_PLACE_RED ? TEAM_RED : TEAM_BLUE];
        r2d_draw_ring(r2d_mouse_world(), k->radius, r2d_px(1.5f), r2d_with_alpha(c, 200));
        r2d_draw_ring(r2d_mouse_world(), k->range, r2d_px(1), r2d_with_alpha(c, 60));
    }
}

/* --- the control panel --- */
typedef struct { float x, y, w; } Layout;
static r2d_rect row_rect(Layout *l, float height) {
    r2d_rect r = r2d_rect_make(l->x, l->y, l->w, height);
    l->y += height + 4;
    return r;
}
static r2d_rect split(r2d_rect r, int index, int count) {
    float gap = 4, w = (r.w - gap * (float)(count - 1)) / (float)count;
    return r2d_rect_make(r.x + (float)index * (w + gap), r.y, w, r.h);
}
static void heading(Layout *l, const char *text) {
    l->y += 4;
    r2d_ui_label_dim(r2d_v2(l->x, l->y), text);
    l->y += r2d_text_height() + 6;
}
static bool stepper(Layout *l, const char *label, const char *value, bool *minus, bool *plus) {
    r2d_rect r = row_rect(l, 24);
    r2d_ui_label(r2d_v2(r.x, r.y + 6), label);
    r2d_ui_label_colored(r2d_v2(r.x + 84, r.y + 6), yellow, value);
    *minus = r2d_ui_button(r2d_rect_make(r.x + r.w - 58, r.y, 27, r.h), "-");
    *plus = r2d_ui_button(r2d_rect_make(r.x + r.w - 27, r.y, 27, r.h), "+");
    return *minus || *plus;
}
/* Light edits apply to every selected light. */
typedef enum { LEDIT_INTENSITY, LEDIT_RADIUS, LEDIT_FALLOFF, LEDIT_VOLUMETRIC, LEDIT_CONE, LEDIT_DIRECTION, LEDIT_FLICKER,
               LEDIT_COLOR, LEDIT_SPOT, LEDIT_SHADOWS } LightEdit;
static const r2d_color light_palette[] = {{255, 200, 130, 255}, {255, 120, 80, 255}, {140, 200, 255, 255}, {120, 255, 150, 255},
                                          {255, 255, 255, 255}, {230, 120, 255, 255}};
static void edit_lights(LightEdit op, float amount) {
    cw_entity_id *ids;
    size_t n = selected_ids(&ids), changed = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!cw_world_is_valid(&app.world, ids[i]) || !cw_comp_has(&app.world, ids[i], LIGHT)) continue;
        Light *l = cw_comp_ptr(&app.world, ids[i], LIGHT);
        switch (op) {
            case LEDIT_INTENSITY: l->intensity = fmaxf(0, fminf(l->intensity + amount, 10)); break;
            case LEDIT_RADIUS: l->radius = fmaxf(20, fminf(l->radius + amount, 3000)); break;
            case LEDIT_FALLOFF: l->falloff = fmaxf(0.25f, fminf(l->falloff + amount, 6)); break;
            case LEDIT_VOLUMETRIC: l->volumetric = fmaxf(0, fminf(l->volumetric + amount, 5)); break;
            case LEDIT_CONE: l->cone = fmaxf(0.1f, fminf(l->cone + amount, 6.2f)); break;
            case LEDIT_DIRECTION: l->direction += amount; break;
            case LEDIT_FLICKER: l->flicker = fmaxf(0, fminf(l->flicker + amount, 1)); break;
            case LEDIT_COLOR: {
                int k = 0;
                for (int c = 0; c < 6; ++c) if (light_palette[c].r == l->r && light_palette[c].g == l->g && light_palette[c].b == l->b) k = c + 1;
                l->r = light_palette[k % 6].r; l->g = light_palette[k % 6].g; l->b = light_palette[k % 6].b;
                break;
            }
            case LEDIT_SPOT: l->flags ^= LIGHT_SPOT; break;
            case LEDIT_SHADOWS: l->flags ^= LIGHT_NO_SHADOWS; break;
        }
        cw_comp_touch(&app.world, ids[i], LIGHT);
        ++changed;
    }
    if (changed) set_status(false, "edited %zu light%s", changed, changed == 1 ? "" : "s");
}
static void toggle_torches(void) {
    cw_entity_id *ids;
    size_t n = selected_ids(&ids), added = 0, removed = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!cw_world_is_valid(&app.world, ids[i]) || !cw_comp_has(&app.world, ids[i], UNIT)) continue;
        cw_entity_id torch = torch_of(ids[i]);
        if (torch != CW_INVALID_ENTITY) { cw_world_mark_destroy(&app.world, torch); ++removed; continue; }
        r2d_color c = r2d_color_mix(team_colors[((Unit *)cw_comp_ptr(&app.world, ids[i], UNIT))->team], white, 0.5f);
        spawn_torch(ids[i], c.r, c.g, c.b);
        ++added;
    }
    settle();
    if (added || removed) set_status(false, "torches: %zu lit, %zu put out", added, removed);
    else set_status(true, "select units to give them torches");
}
static void draw_light_inspector(Layout *l, cw_entity_id id, size_t n) {
    cw_world *w = &app.world;
    const Light *light = cw_comp_ptr(w, id, LIGHT);
    const Position *p = cw_comp_ptr(w, id, POSITION);
    char buffer[64];
    if (n == 1) snprintf(buffer, sizeof(buffer), "#%" PRId64 " light%s%s", id, (light->flags & LIGHT_SPOT) ? " (spot)" : "",
                         cw_comp_has(w, id, ATTACHED) ? " torch" : "");
    else snprintf(buffer, sizeof(buffer), "%zu selected (showing #%" PRId64 ")", n, id);
    r2d_ui_label_colored(r2d_v2(l->x, l->y), light_color(light), buffer);
    l->y += r2d_text_height() + 4;
    snprintf(buffer, sizeof(buffer), "at %.0f, %.0f  rgb %d %d %d", p ? (double)p->x : 0.0, p ? (double)p->y : 0.0, light->r, light->g, light->b);
    r2d_ui_label_dim(r2d_v2(l->x, l->y), buffer);
    l->y += r2d_text_height() + 4;
    bool minus, plus;
    snprintf(buffer, sizeof(buffer), "%.2f", (double)light->intensity);
    if (stepper(l, "intensity", buffer, &minus, &plus)) edit_lights(LEDIT_INTENSITY, plus ? 0.2f : -0.2f);
    snprintf(buffer, sizeof(buffer), "%.0f", (double)light->radius);
    if (stepper(l, "radius", buffer, &minus, &plus)) edit_lights(LEDIT_RADIUS, plus ? 20 : -20);
    snprintf(buffer, sizeof(buffer), "%.2f", (double)light->falloff);
    if (stepper(l, "falloff", buffer, &minus, &plus)) edit_lights(LEDIT_FALLOFF, plus ? 0.25f : -0.25f);
    snprintf(buffer, sizeof(buffer), "%.2f", (double)light->volumetric);
    if (stepper(l, "volumetric", buffer, &minus, &plus)) edit_lights(LEDIT_VOLUMETRIC, plus ? 0.2f : -0.2f);
    snprintf(buffer, sizeof(buffer), "%.2f", (double)light->flicker);
    if (stepper(l, "flicker", buffer, &minus, &plus)) edit_lights(LEDIT_FLICKER, plus ? 0.1f : -0.1f);
    if (light->flags & LIGHT_SPOT) {
        snprintf(buffer, sizeof(buffer), "%.0f deg", (double)(light->cone * 180.0f / PI_F));
        if (stepper(l, "cone", buffer, &minus, &plus)) edit_lights(LEDIT_CONE, plus ? 0.15f : -0.15f);
        snprintf(buffer, sizeof(buffer), "%.0f deg", (double)(light->direction * 180.0f / PI_F));
        if (stepper(l, "aim", buffer, &minus, &plus)) edit_lights(LEDIT_DIRECTION, plus ? 0.2f : -0.2f);
    }
    r2d_rect r = row_rect(l, 24);
    if (r2d_ui_button(split(r, 0, 3), "color")) edit_lights(LEDIT_COLOR, 0);
    if (r2d_ui_button_colored(split(r, 1, 3), "spot", (light->flags & LIGHT_SPOT) ? r2d_rgb(72, 120, 88) : r2d_rgb(58, 64, 76))) edit_lights(LEDIT_SPOT, 0);
    if (r2d_ui_button_colored(split(r, 2, 3), "shadows", (light->flags & LIGHT_NO_SHADOWS) ? r2d_rgb(58, 64, 76) : r2d_rgb(72, 120, 88))) edit_lights(LEDIT_SHADOWS, 0);
    r = row_rect(l, 24);
    if (r2d_ui_button_colored(r, "delete selected (Del)", r2d_rgb(120, 48, 44))) delete_selection();
}
static void draw_inspector(Layout *l) {
    cw_world *w = &app.world;
    size_t n = r2d_selection_count();
    char buffer[64];
    if (!n) {
        r2d_ui_label_dim(r2d_v2(l->x, l->y), "click a unit, drag a box,");
        l->y += r2d_text_height() + 2;
        r2d_ui_label_dim(r2d_v2(l->x, l->y), "shift-click to add");
        l->y += r2d_text_height() + 6;
        return;
    }
    cw_entity_id id = (cw_entity_id)r2d_selection(NULL)[0];
    if (cw_comp_has(w, id, LIGHT)) { draw_light_inspector(l, id, n); return; }
    if (cw_comp_has(w, id, OBSTACLE)) {
        snprintf(buffer, sizeof(buffer), "#%" PRId64 " pillar  radius %.0f", id, (double)((Obstacle *)cw_comp_ptr(w, id, OBSTACLE))->radius);
        r2d_ui_label(r2d_v2(l->x, l->y), buffer);
        l->y += r2d_text_height() + 6;
        r2d_rect rr = row_rect(l, 24);
        if (r2d_ui_button_colored(rr, "delete selected (Del)", r2d_rgb(120, 48, 44))) delete_selection();
        return;
    }
    const Unit *u = cw_comp_ptr(w, id, UNIT);
    const Health *h = cw_comp_ptr(w, id, HEALTH);
    const Attack *a = cw_comp_ptr(w, id, ATTACK);
    const Body *b = cw_comp_ptr(w, id, BODY);
    const Position *p = cw_comp_ptr(w, id, POSITION);
    if (!u || !h || !a || !b || !p) return;
    if (n == 1) snprintf(buffer, sizeof(buffer), "#%" PRId64 " %s %s", id, team_names[u->team], kinds[u->kind].name);
    else snprintf(buffer, sizeof(buffer), "%zu selected (showing #%" PRId64 ")", n, id);
    r2d_ui_label_colored(r2d_v2(l->x, l->y), team_colors[u->team], buffer);
    l->y += r2d_text_height() + 4;
    snprintf(buffer, sizeof(buffer), "at %.0f, %.0f  kills %" PRId32, (double)p->x, (double)p->y, a->kills);
    r2d_ui_label_dim(r2d_v2(l->x, l->y), buffer);
    l->y += r2d_text_height() + 4;
    r2d_rect bar = row_rect(l, 10);
    r2d_ui_bar(bar, h->maximum ? (float)h->current / (float)h->maximum : 0,
               r2d_color_mix(r2d_rgb(220, 60, 50), r2d_rgb(80, 210, 90), h->maximum ? (float)h->current / (float)h->maximum : 0));
    bool minus, plus;
    snprintf(buffer, sizeof(buffer), "%" PRId32 "/%" PRId32, h->current, h->maximum);
    if (stepper(l, "health", buffer, &minus, &plus)) edit_selection(EDIT_HEALTH, plus ? 10 : -10, "health changed");
    snprintf(buffer, sizeof(buffer), "%" PRId32, h->maximum);
    if (stepper(l, "max hp", buffer, &minus, &plus)) edit_selection(EDIT_MAX_HEALTH, plus ? 10 : -10, "max health changed");
    snprintf(buffer, sizeof(buffer), "%" PRId32, a->damage);
    if (stepper(l, "damage", buffer, &minus, &plus)) edit_selection(EDIT_DAMAGE, plus ? 1 : -1, "damage changed");
    snprintf(buffer, sizeof(buffer), "%.0f", (double)a->range);
    if (stepper(l, "range", buffer, &minus, &plus)) edit_selection(EDIT_RANGE, plus ? 10 : -10, "range changed");
    snprintf(buffer, sizeof(buffer), "%.0f", (double)b->speed);
    if (stepper(l, "speed", buffer, &minus, &plus)) edit_selection(EDIT_SPEED, plus ? 10 : -10, "speed changed");
    r2d_rect r = row_rect(l, 24);
    if (r2d_ui_button(split(r, 0, 3), "heal")) edit_selection(EDIT_HEAL, 0, "healed");
    if (r2d_ui_button(split(r, 1, 3), "team")) edit_selection(EDIT_SWAP_TEAM, 0, "team swapped");
    if (r2d_ui_button(split(r, 2, 3), "kind")) edit_selection(EDIT_KIND, 0, "kind changed");
    r = row_rect(l, 24);
    if (r2d_ui_button_colored(r, "delete selected (Del)", r2d_rgb(120, 48, 44))) delete_selection();
}
static void draw_panel(void) {
    r2d_set_space(R2D_SPACE_SCREEN);
    r2d_vec2 v = r2d_viewport();
    r2d_ui_panel(r2d_rect_make(12, 12, PANEL_W, v.y - STATUS_H - 24));
    Layout l = {22, 20, PANEL_W - 20};
    char buffer[64];

    heading(&l, "SIMULATION");
    r2d_rect r = row_rect(&l, 26);
    if (r2d_ui_button_colored(split(r, 0, 2), app.paused ? "resume" : "pause",
                              app.paused ? r2d_rgb(72, 120, 88) : r2d_rgb(58, 64, 76))) toggle_pause();
    if (r2d_ui_button(split(r, 1, 2), "step (N)")) single_step();
    r = row_rect(&l, 24);
    if (r2d_ui_button(split(r, 0, 3), "[ slow")) change_speed(-1);
    snprintf(buffer, sizeof(buffer), "x%.2g", (double)app.speed);
    r2d_ui_label_colored(r2d_v2(split(r, 1, 3).x + 18, r.y + 6), yellow, buffer);
    if (r2d_ui_button(split(r, 2, 3), "fast ]")) change_speed(1);

    heading(&l, "SPAWN");
    r = row_rect(&l, 24);
    if (r2d_ui_button_colored(split(r, 0, 4), "select", ui.tool == TOOL_SELECT ? r2d_rgb(90, 100, 120) : r2d_rgb(58, 64, 76))) ui.tool = TOOL_SELECT;
    if (r2d_ui_button_colored(split(r, 1, 4), "red", ui.tool == TOOL_PLACE_RED ? team_colors[TEAM_RED] : team_dark[TEAM_RED])) ui.tool = TOOL_PLACE_RED;
    if (r2d_ui_button_colored(split(r, 2, 4), "blue", ui.tool == TOOL_PLACE_BLUE ? team_colors[TEAM_BLUE] : team_dark[TEAM_BLUE])) ui.tool = TOOL_PLACE_BLUE;
    if (r2d_ui_button_colored(split(r, 3, 4), "lamp", ui.tool == TOOL_PLACE_LIGHT ? r2d_rgb(150, 130, 60) : r2d_rgb(90, 80, 50))) ui.tool = TOOL_PLACE_LIGHT;
    r = row_rect(&l, 24);
    r2d_ui_label(r2d_v2(r.x, r.y + 6), "kind");
    if (ui.tool == TOOL_PLACE_LIGHT) {
        if (r2d_ui_button(split(r, 1, 3), lamp_names[ui.place_kind % LAMP_KIND_COUNT])) ui.place_kind = (uint8_t)((ui.place_kind + 1) % LAMP_KIND_COUNT);
    } else if (r2d_ui_button(split(r, 1, 3), kinds[ui.place_kind % KIND_COUNT].name)) ui.place_kind = (uint8_t)((ui.place_kind + 1) % KIND_COUNT);
    if (r2d_ui_button(split(r, 2, 3), "wave")) act_spawn_wave(6);
    r = row_rect(&l, 24);
    if (r2d_ui_button(split(r, 0, 4), "level")) act_build_level();
    if (r2d_ui_button(split(r, 1, 4), "torch")) toggle_torches();
    if (r2d_ui_button(split(r, 2, 4), "units")) {   /* select every unit */
        r2d_select_clear();
        const cw_component_id required[] = {UNIT};
        cw_query q = cw_query_iter(&app.world, cw_mask(required, 1));
        cw_entity_id id;
        while (cw_query_next(&q, &id)) r2d_select_add((uint64_t)id);
    }
    if (r2d_ui_button_colored(split(r, 3, 4), "clear", r2d_rgb(120, 48, 44))) { act_clear(); r2d_select_clear(); }

    heading(&l, "SELECTION");
    draw_inspector(&l);

    heading(&l, "SNAPSHOT");
    r = row_rect(&l, 24);
    if (r2d_ui_button(split(r, 0, 2), "save (F5)")) save_with_fx();
    if (r2d_ui_button(split(r, 1, 2), "load (F9)")) load_with_fx();
    const char *name = strrchr(app.path, '/');
    name = name ? name + 1 : app.path;
    snprintf(buffer, sizeof(buffer), "file: %.28s", name);
    r2d_ui_label_dim(r2d_v2(l.x, l.y), buffer);
    l.y += r2d_text_height() + 2;
    if (app.have_saved) {
        snprintf(buffer, sizeof(buffer), "%" PRIu64 " entities, tick %" PRIu64 ", %" PRIu64 " B",
                 app.last_saved.entity_count, app.last_saved.tick, app.last_saved.size);
        r2d_ui_label_dim(r2d_v2(l.x, l.y), buffer);
    } else r2d_ui_label_dim(r2d_v2(l.x, l.y), "nothing saved yet");
    l.y += r2d_text_height() + 2;

    heading(&l, "VIEW");
    r = row_rect(&l, 24);
    if (r2d_ui_button(split(r, 0, 2), "fit (F)")) fit_arena();
    if (r2d_ui_button(split(r, 1, 2), "reset view")) r2d_camera_reset();
    r = row_rect(&l, 24);
    r2d_ui_toggle(split(r, 0, 3), "ranges", &ui.show_ranges);
    r2d_ui_toggle(split(r, 1, 3), "grid", &ui.show_grid);
    r2d_ui_toggle(split(r, 2, 3), "follow", &ui.follow_selection);
    r = row_rect(&l, 24);
    if (r2d_ui_toggle(split(r, 0, 3), "fx", &ui.show_fx) && !ui.show_fx) {
        r2d_particles_clear(ui.particles);
        r2d_effects_clear(ui.effects);
    }
    if (r2d_ui_button(split(r, 1, 3), "boom")) {   /* a demo burst at the arena center */
        Position c = {ARENA_W * 0.5f, ARENA_H * 0.5f};
        fx_death(NULL, c, (uint8_t)(r2d_frame_count() & 1), 22, true);
    }
    if (r2d_ui_button(split(r, 2, 3), "flash")) r2d_effects_flash(ui.effects, r2d_rgba(255, 255, 255, 140), 0.3f);
    if (r2d_post_params_ptr()) {
        r = row_rect(&l, 24);
        r2d_ui_toggle(split(r, 0, 3), "post (P)", &r2d_post_params_ptr()->post.enabled);
        r2d_ui_toggle(split(r, 1, 3), "lights", &ui.show_lights);
        r2d_ui_toggle(split(r, 2, 3), "tune F7", &ui.tuning_open);
        r = row_rect(&l, 24);
        int mode = r2d_post_params_ptr()->quality.tier;
        snprintf(buffer, sizeof(buffer), "tier: %s", mode == 0 ? "auto" : r2d_tier_name((r2d_tier)(mode - 1)));
        if (r2d_ui_button(split(r, 0, 2), buffer)) r2d_post_set_tier((mode + 1) % (R2D_TIER_COUNT + 1));
        snprintf(buffer, sizeof(buffer), "now %s", r2d_tier_name(r2d_post_current_tier()));
        r2d_ui_label_colored(r2d_v2(split(r, 1, 2).x + 6, r.y + 6), yellow, buffer);
    }
    snprintf(buffer, sizeof(buffer), "zoom %.2f  %.0fx%.0f @%.2g", (double)r2d_camera_ptr()->zoom, (double)v.x, (double)v.y, (double)r2d_dpi_scale());
    r2d_ui_label_dim(r2d_v2(l.x, l.y), buffer);
    l.y += r2d_text_height() + 2;
    r2d_stats stats = r2d_get_stats();
    snprintf(buffer, sizeof(buffer), "%.0f fps  %d verts  %zu picks", (double)ui.fps, stats.gl_vertices, stats.pickables);
    r2d_ui_label_dim(r2d_v2(l.x, l.y), buffer);
    l.y += r2d_text_height() + 2;
    snprintf(buffer, sizeof(buffer), "%zu particles  %zu emitters  %zu fx", r2d_particles_count(ui.particles),
             r2d_particles_emitter_count(ui.particles), r2d_effects_count(ui.effects));
    r2d_ui_label_dim(r2d_v2(l.x, l.y), buffer);
}
/* --- post-process tuning panel: generated from the tunables table --- */
static void draw_tuning_panel(void) {
    r2d_post_params *params = r2d_post_params_ptr();
    if (!ui.tuning_open || !params) return;
    r2d_set_space(R2D_SPACE_SCREEN);
    r2d_vec2 v = r2d_viewport();
    const float w = 350;
    r2d_rect panel = r2d_rect_make(v.x - w - 12, 12, w, v.y - STATUS_H - 24);
    r2d_ui_panel(panel);
    size_t count;
    const r2d_tunable *defs = r2d_post_tunable_defs(&count);
    r2d_tunable_set set = r2d_post_tunable_set(params);
    /* section tabs */
    const char *sections[16];
    int nsections = 0;
    for (size_t i = 0; i < count && nsections < 16; ++i) {
        bool seen = false;
        for (int k = 0; k < nsections; ++k) if (strcmp(sections[k], defs[i].section) == 0) seen = true;
        if (!seen) sections[nsections++] = defs[i].section;
    }
    if (ui.tuning_section >= nsections) ui.tuning_section = 0;
    float x = panel.x + 10, y = panel.y + 8;
    r2d_ui_label(r2d_v2(x, y), "POST-PROCESS TUNABLES (F7)");
    y += r2d_text_height() + 6;
    float tab_w = (w - 20 - 4 * 3) / 4, tab_h = 22;
    for (int i = 0; i < nsections; ++i) {
        r2d_rect r = r2d_rect_make(x + (float)(i % 4) * (tab_w + 4), y + (float)(i / 4) * (tab_h + 4), tab_w, tab_h);
        const char *name = sections[i];
        if (strcmp(name, "chromatic_aberration") == 0) name = "chroma";
        else if (strcmp(name, "motion_blur") == 0) name = "motion";
        else if (strcmp(name, "vignette") == 0) name = "vign";
        else if (strcmp(name, "tonemap") == 0) name = "tone";
        else if (strcmp(name, "lighting") == 0) name = "light";
        else if (strcmp(name, "tier_low") == 0) name = "t.low";
        else if (strcmp(name, "tier_medium") == 0) name = "t.med";
        else if (strcmp(name, "tier_high") == 0) name = "t.high";
        else if (strcmp(name, "tier_ultra") == 0) name = "t.ultra";
        if (r2d_ui_button_colored(r, name, i == ui.tuning_section ? r2d_rgb(90, 100, 120) : r2d_rgb(58, 64, 76))) ui.tuning_section = i;
    }
    y += (float)((nsections + 3) / 4) * (tab_h + 4) + 6;
    /* rows of the selected section */
    const char *section = sections[ui.tuning_section];
    char value[64];
    for (size_t i = 0; i < count; ++i) {
        const r2d_tunable *d = &defs[i];
        if (strcmp(d->section, section) != 0) continue;
        r2d_tunables_get(&set, d, value, sizeof(value));
        r2d_rect row = r2d_rect_make(x, y, w - 20, 22);
        if (d->type == R2D_TUNE_STRING) {
            r2d_ui_label(r2d_v2(row.x, row.y + 5), d->key);
            y += 22;
            r2d_ui_label_colored(r2d_v2(row.x, y + 2), yellow, value);
            y += r2d_text_height() + 8;
            continue;
        }
        r2d_ui_label(r2d_v2(row.x, row.y + 5), d->key);
        r2d_ui_label_colored(r2d_v2(row.x + 130, row.y + 5), d->type == R2D_TUNE_BOOL ? (value[0] == 't' ? r2d_rgb(120, 220, 140) : r2d_rgb(220, 120, 110)) : yellow, value);
        if (d->type == R2D_TUNE_BOOL) {
            if (r2d_ui_button(r2d_rect_make(row.x + row.w - 66, row.y, 66, row.h), "toggle")) r2d_tunables_adjust(&set, d, 1);
        } else {
            if (r2d_ui_button(r2d_rect_make(row.x + row.w - 58, row.y, 27, row.h), "-")) r2d_tunables_adjust(&set, d, -1);
            if (r2d_ui_button(r2d_rect_make(row.x + row.w - 27, row.y, 27, row.h), "+")) r2d_tunables_adjust(&set, d, 1);
        }
        y += 26;
    }
    y += 6;
    r2d_rect r = r2d_rect_make(x, y, w - 20, 24);
    if (r2d_ui_button(split(r, 0, 3), "reload (F6)")) {
        char error[256];
        if (r2d_post_load_config(ui.config_path, error, sizeof(error))) set_status(error[0], "reloaded %s%s%s", ui.config_path, error[0] ? ": " : "", error);
        else set_status(true, "cannot load %s: %s", ui.config_path, error);
    }
    if (r2d_ui_button(split(r, 1, 3), "save (F8)")) {
        if (r2d_post_save_config(ui.config_path)) set_status(false, "saved tunables to %s", ui.config_path);
        else set_status(true, "cannot write %s", ui.config_path);
    }
    if (r2d_ui_button(split(r, 2, 3), "defaults")) r2d_post_params_defaults(params);
    y += 30;
    r2d_post_stats ps = r2d_post_get_stats();
    char buffer[96];
    snprintf(buffer, sizeof(buffer), "%s  %d passes  %dx%d%s", ps.active ? "active" : "off", ps.passes_run, ps.width, ps.height, ps.hdr ? "  hdr" : "");
    r2d_ui_label_dim(r2d_v2(x, y), buffer);
    y += r2d_text_height() + 2;
    snprintf(buffer, sizeof(buffer), "motion %.0f,%.0f px  trauma %.2f", (double)ps.motion_px.x, (double)ps.motion_px.y, (double)ps.shake_trauma);
    r2d_ui_label_dim(r2d_v2(x, y), buffer);
    y += r2d_text_height() + 2;
    snprintf(buffer, sizeof(buffer), "tier %s%s  %.1f ms  %d changes  scene %dx%d", r2d_tier_name(ps.tier), ps.tier_automatic ? " (auto)" : "",
             (double)ps.frame_ms, ps.tier_changes, ps.width, ps.height);
    r2d_ui_label_dim(r2d_v2(x, y), buffer);
    y += r2d_text_height() + 2;
    r2d_lighting_stats ls = r2d_lighting_get_stats();
    snprintf(buffer, sizeof(buffer), "%d lights  %d casters  %d rays  %.1f ms  %dx%d", ls.lights, ls.occluders, ls.shadow_rays,
             ls.shadow_seconds * 1000.0, ls.lightmap_width, ls.lightmap_height);
    r2d_ui_label_dim(r2d_v2(x, y), buffer);
    y += r2d_text_height() + 2;
    snprintf(buffer, sizeof(buffer), "file: %.36s", ui.config_path);
    r2d_ui_label_dim(r2d_v2(x, y), buffer);
}
static void draw_status_bar(void) {
    r2d_set_space(R2D_SPACE_SCREEN);
    r2d_vec2 v = r2d_viewport();
    r2d_rect bar = r2d_rect_make(0, v.y - STATUS_H, v.x, STATUS_H);
    r2d_ui_panel(bar);
    char buffer[200];
    snprintf(buffer, sizeof(buffer), "tick %" PRIu64 "  %s  red %zu  blue %zu  kills %" PRIu64 ":%" PRIu64 "  damage %" PRIu64 "  entities %zu",
             app.world.tick, app.paused ? "PAUSED" : "running", count_team(TEAM_RED), count_team(TEAM_BLUE),
             app.hooks.kills[TEAM_RED], app.hooks.kills[TEAM_BLUE], app.hooks.damage_dealt, cw_world_entity_count(&app.world));
    r2d_post_stats ps = r2d_post_get_stats();
    if (ps.active) {
        size_t len = strlen(buffer);
        snprintf(buffer + len, sizeof(buffer) - len, "  post %d%s  tier %s%s %.1fms", ps.passes_run, ps.hdr ? " hdr" : "",
                 r2d_tier_name(ps.tier), ps.tier_automatic ? " (auto)" : "", (double)ps.frame_ms);
    }
    r2d_ui_label(r2d_v2(14, bar.y + 10), buffer);
    r2d_color c = app.status_error ? r2d_rgb(255, 120, 110) : r2d_rgb(150, 220, 160);
    r2d_ui_label_colored(r2d_v2(14, bar.y + 34), c, app.status);
    const char *hint = "Space pause  N step  [ ] speed  1/2/3 place  L torch  Q select  Del delete  F5/F9 save/load  wheel zoom  right-drag pan";
    float hint_w = r2d_text_width(hint);
    if (v.x - hint_w > 620) r2d_ui_label_dim(r2d_v2(v.x - hint_w - 14, bar.y + 34), hint);
}

static void handle_world_input(void) {
    r2d_selection_result sel;
    r2d_selection_update(&sel, ui.tool == TOOL_SELECT);
    if (sel.dragging) {
        cw_entity_id *ids;
        size_t n = selected_ids(&ids);
        act_move(ids, n, sel.drag_delta.x, sel.drag_delta.y);
    }
    if (sel.drag_ended) set_status(false, "moved %zu unit%s", r2d_selection_count(), r2d_selection_count() == 1 ? "" : "s");
    if (sel.clicked != R2D_ID_NONE && ui.show_fx && cw_comp_has(&app.world, (cw_entity_id)sel.clicked, BODY)) {
        float radius = ((Body *)cw_comp_ptr(&app.world, (cw_entity_id)sel.clicked, BODY))->radius;
        r2d_effects_ring(ui.effects, render_position((cw_entity_id)sel.clicked, cw_comp_ptr(&app.world, (cw_entity_id)sel.clicked, POSITION)),
                         radius + 2, radius + 16, 2, r2d_with_alpha(yellow, 200), 0.3f, R2D_EASE_OUT);
    }
    if (ui.tool != TOOL_SELECT) {
        ui.place_cooldown -= r2d_dt();
        if (r2d_mouse_pressed(R2D_MOUSE_LEFT)) ui.place_cooldown = 0;   /* a click always places */
        bool painting = r2d_mouse_pressed(R2D_MOUSE_LEFT) || r2d_mouse_down(R2D_MOUSE_LEFT);
        if (painting && !r2d_mouse_over_ui() && ui.place_cooldown <= 0) {
            r2d_vec2 m = r2d_mouse_world();
            if (r2d_rect_contains(arena_rect(), m) && ui.tool == TOOL_PLACE_LIGHT) {
                int kind = ui.place_kind % LAMP_KIND_COUNT;
                cw_entity_id id = spawn_light(lamp_kinds[kind], m.x, m.y);
                settle();
                r2d_select_set((uint64_t)id);
                ui.place_cooldown = 0.3f;
                set_status(false, "placed %s light #%" PRId64 " at %.0f, %.0f", lamp_names[kind], id, (double)m.x, (double)m.y);
            } else if (r2d_rect_contains(arena_rect(), m)) {
                uint8_t team = ui.tool == TOOL_PLACE_RED ? TEAM_RED : TEAM_BLUE;
                cw_entity_id id = spawn_unit(team, ui.place_kind % KIND_COUNT, m.x, m.y);
                settle();
                fx_spawn(m, team, kinds[ui.place_kind % KIND_COUNT].radius);
                ui.place_cooldown = 0.12f;
                set_status(false, "placed %s %s #%" PRId64 " at %.0f, %.0f", team_names[team], kinds[ui.place_kind % KIND_COUNT].name, id, (double)m.x, (double)m.y);
            }
        }
    }
}
static void handle_keys(void) {
    if (r2d_key_pressed(SAPP_KEYCODE_SPACE)) toggle_pause();
    if (r2d_key_pressed(SAPP_KEYCODE_N) || r2d_key_pressed(SAPP_KEYCODE_PERIOD)) single_step();
    if (r2d_key_pressed(SAPP_KEYCODE_LEFT_BRACKET)) change_speed(-1);
    if (r2d_key_pressed(SAPP_KEYCODE_RIGHT_BRACKET)) change_speed(1);
    if (r2d_key_pressed(SAPP_KEYCODE_Q)) {
        if (r2d_modifiers() & SAPP_MODIFIER_CTRL) sapp_request_quit(); else ui.tool = TOOL_SELECT;
    }
    if (r2d_key_pressed(SAPP_KEYCODE_1)) ui.tool = TOOL_PLACE_RED;
    if (r2d_key_pressed(SAPP_KEYCODE_2)) ui.tool = TOOL_PLACE_BLUE;
    if (r2d_key_pressed(SAPP_KEYCODE_3)) ui.tool = TOOL_PLACE_LIGHT;
    if (r2d_key_pressed(SAPP_KEYCODE_K)) ui.place_kind = (uint8_t)((ui.place_kind + 1) % (ui.tool == TOOL_PLACE_LIGHT ? LAMP_KIND_COUNT : KIND_COUNT));
    if (r2d_key_pressed(SAPP_KEYCODE_L)) toggle_torches();
    if (r2d_key_pressed(SAPP_KEYCODE_W)) act_spawn_wave(6);
    if (r2d_key_pressed(SAPP_KEYCODE_DELETE) || r2d_key_pressed(SAPP_KEYCODE_BACKSPACE) || r2d_key_pressed(SAPP_KEYCODE_X)) delete_selection();
    if (r2d_key_pressed(SAPP_KEYCODE_H)) edit_selection(EDIT_HEAL, 0, "healed");
    if (r2d_key_pressed(SAPP_KEYCODE_T)) edit_selection(EDIT_SWAP_TEAM, 0, "team swapped");
    if (r2d_key_pressed(SAPP_KEYCODE_F5)) save_with_fx();
    if (r2d_key_pressed(SAPP_KEYCODE_F9)) load_with_fx();
    if (r2d_key_pressed(SAPP_KEYCODE_F)) fit_arena();
    if (r2d_key_pressed(SAPP_KEYCODE_F11)) sapp_toggle_fullscreen();
    if (r2d_key_pressed(SAPP_KEYCODE_F6)) {
        char error[256];
        if (r2d_post_load_config(ui.config_path, error, sizeof(error))) set_status(error[0], "reloaded %s%s%s", ui.config_path, error[0] ? ": " : "", error);
        else set_status(true, "cannot load %s: %s", ui.config_path, error);
    }
    if (r2d_key_pressed(SAPP_KEYCODE_F7)) ui.tuning_open = !ui.tuning_open;
    if (r2d_key_pressed(SAPP_KEYCODE_F8)) {
        if (r2d_post_save_config(ui.config_path)) set_status(false, "saved tunables to %s", ui.config_path);
        else set_status(true, "cannot write %s", ui.config_path);
    }
    if (r2d_key_pressed(SAPP_KEYCODE_P) && r2d_post_params_ptr()) {
        r2d_post_params_ptr()->post.enabled = !r2d_post_params_ptr()->post.enabled;
        set_status(false, "post-process stack %s", r2d_post_params_ptr()->post.enabled ? "on" : "off");
    }
}

static void window_init(void) {
    r2d_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.camera = r2d_camera_make(r2d_v2(ARENA_W * 0.5f, ARENA_H * 0.5f), 0.8f, false);
    desc.camera.min_zoom = 0.1f;
    desc.camera.max_zoom = 12.0f;
    desc.camera_controls = true;
    desc.pan_with_right_button = desc.pan_with_middle_button = desc.pan_with_arrows = true;
    desc.escape_clears_selection = true;
    desc.draw_selection_box = true;
    desc.text_scale = 1.25f;
    desc.post_config = ui.config_path;
    r2d_setup(&desc);
    if (!r2d_valid()) { ui.failed = true; sapp_request_quit(); return; }
    if (r2d_post_params_ptr()) {
        if (r2d_post_config_path()[0]) set_status(false, "post-process tunables loaded from %s (F7 tunes, F6 reloads, F8 saves)", ui.config_path);
        else set_status(true, "no %s found: post-process defaults in use (F7 tunes, F8 writes the file)", ui.config_path);
    }
    ui.show_ranges = true;
    ui.show_grid = true;
    ui.show_fx = true;
    ui.show_lights = true;
    ui.particles = r2d_particles_create(16384, 512, 0x1234567u);
    ui.effects = r2d_effects_create(1024);
    if (!ui.particles || !ui.effects) { ui.failed = true; sapp_request_quit(); return; }
    app.hooks.listener = &ui;
    app.hooks.on_attack = fx_attack;
    app.hooks.on_damage = fx_damage;
    app.hooks.on_death = fx_death;
    app.hooks.on_visual_release = fx_visual_release;
    ui.ready = true;
    fit_arena();
    act_build_level();
    act_spawn_wave(ui.max_frames ? 4 : 8);
    set_status(false, "ready: %zu units on the field", cw_world_entity_count(&app.world));
}
static void window_frame(void) {
    if (!ui.ready) return;
    r2d_begin_frame();
    if (r2d_frame_count() == 1) fit_arena();   /* the first frame knows the real window size */
    float dt = r2d_dt();
    ui.fps = ui.fps * 0.95f + (dt > 0 ? 1.0f / dt : 0) * 0.05f;

    handle_keys();
    advance_simulation();
    spawn_queued_flashes();
    r2d_selection_prune(keep_alive, NULL);
    if (ui.follow_selection && r2d_selection_count()) {
        cw_entity_id id = (cw_entity_id)r2d_selection(NULL)[0];
        if (cw_comp_has(&app.world, id, POSITION))
            r2d_camera_ptr()->center = render_position(id, cw_comp_ptr(&app.world, id, POSITION));
    }
    if (r2d_post_reload_if_changed()) set_status(false, "%s changed on disk: tunables reloaded", ui.config_path);
    if (r2d_selection_count()) {   /* depth of field focuses on the selection */
        cw_entity_id id = (cw_entity_id)r2d_selection(NULL)[0];
        if (cw_comp_has(&app.world, id, POSITION))
            r2d_post_set_focus_world(render_position(id, cw_comp_ptr(&app.world, id, POSITION)));
    }
    r2d_effects_update(ui.effects, dt);
    r2d_particles_update(ui.particles, dt);
    r2d_effects_camera_push(ui.effects);       /* camera shake for the world pass */
    uint64_t hovered = r2d_hovered();
    draw_world(hovered);
    handle_world_input();
    r2d_effects_camera_pop(ui.effects);
    r2d_effects_draw(ui.effects, R2D_SPACE_SCREEN);
    draw_panel();
    draw_tuning_panel();
    draw_status_bar();
    r2d_end_frame();

    ++ui.frames;
    if (ui.max_frames && ui.frames >= ui.max_frames) sapp_request_quit();
}
static void window_event(const sapp_event *event) {
    if (!ui.ready) return;
    r2d_handle_event(event);
}
static void window_cleanup(void) {
    if (!ui.ready) return;
    app.hooks.on_attack = NULL;
    app.hooks.on_damage = NULL;
    app.hooks.on_death = NULL;
    app.hooks.on_visual_release = NULL;
    r2d_particles_destroy(ui.particles);
    r2d_effects_destroy(ui.effects);
    ui.particles = NULL;
    ui.effects = NULL;
    r2d_shutdown();
    ui.ready = false;
}
/* The window's sample count comes from the configured tier; that is decided
 * before the window exists, so the config is read once here on its own. */
static int msaa_from_config(void) {
    r2d_post_params p;
    r2d_post_params_defaults(&p);
    r2d_tunable_set set = r2d_post_tunable_set(&p);
    r2d_tunables_load(&set, ui.config_path, NULL, 0);
    r2d_tier tier = p.quality.tier > 0 ? (r2d_tier)(p.quality.tier - 1) : r2d_quality_pick_initial(true, WINDOW_W, WINDOW_H);
    if (tier >= R2D_TIER_COUNT) tier = R2D_TIER_ULTRA;
    int msaa = p.tiers[tier].msaa;
    return msaa == 1 || msaa == 2 || msaa == 4 || msaa == 8 ? msaa : 4;
}
static int run_window(void) {
    sapp_desc window;
    memset(&window, 0, sizeof(window));
    window.init_cb = window_init;
    window.frame_cb = window_frame;
    window.cleanup_cb = window_cleanup;
    window.event_cb = window_event;
    window.width = WINDOW_W;
    window.height = WINDOW_H;
    window.sample_count = msaa_from_config();
    window.high_dpi = true;
    window.window_title = "Clockwork combat - render2d/Sokol";
    window.logger.func = slog_func;
    sapp_run(&window);
    if (ui.max_frames && !ui.failed) printf("OK: rendered %u frames\n", ui.frames);
    return ui.failed ? 1 : 0;
}

int main(int argc, char **argv) {
    bool headless = false;
    const char *path = "clockwork_combat.cwsn";
    snprintf(ui.config_path, sizeof(ui.config_path), "tunable.cfg");
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--headless") == 0) headless = true;
        else if (strcmp(argv[i], "--config") == 0) {
            if (++i >= argc) { fputs("--config needs a path\n", stderr); return 2; }
            snprintf(ui.config_path, sizeof(ui.config_path), "%s", argv[i]);
        }
        else if (strcmp(argv[i], "--frames") == 0) {
            if (++i >= argc) { fputs("--frames needs a positive integer\n", stderr); return 2; }
            char *end;
            errno = 0;
            unsigned long frames = strtoul(argv[i], &end, 10);
            if (errno == ERANGE || argv[i][0] == '-' || *end || !frames || frames > UINT32_MAX) {
                fputs("--frames needs a positive integer\n", stderr);
                return 2;
            }
            ui.max_frames = (unsigned)frames;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("usage: %s [--headless] [--frames N] [--config tunable.cfg] [snapshot path]\n", argv[0]);
            return 0;
        } else path = argv[i];
    }
    app_init(path);
    if (!app.world_ready) return 1;
    int result = headless ? run_headless() : run_window();
    app_shutdown();
    return result;
}
