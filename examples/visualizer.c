/* Terminal visualizer for Clockwork worlds.
 *
 * Runs a small simulation (spawning, moving and expiring entities) and draws
 * an entity density map with live statistics: entity counts, deletions,
 * memory usage and frame/tick rates. Every frame also cross-checks the cached
 * queries against the world, so the tool doubles as a verification harness.
 *
 * Interactive (stdout is a terminal):
 *   q quit   space pause   s step one tick   +/- spawn rate   d destroy half
 *   r reset world
 * Headless (stdout is not a terminal, or --frames N): runs N frames, prints
 * the final panel and exits non-zero if any verification check failed.
 *
 * Options: --frames N  --spawn N  --max N  --hz N  --fps N  --seed N
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "clockwork.h"
#include <fcntl.h>
#include <inttypes.h>
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
#include <malloc.h>
#define HAVE_MALLINFO2 1
#endif
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

typedef struct { float x, y; } Position;
typedef struct { float x, y; } Velocity;
typedef struct { float remaining; } Lifetime;
enum { POSITION, VELOCITY, LIFETIME, COMPONENT_COUNT };

#define WORLD_W 128.0f
#define WORLD_H 64.0f
#define MAX_COLS 160
#define MAX_ROWS 48

typedef struct {
    cw_cached_query *all, *moving, *mortal;
    cw_observer *position_changes;       /* this consumer's cursor on Position */
    size_t commands_recorded;            /* by the spawn system this tick */
    size_t moved_this_tick, spawned_this_tick;
    size_t added_rate, modified_rate, removed_rate;   /* last observed tick */
    uint32_t rng;
    int spawn_per_tick;
    size_t max_entities;
    uint64_t created_total, destroyed_total, last_sweep;
    /* per-second rates */
    uint64_t created_window, destroyed_window, frames_window, ticks_window;
    double window_start, created_rate, destroyed_rate, fps, tps;
    double tick_time_accum, tick_ms;
    /* verification */
    uint64_t checks, violations;
    char last_violation[128];
    bool heap_checked;          /* allocation-free walk verified by mallinfo2 */
} sim_state;

static double now_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
static uint32_t next_random(sim_state *s) {
    s->rng ^= s->rng << 13;
    s->rng ^= s->rng >> 17;
    s->rng ^= s->rng << 5;
    return s->rng;
}
static float random_unit(sim_state *s) {
    return (float)(next_random(s) & 0xffffff) / (float)0x1000000;
}
static size_t process_rss_bytes(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long size = 0, resident = 0;
    int n = fscanf(f, "%lu %lu", &size, &resident);
    fclose(f);
    long page = sysconf(_SC_PAGESIZE);
    return n == 2 && page > 0 ? (size_t)resident * (size_t)page : 0;
}

/* ---- systems ------------------------------------------------------------ */
static void spawn_system(cw_world *w, cw_phase phase, float dt) {
    (void)phase; (void)dt;
    sim_state *s = w->user;
    /* Spawning goes through the world's command buffer: the entities exist
     * once the scheduler flushes at the end of this phase. */
    cw_commands *commands = cw_world_commands(w);
    s->spawned_this_tick = 0;
    s->last_sweep = 0;   /* the destroyed phase only runs when something is pending */
    size_t budget = cw_query_count(s->all) < s->max_entities ? s->max_entities - cw_query_count(s->all) : 0;
    for (int i = 0; i < s->spawn_per_tick && (size_t)i < budget; ++i) {
        cw_entity_id id = cw_commands_create(commands);
        ++s->spawned_this_tick;
        Position p = {random_unit(s) * WORLD_W, random_unit(s) * WORLD_H};
        Velocity v = {(random_unit(s) - 0.5f) * 40.0f, (random_unit(s) - 0.5f) * 20.0f};
        Lifetime l = {1.0f + random_unit(s) * 6.0f};
        CW_COMMANDS_SET(commands, id, POSITION, &p);
        CW_COMMANDS_SET(commands, id, VELOCITY, &v);
        CW_COMMANDS_SET(commands, id, LIFETIME, &l);
    }
    s->commands_recorded = cw_commands_count(commands);
}
/* A cached query must only yield live entities that carry the filtered
 * components, so a NULL here is a cache bug worth reporting, not a crash. */
static void stale_entity(sim_state *s, const char *system, cw_entity_id id) {
    ++s->violations;
    snprintf(s->last_violation, sizeof(s->last_violation),
             "%s: query yielded entity %" PRId64 " without its components", system, id);
}
static size_t heap_in_use(void) {
#ifdef HAVE_MALLINFO2
    return mallinfo2().uordblks;
#else
    return 0;
#endif
}
static void movement_system(cw_world *w, cw_phase phase, float dt) {
    (void)phase;
    sim_state *s = w->user;
    size_t heap_before = heap_in_use();
    cw_query it = cw_query_begin(s->moving);
    cw_entity_id id;
    s->moved_this_tick = 0;
    while (cw_query_next(&it, &id)) {
        /* Checked, allocation-free access: the query declared it writes
         * Position and reads Velocity, so anything else would return NULL. */
        Position *p = CW_QUERY_WRITE(s->moving, id, POSITION, Position);
        const Velocity *v = CW_QUERY_GET(s->moving, id, VELOCITY, Velocity);
        if (!p || !v) { stale_entity(s, "movement", id); continue; }
        ++s->moved_this_tick;
        p->x += v->x * dt;
        p->y += v->y * dt;
        /* bounces flip the velocity, which this query also declared writable */
        Velocity *vel = CW_QUERY_WRITE(s->moving, id, VELOCITY, Velocity);
        if (p->x < 0) { p->x = -p->x; vel->x = -vel->x; }
        if (p->y < 0) { p->y = -p->y; vel->y = -vel->y; }
        if (p->x >= WORLD_W) { p->x = 2 * WORLD_W - p->x - 0.001f; vel->x = -vel->x; }
        if (p->y >= WORLD_H) { p->y = 2 * WORLD_H - p->y - 0.001f; vel->y = -vel->y; }
    }
#ifdef HAVE_MALLINFO2
    if (heap_in_use() != heap_before) {
        ++s->violations;
        snprintf(s->last_violation, sizeof(s->last_violation), "movement walk allocated on the heap");
    }
    s->heap_checked = true;
#else
    (void)heap_before;
#endif
}
static void lifetime_system(cw_world *w, cw_phase phase, float dt) {
    (void)phase;
    sim_state *s = w->user;
    cw_query it = cw_query_begin(s->mortal);
    cw_entity_id id;
    while (cw_query_next(&it, &id)) {
        Lifetime *l = CW_QUERY_WRITE(s->mortal, id, LIFETIME, Lifetime);
        if (!l) { stale_entity(s, "lifetime", id); continue; }
        l->remaining -= dt;
        /* Deferred destruction: recorded during the walk, applied at the phase flush. */
        if (l->remaining <= 0) cw_commands_destroy_entity(cw_world_commands(w), id);
    }
}
static void lifecycle_system(cw_world *w, cw_phase phase, float dt) {
    (void)dt;
    sim_state *s = w->user;
    if (phase == CW_ENTITY_DESTROYED) {
        s->last_sweep = cw_world_pending_destroy(w).count;
        s->destroyed_total += s->last_sweep;
        s->destroyed_window += s->last_sweep;
    } else if (phase == CW_ENTITY_CREATED) {
        s->created_total += cw_world_created(w).count;
        s->created_window += cw_world_created(w).count;
    }
}

/* ---- verification ------------------------------------------------------- */
static void fail_check(sim_state *s, const char *what, size_t expected, size_t actual) {
    ++s->violations;
    snprintf(s->last_violation, sizeof(s->last_violation),
             "%s: expected %zu, got %zu", what, expected, actual);
}
/* Once per tick: observe this tick's Position changes. Spawns add, movement
 * modifies (additions count as modifications too) and the sweep removes. */
static void observe_changes(cw_world *w, sim_state *s) {
    cw_observer_observe(s->position_changes);
    s->added_rate = cw_observer_count(s->position_changes, CW_CHANGE_ADDED);
    s->modified_rate = cw_observer_count(s->position_changes, CW_CHANGE_MODIFIED);
    s->removed_rate = cw_observer_count(s->position_changes, CW_CHANGE_REMOVED);
    if (s->added_rate != s->spawned_this_tick)
        fail_check(s, "observed Position additions vs spawns", s->spawned_this_tick, s->added_rate);
    if (s->modified_rate != s->moved_this_tick)
        fail_check(s, "observed Position modifications vs moved", s->moved_this_tick, s->modified_rate);
    if (s->removed_rate != s->last_sweep)
        fail_check(s, "observed Position removals vs sweep", s->last_sweep, s->removed_rate);
    cw_query changed = cw_observer_iter(s->position_changes, CW_CHANGE_MODIFIED);
    cw_entity_id id;
    size_t readable = 0;
    while (cw_query_next(&changed, &id)) {
        if (!CW_GET(w, id, POSITION, Position)) fail_check(s, "modified walk yielded unreadable id", 1, 0);
        ++readable;
    }
    if (readable != cw_query_count(s->all))
        fail_check(s, "readable modified Positions vs visible entities", cw_query_count(s->all), readable);
}
static void verify(cw_world *w, sim_state *s, bool deep) {
    ++s->checks;
    size_t active = cw_world_entity_count(w), pending = cw_world_pending_destroy(w).count;
    cw_flush_result flush = cw_commands_last_flush(cw_world_commands(w));
    if (flush.failed) fail_check(s, "deferred commands failed to apply", 0, flush.failed);
    if (!cw_commands_is_empty(cw_world_commands(w)))
        fail_check(s, "command buffer not flushed after the tick", 0, cw_commands_count(cw_world_commands(w)));
    if (cw_query_count(s->all) != active - pending)
        fail_check(s, "all-entities cache vs entity table", active - pending, cw_query_count(s->all));
    if (cw_query_count(s->moving) != cw_query_count(s->all))
        fail_check(s, "moving cache vs all cache", cw_query_count(s->all), cw_query_count(s->moving));
    if (!deep) return;
    cw_cached_query *queries[] = {s->all, s->moving, s->mortal};
    for (size_t qi = 0; qi < 3; ++qi) {
        size_t adhoc = cw_query_collect_filter(w, cw_query_filter(queries[qi]), NULL, 0);
        if (adhoc != cw_query_count(queries[qi]))
            fail_check(s, "cached count vs ad-hoc scan", adhoc, cw_query_count(queries[qi]));
        cw_query it = cw_query_begin(queries[qi]);
        cw_entity_id id;
        size_t walked = 0;
        while (cw_query_next(&it, &id)) {
            ++walked;
            if (!cw_filter_matches(cw_query_filter(queries[qi]), w->entities[id]))
                fail_check(s, "cached entity does not match filter", 1, 0);
        }
        if (walked != cw_query_count(queries[qi]))
            fail_check(s, "cached walk vs cached count", cw_query_count(queries[qi]), walked);
    }
}

/* ---- terminal ----------------------------------------------------------- */
static struct termios saved_termios;
static bool termios_saved, interactive;
static volatile sig_atomic_t stop_requested;
static void restore_terminal(void) {
    if (!interactive) return;
    fputs("\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);
    if (termios_saved) tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
}
static void on_signal(int sig) { (void)sig; stop_requested = 1; }
static void setup_terminal(void) {
    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &saved_termios) == 0) {
        termios_saved = true;
        raw = saved_termios;
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags >= 0) fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    fputs("\x1b[?1049h\x1b[?25l", stdout);
    atexit(restore_terminal);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
}
static void terminal_size(int *cols, int *rows) {
    struct winsize ws;
    *cols = 80;
    *rows = 24;
    if (interactive && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    }
}
static int read_key(void) {
    unsigned char c;
    return interactive && read(STDIN_FILENO, &c, 1) == 1 ? c : -1;
}

/* ---- rendering ---------------------------------------------------------- */
static const char *format_bytes(size_t bytes, char *buf, size_t size) {
    if (bytes >= (size_t)1 << 30) snprintf(buf, size, "%.2f GB", (double)bytes / (double)((size_t)1 << 30));
    else if (bytes >= (size_t)1 << 20) snprintf(buf, size, "%.2f MB", (double)bytes / (double)((size_t)1 << 20));
    else if (bytes >= 1024) snprintf(buf, size, "%.1f KB", (double)bytes / 1024.0);
    else snprintf(buf, size, "%zu B", bytes);
    return buf;
}
static void render(cw_world *w, sim_state *s, cw_clock *clock, bool paused) {
    static unsigned counts[MAX_ROWS][MAX_COLS];
    static char line[MAX_COLS + 64];
    int term_cols, term_rows;
    terminal_size(&term_cols, &term_rows);
    int cols = term_cols - 2, rows = term_rows - 15;
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    if (cols < 16) cols = 16;
    if (rows < 4) rows = 4;
    memset(counts, 0, sizeof(counts));
    unsigned peak = 1;
    cw_query it = cw_query_begin(s->moving);
    cw_entity_id id;
    while (cw_query_next(&it, &id)) {
        const Position *p = CW_GET(w, id, POSITION, Position);
        if (!p) continue;
        int cx = (int)(p->x / WORLD_W * (float)cols), cy = (int)(p->y / WORLD_H * (float)rows);
        if (cx < 0) cx = 0; else if (cx >= cols) cx = cols - 1;
        if (cy < 0) cy = 0; else if (cy >= rows) cy = rows - 1;
        if (++counts[cy][cx] > peak) peak = counts[cy][cx];
    }
    if (interactive) fputs("\x1b[H", stdout);
    printf("Clockwork visualizer  tick %" PRIu64 "%s  [q] quit [space] pause [s] step [+/-] spawn [d] destroy half [r] reset\n",
           w->tick, paused ? "  PAUSED" : "");
    line[0] = '+';
    memset(line + 1, '-', (size_t)cols);
    line[cols + 1] = '+';
    line[cols + 2] = '\0';
    puts(line);
    static const char ramp[] = " .:-=+*#%@";
    for (int y = 0; y < rows; ++y) {
        line[0] = '|';
        for (int x = 0; x < cols; ++x) {
            unsigned c = counts[y][x];
            size_t level = c ? 1 + (size_t)((double)(c - 1) / (double)peak * 8.0) : 0;
            if (level > 9) level = 9;
            line[x + 1] = ramp[level];
        }
        line[cols + 1] = '|';
        line[cols + 2] = '\0';
        puts(line);
    }
    line[0] = '+';
    memset(line + 1, '-', (size_t)cols);
    line[cols + 1] = '+';
    puts(line);

    cw_memory_stats m = cw_world_memory(w);
    char b0[32], b1[32], b2[32], b3[32], b4[32], b5[32], b6[32];
    size_t active = cw_world_entity_count(w), pending = cw_world_pending_destroy(w).count;
    const char *eol = interactive ? "\x1b[K" : "";
    printf("entities   %zu alive, %zu visible (cached), %zu pending destroy, capacity %zu, spawn %d/tick, cap %zu%s\n",
           active, cw_query_count(s->all), pending, w->capacity, s->spawn_per_tick, s->max_entities, eol);
    printf("deletions  %" PRIu64 " total, %" PRIu64 " in last sweep, %.0f/s   |   created %" PRIu64 " total, %.0f/s%s\n",
           s->destroyed_total, s->last_sweep, s->destroyed_rate, s->created_total, s->created_rate, eol);
    printf("memory     world %s = entities %s + pools %s (reserved %s) + bitsets %s + queries %s + lists %s%s\n",
           format_bytes(m.total, b0, sizeof(b0)), format_bytes(m.entities, b1, sizeof(b1)),
           format_bytes(m.pool_committed, b2, sizeof(b2)), format_bytes(m.pool_reserved, b3, sizeof(b3)),
           format_bytes(m.bitsets, b4, sizeof(b4)), format_bytes(m.queries, b5, sizeof(b5)),
           format_bytes(m.lists, b6, sizeof(b6)), eol);
    size_t rss = process_rss_bytes();
    printf("           process RSS %s, %zu cached queries (%zu caches), %zu observers (%s)%s\n",
           rss ? format_bytes(rss, b0, sizeof(b0)) : "n/a", w->queries.handle_count, w->queries.cache_count,
           w->observers.count, format_bytes(m.observers, b1, sizeof(b1)), eol);
    printf("changes    Position per tick: %zu added, %zu modified, %zu removed (observation #%" PRIu64 ")%s\n",
           s->added_rate, s->modified_rate, s->removed_rate, cw_observer_observations(s->position_changes), eol);
    cw_flush_result flush = cw_commands_last_flush(cw_world_commands(w));
    printf("commands   spawn recorded %zu this tick (4 per entity); last flush applied %zu, failed %zu; buffer %s%s\n",
           s->commands_recorded, flush.applied, flush.failed,
           format_bytes(m.commands, b2, sizeof(b2)), eol);
    cw_signature conflict = cw_observer_conflicts(s->position_changes, s->moving);
    printf("conflicts  observer(Position) vs movement query: %s   |   lifetime vs movement: %s   |   walk %s%s\n",
           cw_sig_has(conflict, POSITION) ? "Position (write vs observe)" : "none",
           cw_sig_empty(cw_query_conflicts(s->mortal, s->moving)) ? "none" : "yes",
           s->heap_checked ? "allocation-free" : "heap not checked", eol);
    printf("fps        %.1f (%.2f ms/frame)   ticks/s %.1f of %d Hz   sim %.3f ms/tick   alpha %.2f%s\n",
           s->fps, s->fps > 0 ? 1000.0 / s->fps : 0.0, s->tps, clock->hz, s->tick_ms, (double)cw_clock_alpha(clock), eol);
    if (s->violations)
        printf("checks     %" PRIu64 " FAILED of %" PRIu64 " (%s)%s\n", s->violations, s->checks, s->last_violation, eol);
    else
        printf("checks     OK, %" PRIu64 " verified%s\n", s->checks, eol);
    if (interactive) fputs("\x1b[J", stdout);
    fflush(stdout);
}

/* ---- main --------------------------------------------------------------- */
static long parse_arg(int argc, char **argv, int *i, const char *name, long fallback) {
    if (strcmp(argv[*i], name) != 0) return fallback;
    if (*i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", name); exit(2); }
    return strtol(argv[++*i], NULL, 10);
}
int main(int argc, char **argv) {
    long frames = -1, hz = CW_DEFAULT_TICK_HZ, fps = 60, seed = 0x1a2b3c4d;
    sim_state s = {0};
    s.spawn_per_tick = 40;
    s.max_entities = 20000;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--frames") == 0) frames = parse_arg(argc, argv, &i, "--frames", frames);
        else if (strcmp(argv[i], "--spawn") == 0) s.spawn_per_tick = (int)parse_arg(argc, argv, &i, "--spawn", 40);
        else if (strcmp(argv[i], "--max") == 0) s.max_entities = (size_t)parse_arg(argc, argv, &i, "--max", 20000);
        else if (strcmp(argv[i], "--hz") == 0) hz = parse_arg(argc, argv, &i, "--hz", hz);
        else if (strcmp(argv[i], "--fps") == 0) fps = parse_arg(argc, argv, &i, "--fps", fps);
        else if (strcmp(argv[i], "--seed") == 0) seed = parse_arg(argc, argv, &i, "--seed", seed);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    s.rng = seed ? (uint32_t)seed : 1;
    interactive = frames < 0 && isatty(STDOUT_FILENO) && isatty(STDIN_FILENO);
    if (!interactive && frames < 0) frames = 300;

    cw_world world;
    cw_sim sim;
    cw_world_init(&world, &s);
    cw_sim_init(&sim, CW_HOST);
    if (cw_comp_block(&world, COMPONENT_COUNT) != 0 || !CW_REGISTER(&world, POSITION, Position)
        || !CW_REGISTER(&world, VELOCITY, Velocity) || !CW_REGISTER(&world, LIFETIME, Lifetime)) return 1;
    const cw_component_id moving[] = {POSITION, VELOCITY};
    const cw_component_id velocity[] = {VELOCITY}, lifetime[] = {LIFETIME};
    s.all = cw_query_create_access(&world, cw_filter_all(cw_mask(NULL, 0)), cw_access_read(cw_mask(NULL, 0)));
    s.moving = cw_query_create_access(&world, cw_filter_all(cw_mask(moving, 2)),
                                      cw_access_make(cw_mask(velocity, 1), cw_mask(moving, 2)));
    s.mortal = cw_query_create_access(&world, cw_filter_all(cw_mask(lifetime, 1)),
                                      cw_access_write(cw_mask(lifetime, 1)));
    s.position_changes = cw_observer_create(&world, POSITION);
    if (!s.all || !s.moving || !s.mortal || !s.position_changes) return 1;
    cw_world_reserve_entities(&world, s.max_entities);
    const cw_system_desc systems[] = {
        {"spawn", CW_PHASE(CW_EARLY_UPDATE), spawn_system, CW_SHARED, 0, false},
        {"movement", CW_PHASE(CW_UPDATE), movement_system, CW_SHARED, 0, false},
        {"lifetime", CW_PHASE(CW_LATE_UPDATE), lifetime_system, CW_SHARED, 0, false},
        {"lifecycle", CW_PHASE(CW_ENTITY_CREATED) | CW_PHASE(CW_ENTITY_DESTROYED), lifecycle_system, CW_SHARED, 0, true},
    };
    for (size_t i = 0; i < sizeof(systems) / sizeof(*systems); ++i)
        if (!cw_sim_register(&sim, systems[i])) return 1;

    if (interactive) setup_terminal();
    cw_clock clock = cw_clock_make((int)hz);
    double frame_target = fps > 0 ? 1.0 / (double)fps : 0.0;
    double last = now_seconds();
    s.window_start = last;
    bool paused = false;
    for (long frame = 0; !stop_requested && (frames < 0 || frame < frames); ++frame) {
        double frame_start = now_seconds();
        float dt = (float)(frame_start - last);
        last = frame_start;
        if (!interactive) dt = (float)frame_target;   /* deterministic headless pacing */
        int key;
        bool step = false;
        while ((key = read_key()) >= 0) {
            if (key == 'q' || key == 3) stop_requested = 1;
            else if (key == ' ') paused = !paused;
            else if (key == 's') step = true;
            else if (key == '+' || key == '=') s.spawn_per_tick += 10;
            else if (key == '-' && s.spawn_per_tick > 0) s.spawn_per_tick = s.spawn_per_tick > 10 ? s.spawn_per_tick - 10 : 0;
            else if (key == 'd') {
                cw_query it = cw_query_begin(s.all);
                cw_entity_id id;
                while (cw_query_next(&it, &id)) if (id & 1) cw_world_mark_destroy(&world, id);
            } else if (key == 'r') {
                cw_world_reset(&world);
                cw_observer_observe(s.position_changes);   /* drop the reset's removals */
            }
        }
        int steps = paused ? (step ? 1 : 0) : cw_clock_advance(&clock, dt);
        for (int i = 0; i < steps; ++i) {
            double t0 = now_seconds();
            cw_sim_tick(&sim, &world, clock.dt);
            s.tick_time_accum += now_seconds() - t0;
            ++s.ticks_window;
            observe_changes(&world, &s);
            verify(&world, &s, false);
        }
        if (frame % 30 == 0) verify(&world, &s, true);
        ++s.frames_window;
        bool last_frame = frame + 1 == frames;
        double elapsed = now_seconds() - s.window_start;
        if (elapsed >= 1.0 || (last_frame && elapsed > 0)) {
            s.fps = (double)s.frames_window / elapsed;
            s.tps = (double)s.ticks_window / elapsed;
            s.created_rate = (double)s.created_window / elapsed;
            s.destroyed_rate = (double)s.destroyed_window / elapsed;
            s.tick_ms = s.ticks_window ? s.tick_time_accum / (double)s.ticks_window * 1000.0 : 0.0;
            s.frames_window = s.ticks_window = s.created_window = s.destroyed_window = 0;
            s.tick_time_accum = 0;
            s.window_start = now_seconds();
        }
        if (interactive || last_frame) render(&world, &s, &clock, paused);
        if (interactive && frame_target > 0) {
            double remaining = frame_target - (now_seconds() - frame_start);
            if (remaining > 0) {
                struct timespec ts = {(time_t)remaining, (long)((remaining - floor(remaining)) * 1e9)};
                nanosleep(&ts, NULL);
            }
        }
    }
    bool ok = s.violations == 0;
    cw_query_destroy(s.all);
    cw_query_destroy(s.moving);
    cw_query_destroy(s.mortal);
    cw_observer_destroy(s.position_changes);
    cw_sim_destroy(&sim);
    cw_world_destroy(&world);
    if (!interactive) printf("%s: %" PRIu64 " checks, %" PRIu64 " violations\n", ok ? "OK" : "FAILED", s.checks, s.violations);
    return ok ? 0 : 1;
}
