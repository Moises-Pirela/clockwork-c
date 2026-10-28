/* r2d_particles: a particle system on top of render2d.
 *
 * A particle system owns a fixed pool of particles and a small table of
 * emitters. Particles are spawned either as one-shot bursts or by emitters
 * that run for a duration (or until stopped) at a given rate and can be
 * moved around (attached to a moving object) so the particles they emit
 * inherit its velocity. Every spawned particle takes its behaviour from an
 * r2d_particle_desc: lifetime, speed and direction cone, size and color over
 * life, gravity, drag, spin and shape.
 *
 * The simulation is independent of rendering: r2d_particles_update advances
 * it by dt (with its own seeded random generator, so it is deterministic),
 * r2d_particles_draw renders every live particle in the renderer's current
 * space (world or screen) with the lengths interpreted in that space. The
 * system never touches the renderer during update, so it can also be used
 * and tested without a window.
 *
 * Ownership: r2d_particles_create allocates the system; the caller releases
 * it with r2d_particles_destroy. Descs are copied when used. Emitter handles
 * are small values that become invalid once the emitter ends; using a stale
 * handle is harmless (ignored).
 */
#ifndef R2D_PARTICLES_H
#define R2D_PARTICLES_H

#include "render2d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    R2D_PARTICLE_CIRCLE,     /* filled disc */
    R2D_PARTICLE_SQUARE,     /* spinning square */
    R2D_PARTICLE_STREAK,     /* stretched along its velocity */
    R2D_PARTICLE_RING        /* hollow circle */
} r2d_particle_shape;

typedef struct { float min, max; } r2d_range;
static inline r2d_range r2d_range_make(float min, float max) { r2d_range r = {min, max}; return r; }
static inline r2d_range r2d_range_const(float v) { return r2d_range_make(v, v); }

typedef struct {
    r2d_particle_shape shape;
    r2d_range lifetime;          /* seconds */
    r2d_range speed;             /* units per second */
    float direction;             /* radians; 0 = +x, pi/2 = +y (y grows downwards unless the camera is y_up) */
    float spread;                /* radians; total cone width (2*pi emits in all directions) */
    r2d_range size;              /* radius (circle/ring), half side (square), thickness (streak) */
    float size_end_scale;        /* size multiplier at the end of life, 1 = constant */
    r2d_color color_start, color_end;   /* mixed over life (alpha included) */
    float fade_in;               /* fraction of life over which alpha ramps up from 0 */
    r2d_vec2 gravity;            /* units per second^2 */
    float drag;                  /* fraction of velocity lost per second, 0 = none */
    r2d_range spin;              /* radians per second (square) */
    float streak_length;         /* streak: length = speed * streak_length (seconds of travel) */
    float emit_radius;           /* spawn positions are jittered inside this radius */
    float inherit_velocity;      /* fraction of the emitter's velocity added to new particles */
    float rate;                  /* emitters: particles per second */
} r2d_particle_desc;
r2d_particle_desc r2d_particle_desc_default(void);

typedef struct r2d_particles r2d_particles;
typedef struct { uint32_t id; } r2d_emitter;
#define R2D_EMITTER_NONE ((r2d_emitter){0})

r2d_particles *r2d_particles_create(size_t max_particles, size_t max_emitters, uint32_t seed);
void r2d_particles_destroy(r2d_particles *ps);
void r2d_particles_clear(r2d_particles *ps);          /* drop every particle and emitter */
void r2d_particles_seed(r2d_particles *ps, uint32_t seed);

/* Spawn `count` particles at once (clipped to the free capacity). */
void r2d_particles_burst(r2d_particles *ps, const r2d_particle_desc *desc, r2d_vec2 position, int count);
/* Start an emitter; duration <= 0 runs until r2d_emitter_stop. */
r2d_emitter r2d_emitter_start(r2d_particles *ps, const r2d_particle_desc *desc, r2d_vec2 position, float duration);
bool r2d_emitter_alive(const r2d_particles *ps, r2d_emitter emitter);
void r2d_emitter_move(r2d_particles *ps, r2d_emitter emitter, r2d_vec2 position);  /* velocity is derived */
void r2d_emitter_set_rate(r2d_particles *ps, r2d_emitter emitter, float rate);
void r2d_emitter_set_direction(r2d_particles *ps, r2d_emitter emitter, float direction);
void r2d_emitter_stop(r2d_particles *ps, r2d_emitter emitter);         /* live particles finish normally */

void r2d_particles_update(r2d_particles *ps, float dt);
void r2d_particles_draw(const r2d_particles *ps);     /* in the current render2d space */

size_t r2d_particles_count(const r2d_particles *ps);
size_t r2d_particles_capacity(const r2d_particles *ps);
size_t r2d_particles_emitter_count(const r2d_particles *ps);

/* Read-only view of one live particle, for custom rendering or tests. */
typedef struct {
    r2d_vec2 position, velocity;
    float age, lifetime, size, rotation;
    r2d_color color;
    r2d_particle_shape shape;
} r2d_particle_view;
/* Iterate live particles: start with index 0, returns false when done. */
bool r2d_particles_next(const r2d_particles *ps, size_t *index, r2d_particle_view *out);

#ifdef __cplusplus
}
#endif
#endif
