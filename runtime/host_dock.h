/* host_dock: dockable tabbed panels on top of render2d's immediate-mode UI.
 *
 * A dock space is a binary tree: split nodes divide their rectangle between
 * two children (side by side or stacked, by a draggable ratio), leaf nodes
 * hold tabs. A panel is a named draw callback; the host registers its
 * panels once, docks them into an initial layout, and calls hd_frame every
 * frame with the dock area. The layer draws tab bars, splitters and panel
 * backgrounds, handles clicking tabs, dragging a tab onto another leaf (the
 * centre adds it as a tab, an edge splits that leaf), dragging splitters,
 * closing tabs, and calls the active panel of every leaf with its content
 * rectangle. Layouts are saved and loaded as text so they survive restarts.
 *
 * Panels that must not paint a background (a viewport the world is drawn
 * under) are registered transparent; the dock then only draws their tab
 * bar and reports their content rectangle (hd_panel_rect).
 */
#ifndef HOST_DOCK_H
#define HOST_DOCK_H

#include "render2d.h"

#define HD_MAX_PANELS 24
#define HD_MAX_NODES 64
#define HD_MAX_TABS 12

typedef enum { HD_CENTER, HD_LEFT, HD_RIGHT, HD_TOP, HD_BOTTOM } hd_side;
typedef void (*hd_panel_fn)(r2d_rect content, void *user);

typedef struct {
    char id[32], title[32];
    hd_panel_fn draw;
    void *user;
    bool transparent;          /* no panel background: the host draws underneath */
    bool open;                 /* docked somewhere */
    int leaf;                  /* node index while open */
} hd_panel;

typedef struct {
    bool used, leaf;
    bool vertical;             /* split: children left/right; else top/bottom */
    float ratio;               /* split: share of the first child */
    int child[2], parent;
    int tabs[HD_MAX_TABS], tab_count, active;
    r2d_rect rect, content;    /* computed by hd_frame */
} hd_node;

typedef struct {
    hd_panel panels[HD_MAX_PANELS];
    int panel_count;
    hd_node nodes[HD_MAX_NODES];
    int root;
    float tab_height, gutter;
    /* interaction */
    int drag_panel;            /* tab being dragged, -1 */
    int press_panel;           /* tab pressed but not yet dragged, -1 */
    r2d_vec2 press_at;
    int drag_split;            /* splitter being dragged, -1 */
    int hover_leaf;            /* drop target while dragging, -1 */
    hd_side hover_side;
    bool changed;              /* the layout changed since the host last cleared this (save it) */
    uint64_t frame;
} hd_dock;

void hd_init(hd_dock *d);
int hd_add_panel(hd_dock *d, const char *id, const char *title, hd_panel_fn draw, void *user, bool transparent);
int hd_find(const hd_dock *d, const char *id);
/* Docks `id` next to `relative_to` (another panel's id; NULL: the whole dock
 * space). HD_CENTER adds a tab; a side splits off `ratio` of the target for
 * the new panel. Re-docks an already open panel. */
bool hd_dock_panel(hd_dock *d, const char *id, hd_side side, float ratio, const char *relative_to);
void hd_close(hd_dock *d, const char *id);
void hd_show(hd_dock *d, const char *id);          /* reopens a closed panel as a tab somewhere sensible */
void hd_focus(hd_dock *d, const char *id);         /* makes it the active tab of its leaf */
bool hd_is_open(const hd_dock *d, const char *id);
bool hd_panel_rect(const hd_dock *d, const char *id, r2d_rect *content);   /* this frame's content rect of an open, visible panel */
void hd_clear(hd_dock *d);                         /* closes everything; the host then rebuilds its default layout */

/* Lays out, draws chrome, handles input and calls the visible panels. */
void hd_frame(hd_dock *d, r2d_rect area);

/* Layout persistence (text). Loading validates against the registered panels
 * and leaves the current layout alone on any problem. */
bool hd_save(const hd_dock *d, const char *path);
bool hd_load(hd_dock *d, const char *path);

#endif
