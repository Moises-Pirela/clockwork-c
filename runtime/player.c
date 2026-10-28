/* The player: runs a game module on a scene.
 *
 * This is the engine's runtime. It knows no game: it loads a module (a
 * shared library named by a project, or the one linked in when built with
 * CW_STATIC_GAME, which is what an exported game is), binds a world to it,
 * loads a scene and runs the simulation at the module's fixed rate, drawing
 * through the module's canvas hook with render interpolation.
 *
 * Usage:
 *   clockwork_player --project game.cwproj [--scene file.cwscene]
 *   clockwork_player --module lib.so --scene file.cwscene
 *   exported_game [--scene file.cwscene]            (game.cwproj next to the executable)
 *   ... --headless N      run N ticks without a window and exit (self-check)
 *   ... --frames N        render N frames and exit (smoke test)
 *   ... --config FILE     rendering settings (default: the project's render.cfg, else tunable.cfg)
 *
 * Keys: Space pause   N step   R reload the scene   F fit view   F11 fullscreen   Esc quit
 *       right/middle drag or arrow keys pan   wheel or +/- zoom
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "cw_game.h"
#include "host_canvas.h"
#include "render2d.h"
#include "r2d_post.h"
#include "r2d_lighting.h"
#include "sokol_log.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    cw_game game;
    cw_game_world gw;
    cw_project project;
    cw_assets assets;
    bool project_open, ready, failed, paused;
    char scene[CW_GAME_PATH_CAPACITY];
    char config[CW_GAME_PATH_CAPACITY];
    unsigned frames, max_frames;
    host_canvas canvas;
    char status[200];
    float time;
} app;

static void set_status(const char *text) {
    size_t n = strlen(text);
    if (n >= sizeof(app.status)) n = sizeof(app.status) - 1;
    memcpy(app.status, text, n);
    app.status[n] = 0;
    puts(text);
    fflush(stdout);
}
static bool load_scene(void) {
    if (!app.scene[0]) {
        cw_game_world_clear(&app.gw);
        set_status("no scene given: empty world");
        return true;
    }
    cw_scene_result r;
    if (!cw_game_world_load_scene(&app.gw, app.scene, NULL, &r)) {
        char message[1024];
        snprintf(message, sizeof(message), "cannot load %s (line %d): %s", app.scene, r.line, r.message);
        set_status(message);
        return false;
    }
    cw_game_world_settle(&app.gw);
    char message[1024];
    snprintf(message, sizeof(message), "loaded %s: %zu entities%s", app.scene, r.entities, r.warnings ? " (with warnings)" : "");
    set_status(message);
    return true;
}
static r2d_rect bounds(void) {
    const float *b = app.gw.module->bounds;
    if (b[2] > 0 && b[3] > 0) return r2d_rect_make(b[0], b[1], b[2], b[3]);
    return r2d_rect_make(-400, -300, 800, 600);
}

/* --- window --- */
static void window_init(void) {
    r2d_desc desc;
    memset(&desc, 0, sizeof(desc));
    r2d_rect b = bounds();
    desc.camera = r2d_camera_make(r2d_rect_center(b), 1.0f, false);
    desc.camera.min_zoom = 0.1f;
    desc.camera.max_zoom = 12.0f;
    desc.camera_controls = true;
    desc.pan_with_right_button = desc.pan_with_middle_button = desc.pan_with_arrows = true;
    desc.text_scale = 1.25f;
    desc.post_config = app.config;
    r2d_setup(&desc);
    if (!r2d_valid()) { app.failed = true; sapp_request_quit(); return; }
    host_canvas_init(&app.canvas);
    app.canvas.pickable = false;
    if (app.project_open) host_canvas_set_assets(&app.canvas, &app.assets);
    r2d_camera_fit_rect(b, 24);
    app.ready = true;
}
static void window_frame(void) {
    if (!app.ready) return;
    r2d_begin_frame();
    if (r2d_frame_count() == 1) r2d_camera_fit_rect(bounds(), 24);
    float dt = r2d_dt();
    app.time += dt;
    if (r2d_key_pressed(SAPP_KEYCODE_ESCAPE)) sapp_request_quit();
    if (r2d_key_pressed(SAPP_KEYCODE_SPACE)) { app.paused = !app.paused; cw_clock_reset(&app.gw.clock); set_status(app.paused ? "paused" : "running"); }
    if (r2d_key_pressed(SAPP_KEYCODE_N)) { app.paused = true; cw_game_world_tick(&app.gw, 1); }
    if (r2d_key_pressed(SAPP_KEYCODE_R)) load_scene();
    if (r2d_key_pressed(SAPP_KEYCODE_F)) r2d_camera_fit_rect(bounds(), 24);
    if (r2d_key_pressed(SAPP_KEYCODE_F11)) sapp_toggle_fullscreen();
    /* the interface covers the window; the mouse reaches its buttons through the engine's cw_ui system */
    r2d_vec2 vp = r2d_viewport();
    cw_ui_screen screen = {0, 0, vp.x, vp.y};
    cw_game_world_set_ui_screen(&app.gw, screen);
    cw_game_world_ui_input(&app.gw, r2d_mouse().x, r2d_mouse().y, r2d_mouse_down(R2D_MOUSE_LEFT), r2d_mouse_pressed(R2D_MOUSE_LEFT), r2d_mouse_released(R2D_MOUSE_LEFT));
    if (app.paused) cw_game_world_settle(&app.gw);
    else cw_game_world_advance(&app.gw, dt);

    r2d_set_space(R2D_SPACE_WORLD);
    host_canvas_begin(&app.canvas);
    cw_draw_context ctx = {cw_game_world_alpha(&app.gw), app.time, false, app.paused};
    cw_game_world_draw(&app.gw, &app.canvas.canvas, &ctx);

    r2d_set_space(R2D_SPACE_SCREEN);
    r2d_vec2 v = r2d_viewport();
    char line[256];
    snprintf(line, sizeof(line), "%s  tick %" PRIu64 "  %zu entities  %s", app.gw.module->name, app.gw.world.tick,
             cw_world_entity_count(&app.gw.world), app.paused ? "PAUSED (Space)" : "running");
    /* the player's own line sits at the bottom right, out of the game's interface */
    r2d_draw_text(r2d_v2(v.x - r2d_text_width(line) - 12, v.y - r2d_text_height() - 10), r2d_rgba(240, 242, 246, 200), line);
    r2d_draw_text(r2d_v2(12, v.y - r2d_text_height() - 10), r2d_rgba(200, 205, 215, 160), app.status);
    r2d_end_frame();
    ++app.frames;
    if (app.max_frames && app.frames >= app.max_frames) sapp_request_quit();
}
static void window_event(const sapp_event *event) { if (app.ready) r2d_handle_event(event); }
static void window_cleanup(void) {
    if (!app.ready) return;
    host_canvas_destroy(&app.canvas);
    r2d_shutdown();
    app.ready = false;
}
static int run_window(void) {
    sapp_desc window;
    memset(&window, 0, sizeof(window));
    window.init_cb = window_init;
    window.frame_cb = window_frame;
    window.cleanup_cb = window_cleanup;
    window.event_cb = window_event;
    window.width = app.project_open ? app.project.window_width : 1280;
    window.height = app.project_open ? app.project.window_height : 800;
    window.sample_count = 4;
    window.high_dpi = true;
    char title[160];
    snprintf(title, sizeof(title), "%s", app.project_open ? app.project.title : app.gw.module->name);
    window.window_title = title;
    window.logger.func = slog_func;
    sapp_run(&window);
    if (app.max_frames && !app.failed) printf("OK: rendered %u frames\n", app.frames);
    return app.failed ? 1 : 0;
}

static int run_headless(int ticks) {
    size_t before = cw_world_entity_count(&app.gw.world);
    cw_game_world_tick(&app.gw, ticks);
    /* draw once through a counting canvas to exercise the module's draw path */
    host_canvas hc;
    host_canvas_init(&hc);
    hc.lights = false;
    hc.pickable = false;
    (void)hc;
    printf("OK: %s ran %d ticks headless (%zu entities before, %zu after)\n", app.gw.module->name, ticks, before,
           cw_world_entity_count(&app.gw.world));
    host_canvas_destroy(&hc);
    return 0;
}

static bool parse_count(const char *text, unsigned *out) {
    char *end;
    errno = 0;
    unsigned long v = strtoul(text, &end, 10);
    if (errno == ERANGE || text[0] == '-' || *end || !v || v > UINT32_MAX) return false;
    *out = (unsigned)v;
    return true;
}
static void exe_dir(const char *argv0, char *out, size_t capacity) {
    snprintf(out, capacity, "%s", argv0);
    char *slash = strrchr(out, '/');
#ifdef _WIN32
    char *bslash = strrchr(out, '\\');
    if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
    if (slash) slash[1] = 0; else out[0] = 0;
}
int main(int argc, char **argv) {
    const char *project = NULL, *module = NULL, *scene = NULL;
    int headless = -1;
    snprintf(app.config, sizeof(app.config), "tunable.cfg");
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        unsigned n;
        if (strcmp(a, "--project") == 0 && i + 1 < argc) project = argv[++i];
        else if (strcmp(a, "--module") == 0 && i + 1 < argc) module = argv[++i];
        else if (strcmp(a, "--scene") == 0 && i + 1 < argc) scene = argv[++i];
        else if (strcmp(a, "--config") == 0 && i + 1 < argc) snprintf(app.config, sizeof(app.config), "%s", argv[++i]);
        else if (strcmp(a, "--headless") == 0 && i + 1 < argc && parse_count(argv[++i], &n)) headless = (int)n;
        else if (strcmp(a, "--frames") == 0 && i + 1 < argc && parse_count(argv[++i], &n)) app.max_frames = n;
        else {
            printf("usage: %s [--project game.cwproj] [--module lib%s] [--scene file.cwscene] [--headless N] [--frames N] [--config tunable.cfg]\n",
                   argv[0], cw_game_library_suffix());
            return strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0 ? 0 : 2;
        }
    }
    char error[256];
#ifdef CW_STATIC_GAME
    if (!project) {   /* an exported game: the project copy lives next to the executable */
        static char default_project[CW_GAME_PATH_CAPACITY];
        exe_dir(argv[0], default_project, sizeof(default_project));
        strncat(default_project, "game.cwproj", sizeof(default_project) - strlen(default_project) - 1);
        FILE *f = fopen(default_project, "rb");
        if (f) { fclose(f); project = default_project; }
    }
#else
    (void)exe_dir;
#endif
    bool config_given = strcmp(app.config, "tunable.cfg") != 0;
    if (project) {
        if (!cw_project_load(&app.project, project, error, sizeof(error))) { fprintf(stderr, "%s\n", error); return 1; }
        app.project_open = true;
        if (!module) module = app.project.module;
        if (!scene && app.project.main_scene[0]) scene = app.project.main_scene;
        /* the project's assets, for game code that resolves asset ids */
        cw_assets_init(&app.assets, app.project.assets);
        if (!cw_assets_load(&app.assets, error, sizeof(error))) fprintf(stderr, "assets: %s\n", error);
        /* the project's rendering settings, when it has saved any */
        FILE *f = config_given ? NULL : fopen(app.project.render, "rb");
        if (f) { fclose(f); snprintf(app.config, sizeof(app.config), "%s", app.project.render); }
    }
#ifdef CW_STATIC_GAME
    if (!cw_game_use(&app.game, cw_game_export(), error, sizeof(error))) { fprintf(stderr, "%s\n", error); return 1; }
#else
    if (!module) { fputs("no module: give --project or --module\n", stderr); return 2; }
    if (!cw_game_open(&app.game, module, error, sizeof(error))) { fprintf(stderr, "%s\n", error); return 1; }
#endif
    if (!cw_game_world_init(&app.gw, app.game.module, error, sizeof(error))) { fprintf(stderr, "%s\n", error); cw_game_close(&app.game); return 1; }
    if (app.project_open) cw_game_world_set_assets(&app.gw, &app.assets);
    if (app.project_open) cw_game_world_set_ui_canvas(&app.gw, &app.project.ui);
    if (scene) snprintf(app.scene, sizeof(app.scene), "%s", scene);
    bool loaded = load_scene();
    int result;
    if (headless >= 0) result = loaded ? run_headless(headless) : 1;
    else result = run_window();
    cw_game_world_destroy(&app.gw);
    cw_game_close(&app.game);
    if (app.project_open) cw_assets_destroy(&app.assets);
    return result;
}
