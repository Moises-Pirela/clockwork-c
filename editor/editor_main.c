/* The Clockwork editor: a standalone tool that opens a game project, edits
 * its scenes and builds the game. It never contains game code: the game is
 * a module (shared library) loaded from the project, and everything the
 * editor shows comes from the ECS world bound to that module plus the
 * reflection the module registered.
 *
 * Usage:
 *   clockwork_editor [--project game.cwproj] [--scene file.cwscene]
 *                    [--engine ROOT] [--config tunable.cfg] [--frames N]
 *
 * Layout: palette, scenes and entity list on the left; the viewport in the
 * middle; the inspector on the right; a toolbar above and the status bar
 * below (` toggles the build log).
 *
 * Keys: Q select tool   1-9 module prefabs   E create an empty entity   click/drag select, shift adds, drag moves
 *       arrows nudge (shift x10)   Del delete   Ctrl+D duplicate   Ctrl+A all
 *       Ctrl+Z / Ctrl+Y undo/redo   Ctrl+S save   Ctrl+N new scene
 *       F5 play/stop   Space pause   N step   F7 build module   F8 export game   F9 run player
 *       F fit view   G grid   L preview the game's rendering   F10 settings   F11 fullscreen   Ctrl+Q quit   Esc stop/deselect
 *
 * Every panel is a dockable tab (runtime/host_dock.h): scene (palette,
 * scene files, entities), assets (the project's asset browser: import by
 * path or by dropping files on the window, scan, rename, remove), world (the
 * ECS itself: entity and component counts, the registered systems with
 * enable toggles, memory), inspector, project, rendering (every render2d
 * tunable, edited live and saved to the project's render.cfg), preferences
 * (the user's own settings, saved per user), log, animation (clips of
 * keyframes on the selected entity's fields: tracks, keys, scrubbing,
 * preview, record) and the viewport itself.
 * Drag a tab onto another panel to group them, onto an edge to split, drag
 * the gutters to resize, close with the x; the `panels` menu reopens them
 * and `reset layout` restores the default. The layout is saved per user.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#ifndef _WIN32
#define _DEFAULT_SOURCE 1
#endif
#include "editor_core.h"
#include "editor_settings.h"
#include "editor_anim.h"
#include "host_canvas.h"
#include "host_dock.h"
#include "host_ui.h"
#include "render2d.h"
#include "r2d_post.h"
#include "r2d_lighting.h"
#include "r2d_tunables.h"
#include "sokol_log.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOOLBAR_H 38.0f
#define STATUS_H 52.0f
#define ROW_H 22.0f

typedef enum { TOOL_SELECT, TOOL_PLACE, TOOL_PREFAB, TOOL_CREATE } Tool;
static struct {
    ed_document doc;
    host_canvas canvas;
    hd_dock dock;
    ea_state anim;                   /* the animation editor */
    char anim_name[64], anim_length[16], key_time[16], key_value[32];
    bool anim_add_open;
    int anim_drag_track, anim_drag_key;
    bool anim_scrubbing;
    bool panels_menu;
    float panels_menu_x;
    /* the material editor */
    r2d_material material, material_saved;
    bool material_open;
    uint32_t material_asset;
    char material_name[64], shader_name[64];
    int material_picker;             /* 0 none, 1 texture, 2 shader */
    int material_scroll;             /* the material column scrolls with the wheel */
    int material_preview;            /* 0 a sprite on a checkerboard, 1 an interface element reacting to the mouse */
    int shader_example;              /* which starter a new shader is written from */
    cw_ui_layout preview_state;      /* the element preview's eased hover/press */
    uint64_t shader_stamp;
    float shader_watch;
    char layout_path[512];
    bool ready, failed;
    unsigned frames, max_frames;
    char config[CW_GAME_PATH_CAPACITY];
    char project_arg[CW_GAME_PATH_CAPACITY], scene_arg[CW_GAME_PATH_CAPACITY];
    Tool tool;
    size_t prefab;                   /* module prefab index for TOOL_PLACE */
    uint32_t prefab_asset;           /* project prefab (asset id) for TOOL_PREFAB */
    char prefab_name[CW_ASSET_NAME_CAPACITY];
    bool grid, lights;
    int settings_section, settings_scroll;
    ed_settings prefs, prefs_saved;
    float autosave_timer;
    int asset_filter;                /* -1 all, else a cw_asset_type */
    int asset_scroll, world_scroll;
    uint32_t asset_selected;
    char import_path[CW_GAME_PATH_CAPACITY], asset_name[CW_ASSET_NAME_CAPACITY];
    const void *asset_picker;        /* inspector field whose asset list is open */
    r2d_post_params render_saved;    /* what the project's render file holds, for the dirty mark */
    bool render_loaded;
    char win_w[16], win_h[16], win_title[96];
    float fps, time;
    int entity_scroll, inspector_scroll;
    size_t field_ordinal;            /* inspector rows drawn this frame */
    uint64_t hovered;
    char save_name[128], open_path[CW_GAME_PATH_CAPACITY], name_field[CW_SCENE_NAME_CAPACITY];
    cw_entity_id name_field_id;
    char confirm[128];               /* scene name awaiting a second click while dirty */
    /* the interface editor: the game view (a resolution preset letterboxed in
     * the viewport, where the interface is laid out) and element drags */
    int screen_preset;               /* index into screen_presets; 0 is the whole viewport */
    struct { bool active; int handles; cw_entity_id id; r2d_vec2 last; bool moved; } ui_drag;   /* handles: bit 0 left, 1 top, 2 right, 3 bottom; 0 moves */
    cw_entity_id ui_hover;
    char ui_w[16], ui_h[16];
} ui;
static const struct { const char *name; float w, h; } screen_presets[] = {
    {"free", 0, 0}, {"1280x720", 1280, 720}, {"1920x1080", 1920, 1080}, {"800x600", 800, 600}, {"1080x1920", 1080, 1920}, {"2560x1080", 2560, 1080},
};
#define SCREEN_PRESETS ((int)(sizeof(screen_presets) / sizeof(screen_presets[0])))

#define yellow hui_yellow
#define white hui_white
#define red hui_red
#define green hui_green
#define accent hui_accent
#define plain hui_plain
typedef hui_layout Layout;
#define row hui_row
#define split hui_split
#define heading hui_heading
#define label_line hui_line
#define button hui_button
#define textfield hui_textfield
static void snap_selection(void);
static void default_layout(void);

/* ---------------------------------------------------------------------------
 * selection sync between the document and render2d's controller
 * ------------------------------------------------------------------------- */
static void selection_to_r2d(void) {
    ui.prefab_name[0] = 0;
    r2d_select_clear();
    for (size_t i = 0; i < ui.doc.selection_count; ++i) r2d_select_add((uint64_t)ui.doc.selection[i]);
}
static void selection_from_r2d(void) {
    ui.prefab_name[0] = 0;
    size_t n;
    const uint64_t *ids = r2d_selection(&n);
    ed_select_clear(&ui.doc);
    for (size_t i = 0; i < n; ++i) ed_select_add(&ui.doc, (cw_entity_id)ids[i]);
}
static bool keep_alive(uint64_t id, void *user) {
    (void)user;
    return ui.doc.world_ready && cw_world_is_valid(&ui.doc.gw.world, (cw_entity_id)id)
        && !(ui.doc.gw.world.entities[id].flags & CW_TO_DESTROY);
}

/* ---------------------------------------------------------------------------
 * viewport
 * ------------------------------------------------------------------------- */
static r2d_rect dock_area(void) {
    r2d_vec2 v = r2d_viewport();
    return r2d_rect_make(6, TOOLBAR_H + 6, v.x - 12, v.y - TOOLBAR_H - STATUS_H - 12);
}
static r2d_rect world_area(void) {
    r2d_rect r;
    if (hd_panel_rect(&ui.dock, "viewport", &r)) return r;
    return dock_area();
}
static r2d_rect level_bounds(void) {
    if (!ui.doc.world_ready) return r2d_rect_make(-400, -300, 800, 600);
    const float *b = ui.doc.gw.module->bounds;
    return b[2] > 0 && b[3] > 0 ? r2d_rect_make(b[0], b[1], b[2], b[3]) : r2d_rect_make(-400, -300, 800, 600);
}
static void fit_view(void) { r2d_camera_fit_rect_in(world_area(), level_bounds(), 24); }
/* The game view: where the interface is laid out. The whole viewport, or a
 * resolution preset's aspect letterboxed into it, to see the layout the
 * way a player with that screen would. */
static r2d_rect game_rect(void) {
    r2d_rect v = world_area();
    int p = ui.screen_preset >= 0 && ui.screen_preset < SCREEN_PRESETS ? ui.screen_preset : 0;
    if (!(screen_presets[p].w > 0) || v.w <= 0 || v.h <= 0) return v;
    float aspect = screen_presets[p].w / screen_presets[p].h;
    float w = v.w, h = v.w / aspect;
    if (h > v.h) { h = v.h; w = v.h * aspect; }
    return r2d_rect_make(v.x + (v.w - w) * 0.5f, v.y + (v.h - h) * 0.5f, w, h);
}
static cw_ui_screen game_screen(void) {
    r2d_rect g = game_rect();
    cw_ui_screen s = {g.x, g.y, g.w, g.h};
    return s;
}
static bool has_ui_elements(void) {
    ed_document *d = &ui.doc;
    return d->world_ready && cw_ui_ready(&d->gw.ui) && ed_component_entities(d, d->gw.ui.rect) > 0;
}
static r2d_rect ui_layout_rect(cw_entity_id id) {
    const cw_ui_layout *l = cw_comp_ptr(&ui.doc.gw.world, id, ui.doc.gw.ui.layout);
    return l ? r2d_rect_make(l->x, l->y, l->w, l->h) : r2d_rect_make(0, 0, 0, 0);
}
/* the eight resize handles of an element, as a bit set per edge */
static int ui_handle_at(r2d_rect r, r2d_vec2 m) {
    const float grab = 7;
    bool near_l = fabsf(m.x - r.x) <= grab, near_r = fabsf(m.x - (r.x + r.w)) <= grab, near_t = fabsf(m.y - r.y) <= grab, near_b = fabsf(m.y - (r.y + r.h)) <= grab;
    bool in_x = m.x >= r.x - grab && m.x <= r.x + r.w + grab, in_y = m.y >= r.y - grab && m.y <= r.y + r.h + grab;
    if (!in_x || !in_y) return 0;
    int h = 0;
    if (near_l) h |= 1;
    if (near_t) h |= 2;
    if (near_r) h |= 4;
    if (near_b) h |= 8;
    return h;
}
/* Selection, hover, anchors and handles of interface elements, in screen space over the game view. */
static void draw_ui_gizmos(void) {
    ed_document *d = &ui.doc;
    if (!d->world_ready || !cw_ui_ready(&d->gw.ui)) return;
    r2d_set_space(R2D_SPACE_SCREEN);
    r2d_rect g = game_rect();
    bool show_frame = ui.screen_preset != 0 || has_ui_elements();
    if (show_frame) {
        r2d_draw_rect_outline(g, 1, r2d_rgba(120, 200, 255, ui.screen_preset ? 160 : 70));
        if (ui.screen_preset) {
            char label[64];
            snprintf(label, sizeof(label), "%s  x%.2f", screen_presets[ui.screen_preset].name, (double)cw_ui_scale(&d->gw.ui_canvas, g.w, g.h));
            r2d_draw_text(r2d_v2(g.x + 6, g.y + g.h - r2d_text_height() - 4), r2d_rgba(120, 200, 255, 200), label);
        }
    }
    if (d->playing) return;   /* the interface is live: no editing marks */
    cw_world *w = &d->gw.world;
    const cw_ui_components *c = &d->gw.ui;
    if (ui.ui_hover != CW_INVALID_ENTITY && !ed_is_selected(d, ui.ui_hover) && ed_is_ui(d, ui.ui_hover))
        r2d_draw_rect_outline(ui_layout_rect(ui.ui_hover), 1, r2d_with_alpha(white, 150));
    for (size_t i = 0; i < d->selection_count; ++i) {
        cw_entity_id id = d->selection[i];
        if (!ed_is_ui(d, id) || !cw_comp_has(w, id, c->layout)) continue;
        r2d_rect r = ui_layout_rect(id);
        bool primary = id == ed_primary(d);
        r2d_draw_rect_outline(r, primary ? 2 : 1, yellow);
        if (!primary) continue;
        /* the parent's rect and the anchors on it */
        const cw_ui_rect *rect = cw_comp_ptr(w, id, c->rect);
        const cw_ui_layout *l = cw_comp_ptr(w, id, c->layout);
        r2d_rect pr = ed_is_ui(d, rect->parent) && rect->parent != id ? ui_layout_rect(rect->parent) : g;
        r2d_draw_rect_outline(pr, 1, r2d_rgba(255, 220, 80, 90));
        float ax0 = pr.x + pr.w * rect->anchor_min_x, ax1 = pr.x + pr.w * rect->anchor_max_x;
        float ay0 = pr.y + pr.h * rect->anchor_min_y, ay1 = pr.y + pr.h * rect->anchor_max_y;
        const float k = 7;
        r2d_color ac = r2d_rgba(255, 220, 80, 220);
        /* four small triangles pointing at the element, like Unity's */
        r2d_draw_triangle(r2d_v2(ax0, ay0), r2d_v2(ax0 - k, ay0 - k * 0.5f), r2d_v2(ax0 - k * 0.5f, ay0 - k), ac);
        r2d_draw_triangle(r2d_v2(ax1, ay0), r2d_v2(ax1 + k, ay0 - k * 0.5f), r2d_v2(ax1 + k * 0.5f, ay0 - k), ac);
        r2d_draw_triangle(r2d_v2(ax0, ay1), r2d_v2(ax0 - k, ay1 + k * 0.5f), r2d_v2(ax0 - k * 0.5f, ay1 + k), ac);
        r2d_draw_triangle(r2d_v2(ax1, ay1), r2d_v2(ax1 + k, ay1 + k * 0.5f), r2d_v2(ax1 + k * 0.5f, ay1 + k), ac);
        /* handles */
        const float hs = 6;
        float xs[3] = {r.x, r.x + r.w * 0.5f, r.x + r.w}, ys[3] = {r.y, r.y + r.h * 0.5f, r.y + r.h};
        for (int yy = 0; yy < 3; ++yy)
            for (int xx = 0; xx < 3; ++xx) {
                if (xx == 1 && yy == 1) continue;
                r2d_draw_rect(r2d_rect_make(xs[xx] - hs * 0.5f, ys[yy] - hs * 0.5f, hs, hs), yellow);
            }
        char label[96], name[64];
        ed_label(d, id, name, sizeof(name));
        snprintf(label, sizeof(label), "%s  %.0fx%.0f", name, (double)l->rw, (double)l->rh);
        r2d_draw_text(r2d_v2(r.x, r.y - r2d_text_height() - 4), r2d_with_alpha(white, 220), label);
    }
}
/* Clicking and dragging interface elements in the game view; returns true
 * when the mouse belongs to the interface this frame (the world's selection
 * controller then stays out of it). */
static bool handle_ui_input(void) {
    ed_document *d = &ui.doc;
    ui.ui_hover = CW_INVALID_ENTITY;
    if (!d->world_ready || !cw_ui_ready(&d->gw.ui) || ui.tool != TOOL_SELECT || hui_focused()) return ui.ui_drag.active = false;
    r2d_vec2 m = r2d_mouse();
    bool in_view = r2d_rect_contains(world_area(), m) && !r2d_mouse_over_ui();
    if (d->playing) {   /* the interface is live: feed it the mouse instead of editing it */
        if (in_view || r2d_mouse_down(R2D_MOUSE_LEFT)) cw_game_world_ui_input(&d->gw, m.x, m.y, r2d_mouse_down(R2D_MOUSE_LEFT), r2d_mouse_pressed(R2D_MOUSE_LEFT) && in_view, r2d_mouse_released(R2D_MOUSE_LEFT));
        ui.ui_drag.active = false;
        return in_view && cw_ui_hit(&d->gw.world, &d->gw.ui, m.x, m.y, true) != CW_INVALID_ENTITY;
    }
    if (ui.ui_drag.active) {
        if (!r2d_mouse_down(R2D_MOUSE_LEFT)) {
            ui.ui_drag.active = false;
            if (ui.ui_drag.moved) { ed_commit(d, ui.ui_drag.handles ? "resize element" : "move element"); ed_status(d, false, "%s", ui.ui_drag.handles ? "resized" : "moved"); }
            return true;
        }
        const cw_ui_layout *l = ed_is_ui(d, ui.ui_drag.id) ? cw_comp_ptr(&d->gw.world, ui.ui_drag.id, d->gw.ui.layout) : NULL;
        float scale = l && l->scale > 0 ? l->scale : 1;
        float dx = (m.x - ui.ui_drag.last.x) / scale, dy = (m.y - ui.ui_drag.last.y) / scale;
        if (ui.prefs.snap && ui.prefs.grid_spacing > 0) {   /* snap the drag to whole reference pixels in grid steps */
            float step = ui.prefs.grid_spacing >= 8 ? 8 : 1;
            dx = floorf(dx / step + 0.5f) * step;
            dy = floorf(dy / step + 0.5f) * step;
            if (dx == 0 && dy == 0) return true;
        }
        if (dx != 0 || dy != 0) {
            if (ui.ui_drag.handles) ed_ui_resize(d, ui.ui_drag.id, ui.ui_drag.handles & 1, ui.ui_drag.handles & 2, ui.ui_drag.handles & 4, ui.ui_drag.handles & 8, dx, dy);
            else ed_ui_move_selection(d, dx, dy);
            ui.ui_drag.moved = true;
            ui.ui_drag.last.x += dx * scale;
            ui.ui_drag.last.y += dy * scale;
        }
        return true;
    }
    if (!in_view) return false;
    /* hover: the primary's handles first, then the topmost element */
    cw_entity_id primary = ed_primary(d);
    int handles = primary != CW_INVALID_ENTITY && ed_is_ui(d, primary) ? ui_handle_at(ui_layout_rect(primary), m) : 0;
    cw_entity_id hit = ed_ui_hit(d, m.x, m.y);
    /* a selected element keeps the mouse while the cursor is inside it, so a
     * button can be dragged although its label sits on top */
    if (hit != CW_INVALID_ENTITY && primary != CW_INVALID_ENTITY && hit != primary && ed_is_ui(d, primary) && r2d_rect_contains(ui_layout_rect(primary), m)
        && !(r2d_modifiers() & SAPP_MODIFIER_CTRL)) hit = primary;   /* ctrl-click reaches the element on top */
    if (handles) ui.ui_hover = primary;
    else ui.ui_hover = hit;
    if (handles == 0 && hit == CW_INVALID_ENTITY) return false;
    if (r2d_mouse_pressed(R2D_MOUSE_LEFT)) {
        bool shift = (r2d_modifiers() & SAPP_MODIFIER_SHIFT) != 0;
        if (handles) {
            ui.ui_drag.id = primary;
        } else if (shift) {
            ed_select_toggle(d, hit);
            selection_to_r2d();
            return true;
        } else {
            if (!ed_is_selected(d, hit)) { ed_select_set(d, hit); selection_to_r2d(); }
            ui.ui_drag.id = hit;
        }
        ui.ui_drag.active = true;
        ui.ui_drag.handles = handles;
        ui.ui_drag.last = m;
        ui.ui_drag.moved = false;
    }
    return true;
}
static void draw_world(void) {
    r2d_set_space(R2D_SPACE_WORLD);
    r2d_rect b = level_bounds();
    host_canvas_begin(&ui.canvas);
    ui.canvas.lights = ui.lights;
    /* the post-process stack (lighting, fog, bloom...) is the game's look;
     * while editing it is optional so the level stays readable */
    r2d_post_set_bypass(!ui.lights);
    if (ui.doc.world_ready) {
        ed_ui_layout(&ui.doc, game_screen());   /* the interface follows the game view */
        cw_draw_context ctx = {cw_game_world_alpha(&ui.doc.gw), ui.time, !ui.doc.playing, ui.doc.paused || !ui.doc.playing};
        cw_game_world_draw(&ui.doc.gw, &ui.canvas.canvas, &ctx);
        r2d_set_space(R2D_SPACE_WORLD_OVERLAY);   /* the editor's own marks sit above sprites */
        /* entities the module did not draw still get a generic gizmo */
        cw_entity_id ids[4096];
        size_t n = ed_entities(&ui.doc, ids, 4096);
        for (size_t i = 0; i < n; ++i) {
            if (host_canvas_find(&ui.canvas, ids[i])) continue;
            float x, y;
            if (!cw_reflect_position(&ui.doc.gw.world, ids[i], &x, &y)) continue;
            float r = cw_reflect_radius(&ui.doc.gw.world, ids[i], r2d_px(8));
            r2d_draw_ring(r2d_v2(x, y), r, r2d_px(1.5f), r2d_rgba(200, 200, 220, 180));
            ui.canvas.canvas.pick(&ui.canvas, ids[i], x, y, r);
        }
    }
    if (ui.grid) r2d_draw_grid(b, ui.prefs.grid_spacing > 0 ? ui.prefs.grid_spacing : 50, 24, r2d_rgba(255, 255, 255, ui.prefs.snap ? 26 : 14));
    r2d_set_space(R2D_SPACE_WORLD_OVERLAY);
    r2d_draw_rect_outline(b, r2d_px(1), r2d_rgba(120, 130, 150, 120));
    /* hover and selection feedback around what the module declared pickable */
    for (size_t i = 0; i < ui.canvas.pick_count; ++i) {
        const host_pick *p = &ui.canvas.picks[i];
        bool selected = ed_is_selected(&ui.doc, p->id), hover = ui.hovered == (uint64_t)p->id;
        if (!selected && !hover) continue;
        /* an entity may register several pick areas (its shape, its sprite): one ring, around the largest */
        if (host_canvas_find(&ui.canvas, p->id) != p) continue;
        float radius = p->radius;
        for (size_t j = 0; j < i; ++j) if (ui.canvas.picks[j].id == p->id && ui.canvas.picks[j].radius > radius) radius = ui.canvas.picks[j].radius;
        if (selected) r2d_draw_ring(r2d_v2(p->x, p->y), radius + r2d_px(4), r2d_px(2), yellow);
        else r2d_draw_ring(r2d_v2(p->x, p->y), radius + r2d_px(3), r2d_px(1.5f), r2d_with_alpha(white, 170));
        if (hover || (selected && p->id == ed_primary(&ui.doc))) {
            char label[96], name[64];
            ed_label(&ui.doc, p->id, name, sizeof(name));
            snprintf(label, sizeof(label), "#%" PRId64 " %s", p->id, name);
            r2d_draw_text(r2d_add(r2d_v2(p->x, p->y), r2d_v2(radius + r2d_px(6), -radius - r2d_px(14))), r2d_with_alpha(white, 220), label);
        }
    }
    if (ui.tool != TOOL_SELECT && ui.doc.world_ready && !r2d_mouse_over_ui() && r2d_rect_contains(world_area(), r2d_mouse())) {
        r2d_vec2 m = r2d_mouse_world();
        r2d_draw_ring(m, r2d_px(12), r2d_px(1.5f), r2d_with_alpha(yellow, 200));
        r2d_draw_line(r2d_sub(m, r2d_v2(r2d_px(18), 0)), r2d_add(m, r2d_v2(r2d_px(18), 0)), r2d_px(1), r2d_with_alpha(yellow, 120));
        r2d_draw_line(r2d_sub(m, r2d_v2(0, r2d_px(18))), r2d_add(m, r2d_v2(0, r2d_px(18))), r2d_px(1), r2d_with_alpha(yellow, 120));
        if (ui.tool == TOOL_PLACE) {
            const cw_prefab *p = &ui.doc.gw.module->prefabs[ui.prefab];
            r2d_draw_textf(r2d_add(m, r2d_v2(r2d_px(16), r2d_px(8))), yellow, "%s %s", p->category ? p->category : "", p->name);
        } else if (ui.tool == TOOL_PREFAB) {
            const cw_asset *a = cw_assets_find(&ui.doc.assets, ui.prefab_asset);
            r2d_draw_textf(r2d_add(m, r2d_v2(r2d_px(16), r2d_px(8))), yellow, "prefab %s", a ? a->name : "?");
        } else r2d_draw_text(r2d_add(m, r2d_v2(r2d_px(16), r2d_px(8))), yellow, "new entity");
    }
}
static void handle_world_input(void) {
    if (!ui.doc.world_ready) return;
    bool in_viewport = r2d_rect_contains(world_area(), r2d_mouse()) && !r2d_mouse_over_ui();
    bool ui_owns_mouse = handle_ui_input();
    r2d_selection_result sel;
    r2d_selection_update(&sel, ui.tool == TOOL_SELECT && !hui_focused() && !ui_owns_mouse);
    if (sel.changed) selection_from_r2d();
    if (sel.dragging) ed_move_selection(&ui.doc, sel.drag_delta.x, sel.drag_delta.y);
    if (sel.drag_ended) {
        snap_selection();
        ed_commit(&ui.doc, "move");
        ed_status(&ui.doc, false, "moved %zu entit%s", ui.doc.selection_count, ui.doc.selection_count == 1 ? "y" : "ies");
    }
    if (ui.tool != TOOL_SELECT) {
        if (r2d_mouse_pressed(R2D_MOUSE_LEFT) && in_viewport && !hui_focused()) {
            r2d_vec2 m = r2d_mouse_world();
            float x = ed_settings_snap(&ui.prefs, m.x), y = ed_settings_snap(&ui.prefs, m.y);
            if (ui.tool == TOOL_PLACE) ed_place(&ui.doc, ui.prefab, x, y);
            else if (ui.tool == TOOL_PREFAB) ed_place_prefab(&ui.doc, ui.prefab_asset, x, y);
            else ed_create_entity(&ui.doc, x, y);
            selection_to_r2d();
            if (!(r2d_modifiers() & SAPP_MODIFIER_SHIFT)) ui.tool = TOOL_SELECT;   /* shift keeps placing */
        }
        if (r2d_mouse_pressed(R2D_MOUSE_RIGHT) && in_viewport) ui.tool = TOOL_SELECT;
    }
}

/* ---------------------------------------------------------------------------
 * preferences
 * ------------------------------------------------------------------------- */
static void apply_prefs(void) {
    r2d_set_text_scale(ui.prefs.text_scale);
    ui.grid = ui.prefs.grid;
}
static void snap_selection(void) {
    if (!ui.prefs.snap || !ui.doc.world_ready) return;
    for (size_t i = 0; i < ui.doc.selection_count; ++i) {
        float x, y;
        if (!cw_reflect_position(&ui.doc.gw.world, ui.doc.selection[i], &x, &y)) continue;
        cw_reflect_set_position(&ui.doc.gw.world, ui.doc.selection[i], ed_settings_snap(&ui.prefs, x), ed_settings_snap(&ui.prefs, y));
    }
}

/* ---------------------------------------------------------------------------
 * project settings: window and rendering
 * ------------------------------------------------------------------------- */
static void load_render_settings(void) {
    r2d_post_params *p = r2d_post_params_ptr();
    ui.render_loaded = false;
    if (!p || !ui.doc.project_open) return;
    char path[CW_GAME_PATH_CAPACITY], error[256] = "";
    ed_render_path(&ui.doc, path, sizeof(path));
    if (ed_render_settings_exist(&ui.doc)) {
        if (!r2d_post_load_config(path, error, sizeof(error))) ed_status(&ui.doc, true, "cannot load %s: %s", path, error);
        else if (error[0]) ed_status(&ui.doc, true, "%s: %s", path, error);
    } else {
        r2d_post_params_defaults(p);   /* a project without settings renders with the defaults */
        r2d_post_set_tier(p->quality.tier);
    }
    ui.render_saved = *p;
    ui.render_loaded = true;
    snprintf(ui.win_w, sizeof(ui.win_w), "%d", ui.doc.project.window_width);
    snprintf(ui.win_h, sizeof(ui.win_h), "%d", ui.doc.project.window_height);
    snprintf(ui.win_title, sizeof(ui.win_title), "%s", ui.doc.project.title);
}
static bool render_dirty(void) {
    r2d_post_params *p = r2d_post_params_ptr();
    return p && ui.render_loaded && memcmp(p, &ui.render_saved, sizeof(*p)) != 0;
}
static void save_render_settings(void) {
    r2d_post_params *p = r2d_post_params_ptr();
    if (!p || !ui.doc.project_open) return;
    char path[CW_GAME_PATH_CAPACITY];
    ed_render_path(&ui.doc, path, sizeof(path));
    if (!r2d_post_save_config(path)) { ed_status(&ui.doc, true, "cannot write %s", path); return; }
    ui.render_saved = *p;
    ed_status(&ui.doc, false, "rendering settings saved to %s", path);
}
static void save_window_settings(void) {
    int w = atoi(ui.win_w), h = atoi(ui.win_h);
    if (ed_set_window(&ui.doc, w, h, ui.win_title)) {
        snprintf(ui.win_w, sizeof(ui.win_w), "%d", ui.doc.project.window_width);
        snprintf(ui.win_h, sizeof(ui.win_h), "%d", ui.doc.project.window_height);
    }
}

/* ---------------------------------------------------------------------------
 * panels
 * ------------------------------------------------------------------------- */
static void open_project(const char *path) {
    if (ed_open_project(&ui.doc, path)) {
        host_canvas_set_assets(&ui.canvas, ui.doc.project_open ? &ui.doc.assets : NULL);
        load_render_settings();
        ui.lights = ui.prefs.preview_rendering;
        if (ui.scene_arg[0] && ui.doc.world_ready) ed_load_scene(&ui.doc, ui.scene_arg);
        else if (!ui.prefs.open_last_scene && ui.doc.world_ready) ed_new_scene(&ui.doc);
        ui.scene_arg[0] = 0;
        if (ui.doc.world_ready) fit_view();
    }
    selection_to_r2d();
    ui.tool = TOOL_SELECT;
    ui.prefab = 0;
}
static void save_as(void) {
    if (!ui.save_name[0]) { ed_status(&ui.doc, true, "type a scene name in the scenes panel first"); return; }
    char path[CW_GAME_PATH_CAPACITY];
    ed_scene_file(&ui.doc, ui.save_name, path, sizeof(path));
    ed_save_scene(&ui.doc, path);
}
static void save(void) {
    if (ui.doc.scene_path[0]) ed_save_scene(&ui.doc, NULL);
    else save_as();
}
static void open_scene_named(const char *name) {
    ea_stop(&ui.anim, &ui.doc);
    if (ui.doc.dirty && ui.prefs.confirm_discard && strcmp(ui.confirm, name) != 0) {
        snprintf(ui.confirm, sizeof(ui.confirm), "%s", name);
        ed_status(&ui.doc, true, "unsaved changes: click %s again to discard them", name);
        return;
    }
    ui.confirm[0] = 0;
    char path[CW_GAME_PATH_CAPACITY];
    ed_scene_file(&ui.doc, name, path, sizeof(path));
    if (ed_load_scene(&ui.doc, path)) {
        snprintf(ui.save_name, sizeof(ui.save_name), "%.*s", (int)(strlen(name) - 8), name);
        fit_view();
    }
    selection_to_r2d();
}
static bool toolbar_button(float *x, float y, float w, const char *label, bool on, bool enabled) {
    w *= ui.prefs.text_scale > 0 ? ui.prefs.text_scale : 1;   /* labels grow with the text */
    bool hit = r2d_ui_button_colored(r2d_rect_make(*x, y, w, 24), label, on ? accent : (enabled ? plain : r2d_rgb(44, 48, 56)));
    *x += w + 4;
    return hit && enabled;
}
static void draw_toolbar(void) {
    r2d_set_space(R2D_SPACE_SCREEN);
    r2d_vec2 v = r2d_viewport();
    r2d_ui_panel(r2d_rect_make(0, 0, v.x, TOOLBAR_H));
    float x = 10, y = 7;
    ed_document *d = &ui.doc;
    bool ready = d->world_ready;
#define TB(label, w, on, enabled) toolbar_button(&x, y, (w), (label), (on), (enabled))
    if (TB("new", 44, false, ready)) { ea_stop(&ui.anim, d); ed_new_scene(d); selection_to_r2d(); }
    if (TB("save", 46, d->dirty && ready, ready)) save();
    x += 8;
    if (TB("undo", 48, false, ed_can_undo(d))) { ed_undo(d); selection_to_r2d(); }
    if (TB("redo", 48, false, ed_can_redo(d))) { ed_redo(d); selection_to_r2d(); }
    x += 8;
    if (TB(d->playing ? "stop" : "play", 52, d->playing, ready)) { ea_stop(&ui.anim, d); if (d->playing) ed_stop(d); else ed_play(d); selection_to_r2d(); }
    if (TB(d->paused ? "resume" : "pause", 60, d->paused, ready && d->playing)) ed_toggle_pause(d);
    if (TB("step", 46, false, ready)) ed_step(d, 1);
    x += 8;
    if (TB("build", 52, false, d->project_open)) { ed_build(d, "module"); selection_to_r2d(); if (d->last_build_status != 0) hd_show(&ui.dock, "log"); }
    if (TB("export", 58, false, d->project_open)) { ed_build(d, "export"); hd_show(&ui.dock, "log"); }
    if (TB("run", 42, false, ready)) ed_run_player(d);
    x += 8;
    if (TB("fit", 40, false, true)) fit_view();
    if (TB("grid", 46, ui.grid, true)) ui.grid = !ui.grid;
    if (TB("lights", 56, ui.lights, true)) ui.lights = !ui.lights;
    {
        char label[32];
        snprintf(label, sizeof(label), "view %s", screen_presets[ui.screen_preset].name);
        if (TB(label, 118, ui.screen_preset != 0, true)) ui.screen_preset = (ui.screen_preset + 1) % SCREEN_PRESETS;
    }
    if (TB("log", 44, hd_is_open(&ui.dock, "log"), true)) { if (hd_is_open(&ui.dock, "log")) hd_close(&ui.dock, "log"); else hd_show(&ui.dock, "log"); }
    if (TB(render_dirty() ? "settings*" : "settings", 76, false, d->project_open)) { hd_show(&ui.dock, "rendering"); ui.lights = true; }
    float menu_x = x;
    if (TB("panels", 60, ui.panels_menu, true)) ui.panels_menu = !ui.panels_menu;
    ui.panels_menu_x = menu_x;
#undef TB
    char title[1024];
    const char *scene = d->scene_path[0] ? strrchr(d->scene_path, '/') : NULL;
    scene = scene ? scene + 1 : (d->scene_path[0] ? d->scene_path : "untitled");
    if (d->project_open) snprintf(title, sizeof(title), "%s  -  %s%s%s", d->project.name, scene, d->dirty ? "*" : "", d->playing ? "  [PLAYING]" : "");
    else snprintf(title, sizeof(title), "no project");
    float tw = r2d_text_width(title);
    r2d_ui_label_colored(r2d_v2(v.x - tw - 12, y + 6), d->playing ? green : white, title);
}
static void draw_palette(Layout *l) {
    const cw_game_module *m = ui.doc.gw.module;
    heading(l, "PALETTE");
    r2d_rect r = row(l, ROW_H);
    if (button(r, ui.tool == TOOL_SELECT ? "select tool (Q)" : "back to select (Q)", ui.tool == TOOL_SELECT)) ui.tool = TOOL_SELECT;
    const char *category = NULL;
    int column = 0;
    r2d_rect line = r2d_rect_make(0, 0, 0, 0);
    for (size_t i = 0; i < m->prefab_count; ++i) {
        const cw_prefab *p = &m->prefabs[i];
        const char *cat = p->category ? p->category : "";
        if (!category || strcmp(category, cat) != 0) {
            category = cat;
            column = 0;
            l->y += 2;
            r2d_ui_label_dim(r2d_v2(l->x, l->y), cat);
            l->y += r2d_text_height() + 3;
        }
        if (column == 0) line = row(l, ROW_H);
        char label[40];
        if (i < 9) snprintf(label, sizeof(label), "%zu %s", i + 1, p->name); else snprintf(label, sizeof(label), "%s", p->name);
        if (button(split(line, column, 2), label, ui.tool == TOOL_PLACE && ui.prefab == i)) { ui.tool = TOOL_PLACE; ui.prefab = i; }
        column = (column + 1) % 2;
    }
    /* the editor's own: empty entities and project prefabs */
    l->y += 2;
    r2d_ui_label_dim(r2d_v2(l->x, l->y), "editor");
    l->y += r2d_text_height() + 3;
    r = row(l, ROW_H);
    if (button(r, "E  new empty entity", ui.tool == TOOL_CREATE)) ui.tool = TOOL_CREATE;
    /* interface elements: created in the game view, inside the selected element when there is one */
    l->y += 2;
    cw_entity_id parent = ed_primary(&ui.doc);
    bool inside = parent != CW_INVALID_ENTITY && ed_is_ui(&ui.doc, parent);
    r2d_ui_label_dim(r2d_v2(l->x, l->y), inside ? "interface (inside the selection)" : "interface");
    l->y += r2d_text_height() + 3;
    column = 0;
    for (int k = 0; k < ED_UI_KINDS; ++k) {
        if (column == 0) line = row(l, ROW_H);
        if (button(split(line, column, 3), ed_ui_kind_name((ed_ui_kind)k), false)) {
            ed_ui_layout(&ui.doc, game_screen());
            ed_create_ui(&ui.doc, (ed_ui_kind)k, inside ? parent : CW_INVALID_ENTITY);
            selection_to_r2d();
        }
        column = (column + 1) % 3;
    }
    const cw_asset *prefabs[64];
    size_t n = ed_list_prefabs(&ui.doc, prefabs, 64);
    if (n) {
        l->y += 2;
        r2d_ui_label_dim(r2d_v2(l->x, l->y), "project prefabs (assets panel deletes)");
        l->y += r2d_text_height() + 3;
        column = 0;
        for (size_t i = 0; i < n; ++i) {
            if (column == 0) line = row(l, ROW_H);
            char name[CW_ASSET_NAME_CAPACITY];
            size_t len = strlen(prefabs[i]->name);
            snprintf(name, sizeof(name), "%.*s", (int)(len > 9 && strcmp(prefabs[i]->name + len - 9, ".cwprefab") == 0 ? len - 9 : len), prefabs[i]->name);
            if (button(split(line, column, 2), name, ui.tool == TOOL_PREFAB && ui.prefab_asset == prefabs[i]->id)) { ui.tool = TOOL_PREFAB; ui.prefab_asset = prefabs[i]->id; }
            column = (column + 1) % 2;
        }
    }
}
static void draw_scenes(Layout *l) {
    ed_document *d = &ui.doc;
    heading(l, "SCENES");
    char names[64][128];
    size_t n = ed_list_scenes(d, names, 64);
    const char *current = d->scene_path[0] ? strrchr(d->scene_path, '/') : NULL;
    current = current ? current + 1 : "";
    for (size_t i = 0; i < n && i < 8; ++i) {
        r2d_rect r = row(l, ROW_H - 2);
        char label[64];
        snprintf(label, sizeof(label), "%.*s", (int)(strlen(names[i]) - 8), names[i]);
        if (button(r, label, strcmp(names[i], current) == 0)) open_scene_named(names[i]);
    }
    if (n > 8) label_line(l, r2d_rgb(150, 155, 165), "...");
    r2d_rect r = row(l, ROW_H);
    if (textfield(r2d_rect_make(r.x, r.y, r.w - 76, r.h), &ui.save_name, ui.save_name, sizeof(ui.save_name))) save_as();
    if (button(r2d_rect_make(r.x + r.w - 72, r.y, 72, r.h), "save as", false)) save_as();
}
static void draw_entities(Layout *l, float bottom) {
    ed_document *d = &ui.doc;
    char text[96];
    snprintf(text, sizeof(text), "ENTITIES (%zu)", ed_entity_count(d));
    heading(l, text);
    static cw_entity_id ids[4096];
    size_t n = ed_entities(d, ids, 4096);
    int visible = (int)((bottom - l->y) / (ROW_H - 2 + 4));
    if (visible < 1) visible = 1;
    r2d_rect list_area = r2d_rect_make(l->x, l->y, l->w, bottom - l->y);
    if (r2d_rect_contains(list_area, r2d_mouse()) && r2d_wheel() != 0) ui.entity_scroll -= (int)r2d_wheel() * 3;
    if (ui.entity_scroll > (int)n - visible) ui.entity_scroll = (int)n - visible;
    if (ui.entity_scroll < 0) ui.entity_scroll = 0;
    for (size_t i = (size_t)ui.entity_scroll; i < n && (int)(i - (size_t)ui.entity_scroll) < visible; ++i) {
        r2d_rect r = row(l, ROW_H - 2);
        char label[64], line[96];
        ed_label(d, ids[i], label, sizeof(label));
        int depth = ed_is_ui(d, ids[i]) ? cw_ui_depth(&d->gw.world, &d->gw.ui, ids[i]) : 0;   /* interface children indent under their parent */
        snprintf(line, sizeof(line), "%*s#%" PRId64 " %s%s", depth > 6 ? 12 : depth * 2, "", ids[i], ed_is_ui(d, ids[i]) ? "ui " : "", label);
        if (button(r, line, ed_is_selected(d, ids[i]))) {
            if (r2d_modifiers() & (SAPP_MODIFIER_SHIFT | SAPP_MODIFIER_CTRL)) ed_select_toggle(d, ids[i]);
            else ed_select_set(d, ids[i]);
            selection_to_r2d();
        }
    }
    if (n > (size_t)visible) {
        snprintf(text, sizeof(text), "%d-%d of %zu (wheel scrolls)", ui.entity_scroll + 1, ui.entity_scroll + visible < (int)n ? ui.entity_scroll + visible : (int)n, n);
        r2d_ui_label_dim(r2d_v2(l->x, bottom - r2d_text_height()), text);
    }
}
/* the asset browser */
static void import_path(void) {
    if (!ui.import_path[0]) return;
    struct stat st;
    if (stat(ui.import_path, &st) == 0 && (st.st_mode & S_IFDIR)) ed_import_directory(&ui.doc, ui.import_path, false);
    else ed_import_asset(&ui.doc, ui.import_path, false);
    ui.import_path[0] = 0;
}
static void draw_assets(Layout *l, float bottom) {
    ed_document *d = &ui.doc;
    char text[CW_ASSET_PATH_CAPACITY + 64];
    snprintf(text, sizeof(text), "ASSETS (%zu)", d->assets.count);
    heading(l, text);
    r2d_rect r = row(l, ROW_H);
    if (textfield(r2d_rect_make(r.x, r.y, r.w - 66, r.h), &ui.import_path, ui.import_path, sizeof(ui.import_path))) import_path();
    if (button(r2d_rect_make(r.x + r.w - 62, r.y, 62, r.h), "import", false)) import_path();
    label_line(l, r2d_rgb(150, 155, 165), "a file or directory path; or drop files here");
    r = row(l, ROW_H);
    if (button(split(r, 0, 2), "scan project", false)) { size_t n = ed_scan_assets(d); ed_status(d, false, "scan registered %zu new file%s", n, n == 1 ? "" : "s"); }
    if (button(split(r, 1, 2), "refresh", false)) cw_assets_refresh(&d->assets);
    /* type filter */
    static const int filters[] = {-1, CW_ASSET_SCENE, CW_ASSET_PREFAB, CW_ASSET_IMAGE, CW_ASSET_MATERIAL, CW_ASSET_ANIMATION, CW_ASSET_DATA, CW_ASSET_OTHER};
    static const char *const filter_names[] = {"all", "scene", "prefab", "image", "material", "anim", "data", "other"};
    r = row(l, ROW_H - 2);
    for (int i = 0; i < 4; ++i) if (button(split(r, i, 4), filter_names[i], ui.asset_filter == filters[i])) ui.asset_filter = filters[i];
    r = row(l, ROW_H - 2);
    for (int i = 4; i < 8; ++i) if (button(split(r, i - 4, 4), filter_names[i], ui.asset_filter == filters[i])) ui.asset_filter = filters[i];
    /* the list */
    size_t shown = 0;
    const cw_asset *selected = cw_assets_find(&d->assets, ui.asset_selected);
    float details_h = selected ? ROW_H * 4 + 40 : 0;
    float list_bottom = bottom - details_h;
    int visible = (int)((list_bottom - l->y) / (ROW_H - 2 + 4));
    if (visible < 1) visible = 1;
    r2d_rect list_area = r2d_rect_make(l->x, l->y, l->w, list_bottom - l->y);
    if (r2d_rect_contains(list_area, r2d_mouse()) && r2d_wheel() != 0) ui.asset_scroll -= (int)r2d_wheel() * 3;
    size_t matching = 0;
    for (size_t i = 0; i < d->assets.count; ++i) if (ui.asset_filter < 0 || (int)d->assets.items[i].type == ui.asset_filter) ++matching;
    if (ui.asset_scroll > (int)matching - visible) ui.asset_scroll = (int)matching - visible;
    if (ui.asset_scroll < 0) ui.asset_scroll = 0;
    int index = 0;
    for (size_t i = 0; i < d->assets.count && (int)shown < visible; ++i) {
        const cw_asset *a = &d->assets.items[i];
        if (ui.asset_filter >= 0 && (int)a->type != ui.asset_filter) continue;
        if (index++ < ui.asset_scroll) continue;
        r = row(l, ROW_H - 2);
        snprintf(text, sizeof(text), "%s%s  %s", a->missing ? "! " : "", cw_asset_type_name(a->type), a->name);
        if (r2d_ui_button_colored(r, text, a->id == ui.asset_selected ? accent : (a->missing ? r2d_rgb(110, 48, 44) : plain))) {
            ui.asset_selected = a->id;
            snprintf(ui.asset_name, sizeof(ui.asset_name), "%s", a->name);
            hui_blur();
        }
        ++shown;
    }
    if (!matching) label_line(l, r2d_rgb(150, 155, 165), "nothing here yet");
    if (matching > (size_t)visible) {
        snprintf(text, sizeof(text), "%d-%zu of %zu (wheel scrolls)", ui.asset_scroll + 1, (size_t)ui.asset_scroll + shown, matching);
        label_line(l, r2d_rgb(150, 155, 165), text);
    }
    if (!selected) return;
    /* details of the selected asset */
    l->y = bottom - details_h + 4;
    snprintf(text, sizeof(text), "%s  %llu bytes  id %08x%s", cw_asset_type_name(selected->type), (unsigned long long)selected->size, selected->id, selected->missing ? "  MISSING" : "");
    label_line(l, selected->missing ? red : yellow, text);
    label_line(l, r2d_rgb(150, 155, 165), selected->path);
    size_t refs = ed_asset_references(d, selected->id);
    snprintf(text, sizeof(text), "used by %zu component%s in this scene", refs, refs == 1 ? "" : "s");
    label_line(l, r2d_rgb(150, 155, 165), text);
    r = row(l, ROW_H);
    if (textfield(r2d_rect_make(r.x, r.y, r.w - 66, r.h), &ui.asset_name, ui.asset_name, sizeof(ui.asset_name))) {
        if (ed_rename_asset(d, selected->id, ui.asset_name)) ui.asset_selected = cw_asset_id(ui.asset_name);
    }
    if (button(r2d_rect_make(r.x + r.w - 62, r.y, 62, r.h), "rename", false) && ed_rename_asset(d, selected->id, ui.asset_name)) ui.asset_selected = cw_asset_id(ui.asset_name);
    r = row(l, ROW_H);
    if (selected->type == CW_ASSET_SCENE) {
        if (button(split(r, 0, 3), "open", false)) { const char *name = strrchr(selected->path, '/'); open_scene_named(name ? name + 1 : selected->path); }
    } else if (selected->type == CW_ASSET_PREFAB) {
        if (button(split(r, 0, 3), "place", false)) { ui.tool = TOOL_PREFAB; ui.prefab_asset = selected->id; }
    } else if (button(split(r, 0, 3), "users", false)) {
        /* select every entity referencing it */
        ed_select_clear(d);
        cw_world *w = &d->gw.world;
        for (unsigned c = 0; c < CW_MAX_COMPONENTS; ++c) {
            const cw_type_info *t = cw_comp_type(w, (cw_component_id)c);
            for (size_t f = 0; t && f < t->field_count; ++f) {
                if (t->fields[f].semantic != CW_SEM_ASSET) continue;
                for (size_t i = 0; i < w->capacity; ++i)
                    if (cw_query_visible(w->entities[i]) && cw_comp_has(w, (cw_entity_id)i, (cw_component_id)c)
                        && (uint32_t)cw_field_get(&t->fields[f], cw_comp_ptr(w, (cw_entity_id)i, (cw_component_id)c)) == selected->id) ed_select_add(d, (cw_entity_id)i);
            }
        }
        selection_to_r2d();
    }
    if (button(split(r, 1, 3), "forget", false)) { ed_remove_asset(d, selected->id, false); ui.asset_selected = 0; }
    if (r2d_ui_button_colored(split(r, 2, 3), "delete", r2d_rgb(120, 48, 44))) { ed_remove_asset(d, selected->id, true); ui.asset_selected = 0; }
}

/* the world inspector: the ECS as it is right now */
static void phase_names(cw_phase_mask phases, char *out, size_t capacity) {
    static const char *const names[] = {"init", "start", "stop", "shutdown", "early", "physics", "update", "late", "overlay", "created", "destroyed", "u0", "u1", "u2", "u3", "end"};
    out[0] = 0;
    for (int i = 0; i < 16; ++i) {
        if (!(phases & CW_PHASE(i))) continue;
        if (out[0]) strncat(out, ",", capacity - strlen(out) - 1);
        strncat(out, names[i], capacity - strlen(out) - 1);
    }
}
static void draw_world_inspector(Layout *l, float bottom) {
    ed_document *d = &ui.doc;
    cw_world *w = &d->gw.world;
    const cw_game_module *m = d->gw.module;
    char text[160];
    heading(l, "MODULE");
    snprintf(text, sizeof(text), "%s  v%u  %d Hz  %zu prefabs", m->name, m->scene_version, d->gw.clock.hz, m->prefab_count);
    label_line(l, white, text);
    snprintf(text, sizeof(text), "bounds %.0fx%.0f at %.0f,%.0f", (double)m->bounds[2], (double)m->bounds[3], (double)m->bounds[0], (double)m->bounds[1]);
    label_line(l, r2d_rgb(150, 155, 165), text);
    heading(l, "WORLD");
    cw_memory_stats mem = cw_world_memory(w);
    snprintf(text, sizeof(text), "%zu entities  tick %" PRIu64 "  next %" PRId64, cw_world_entity_count(w), w->tick, w->next_id);
    label_line(l, white, text);
    snprintf(text, sizeof(text), "%zu pending  %zu created  %zu recycled", w->pending_destroy.count, w->created.count, w->recycled_ids.count);
    label_line(l, r2d_rgb(150, 155, 165), text);
    snprintf(text, sizeof(text), "%.0f KB (pools %.0f KB)  %zu queries", mem.total / 1024.0, mem.pool_committed / 1024.0, w->queries.handle_count);
    label_line(l, r2d_rgb(150, 155, 165), text);
    if (cw_reflect_has_bakes(w)) {
        snprintf(text, sizeof(text), "baking %s: config -> runtime components %s", d->gw.baking ? "on" : "off", d->gw.baking ? "are generated" : "appear in play mode");
        label_line(l, d->gw.baking ? green : r2d_rgb(150, 155, 165), text);
    }
    /* scrolling area: components then systems */
    float top = l->y;
    r2d_rect area = r2d_rect_make(l->x, top, l->w, bottom - top);
    if (r2d_rect_contains(area, r2d_mouse()) && r2d_wheel() != 0) ui.world_scroll -= (int)(r2d_wheel() * 40);
    if (ui.world_scroll < 0) ui.world_scroll = 0;
    l->y -= (float)ui.world_scroll;
#define VISIBLE (l->y >= top - ROW_H && l->y + ROW_H <= bottom)
    if (VISIBLE) heading(l, "COMPONENTS (click = select carriers)"); else l->y += r2d_text_height() + 8;
    for (unsigned c = 0; c < CW_MAX_COMPONENTS; ++c) {
        if (!w->pools[c].elem_size) continue;   /* not registered */
        const char *name = cw_comp_name(w, (cw_component_id)c);
        const cw_type_info *t = cw_comp_type(w, (cw_component_id)c);
        size_t n = ed_component_entities(d, (cw_component_id)c);
        const char *kind = !t ? "  (opaque)" : (t->flags & CW_TYPE_TRANSIENT) ? "  (transient)" : (t->flags & CW_TYPE_RUNTIME) ? "  (runtime)" : (t->flags & CW_TYPE_INTERNAL) ? "  (internal)" : "";
        if (t && (t->flags & CW_TYPE_CONFIG)) snprintf(text, sizeof(text), "%s  %zu  (config -> %s)", name, n, t->generates);
        else snprintf(text, sizeof(text), "%s  %zu%s", name, n, kind);
        if (VISIBLE) {
            r2d_rect r = row(l, ROW_H - 2);
            if (button(r, text, false)) { ed_select_with_component(d, (cw_component_id)c); selection_to_r2d(); }
        } else l->y += ROW_H + 2;
    }
    size_t count;
    const cw_system *systems = cw_sim_systems(&d->gw.sim, &count);
    if (VISIBLE) heading(l, "SYSTEMS (dispatch order; click = toggle)"); else l->y += r2d_text_height() + 8;
    for (size_t i = 0; i < count; ++i) {
        char phases[96];
        phase_names(systems[i].phases, phases, sizeof(phases));
        snprintf(text, sizeof(text), "%s%s  [%s] %+d%s", systems[i].enabled ? "" : "(off) ", systems[i].name, phases, systems[i].order,
                 systems[i].authority == CW_HOST_ONLY ? " host" : "");
        if (VISIBLE) {
            r2d_rect r = row(l, ROW_H - 2);
            if (r2d_ui_button_colored(r, text, systems[i].enabled ? plain : r2d_rgb(70, 50, 50))) ed_set_system_enabled(d, systems[i].name, !systems[i].enabled);
        } else l->y += ROW_H + 2;
    }
#undef VISIBLE
}

/* before a project is open or built, every panel shows the same thing */
static bool draw_not_ready(Layout *l) {
    ed_document *d = &ui.doc;
    if (!d->project_open) {
        heading(l, "PROJECT");
        label_line(l, white, "path to a .cwproj file:");
        r2d_rect r = row(l, ROW_H);
        if (textfield(r, &ui.open_path, ui.open_path, sizeof(ui.open_path))) open_project(ui.open_path);
        r = row(l, ROW_H);
        if (button(r, "open project", false)) open_project(ui.open_path);
        return true;
    }
    if (!d->world_ready) {
        heading(l, "PROJECT");
        label_line(l, white, d->project.name);
        label_line(l, red, "module not built");
        r2d_rect r = row(l, ROW_H);
        if (button(r, "build module (F7)", true)) { ed_build(d, "module"); if (d->last_build_status != 0) hd_show(&ui.dock, "log"); if (d->world_ready) fit_view(); }
        if (!ui.render_loaded) load_render_settings();
        return true;
    }
    return false;
}
static Layout content_layout(r2d_rect content) {
    Layout l = {content.x + 10, content.y + 8, content.w - 20};
    return l;
}
static void panel_scene(r2d_rect content, void *user) {
    (void)user;
    Layout l = content_layout(content);
    if (draw_not_ready(&l)) return;
    draw_palette(&l);
    draw_scenes(&l);
    draw_entities(&l, content.y + content.h - 8);
}
static void panel_assets(r2d_rect content, void *user) {
    (void)user;
    Layout l = content_layout(content);
    if (draw_not_ready(&l)) return;
    draw_assets(&l, content.y + content.h - 8);
}
static void panel_world(r2d_rect content, void *user) {
    (void)user;
    Layout l = content_layout(content);
    if (draw_not_ready(&l)) return;
    draw_world_inspector(&l, content.y + content.h - 8);
}
static void panel_viewport(r2d_rect content, void *user) {
    (void)content; (void)user;   /* the world is drawn underneath; the dock only frames it */
}

/* the inspector: every visible field of every reflected component */
/* inspector edits go through these so record mode can turn them into keys */
static bool edit_set(ed_document *d, cw_entity_id id, cw_component_id ct, const cw_field *f, const char *text) {
    bool ok = ed_set_field(d, id, ct, f, text);
    if (ok && ui.anim.record) ea_record_field(&ui.anim, d, id, ct, f);
    return ok;
}
static bool edit_adjust(ed_document *d, cw_entity_id id, cw_component_id ct, const cw_field *f, int direction) {
    bool ok = ed_adjust_field(d, id, ct, f, direction);
    if (ok && ui.anim.record) ea_record_field(&ui.anim, d, id, ct, f);
    return ok;
}
static void draw_field(Layout *l, cw_entity_id id, cw_component_id ct, const cw_field *f) {
    ed_document *d = &ui.doc;
    const void *value = cw_comp_ptr(&d->gw.world, id, ct);
    char text[64];
    cw_field_format(f, value, text, sizeof(text));
    if (f->type == CW_FIELD_F32) snprintf(text, sizeof(text), "%.6g", cw_field_get(f, value));   /* readable, not exact */
    r2d_rect r = row(l, ROW_H);
    r2d_ui_label(r2d_v2(r.x, r.y + 5), f->name);
    float vx = r.x + 104;
    switch (f->type) {
        case CW_FIELD_BOOL:
            if (button(r2d_rect_make(vx, r.y, 70, r.h), text, cw_field_get(f, value) != 0)) edit_adjust(d, id, ct, f, 1);
            break;
        case CW_FIELD_ENUM:
            if (button(r2d_rect_make(vx, r.y, r.w - 104 - 62, r.h), text, false)) edit_adjust(d, id, ct, f, 1);
            if (button(r2d_rect_make(r.x + r.w - 58, r.y, 27, r.h), "-", false)) edit_adjust(d, id, ct, f, -1);
            if (button(r2d_rect_make(r.x + r.w - 27, r.y, 27, r.h), "+", false)) edit_adjust(d, id, ct, f, 1);
            break;
        case CW_FIELD_STRING: {
            enum { STR_SLOTS = 16 };
            static char sbuffers[STR_SLOTS][CW_UI_TEXT_CAPACITY + 32];
            static const cw_field *skeys[STR_SLOTS];
            static cw_entity_id skey_ids[STR_SLOTS];
            size_t slot = ui.field_ordinal++ % STR_SLOTS;
            if (skeys[slot] != f || skey_ids[slot] != id || hui_focus_key() != &skeys[slot]) {
                skeys[slot] = f;
                skey_ids[slot] = id;
                cw_field_format(f, value, sbuffers[slot], sizeof(sbuffers[slot]));
            }
            if (textfield(r2d_rect_make(vx, r.y, r.w - 104, r.h), &skeys[slot], sbuffers[slot], sizeof(sbuffers[slot]))) {
                edit_set(d, id, ct, f, sbuffers[slot]);
                skeys[slot] = NULL;
            }
            break;
        }
        case CW_FIELD_FLAGS: {
            unsigned bits = (unsigned)cw_field_get(f, value);
            float bw = (r.w - 104 - 4 * (float)(f->name_count - 1)) / (float)f->name_count;
            for (unsigned i = 0; i < f->name_count; ++i)
                if (button(r2d_rect_make(vx + (float)i * (bw + 4), r.y, bw, r.h), f->names[i], (bits >> i) & 1u)) edit_adjust(d, id, ct, f, (int)i + 1);
            break;
        }
        case CW_FIELD_U32:
            if (f->semantic == CW_SEM_ASSET) {
                /* an asset reference: pick from the project's assets */
                const void *key = (const char *)f + 1;
                if (button(r2d_rect_make(vx, r.y, r.w - 104, r.h), text[0] == '@' ? text + 1 : text, ui.asset_picker == key)) ui.asset_picker = ui.asset_picker == key ? NULL : key;
                if (ui.asset_picker == key) {
                    /* the field's name says what kind it wants (Sprite.material, Shape.sprite, ...) */
                    cw_asset_type want = cw_asset_type_for_field(f->name);
                    r2d_rect none = row(l, ROW_H - 2);
                    if (button(r2d_rect_make(none.x + 104, none.y, none.w - 104, none.h), "none", false)) { edit_set(d, id, ct, f, "none"); ui.asset_picker = NULL; }
                    size_t listed = 0, matching = 0;
                    for (size_t i = 0; i < d->assets.count; ++i) {
                        const cw_asset *a = &d->assets.items[i];
                        if (want != CW_ASSET_OTHER ? a->type != want : (a->type == CW_ASSET_SOURCE || a->type == CW_ASSET_SCENE || a->type == CW_ASSET_CONFIG)) continue;
                        ++matching;
                        if (listed >= 12) continue;
                        char at[CW_ASSET_NAME_CAPACITY + 2];
                        snprintf(at, sizeof(at), "@%s", a->name);
                        r2d_rect ar = row(l, ROW_H - 2);
                        if (button(r2d_rect_make(ar.x + 104, ar.y, ar.w - 104, ar.h), a->name, false)) { edit_set(d, id, ct, f, at); ui.asset_picker = NULL; }
                        ++listed;
                    }
                    char hint[96];
                    if (want != CW_ASSET_OTHER) snprintf(hint, sizeof(hint), "(%s assets%s; %zu in the project)", cw_asset_type_name(want), matching > listed ? ", first 12" : "", matching);
                    else snprintf(hint, sizeof(hint), "(any asset%s; import more in assets)", matching > listed ? ", first 12" : "");
                    label_line(l, r2d_rgb(150, 155, 165), hint);
                }
                break;
            }
            /* fall through */
        default: {
            /* one text buffer per inspector row, in drawing order; a row is
             * re-seeded from the world unless it is the focused field */
            enum { SLOTS = 128 };
            static char buffers[SLOTS][64];
            static const cw_field *keys[SLOTS];
            static cw_entity_id key_ids[SLOTS];
            size_t slot = ui.field_ordinal++ % SLOTS;
            if (keys[slot] != f || key_ids[slot] != id || hui_focus_key() != &keys[slot]) {
                keys[slot] = f;
                key_ids[slot] = id;
                snprintf(buffers[slot], sizeof(buffers[slot]), "%s", text);
            }
            if (f->semantic == CW_SEM_ANGLE && hui_focus_key() != &keys[slot])
                snprintf(buffers[slot], sizeof(buffers[slot]), "%.1f", cw_field_get(f, value) * 180.0 / 3.14159265358979);
            if (textfield(r2d_rect_make(vx, r.y, r.w - 104 - 62, r.h), &keys[slot], buffers[slot], sizeof(buffers[slot]))) {
                if (f->semantic == CW_SEM_ANGLE) {
                    char radians[32];
                    snprintf(radians, sizeof(radians), "%.9g", atof(buffers[slot]) * 3.14159265358979 / 180.0);
                    edit_set(d, id, ct, f, radians);
                } else edit_set(d, id, ct, f, buffers[slot]);
                keys[slot] = NULL;   /* re-seed from the (possibly clamped) value next frame */
            }
            if (button(r2d_rect_make(r.x + r.w - 58, r.y, 27, r.h), "-", false)) edit_adjust(d, id, ct, f, -1);
            if (button(r2d_rect_make(r.x + r.w - 27, r.y, 27, r.h), "+", false)) edit_adjust(d, id, ct, f, 1);
            break;
        }
    }
}
static void panel_inspector(r2d_rect panel, void *user) {
    (void)user;
    ed_document *d = &ui.doc;
    Layout l = content_layout(panel);
    heading(&l, "INSPECTOR");
    ui.field_ordinal = 0;
    if (!d->world_ready) { label_line(&l, r2d_rgb(150, 155, 165), "no world"); return; }
    cw_entity_id id = ed_primary(d);
    if (id == CW_INVALID_ENTITY) {
        label_line(&l, r2d_rgb(150, 155, 165), "click an entity, drag a box,");
        label_line(&l, r2d_rgb(150, 155, 165), "shift-click to add; pick a prefab");
        label_line(&l, r2d_rgb(150, 155, 165), "and click the level to place it.");
        return;
    }
    cw_world *w = &d->gw.world;
    char text[128];
    if (d->selection_count > 1) snprintf(text, sizeof(text), "%zu selected, showing #%" PRId64, d->selection_count, id);
    else snprintf(text, sizeof(text), "entity #%" PRId64, id);
    label_line(&l, yellow, text);
    /* name */
    r2d_rect r = row(&l, ROW_H);
    r2d_ui_label(r2d_v2(r.x, r.y + 5), "name");
    if (ui.name_field_id != id && hui_focus_key() != &ui.name_field) { snprintf(ui.name_field, sizeof(ui.name_field), "%s", ed_name_of(d, id)); ui.name_field_id = id; }
    if (textfield(r2d_rect_make(r.x + 104, r.y, r.w - 104, r.h), &ui.name_field, ui.name_field, sizeof(ui.name_field))) ed_rename(d, id, ui.name_field);
    float x, y;
    if (cw_reflect_position(w, id, &x, &y)) {
        snprintf(text, sizeof(text), "at %.1f, %.1f", (double)x, (double)y);
        label_line(&l, r2d_rgb(150, 155, 165), text);
    }
    /* scrolling */
    if (r2d_rect_contains(panel, r2d_mouse()) && r2d_wheel() != 0) ui.inspector_scroll -= (int)(r2d_wheel() * 40);
    if (ui.inspector_scroll < 0) ui.inspector_scroll = 0;
    float top = l.y, bottom = panel.y + panel.h - 8;
    l.y -= (float)ui.inspector_scroll;
    for (unsigned c = 0; c < CW_MAX_COMPONENTS; ++c) {
        cw_component_id ct = (cw_component_id)c;
        const cw_type_info *t = cw_comp_type(w, ct);
        if (!t || (t->flags & (CW_TYPE_TRANSIENT | CW_TYPE_INTERNAL)) || !cw_comp_has(w, id, ct)) continue;
        bool runtime = (t->flags & CW_TYPE_RUNTIME) != 0, config = (t->flags & CW_TYPE_CONFIG) != 0;
        l.y += 4;
        if (l.y >= top - ROW_H && l.y + ROW_H <= bottom) {
            r2d_rect hr = row(&l, ROW_H);
            r2d_draw_rect(hr, runtime ? r2d_rgb(40, 58, 52) : (config ? r2d_rgb(58, 52, 42) : r2d_rgb(48, 54, 66)));
            r2d_ui_label_colored(r2d_v2(hr.x + 4, hr.y + 5), white, t->name);
            char kind[96];
            if (config) snprintf(kind, sizeof(kind), "config -> %s", t->generates);
            else if (runtime) snprintf(kind, sizeof(kind), "runtime (generated)");
            else kind[0] = 0;
            if (kind[0]) r2d_ui_label_colored(r2d_v2(hr.x + 8 + r2d_text_width(t->name), hr.y + 5), runtime ? green : yellow, kind);
            if (!runtime && r2d_ui_button_colored(r2d_rect_make(hr.x + hr.w - 24, hr.y + 1, 22, hr.h - 2), "x", r2d_rgb(110, 48, 44))) { ed_remove_component(d, id, ct); break; }
        } else l.y += ROW_H + 4;
        if (t == &cw_ui_rect_type) {   /* anchor presets, like Unity's: where the element is pinned to its parent */
            r2d_rect ar = row(&l, ROW_H);
            if (ar.y >= top - ROW_H && ar.y + (ROW_H + 4) * 4 <= bottom + ROW_H) {
                /* three rows: the nine points in a grid, the stretches beside them */
                static const char *const labels[12] = {"tl", "t", "tr", "l", "c", "r", "bl", "b", "br", "<->", "^v", "all"};
                const cw_ui_rect *rect = cw_comp_ptr(w, id, ct);
                for (int rowi = 0; rowi < 3; ++rowi) {
                    if (rowi) ar = row(&l, ROW_H);
                    if (rowi == 0) r2d_ui_label(r2d_v2(ar.x, ar.y + 5), "anchors");
                    r2d_rect grid = r2d_rect_make(ar.x + 104, ar.y, ar.w - 104, ar.h);
                    for (int col = 0; col < 4; ++col) {
                        int p = col < 3 ? rowi * 3 + col : 9 + rowi;
                        bool on;
                        if (p < 9) on = rect->anchor_min_x == rect->anchor_max_x && rect->anchor_min_y == rect->anchor_max_y && rect->anchor_min_x == (float)(p % 3) * 0.5f && rect->anchor_min_y == (float)(p / 3) * 0.5f;
                        else if (p == 9) on = rect->anchor_min_x == 0 && rect->anchor_max_x == 1 && rect->anchor_min_y == rect->anchor_max_y;
                        else if (p == 10) on = rect->anchor_min_y == 0 && rect->anchor_max_y == 1 && rect->anchor_min_x == rect->anchor_max_x;
                        else on = rect->anchor_min_x == 0 && rect->anchor_max_x == 1 && rect->anchor_min_y == 0 && rect->anchor_max_y == 1;
                        if (button(split(grid, col, 4), labels[p], on)) { ed_ui_layout(d, game_screen()); ed_ui_set_anchor_preset(d, id, p); }
                    }
                }
                ar = row(&l, ROW_H);
                r2d_ui_label(r2d_v2(ar.x, ar.y + 5), "size");
                const cw_ui_layout *lay = cw_comp_has(w, id, d->gw.ui.layout) ? cw_comp_ptr(w, id, d->gw.ui.layout) : NULL;
                snprintf(text, sizeof(text), "%s%.0fx%.0f at %.0f,%.0f", lay ? "" : "?", lay ? (double)lay->rw : 0.0, lay ? (double)lay->rh : 0.0, lay ? (double)lay->rx : 0.0, lay ? (double)lay->ry : 0.0);
                r2d_ui_label_colored(r2d_v2(ar.x + 104, ar.y + 5), r2d_rgb(150, 155, 165), text);
                if (button(r2d_rect_make(ar.x + ar.w - 104, ar.y, 104, ar.h), rect->parent == CW_INVALID_ENTITY ? "into 2nd sel." : "to canvas", false)) {
                    ed_ui_layout(d, game_screen());
                    if (rect->parent != CW_INVALID_ENTITY) ed_ui_set_parent(d, id, CW_INVALID_ENTITY);
                    else {   /* the second selected element becomes the parent */
                        cw_entity_id other = CW_INVALID_ENTITY;
                        for (size_t k = 0; k < d->selection_count; ++k) if (d->selection[k] != id && ed_is_ui(d, d->selection[k])) { other = d->selection[k]; break; }
                        if (other == CW_INVALID_ENTITY) ed_status(d, true, "shift-click the element that should become the parent first");
                        else ed_ui_set_parent(d, id, other);
                    }
                }
            } else l.y += (ROW_H + 4) * 3;
        }
        for (size_t f = 0; f < t->field_count; ++f) {
            if (t->fields[f].semantic == CW_SEM_RUNTIME) continue;
            if (l.y >= top - ROW_H && l.y + ROW_H <= bottom) {
                if (runtime) {   /* read-only: generated state is the systems' business */
                    r2d_rect rr = row(&l, ROW_H);
                    char text[64];
                    cw_field_format(&t->fields[f], cw_comp_ptr(w, id, ct), text, sizeof(text));
                    r2d_ui_label(r2d_v2(rr.x, rr.y + 5), t->fields[f].name);
                    r2d_ui_label_colored(r2d_v2(rr.x + 104, rr.y + 5), green, text);
                } else draw_field(&l, id, ct, &t->fields[f]);
            } else l.y += ROW_H + 4;
            if (!cw_world_is_valid(w, id) || !cw_comp_has(w, id, ct)) break;
        }
    }
    /* add component */
    l.y += 6;
    if (l.y + ROW_H <= bottom && l.y >= top) r2d_ui_label_dim(r2d_v2(l.x, l.y), "add component");
    l.y += r2d_text_height() + 4;
    int column = 0;
    r2d_rect line = r2d_rect_make(0, 0, 0, 0);
    for (unsigned c = 0; c < CW_MAX_COMPONENTS; ++c) {
        cw_component_id ct = (cw_component_id)c;
        const cw_type_info *t = cw_comp_type(w, ct);
        if (!t || (t->flags & (CW_TYPE_TRANSIENT | CW_TYPE_INTERNAL)) || cw_comp_has(w, id, ct)) continue;
        if ((t->flags & CW_TYPE_RUNTIME) && !d->playing) continue;   /* generated from config, not authored */
        if (column == 0) { line = r2d_rect_make(l.x, l.y, l.w, ROW_H); l.y += ROW_H + 4; }
        if (line.y >= top && line.y + ROW_H <= bottom && button(split(line, column, 2), t->name, false)) ed_add_component(d, id, ct);
        column = (column + 1) % 2;
    }
    l.y += 8;
    if (l.y + ROW_H <= bottom && l.y >= top) {
        r2d_rect dr = r2d_rect_make(l.x, l.y, l.w, ROW_H);
        if (r2d_ui_button_colored(split(dr, 0, 2), "duplicate (Ctrl+D)", plain)) { ed_duplicate_selection(d, 30, 30); selection_to_r2d(); }
        l.y += ROW_H + 6;
        if (l.y + ROW_H <= bottom) {
            r2d_ui_label_dim(r2d_v2(l.x, l.y), d->selection_count > 1 ? "save the selection as a prefab" : "save as a prefab");
            l.y += r2d_text_height() + 4;
            r2d_rect pr = r2d_rect_make(l.x, l.y, l.w, ROW_H);
            if (!ui.prefab_name[0]) { ed_label(d, id, ui.prefab_name, sizeof(ui.prefab_name)); for (char *c = ui.prefab_name; *c; ++c) if (*c == ' ') *c = '_'; }
            bool go = textfield(r2d_rect_make(pr.x, pr.y, pr.w - 66, pr.h), &ui.prefab_name, ui.prefab_name, sizeof(ui.prefab_name));
            if (button(r2d_rect_make(pr.x + pr.w - 62, pr.y, 62, pr.h), "save", false) || go) { if (ed_save_prefab(d, ui.prefab_name)) ui.prefab_name[0] = 0; }
        }
        if (r2d_ui_button_colored(split(dr, 1, 2), "delete (Del)", r2d_rgb(120, 48, 44))) { ed_delete_selection(d); selection_to_r2d(); }
    }
}
/* the settings panel: the player's window, then every rendering tunable */
static void draw_tunable_row(Layout *l, const r2d_tunable_set *set, const r2d_tunable *t, size_t index) {
    r2d_rect r = row(l, ROW_H);
    char value[64];
    r2d_tunables_get(set, t, value, sizeof(value));
    r2d_ui_label(r2d_v2(r.x, r.y + 5), t->key);
    float label_w = r2d_text_width("M") * 18;
    float vx = r.x + label_w, vw = r.w - label_w;
    static char buffers[512][64];
    static bool seeded[512];
    const void *key = &buffers[index % 512];
    switch (t->type) {
        case R2D_TUNE_BOOL:
            if (button(r2d_rect_make(vx, r.y, 70, r.h), value, value[0] == 't')) r2d_tunables_adjust(set, t, 1);
            break;
        case R2D_TUNE_ENUM:
            if (button(r2d_rect_make(vx, r.y, vw - 62, r.h), value, false)) r2d_tunables_adjust(set, t, 1);
            if (button(r2d_rect_make(r.x + r.w - 58, r.y, 27, r.h), "-", false)) r2d_tunables_adjust(set, t, -1);
            if (button(r2d_rect_make(r.x + r.w - 27, r.y, 27, r.h), "+", false)) r2d_tunables_adjust(set, t, 1);
            break;
        case R2D_TUNE_STRING:
            if (hui_focus_key() != key || !seeded[index % 512]) { snprintf(buffers[index % 512], 64, "%s", value); seeded[index % 512] = true; }
            if (textfield(r2d_rect_make(vx, r.y, vw, r.h), key, buffers[index % 512], 64)) {
                if (!r2d_tunables_set(set, t, buffers[index % 512])) ed_status(&ui.doc, true, "'%s' is not valid for %s.%s", buffers[index % 512], t->section, t->key);
                seeded[index % 512] = false;
            }
            break;
        default:
            if (hui_focus_key() != key) snprintf(buffers[index % 512], 64, "%s", value);
            if (textfield(r2d_rect_make(vx, r.y, vw - 62, r.h), key, buffers[index % 512], 64)) {
                if (!r2d_tunables_set(set, t, buffers[index % 512])) ed_status(&ui.doc, true, "'%s' is not valid for %s.%s (%g..%g)", buffers[index % 512], t->section, t->key, (double)t->min, (double)t->max);
            }
            if (button(r2d_rect_make(r.x + r.w - 58, r.y, 27, r.h), "-", false)) r2d_tunables_adjust(set, t, -1);
            if (button(r2d_rect_make(r.x + r.w - 27, r.y, 27, r.h), "+", false)) r2d_tunables_adjust(set, t, 1);
            break;
    }
}
static void panel_preferences(r2d_rect panel, void *user) {
    (void)user;
    Layout layout = content_layout(panel), *l = &layout;
    heading(l, memcmp(&ui.prefs, &ui.prefs_saved, sizeof(ui.prefs)) ? "EDITOR PREFERENCES*" : "EDITOR PREFERENCES");
    char path[512];
    ed_settings_path(path, sizeof(path));
    const char *name = strrchr(path, '/');
    label_line(l, r2d_rgb(150, 155, 165), path[0] ? (name ? name + 1 : path) : "(no config location: set CW_EDITOR_CONFIG)");
    r2d_rect r = row(l, ROW_H);
    if (button(split(r, 0, 2), "save", memcmp(&ui.prefs, &ui.prefs_saved, sizeof(ui.prefs)) != 0)) {
        if (ed_settings_save(&ui.prefs, NULL)) { ui.prefs_saved = ui.prefs; ed_status(&ui.doc, false, "preferences saved to %s", path); }
        else ed_status(&ui.doc, true, "cannot save preferences to %s", path);
    }
    if (button(split(r, 1, 2), "defaults", false)) { ed_settings_defaults(&ui.prefs); apply_prefs(); }
    size_t count;
    const r2d_tunable *defs = ed_settings_defs(&count);
    r2d_tunable_set set = ed_settings_set(&ui.prefs);
    const char *section = NULL;
    float bottom = panel.y + panel.h - 8;
    for (size_t i = 0; i < count && l->y + ROW_H <= bottom; ++i) {
        if (!section || strcmp(section, defs[i].section) != 0) { section = defs[i].section; heading(l, section); }
        float before = ui.prefs.text_scale;
        draw_tunable_row(l, &set, &defs[i], 300 + i);
        if (ui.prefs.text_scale != before) r2d_set_text_scale(ui.prefs.text_scale);
        ui.grid = ui.prefs.grid == ui.prefs_saved.grid ? ui.grid : ui.prefs.grid;
    }
    label_line(l, r2d_rgb(150, 155, 165), "zoom step applies on the next start");
    r2d_rect lr = row(l, ROW_H);
    if (button(split(lr, 0, 2), "reset layout", false)) { hd_clear(&ui.dock); default_layout(); }
}
static void panel_project(r2d_rect panel, void *user) {
    (void)user;
    ed_document *d = &ui.doc;
    Layout l = content_layout(panel);
    if (!d->project_open) { label_line(&l, r2d_rgb(150, 155, 165), "no project"); return; }
    char text[CW_GAME_PATH_CAPACITY * 2 + 32];
    heading(&l, "PROJECT SETTINGS");
    r2d_rect r = row(&l, ROW_H);
    r2d_ui_label(r2d_v2(r.x, r.y + 5), "window");
    textfield(r2d_rect_make(r.x + 118, r.y, 60, r.h), &ui.win_w, ui.win_w, sizeof(ui.win_w));
    r2d_ui_label(r2d_v2(r.x + 184, r.y + 5), "x");
    textfield(r2d_rect_make(r.x + 198, r.y, 60, r.h), &ui.win_h, ui.win_h, sizeof(ui.win_h));
    r = row(&l, ROW_H);
    r2d_ui_label(r2d_v2(r.x, r.y + 5), "title");
    textfield(r2d_rect_make(r.x + 118, r.y, r.w - 118, r.h), &ui.win_title, ui.win_title, sizeof(ui.win_title));
    r = row(&l, ROW_H);
    bool window_dirty = atoi(ui.win_w) != d->project.window_width || atoi(ui.win_h) != d->project.window_height || strcmp(ui.win_title, d->project.title) != 0;
    if (button(split(r, 0, 2), window_dirty ? "save project*" : "save project", window_dirty)) save_window_settings();
    label_line(&l, r2d_rgb(150, 155, 165), "the player and exported games use these");
    /* the interface canvas */
    l.y += 4;
    r2d_ui_label_dim(r2d_v2(l.x, l.y), "interface: designed for");
    l.y += r2d_text_height() + 4;
    r = row(&l, ROW_H);
    r2d_ui_label(r2d_v2(r.x, r.y + 5), "reference");
    if (hui_focus_key() != &ui.ui_w && hui_focus_key() != &ui.ui_h && atof(ui.ui_w) != d->project.ui.width) snprintf(ui.ui_w, sizeof(ui.ui_w), "%g", (double)d->project.ui.width);
    if (hui_focus_key() != &ui.ui_w && hui_focus_key() != &ui.ui_h && atof(ui.ui_h) != d->project.ui.height) snprintf(ui.ui_h, sizeof(ui.ui_h), "%g", (double)d->project.ui.height);
    bool commit = textfield(r2d_rect_make(r.x + 118, r.y, 60, r.h), &ui.ui_w, ui.ui_w, sizeof(ui.ui_w));
    r2d_ui_label(r2d_v2(r.x + 184, r.y + 5), "x");
    commit = textfield(r2d_rect_make(r.x + 198, r.y, 60, r.h), &ui.ui_h, ui.ui_h, sizeof(ui.ui_h)) || commit;
    if (commit) ed_set_ui_canvas(d, (float)atof(ui.ui_w), (float)atof(ui.ui_h), d->project.ui.mode);
    r = row(&l, ROW_H);
    r2d_ui_label(r2d_v2(r.x, r.y + 5), "scale");
    r2d_rect modes = r2d_rect_make(r.x + 118, r.y, r.w - 118, r.h);
    for (int m = 0; m < CW_UI_SCALE_MODES; ++m)
        if (button(split(modes, m, CW_UI_SCALE_MODES), cw_ui_scale_mode_name((cw_ui_scale_mode)m), d->project.ui.mode == (cw_ui_scale_mode)m) && d->project.ui.mode != (cw_ui_scale_mode)m)
            ed_set_ui_canvas(d, d->project.ui.width, d->project.ui.height, (cw_ui_scale_mode)m);
    label_line(&l, r2d_rgb(150, 155, 165), "expand: the design always fits (scale = the smaller ratio);");
    label_line(&l, r2d_rgb(150, 155, 165), "shrink: it always fills; width/height: one axis; pixels: never scale");
    snprintf(text, sizeof(text), "module: %s", d->project.module_rel);
    label_line(&l, r2d_rgb(150, 155, 165), text);
    for (size_t i = 0; i < d->project.source_count; ++i) { snprintf(text, sizeof(text), "source: %s", d->project.sources_rel[i]); label_line(&l, r2d_rgb(150, 155, 165), text); }
    snprintf(text, sizeof(text), "scenes: %s   main: %s", d->project.scenes_rel, d->project.main_scene_rel[0] ? d->project.main_scene_rel : "-");
    label_line(&l, r2d_rgb(150, 155, 165), text);
    snprintf(text, sizeof(text), "assets: %s   render: %s", d->project.assets_rel, d->project.render_rel);
    label_line(&l, r2d_rgb(150, 155, 165), text);
}
static void panel_rendering(r2d_rect panel, void *user) {
    (void)user;
    ed_document *d = &ui.doc;
    Layout l = content_layout(panel);
    if (!d->project_open) { label_line(&l, r2d_rgb(150, 155, 165), "no project"); return; }
    char path[CW_GAME_PATH_CAPACITY], text[CW_GAME_PATH_CAPACITY * 2 + 32];
    r2d_post_params *p = r2d_post_params_ptr();
    r2d_rect r;
    heading(&l, render_dirty() ? "RENDERING*" : "RENDERING");
    if (!p) { label_line(&l, red, "no post-process stack (graphics setup failed)"); return; }
    ed_render_path(d, path, sizeof(path));
    const char *name = strrchr(path, '/');
    snprintf(text, sizeof(text), "%s%s", name ? name + 1 : path, ed_render_settings_exist(d) ? "" : " (not saved yet: defaults)");
    label_line(&l, r2d_rgb(150, 155, 165), text);
    r = row(&l, ROW_H);
    if (button(split(r, 0, 3), "save", render_dirty())) save_render_settings();
    if (button(split(r, 1, 3), "reload", false)) load_render_settings();
    if (button(split(r, 2, 3), "defaults", false)) { r2d_post_params_defaults(p); r2d_post_set_tier(p->quality.tier); }
    r = row(&l, ROW_H);
    if (r2d_ui_toggle(split(r, 0, 2), "preview (L)", &ui.lights)) { /* toggled */ }
    r2d_post_stats ps = r2d_post_get_stats();
    snprintf(text, sizeof(text), "%s  %d passes  tier %s%s", ps.active ? "active" : "off", ps.passes_run, r2d_tier_name(ps.tier), ps.tier_automatic ? " (auto)" : "");
    r2d_ui_label_dim(r2d_v2(split(r, 1, 2).x + 4, r.y + 5), text);

    /* section tabs */
    size_t count;
    const r2d_tunable *defs = r2d_post_tunable_defs(&count);
    r2d_tunable_set set = r2d_post_tunable_set(p);
    const char *sections[24];
    int nsections = 0;
    for (size_t i = 0; i < count && nsections < 24; ++i) {
        bool seen = false;
        for (int k = 0; k < nsections; ++k) if (strcmp(sections[k], defs[i].section) == 0) seen = true;
        if (!seen) sections[nsections++] = defs[i].section;
    }
    if (ui.settings_section >= nsections) ui.settings_section = 0;
    int column = 0;
    r2d_rect line = r2d_rect_make(0, 0, 0, 0);
    for (int i = 0; i < nsections; ++i) {
        if (column == 0) line = row(&l, ROW_H);
        const char *label = sections[i];
        if (strcmp(label, "chromatic_aberration") == 0) label = "chroma";
        else if (strcmp(label, "motion_blur") == 0) label = "motion";
        else if (strncmp(label, "tier_", 5) == 0) label += 5;
        if (button(split(line, column, 4), label, i == ui.settings_section)) { ui.settings_section = i; ui.settings_scroll = 0; }
        column = (column + 1) % 4;
    }
    l.y += 4;
    /* rows of the selected section, scrollable */
    float top = l.y, bottom = panel.y + panel.h - 8;
    if (r2d_rect_contains(panel, r2d_mouse()) && r2d_wheel() != 0) ui.settings_scroll -= (int)(r2d_wheel() * 40);
    if (ui.settings_scroll < 0) ui.settings_scroll = 0;
    l.y -= (float)ui.settings_scroll;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(defs[i].section, sections[ui.settings_section]) != 0) continue;
        if (l.y >= top - ROW_H && l.y + ROW_H <= bottom) draw_tunable_row(&l, &set, &defs[i], i);
        else l.y += ROW_H + 4;
    }
    if (strcmp(sections[ui.settings_section], "lighting") == 0 || strcmp(sections[ui.settings_section], "fog") == 0) {
        r2d_lighting_stats ls = r2d_lighting_get_stats();
        snprintf(text, sizeof(text), "%d lights  %d casters  %.1f ms shadows", ls.lights, ls.occluders, ls.shadow_seconds * 1000.0);
        if (l.y + ROW_H <= bottom && l.y >= top) r2d_ui_label_dim(r2d_v2(l.x, l.y), text);
    }
}
static void draw_status_bar(void) {
    r2d_set_space(R2D_SPACE_SCREEN);
    r2d_vec2 v = r2d_viewport();
    r2d_rect bar = r2d_rect_make(0, v.y - STATUS_H, v.x, STATUS_H);
    r2d_ui_panel(bar);
    ed_document *d = &ui.doc;
    r2d_ui_label_colored(r2d_v2(14, bar.y + 8), d->status_error ? red : green, d->status);
    char line[256];
    if (d->world_ready)
        snprintf(line, sizeof(line), "%s  %zu entities  tick %" PRIu64 "  %s  zoom %.2f  %.0f fps  undo: %s",
                 d->playing ? (d->paused ? "PAUSED" : "PLAYING") : "editing", ed_entity_count(d), d->gw.world.tick,
                 ui.tool == TOOL_PLACE ? "place tool" : "select tool", (double)r2d_camera_ptr()->zoom, (double)ui.fps,
                 ed_can_undo(d) ? ed_undo_label(d) : "-");
    else snprintf(line, sizeof(line), "%.0f fps", (double)ui.fps);
    r2d_ui_label_dim(r2d_v2(14, bar.y + 30), line);
    const char *hint = "Q select  1-9 place  Del  Ctrl+D dup  Ctrl+Z/Y undo  Ctrl+S save  F5 play  F7 build  F9 run  ` log";
    float hw = r2d_text_width(hint);
    if (v.x - hw > 700) r2d_ui_label_dim(r2d_v2(v.x - hw - 14, bar.y + 30), hint);
}
static void panel_log(r2d_rect panel, void *user) {
    (void)user;
    r2d_ui_label(r2d_v2(panel.x + 8, panel.y + 6), ui.doc.last_build_status == 0 ? "BUILD LOG (ok)" : (ui.doc.last_build_status < 0 ? "BUILD LOG" : "BUILD LOG (failed)"));
    if (r2d_ui_button(r2d_rect_make(panel.x + panel.w - 60, panel.y + 4, 52, 20), "clear")) ui.doc.log[0] = 0;
    /* the last lines that fit */
    const char *log = ui.doc.log;
    int max_lines = (int)((panel.h - 34) / (r2d_text_height() + 2));
    int lines = 0;
    for (const char *p = log; *p; ++p) lines += *p == '\n';
    const char *start = log;
    for (int skip = lines - max_lines; skip > 0 && *start; --skip) { start = strchr(start, '\n'); start = start ? start + 1 : log + strlen(log); }
    float y = panel.y + 30;
    size_t max_chars = (size_t)((panel.w - 16) / (r2d_text_width("M") > 0 ? r2d_text_width("M") : 8));
    while (*start && y + r2d_text_height() <= panel.y + panel.h) {
        const char *nl = strchr(start, '\n');
        size_t len = nl ? (size_t)(nl - start) : strlen(start);
        char line[200];
        if (len > max_chars) len = max_chars;
        snprintf(line, sizeof(line), "%.*s", (int)(len < 190 ? len : 190), start);
        r2d_ui_label_colored(r2d_v2(panel.x + 8, y), strstr(line, "error") ? red : r2d_rgb(200, 205, 215), line);
        y += r2d_text_height() + 2;
        if (!nl) break;
        start = nl + 1;
    }
    if (!*log) r2d_ui_label_dim(r2d_v2(panel.x + 8, y), "nothing built yet (F7 builds the module, F8 exports the game)");
}

/* ---------------------------------------------------------------------------
 * the animation panel: clips, timeline, tracks, keys, preview and record
 * ------------------------------------------------------------------------- */
static float nice_step(float seconds_per_pixel) {
    static const float steps[] = {0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.5f, 1, 2, 5, 10, 30, 60};
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i)
        if (steps[i] / seconds_per_pixel >= 60) return steps[i];
    return 60;
}
static bool anim_button(float *x, r2d_rect r, const char *label, float w, bool on) {
    bool hit = button(r2d_rect_make(*x, r.y, w, r.h), label, on);
    *x += w + 4;
    return hit;
}
static void panel_animation(r2d_rect content, void *user) {
    (void)user;
    ed_document *d = &ui.doc;
    ea_state *a = &ui.anim;
    Layout l = content_layout(content);
    if (draw_not_ready(&l)) return;
    float left_w = 170;
    /* --- clips --- */
    Layout c = {content.x + 10, content.y + 8, left_w - 10};
    heading(&c, "CLIPS");
    const cw_asset *clips[32];
    size_t n = ea_list(d, clips, 32);
    for (size_t i = 0; i < n && c.y + ROW_H <= content.y + content.h - 70; ++i) {
        r2d_rect r = row(&c, ROW_H - 2);
        char name[CW_ASSET_NAME_CAPACITY];
        size_t len = strlen(clips[i]->name);
        snprintf(name, sizeof(name), "%.*s", (int)(len > 7 && strcmp(clips[i]->name + len - 7, ".cwanim") == 0 ? len - 7 : len), clips[i]->name);
        if (button(r, name, a->open && a->asset == clips[i]->id)) { ea_open(a, d, clips[i]->id); snprintf(ui.anim_length, sizeof(ui.anim_length), "%g", (double)a->clip.length); }
    }
    if (a->open && !a->asset) { r2d_rect r = row(&c, ROW_H - 2); button(r, a->clip.name, true); }
    c.y = content.y + content.h - 62;
    r2d_rect nr = row(&c, ROW_H);
    if (textfield(r2d_rect_make(nr.x, nr.y, nr.w - 50, nr.h), &ui.anim_name, ui.anim_name, sizeof(ui.anim_name)) || button(r2d_rect_make(nr.x + nr.w - 46, nr.y, 46, nr.h), "new", false)) {
        if (ea_new(a, d, ui.anim_name, 1.0f)) { ui.anim_name[0] = 0; snprintf(ui.anim_length, sizeof(ui.anim_length), "1"); }
    }
    nr = row(&c, ROW_H);
    if (button(split(nr, 0, 2), a->dirty ? "save*" : "save", a->dirty)) ea_save(a, d);
    if (r2d_ui_button_colored(split(nr, 1, 2), "delete", r2d_rgb(120, 48, 44)) && a->open && a->asset) ea_delete(a, d, a->asset);
    /* --- the clip --- */
    l.x = content.x + left_w + 10;
    l.w = content.w - left_w - 20;
    if (!a->open) {
        heading(&l, "ANIMATION");
        label_line(&l, r2d_rgb(150, 155, 165), "open a clip on the left or type a name and press new;");
        label_line(&l, r2d_rgb(150, 155, 165), "select an entity, add tracks from its fields, set keys at the playhead.");
        label_line(&l, r2d_rgb(150, 155, 165), "record turns inspector edits into keys; an Animation component plays clips in the game.");
        return;
    }
    cw_entity_id target = ea_target(a, d);
    char text[160], label[64] = "";
    if (target != CW_INVALID_ENTITY) ed_label(d, target, label, sizeof(label));
    snprintf(text, sizeof(text), "%s%s  %.2fs  target: %s%s", a->clip.name, a->dirty ? "*" : "", (double)a->clip.length,
             target == CW_INVALID_ENTITY ? "select an entity" : label, a->previewing ? " (previewing)" : "");
    label_line(&l, yellow, text);
    r2d_rect r = row(&l, ROW_H);
    float x = r.x;
    if (anim_button(&x, r, a->playing ? "pause" : "play", 52, a->playing)) { if (a->playing) ea_pause(a); else ea_play(a, d); }
    if (anim_button(&x, r, "stop", 44, false)) ea_stop(a, d);
    if (r2d_ui_button_colored(r2d_rect_make(x, r.y, 58, r.h), "record", a->record ? r2d_rgb(160, 50, 50) : plain)) a->record = !a->record;
    x += 68;
    for (int m = 0; m < CW_ANIM_MODES; ++m) if (anim_button(&x, r, cw_anim_mode_name((cw_anim_mode)m), 66, (int)a->clip.mode == m)) ea_set_mode(a, (cw_anim_mode)m);
    x += 6;
    r2d_ui_label(r2d_v2(x, r.y + 5), "length");
    x += 52;
    if (!ui.anim_length[0] || hui_focus_key() != &ui.anim_length) snprintf(ui.anim_length, sizeof(ui.anim_length), "%g", (double)a->clip.length);
    if (textfield(r2d_rect_make(x, r.y, 56, r.h), &ui.anim_length, ui.anim_length, sizeof(ui.anim_length))) { if (!ea_set_length(a, (float)atof(ui.anim_length))) ed_status(d, true, "length must be 0..3600 s"); }
    x += 62;
    snprintf(text, sizeof(text), "t %.2fs", (double)a->playhead);
    r2d_ui_label_colored(r2d_v2(x, r.y + 5), yellow, text);
    /* --- timeline --- */
    float label_w = 240, lane_x = l.x + label_w, lane_w = l.w - label_w;
    if (lane_w < 80) lane_w = 80;
    float spp = a->clip.length / lane_w;   /* seconds per pixel */
    r2d_rect ruler = row(&l, 18);
    r2d_rect lane_ruler = r2d_rect_make(lane_x, ruler.y, lane_w, ruler.h);
    r2d_draw_rect(lane_ruler, r2d_rgb(30, 33, 40));
    r2d_ui_block(lane_ruler);
    float step = nice_step(spp);
    for (float t = 0; t <= a->clip.length + 1e-4f; t += step) {
        float px = lane_x + t / a->clip.length * lane_w;
        r2d_draw_line(r2d_v2(px, ruler.y + ruler.h - 6), r2d_v2(px, ruler.y + ruler.h), 1, r2d_rgb(120, 126, 140));
        snprintf(text, sizeof(text), "%g", (double)t);
        if (px + r2d_text_width(text) < lane_x + lane_w) r2d_draw_text(r2d_v2(px + 2, ruler.y + 1), r2d_rgb(150, 155, 165), text);
    }
    bool over_ruler = r2d_rect_contains(lane_ruler, r2d_mouse());
    if (over_ruler && r2d_mouse_pressed(R2D_MOUSE_LEFT)) ui.anim_scrubbing = true;
    if (ui.anim_scrubbing) {
        if (!r2d_mouse_down(R2D_MOUSE_LEFT)) ui.anim_scrubbing = false;
        else ea_scrub(a, d, (r2d_mouse().x - lane_x) * spp);
    }
    /* --- tracks --- */
    float tracks_top = l.y, bottom = content.y + content.h - 8 - ROW_H * 2 - 12;
    for (size_t i = 0; i < a->clip.track_count && l.y + ROW_H <= bottom; ++i) {
        cw_anim_track *t = &a->clip.tracks[i];
        r = row(&l, ROW_H);
        bool resolvable = ea_track_field(a, d, i, NULL) != NULL;
        snprintf(text, sizeof(text), "%.17s.%s", t->component, t->field);
        text[18] = 0;
        r2d_ui_label_colored(r2d_v2(r.x, r.y + 5), resolvable ? white : r2d_rgb(150, 155, 165), text);
        if (button(r2d_rect_make(r.x + label_w - 70, r.y, 40, r.h), "+key", false)) ea_key_from_entity(a, d, i);
        if (r2d_ui_button_colored(r2d_rect_make(r.x + label_w - 26, r.y, 22, r.h), "x", r2d_rgb(110, 48, 44))) { ea_track_remove(a, i); break; }
        r2d_rect lane = r2d_rect_make(lane_x, r.y, lane_w, r.h);
        r2d_draw_rect(lane, (int)i == a->selected_track ? r2d_rgb(36, 40, 50) : r2d_rgb(28, 30, 36));
        r2d_ui_block(lane);
        if (r2d_rect_contains(lane, r2d_mouse()) && r2d_mouse_pressed(R2D_MOUSE_LEFT) && ui.anim_drag_track < 0) {
            /* a key under the mouse starts a drag; empty lane scrubs */
            bool hit = false;
            for (size_t k = 0; k < t->key_count; ++k) {
                float kx = lane_x + t->keys[k].time / a->clip.length * lane_w;
                if (fabsf(r2d_mouse().x - kx) <= 6) { a->selected_track = (int)i; a->selected_key = (int)k; ui.anim_drag_track = (int)i; ui.anim_drag_key = (int)k; hit = true; break; }
            }
            if (!hit) { a->selected_track = (int)i; a->selected_key = -1; ui.anim_scrubbing = true; ea_scrub(a, d, (r2d_mouse().x - lane_x) * spp); }
        }
        for (size_t k = 0; k < t->key_count; ++k) {
            float kx = lane_x + t->keys[k].time / a->clip.length * lane_w, ky = r.y + r.h * 0.5f;
            bool sel = (int)i == a->selected_track && (int)k == a->selected_key;
            r2d_vec2 dmd[4] = {{kx, ky - 6}, {kx + 6, ky}, {kx, ky + 6}, {kx - 6, ky}};
            r2d_draw_polygon(dmd, 4, sel ? yellow : (t->keys[k].interp == CW_INTERP_STEP ? r2d_rgb(200, 140, 90) : r2d_rgb(150, 190, 255)));
        }
    }
    if (ui.anim_drag_track >= 0) {
        if (!r2d_mouse_down(R2D_MOUSE_LEFT)) ui.anim_drag_track = ui.anim_drag_key = -1;
        else if ((size_t)ui.anim_drag_track < a->clip.track_count) {
            float t = (r2d_mouse().x - lane_x) * spp;
            if (ui.prefs.snap) t = roundf(t / step) * step;
            if (t < 0) t = 0;
            if (t > a->clip.length) t = a->clip.length;
            int moved = ea_key_move(a, (size_t)ui.anim_drag_track, (size_t)ui.anim_drag_key, t);
            if (moved >= 0) ui.anim_drag_key = moved;
        }
    }
    /* the playhead over the lanes */
    float phx = lane_x + a->playhead / a->clip.length * lane_w;
    r2d_draw_line(r2d_v2(phx, ruler.y), r2d_v2(phx, l.y), 1.5f, yellow);
    if (a->clip.track_count == 0 && l.y + ROW_H <= bottom) label_line(&l, r2d_rgb(150, 155, 165), "no tracks yet: + track adds one of the target's fields");
    /* --- add track --- */
    if (l.y + ROW_H <= bottom) {
        r = row(&l, ROW_H);
        if (button(r2d_rect_make(r.x, r.y, 90, r.h), "+ track", ui.anim_add_open)) ui.anim_add_open = !ui.anim_add_open;
        if (ui.anim_add_open && target != CW_INVALID_ENTITY) {
            cw_world *w = &d->gw.world;
            int column = 0;
            r2d_rect line = r2d_rect_make(0, 0, 0, 0);
            for (unsigned ct = 0; ct < CW_MAX_COMPONENTS && l.y + ROW_H <= bottom + ROW_H; ++ct) {
                const cw_type_info *ty = cw_comp_type(w, (cw_component_id)ct);
                if (!ty || (ty->flags & (CW_TYPE_TRANSIENT | CW_TYPE_INTERNAL)) || !cw_comp_has(w, target, (cw_component_id)ct)) continue;
                for (size_t f = 0; f < ty->field_count; ++f) {
                    if (!cw_anim_field_animatable(&ty->fields[f]) || ty->fields[f].semantic == CW_SEM_RUNTIME) continue;
                    if (cw_anim_track_find(&a->clip, ty->name, ty->fields[f].name) >= 0) continue;
                    if (column == 0) line = row(&l, ROW_H - 2);
                    snprintf(text, sizeof(text), "%s.%s", ty->name, ty->fields[f].name);
                    if (button(split(line, column, 4), text, false)) { ea_track_add(a, d, (cw_component_id)ct, &ty->fields[f]); ui.anim_add_open = false; }
                    column = (column + 1) % 4;
                }
            }
        } else if (ui.anim_add_open) label_line(&l, r2d_rgb(150, 155, 165), "select an entity first");
    }
    /* --- the selected key --- */
    l.y = content.y + content.h - 8 - ROW_H;
    if (a->selected_track >= 0 && (size_t)a->selected_track < a->clip.track_count && a->selected_key >= 0 && (size_t)a->selected_key < a->clip.tracks[a->selected_track].key_count) {
        cw_anim_track *t = &a->clip.tracks[a->selected_track];
        cw_anim_key *k = &t->keys[a->selected_key];
        r = r2d_rect_make(l.x, l.y, l.w, ROW_H);
        x = r.x;
        snprintf(text, sizeof(text), "key %s.%s", t->component, t->field);
        r2d_ui_label(r2d_v2(x, r.y + 5), text);
        x += r2d_text_width(text) + 10;
        r2d_ui_label_dim(r2d_v2(x, r.y + 5), "time");
        x += 40;
        if (hui_focus_key() != &ui.key_time) snprintf(ui.key_time, sizeof(ui.key_time), "%.3g", (double)k->time);
        if (textfield(r2d_rect_make(x, r.y, 60, r.h), &ui.key_time, ui.key_time, sizeof(ui.key_time))) { if (ea_key_move(a, (size_t)a->selected_track, (size_t)a->selected_key, (float)atof(ui.key_time)) < 0) ed_status(d, true, "another key is at that time"); }
        x += 66;
        r2d_ui_label_dim(r2d_v2(x, r.y + 5), "value");
        x += 46;
        if (hui_focus_key() != &ui.key_value) snprintf(ui.key_value, sizeof(ui.key_value), "%.5g", (double)k->value);
        if (textfield(r2d_rect_make(x, r.y, 80, r.h), &ui.key_value, ui.key_value, sizeof(ui.key_value))) { ea_key_set(a, (size_t)a->selected_track, k->time, (float)atof(ui.key_value), k->interp); if (a->previewing) ea_scrub(a, d, a->playhead); }
        x += 86;
        static const char *const interp_labels[CW_INTERP_COUNT] = {"linear", "step", "ease", "in", "out"};
        for (int i = 0; i < CW_INTERP_COUNT; ++i) {
            float bw = i < 3 ? 56 : 36;
            if (button(r2d_rect_make(x, r.y, bw, r.h), interp_labels[i], (int)k->interp == i)) ea_key_set_interp(a, (size_t)a->selected_track, (size_t)a->selected_key, (cw_interp)i);
            x += bw + 4;
        }
        if (r2d_ui_button_colored(r2d_rect_make(x, r.y, 60, r.h), "delete", r2d_rgb(120, 48, 44))) ea_key_remove(a, (size_t)a->selected_track, (size_t)a->selected_key);
    } else r2d_ui_label_dim(r2d_v2(l.x, l.y + 5), "click a key to edit it; drag keys to retime; click a lane or the ruler to scrub");
    (void)tracks_top;
}

/* ---------------------------------------------------------------------------
 * the material panel: materials and sprite shaders
 * ------------------------------------------------------------------------- */
static bool material_dirty(void) { return ui.material_open && !r2d_material_equal(&ui.material, &ui.material_saved); }
static void material_select(uint32_t id) {
    const cw_asset *a = cw_assets_find(&ui.doc.assets, id);
    if (!a) return;
    char path[CW_GAME_PATH_CAPACITY], error[160];
    cw_assets_resolve(&ui.doc.assets, a->path, path, sizeof(path));
    if (!r2d_material_load(&ui.material, path, error, sizeof(error))) { ed_status(&ui.doc, true, "%s: %s", a->name, error); return; }
    ui.material_saved = ui.material;
    ui.material_open = true;
    ui.material_asset = id;
    ui.material_picker = 0;
    ed_status(&ui.doc, false, "material %s: %s effect", ui.material.name, r2d_effect_name(ui.material.effect));
}
static void material_save(void) {
    if (!ui.material_open) return;
    char rel[160], text[2048];
    snprintf(rel, sizeof(rel), "materials/%s.cwmat", ui.material.name);
    size_t len = r2d_material_format(&ui.material, text, sizeof(text));
    const cw_asset *a = ed_write_asset(&ui.doc, rel, text, len);
    if (!a) return;
    ui.material_asset = a->id;
    ui.material_saved = ui.material;
    host_canvas_reload(&ui.canvas);   /* sprites pick up the new version */
    ed_status(&ui.doc, false, "material %s saved", a->name);
}
static void material_new(const char *name) {
    if (!name || !*name || strpbrk(name, "/\\\" \r\n")) { ed_status(&ui.doc, true, "give the material a file name (no spaces, slashes or quotes)"); return; }
    r2d_material_defaults(&ui.material, name);
    ui.material_saved = ui.material;
    ui.material_saved.name[0] = 0;   /* dirty until saved */
    ui.material_open = true;
    ui.material_asset = 0;
    ui.material_picker = 0;
    ed_status(&ui.doc, false, "new material %s: pick a texture and an effect, then save", name);
}
static void shader_new(const char *name, int example) {
    if (!name || !*name || strpbrk(name, "/\\\" \r\n")) { ed_status(&ui.doc, true, "give the shader a file name"); return; }
    const char *source = r2d_shader_example_source((size_t)(example < 0 ? 0 : example));
    if (!source) source = r2d_shader_example();
    char rel[160];
    snprintf(rel, sizeof(rel), "shaders/%s.cwshader", name);
    const cw_asset *a = ed_write_asset(&ui.doc, rel, source, strlen(source));
    if (!a) return;
    snprintf(ui.material.shader, sizeof(ui.material.shader), "%s", a->name);
    r2d_material_set_effect(&ui.material, R2D_EFFECT_CUSTOM);
    host_canvas_reload(&ui.canvas);
    ed_status(&ui.doc, false, "shader %s created from the '%s' starter: edit it in any editor, the preview reloads on save", a->name, r2d_shader_example_name((size_t)example) ? r2d_shader_example_name((size_t)example) : "rim");
}
/* a row's label, hidden with the row when the column is clipped */
static void row_label(r2d_rect r, const char *text) { if (hui_visible(r)) r2d_ui_label(r2d_v2(r.x, r.y + 5), text); }
static void draw_number(Layout *l, const char *label, float *value, float lo, float hi, float step, const void *key, char *buffer, size_t capacity) {
    r2d_rect r = row(l, ROW_H);
    row_label(r, label);
    float vx = r.x + 110;
    if (hui_focus_key() != key) snprintf(buffer, capacity, "%.4g", (double)*value);
    if (textfield(r2d_rect_make(vx, r.y, r.w - 110 - 62, r.h), key, buffer, capacity)) { float v = (float)atof(buffer); *value = v < lo ? lo : (v > hi ? hi : v); }
    if (button(r2d_rect_make(r.x + r.w - 58, r.y, 27, r.h), "-", false)) { *value -= step; if (*value < lo) *value = lo; }
    if (button(r2d_rect_make(r.x + r.w - 27, r.y, 27, r.h), "+", false)) { *value += step; if (*value > hi) *value = hi; }
}
static void panel_material(r2d_rect content, void *user) {
    (void)user;
    ed_document *d = &ui.doc;
    Layout l = content_layout(content);
    if (draw_not_ready(&l)) return;
    float left_w = 170;
    Layout c = {content.x + 10, content.y + 8, left_w - 10};
    heading(&c, "MATERIALS");
    size_t shown = 0;
    for (size_t i = 0; i < d->assets.count && c.y + ROW_H <= content.y + content.h - 70; ++i) {
        const cw_asset *a = &d->assets.items[i];
        if (a->type != CW_ASSET_MATERIAL) continue;
        r2d_rect r = row(&c, ROW_H - 2);
        char name[CW_ASSET_NAME_CAPACITY];
        size_t len = strlen(a->name);
        snprintf(name, sizeof(name), "%.*s", (int)(len > 6 && strcmp(a->name + len - 6, ".cwmat") == 0 ? len - 6 : len), a->name);
        if (button(r, name, ui.material_open && ui.material_asset == a->id)) material_select(a->id);
        ++shown;
    }
    if (!shown) label_line(&c, r2d_rgb(150, 155, 165), "none yet");
    if (ui.material_open && !ui.material_asset) { r2d_rect r = row(&c, ROW_H - 2); button(r, ui.material.name, true); }
    c.y = content.y + content.h - 62;
    r2d_rect nr = row(&c, ROW_H);
    if (textfield(r2d_rect_make(nr.x, nr.y, nr.w - 50, nr.h), &ui.material_name, ui.material_name, sizeof(ui.material_name)) || button(r2d_rect_make(nr.x + nr.w - 46, nr.y, 46, nr.h), "new", false)) {
        if (ui.material_name[0]) { material_new(ui.material_name); ui.material_name[0] = 0; }
    }
    nr = row(&c, ROW_H);
    if (button(split(nr, 0, 2), material_dirty() ? "save*" : "save", material_dirty())) material_save();
    if (r2d_ui_button_colored(split(nr, 1, 2), "delete", r2d_rgb(120, 48, 44)) && ui.material_open && ui.material_asset) {
        ed_remove_asset(d, ui.material_asset, true);
        ui.material_open = false;
        host_canvas_reload(&ui.canvas);
    }
    /* --- the material --- */
    l.x = content.x + left_w + 10;
    l.w = content.w - left_w - 20;
    if (!ui.material_open) {
        heading(&l, "MATERIAL");
        label_line(&l, r2d_rgb(150, 155, 165), "pick a material on the left or type a name and press new.");
        label_line(&l, r2d_rgb(150, 155, 165), "a material is a texture (an imported image), an effect with its");
        label_line(&l, r2d_rgb(150, 155, 165), "parameters, a blend mode and a tint; entities show it through a Sprite component.");
        return;
    }
    r2d_material *m = &ui.material;
    /* the column scrolls; rows outside the panel are neither drawn nor clickable */
    float top = content.y + 4, bottom = content.y + content.h - 6;
    if (r2d_rect_contains(content, r2d_mouse()) && r2d_wheel() != 0 && !r2d_rect_contains(r2d_rect_make(content.x, content.y, left_w, content.h), r2d_mouse()))
        ui.material_scroll -= (int)(r2d_wheel() * 40);
    if (ui.material_scroll < 0) ui.material_scroll = 0;
    l.y -= (float)ui.material_scroll;
    hui_clip(top, bottom);
    /* preview on the right */
    float preview = 150;
    r2d_rect pv = r2d_rect_make(content.x + content.w - preview - 12, content.y + 12, preview, preview);
    r2d_ui_block(pv);
    for (int cy = 0; cy < 6; ++cy) for (int cx = 0; cx < 6; ++cx)
        r2d_draw_rect(r2d_rect_make(pv.x + cx * preview / 6, pv.y + cy * preview / 6, preview / 6, preview / 6), (cx + cy) & 1 ? r2d_rgb(60, 62, 70) : r2d_rgb(44, 46, 52));
    {
        const host_texture *t = m->texture[0] ? host_canvas_texture(&ui.canvas, cw_asset_id(m->texture)) : NULL;
        const host_shader *sh = m->effect == R2D_EFFECT_CUSTOM && m->shader[0] ? host_canvas_shader(&ui.canvas, cw_asset_id(m->shader)) : NULL;
        if (ui.material_preview == 0) {
            r2d_draw_sprite_screen(m, t && !t->missing ? &t->texture : NULL, sh ? &sh->shader : NULL, r2d_rect_center(pv), r2d_v2(preview - 16, preview - 16), 0, r2d_rgb(255, 255, 255), NULL, NULL);
        } else {
            /* an element: a button-sized quad that sees the mouse like a UiButton would, a fill that cycles */
            r2d_rect el = r2d_rect_make(pv.x + 8, pv.y + 66, preview - 16, 56);
            cw_ui_layout *st = &ui.preview_state;
            bool over = r2d_rect_contains(el, r2d_mouse()), down = over && r2d_mouse_down(R2D_MOUSE_LEFT);
            if (over != st->hovered || down != st->pressed) st->since = 0; else st->since += r2d_dt();
            st->hovered = over; st->pressed = down;
            float k = r2d_dt() * 14 > 1 ? 1 : r2d_dt() * 14;
            st->hover_t += ((over ? 1.0f : 0.0f) - st->hover_t) * k;
            st->press_t += ((down ? 1.0f : 0.0f) - st->press_t) * k;
            float fill = 0.5f + 0.5f * sinf(ui.time * 0.7f);
            r2d_sprite_dynamic dyn = {{st->hover_t, st->press_t, fill, st->since}, {0, 0, 0, 0}};
            /* tinted like a typical element (UiImage colour), so glows and gradients show */
            r2d_color tint = t && !t->missing ? r2d_rgb(255, 255, 255) : r2d_rgb(70, 110, 170);
            r2d_draw_sprite_screen(m, t && !t->missing ? &t->texture : NULL, sh ? &sh->shader : NULL, r2d_rect_center(el), r2d_v2(el.w, el.h), 0, tint, NULL, &dyn);
            r2d_rect ring = r2d_rect_make(pv.x + preview * 0.5f - 24, pv.y + 8, 48, 48);   /* a square one above, for rings */
            r2d_draw_sprite_screen(m, t && !t->missing ? &t->texture : NULL, sh ? &sh->shader : NULL, r2d_rect_center(ring), r2d_v2(48, 48), 0, tint, NULL, &dyn);
            char fill_text[32];
            snprintf(fill_text, sizeof(fill_text), "fill %.2f", (double)fill);
            r2d_ui_label_dim(r2d_v2(pv.x + 6, pv.y + preview - 20), fill_text);
        }
        r2d_draw_rect_outline(pv, 1, r2d_rgb(90, 96, 110));
        char info[200];
        if (t && t->missing) snprintf(info, sizeof(info), "texture: %s", t->error);
        else if (t) snprintf(info, sizeof(info), "%dx%d", t->texture.width, t->texture.height);
        else snprintf(info, sizeof(info), "no texture (white)");
        r2d_ui_label_dim(r2d_v2(pv.x, pv.y + pv.h + 4), info);
        r2d_rect mode = r2d_rect_make(pv.x, pv.y + pv.h + 20, pv.w, 20);
        if (r2d_ui_button_colored(split(mode, 0, 2), "sprite", ui.material_preview == 0 ? accent : plain)) ui.material_preview = 0;
        if (r2d_ui_button_colored(split(mode, 1, 2), "element", ui.material_preview == 1 ? accent : plain)) ui.material_preview = 1;
    }
    l.w -= preview + 16;
    char text[200];
    snprintf(text, sizeof(text), "%s%s", m->name, material_dirty() ? "*" : "");
    label_line(&l, yellow, text);
    /* texture */
    r2d_rect r = row(&l, ROW_H);
    row_label(r, "texture");
    if (button(r2d_rect_make(r.x + 110, r.y, r.w - 110, r.h), m->texture[0] ? m->texture : "none (white)", ui.material_picker == 1)) ui.material_picker = ui.material_picker == 1 ? 0 : 1;
    if (ui.material_picker == 1) {
        r2d_rect nr2 = row(&l, ROW_H - 2);
        if (button(r2d_rect_make(nr2.x + 110, nr2.y, nr2.w - 110, nr2.h), "none (white)", false)) { m->texture[0] = 0; ui.material_picker = 0; }
        for (size_t i = 0; i < d->assets.count; ++i) {
            const cw_asset *a = &d->assets.items[i];
            if (a->type != CW_ASSET_IMAGE) continue;
            r2d_rect ar = row(&l, ROW_H - 2);
            if (button(r2d_rect_make(ar.x + 110, ar.y, ar.w - 110, ar.h), a->name, false)) { snprintf(m->texture, sizeof(m->texture), "%s", a->name); ui.material_picker = 0; }
        }
        label_line(&l, r2d_rgb(150, 155, 165), "(import images in the assets panel)");
    }
    /* effect */
    r = row(&l, ROW_H);
    row_label(r, "effect");
    int column = 0;
    r2d_rect line = r2d_rect_make(r.x + 110, r.y, r.w - 110, r.h);
    for (int e = 0; e < R2D_EFFECT_COUNT; ++e) {
        if (e && column == 0) line = row(&l, ROW_H - 2), line = r2d_rect_make(line.x + 110, line.y, line.w - 110, line.h);
        if (button(split(line, column, 3), r2d_effect_name((r2d_effect)e), (int)m->effect == e)) r2d_material_set_effect(m, (r2d_effect)e);
        column = (column + 1) % 3;
    }
    label_line(&l, r2d_rgb(150, 155, 165), r2d_effect_help(m->effect));
    /* custom shader */
    if (m->effect == R2D_EFFECT_CUSTOM) {
        r = row(&l, ROW_H);
        row_label(r, "shader");
        if (button(r2d_rect_make(r.x + 110, r.y, r.w - 110, r.h), m->shader[0] ? m->shader : "pick a shader asset", ui.material_picker == 2)) ui.material_picker = ui.material_picker == 2 ? 0 : 2;
        if (ui.material_picker == 2) {
            for (size_t i = 0; i < d->assets.count; ++i) {
                const cw_asset *a = &d->assets.items[i];
                if (a->type != CW_ASSET_SHADER) continue;
                r2d_rect ar = row(&l, ROW_H - 2);
                if (button(r2d_rect_make(ar.x + 110, ar.y, ar.w - 110, ar.h), a->name, false)) { snprintf(m->shader, sizeof(m->shader), "%s", a->name); ui.material_picker = 0; host_canvas_reload(&ui.canvas); }
            }
        }
        r = row(&l, ROW_H);
        row_label(r, "starter");
        {
            r2d_rect ex = r2d_rect_make(r.x + 110, r.y, r.w - 110, r.h);
            int n = (int)r2d_shader_example_count();
            for (int i = 0; i < n; ++i)
                if (button(split(ex, i, n), r2d_shader_example_name((size_t)i), ui.shader_example == i)) ui.shader_example = i;
        }
        r = row(&l, ROW_H);
        if (textfield(r2d_rect_make(r.x + 110, r.y, r.w - 110 - 140, r.h), &ui.shader_name, ui.shader_name, sizeof(ui.shader_name)) || button(r2d_rect_make(r.x + r.w - 136, r.y, 66, r.h), "new", false)) {
            if (ui.shader_name[0]) { shader_new(ui.shader_name, ui.shader_example); ui.shader_name[0] = 0; }
        }
        if (button(r2d_rect_make(r.x + r.w - 66, r.y, 66, r.h), "reload", false)) host_canvas_reload(&ui.canvas);
        if (m->shader[0]) {
            const host_shader *sh = host_canvas_shader(&ui.canvas, cw_asset_id(m->shader));
            const char *log = sh ? sh->shader.log : "";
            r2d_color col = sh && sh->shader.valid ? green : red;
            /* the first lines of the compiler output */
            const char *p = log;
            for (int i = 0; i < 3 && *p; ++i) {
                const char *nl = strchr(p, '\n');
                size_t len = nl ? (size_t)(nl - p) : strlen(p);
                snprintf(text, sizeof(text), "%.*s", (int)(len < 120 ? len : 120), p);
                label_line(&l, col, text);
                if (!nl) break;
                p = nl + 1;
            }
            /* watch the file: an external editor saving it recompiles */
            char path[CW_ASSET_PATH_CAPACITY];
            if (cw_assets_path(&d->assets, cw_asset_id(m->shader), path, sizeof(path)) && r2d_tunables_file_changed(path, &ui.shader_stamp) && ui.shader_watch > 0) {
                host_canvas_reload(&ui.canvas);
                ed_status(d, false, "shader %s changed on disk: recompiled", m->shader);
            }
            ui.shader_watch = 1;
        }
        label_line(&l, r2d_rgb(150, 155, 165), "vec4 material(vec4 base, vec2 uv, vec4 p0, vec4 p1, vec4 p2, vec4 p3, float time)");
    }
    /* blend, filter, wrap */
    r = row(&l, ROW_H);
    row_label(r, "blend");
    line = r2d_rect_make(r.x + 110, r.y, r.w - 110, r.h);
    for (int b = 0; b < R2D_BLEND_COUNT; ++b) if (button(split(line, b, 4), r2d_blend_name((r2d_blend)b), (int)m->blend == b)) m->blend = (r2d_blend)b;
    r = row(&l, ROW_H);
    row_label(r, "sampling");
    line = r2d_rect_make(r.x + 110, r.y, r.w - 110, r.h);
    if (button(split(line, 0, 4), "linear", !m->nearest)) m->nearest = false;
    if (button(split(line, 1, 4), "nearest", m->nearest)) m->nearest = true;
    if (button(split(line, 2, 4), "clamp", !m->repeat)) m->repeat = false;
    if (button(split(line, 3, 4), "repeat", m->repeat)) m->repeat = true;
    /* tint, tiling, offset */
    static char buffers[24][24];
    static const char *const tint_labels[4] = {"tint r", "tint g", "tint b", "tint a"};
    for (int i = 0; i < 4; ++i) draw_number(&l, tint_labels[i], &m->tint[i], 0, 4, 0.05f, &buffers[i], buffers[i], sizeof(buffers[i]));
    draw_number(&l, "tiling x", &m->tiling[0], 0.01f, 64, 0.5f, &buffers[4], buffers[4], sizeof(buffers[4]));
    draw_number(&l, "tiling y", &m->tiling[1], 0.01f, 64, 0.5f, &buffers[5], buffers[5], sizeof(buffers[5]));
    draw_number(&l, "offset x", &m->offset[0], -64, 64, 0.05f, &buffers[6], buffers[6], sizeof(buffers[6]));
    draw_number(&l, "offset y", &m->offset[1], -64, 64, 0.05f, &buffers[7], buffers[7], sizeof(buffers[7]));
    /* the effect's parameters */
    size_t count;
    const r2d_material_param *params = r2d_material_params(m->effect, &count);
    if (count) heading(&l, "PARAMETERS");
    for (size_t i = 0; i < count && i < 16; ++i)
        draw_number(&l, params[i].label, &m->params[params[i].index][params[i].component], params[i].min, params[i].max, params[i].step, &buffers[8 + i], buffers[8 + i], sizeof(buffers[8 + i]));
    hui_unclip();
    /* never scroll past the end */
    int max_scroll = (int)(l.y + (float)ui.material_scroll - bottom + 4);
    if (ui.material_scroll > max_scroll) ui.material_scroll = max_scroll > 0 ? max_scroll : 0;
    if (max_scroll > 0) {
        char hint[48];
        snprintf(hint, sizeof(hint), "%d%% (wheel scrolls)", (int)(100.0f * (float)ui.material_scroll / (float)max_scroll));
        r2d_ui_label_dim(r2d_v2(content.x + content.w - preview - 12, content.y + content.h - 22), hint);
    }
}

/* the default arrangement: viewport in the middle, scene/assets/world left,
 * inspector and settings right, the log below the viewport (closed) */
static void default_layout(void) {
    hd_dock *k = &ui.dock;
    hd_dock_panel(k, "viewport", HD_CENTER, 0, NULL);
    hd_dock_panel(k, "scene", HD_LEFT, 0.19f, NULL);
    hd_dock_panel(k, "assets", HD_CENTER, 0, "scene");
    hd_dock_panel(k, "world", HD_CENTER, 0, "scene");
    hd_dock_panel(k, "inspector", HD_RIGHT, 0.26f, NULL);
    hd_dock_panel(k, "project", HD_CENTER, 0, "inspector");
    hd_dock_panel(k, "rendering", HD_CENTER, 0, "inspector");
    hd_dock_panel(k, "preferences", HD_CENTER, 0, "inspector");
    hd_dock_panel(k, "animation", HD_BOTTOM, 0.32f, "viewport");
    hd_dock_panel(k, "material", HD_CENTER, 0, "animation");
    hd_dock_panel(k, "log", HD_CENTER, 0, "animation");
    hd_focus(k, "animation");
    hd_focus(k, "scene");
    hd_focus(k, "inspector");
}
static void setup_dock(void) {
    hd_dock *k = &ui.dock;
    hd_init(k);
    hd_add_panel(k, "viewport", "viewport", panel_viewport, NULL, true);
    hd_add_panel(k, "scene", "scene", panel_scene, NULL, false);
    hd_add_panel(k, "assets", "assets", panel_assets, NULL, false);
    hd_add_panel(k, "world", "world", panel_world, NULL, false);
    hd_add_panel(k, "inspector", "inspector", panel_inspector, NULL, false);
    hd_add_panel(k, "project", "project", panel_project, NULL, false);
    hd_add_panel(k, "rendering", "rendering", panel_rendering, NULL, false);
    hd_add_panel(k, "preferences", "preferences", panel_preferences, NULL, false);
    hd_add_panel(k, "log", "build log", panel_log, NULL, false);
    hd_add_panel(k, "animation", "animation", panel_animation, NULL, false);
    hd_add_panel(k, "material", "material", panel_material, NULL, false);
    if (!hd_load(k, ui.layout_path)) default_layout();
}
/* the panels menu: reopen closed panels, reset the layout */
static void draw_panels_menu(float x, float y) {
    if (!ui.panels_menu) return;
    hd_dock *k = &ui.dock;
    float h = ROW_H + 2, w = 150;
    r2d_rect menu = r2d_rect_make(x, y, w, h * (float)(k->panel_count + 1) + 8);
    r2d_ui_panel(menu);
    float yy = menu.y + 4;
    for (int i = 0; i < k->panel_count; ++i) {
        const hd_panel *p = &k->panels[i];
        char label[48];
        snprintf(label, sizeof(label), "%s %s", p->open ? "[x]" : "[ ]", p->title);
        if (button(r2d_rect_make(menu.x + 4, yy, w - 8, h - 4), label, false)) { if (p->open) hd_close(k, p->id); else hd_show(k, p->id); }
        yy += h;
    }
    if (button(r2d_rect_make(menu.x + 4, yy, w - 8, h - 4), "reset layout", false)) { hd_clear(k); default_layout(); ui.panels_menu = false; }
    if (r2d_mouse_pressed(R2D_MOUSE_LEFT) && !r2d_rect_contains(menu, r2d_mouse()) && !r2d_rect_contains(r2d_rect_make(x, y - 30, 80, 30), r2d_mouse())) ui.panels_menu = false;
}

/* ---------------------------------------------------------------------------
 * keys
 * ------------------------------------------------------------------------- */
static void handle_keys(void) {
    ed_document *d = &ui.doc;
    bool ctrl = (r2d_modifiers() & (SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SUPER)) != 0;
    bool shift = (r2d_modifiers() & SAPP_MODIFIER_SHIFT) != 0;
    if (hui_focused()) return;   /* a text field owns the keyboard */
    if (r2d_key_pressed(SAPP_KEYCODE_Q)) { if (ctrl) sapp_request_quit(); else ui.tool = TOOL_SELECT; }
    if (r2d_key_pressed(SAPP_KEYCODE_F11)) sapp_toggle_fullscreen();
    if (r2d_key_pressed(SAPP_KEYCODE_GRAVE_ACCENT)) { if (hd_is_open(&ui.dock, "log")) hd_close(&ui.dock, "log"); else hd_show(&ui.dock, "log"); }
    if (r2d_key_pressed(SAPP_KEYCODE_G)) ui.grid = !ui.grid;
    if (r2d_key_pressed(SAPP_KEYCODE_F10) && d->project_open) { hd_show(&ui.dock, "rendering"); ui.lights = true; }
    if (r2d_key_pressed(SAPP_KEYCODE_L)) ui.lights = !ui.lights;
    if (r2d_key_pressed(SAPP_KEYCODE_F)) fit_view();
    if (r2d_key_pressed(SAPP_KEYCODE_F7)) { bool had = d->world_ready; ed_build(d, "module"); selection_to_r2d(); if (d->last_build_status != 0) hd_show(&ui.dock, "log"); if (d->world_ready && !had) fit_view(); }
    if (r2d_key_pressed(SAPP_KEYCODE_F8)) { ed_build(d, "export"); hd_show(&ui.dock, "log"); }
    if (!d->world_ready) return;
    if (r2d_key_pressed(SAPP_KEYCODE_F9)) ed_run_player(d);
    if (r2d_key_pressed(SAPP_KEYCODE_F5)) { ea_stop(&ui.anim, d); if (d->playing) ed_stop(d); else ed_play(d); selection_to_r2d(); }
    if (r2d_key_pressed(SAPP_KEYCODE_SPACE)) { if (d->playing) ed_toggle_pause(d); else ed_play(d); }
    if (r2d_key_pressed(SAPP_KEYCODE_N)) ed_step(d, 1);
    if (r2d_key_pressed(SAPP_KEYCODE_ESCAPE)) {
        if (d->playing) { ed_stop(d); selection_to_r2d(); }
        else if (ui.tool != TOOL_SELECT) ui.tool = TOOL_SELECT;
        else { ed_select_clear(d); r2d_select_clear(); }
    }
    if (ctrl && r2d_key_pressed(SAPP_KEYCODE_S)) { if (shift) save_as(); else save(); }
    if (ctrl && r2d_key_pressed(SAPP_KEYCODE_N)) { ed_new_scene(d); selection_to_r2d(); }
    if (ctrl && r2d_key_pressed(SAPP_KEYCODE_Z)) { if (shift) ed_redo(d); else ed_undo(d); selection_to_r2d(); }
    if (ctrl && r2d_key_pressed(SAPP_KEYCODE_Y)) { ed_redo(d); selection_to_r2d(); }
    if (ctrl && r2d_key_pressed(SAPP_KEYCODE_A)) { ed_select_all(d); selection_to_r2d(); }
    if (ctrl && r2d_key_pressed(SAPP_KEYCODE_D)) { ed_duplicate_selection(d, 30, 30); selection_to_r2d(); }
    if (r2d_key_pressed(SAPP_KEYCODE_DELETE) || r2d_key_pressed(SAPP_KEYCODE_BACKSPACE) || (!ctrl && r2d_key_pressed(SAPP_KEYCODE_X))) {
        ed_delete_selection(d);
        selection_to_r2d();
    }
    for (int k = 0; k < 9; ++k)
        if (r2d_key_pressed((sapp_keycode)(SAPP_KEYCODE_1 + k)) && (size_t)k < d->gw.module->prefab_count) { ui.tool = TOOL_PLACE; ui.prefab = (size_t)k; }
    if (r2d_key_pressed(SAPP_KEYCODE_E) && !ctrl) ui.tool = TOOL_CREATE;
    float nudge = (ui.prefs.nudge > 0 ? ui.prefs.nudge : 1) * (shift ? 10 : 1), dx = 0, dy = 0;
    if (r2d_key_typed(SAPP_KEYCODE_LEFT)) dx -= nudge;
    if (r2d_key_typed(SAPP_KEYCODE_RIGHT)) dx += nudge;
    if (r2d_key_typed(SAPP_KEYCODE_UP)) dy -= nudge;
    if (r2d_key_typed(SAPP_KEYCODE_DOWN)) dy += nudge;
    if ((dx != 0 || dy != 0) && d->selection_count) {
        ed_move_selection(d, dx, dy);
        ed_ui_move_selection(d, dx, dy);   /* interface elements nudge in reference pixels */
        if (r2d_key_released(SAPP_KEYCODE_LEFT) || r2d_key_released(SAPP_KEYCODE_RIGHT) || r2d_key_released(SAPP_KEYCODE_UP) || r2d_key_released(SAPP_KEYCODE_DOWN)) ed_commit(d, "nudge");
    } else if (r2d_key_released(SAPP_KEYCODE_LEFT) || r2d_key_released(SAPP_KEYCODE_RIGHT) || r2d_key_released(SAPP_KEYCODE_UP) || r2d_key_released(SAPP_KEYCODE_DOWN)) {
        if (d->selection_count) ed_commit(d, "nudge");
    }
}

/* ---------------------------------------------------------------------------
 * window
 * ------------------------------------------------------------------------- */
static void window_init(void) {
    ui.ui_hover = CW_INVALID_ENTITY;
    ui.ui_drag.id = CW_INVALID_ENTITY;
    r2d_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.camera = r2d_camera_make(r2d_v2(0, 0), 1.0f, false);
    desc.camera.min_zoom = 0.05f;
    desc.camera.max_zoom = 16.0f;
    desc.camera_controls = true;
    desc.pan_with_right_button = desc.pan_with_middle_button = true;
    desc.pan_with_arrows = false;   /* arrows nudge the selection */
    desc.escape_clears_selection = false;
    desc.draw_selection_box = true;
    desc.text_scale = ui.prefs.text_scale;
    desc.wheel_zoom_step = ui.prefs.zoom_step;
    desc.post_config = ui.config;
    r2d_setup(&desc);
    if (!r2d_valid()) { ui.failed = true; sapp_request_quit(); return; }
    host_canvas_init(&ui.canvas);
    ea_init(&ui.anim);
    ui.anim_drag_track = ui.anim_drag_key = -1;
    setup_dock();
    apply_prefs();
    ui.lights = ui.prefs.preview_rendering;   /* L previews the game's lighting; off keeps the level readable */
    ui.asset_filter = -1;
    ui.ready = true;
    if (ui.project_arg[0]) open_project(ui.project_arg);
    fit_view();
}
static void window_frame(void) {
    if (!ui.ready) return;
    r2d_begin_frame();
    if (r2d_frame_count() == 1) fit_view();
    float dt = r2d_dt();
    ui.time += dt;
    hui_frame(dt);
    ui.fps = ui.fps * 0.95f + (dt > 0 ? 1.0f / dt : 0) * 0.05f;
    handle_keys();
    if (ui.render_loaded && r2d_post_reload_if_changed()) {   /* edited outside the editor */
        ui.render_saved = *r2d_post_params_ptr();
        ed_status(&ui.doc, false, "rendering settings reloaded from disk");
    }
    ui.doc.no_history = ui.anim.open && ui.anim.record;
    ea_frame(&ui.anim, &ui.doc, dt);
    ed_frame(&ui.doc, dt);
    if (ui.prefs.autosave_seconds > 0 && ui.doc.dirty && ui.doc.scene_path[0] && !ui.doc.playing) {
        ui.autosave_timer += dt;
        if (ui.autosave_timer >= (float)ui.prefs.autosave_seconds) {
            ui.autosave_timer = 0;
            if (ed_autosave(&ui.doc)) ed_status(&ui.doc, false, "autosaved a copy next to the scene");
        }
    } else ui.autosave_timer = 0;
    r2d_selection_prune(keep_alive, NULL);
    ui.hovered = r2d_hovered();
    draw_world();
    handle_world_input();
    draw_ui_gizmos();
    draw_toolbar();
    hd_frame(&ui.dock, dock_area());
    if (ui.dock.changed) { hd_save(&ui.dock, ui.layout_path); ui.dock.changed = false; }
    draw_panels_menu(ui.panels_menu_x, TOOLBAR_H);
    draw_status_bar();
    r2d_end_frame();
    ++ui.frames;
    if (ui.max_frames && ui.frames >= ui.max_frames) sapp_request_quit();
}
static void window_event(const sapp_event *event) {
    if (!ui.ready) return;
    if (event->type == SAPP_EVENTTYPE_FILES_DROPPED) {
        int n = sapp_get_num_dropped_files(), imported = 0;
        for (int i = 0; i < n; ++i) {
            const char *path = sapp_get_dropped_file_path(i);
            struct stat st;
            if (stat(path, &st) == 0 && (st.st_mode & S_IFDIR)) imported += (int)ed_import_directory(&ui.doc, path, false);
            else if (ed_import_asset(&ui.doc, path, false)) ++imported;
        }
        if (n) { hd_show(&ui.dock, "assets"); ed_status(&ui.doc, imported == 0, "dropped %d item%s: %d imported", n, n == 1 ? "" : "s", imported); }
        return;
    }
    r2d_handle_event(event);
}
static void window_cleanup(void) {
    if (!ui.ready) return;
    host_canvas_destroy(&ui.canvas);
    r2d_shutdown();
    ui.ready = false;
}

int main(int argc, char **argv) {
    const char *engine_root = getenv("CW_ENGINE_ROOT");
#ifdef CW_ENGINE_ROOT
    if (!engine_root || !*engine_root) engine_root = CW_ENGINE_ROOT;
#endif
    snprintf(ui.config, sizeof(ui.config), "tunable.cfg");
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--project") == 0 && i + 1 < argc) snprintf(ui.project_arg, sizeof(ui.project_arg), "%s", argv[++i]);
        else if (strcmp(a, "--scene") == 0 && i + 1 < argc) snprintf(ui.scene_arg, sizeof(ui.scene_arg), "%s", argv[++i]);
        else if (strcmp(a, "--engine") == 0 && i + 1 < argc) engine_root = argv[++i];
        else if (strcmp(a, "--config") == 0 && i + 1 < argc) snprintf(ui.config, sizeof(ui.config), "%s", argv[++i]);
        else if (strcmp(a, "--frames") == 0 && i + 1 < argc) {
            char *end;
            errno = 0;
            unsigned long n = strtoul(argv[++i], &end, 10);
            if (errno == ERANGE || *end || !n || n > UINT32_MAX) { fputs("--frames needs a positive integer\n", stderr); return 2; }
            ui.max_frames = (unsigned)n;
        } else if (a[0] != '-' && !ui.project_arg[0]) snprintf(ui.project_arg, sizeof(ui.project_arg), "%s", a);
        else {
            printf("usage: %s [--project game.cwproj] [--scene file.cwscene] [--engine ROOT] [--config tunable.cfg] [--frames N]\n", argv[0]);
            return strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0 ? 0 : 2;
        }
    }
    /* the player is built next to the editor */
    char player[CW_GAME_PATH_CAPACITY];
    snprintf(player, sizeof(player), "%s", argv[0]);
    char *slash = strrchr(player, '/');
#ifdef _WIN32
    char *bslash = strrchr(player, '\\');
    if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
    if (slash) slash[1] = 0; else player[0] = 0;
    strncat(player, "clockwork_player", sizeof(player) - strlen(player) - 1);
#ifdef _WIN32
    strncat(player, ".exe", sizeof(player) - strlen(player) - 1);
#endif
    ed_init(&ui.doc, engine_root, player);
    ed_settings_defaults(&ui.prefs);
    {
        char error[256] = "";
        ed_settings_load(&ui.prefs, NULL, error, sizeof(error));
        if (error[0]) fprintf(stderr, "editor preferences: %s\n", error);
        ui.prefs_saved = ui.prefs;
        /* the dock layout lives next to the preferences */
        ed_settings_path(ui.layout_path, sizeof(ui.layout_path));
        char *slash = strrchr(ui.layout_path, '/');
#ifdef _WIN32
        char *bslash = strrchr(ui.layout_path, '\\');
        if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
        if (slash) snprintf(slash + 1, sizeof(ui.layout_path) - (size_t)(slash + 1 - ui.layout_path), "editor_layout.cfg");
        else snprintf(ui.layout_path, sizeof(ui.layout_path), "editor_layout.cfg");
        const char *env = getenv("CW_EDITOR_LAYOUT");
        if (env && *env) snprintf(ui.layout_path, sizeof(ui.layout_path), "%s", env);
    }
    snprintf(ui.open_path, sizeof(ui.open_path), "%s", ui.project_arg[0] ? ui.project_arg : "games/combat/combat.cwproj");
    sapp_desc window;
    memset(&window, 0, sizeof(window));
    window.init_cb = window_init;
    window.frame_cb = window_frame;
    window.cleanup_cb = window_cleanup;
    window.event_cb = window_event;
    window.width = 1480;
    window.height = 900;
    window.sample_count = 4;
    window.high_dpi = true;
    window.window_title = "Clockwork editor";
    window.enable_dragndrop = true;
    window.max_dropped_files = 64;
    window.logger.func = slog_func;
    sapp_run(&window);
    int result = ui.failed ? 1 : 0;
    if (ui.max_frames && !ui.failed) printf("OK: editor rendered %u frames\n", ui.frames);
    ed_shutdown(&ui.doc);
    return result;
}
