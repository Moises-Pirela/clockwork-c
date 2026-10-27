#include "clockwork.h"
#include <stdio.h>

typedef struct { float x, y; } Position;
typedef struct { float x, y; } Velocity;
enum { POSITION, VELOCITY, FROZEN, COMPONENT_COUNT };

/* Cached query used by the movement system: entities with a position and
 * velocity that are not frozen. It declares that the system reads Velocity
 * and writes Position, which lets a scheduler detect conflicts with other
 * systems later. Owned by the world, released before the world is. */
static cw_cached_query *moving;

static void movement(cw_world *world, cw_phase phase, float dt) {
    (void)phase;
    (void)world;
    cw_query query = cw_query_begin(moving);
    cw_entity_id id;
    while (cw_query_next(&query, &id)) {
        /* Checked access: only the declared components can be touched. */
        Position *p = CW_QUERY_WRITE(moving, id, POSITION, Position);
        const Velocity *v = CW_QUERY_GET(moving, id, VELOCITY, Velocity);
        p->x += v->x * dt;
        p->y += v->y * dt;
    }
}

int main(void) {
    cw_world world;
    cw_sim sim;
    cw_world_init(&world, NULL);
    cw_sim_init(&sim, CW_HOST);
    if (cw_comp_block(&world, COMPONENT_COUNT) != 0 ||
        !CW_REGISTER(&world, POSITION, Position) ||
        !CW_REGISTER(&world, VELOCITY, Velocity) ||
        !cw_comp_register(&world, FROZEN, 0, 1, "Frozen", NULL)) return 1;
    const cw_component_id required[] = { POSITION, VELOCITY };
    const cw_component_id excluded[] = { FROZEN };
    const cw_component_id reads[] = { VELOCITY }, writes[] = { POSITION };
    moving = cw_query_create_access(&world,
        cw_filter_make(cw_mask(required, 2), cw_mask(NULL, 0), cw_mask(excluded, 1)),
        cw_access_make(cw_mask(reads, 1), cw_mask(writes, 1)));
    if (!moving) return 1;

    cw_entity_id entity = cw_world_create_entity(&world);
    cw_entity_id statue = cw_world_create_entity(&world);
    Position position = {0, 0};
    Velocity velocity = {3, 1};
    if (!cw_comp_set(&world, entity, POSITION, &position, sizeof(position)) ||
        !cw_comp_set(&world, entity, VELOCITY, &velocity, sizeof(velocity)) ||
        !cw_comp_set(&world, statue, POSITION, &position, sizeof(position)) ||
        !cw_comp_set(&world, statue, VELOCITY, &velocity, sizeof(velocity)) ||
        !cw_comp_add(&world, statue, FROZEN)) return 1;

    cw_system_desc system = {
        .name = "movement", .phases = CW_PHASE(CW_UPDATE),
        .fn = movement, .authority = CW_SHARED
    };
    if (!cw_sim_register(&sim, system)) return 1;
    cw_clock clock = cw_clock_make(CW_DEFAULT_TICK_HZ);
    for (int tick = 0; tick < 30; ++tick) cw_sim_tick(&sim, &world, clock.dt);

    Position *result = CW_GET(&world, entity, POSITION, Position);
    Position *still = CW_GET(&world, statue, POSITION, Position);
    printf("After 30 ticks: (%.2f, %.2f)\n", (double)result->x, (double)result->y);
    printf("Frozen statue: (%.2f, %.2f), %zu moving of %zu entities\n",
           (double)still->x, (double)still->y, cw_query_count(moving),
           cw_world_entity_count(&world));
    cw_world_mark_destroy(&world, entity);
    cw_sim_entity_lifecycle(&sim, &world, 0);
    cw_query_destroy(moving);
    cw_sim_destroy(&sim);
    cw_world_destroy(&world);
    return 0;
}
