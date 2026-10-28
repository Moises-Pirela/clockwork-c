/* cw_ui: a game's interface as entities.
 *
 * Menus, HUDs and dialogs are built from entities that carry the engine's
 * Ui components, edited in the editor like any other entity, saved in
 * scenes and drawn by the engine, so a game needs no interface code to show
 * a title, a health bar or a start button. The model is the one Unity's
 * canvas uses, reduced to what a 2D game needs:
 *
 *   UiRect      where an element is. Every element has a parent (another
 *               UiRect entity, or the canvas) and sits between two anchors
 *               given as fractions of the parent's rectangle (0..1, y down)
 *               plus pixel offsets from them:
 *                 x0 = parent.x0 + parent.w * anchor_min_x + left
 *                 x1 = parent.x0 + parent.w * anchor_max_x + right
 *               and the same for y with top and bottom. Equal anchors pin a
 *               fixed-size element to a point of the parent (a corner, the
 *               centre); different anchors stretch it with the parent (a
 *               bar across the top, a panel filling everything). That is
 *               what makes a layout survive any aspect ratio.
 *   UiImage     a material (cw_assets) or a flat colour filling the rect,
 *               optionally nine-sliced (`border`) so frames keep their
 *               corners at any size, optionally partially filled (bars).
 *               The material's shader (built-in interface effects such as
 *               rounded, ring or pulse, or a custom one) receives the
 *               element's pixel size, its eased hover/press state, its fill
 *               and four numbers of its own (param_x..w), so one material
 *               reacts to every element it is on.
 *   UiText      a line of text with a size, colour and alignment.
 *   UiButton    makes the element clickable: the engine tracks hover, press
 *               and click (cw_ui_clicked) and tints the image.
 *   UiLayout    transient, written by the layout: the element's window
 *               rectangle and its interaction state. Never saved.
 *
 * Resolution: offsets and sizes are in *reference pixels*. The canvas
 * (cw_ui_canvas, stored in the project) names a reference resolution and
 * how to scale when the window differs: `expand` keeps everything designed
 * for the reference visible (scale = min of the width and height ratios),
 * `shrink` fills the window (max), `width`/`height` follow one axis,
 * `pixels` never scales. The root rectangle is the window divided by that
 * scale, so anchored elements spread with the aspect ratio while their
 * sizes scale uniformly - a 1280x720 design looks the same on 1920x1080 and
 * keeps its corners on an ultra-wide or a phone.
 *
 * Hosts call cw_ui_layout_all with the window (or the game view) each frame
 * before drawing; cw_game.h wires this into bound worlds (cw_game_world
 * draws the interface after the module and feeds the mouse to the cw_ui
 * system so buttons work in the player, in play mode and in exports).
 */
#ifndef CW_UI_H
#define CW_UI_H

#include "cw_reflect.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CW_UI_TEXT_CAPACITY 96
#define CW_UI_DEFAULT_WIDTH 1280
#define CW_UI_DEFAULT_HEIGHT 720
#define CW_UI_MAX_DEPTH 32

typedef enum { CW_UI_SCALE_EXPAND, CW_UI_SCALE_SHRINK, CW_UI_SCALE_WIDTH, CW_UI_SCALE_HEIGHT, CW_UI_SCALE_PIXELS, CW_UI_SCALE_MODES } cw_ui_scale_mode;
typedef struct {
    float width, height;       /* the reference resolution the interface was designed for */
    cw_ui_scale_mode mode;
} cw_ui_canvas;
const char *cw_ui_scale_mode_name(cw_ui_scale_mode mode);
bool cw_ui_scale_mode_parse(const char *text, cw_ui_scale_mode *out);
cw_ui_canvas cw_ui_canvas_default(void);
/* The scale a screen of this size gets under the canvas (reference pixel -> window pixel). */
float cw_ui_scale(const cw_ui_canvas *canvas, float screen_w, float screen_h);

/* --- the components --- */
typedef struct {
    cw_entity_id parent;       /* a UiRect entity, or CW_INVALID_ENTITY: the canvas */
    float anchor_min_x, anchor_min_y, anchor_max_x, anchor_max_y;   /* fractions of the parent, y down */
    float left, top, right, bottom;                                 /* reference pixels from the anchors */
    int32_t order;             /* among siblings: lower draws first (below) */
    bool visible;              /* hides the element and everything under it */
} cw_ui_rect;

typedef enum { CW_UI_FILL_NONE, CW_UI_FILL_HORIZONTAL, CW_UI_FILL_VERTICAL } cw_ui_fill_mode;
typedef struct {
    uint32_t material;         /* a material asset; 0 draws the colour alone */
    uint8_t r, g, b, a;
    float border;              /* nine-slice border in texture pixels; 0 stretches the whole texture */
    uint8_t fill_mode;         /* cw_ui_fill_mode: a bar filled by `fill` from the left / the bottom */
    float fill;                /* 0..1; also handed to the shader (a ring, a custom bar) */
    float param_x, param_y, param_z, param_w;   /* the element's own shader inputs (`elem`); animatable */
} cw_ui_image;

typedef enum { CW_UI_ALIGN_LEFT, CW_UI_ALIGN_CENTER, CW_UI_ALIGN_RIGHT } cw_ui_align;
typedef enum { CW_UI_VALIGN_TOP, CW_UI_VALIGN_MIDDLE, CW_UI_VALIGN_BOTTOM } cw_ui_valign;
typedef struct {
    char text[CW_UI_TEXT_CAPACITY];
    float size;                /* 1 = the 8-pixel font at the reference scale */
    uint8_t r, g, b, a;
    uint8_t align, valign;     /* cw_ui_align, cw_ui_valign */
} cw_ui_text;

typedef struct {
    bool enabled;              /* disabled buttons ignore the mouse and draw dimmed */
    int32_t action;            /* a number for the game to tell buttons apart (cw_ui_clicked_action) */
    uint8_t hover_r, hover_g, hover_b;     /* image tint while hovered */
    uint8_t press_r, press_g, press_b;     /* image tint while pressed */
} cw_ui_button;

/* Transient: the result of the last layout and the interaction state. */
typedef struct {
    float x, y, w, h;          /* window pixels */
    float rx, ry, rw, rh;      /* reference pixels, relative to the screen's origin */
    float pw, ph;              /* the parent's reference size the anchors were applied to (the canvas for roots) */
    float scale;               /* reference -> window */
    bool shown;                /* visible, with every ancestor visible */
    bool hovered, pressed, clicked;   /* buttons; clicked holds for one update */
    float hover_t, press_t;    /* hovered/pressed eased to 0..1 over a few frames, for shaders */
    float since;               /* seconds since hovered or pressed last changed */
} cw_ui_layout;

extern const cw_type_info cw_ui_rect_type, cw_ui_image_type, cw_ui_text_type, cw_ui_button_type, cw_ui_layout_type;
extern const char *const cw_ui_align_names[3], *const cw_ui_valign_names[3], *const cw_ui_fill_names[3];

typedef struct { cw_component_id rect, image, text, button, layout; } cw_ui_components;
/* Registers the five components in a block; fills `out`. */
bool cw_ui_register(cw_world *w, cw_ui_components *out);
static inline bool cw_ui_ready(const cw_ui_components *c) { return c && c->rect != CW_INVALID_COMPONENT; }

/* --- layout --- */
typedef struct { float x, y, w, h; } cw_ui_screen;   /* where the interface goes, in window pixels */
/* Lays out every UiRect entity into its UiLayout (added when missing) for
 * this screen and canvas; returns how many. Parents are resolved through
 * any depth; a missing or cyclic parent counts as the canvas. */
size_t cw_ui_layout_all(cw_world *w, const cw_ui_components *c, const cw_ui_canvas *canvas, cw_ui_screen screen);
/* Entities with a UiRect in draw order (parents before children, siblings
 * by `order` then id); returns the count (bounded by capacity). */
size_t cw_ui_collect(cw_world *w, const cw_ui_components *c, cw_entity_id *out, size_t capacity);
/* The topmost shown element under a window point (buttons_only: one with an enabled UiButton). */
cw_entity_id cw_ui_hit(cw_world *w, const cw_ui_components *c, float x, float y, bool buttons_only);
/* Depth of an element in the hierarchy (0 at the canvas); also detects cycles. */
int cw_ui_depth(cw_world *w, const cw_ui_components *c, cw_entity_id id);
/* The parent's reference-space size the last layout used (the canvas for roots), for editing. */
bool cw_ui_parent_size(cw_world *w, const cw_ui_components *c, cw_entity_id id, float *pw, float *ph);

/* --- editing helpers: the rect's edges in the parent's reference space --- */
void cw_ui_rect_edges(const cw_ui_rect *r, float parent_w, float parent_h, float *x0, float *y0, float *x1, float *y1);
void cw_ui_rect_set_edges(cw_ui_rect *r, float parent_w, float parent_h, float x0, float y0, float x1, float y1);
/* Changes the anchors without moving the element on screen. */
void cw_ui_rect_set_anchors(cw_ui_rect *r, float parent_w, float parent_h, float min_x, float min_y, float max_x, float max_y);

/* --- interaction --- */
typedef struct { float x, y; bool down, pressed, released; } cw_ui_input;   /* window pixels; edges since the last update */
/* Updates hover/press/click of every button from the last layout; `dt`
 * advances the eased states shaders see (hover_t, press_t, since). */
void cw_ui_update(cw_world *w, const cw_ui_components *c, const cw_ui_input *in, float dt);
bool cw_ui_clicked(cw_world *w, const cw_ui_components *c, cw_entity_id id);
/* The first button clicked this update whose action is `action`, or CW_INVALID_ENTITY. */
cw_entity_id cw_ui_clicked_action(cw_world *w, const cw_ui_components *c, int32_t action);
bool cw_ui_set_text(cw_world *w, const cw_ui_components *c, cw_entity_id id, const char *text);
#if defined(__GNUC__)
__attribute__((format(printf, 4, 5)))
#endif
bool cw_ui_set_textf(cw_world *w, const cw_ui_components *c, cw_entity_id id, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
#endif
