/* A 3D starter game module, created by the launcher. Everything a game needs
 * lives in this file: components described with reflection (so the editor
 * can place and edit them and scenes can store them), systems and the
 * prefabs that appear in the editor's palette. Build it with the editor (F7)
 * or tools/build-game.sh.
 *
 * A 3D game needs no drawing code for its level: entities show themselves
 * through the engine's components - Shape3D (a sphere, capsule, cube or
 * plane with a material), Brush (Quake-style level blocks: boxes and wedges
 * with a material per face, clipped with the editor's clip tool, painted
 * with its paint tool) and Light3D (ambient, directional with shadows,
 * point, spot). The world is right-handed with y up, in metres; a position
 * carries CW_SEM_POSITION_X/Y/Z so the editor's gizmos, picking and the
 * ground-plane placement know where things are.
 *
 * Ideas to grow from here: add fields (they appear in the inspector and in
 * scenes automatically), add components and prefabs, add systems in other
 * phases (CW_EARLY_UPDATE, CW_LATE_UPDATE, CW_ENTITY_CREATED...), use
 * cw_commands for deferred changes, and separate authored data from runtime
 * state with CW_CONFIG_TYPE / CW_RUNTIME_TYPE (see games/combat). */
#include "cw_game.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TICK_HZ 60
#define GROUND 16.0f

/* --- components --- */
typedef struct { float x, y, z; } Position;
typedef struct { float speed; } Spin;                        /* turns the entity's Shape3D about y, radians per second */
typedef struct { float amplitude, speed, base, phase; } Bob;  /* y = base + amplitude * sin(phase) */

enum { POSITION, SPIN, BOB, COMPONENT_COUNT };

static const cw_field position_fields[] = {
    CW_FIELD_SEM(Position, x, CW_FIELD_F32, CW_SEM_POSITION_X),
    CW_FIELD_SEM(Position, y, CW_FIELD_F32, CW_SEM_POSITION_Y),
    CW_FIELD_SEM(Position, z, CW_FIELD_F32, CW_SEM_POSITION_Z),
};
static const cw_field spin_fields[] = { CW_FIELD_RANGE(Spin, speed, CW_FIELD_F32, -20, 20, 0.1f) };
static const cw_field bob_fields[] = {
    CW_FIELD_RANGE(Bob, amplitude, CW_FIELD_F32, 0, 100, 0.1f),
    CW_FIELD_RANGE(Bob, speed, CW_FIELD_F32, 0, 20, 0.1f),
    CW_FIELD_RANGE(Bob, base, CW_FIELD_F32, -1000, 1000, 0.1f),
    CW_FIELD_SEM(Bob, phase, CW_FIELD_F32, CW_SEM_RUNTIME),
};
static const Spin spin_defaults = {1.0f};
static const Bob bob_defaults = {0.5f, 2.0f, 1.0f, 0};
static const cw_type_info types[COMPONENT_COUNT] = {
    [POSITION] = CW_TYPE(Position, position_fields, NULL, NULL, 0),
    [SPIN] = CW_TYPE(Spin, spin_fields, &spin_defaults, NULL, 0),
    [BOB] = CW_TYPE(Bob, bob_fields, &bob_defaults, NULL, 0),
};
static bool register_components(cw_world *w) {
    if (cw_comp_block(w, COMPONENT_COUNT) != 0) return false;
    for (unsigned i = 0; i < COMPONENT_COUNT; ++i)
        if (!cw_reflect_register(w, (cw_component_id)i, &types[i])) return false;
    return true;
}
/* The engine registers Shape3D after the module's components; find it by name. */
static cw_component_id shape_component(cw_world *w) {
    cw_component_id ct = CW_INVALID_COMPONENT;
    cw_comp_by_name(w, "Shape3D", &ct);
    return ct;
}

/* --- systems --- */
static void sys_spin(cw_world *w, cw_phase phase, float dt) {
    (void)phase;
    cw_component_id shape = shape_component(w);
    if (shape == CW_INVALID_COMPONENT) return;
    const cw_component_id both[] = {SPIN, shape};
    cw_query q = cw_query_iter(w, cw_mask(both, 2));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        const Spin *s = cw_comp_ptr(w, id, SPIN);
        cw_shape3d *sh = cw_comp_ptr(w, id, shape);
        sh->ry = fmodf(sh->ry + s->speed * dt, 2 * 3.14159265f);
        cw_comp_touch(w, id, shape);
    }
}
static void sys_bob(cw_world *w, cw_phase phase, float dt) {
    (void)phase;
    const cw_component_id both[] = {POSITION, BOB};
    cw_query q = cw_query_iter(w, cw_mask(both, 2));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        Bob *b = cw_comp_ptr(w, id, BOB);
        Position *p = cw_comp_ptr(w, id, POSITION);
        b->phase = fmodf(b->phase + b->speed * dt, 2 * 3.14159265f);
        p->y = b->base + b->amplitude * sinf(b->phase);
        cw_comp_touch(w, id, POSITION);
    }
}
/* The interface: the scene's "faster" button (UiButton action 1) speeds every spinner up. */
enum { ACTION_FASTER = 1 };
static void sys_interface(cw_world *w, cw_phase phase, float dt) {
    (void)phase; (void)dt;
    if (!cw_game_ui_clicked(w, ACTION_FASTER)) return;
    const cw_component_id spinners[] = {SPIN};
    cw_query q = cw_query_iter(w, cw_mask(spinners, 1));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        Spin *s = cw_comp_ptr(w, id, SPIN);
        s->speed = s->speed * 1.5f;
        if (fabsf(s->speed) > 20) s->speed = s->speed > 0 ? 1 : -1;
        cw_comp_touch(w, id, SPIN);
    }
}
static bool register_systems(cw_sim *sim) {
    const cw_system_desc systems[] = {
        {"spin", CW_PHASE(CW_UPDATE), sys_spin, CW_SHARED, 0, false},
        {"bob", CW_PHASE(CW_UPDATE), sys_bob, CW_SHARED, 10, false},
        {"interface", CW_PHASE(CW_UPDATE), sys_interface, CW_SHARED, 20, false},
    };
    for (size_t i = 0; i < sizeof(systems) / sizeof(systems[0]); ++i)
        if (!cw_sim_register(sim, systems[i])) return false;
    return true;
}

/* --- prefabs: the editor passes the ground coordinates (x, z) in the (x, y) arguments --- */
static bool place(cw_world *w, cw_entity_id id, float x, float height, float z, cw_shape3d shape) {
    cw_component_id ct = shape_component(w);
    Position p = {x, height, z};
    return ct != CW_INVALID_COMPONENT && cw_comp_set(w, id, POSITION, &p, sizeof(p)) && cw_comp_set(w, id, ct, &shape, sizeof(shape));
}
static bool spawn_sphere(cw_world *w, cw_entity_id id, float x, float z) {
    cw_shape3d s = {CW_SHAPE_SPHERE, 0.5f, 0.5f, 0.5f, 0, 0, 0, 235, 120, 90, 255, 0, false, 0};
    return place(w, id, x, 0.5f, z, s);
}
static bool spawn_capsule(cw_world *w, cw_entity_id id, float x, float z) {
    cw_shape3d s = {CW_SHAPE_CAPSULE, 0.4f, 1.0f, 0.4f, 0, 0, 0, 120, 200, 140, 255, 0, false, 0};
    return place(w, id, x, 0.9f, z, s);
}
static bool spawn_cube(cw_world *w, cw_entity_id id, float x, float z) {
    cw_shape3d s = {CW_SHAPE_CUBE, 1, 1, 1, 0, 0, 0, 110, 150, 235, 255, 0, false, 0};
    return place(w, id, x, 0.5f, z, s);
}
static bool spawn_spinner(cw_world *w, cw_entity_id id, float x, float z) {
    cw_shape3d s = {CW_SHAPE_CUBE, 1.2f, 0.4f, 1.2f, 0, 0, 0, 240, 200, 90, 255, 0, false, 0};
    Spin spin = {1.2f};
    return place(w, id, x, 0.2f, z, s) && cw_comp_set(w, id, SPIN, &spin, sizeof(spin));
}
static bool spawn_bouncer(cw_world *w, cw_entity_id id, float x, float z) {
    cw_shape3d s = {CW_SHAPE_SPHERE, 0.4f, 0.4f, 0.4f, 0, 0, 0, 230, 110, 200, 255, 0, false, 0};
    Bob bob = {0.6f, 2.5f, 1.0f, 0};
    return place(w, id, x, 1.0f, z, s) && cw_comp_set(w, id, BOB, &bob, sizeof(bob));
}
static bool spawn_lamp(cw_world *w, cw_entity_id id, float x, float z) {
    cw_component_id light = CW_INVALID_COMPONENT;
    cw_comp_by_name(w, "Light3D", &light);
    cw_shape3d s = {CW_SHAPE_SPHERE, 0.2f, 0.2f, 0.2f, 0, 0, 0, 255, 230, 160, 255, 0, false, 0};
    if (light == CW_INVALID_COMPONENT || !place(w, id, x, 2.2f, z, s)) return false;
    cw_light3d l;
    cw_type_default(cw_comp_type(w, light), &l);   /* a point light */
    l.intensity = 1.5f;
    l.range = 8;
    return cw_comp_set(w, id, light, &l, sizeof(l));
}
static const cw_prefab prefabs[] = {
    {"sphere", "primitives", spawn_sphere},
    {"capsule", "primitives", spawn_capsule},
    {"cube", "primitives", spawn_cube},
    {"spinning slab", "animated", spawn_spinner},
    {"bouncing ball", "animated", spawn_bouncer},
    {"lamp", "lights", spawn_lamp},
};

static void describe(cw_world *w, cw_entity_id id, char *out, size_t capacity) {
    cw_component_id shape = shape_component(w), brush = CW_INVALID_COMPONENT, light = CW_INVALID_COMPONENT;
    cw_comp_by_name(w, "Brush", &brush);
    cw_comp_by_name(w, "Light3D", &light);
    const char *what = "entity";
    if (shape != CW_INVALID_COMPONENT && cw_comp_has(w, id, shape)) {
        const cw_shape3d *s = cw_comp_ptr(w, id, shape);
        what = s->kind < CW_SHAPE_KINDS ? cw_shape3d_kind_names[s->kind] : "shape";
    } else if (brush != CW_INVALID_COMPONENT && cw_comp_has(w, id, brush)) what = "brush";
    else if (light != CW_INVALID_COMPONENT && cw_comp_has(w, id, light)) what = "light";
    const char *motion = cw_comp_has(w, id, SPIN) ? " (spinning)" : (cw_comp_has(w, id, BOB) ? " (bobbing)" : "");
    snprintf(out, capacity, "%s%s", what, motion);
}

static const cw_game_module module = {
    CW_GAME_ABI_VERSION, "starter", 1, TICK_HZ, {-GROUND / 2, -GROUND / 2, GROUND, GROUND},
    register_components, register_systems, NULL, NULL,
    prefabs, sizeof(prefabs) / sizeof(prefabs[0]),
    NULL,   /* nothing to draw by hand: the engine draws Shape3D, Brush and Light3D */
    describe,
};
CW_GAME_API const cw_game_module *cw_game_export(void) { return &module; }
