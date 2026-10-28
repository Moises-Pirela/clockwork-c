#include "r2d_particles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R2D_P_PI 3.14159265358979f

typedef struct {
    r2d_vec2 position, velocity, gravity;
    float age, lifetime;
    float size, size_end, rotation, spin;
    float drag, fade_in, streak_length;
    r2d_color color_start, color_end;
    uint8_t shape;
} particle;

typedef struct {
    r2d_particle_desc desc;
    r2d_vec2 position, previous, velocity;
    float remaining;             /* seconds left when limited */
    float accumulator;           /* fractional particles to emit */
    uint32_t generation;
    bool active, moved, limited;
} emitter;

struct r2d_particles {
    particle *particles;
    size_t count, capacity;
    emitter *emitters;
    size_t emitter_capacity;
    uint32_t rng;
};

static uint32_t next_random(r2d_particles *ps) {
    ps->rng ^= ps->rng << 13;
    ps->rng ^= ps->rng >> 17;
    ps->rng ^= ps->rng << 5;
    return ps->rng;
}
static float random_unit(r2d_particles *ps) { return (float)(next_random(ps) & 0xffffff) / (float)0x1000000; }
static float random_range(r2d_particles *ps, r2d_range r) {
    float lo = r.min < r.max ? r.min : r.max, hi = r.min < r.max ? r.max : r.min;
    return lo + random_unit(ps) * (hi - lo);
}

r2d_particle_desc r2d_particle_desc_default(void) {
    r2d_particle_desc d;
    memset(&d, 0, sizeof(d));
    d.shape = R2D_PARTICLE_CIRCLE;
    d.lifetime = r2d_range_make(0.5f, 1.0f);
    d.speed = r2d_range_make(20.0f, 60.0f);
    d.direction = 0;
    d.spread = 2.0f * R2D_P_PI;
    d.size = r2d_range_make(2.0f, 4.0f);
    d.size_end_scale = 0.0f;
    d.color_start = r2d_rgba(255, 255, 255, 255);
    d.color_end = r2d_rgba(255, 255, 255, 0);
    d.streak_length = 0.05f;
    d.rate = 20.0f;
    return d;
}

r2d_particles *r2d_particles_create(size_t max_particles, size_t max_emitters, uint32_t seed) {
    if (!max_particles) max_particles = 1024;
    if (!max_emitters) max_emitters = 64;
    r2d_particles *ps = calloc(1, sizeof(*ps));
    if (!ps) return NULL;
    ps->particles = calloc(max_particles, sizeof(*ps->particles));
    ps->emitters = calloc(max_emitters, sizeof(*ps->emitters));
    if (!ps->particles || !ps->emitters) { r2d_particles_destroy(ps); return NULL; }
    ps->capacity = max_particles;
    ps->emitter_capacity = max_emitters;
    for (size_t i = 0; i < max_emitters; ++i) ps->emitters[i].generation = 1;
    r2d_particles_seed(ps, seed);
    return ps;
}
void r2d_particles_destroy(r2d_particles *ps) {
    if (!ps) return;
    free(ps->particles);
    free(ps->emitters);
    free(ps);
}
void r2d_particles_clear(r2d_particles *ps) {
    if (!ps) return;
    ps->count = 0;
    for (size_t i = 0; i < ps->emitter_capacity; ++i) {
        if (ps->emitters[i].active) ++ps->emitters[i].generation;
        ps->emitters[i].active = false;
    }
}
void r2d_particles_seed(r2d_particles *ps, uint32_t seed) { if (ps) ps->rng = seed ? seed : 0x9e3779b9u; }

static void spawn(r2d_particles *ps, const r2d_particle_desc *d, r2d_vec2 position, r2d_vec2 base_velocity) {
    if (ps->count >= ps->capacity) return;
    particle *p = &ps->particles[ps->count++];
    memset(p, 0, sizeof(*p));
    float angle = d->direction + (random_unit(ps) - 0.5f) * d->spread;
    float speed = random_range(ps, d->speed);
    p->position = position;
    if (d->emit_radius > 0) {
        float a = random_unit(ps) * 2.0f * R2D_P_PI, r = sqrtf(random_unit(ps)) * d->emit_radius;
        p->position = r2d_add(p->position, r2d_v2(cosf(a) * r, sinf(a) * r));
    }
    p->velocity = r2d_add(r2d_v2(cosf(angle) * speed, sinf(angle) * speed), r2d_scale(base_velocity, d->inherit_velocity));
    p->gravity = d->gravity;
    p->lifetime = random_range(ps, d->lifetime);
    if (p->lifetime <= 0) p->lifetime = 0.001f;
    p->size = random_range(ps, d->size);
    p->size_end = p->size * d->size_end_scale;
    p->rotation = random_unit(ps) * 2.0f * R2D_P_PI;
    p->spin = random_range(ps, d->spin);
    p->drag = d->drag;
    p->fade_in = d->fade_in;
    p->streak_length = d->streak_length;
    p->color_start = d->color_start;
    p->color_end = d->color_end;
    p->shape = (uint8_t)d->shape;
}

void r2d_particles_burst(r2d_particles *ps, const r2d_particle_desc *desc, r2d_vec2 position, int count) {
    if (!ps || !desc) return;
    for (int i = 0; i < count; ++i) spawn(ps, desc, position, r2d_v2(0, 0));
}

static emitter *lookup(r2d_particles *ps, r2d_emitter handle) {
    if (!ps || !handle.id) return NULL;
    size_t index = (handle.id & 0xffff) - 1;
    uint32_t generation = handle.id >> 16;
    if (index >= ps->emitter_capacity) return NULL;
    emitter *e = &ps->emitters[index];
    return e->active && e->generation == generation ? e : NULL;
}
r2d_emitter r2d_emitter_start(r2d_particles *ps, const r2d_particle_desc *desc, r2d_vec2 position, float duration) {
    r2d_emitter none = {0};
    if (!ps || !desc) return none;
    for (size_t i = 0; i < ps->emitter_capacity && i < 0xffff; ++i) {
        emitter *e = &ps->emitters[i];
        if (e->active) continue;
        e->active = true;
        e->moved = false;
        e->desc = *desc;
        e->position = e->previous = position;
        e->velocity = r2d_v2(0, 0);
        e->limited = duration > 0;
        e->remaining = duration > 0 ? duration : 0;
        e->accumulator = 0;
        r2d_emitter handle = {(uint32_t)(i + 1) | (e->generation << 16)};
        return handle;
    }
    return none;
}
bool r2d_emitter_alive(const r2d_particles *ps, r2d_emitter handle) { return lookup((r2d_particles *)ps, handle) != NULL; }
void r2d_emitter_move(r2d_particles *ps, r2d_emitter handle, r2d_vec2 position) {
    emitter *e = lookup(ps, handle);
    if (!e) return;
    e->position = position;
    e->moved = true;    /* velocity is measured in the next update */
}
void r2d_emitter_set_rate(r2d_particles *ps, r2d_emitter handle, float rate) {
    emitter *e = lookup(ps, handle);
    if (e) e->desc.rate = rate < 0 ? 0 : rate;
}
void r2d_emitter_set_direction(r2d_particles *ps, r2d_emitter handle, float direction) {
    emitter *e = lookup(ps, handle);
    if (e) e->desc.direction = direction;
}
void r2d_emitter_stop(r2d_particles *ps, r2d_emitter handle) {
    emitter *e = lookup(ps, handle);
    if (!e) return;
    e->active = false;
    ++e->generation;
    if (e->generation == 0) e->generation = 1;
}

void r2d_particles_update(r2d_particles *ps, float dt) {
    if (!ps || !(dt > 0)) return;
    /* emitters */
    for (size_t i = 0; i < ps->emitter_capacity; ++i) {
        emitter *e = &ps->emitters[i];
        if (!e->active) continue;
        /* velocity comes from the positions handed to r2d_emitter_move */
        e->velocity = e->moved ? r2d_scale(r2d_sub(e->position, e->previous), 1.0f / dt) : r2d_v2(0, 0);
        e->previous = e->position;
        float step = dt;
        if (e->limited) {
            if (e->remaining < step) step = e->remaining;
            e->remaining -= dt;
        }
        e->accumulator += e->desc.rate * step;
        int n = (int)e->accumulator;
        e->accumulator -= (float)n;
        for (int k = 0; k < n; ++k) {
            /* spread the spawns along the path travelled this frame */
            float t = n > 1 ? (float)k / (float)(n - 1) : 1.0f;
            r2d_vec2 at = r2d_sub(e->position, r2d_scale(e->velocity, dt * (1.0f - t)));
            spawn(ps, &e->desc, at, e->velocity);
        }
        if (e->limited && e->remaining <= 0) {
            e->active = false;
            ++e->generation;
            if (e->generation == 0) e->generation = 1;
        }
        e->moved = false;
    }
    /* particles */
    for (size_t i = 0; i < ps->count;) {
        particle *p = &ps->particles[i];
        p->age += dt;
        if (p->age >= p->lifetime) {
            ps->particles[i] = ps->particles[--ps->count];
            continue;
        }
        p->velocity = r2d_add(p->velocity, r2d_scale(p->gravity, dt));
        if (p->drag > 0) {
            float k = 1.0f - p->drag * dt;
            p->velocity = r2d_scale(p->velocity, k < 0 ? 0 : k);
        }
        p->position = r2d_add(p->position, r2d_scale(p->velocity, dt));
        p->rotation += p->spin * dt;
        ++i;
    }
}

static float life_t(const particle *p) { return p->lifetime > 0 ? p->age / p->lifetime : 1.0f; }
static r2d_color particle_color(const particle *p) {
    float t = life_t(p);
    r2d_color c = r2d_color_mix(p->color_start, p->color_end, t);
    if (p->fade_in > 0 && t < p->fade_in) c.a = (uint8_t)((float)c.a * (t / p->fade_in));
    return c;
}
static float particle_size(const particle *p) { return p->size + (p->size_end - p->size) * life_t(p); }

void r2d_particles_draw(const r2d_particles *ps) {
    if (!ps) return;
    for (size_t i = 0; i < ps->count; ++i) {
        const particle *p = &ps->particles[i];
        r2d_color c = particle_color(p);
        float s = particle_size(p);
        if (s <= 0 || c.a == 0) continue;
        switch (p->shape) {
            case R2D_PARTICLE_SQUARE: {
                r2d_vec2 dir = r2d_v2(cosf(p->rotation), sinf(p->rotation));
                r2d_draw_oriented_rect(p->position, dir, s * 2, s * 2, c);
                break;
            }
            case R2D_PARTICLE_STREAK: {
                float speed = r2d_length(p->velocity);
                float len = speed * p->streak_length;
                if (len < s) len = s;
                r2d_vec2 dir = speed > 0 ? r2d_scale(p->velocity, 1.0f / speed) : r2d_v2(1, 0);
                r2d_draw_line(r2d_sub(p->position, r2d_scale(dir, len * 0.5f)),
                              r2d_add(p->position, r2d_scale(dir, len * 0.5f)), s, c);
                break;
            }
            case R2D_PARTICLE_RING:
                r2d_draw_ring(p->position, s, s * 0.35f + 0.5f, c);
                break;
            default:
                r2d_draw_circle(p->position, s, c);
                break;
        }
    }
}

size_t r2d_particles_count(const r2d_particles *ps) { return ps ? ps->count : 0; }
size_t r2d_particles_capacity(const r2d_particles *ps) { return ps ? ps->capacity : 0; }
size_t r2d_particles_emitter_count(const r2d_particles *ps) {
    size_t n = 0;
    if (ps) for (size_t i = 0; i < ps->emitter_capacity; ++i) n += ps->emitters[i].active;
    return n;
}
bool r2d_particles_next(const r2d_particles *ps, size_t *index, r2d_particle_view *out) {
    if (!ps || !index || *index >= ps->count) return false;
    const particle *p = &ps->particles[*index];
    if (out) {
        out->position = p->position;
        out->velocity = p->velocity;
        out->age = p->age;
        out->lifetime = p->lifetime;
        out->size = particle_size(p);
        out->rotation = p->rotation;
        out->color = particle_color(p);
        out->shape = (r2d_particle_shape)p->shape;
    }
    ++*index;
    return true;
}
