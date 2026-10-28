#include "render2d.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "util/sokol_gl.h"
#include "util/sokol_debugtext.h"
#include "r2d_post_internal.h"
#include "r2d_lighting_internal.h"
#include "r2d_sprite_internal.h"

#define R2D_GLYPH 8.0f
#define R2D_PI 3.14159265358979f
#define R2D_MAX_TEXT 1024

typedef enum { PICK_CIRCLE, PICK_RECT } pick_kind;
typedef struct {
    uint64_t id;
    pick_kind kind;
    r2d_vec2 center;     /* screen space */
    float radius;
    r2d_rect rect;       /* screen space */
} pickable;

typedef struct { r2d_rect *items; size_t count, capacity; } rect_list;

typedef enum { SEL_IDLE, SEL_PRESS_ITEM, SEL_PRESS_EMPTY, SEL_DRAG_ITEM, SEL_DRAG_BOX } sel_mode;

static struct {
    bool valid, setup_gfx;
    r2d_desc desc;
    r2d_camera camera, initial_camera;
    r2d_vec2 viewport;
    float dpi;
    float dt, time;
    uint64_t frame;
    bool resized;
    bool sprites_ready;
    sgl_pipeline pipeline;          /* alpha-blend pipeline of the overlay (window) context */
    r2d_space space;
    /* post-processing: the scene layer is drawn into offscreen contexts whose
     * formats match the post stack's targets; the overlay contexts (the
     * sokol-gl / debugtext defaults) target the window */
    bool post_created, post_frame;   /* post_frame: this frame's scene goes through the stack */
    sgl_context gl_scene;
    sgl_pipeline pipeline_scene;
    sdtx_context text_scene;
    sg_pixel_format scene_format;
    r2d_camera camera_prev;

    /* input */
    r2d_vec2 mouse, mouse_prev, mouse_delta;
    bool button_down[R2D_MOUSE_BUTTONS], button_pressed[R2D_MOUSE_BUTTONS], button_released[R2D_MOUSE_BUTTONS];
    float wheel;
    bool key_down[SAPP_MAX_KEYCODES], key_pressed[SAPP_MAX_KEYCODES], key_released[SAPP_MAX_KEYCODES];
    uint8_t key_typed[SAPP_MAX_KEYCODES]; /* presses and auto-repeats this frame */
    uint32_t chars[R2D_MAX_CHARS];       /* characters typed this frame */
    size_t char_count;
    uint32_t modifiers;
    bool panning;
    int pan_button;

    /* picking and selection */
    pickable *picks;
    size_t pick_count, pick_capacity;
    uint64_t *selected;
    size_t selected_count, selected_capacity;
    sel_mode mode;
    r2d_vec2 press_screen, press_world, last_drag_world;
    uint64_t press_id;
    bool press_additive, press_was_selected;

    /* ui */
    rect_list ui_rects[2];       /* [frame & 1] is the current frame */
    r2d_vec2 ui_press;           /* mouse position when the left button went down */
    bool ui_press_valid;

    r2d_stats stats, pending_stats;
} S;

/* ---------------------------------------------------------------------------
 * helpers
 * ------------------------------------------------------------------------- */
static void *grow(void *data, size_t *capacity, size_t needed, size_t elem) {
    if (needed <= *capacity) return data;
    size_t cap = *capacity ? *capacity : 64;
    while (cap < needed) cap *= 2;
    void *next = realloc(data, cap * elem);
    if (!next) { fputs("render2d: out of memory\n", stderr); abort(); }
    *capacity = cap;
    return next;
}
static void rect_list_push(rect_list *list, r2d_rect r) {
    list->items = grow(list->items, &list->capacity, list->count + 1, sizeof(*list->items));
    list->items[list->count++] = r;
}
static bool in_screen_space(void) { return S.space == R2D_SPACE_SCREEN || S.space == R2D_SPACE_HUD; }
static r2d_vec2 to_screen(r2d_vec2 p) {
    return !in_screen_space() ? r2d_camera_world_to_screen(&S.camera, S.viewport, p) : p;
}
static float to_screen_len(float len) { return !in_screen_space() ? len * S.camera.zoom : len; }
static r2d_rect to_screen_rect(r2d_rect r) {
    if (in_screen_space()) return r;
    return r2d_rect_from_points(to_screen(r2d_rect_min(r)), to_screen(r2d_rect_max(r)));
}
static bool on_screen(r2d_rect screen_bounds) {
    return r2d_rect_overlaps(screen_bounds, r2d_rect_make(-8, -8, S.viewport.x + 16, S.viewport.y + 16));
}
static void set_color(r2d_color c) { sgl_c4b(c.r, c.g, c.b, c.a); }
static float text_scale(void) { return S.desc.text_scale; }
static int circle_segments(float screen_radius) {
    int n = (int)(screen_radius * 1.2f) + 10;
    return n < 10 ? 10 : (n > 128 ? 128 : n);
}

r2d_ui_style r2d_ui_style_default(void) {
    r2d_ui_style s;
    s.panel = r2d_rgba(28, 31, 38, 235);
    s.panel_border = r2d_rgb(58, 63, 74);
    s.text = r2d_rgb(226, 230, 236);
    s.text_dim = r2d_rgb(140, 146, 158);
    s.button = r2d_rgb(58, 64, 76);
    s.button_hover = r2d_rgb(80, 90, 108);
    s.button_active = r2d_rgb(110, 130, 160);
    s.button_text = r2d_rgb(236, 240, 246);
    s.toggle_on = r2d_rgb(72, 140, 96);
    s.selection_box = r2d_rgba(120, 180, 255, 50);
    s.selection_box_border = r2d_rgba(150, 200, 255, 220);
    return s;
}
static bool style_is_zero(const r2d_ui_style *s) {
    r2d_ui_style zero;
    memset(&zero, 0, sizeof(zero));
    return memcmp(s, &zero, sizeof(zero)) == 0;
}

/* ---------------------------------------------------------------------------
 * setup / teardown
 * ------------------------------------------------------------------------- */
static sg_pipeline_desc alpha_pipeline_desc(void) {
    sg_pipeline_desc pip;
    memset(&pip, 0, sizeof(pip));
    pip.colors[0].blend.enabled = true;
    pip.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    pip.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pip.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    pip.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ZERO;
    pip.depth.write_enabled = false;
    pip.label = "render2d-alpha";
    return pip;
}
static void destroy_scene_contexts(void) {
    if (S.pipeline_scene.id) sgl_destroy_pipeline(S.pipeline_scene);
    if (S.gl_scene.id) sgl_destroy_context(S.gl_scene);
    if (S.text_scene.id) sdtx_destroy_context(S.text_scene);
    S.pipeline_scene.id = 0;
    S.gl_scene.id = 0;
    S.text_scene.id = 0;
}
static bool make_scene_contexts(sg_pixel_format format, int sample_count) {
    destroy_scene_contexts();
    sgl_context_desc_t gl;
    memset(&gl, 0, sizeof(gl));
    gl.max_vertices = S.desc.max_vertices;
    gl.max_commands = S.desc.max_commands;
    gl.color_format = format;
    gl.depth_format = SG_PIXELFORMAT_NONE;
    gl.sample_count = sample_count;
    S.gl_scene = sgl_make_context(&gl);
    sg_pipeline_desc pip = alpha_pipeline_desc();
    S.pipeline_scene = sgl_context_make_pipeline(S.gl_scene, &pip);
    sdtx_context_desc_t text;
    memset(&text, 0, sizeof(text));
    text.char_buf_size = S.desc.text_chars;
    text.color_format = format;
    text.depth_format = SG_PIXELFORMAT_NONE;
    text.sample_count = sample_count;
    S.text_scene = sdtx_make_context(&text);
    S.scene_format = format;
    return S.gl_scene.id && S.pipeline_scene.id && S.text_scene.id;
}
void r2d_setup(const r2d_desc *desc) {
    r2d_desc d;
    if (desc) d = *desc; else memset(&d, 0, sizeof(d));
    memset(&S, 0, sizeof(S));
    if (d.camera.zoom <= 0) d.camera = r2d_camera_make(r2d_v2(0, 0), 1.0f, false);
    if (d.camera.min_zoom <= 0) d.camera.min_zoom = 0.01f;
    if (d.camera.max_zoom <= 0) d.camera.max_zoom = 1000.0f;
    if (d.clear_color.a == 0) d.clear_color = r2d_rgb(22, 24, 29);
    if (d.text_scale <= 0) d.text_scale = 1.5f;
    if (d.max_vertices <= 0) d.max_vertices = 1 << 20;
    if (d.max_commands <= 0) d.max_commands = 1 << 16;
    if (d.text_chars <= 0) d.text_chars = 1 << 16;
    if (d.camera_controls && !d.pan_with_right_button && !d.pan_with_middle_button && !d.pan_with_arrows && !d.pan_with_wasd) {
        d.pan_with_right_button = d.pan_with_middle_button = true;
    }
    if (d.key_pan_speed <= 0) d.key_pan_speed = 600.0f;
    if (d.wheel_zoom_step <= 1.0f) d.wheel_zoom_step = 1.15f;
    if (d.pick_slop <= 0) d.pick_slop = 2.0f;
    if (d.drag_threshold <= 0) d.drag_threshold = 4.0f;
    if (style_is_zero(&d.style)) d.style = r2d_ui_style_default();
    S.desc = d;
    S.camera = S.initial_camera = d.camera;
    S.space = R2D_SPACE_WORLD;
    S.pan_button = -1;
    S.viewport = r2d_v2(sapp_widthf(), sapp_heightf());
    S.dpi = sapp_dpi_scale() > 0 ? sapp_dpi_scale() : 1.0f;
    S.viewport = r2d_scale(S.viewport, 1.0f / S.dpi);

    if (!d.skip_gfx_setup) {
        sg_desc gfx;
        memset(&gfx, 0, sizeof(gfx));
        gfx.environment = sglue_environment();
        gfx.logger.func = r2d_sprite__gfx_logger;
        sg_setup(&gfx);
        S.setup_gfx = true;
    }
    if (!sg_isvalid()) return;

    sgl_desc_t gl;
    memset(&gl, 0, sizeof(gl));
    gl.max_vertices = d.max_vertices;
    gl.max_commands = d.max_commands;
    gl.sample_count = sapp_sample_count();
    gl.logger.func = slog_func;
    sgl_setup(&gl);

    sg_pipeline_desc pip = alpha_pipeline_desc();
    S.pipeline = sgl_make_pipeline(&pip);

    sdtx_desc_t text;
    memset(&text, 0, sizeof(text));
    text.fonts[0] = sdtx_font_cpc();
    text.context.char_buf_size = d.text_chars;
    text.context.sample_count = sapp_sample_count();
    text.logger.func = slog_func;
    sdtx_setup(&text);
    S.valid = sgl_error().any == false;
    S.camera_prev = S.camera;
    if (S.valid) S.sprites_ready = r2d_sprite__setup();

    if (S.valid && !d.no_post_process) {
        sg_environment env = sglue_environment();
        S.post_created = r2d_post__create(sapp_sample_count(), env.defaults.color_format, env.defaults.depth_format);
        if (S.post_created && d.post_config) {
            char error[256];
            if (!r2d_post_load_config(d.post_config, error, sizeof(error)) || error[0])
                fprintf(stderr, "render2d: post config %s: %s\n", d.post_config, error[0] ? error : "not loaded");
        }
        if (S.post_created && !make_scene_contexts(r2d_post__scene_color_format(), r2d_post__scene_sample_count())) {
            destroy_scene_contexts();
            r2d_post__destroy();
            S.post_created = false;
        }
    }
}
void r2d_shutdown(void) {
    if (S.valid) {
        if (S.sprites_ready) r2d_sprite__shutdown();
        destroy_scene_contexts();
        if (S.post_created) r2d_post__destroy();
        sdtx_shutdown();
        sgl_destroy_pipeline(S.pipeline);
        sgl_shutdown();
    }
    if (S.setup_gfx && sg_isvalid()) sg_shutdown();
    free(S.picks);
    free(S.selected);
    free(S.ui_rects[0].items);
    free(S.ui_rects[1].items);
    memset(&S, 0, sizeof(S));
}
bool r2d_valid(void) { return S.valid; }
void r2d_set_clear_color(r2d_color color) { S.desc.clear_color = color; }
void r2d_set_text_scale(float scale) { if (scale > 0) S.desc.text_scale = scale; }

/* ---------------------------------------------------------------------------
 * events and input
 * ------------------------------------------------------------------------- */
static bool key_in_range(sapp_keycode key) { return (int)key > 0 && (int)key < (int)SAPP_MAX_KEYCODES; }
static int map_button(sapp_mousebutton b) {
    switch (b) {
        case SAPP_MOUSEBUTTON_LEFT: return R2D_MOUSE_LEFT;
        case SAPP_MOUSEBUTTON_RIGHT: return R2D_MOUSE_RIGHT;
        case SAPP_MOUSEBUTTON_MIDDLE: return R2D_MOUSE_MIDDLE;
        default: return -1;
    }
}
static void release_everything(void) {
    for (int i = 0; i < R2D_MOUSE_BUTTONS; ++i) {
        if (S.button_down[i]) S.button_released[i] = true;
        S.button_down[i] = false;
    }
    for (int i = 0; i < SAPP_MAX_KEYCODES; ++i) {
        if (S.key_down[i]) S.key_released[i] = true;
        S.key_down[i] = false;
    }
    S.modifiers = 0;
}
void r2d_handle_event(const sapp_event *e) {
    if (!S.valid || !e) return;
    float dpi = S.dpi > 0 ? S.dpi : 1.0f;
    switch (e->type) {
        case SAPP_EVENTTYPE_MOUSE_MOVE:
        case SAPP_EVENTTYPE_MOUSE_DOWN:
        case SAPP_EVENTTYPE_MOUSE_UP:
        case SAPP_EVENTTYPE_MOUSE_SCROLL:
            S.mouse = r2d_v2(e->mouse_x / dpi, e->mouse_y / dpi);
            /* modifiers are taken from press events only, so that a release
             * arriving in the same frame cannot clear them before the frame
             * has looked at the press (shift-click, ctrl+key on slow frames) */
            if (e->type != SAPP_EVENTTYPE_MOUSE_UP) S.modifiers = e->modifiers;
            if (e->type == SAPP_EVENTTYPE_MOUSE_DOWN || e->type == SAPP_EVENTTYPE_MOUSE_UP) {
                int b = map_button(e->mouse_button);
                if (b < 0) break;
                if (e->type == SAPP_EVENTTYPE_MOUSE_DOWN) {
                    S.button_down[b] = true;
                    S.button_pressed[b] = true;
                    if (b == R2D_MOUSE_LEFT) { S.ui_press = S.mouse; S.ui_press_valid = true; }
                } else {
                    S.button_down[b] = false;
                    S.button_released[b] = true;
                }
            } else if (e->type == SAPP_EVENTTYPE_MOUSE_SCROLL) {
                S.wheel += e->scroll_y;
            }
            break;
        case SAPP_EVENTTYPE_KEY_DOWN:
            S.modifiers = e->modifiers;
            if (key_in_range(e->key_code)) {
                if (S.key_typed[e->key_code] < 255) ++S.key_typed[e->key_code];
                if (!e->key_repeat) {
                    S.key_down[e->key_code] = true;
                    S.key_pressed[e->key_code] = true;
                }
            }
            break;
        case SAPP_EVENTTYPE_CHAR:
            if (S.char_count < R2D_MAX_CHARS && e->char_code >= 32 && e->char_code != 127) S.chars[S.char_count++] = e->char_code;
            break;
        case SAPP_EVENTTYPE_KEY_UP:
            if (key_in_range(e->key_code)) {
                S.key_down[e->key_code] = false;
                S.key_released[e->key_code] = true;
            }
            break;
        case SAPP_EVENTTYPE_RESIZED:
            S.resized = true;
            break;
        case SAPP_EVENTTYPE_UNFOCUSED:
        case SAPP_EVENTTYPE_ICONIFIED:
        case SAPP_EVENTTYPE_SUSPENDED:
            release_everything();
            break;
        default:
            break;
    }
}
r2d_vec2 r2d_mouse(void) { return S.mouse; }
r2d_vec2 r2d_mouse_world(void) { return r2d_camera_screen_to_world(&S.camera, S.viewport, S.mouse); }
r2d_vec2 r2d_mouse_delta(void) { return S.mouse_delta; }
bool r2d_mouse_down(r2d_mouse_button b) { return b < R2D_MOUSE_BUTTONS && S.button_down[b]; }
bool r2d_mouse_pressed(r2d_mouse_button b) { return b < R2D_MOUSE_BUTTONS && S.button_pressed[b]; }
bool r2d_mouse_released(r2d_mouse_button b) { return b < R2D_MOUSE_BUTTONS && S.button_released[b]; }
float r2d_wheel(void) { return S.wheel; }
bool r2d_key_down(sapp_keycode key) { return key_in_range(key) && S.key_down[key]; }
bool r2d_key_pressed(sapp_keycode key) { return key_in_range(key) && S.key_pressed[key]; }
bool r2d_key_released(sapp_keycode key) { return key_in_range(key) && S.key_released[key]; }
bool r2d_key_typed(sapp_keycode key) { return key_in_range(key) && S.key_typed[key] > 0; }
unsigned r2d_key_typed_count(sapp_keycode key) { return key_in_range(key) ? S.key_typed[key] : 0; }
const uint32_t *r2d_chars(size_t *count) {
    if (count) *count = S.char_count;
    return S.chars;
}
uint32_t r2d_modifiers(void) { return S.modifiers; }
bool r2d_resized_this_frame(void) { return S.resized; }
static bool point_over_ui(r2d_vec2 p) {
    for (int buffer = 0; buffer < 2; ++buffer) {
        const rect_list *list = &S.ui_rects[buffer];
        for (size_t i = 0; i < list->count; ++i)
            if (r2d_rect_contains(list->items[i], p)) return true;
    }
    return false;
}
bool r2d_mouse_over_ui(void) { return point_over_ui(S.mouse); }

/* ---------------------------------------------------------------------------
 * frame
 * ------------------------------------------------------------------------- */
static void camera_controls(void) {
    const r2d_desc *d = &S.desc;
    if (!d->camera_controls) return;
    /* mouse panning */
    if (S.pan_button < 0) {
        int candidates[2] = {d->pan_with_right_button ? R2D_MOUSE_RIGHT : -1, d->pan_with_middle_button ? R2D_MOUSE_MIDDLE : -1};
        for (int i = 0; i < 2; ++i) {
            int b = candidates[i];
            if (b >= 0 && S.button_pressed[b] && !point_over_ui(S.mouse)) { S.pan_button = b; S.panning = true; }
        }
    }
    if (S.pan_button >= 0) {
        if (S.button_down[S.pan_button]) r2d_camera_pan_screen(&S.camera, S.mouse_delta);
        else { S.pan_button = -1; S.panning = false; }
    }
    /* wheel zoom about the cursor */
    if (S.wheel != 0 && !point_over_ui(S.mouse)) {
        float factor = powf(d->wheel_zoom_step, S.wheel);
        r2d_camera_zoom_at(&S.camera, S.viewport, S.mouse, factor);
    }
    if (!d->pan_with_arrows && !d->pan_with_wasd) return;
    r2d_vec2 dir = r2d_v2(0, 0);
    if (d->pan_with_arrows) {
        if (r2d_key_down(SAPP_KEYCODE_LEFT)) dir.x -= 1;
        if (r2d_key_down(SAPP_KEYCODE_RIGHT)) dir.x += 1;
        if (r2d_key_down(SAPP_KEYCODE_UP)) dir.y -= 1;
        if (r2d_key_down(SAPP_KEYCODE_DOWN)) dir.y += 1;
    }
    if (d->pan_with_wasd) {
        if (r2d_key_down(SAPP_KEYCODE_A)) dir.x -= 1;
        if (r2d_key_down(SAPP_KEYCODE_D)) dir.x += 1;
        if (r2d_key_down(SAPP_KEYCODE_W)) dir.y -= 1;
        if (r2d_key_down(SAPP_KEYCODE_S)) dir.y += 1;
    }
    if (dir.x != 0 || dir.y != 0) {
        /* the view moves in the key direction, so the content moves the other way */
        r2d_camera_pan_screen(&S.camera, r2d_scale(r2d_normalize(dir), -d->key_pan_speed * S.dt));
    }
    float zoom = 0;
    if (r2d_key_down(SAPP_KEYCODE_EQUAL) || r2d_key_down(SAPP_KEYCODE_KP_ADD)) zoom += 1;
    if (r2d_key_down(SAPP_KEYCODE_MINUS) || r2d_key_down(SAPP_KEYCODE_KP_SUBTRACT)) zoom -= 1;
    if (zoom != 0) r2d_camera_zoom_at(&S.camera, S.viewport, r2d_scale(S.viewport, 0.5f), powf(2.0f, zoom * S.dt));
    if (r2d_key_pressed(SAPP_KEYCODE_HOME)) r2d_camera_reset();
}
void r2d_begin_frame(void) {
    if (!S.valid) return;
    S.dpi = sapp_dpi_scale() > 0 ? sapp_dpi_scale() : 1.0f;
    S.viewport = r2d_v2(sapp_widthf() / S.dpi, sapp_heightf() / S.dpi);
    if (S.viewport.x < 1) S.viewport.x = 1;
    if (S.viewport.y < 1) S.viewport.y = 1;
    double seconds = sapp_frame_duration();
    S.dt = (float)(seconds < 0 ? 0 : (seconds > 0.1 ? 0.1 : seconds));
    S.time += S.dt;
    S.mouse_delta = r2d_sub(S.mouse, S.mouse_prev);
    S.pending_stats.frame_seconds = seconds;
    camera_controls();

    S.post_frame = false;
    if (S.post_created) {
        r2d_post__begin_frame(S.dt, S.viewport, sapp_width(), sapp_height());
        S.post_frame = r2d_post__scene_ready();
        if (S.post_frame && S.scene_format != r2d_post__scene_color_format()) {
            /* the hdr switch changed the target format: rebuild the scene contexts */
            if (!make_scene_contexts(r2d_post__scene_color_format(), r2d_post__scene_sample_count())) S.post_frame = false;
        }
    }
    float ts = text_scale();
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1 && !S.gl_scene.id) break;
        sgl_set_context(pass == 0 ? sgl_default_context() : S.gl_scene);
        sgl_defaults();
        sgl_load_pipeline(pass == 0 ? S.pipeline : S.pipeline_scene);
        sgl_matrix_mode_projection();
        sgl_ortho(0, S.viewport.x, S.viewport.y, 0, -1, 1);
        sgl_matrix_mode_modelview();
        sgl_load_identity();
        sdtx_set_context(pass == 0 ? sdtx_default_context() : S.text_scene);
        sdtx_canvas(S.viewport.x / ts, S.viewport.y / ts);
        sdtx_origin(0, 0);
        sdtx_font(0);
    }
    r2d_set_space(R2D_SPACE_WORLD);
}
void r2d_end_frame(void) {
    if (!S.valid) return;
    sg_pass pass;
    memset(&pass, 0, sizeof(pass));
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value.r = S.desc.clear_color.r / 255.0f;
    pass.action.colors[0].clear_value.g = S.desc.clear_color.g / 255.0f;
    pass.action.colors[0].clear_value.b = S.desc.clear_color.b / 255.0f;
    pass.action.colors[0].clear_value.a = 1.0f;
    pass.swapchain = sglue_swapchain();
    sgl_context overlay = sgl_default_context();
    sdtx_context overlay_text = sdtx_default_context();
    sgl_set_context(overlay);
    S.pending_stats.gl_vertices = sgl_num_vertices();
    S.pending_stats.gl_commands = sgl_num_commands();
    if (S.gl_scene.id) {
        sgl_set_context(S.gl_scene);
        S.pending_stats.gl_vertices += sgl_num_vertices();
        S.pending_stats.gl_commands += sgl_num_commands();
        sgl_set_context(overlay);
    }
    bool ui_in_scene = S.post_frame && r2d_post_params_ptr() && r2d_post_params_ptr()->post.apply_to_ui;
    sg_environment env = sglue_environment();
    if (S.post_frame && r2d_post__begin_scene(pass.action.colors[0].clear_value)) {
        sgl_context_draw_layer(S.gl_scene, 0);
        sdtx_context_draw_layer(S.text_scene, 0);
        if (S.sprites_ready) r2d_sprite__flush(r2d_post__scene_color_format(), SG_PIXELFORMAT_NONE, r2d_post__scene_sample_count(), &S.camera, S.viewport, S.time);
        sgl_context_draw_layer(S.gl_scene, 2);
        sdtx_context_draw_layer(S.text_scene, 2);
        if (ui_in_scene) {
            if (S.sprites_ready) r2d_sprite__flush_hud(r2d_post__scene_color_format(), SG_PIXELFORMAT_NONE, r2d_post__scene_sample_count(), S.viewport, S.time);
            sgl_context_draw_layer(S.gl_scene, 3);
            sdtx_context_draw_layer(S.text_scene, 3);
            sgl_context_draw_layer(S.gl_scene, 1);
            sdtx_context_draw_layer(S.text_scene, 1);
        }
        r2d_post__end_scene();
        r2d_post__set_motion(&S.camera_prev, &S.camera, S.viewport);
        r2d_post__set_view(&S.camera, S.viewport, S.dpi);
        r2d_post__run(pass.swapchain);   /* leaves the window pass open with the result drawn */
    } else {
        sg_begin_pass(&pass);
    }
    sgl_context_draw_layer(overlay, 0);
    sdtx_context_draw_layer(overlay_text, 0);
    if (!S.post_frame) {
        if (S.sprites_ready) r2d_sprite__flush(env.defaults.color_format, env.defaults.depth_format, sapp_sample_count(), &S.camera, S.viewport, S.time);
        sgl_context_draw_layer(overlay, 2);
        sdtx_context_draw_layer(overlay_text, 2);
    }
    if (!ui_in_scene) {   /* the HUD: its sprites, then its shapes and text */
        if (S.sprites_ready) r2d_sprite__flush_hud(env.defaults.color_format, env.defaults.depth_format, sapp_sample_count(), S.viewport, S.time);
        sgl_context_draw_layer(overlay, 3);
        sdtx_context_draw_layer(overlay_text, 3);
    }
    sgl_context_draw_layer(overlay, 1);
    sdtx_context_draw_layer(overlay_text, 1);
    if (S.sprites_ready) r2d_sprite__flush_screen(env.defaults.color_format, env.defaults.depth_format, sapp_sample_count(), S.viewport, S.time);
    sg_end_pass();
    sg_commit();
    S.camera_prev = S.camera;
    if (S.post_created) r2d_lighting__end_frame();
    if (sgl_context_error(overlay).any || (S.gl_scene.id && sgl_context_error(S.gl_scene).any))
        fputs("render2d: sokol_gl vertex or command budget exceeded\n", stderr);

    S.pending_stats.pickables = S.pick_count;
    S.pending_stats.selection = S.selected_count;
    S.pending_stats.ui_rects = S.ui_rects[S.frame & 1].count;
    S.stats = S.pending_stats;

    memset(S.button_pressed, 0, sizeof(S.button_pressed));
    memset(S.button_released, 0, sizeof(S.button_released));
    memset(S.key_pressed, 0, sizeof(S.key_pressed));
    memset(S.key_released, 0, sizeof(S.key_released));
    memset(S.key_typed, 0, sizeof(S.key_typed));
    S.char_count = 0;
    S.wheel = 0;
    S.resized = false;
    S.mouse_prev = S.mouse;
    S.pick_count = 0;
    ++S.frame;
    S.ui_rects[S.frame & 1].count = 0;
}

/* ---------------------------------------------------------------------------
 * viewport and camera
 * ------------------------------------------------------------------------- */
r2d_vec2 r2d_viewport(void) { return S.viewport; }
float r2d_dpi_scale(void) { return S.dpi; }
float r2d_dt(void) { return S.dt; }
uint64_t r2d_frame_count(void) { return S.frame; }
r2d_camera *r2d_camera_ptr(void) { return &S.camera; }
void r2d_set_camera(r2d_camera camera) {
    if (camera.zoom <= 0) camera.zoom = 1;
    if (camera.min_zoom <= 0) camera.min_zoom = S.initial_camera.min_zoom;
    if (camera.max_zoom <= 0) camera.max_zoom = S.initial_camera.max_zoom;
    camera.zoom = r2d_camera_clamp_zoom(&camera, camera.zoom);
    S.camera = camera;
}
void r2d_camera_reset(void) { S.camera = S.initial_camera; }
void r2d_camera_zoom_around(r2d_vec2 anchor, float factor) { r2d_camera_zoom_at(&S.camera, S.viewport, anchor, factor); }
void r2d_camera_fit_rect(r2d_rect world, float padding) { r2d_camera_fit(&S.camera, S.viewport, world, padding); }
void r2d_camera_fit_rect_in(r2d_rect area, r2d_rect world, float padding) {
    r2d_camera_fit_in(&S.camera, S.viewport, area, world, padding);
}
r2d_rect r2d_visible_world(void) { return r2d_camera_visible_rect(&S.camera, S.viewport); }
r2d_vec2 r2d_world_to_screen(r2d_vec2 p) { return r2d_camera_world_to_screen(&S.camera, S.viewport, p); }
r2d_vec2 r2d_screen_to_world(r2d_vec2 p) { return r2d_camera_screen_to_world(&S.camera, S.viewport, p); }
float r2d_px(float pixels) { return pixels / S.camera.zoom; }

/* ---------------------------------------------------------------------------
 * drawing
 * ------------------------------------------------------------------------- */
void r2d_set_space(r2d_space space) {
    S.space = space;
    if (!S.valid) return;
    bool scene = false;
    if (S.post_frame && S.gl_scene.id) {
        const r2d_post_params *p = r2d_post_params_ptr();
        scene = !(space == R2D_SPACE_SCREEN || space == R2D_SPACE_HUD) || (p && p->post.apply_to_ui);
    }
    sgl_set_context(scene ? S.gl_scene : sgl_default_context());
    sdtx_set_context(scene ? S.text_scene : sdtx_default_context());
    int layer = space == R2D_SPACE_WORLD ? 0 : (space == R2D_SPACE_SCREEN ? 1 : (space == R2D_SPACE_HUD ? 3 : 2));
    sgl_layer(layer);
    sdtx_layer(layer);
}
r2d_space r2d_get_space(void) { return S.space; }

static void emit_circle(r2d_vec2 c, float r, r2d_color color) {
    int n = circle_segments(r);
    sgl_begin_triangles();
    set_color(color);
    for (int i = 0; i < n; ++i) {
        float a0 = (float)i * 2.0f * R2D_PI / (float)n, a1 = (float)(i + 1) * 2.0f * R2D_PI / (float)n;
        sgl_v2f(c.x, c.y);
        sgl_v2f(c.x + cosf(a0) * r, c.y + sinf(a0) * r);
        sgl_v2f(c.x + cosf(a1) * r, c.y + sinf(a1) * r);
    }
    sgl_end();
}
static void emit_arc(r2d_vec2 c, float r, float thickness, float a_start, float a_end, r2d_color color) {
    float inner = r - thickness * 0.5f, outer = r + thickness * 0.5f;
    if (inner < 0) inner = 0;
    float span = a_end - a_start;
    int n = (int)((float)circle_segments(outer) * fabsf(span) / (2.0f * R2D_PI)) + 1;
    if (n < 2) n = 2;
    sgl_begin_triangle_strip();
    set_color(color);
    for (int i = 0; i <= n; ++i) {
        float a = a_start + span * (float)i / (float)n;
        float ca = cosf(a), sa = sinf(a);
        sgl_v2f(c.x + ca * outer, c.y + sa * outer);
        sgl_v2f(c.x + ca * inner, c.y + sa * inner);
    }
    sgl_end();
}
static void emit_quad(r2d_vec2 a, r2d_vec2 b, r2d_vec2 c, r2d_vec2 d, r2d_color color) {
    sgl_begin_quads();
    set_color(color);
    sgl_v2f(a.x, a.y);
    sgl_v2f(b.x, b.y);
    sgl_v2f(c.x, c.y);
    sgl_v2f(d.x, d.y);
    sgl_end();
}
static void emit_rect(r2d_rect r, r2d_color color) {
    emit_quad(r2d_v2(r.x, r.y), r2d_v2(r.x + r.w, r.y), r2d_v2(r.x + r.w, r.y + r.h), r2d_v2(r.x, r.y + r.h), color);
}
static void emit_segment(r2d_vec2 a, r2d_vec2 b, float thickness, r2d_color color) {
    r2d_vec2 d = r2d_sub(b, a);
    float len = r2d_length(d);
    if (thickness < 1.0f) thickness = 1.0f;
    if (len <= 0) { emit_circle(a, thickness * 0.5f, color); return; }
    r2d_vec2 n = r2d_scale(r2d_v2(-d.y / len, d.x / len), thickness * 0.5f);
    emit_quad(r2d_add(a, n), r2d_add(b, n), r2d_sub(b, n), r2d_sub(a, n), color);
}

void r2d_draw_circle(r2d_vec2 center, float radius, r2d_color color) {
    if (!S.valid) return;
    r2d_vec2 c = to_screen(center);
    float r = to_screen_len(radius);
    if (!on_screen(r2d_rect_centered(c, 2 * r, 2 * r))) return;
    emit_circle(c, r, color);
}
void r2d_draw_ring(r2d_vec2 center, float radius, float thickness, r2d_color color) {
    r2d_draw_arc(center, radius, thickness, 0, 2.0f * R2D_PI, color);
}
void r2d_draw_arc(r2d_vec2 center, float radius, float thickness, float start, float end, r2d_color color) {
    if (!S.valid) return;
    r2d_vec2 c = to_screen(center);
    float r = to_screen_len(radius), t = to_screen_len(thickness);
    if (t < 1.0f) t = 1.0f;
    if (!on_screen(r2d_rect_centered(c, 2 * (r + t), 2 * (r + t)))) return;
    if (!in_screen_space() && S.camera.y_up) { start = -start; end = -end; }
    emit_arc(c, r, t, start, end, color);
}
void r2d_draw_rect(r2d_rect rect, r2d_color color) {
    if (!S.valid) return;
    r2d_rect r = to_screen_rect(rect);
    if (!on_screen(r)) return;
    emit_rect(r, color);
}
void r2d_draw_rect_outline(r2d_rect rect, float thickness, r2d_color color) {
    if (!S.valid) return;
    r2d_rect r = to_screen_rect(rect);
    float t = to_screen_len(thickness);
    if (t < 1.0f) t = 1.0f;
    if (!on_screen(r2d_rect_expand(r, t))) return;
    float h = t * 0.5f;
    emit_rect(r2d_rect_make(r.x - h, r.y - h, r.w + t, t), color);
    emit_rect(r2d_rect_make(r.x - h, r.y + r.h - h, r.w + t, t), color);
    emit_rect(r2d_rect_make(r.x - h, r.y + h, t, r.h - t), color);
    emit_rect(r2d_rect_make(r.x + r.w - h, r.y + h, t, r.h - t), color);
}
void r2d_draw_line(r2d_vec2 a, r2d_vec2 b, float thickness, r2d_color color) {
    if (!S.valid) return;
    r2d_vec2 sa = to_screen(a), sb = to_screen(b);
    float t = to_screen_len(thickness);
    if (!on_screen(r2d_rect_expand(r2d_rect_from_points(sa, sb), t))) return;
    emit_segment(sa, sb, t, color);
}
void r2d_draw_polyline(const r2d_vec2 *points, size_t count, float thickness, bool closed, r2d_color color) {
    if (!S.valid || !points || count < 2) return;
    for (size_t i = 0; i + 1 < count; ++i) r2d_draw_line(points[i], points[i + 1], thickness, color);
    if (closed) r2d_draw_line(points[count - 1], points[0], thickness, color);
}
void r2d_draw_polygon(const r2d_vec2 *points, size_t count, r2d_color color) {
    if (!S.valid || !points || count < 3) return;
    r2d_vec2 first = to_screen(points[0]);
    sgl_begin_triangles();
    set_color(color);
    r2d_vec2 prev = to_screen(points[1]);
    for (size_t i = 2; i < count; ++i) {
        r2d_vec2 p = to_screen(points[i]);
        sgl_v2f(first.x, first.y);
        sgl_v2f(prev.x, prev.y);
        sgl_v2f(p.x, p.y);
        prev = p;
    }
    sgl_end();
}
void r2d_draw_triangle(r2d_vec2 a, r2d_vec2 b, r2d_vec2 c, r2d_color color) {
    r2d_vec2 pts[3] = {a, b, c};
    r2d_draw_polygon(pts, 3, color);
}
void r2d_draw_oriented_rect(r2d_vec2 center, r2d_vec2 direction, float width, float height, r2d_color color) {
    r2d_vec2 f = r2d_normalize(direction);
    if (f.x == 0 && f.y == 0) f = r2d_v2(1, 0);
    r2d_vec2 s = r2d_v2(-f.y, f.x);
    r2d_vec2 fw = r2d_scale(f, width * 0.5f), sh = r2d_scale(s, height * 0.5f);
    r2d_vec2 pts[4] = {
        r2d_add(center, r2d_add(fw, sh)), r2d_add(center, r2d_sub(fw, sh)),
        r2d_sub(center, r2d_add(fw, sh)), r2d_sub(center, r2d_sub(fw, sh))
    };
    r2d_draw_polygon(pts, 4, color);
}
void r2d_draw_grid(r2d_rect bounds, float spacing, float min_pixels, r2d_color color) {
    if (!S.valid || spacing <= 0) return;
    r2d_space previous = S.space;
    r2d_set_space(R2D_SPACE_WORLD);
    if (min_pixels < 1) min_pixels = 1;
    static const float steps[] = {2.0f, 2.5f, 2.0f};   /* 1, 2, 5, 10, 20, 50 ... */
    int step = 0;
    while (spacing * S.camera.zoom < min_pixels) { spacing *= steps[step % 3]; ++step; }
    r2d_rect v = r2d_visible_world();
    if (bounds.w > 0 && bounds.h > 0) {
        float x0 = v.x > bounds.x ? v.x : bounds.x, y0 = v.y > bounds.y ? v.y : bounds.y;
        float x1 = v.x + v.w < bounds.x + bounds.w ? v.x + v.w : bounds.x + bounds.w;
        float y1 = v.y + v.h < bounds.y + bounds.h ? v.y + v.h : bounds.y + bounds.h;
        if (x1 <= x0 || y1 <= y0) { r2d_set_space(previous); return; }
        v = r2d_rect_make(x0, y0, x1 - x0, y1 - y0);
    }
    float x0 = ceilf(v.x / spacing) * spacing, x1 = v.x + v.w;
    float y0 = ceilf(v.y / spacing) * spacing, y1 = v.y + v.h;
    sgl_begin_lines();
    set_color(color);
    for (float x = x0; x <= x1; x += spacing) {
        r2d_vec2 a = to_screen(r2d_v2(x, v.y)), b = to_screen(r2d_v2(x, y1));
        sgl_v2f(a.x, a.y);
        sgl_v2f(b.x, b.y);
    }
    for (float y = y0; y <= y1; y += spacing) {
        r2d_vec2 a = to_screen(r2d_v2(v.x, y)), b = to_screen(r2d_v2(x1, y));
        sgl_v2f(a.x, a.y);
        sgl_v2f(b.x, b.y);
    }
    sgl_end();
    r2d_set_space(previous);
}
void r2d_draw_bar(r2d_rect rect, float fraction, r2d_color fill, r2d_color background) {
    if (!S.valid) return;
    r2d_rect r = to_screen_rect(rect);
    if (!on_screen(r)) return;
    fraction = r2d_clampf(fraction, 0, 1);
    emit_rect(r, background);
    if (fraction > 0) emit_rect(r2d_rect_make(r.x, r.y, r.w * fraction, r.h), fill);
}

static void emit_text(r2d_vec2 screen, r2d_color color, const char *text, size_t max_len) {
    float ts = text_scale();
    float cell = R2D_GLYPH * ts;
    size_t len = strlen(text);
    if (len > max_len) len = max_len;
    if (!on_screen(r2d_rect_make(screen.x, screen.y, (float)len * cell, cell))) return;
    sdtx_pos(screen.x / cell, screen.y / cell);
    sdtx_color4b(color.r, color.g, color.b, color.a);
    sdtx_putr(text, (int)len);
}
void r2d_draw_text(r2d_vec2 pos, r2d_color color, const char *text) {
    if (!S.valid || !text) return;
    emit_text(to_screen(pos), color, text, R2D_MAX_TEXT);
}
void r2d_draw_textf(r2d_vec2 pos, r2d_color color, const char *fmt, ...) {
    char buffer[R2D_MAX_TEXT];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    r2d_draw_text(pos, color, buffer);
}
void r2d_draw_text_centered(r2d_vec2 pos, r2d_color color, const char *text) {
    if (!S.valid || !text) return;
    r2d_vec2 s = to_screen(pos);
    s.x -= r2d_text_width(text) * 0.5f;
    s.y -= r2d_text_height() * 0.5f;
    emit_text(s, color, text, R2D_MAX_TEXT);
}
float r2d_text_width(const char *text) { return text ? (float)strlen(text) * R2D_GLYPH * text_scale() : 0; }
float r2d_text_height(void) { return R2D_GLYPH * text_scale(); }
void r2d_draw_text_scaled(r2d_vec2 pos, r2d_color color, float scale, const char *text) {
    if (!S.valid || !text || !(scale > 0)) return;
    float ts = scale, cell = R2D_GLYPH * ts;   /* absolute: 1 is the 8-pixel font, whatever the UI text scale */
    r2d_vec2 screen = to_screen(pos);
    size_t len = strlen(text);
    if (len > R2D_MAX_TEXT) len = R2D_MAX_TEXT;
    if (!on_screen(r2d_rect_make(screen.x, screen.y, (float)len * cell, cell))) return;
    /* debugtext lays glyphs out against the canvas size at put time, so a
     * temporary canvas scales this run only */
    sdtx_canvas(S.viewport.x / ts, S.viewport.y / ts);
    sdtx_pos(screen.x / cell, screen.y / cell);
    sdtx_color4b(color.r, color.g, color.b, color.a);
    sdtx_putr(text, (int)len);
    sdtx_canvas(S.viewport.x / text_scale(), S.viewport.y / text_scale());
}
float r2d_text_width_scaled(float scale, const char *text) { return text && scale > 0 ? (float)strlen(text) * R2D_GLYPH * scale : 0; }
float r2d_text_height_scaled(float scale) { return scale > 0 ? R2D_GLYPH * scale : 0; }

/* ---------------------------------------------------------------------------
 * picking
 * ------------------------------------------------------------------------- */
static pickable *push_pick(uint64_t id) {
    S.picks = grow(S.picks, &S.pick_capacity, S.pick_count + 1, sizeof(*S.picks));
    pickable *p = &S.picks[S.pick_count++];
    memset(p, 0, sizeof(*p));
    p->id = id;
    return p;
}
void r2d_pick_circle(uint64_t id, r2d_vec2 center, float radius) {
    if (id == R2D_ID_NONE) return;
    pickable *p = push_pick(id);
    p->kind = PICK_CIRCLE;
    p->center = to_screen(center);
    p->radius = to_screen_len(radius);
}
void r2d_pick_rect(uint64_t id, r2d_rect rect) {
    if (id == R2D_ID_NONE) return;
    pickable *p = push_pick(id);
    p->kind = PICK_RECT;
    p->rect = to_screen_rect(rect);
}
static bool pick_hit_point(const pickable *p, r2d_vec2 s, float slop) {
    if (p->kind == PICK_CIRCLE) return r2d_distance(p->center, s) <= p->radius + slop;
    return r2d_rect_contains(r2d_rect_expand(p->rect, slop), s);
}
static bool pick_hit_rect(const pickable *p, r2d_rect r) {
    if (p->kind == PICK_CIRCLE) return r2d_rect_overlaps_circle(r, p->center, p->radius);
    return r2d_rect_overlaps(p->rect, r);
}
uint64_t r2d_pick_at(r2d_vec2 screen) {
    for (size_t i = S.pick_count; i-- > 0;)
        if (pick_hit_point(&S.picks[i], screen, S.desc.pick_slop)) return S.picks[i].id;
    return R2D_ID_NONE;
}
size_t r2d_pick_in_rect(r2d_rect screen, uint64_t *out, size_t capacity) {
    size_t n = 0;
    for (size_t i = 0; i < S.pick_count; ++i) {
        if (!pick_hit_rect(&S.picks[i], screen)) continue;
        if (out && n < capacity) out[n] = S.picks[i].id;
        ++n;
    }
    return n;
}
uint64_t r2d_hovered(void) { return point_over_ui(S.mouse) ? R2D_ID_NONE : r2d_pick_at(S.mouse); }

/* ---------------------------------------------------------------------------
 * selection set (sorted, unique)
 * ------------------------------------------------------------------------- */
static bool selection_find(uint64_t id, size_t *index) {
    size_t lo = 0, hi = S.selected_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (S.selected[mid] < id) lo = mid + 1; else hi = mid;
    }
    *index = lo;
    return lo < S.selected_count && S.selected[lo] == id;
}
size_t r2d_selection_count(void) { return S.selected_count; }
const uint64_t *r2d_selection(size_t *count) {
    if (count) *count = S.selected_count;
    return S.selected;
}
bool r2d_is_selected(uint64_t id) { size_t i; return selection_find(id, &i); }
bool r2d_select_add(uint64_t id) {
    size_t at;
    if (id == R2D_ID_NONE || selection_find(id, &at)) return false;
    S.selected = grow(S.selected, &S.selected_capacity, S.selected_count + 1, sizeof(*S.selected));
    memmove(S.selected + at + 1, S.selected + at, (S.selected_count - at) * sizeof(*S.selected));
    S.selected[at] = id;
    ++S.selected_count;
    return true;
}
bool r2d_select_remove(uint64_t id) {
    size_t at;
    if (!selection_find(id, &at)) return false;
    memmove(S.selected + at, S.selected + at + 1, (S.selected_count - at - 1) * sizeof(*S.selected));
    --S.selected_count;
    return true;
}
void r2d_select_set(uint64_t id) { S.selected_count = 0; r2d_select_add(id); }
void r2d_select_toggle(uint64_t id) { if (!r2d_select_remove(id)) r2d_select_add(id); }
void r2d_select_clear(void) { S.selected_count = 0; }
size_t r2d_selection_prune(bool (*keep)(uint64_t id, void *user), void *user) {
    size_t kept = 0, removed = 0;
    if (!keep) return 0;
    for (size_t i = 0; i < S.selected_count; ++i) {
        if (keep(S.selected[i], user)) S.selected[kept++] = S.selected[i]; else ++removed;
    }
    S.selected_count = kept;
    return removed;
}

/* ---------------------------------------------------------------------------
 * selection controller
 * ------------------------------------------------------------------------- */
static bool additive_modifier(void) { return (S.modifiers & (SAPP_MODIFIER_SHIFT | SAPP_MODIFIER_CTRL)) != 0; }
static void draw_selection_box(r2d_rect box) {
    r2d_space previous = S.space;
    r2d_set_space(R2D_SPACE_SCREEN);
    r2d_draw_rect(box, S.desc.style.selection_box);
    r2d_draw_rect_outline(box, 1, S.desc.style.selection_box_border);
    r2d_set_space(previous);
}
void r2d_selection_update(r2d_selection_result *out, bool enabled) {
    r2d_selection_result r;
    memset(&r, 0, sizeof(r));
    r.clicked = r.hovered = r.drag_id = R2D_ID_NONE;
    if (!S.valid || !enabled) { S.mode = SEL_IDLE; if (out) *out = r; return; }
    r.hovered = r2d_hovered();
    r2d_vec2 mouse_world = r2d_mouse_world();

    if (S.desc.escape_clears_selection && r2d_key_pressed(SAPP_KEYCODE_ESCAPE) && S.selected_count) {
        r2d_select_clear();
        r.changed = true;
    }
    /* press */
    if (S.mode == SEL_IDLE && S.button_pressed[R2D_MOUSE_LEFT] && !point_over_ui(S.mouse) && !S.panning) {
        S.press_screen = S.mouse;
        S.press_world = mouse_world;
        S.last_drag_world = mouse_world;
        S.press_additive = additive_modifier();
        S.press_id = r2d_pick_at(S.mouse);
        if (S.press_id != R2D_ID_NONE) {
            S.mode = SEL_PRESS_ITEM;
            S.press_was_selected = r2d_is_selected(S.press_id);
            if (S.press_additive) { r2d_select_toggle(S.press_id); r.changed = true; }
            else if (!S.press_was_selected) { r2d_select_set(S.press_id); r.changed = true; }
        } else {
            S.mode = SEL_PRESS_EMPTY;
        }
    }
    /* motion */
    if ((S.mode == SEL_PRESS_ITEM || S.mode == SEL_PRESS_EMPTY) && S.button_down[R2D_MOUSE_LEFT] &&
        r2d_distance(S.mouse, S.press_screen) > S.desc.drag_threshold) {
        if (S.mode == SEL_PRESS_ITEM) {
            if (r2d_is_selected(S.press_id)) S.mode = SEL_DRAG_ITEM;
        } else {
            S.mode = SEL_DRAG_BOX;
        }
    }
    if (S.mode == SEL_DRAG_ITEM) {
        r.dragging = true;
        r.drag_id = S.press_id;
        r.drag_delta = r2d_sub(mouse_world, S.last_drag_world);
        r.drag_total = r2d_sub(mouse_world, S.press_world);
        S.last_drag_world = mouse_world;
    }
    if (S.mode == SEL_DRAG_BOX) {
        r.box_active = true;
        r.box = r2d_rect_from_points(S.press_screen, S.mouse);
        if (S.desc.draw_selection_box) draw_selection_box(r.box);
    }
    /* release */
    if (S.mode != SEL_IDLE && (S.button_released[R2D_MOUSE_LEFT] || !S.button_down[R2D_MOUSE_LEFT])) {
        switch (S.mode) {
            case SEL_PRESS_ITEM:
                r.clicked = S.press_id;
                r.click_world = S.press_world;
                if (!S.press_additive && S.press_was_selected && S.selected_count != 1) {
                    r2d_select_set(S.press_id);
                    r.changed = true;
                }
                break;
            case SEL_PRESS_EMPTY:
                r.clicked_empty = true;
                r.click_world = S.press_world;
                if (!S.press_additive && S.selected_count) { r2d_select_clear(); r.changed = true; }
                break;
            case SEL_DRAG_ITEM:
                r.drag_ended = true;
                r.drag_id = S.press_id;
                r.drag_total = r2d_sub(mouse_world, S.press_world);
                break;
            case SEL_DRAG_BOX: {
                r2d_rect box = r2d_rect_from_points(S.press_screen, S.mouse);
                size_t n = r2d_pick_in_rect(box, NULL, 0);
                uint64_t *ids = n ? malloc(n * sizeof(*ids)) : NULL;
                if (ids) r2d_pick_in_rect(box, ids, n);
                size_t before = S.selected_count;
                if (!S.press_additive) r2d_select_clear();
                for (size_t i = 0; ids && i < n; ++i) if (r2d_select_add(ids[i])) r.changed = true;
                if (!S.press_additive && (before != S.selected_count || n)) r.changed = true;
                free(ids);
                r.box_active = false;
                break;
            }
            default: break;
        }
        S.mode = SEL_IDLE;
    }
    if (out) *out = r;
}

/* ---------------------------------------------------------------------------
 * ui
 * ------------------------------------------------------------------------- */
static void ui_register(r2d_rect rect) { rect_list_push(&S.ui_rects[S.frame & 1], rect); }
static void ui_text_in(r2d_rect rect, r2d_color color, const char *text, bool centered) {
    float pad = 4.0f, cell = R2D_GLYPH * text_scale();
    size_t max_len = rect.w > 2 * pad ? (size_t)((rect.w - 2 * pad) / cell) : 0;
    size_t len = strlen(text);
    if (len > max_len) len = max_len;
    float x = centered ? rect.x + (rect.w - (float)len * cell) * 0.5f : rect.x + pad;
    float y = rect.y + (rect.h - cell) * 0.5f;
    r2d_space previous = S.space;
    r2d_set_space(R2D_SPACE_SCREEN);
    emit_text(r2d_v2(x, y), color, text, len);
    r2d_set_space(previous);
}
static bool ui_widget(r2d_rect rect, const char *label, r2d_color base, bool *hovered_out) {
    ui_register(rect);
    bool hovered = r2d_rect_contains(rect, S.mouse);
    bool held = hovered && S.button_down[R2D_MOUSE_LEFT] && S.ui_press_valid && r2d_rect_contains(rect, S.ui_press);
    bool fired = hovered && S.button_released[R2D_MOUSE_LEFT] && S.ui_press_valid && r2d_rect_contains(rect, S.ui_press);
    const r2d_ui_style *st = &S.desc.style;
    r2d_color fill = held ? st->button_active : (hovered ? r2d_color_mix(base, st->button_hover, 0.5f) : base);
    r2d_space previous = S.space;
    r2d_set_space(R2D_SPACE_SCREEN);
    r2d_draw_rect(rect, fill);
    r2d_draw_rect_outline(rect, 1, st->panel_border);
    r2d_set_space(previous);
    if (label) ui_text_in(rect, st->button_text, label, true);
    if (hovered_out) *hovered_out = hovered;
    return fired;
}
void r2d_ui_panel(r2d_rect rect) {
    if (!S.valid) return;
    ui_register(rect);
    r2d_space previous = S.space;
    r2d_set_space(R2D_SPACE_SCREEN);
    r2d_draw_rect(rect, S.desc.style.panel);
    r2d_draw_rect_outline(rect, 1, S.desc.style.panel_border);
    r2d_set_space(previous);
}
void r2d_ui_block(r2d_rect rect) { ui_register(rect); }
void r2d_ui_label_colored(r2d_vec2 pos, r2d_color color, const char *text) {
    if (!S.valid || !text) return;
    r2d_space previous = S.space;
    r2d_set_space(R2D_SPACE_SCREEN);
    emit_text(pos, color, text, R2D_MAX_TEXT);
    r2d_set_space(previous);
}
void r2d_ui_label(r2d_vec2 pos, const char *text) { r2d_ui_label_colored(pos, S.desc.style.text, text); }
void r2d_ui_label_dim(r2d_vec2 pos, const char *text) { r2d_ui_label_colored(pos, S.desc.style.text_dim, text); }
void r2d_ui_labelf(r2d_vec2 pos, const char *fmt, ...) {
    char buffer[R2D_MAX_TEXT];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    r2d_ui_label(pos, buffer);
}
bool r2d_ui_button(r2d_rect rect, const char *label) {
    if (!S.valid) return false;
    return ui_widget(rect, label, S.desc.style.button, NULL);
}
bool r2d_ui_button_colored(r2d_rect rect, const char *label, r2d_color color) {
    if (!S.valid) return false;
    return ui_widget(rect, label, color, NULL);
}
bool r2d_ui_toggle(r2d_rect rect, const char *label, bool *value) {
    if (!S.valid || !value) return false;
    bool fired = ui_widget(rect, label, *value ? S.desc.style.toggle_on : S.desc.style.button, NULL);
    if (fired) *value = !*value;
    return fired;
}
void r2d_ui_bar(r2d_rect rect, float fraction, r2d_color fill) {
    if (!S.valid) return;
    ui_register(rect);
    r2d_space previous = S.space;
    r2d_set_space(R2D_SPACE_SCREEN);
    r2d_draw_bar(rect, fraction, fill, S.desc.style.button);
    r2d_draw_rect_outline(rect, 1, S.desc.style.panel_border);
    r2d_set_space(previous);
}
const r2d_ui_style *r2d_ui_style_get(void) { return &S.desc.style; }
r2d_stats r2d_get_stats(void) { return S.stats; }
