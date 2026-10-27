#include "clockwork.h"
#include <stdio.h>

typedef struct { float x, y; } Position;
typedef struct { float x, y; } Velocity;
enum { POSITION, VELOCITY, COMPONENT_COUNT };

static void movement(cw_world *world, cw_phase phase, float dt) {
    (void)phase;
    const cw_component_id types[] = { POSITION, VELOCITY };
    cw_query query = cw_query_iter(world, cw_mask(types, 2));
    cw_entity_id id;
    while (cw_query_next(&query, &id)) {
        Position *p = CW_GET(world, id, POSITION, Position);
        Velocity *v = CW_GET(world, id, VELOCITY, Velocity);
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
        !CW_REGISTER(&world, VELOCITY, Velocity)) return 1;

    cw_entity_id entity = cw_world_create_entity(&world);
    Position position = {0, 0};
    Velocity velocity = {3, 1};
    if (!cw_comp_set(&world, entity, POSITION, &position, sizeof(position)) ||
        !cw_comp_set(&world, entity, VELOCITY, &velocity, sizeof(velocity))) return 1;

    cw_system_desc system = {
        .name = "movement", .phases = CW_PHASE(CW_UPDATE),
        .fn = movement, .authority = CW_SHARED
    };
    if (!cw_sim_register(&sim, system)) return 1;
    cw_clock clock = cw_clock_make(CW_DEFAULT_TICK_HZ);
    for (int tick = 0; tick < 30; ++tick) cw_sim_tick(&sim, &world, clock.dt);

    Position *result = CW_GET(&world, entity, POSITION, Position);
    printf("After 30 ticks: (%.2f, %.2f)\n", (double)result->x, (double)result->y);
    cw_world_mark_destroy(&world, entity);
    cw_sim_entity_lifecycle(&sim, &world, 0);
    cw_sim_destroy(&sim);
    cw_world_destroy(&world);
    return 0;
}
