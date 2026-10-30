/* shapes3d: render3d on its own, without render2d or the engine.
 *
 * A window with the four primitives the renderer knows - a sphere, a
 * capsule, a cube and a plane - drawn unlit, the camera slowly orbiting
 * the scene, plus the grid and axes an editor would draw. This is the
 * smallest use of render3d: setup, begin_frame, draw, present.
 *
 *   clockwork_shapes3d [--frames N]     render N frames and exit (smoke test)
 */
#include "render3d.h"
#include "sokol_app.h"
#include "sokol_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct { unsigned frames, max_frames; float time; bool ready; } app;

static void init(void) {
    r3d_desc d;
    memset(&d, 0, sizeof(d));
    d.background = r3d_rgb(30, 33, 40);
    app.ready = r3d_setup(&d);
    if (!app.ready) { fputs("render3d setup failed\n", stderr); r3d_shutdown(); sapp_request_quit(); return; }
    r3d_camera cam = r3d_camera_make(r3d_v3(7, 5, 9), r3d_v3(0, 0.8f, 0));
    r3d_set_camera(&cam);
}
static void frame(void) {
    if (!app.ready) return;
    float dt = (float)sapp_frame_duration();
    app.time += dt;
    float w = sapp_widthf() / sapp_dpi_scale(), h = sapp_heightf() / sapp_dpi_scale();
    r3d_begin_frame(0, 0, w, h, sapp_dpi_scale());
    r3d_camera *cam = r3d_camera_ptr();
    r3d_camera_orbit(cam, dt * 0.3f, 0);
    r3d_set_camera(cam);

    r3d_primitive ground = r3d_primitive_make(R3D_PLANE, r3d_v3(0, 0, 0), r3d_v3(12, 1, 12), r3d_rgb(118, 124, 136));
    ground.checker = 1;
    r3d_draw_primitive(&ground);
    r3d_draw_sphere(r3d_v3(-2.5f, 1, 0), 1, r3d_rgb(235, 120, 90));
    r3d_draw_capsule(r3d_v3(0, 1.2f, -1.5f), 0.5f, 1.4f, r3d_v3(0, 0, app.time * 0.5f), r3d_rgb(120, 200, 140));
    r3d_draw_cube(r3d_v3(2.5f, 0.75f, 0.5f), r3d_v3(1.5f, 1.5f, 1.5f), r3d_v3(0, app.time, 0), r3d_rgb(110, 150, 235));
    r3d_draw_plane(r3d_v3(0, 1.5f, 2.5f), 2, 1.2f, r3d_v3(R3D_PI / 2, 0, 0), r3d_rgba(255, 220, 120, 160));
    r3d_primitive wire = r3d_primitive_make(R3D_SPHERE, r3d_v3(0, 2.6f, 0), r3d_v3(0.6f, 0.6f, 0.6f), r3d_rgb(255, 255, 255));
    wire.wireframe = true;
    r3d_draw_primitive(&wire);
    r3d_draw_grid(1, 20, r3d_rgba(255, 255, 255, 22), r3d_rgba(255, 255, 255, 60));
    r3d_draw_axes(r3d_v3(0, 0.01f, 0), 1.5f);
    r3d_present();
    ++app.frames;
    if (app.max_frames && app.frames >= app.max_frames) sapp_request_quit();
}
static void cleanup(void) { if (app.ready) r3d_shutdown(); }
static void event(const sapp_event *e) {
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN && e->key_code == SAPP_KEYCODE_ESCAPE) sapp_request_quit();
}
int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) app.max_frames = (unsigned)strtoul(argv[++i], NULL, 10);
        else { printf("usage: %s [--frames N]\n", argv[0]); return 2; }
    }
    sapp_desc d;
    memset(&d, 0, sizeof(d));
    d.init_cb = init;
    d.frame_cb = frame;
    d.cleanup_cb = cleanup;
    d.event_cb = event;
    d.width = 960;
    d.height = 600;
    d.sample_count = 4;
    d.high_dpi = true;
    d.window_title = "render3d: shapes";
    d.logger.func = slog_func;
    sapp_run(&d);
    if (app.max_frames) printf("OK: rendered %u frames\n", app.frames);
    return app.ready ? 0 : 1;
}
