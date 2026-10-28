/* The Clockwork launcher: the engine's front door. It lists the projects it
 * knows (registry plus everything under games/), shows each one's state,
 * creates new projects from the starter template, builds them, and opens
 * them in the editor or runs them in the player. It contains no game code
 * and no editing: it starts the other tools.
 *
 * Usage:
 *   clockwork_launcher [--engine ROOT] [--frames N]
 *
 * Registry: <engine_root>/projects.cwlist (CW_PROJECTS overrides); the
 * editor and player are expected next to the launcher (CW_EDITOR and
 * CW_PLAYER override). The panels (projects, details, new project, build
 * log) are dockable tabs (runtime/host_dock.h); the layout is saved next to
 * the editor preferences (CW_EDITOR_CONFIG's directory, or ~/.config/clockwork).
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "launcher_core.h"
#include "host_dock.h"
#include "host_ui.h"
#include "render2d.h"
#include "sokol_log.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HEADER_H 56.0f
#define STATUS_H 44.0f
#define ROW_H 22.0f

static struct {
    lc_launcher *l;
    bool ready, failed;
    unsigned frames, max_frames;
    int selected;                 /* index into the registry, -1 for none */
    int scroll;
    hd_dock dock;
    char layout_path[512];
    char new_name[64], new_dir[CW_GAME_PATH_CAPACITY], add_path[CW_GAME_PATH_CAPACITY];
    float last_click_time;
    int last_click_index;
    float time;
} ui;

static void default_layout(void);
static void select_index(int index) { ui.selected = index; }
static void format_time(time_t t, char *out, size_t capacity) {
    if (!t) { snprintf(out, capacity, "-"); return; }
    struct tm *tm = localtime(&t);
    if (!tm) { snprintf(out, capacity, "-"); return; }
    strftime(out, capacity, "%Y-%m-%d %H:%M", tm);
}
static void open_selected(const char *scene) {
    if (ui.selected < 0) return;
    if (lc_open_in_editor(ui.l, (size_t)ui.selected, scene)) select_index(0);   /* it moved to the top */
}
static void run_selected(const char *scene) {
    if (ui.selected < 0) return;
    if (lc_run(ui.l, (size_t)ui.selected, scene)) select_index(0);
}
static void build_selected(void) {
    if (ui.selected < 0) return;
    if (!lc_build(ui.l, (size_t)ui.selected)) hd_show(&ui.dock, "log");
    lc_save(ui.l);
}
static void create_project(void) {
    size_t index;
    if (!lc_create(ui.l, ui.new_dir, ui.new_name, &index)) return;
    select_index((int)index);
    hd_focus(&ui.dock, "details");
    if (lc_build(ui.l, index)) lc_status(ui.l, false, "created and built %s: open it in the editor", ui.l->entries[index].project.name);
    else hd_show(&ui.dock, "log");
}

/* --- panels --- */
static void draw_header(void) {
    r2d_vec2 v = r2d_viewport();
    r2d_ui_panel(r2d_rect_make(0, 0, v.x, HEADER_H));
    r2d_ui_label_colored(r2d_v2(16, 12), hui_white, "CLOCKWORK");
    r2d_ui_label_dim(r2d_v2(16, 30), "projects");
    float x = 130, y = 16, h = 24;
    if (hui_button(r2d_rect_make(x, y, 110, h), "new project", false)) { ui.new_name[0] = 0; hd_show(&ui.dock, "new"); }
    x += 118;
    if (hui_button(r2d_rect_make(x, y, 70, h), "refresh", false)) { lc_load(ui.l); if (ui.selected >= (int)ui.l->count) ui.selected = -1; }
    x += 78;
    r2d_ui_label_dim(r2d_v2(x, y + 6), "add existing:");
    x += 96;
    float field_w = v.x - x - 70 - 16 - 116;
    if (hui_button(r2d_rect_make(v.x - 112, y, 100, h), "reset layout", false)) { hd_clear(&ui.dock); default_layout(); }
    if (field_w > 120) {
        bool commit = hui_textfield(r2d_rect_make(x, y, field_w, h), &ui.add_path, ui.add_path, sizeof(ui.add_path));
        if ((commit || hui_button(r2d_rect_make(x + field_w + 6, y, 60, h), "add", false)) && ui.add_path[0]) {
            int at = lc_add(ui.l, ui.add_path);
            if (at >= 0) {
                lc_touch(ui.l, (size_t)at);
                lc_save(ui.l);
                select_index(0);
                lc_status(ui.l, false, "added %s", ui.l->entries[0].loadable ? ui.l->entries[0].project.name : ui.add_path);
                ui.add_path[0] = 0;
            }
        }
    }
}
static void panel_projects(r2d_rect panel, void *user) {
    (void)user;
    hui_layout l = {panel.x + 10, panel.y + 8, panel.w - 20};
    char text[160];
    snprintf(text, sizeof(text), "PROJECTS (%zu)  - most recent first", ui.l->count);
    hui_heading(&l, text);
    if (!ui.l->count) {
        hui_line(&l, hui_dim, "no projects yet: create one, or add an");
        hui_line(&l, hui_dim, "existing .cwproj file above");
        return;
    }
    float row_h = 46;
    int visible = (int)((panel.y + panel.h - 8 - l.y) / (row_h + 4));
    if (visible < 1) visible = 1;
    if (r2d_rect_contains(panel, r2d_mouse()) && r2d_wheel() != 0) ui.scroll -= (int)r2d_wheel();
    if (ui.scroll > (int)ui.l->count - visible) ui.scroll = (int)ui.l->count - visible;
    if (ui.scroll < 0) ui.scroll = 0;
    for (int i = ui.scroll; i < (int)ui.l->count && i - ui.scroll < visible; ++i) {
        const lc_entry *e = &ui.l->entries[i];
        r2d_rect r = hui_row(&l, row_h);
        bool selected = ui.selected == i, hover = r2d_rect_contains(r, r2d_mouse());
        r2d_draw_rect(r, selected ? r2d_rgb(64, 74, 96) : (hover ? r2d_rgb(50, 56, 68) : r2d_rgb(44, 48, 58)));
        r2d_ui_block(r);
        const char *name = e->loadable ? e->project.name : "(unreadable)";
        r2d_draw_text(r2d_v2(r.x + 10, r.y + 7), e->loadable ? hui_white : hui_red, name);
        char info[160], when[32];
        format_time(e->modified, when, sizeof(when));
        if (e->loadable) snprintf(info, sizeof(info), "%s  %zu scene%s  %s", e->module_built ? "built" : "not built", e->scene_count, e->scene_count == 1 ? "" : "s", when);
        else snprintf(info, sizeof(info), "%s", e->error);
        r2d_draw_text(r2d_v2(r.x + 10, r.y + 26), e->loadable ? (e->module_built ? hui_green : hui_yellow) : hui_red, info);
        const char *path = e->path;
        size_t len = strlen(path), max = 34;
        char short_path[CW_GAME_PATH_CAPACITY + 4];
        if (len > max) snprintf(short_path, sizeof(short_path), "...%s", path + len - max + 3); else snprintf(short_path, sizeof(short_path), "%s", path);
        float pw = r2d_text_width(short_path);
        r2d_draw_text(r2d_v2(r.x + r.w - pw - 10, r.y + 7), hui_dim, short_path);
        if (hover && r2d_mouse_pressed(R2D_MOUSE_LEFT)) {
            bool double_click = ui.last_click_index == i && ui.time - ui.last_click_time < 0.6f;
            ui.last_click_index = i;
            ui.last_click_time = ui.time;
            select_index(i);
            if (double_click && e->loadable) open_selected(NULL);
        }
    }
    if ((int)ui.l->count > visible) {
        snprintf(text, sizeof(text), "%d-%d of %zu (wheel scrolls)", ui.scroll + 1, ui.scroll + visible < (int)ui.l->count ? ui.scroll + visible : (int)ui.l->count, ui.l->count);
        r2d_ui_label_dim(r2d_v2(l.x, panel.y + panel.h - r2d_text_height() - 8), text);
    }
}
static void panel_new(r2d_rect panel, void *user) {
    (void)user;
    hui_layout layout = {panel.x + 12, panel.y + 8, panel.w - 24}, *l = &layout;
    hui_heading(l, "NEW PROJECT");
    hui_line(l, hui_dim, "name (letters, digits, _):");
    r2d_rect r = hui_row(l, ROW_H);
    hui_textfield(r, &ui.new_name, ui.new_name, sizeof(ui.new_name));
    hui_line(l, hui_dim, "in directory:");
    r = hui_row(l, ROW_H);
    hui_textfield(r, &ui.new_dir, ui.new_dir, sizeof(ui.new_dir));
    char preview[CW_GAME_PATH_CAPACITY * 2 + 64];
    snprintf(preview, sizeof(preview), "-> %s/%s/%s.cwproj", ui.new_dir, ui.new_name[0] ? ui.new_name : "<name>", ui.new_name[0] ? ui.new_name : "<name>");
    hui_line(l, hui_dim, preview);
    hui_line(l, hui_dim, "creates the project file, a starter game module");
    hui_line(l, hui_dim, "(components, systems, prefabs, drawing) and a scene,");
    hui_line(l, hui_dim, "then builds the module so the editor can open it.");
    r = hui_row(l, 26);
    if (hui_button(hui_split(r, 0, 2), "create and build", lc_name_valid(ui.new_name))) create_project();
    if (hui_button(hui_split(r, 1, 2), "cancel", false)) hd_focus(&ui.dock, "details");
}
static void panel_details(r2d_rect panel, void *user) {
    (void)user;
    hui_layout l = {panel.x + 12, panel.y + 8, panel.w - 24};
    if (ui.selected < 0 || ui.selected >= (int)ui.l->count) {
        hui_heading(&l, "PROJECT");
        hui_line(&l, hui_dim, "select a project on the left (double-click opens it),");
        hui_line(&l, hui_dim, "or start a new one.");
        return;
    }
    const lc_entry *e = &ui.l->entries[ui.selected];
    char text[CW_GAME_PATH_CAPACITY + 64];
    if (!e->loadable) {
        hui_heading(&l, "PROJECT");
        hui_line(&l, hui_red, "this project file cannot be read:");
        hui_line(&l, hui_red, e->error);
        hui_line(&l, hui_dim, e->path);
        r2d_rect r = hui_row(&l, 26);
        if (hui_button(hui_split(r, 0, 3), "refresh", false)) lc_refresh_entry(&ui.l->entries[ui.selected]);
        if (r2d_ui_button_colored(hui_split(r, 2, 3), "remove from list", r2d_rgb(120, 48, 44))) { lc_remove(ui.l, (size_t)ui.selected); lc_save(ui.l); select_index(-1); lc_status(ui.l, false, "removed from the list (the files stay)"); }
        return;
    }
    hui_line(&l, hui_yellow, e->project.name);
    hui_line(&l, hui_dim, e->path);
    snprintf(text, sizeof(text), "module: %s  (%s)", e->module_built ? "built" : "not built", e->project.module + (strlen(e->project.module) > 60 ? strlen(e->project.module) - 60 : 0));
    hui_line(&l, e->module_built ? hui_green : hui_yellow, text);
    for (size_t i = 0; i < e->project.source_count; ++i) {
        snprintf(text, sizeof(text), "source: %s", e->project.sources[i]);
        hui_line(&l, hui_dim, text);
    }
    char when[32];
    format_time(e->modified, when, sizeof(when));
    snprintf(text, sizeof(text), "last change: %s", when);
    hui_line(&l, hui_dim, text);
    l.y += 4;
    r2d_rect r = hui_row(&l, 28);
    if (hui_button(hui_split(r, 0, 4), "open in editor", true)) open_selected(NULL);
    if (hui_button(hui_split(r, 1, 4), "run", false)) run_selected(NULL);
    if (hui_button(hui_split(r, 2, 4), e->module_built ? "rebuild" : "build", !e->module_built)) build_selected();
    if (r2d_ui_button_colored(hui_split(r, 3, 4), "remove from list", r2d_rgb(120, 48, 44))) { lc_remove(ui.l, (size_t)ui.selected); lc_save(ui.l); select_index(-1); lc_status(ui.l, false, "removed from the list (the files stay)"); return; }
    snprintf(text, sizeof(text), "SCENES (%zu)", e->scene_count);
    hui_heading(&l, text);
    if (!e->scene_count) hui_line(&l, hui_dim, "none yet: the editor's 'save as' creates them");
    for (size_t i = 0; i < e->scene_count && i < 12; ++i) {
        r = hui_row(&l, ROW_H);
        snprintf(text, sizeof(text), "%.*s", (int)(strlen(e->scenes[i]) - 8), e->scenes[i]);
        r2d_ui_label(r2d_v2(r.x + 4, r.y + 5), text);
        if (hui_button(r2d_rect_make(r.x + r.w - 124, r.y, 60, r.h), "edit", false)) open_selected(e->scenes[i]);
        if (hui_button(r2d_rect_make(r.x + r.w - 60, r.y, 60, r.h), "play", false)) run_selected(e->scenes[i]);
    }
}
static void panel_log(r2d_rect panel, void *user) {
    (void)user;
    hui_layout l = {panel.x + 12, panel.y + 8, panel.w - 24};
    hui_heading(&l, "BUILD LOG");
    const char *log = ui.l->log;
    float bottom = panel.y + panel.h - 8;
    int max_lines = (int)((bottom - l.y) / (r2d_text_height() + 2)), lines = 0;
    for (const char *p = log; *p; ++p) lines += *p == '\n';
    const char *start = log;
    for (int skip = lines - max_lines; skip > 0 && *start; --skip) { start = strchr(start, '\n'); start = start ? start + 1 : log + strlen(log); }
    size_t max_chars = (size_t)((panel.w - 24) / (r2d_text_width("M") > 0 ? r2d_text_width("M") : 8));
    while (*start && l.y + r2d_text_height() <= bottom) {
        const char *nl = strchr(start, '\n');
        size_t len = nl ? (size_t)(nl - start) : strlen(start);
        if (len > max_chars) len = max_chars;
        char line[200];
        snprintf(line, sizeof(line), "%.*s", (int)(len < 190 ? len : 190), start);
        r2d_ui_label_colored(r2d_v2(l.x, l.y), strstr(line, "error") ? hui_red : r2d_rgb(200, 205, 215), line);
        l.y += r2d_text_height() + 2;
        if (!nl) break;
        start = nl + 1;
    }
    if (!*log) hui_line(&l, hui_dim, "nothing built yet");
}
static void default_layout(void) {
    hd_dock *k = &ui.dock;
    hd_dock_panel(k, "details", HD_CENTER, 0, NULL);
    hd_dock_panel(k, "new", HD_CENTER, 0, "details");
    hd_dock_panel(k, "projects", HD_LEFT, 0.42f, NULL);
    hd_dock_panel(k, "log", HD_BOTTOM, 0.3f, "details");
    hd_close(k, "log");
    hd_focus(k, "details");
}
static void setup_dock(void) {
    hd_dock *k = &ui.dock;
    hd_init(k);
    hd_add_panel(k, "projects", "projects", panel_projects, NULL, false);
    hd_add_panel(k, "details", "project", panel_details, NULL, false);
    hd_add_panel(k, "new", "new project", panel_new, NULL, false);
    hd_add_panel(k, "log", "build log", panel_log, NULL, false);
    if (!hd_load(k, ui.layout_path)) default_layout();
}
static void draw_status(void) {
    r2d_vec2 v = r2d_viewport();
    r2d_rect bar = r2d_rect_make(0, v.y - STATUS_H, v.x, STATUS_H);
    r2d_ui_panel(bar);
    r2d_ui_label_colored(r2d_v2(14, bar.y + 8), ui.l->status_error ? hui_red : hui_green, ui.l->status);
    char line[CW_GAME_PATH_CAPACITY * 2 + 64];
    snprintf(line, sizeof(line), "registry %s   editor %s", ui.l->registry, ui.l->editor);
    r2d_ui_label_dim(r2d_v2(14, bar.y + 26), line);
}

/* --- window --- */
static void window_init(void) {
    r2d_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.text_scale = 1.0f;
    desc.no_post_process = true;   /* no scene to light or bloom here */
    r2d_setup(&desc);
    if (!r2d_valid()) { ui.failed = true; sapp_request_quit(); return; }
    ui.ready = true;
    ui.selected = ui.l->count ? 0 : -1;
    ui.last_click_index = -1;
    setup_dock();
}
static void window_frame(void) {
    if (!ui.ready) return;
    r2d_begin_frame();
    float dt = r2d_dt();
    ui.time += dt;
    hui_frame(dt);
    if (!hui_focused()) {
        if (r2d_key_pressed(SAPP_KEYCODE_ESCAPE)) sapp_request_quit();
        if (r2d_key_pressed(SAPP_KEYCODE_ENTER) && ui.selected >= 0) open_selected(NULL);
        if (r2d_key_pressed(SAPP_KEYCODE_N) && (r2d_modifiers() & SAPP_MODIFIER_CTRL)) { ui.new_name[0] = 0; hd_show(&ui.dock, "new"); }
        if (r2d_key_pressed(SAPP_KEYCODE_GRAVE_ACCENT)) { if (hd_is_open(&ui.dock, "log")) hd_close(&ui.dock, "log"); else hd_show(&ui.dock, "log"); }
        if (r2d_key_pressed(SAPP_KEYCODE_F5)) { lc_load(ui.l); if (ui.selected >= (int)ui.l->count) ui.selected = -1; }
        if (r2d_key_pressed(SAPP_KEYCODE_DOWN) && ui.selected + 1 < (int)ui.l->count) select_index(ui.selected + 1);
        if (r2d_key_pressed(SAPP_KEYCODE_UP) && ui.selected > 0) select_index(ui.selected - 1);
    }
    r2d_set_space(R2D_SPACE_SCREEN);
    draw_header();
    r2d_vec2 v = r2d_viewport();
    hd_frame(&ui.dock, r2d_rect_make(6, HEADER_H + 6, v.x - 12, v.y - HEADER_H - STATUS_H - 12));
    if (ui.dock.changed) { hd_save(&ui.dock, ui.layout_path); ui.dock.changed = false; }
    draw_status();
    r2d_end_frame();
    ++ui.frames;
    if (ui.max_frames && ui.frames >= ui.max_frames) sapp_request_quit();
}
static void window_event(const sapp_event *event) { if (ui.ready) r2d_handle_event(event); }
static void window_cleanup(void) {
    if (!ui.ready) return;
    r2d_shutdown();
    ui.ready = false;
}

int main(int argc, char **argv) {
    const char *engine_root = getenv("CW_ENGINE_ROOT");
#ifdef CW_ENGINE_ROOT
    if (!engine_root || !*engine_root) engine_root = CW_ENGINE_ROOT;
#endif
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--engine") == 0 && i + 1 < argc) engine_root = argv[++i];
        else if (strcmp(a, "--frames") == 0 && i + 1 < argc) {
            char *end;
            errno = 0;
            unsigned long n = strtoul(argv[++i], &end, 10);
            if (errno == ERANGE || *end || !n || n > UINT32_MAX) { fputs("--frames needs a positive integer\n", stderr); return 2; }
            ui.max_frames = (unsigned)n;
        } else {
            printf("usage: %s [--engine ROOT] [--frames N]\n", argv[0]);
            return strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0 ? 0 : 2;
        }
    }
    /* the editor and the player live next to the launcher */
    char tool_dir[CW_GAME_PATH_CAPACITY];
    snprintf(tool_dir, sizeof(tool_dir), "%s", argv[0]);
    char *slash = strrchr(tool_dir, '/');
#ifdef _WIN32
    char *bslash = strrchr(tool_dir, '\\');
    if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
    if (slash) slash[1] = 0; else tool_dir[0] = 0;
    ui.l = calloc(1, sizeof(*ui.l));
    if (!ui.l) return 1;
    lc_init(ui.l, engine_root, tool_dir);
    lc_load(ui.l);
    {   /* the dock layout lives with the editor preferences */
        const char *env = getenv("CW_LAUNCHER_LAYOUT"), *cfg = getenv("CW_EDITOR_CONFIG");
        if (env && *env) snprintf(ui.layout_path, sizeof(ui.layout_path), "%s", env);
        else if (cfg && *cfg) {
            snprintf(ui.layout_path, sizeof(ui.layout_path), "%s", cfg);
            char *slash = strrchr(ui.layout_path, '/');
            if (slash) snprintf(slash + 1, sizeof(ui.layout_path) - (size_t)(slash + 1 - ui.layout_path), "launcher_layout.cfg");
        } else {
#ifdef _WIN32
            const char *base = getenv("APPDATA");
            if (base && *base) snprintf(ui.layout_path, sizeof(ui.layout_path), "%s\\clockwork\\launcher_layout.cfg", base);
#else
            const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
            if (xdg && *xdg) snprintf(ui.layout_path, sizeof(ui.layout_path), "%s/clockwork/launcher_layout.cfg", xdg);
            else if (home && *home) snprintf(ui.layout_path, sizeof(ui.layout_path), "%s/.config/clockwork/launcher_layout.cfg", home);
#endif
        }
    }
    snprintf(ui.new_dir, sizeof(ui.new_dir), "%s/games", engine_root && *engine_root ? engine_root : ".");
    sapp_desc window;
    memset(&window, 0, sizeof(window));
    window.init_cb = window_init;
    window.frame_cb = window_frame;
    window.cleanup_cb = window_cleanup;
    window.event_cb = window_event;
    window.width = 1100;
    window.height = 680;
    window.sample_count = 4;
    window.high_dpi = true;
    window.window_title = "Clockwork launcher";
    window.logger.func = slog_func;
    sapp_run(&window);
    if (ui.max_frames && !ui.failed) printf("OK: launcher rendered %u frames\n", ui.frames);
    int result = ui.failed ? 1 : 0;
    free(ui.l);
    return result;
}
