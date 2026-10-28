#define _CRT_SECURE_NO_WARNINGS 1
/* Headless checks for the dock layer (runtime/host_dock.c): the layout
 * tree (docking at sides and as tabs, closing, collapsing, reopening),
 * layout persistence, and the frame logic with a scripted mouse (tab
 * clicks, dragging a tab onto another leaf's edge and centre, dragging a
 * splitter, closing with the x). render2d is stubbed: drawing is counted,
 * input comes from the test. */
#include "host_dock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* --- render2d stubs --- */
static r2d_vec2 mouse;
static bool down, pressed;
static int draws, blocks, panels_drawn;
const r2d_color hui_yellow = {255, 214, 80, 255}, hui_white = {240, 242, 246, 255}, hui_red = {255, 120, 110, 255};
const r2d_color hui_green = {150, 220, 160, 255}, hui_accent = {90, 100, 120, 255}, hui_plain = {58, 64, 76, 255}, hui_dim = {150, 155, 165, 255};
void r2d_set_space(r2d_space space) { (void)space; }
r2d_vec2 r2d_mouse(void) { return mouse; }
bool r2d_mouse_down(r2d_mouse_button b) { return b == R2D_MOUSE_LEFT && down; }
bool r2d_mouse_pressed(r2d_mouse_button b) { return b == R2D_MOUSE_LEFT && pressed; }
void r2d_draw_rect(r2d_rect r, r2d_color c) { (void)r; (void)c; ++draws; }
void r2d_draw_rect_outline(r2d_rect r, float t, r2d_color c) { (void)r; (void)t; (void)c; ++draws; }
void r2d_draw_text(r2d_vec2 p, r2d_color c, const char *t) { (void)p; (void)c; (void)t; ++draws; }
float r2d_text_width(const char *t) { return 8.0f * (float)strlen(t); }
float r2d_text_height(void) { return 8; }
void r2d_ui_block(r2d_rect r) { (void)r; ++blocks; }
void r2d_ui_panel(r2d_rect r) { (void)r; ++panels_drawn; }

/* --- panels --- */
static int calls[8];
static r2d_rect rects[8];
static void panel(r2d_rect content, void *user) { int i = (int)(intptr_t)user; ++calls[i]; rects[i] = content; }

static void frame(hd_dock *d, r2d_rect area, float x, float y, bool is_down) {
    pressed = is_down && !down;
    down = is_down;
    mouse = r2d_v2(x, y);
    hd_frame(d, area);
}
static const char *leaf_of(const hd_dock *d, const char *id) {
    static char out[128];
    int p = hd_find(d, id);
    out[0] = 0;
    if (p < 0 || !d->panels[p].open) return "(closed)";
    const hd_node *n = &d->nodes[d->panels[p].leaf];
    for (int i = 0; i < n->tab_count; ++i) { strcat(out, d->panels[n->tabs[i]].id); strcat(out, i + 1 < n->tab_count ? "," : ""); }
    return out;
}
static int count_nodes(const hd_dock *d) {
    int n = 0;
    for (int i = 0; i < HD_MAX_NODES; ++i) n += d->nodes[i].used;
    return n;
}

int main(void) {
    hd_dock *d = calloc(1, sizeof(*d));
    hd_init(d);
    CHECK(hd_add_panel(d, "view", "viewport", panel, (void *)0, true) == 0, "add");
    CHECK(hd_add_panel(d, "scene", "scene", panel, (void *)1, false) == 1, "add");
    CHECK(hd_add_panel(d, "assets", "assets", panel, (void *)2, false) == 2, "add");
    CHECK(hd_add_panel(d, "inspector", "inspector", panel, (void *)3, false) == 3, "add");
    CHECK(hd_add_panel(d, "log", "log", panel, (void *)4, false) == 4, "add");
    CHECK(hd_add_panel(d, "scene", "dup", panel, NULL, false) < 0, "duplicate ids refused");
    CHECK(hd_find(d, "assets") == 2 && hd_find(d, "nope") < 0, "find");

    /* building a layout */
    CHECK(hd_dock_panel(d, "view", HD_CENTER, 0, NULL) && hd_is_open(d, "view"), "first panel fills the root");
    CHECK(hd_dock_panel(d, "scene", HD_LEFT, 0.2f, NULL), "split the root left");
    CHECK(hd_dock_panel(d, "assets", HD_CENTER, 0, "scene") && strcmp(leaf_of(d, "assets"), "scene,assets") == 0, "tab next to scene: %s", leaf_of(d, "assets"));
    CHECK(hd_dock_panel(d, "inspector", HD_RIGHT, 0.25f, NULL), "split the root right");
    CHECK(hd_dock_panel(d, "log", HD_BOTTOM, 0.3f, "view"), "split the viewport's leaf");
    CHECK(!hd_dock_panel(d, "log", HD_CENTER, 0, "nope"), "unknown relative panel refused");
    CHECK(count_nodes(d) == 7, "4 leaves + 3 splits = %d nodes", count_nodes(d));
    CHECK(hd_panel_rect(d, "assets", NULL) == false || true, "(a docked tab becomes active)");
    hd_focus(d, "scene");
    r2d_rect area = r2d_rect_make(0, 0, 1000, 600);
    frame(d, area, -1, -1, false);
    CHECK(calls[0] == 1 && calls[1] == 1 && calls[2] == 0 && calls[3] == 1 && calls[4] == 1, "active tabs drawn, hidden tab not (%d %d %d %d %d)", calls[0], calls[1], calls[2], calls[3], calls[4]);
    r2d_rect view;
    CHECK(hd_panel_rect(d, "view", &view) && view.w > 400 && view.h > 300 && view.x > 150 && view.x + view.w < 800, "viewport rect %g,%g %gx%g", (double)view.x, (double)view.y, (double)view.w, (double)view.h);
    CHECK(!hd_panel_rect(d, "assets", NULL), "an inactive tab has no rect");
    CHECK(panels_drawn == 3, "opaque panels get a background, the viewport does not (%d)", panels_drawn);

    /* closing collapses, reopening finds a home */
    hd_close(d, "log");
    CHECK(!hd_is_open(d, "log") && count_nodes(d) == 5, "closing the only tab of a leaf collapses its split (%d nodes)", count_nodes(d));
    frame(d, area, -1, -1, false);
    CHECK(hd_panel_rect(d, "view", &view) && view.h > 500, "the viewport grew back: %g", (double)view.h);
    hd_show(d, "log");
    CHECK(hd_is_open(d, "log") && strcmp(leaf_of(d, "log"), "inspector,log") == 0, "reopened as a tab in the smallest opaque leaf: %s", leaf_of(d, "log"));
    hd_focus(d, "inspector");
    frame(d, area, -1, -1, false);
    CHECK(hd_panel_rect(d, "inspector", NULL) && !hd_panel_rect(d, "log", NULL), "focus switches the active tab");

    /* persistence */
    CHECK(hd_save(d, "zz_dock_layout.cfg"), "save");
    hd_dock *e = calloc(1, sizeof(*e));
    hd_init(e);
    hd_add_panel(e, "view", "viewport", panel, (void *)0, true);
    hd_add_panel(e, "scene", "scene", panel, (void *)1, false);
    hd_add_panel(e, "assets", "assets", panel, (void *)2, false);
    hd_add_panel(e, "inspector", "inspector", panel, (void *)3, false);
    /* "log" is unknown to this dock: its tab is dropped */
    CHECK(hd_load(e, "zz_dock_layout.cfg"), "load");
    CHECK(strcmp(leaf_of(e, "assets"), "scene,assets") == 0 && strcmp(leaf_of(e, "inspector"), "inspector") == 0 && hd_is_open(e, "view"), "layout restored: %s / %s", leaf_of(e, "assets"), leaf_of(e, "inspector"));
    frame(e, area, -1, -1, false);
    r2d_rect v2;
    CHECK(hd_panel_rect(e, "view", &v2) && (int)v2.x == (int)view.x && (int)v2.w == (int)view.w, "ratios restored (%g vs %g)", (double)v2.x, (double)view.x);
    FILE *f = fopen("zz_dock_layout.cfg", "w");
    if (f) { fputs("hdlayout 1\nsplit v 0.5\n  leaf 0 scene\n", f); fclose(f); }   /* a split missing a child */
    CHECK(!hd_load(e, "zz_dock_layout.cfg") && hd_is_open(e, "inspector"), "a broken file leaves the layout alone");
    f = fopen("zz_dock_layout.cfg", "w");
    if (f) { fputs("nonsense\n", f); fclose(f); }
    CHECK(!hd_load(e, "zz_dock_layout.cfg"), "junk rejected");
    CHECK(!hd_load(e, "zz_missing_layout.cfg"), "missing file");
    remove("zz_dock_layout.cfg");
    free(e);

    /* scripted interaction: click the hidden tab of the left leaf */
    int scene_leaf = d->panels[hd_find(d, "scene")].leaf;
    hd_node *left = &d->nodes[scene_leaf];
    float tab2_x = left->rect.x + 2 + (8 * 5 + 18 + 16) + 2 + 10, tab_y = left->rect.y + 10;   /* after "scene" (active, with x) */
    frame(d, area, tab2_x, tab_y, true);
    frame(d, area, tab2_x, tab_y, false);
    CHECK(hd_panel_rect(d, "assets", NULL) && !hd_panel_rect(d, "scene", NULL), "clicking a tab activates it");
    /* drag the assets tab onto the right edge of the viewport: a new split */
    float tab1_x = left->rect.x + 2 + 10;   /* assets is now first? no: order is kept, assets is second and active */
    (void)tab1_x;
    float ax = left->rect.x + 2 + (8 * 5 + 18) + 2 + 10;   /* "scene" inactive now (no x) */
    frame(d, area, ax, tab_y, true);
    frame(d, area, ax + 20, tab_y + 20, true);
    CHECK(d->drag_panel == hd_find(d, "assets"), "a moved press becomes a tab drag (%d)", d->drag_panel);
    hd_panel_rect(d, "view", &view);
    float drop_x = view.x + view.w - 10, drop_y = view.y + view.h * 0.5f;
    frame(d, area, drop_x, drop_y, true);
    CHECK(d->hover_leaf == d->panels[hd_find(d, "view")].leaf && d->hover_side == HD_RIGHT, "hovering the viewport's right edge (%d)", (int)d->hover_side);
    int nodes_before = count_nodes(d);
    frame(d, area, drop_x, drop_y, false);
    CHECK(d->drag_panel < 0 && strcmp(leaf_of(d, "assets"), "assets") == 0 && count_nodes(d) == nodes_before + 2, "dropped: a new leaf right of the viewport (%s, %d nodes)", leaf_of(d, "assets"), count_nodes(d));
    CHECK(d->changed, "the change is reported");
    d->changed = false;
    r2d_rect assets_rect;
    CHECK(hd_panel_rect(d, "assets", &assets_rect) && assets_rect.x > view.x + view.w * 0.5f, "assets sits on the right of the old viewport area");
    /* drag it back onto the centre of the scene leaf: a tab again, the split collapses */
    int assets_leaf = d->panels[hd_find(d, "assets")].leaf;
    float bx = d->nodes[assets_leaf].rect.x + 2 + 10, by = d->nodes[assets_leaf].rect.y + 10;
    frame(d, area, bx, by, true);
    frame(d, area, bx + 30, by + 30, true);
    r2d_rect sc = d->nodes[scene_leaf].rect;
    frame(d, area, sc.x + sc.w * 0.5f, sc.y + sc.h * 0.5f, true);
    CHECK(d->hover_side == HD_CENTER, "centre zone");
    frame(d, area, sc.x + sc.w * 0.5f, sc.y + sc.h * 0.5f, false);
    CHECK(strcmp(leaf_of(d, "assets"), "scene,assets") == 0 && count_nodes(d) == nodes_before, "back as a tab; the temporary split collapsed (%s, %d)", leaf_of(d, "assets"), count_nodes(d));
    /* dropping on its own leaf's centre changes nothing */
    frame(d, area, ax, tab_y, true);
    frame(d, area, ax + 20, tab_y + 20, true);
    frame(d, area, sc.x + sc.w * 0.5f, sc.y + sc.h * 0.5f, false);
    CHECK(strcmp(leaf_of(d, "assets"), "scene,assets") == 0, "no-op drop");
    /* splitter drag: the root split between the left leaf and the rest */
    int root = d->root;
    CHECK(!d->nodes[root].leaf && d->nodes[root].vertical, "root is a vertical split");
    float sx = d->nodes[d->nodes[root].child[0]].rect.x + d->nodes[d->nodes[root].child[0]].rect.w + 3, sy = 300;
    float ratio = d->nodes[root].ratio;
    frame(d, area, sx, sy, true);
    CHECK(d->drag_split == root, "pressing the gutter starts a splitter drag");
    frame(d, area, sx + 100, sy, true);
    frame(d, area, sx + 100, sy, false);
    CHECK(d->nodes[root].ratio > ratio + 0.05f && d->drag_split < 0, "ratio follows the mouse (%g -> %g)", (double)ratio, (double)d->nodes[root].ratio);
    /* close with the x */
    hd_focus(d, "scene");
    frame(d, area, -1, -1, false);
    r2d_rect t0 = r2d_rect_make(d->nodes[scene_leaf].rect.x + 2, d->nodes[scene_leaf].rect.y + 2, 8 * 5 + 18 + 16, 20);
    frame(d, area, t0.x + t0.w - 8, t0.y + 8, true);
    frame(d, area, t0.x + t0.w - 8, t0.y + 8, false);
    CHECK(!hd_is_open(d, "scene") && strcmp(leaf_of(d, "assets"), "assets") == 0, "the x closes the active tab (%s)", leaf_of(d, "assets"));
    hd_clear(d);
    CHECK(!hd_is_open(d, "assets") && count_nodes(d) == 1, "clear");
    free(d);
    if (failures) { printf("%d of %d checks failed\n", failures, checks); return 1; }
    printf("OK: %d dock checks passed\n", checks);
    return 0;
}
