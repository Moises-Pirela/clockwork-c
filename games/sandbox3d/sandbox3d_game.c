/* Sandbox 3D: the first 3D game module.
 *
 * It exists to exercise the 3D path end to end: a project whose renderer
 * is 3d, a Position with three axes (CW_SEM_POSITION_X/Y/Z, y up), and
 * entities that show themselves through the engine's Shape3D component - a
 * sphere, a capsule, a cube or a plane drawn unlit by render3d - so the
 * module needs no drawing code at all. Two small systems animate them:
 * Spin turns a shape about its vertical axis, Bob moves it up and down.
 *
 * Prefab spawns receive the ground-plane coordinates in their (x, y)
 * arguments: x stays x and y becomes z, the way the 3D editor places
 * things where the mouse meets the ground.
 */
#include "cw_game.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TICK_HZ 60
#define GROUND 16.0f

typedef struct { float x, y, z; } Position;
typedef struct { float speed; } Spin;                       /* radians per second about y */
typedef struct { float amplitude, speed, base, phase; } Bob; /* y = base + amplitude * sin(phase) */

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
static bool register_systems(cw_sim *sim) {
    const cw_system_desc systems[] = {
        {"spin", CW_PHASE(CW_UPDATE), sys_spin, CW_SHARED, 0, false},
        {"bob", CW_PHASE(CW_UPDATE), sys_bob, CW_SHARED, 10, false},
    };
    for (size_t i = 0; i < sizeof(systems) / sizeof(systems[0]); ++i)
        if (!cw_sim_register(sim, systems[i])) return false;
    return true;
}

/* --- prefabs: (x, y) are ground coordinates (x, z) --- */
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
static bool spawn_plane(cw_world *w, cw_entity_id id, float x, float z) {
    cw_shape3d s = {CW_SHAPE_PLANE, 4, 0, 4, 0, 0, 0, 200, 190, 150, 255, 1, false, 0};
    return place(w, id, x, 0.01f, z, s);
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
static const cw_prefab prefabs[] = {
    {"sphere", "primitives", spawn_sphere},
    {"capsule", "primitives", spawn_capsule},
    {"cube", "primitives", spawn_cube},
    {"plane", "primitives", spawn_plane},
    {"spinning slab", "animated", spawn_spinner},
    {"bouncing ball", "animated", spawn_bouncer},
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
    else if (light != CW_INVALID_COMPONENT && cw_comp_has(w, id, light)) {
        const cw_light3d *l = cw_comp_ptr(w, id, light);
        what = l->kind < CW_LIGHT_KINDS ? cw_light3d_kind_names[l->kind] : "light";
    }
    const char *motion = cw_comp_has(w, id, SPIN) ? " (spinning)" : (cw_comp_has(w, id, BOB) ? " (bobbing)" : "");
    snprintf(out, capacity, "%s%s", what, motion);
}

static const cw_game_module module = {
    CW_GAME_ABI_VERSION, "sandbox3d", 1, TICK_HZ, {-GROUND / 2, -GROUND / 2, GROUND, GROUND},
    register_components, register_systems, NULL, NULL,
    prefabs, sizeof(prefabs) / sizeof(prefabs[0]),
    NULL,   /* nothing to draw by hand: the engine draws Shape3D */
    describe,
};
CW_GAME_API const cw_game_module *cw_game_export(void) { return &module; }
