#include "cw_ui.h"
#include "cw_internal.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

/* --- the canvas --- */
static const char *const mode_names[CW_UI_SCALE_MODES] = {"expand", "shrink", "width", "height", "pixels"};
const char *cw_ui_scale_mode_name(cw_ui_scale_mode mode) { return mode >= 0 && mode < CW_UI_SCALE_MODES ? mode_names[mode] : "?"; }
bool cw_ui_scale_mode_parse(const char *text, cw_ui_scale_mode *out) {
    for (int i = 0; text && i < CW_UI_SCALE_MODES; ++i)
        if (strcmp(mode_names[i], text) == 0) { if (out) *out = (cw_ui_scale_mode)i; return true; }
    return false;
}
cw_ui_canvas cw_ui_canvas_default(void) {
    cw_ui_canvas c = {CW_UI_DEFAULT_WIDTH, CW_UI_DEFAULT_HEIGHT, CW_UI_SCALE_EXPAND};
    return c;
}
float cw_ui_scale(const cw_ui_canvas *canvas, float screen_w, float screen_h) {
    if (!canvas || !(canvas->width > 0) || !(canvas->height > 0) || !(screen_w > 0) || !(screen_h > 0)) return 1;
    float sx = screen_w / canvas->width, sy = screen_h / canvas->height;
    switch (canvas->mode) {
        case CW_UI_SCALE_EXPAND: return sx < sy ? sx : sy;
        case CW_UI_SCALE_SHRINK: return sx > sy ? sx : sy;
        case CW_UI_SCALE_WIDTH: return sx;
        case CW_UI_SCALE_HEIGHT: return sy;
        default: return 1;
    }
}

/* --- the components --- */
const char *const cw_ui_align_names[3] = {"left", "center", "right"};
const char *const cw_ui_valign_names[3] = {"top", "middle", "bottom"};
const char *const cw_ui_fill_names[3] = {"none", "horizontal", "vertical"};

static const cw_field rect_fields[] = {
    CW_FIELD(cw_ui_rect, parent, CW_FIELD_ENTITY),
    CW_FIELD_RANGE(cw_ui_rect, anchor_min_x, CW_FIELD_F32, 0, 1, 0.5f),
    CW_FIELD_RANGE(cw_ui_rect, anchor_min_y, CW_FIELD_F32, 0, 1, 0.5f),
    CW_FIELD_RANGE(cw_ui_rect, anchor_max_x, CW_FIELD_F32, 0, 1, 0.5f),
    CW_FIELD_RANGE(cw_ui_rect, anchor_max_y, CW_FIELD_F32, 0, 1, 0.5f),
    CW_FIELD_RANGE(cw_ui_rect, left, CW_FIELD_F32, -8192, 8192, 1),
    CW_FIELD_RANGE(cw_ui_rect, top, CW_FIELD_F32, -8192, 8192, 1),
    CW_FIELD_RANGE(cw_ui_rect, right, CW_FIELD_F32, -8192, 8192, 1),
    CW_FIELD_RANGE(cw_ui_rect, bottom, CW_FIELD_F32, -8192, 8192, 1),
    CW_FIELD_RANGE(cw_ui_rect, order, CW_FIELD_I32, -1000, 1000, 1),
    CW_FIELD(cw_ui_rect, visible, CW_FIELD_BOOL),
};
static const cw_ui_rect rect_defaults = {CW_INVALID_ENTITY, 0.5f, 0.5f, 0.5f, 0.5f, -100, -25, 100, 25, 0, true};
static bool rect_valid(const void *v) {
    const cw_ui_rect *r = v;
    return r->anchor_min_x <= r->anchor_max_x && r->anchor_min_y <= r->anchor_max_y;
}
const cw_type_info cw_ui_rect_type = {"UiRect", sizeof(cw_ui_rect), _Alignof(cw_ui_rect), rect_fields, 11, &rect_defaults, rect_valid, 0, NULL, NULL};

static const cw_field image_fields[] = {
    CW_FIELD_SEM(cw_ui_image, material, CW_FIELD_U32, CW_SEM_ASSET),
    CW_FIELD_SEM(cw_ui_image, r, CW_FIELD_U8, CW_SEM_COLOR_R),
    CW_FIELD_SEM(cw_ui_image, g, CW_FIELD_U8, CW_SEM_COLOR_G),
    CW_FIELD_SEM(cw_ui_image, b, CW_FIELD_U8, CW_SEM_COLOR_B),
    CW_FIELD_RANGE(cw_ui_image, a, CW_FIELD_U8, 0, 255, 5),
    CW_FIELD_RANGE(cw_ui_image, border, CW_FIELD_F32, 0, 256, 1),
    CW_FIELD_NAMED(cw_ui_image, fill_mode, CW_FIELD_ENUM, cw_ui_fill_names, 3),
    CW_FIELD_RANGE(cw_ui_image, fill, CW_FIELD_F32, 0, 1, 0.05f),
    CW_FIELD_RANGE(cw_ui_image, param_x, CW_FIELD_F32, -100, 100, 0.1f),
    CW_FIELD_RANGE(cw_ui_image, param_y, CW_FIELD_F32, -100, 100, 0.1f),
    CW_FIELD_RANGE(cw_ui_image, param_z, CW_FIELD_F32, -100, 100, 0.1f),
    CW_FIELD_RANGE(cw_ui_image, param_w, CW_FIELD_F32, -100, 100, 0.1f),
};
static const cw_ui_image image_defaults = {0, 255, 255, 255, 255, 0, CW_UI_FILL_NONE, 1, 0, 0, 0, 0};
const cw_type_info cw_ui_image_type = {"UiImage", sizeof(cw_ui_image), _Alignof(cw_ui_image), image_fields, 12, &image_defaults, NULL, 0, NULL, NULL};

static const cw_field text_fields[] = {
    CW_FIELD_STR(cw_ui_text, text),
    CW_FIELD_RANGE(cw_ui_text, size, CW_FIELD_F32, 0.25f, 16, 0.25f),
    CW_FIELD_SEM(cw_ui_text, r, CW_FIELD_U8, CW_SEM_COLOR_R),
    CW_FIELD_SEM(cw_ui_text, g, CW_FIELD_U8, CW_SEM_COLOR_G),
    CW_FIELD_SEM(cw_ui_text, b, CW_FIELD_U8, CW_SEM_COLOR_B),
    CW_FIELD_RANGE(cw_ui_text, a, CW_FIELD_U8, 0, 255, 5),
    CW_FIELD_NAMED(cw_ui_text, align, CW_FIELD_ENUM, cw_ui_align_names, 3),
    CW_FIELD_NAMED(cw_ui_text, valign, CW_FIELD_ENUM, cw_ui_valign_names, 3),
};
static const cw_ui_text text_defaults = {"text", 2, 255, 255, 255, 255, CW_UI_ALIGN_CENTER, CW_UI_VALIGN_MIDDLE};
const cw_type_info cw_ui_text_type = {"UiText", sizeof(cw_ui_text), _Alignof(cw_ui_text), text_fields, 8, &text_defaults, NULL, 0, NULL, NULL};

static const cw_field button_fields[] = {
    CW_FIELD(cw_ui_button, enabled, CW_FIELD_BOOL),
    CW_FIELD_RANGE(cw_ui_button, action, CW_FIELD_I32, 0, 1000000, 1),
    CW_FIELD_SEM(cw_ui_button, hover_r, CW_FIELD_U8, CW_SEM_COLOR_R),
    CW_FIELD_SEM(cw_ui_button, hover_g, CW_FIELD_U8, CW_SEM_COLOR_G),
    CW_FIELD_SEM(cw_ui_button, hover_b, CW_FIELD_U8, CW_SEM_COLOR_B),
    CW_FIELD_SEM(cw_ui_button, press_r, CW_FIELD_U8, CW_SEM_COLOR_R),
    CW_FIELD_SEM(cw_ui_button, press_g, CW_FIELD_U8, CW_SEM_COLOR_G),
    CW_FIELD_SEM(cw_ui_button, press_b, CW_FIELD_U8, CW_SEM_COLOR_B),
};
static const cw_ui_button button_defaults = {true, 1, 230, 230, 230, 160, 160, 160};
const cw_type_info cw_ui_button_type = {"UiButton", sizeof(cw_ui_button), _Alignof(cw_ui_button), button_fields, 8, &button_defaults, NULL, 0, NULL, NULL};

static const cw_field layout_fields[] = {
    CW_FIELD(cw_ui_layout, x, CW_FIELD_F32), CW_FIELD(cw_ui_layout, y, CW_FIELD_F32),
    CW_FIELD(cw_ui_layout, w, CW_FIELD_F32), CW_FIELD(cw_ui_layout, h, CW_FIELD_F32),
    CW_FIELD(cw_ui_layout, rx, CW_FIELD_F32), CW_FIELD(cw_ui_layout, ry, CW_FIELD_F32),
    CW_FIELD(cw_ui_layout, rw, CW_FIELD_F32), CW_FIELD(cw_ui_layout, rh, CW_FIELD_F32),
    CW_FIELD(cw_ui_layout, pw, CW_FIELD_F32), CW_FIELD(cw_ui_layout, ph, CW_FIELD_F32),
    CW_FIELD(cw_ui_layout, scale, CW_FIELD_F32),
    CW_FIELD(cw_ui_layout, shown, CW_FIELD_BOOL),
    CW_FIELD(cw_ui_layout, hovered, CW_FIELD_BOOL), CW_FIELD(cw_ui_layout, pressed, CW_FIELD_BOOL), CW_FIELD(cw_ui_layout, clicked, CW_FIELD_BOOL),
    CW_FIELD(cw_ui_layout, hover_t, CW_FIELD_F32), CW_FIELD(cw_ui_layout, press_t, CW_FIELD_F32), CW_FIELD(cw_ui_layout, since, CW_FIELD_F32),
};
const cw_type_info cw_ui_layout_type = {"UiLayout", sizeof(cw_ui_layout), _Alignof(cw_ui_layout), layout_fields, 18, NULL, NULL, CW_TYPE_TRANSIENT, NULL, NULL};

bool cw_ui_register(cw_world *w, cw_ui_components *out) {
    if (!w || !out) return false;
    cw_component_id base = cw_comp_block(w, 5);
    if (base == CW_INVALID_COMPONENT) return false;
    out->rect = base; out->image = (cw_component_id)(base + 1); out->text = (cw_component_id)(base + 2);
    out->button = (cw_component_id)(base + 3); out->layout = (cw_component_id)(base + 4);
    return cw_reflect_register(w, out->rect, &cw_ui_rect_type) && cw_reflect_register(w, out->image, &cw_ui_image_type)
        && cw_reflect_register(w, out->text, &cw_ui_text_type) && cw_reflect_register(w, out->button, &cw_ui_button_type)
        && cw_reflect_register(w, out->layout, &cw_ui_layout_type);
}

/* --- edges --- */
void cw_ui_rect_edges(const cw_ui_rect *r, float pw, float ph, float *x0, float *y0, float *x1, float *y1) {
    if (x0) *x0 = pw * r->anchor_min_x + r->left;
    if (y0) *y0 = ph * r->anchor_min_y + r->top;
    if (x1) *x1 = pw * r->anchor_max_x + r->right;
    if (y1) *y1 = ph * r->anchor_max_y + r->bottom;
}
void cw_ui_rect_set_edges(cw_ui_rect *r, float pw, float ph, float x0, float y0, float x1, float y1) {
    r->left = x0 - pw * r->anchor_min_x;
    r->top = y0 - ph * r->anchor_min_y;
    r->right = x1 - pw * r->anchor_max_x;
    r->bottom = y1 - ph * r->anchor_max_y;
}
void cw_ui_rect_set_anchors(cw_ui_rect *r, float pw, float ph, float min_x, float min_y, float max_x, float max_y) {
    float x0, y0, x1, y1;
    cw_ui_rect_edges(r, pw, ph, &x0, &y0, &x1, &y1);
    r->anchor_min_x = min_x < 0 ? 0 : (min_x > 1 ? 1 : min_x);
    r->anchor_min_y = min_y < 0 ? 0 : (min_y > 1 ? 1 : min_y);
    r->anchor_max_x = max_x < r->anchor_min_x ? r->anchor_min_x : (max_x > 1 ? 1 : max_x);
    r->anchor_max_y = max_y < r->anchor_min_y ? r->anchor_min_y : (max_y > 1 ? 1 : max_y);
    cw_ui_rect_set_edges(r, pw, ph, x0, y0, x1, y1);
}

/* --- layout --- */
typedef struct {
    cw_world *w;
    const cw_ui_components *c;
    float scale, ox, oy;       /* screen origin */
    float root_w, root_h;      /* the canvas in reference pixels */
    unsigned char *state;      /* per entity: 0 pending, 1 in progress, 2 done */
} layout_pass;
static const cw_ui_rect *rect_of(cw_world *w, const cw_ui_components *c, cw_entity_id id) {
    return cw_world_is_valid(w, id) && cw_comp_has(w, id, c->rect) ? cw_comp_ptr(w, id, c->rect) : NULL;
}
static cw_ui_layout *layout_of(cw_world *w, const cw_ui_components *c, cw_entity_id id) {
    if (!cw_world_is_valid(w, id) || !cw_comp_has(w, id, c->rect)) return NULL;
    if (!cw_comp_has(w, id, c->layout)) cw_comp_add(w, id, c->layout);
    return cw_comp_ptr(w, id, c->layout);
}
/* Lays out `id` and, first, its parents. Returns its layout, or NULL when it has no rect. */
static const cw_ui_layout *resolve(layout_pass *p, cw_entity_id id, int depth) {
    const cw_ui_rect *r = rect_of(p->w, p->c, id);
    if (!r) return NULL;
    cw_ui_layout *l = layout_of(p->w, p->c, id);
    if (!l) return NULL;
    if (p->state[id] == 2) return l;
    float px = 0, py = 0, pw = p->root_w, ph = p->root_h;
    bool parent_shown = true;
    if (p->state[id] == 0 && depth < CW_UI_MAX_DEPTH && r->parent != id) {
        p->state[id] = 1;   /* a cycle through here resolves to the canvas */
        const cw_ui_layout *parent = resolve(p, r->parent, depth + 1);
        r = rect_of(p->w, p->c, id);   /* the pool may have moved */
        l = layout_of(p->w, p->c, id);
        if (parent) { px = parent->rx; py = parent->ry; pw = parent->rw; ph = parent->rh; parent_shown = parent->shown; }
    }
    float x0, y0, x1, y1;
    cw_ui_rect_edges(r, pw, ph, &x0, &y0, &x1, &y1);
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    l->rx = px + x0; l->ry = py + y0; l->rw = x1 - x0; l->rh = y1 - y0;
    l->pw = pw; l->ph = ph;
    l->scale = p->scale;
    l->x = p->ox + l->rx * p->scale; l->y = p->oy + l->ry * p->scale;
    l->w = l->rw * p->scale; l->h = l->rh * p->scale;
    l->shown = r->visible && parent_shown;
    p->state[id] = 2;
    return l;
}
size_t cw_ui_layout_all(cw_world *w, const cw_ui_components *c, const cw_ui_canvas *canvas, cw_ui_screen screen) {
    if (!w || !cw_ui_ready(c)) return 0;
    cw_ui_canvas cv = canvas ? *canvas : cw_ui_canvas_default();
    if (!(screen.w > 0) || !(screen.h > 0)) { screen.w = cv.width > 0 ? cv.width : CW_UI_DEFAULT_WIDTH; screen.h = cv.height > 0 ? cv.height : CW_UI_DEFAULT_HEIGHT; }
    layout_pass p;
    p.w = w; p.c = c;
    p.scale = cw_ui_scale(&cv, screen.w, screen.h);
    if (!(p.scale > 0)) p.scale = 1;
    p.ox = screen.x; p.oy = screen.y;
    p.root_w = screen.w / p.scale; p.root_h = screen.h / p.scale;
    p.state = calloc(w->capacity ? w->capacity : 1, 1);
    cw_require(p.state != NULL);
    size_t n = 0;
    for (size_t i = 0; i < w->capacity; ++i) {
        cw_entity_id id = (cw_entity_id)i;
        if (!cw_world_is_valid(w, id) || !cw_comp_has(w, id, c->rect)) continue;
        if (resolve(&p, id, 0)) ++n;
    }
    free(p.state);
    return n;
}
int cw_ui_depth(cw_world *w, const cw_ui_components *c, cw_entity_id id) {
    int depth = 0;
    for (const cw_ui_rect *r = rect_of(w, c, id); r && depth < CW_UI_MAX_DEPTH; r = rect_of(w, c, r->parent)) {
        if (r->parent == CW_INVALID_ENTITY || !rect_of(w, c, r->parent)) break;
        ++depth;
    }
    return depth;
}
bool cw_ui_parent_size(cw_world *w, const cw_ui_components *c, cw_entity_id id, float *pw, float *ph) {
    const cw_ui_rect *r = rect_of(w, c, id);
    if (!r || !cw_comp_has(w, id, c->layout)) return false;
    const cw_ui_layout *l = cw_comp_ptr(w, id, c->layout);
    if (pw) *pw = l->pw;
    if (ph) *ph = l->ph;
    return true;
}

/* --- draw order --- */
typedef struct { cw_entity_id id, parent; int32_t order; } node;
static int compare_nodes(const void *a, const void *b) {
    const node *x = a, *y = b;
    if (x->parent != y->parent) return x->parent < y->parent ? -1 : 1;
    if (x->order != y->order) return x->order < y->order ? -1 : 1;
    return x->id < y->id ? -1 : (x->id > y->id);
}
static void emit_children(const node *nodes, size_t count, cw_entity_id parent, cw_entity_id *out, size_t *n, size_t capacity, int depth) {
    if (depth > CW_UI_MAX_DEPTH) return;
    /* nodes are sorted by parent: find the run */
    size_t lo = 0, hi = count;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (nodes[mid].parent < parent) lo = mid + 1; else hi = mid; }
    for (size_t i = lo; i < count && nodes[i].parent == parent; ++i) {
        if (*n < capacity) out[(*n)++] = nodes[i].id;
        if (nodes[i].id != parent) emit_children(nodes, count, nodes[i].id, out, n, capacity, depth + 1);
    }
}
size_t cw_ui_collect(cw_world *w, const cw_ui_components *c, cw_entity_id *out, size_t capacity) {
    if (!w || !cw_ui_ready(c) || !out) return 0;
    size_t total = 0;
    for (size_t i = 0; i < w->capacity; ++i)
        if (cw_world_is_valid(w, (cw_entity_id)i) && cw_comp_has(w, (cw_entity_id)i, c->rect)) ++total;
    if (!total) return 0;
    node *nodes = malloc(sizeof(node) * total);
    cw_require(nodes != NULL);
    size_t k = 0;
    for (size_t i = 0; i < w->capacity && k < total; ++i) {
        cw_entity_id id = (cw_entity_id)i;
        if (!cw_world_is_valid(w, id) || !cw_comp_has(w, id, c->rect)) continue;
        const cw_ui_rect *r = cw_comp_ptr(w, id, c->rect);
        nodes[k].id = id;
        nodes[k].order = r->order;
        /* orphans (no parent, an invalid one, or a cycle) hang from the canvas */
        bool has_parent = r->parent != id && rect_of(w, c, r->parent) != NULL;
        nodes[k].parent = has_parent ? r->parent : CW_INVALID_ENTITY;
        ++k;
    }
    qsort(nodes, k, sizeof(node), compare_nodes);
    size_t n = 0;
    emit_children(nodes, k, CW_INVALID_ENTITY, out, &n, capacity, 0);
    /* nodes inside a cycle are reachable from no root: append them so nothing disappears */
    if (n < k) {
        for (size_t i = 0; i < k && n < capacity; ++i) {
            bool listed = false;
            for (size_t j = 0; j < n && !listed; ++j) listed = out[j] == nodes[i].id;
            if (!listed) out[n++] = nodes[i].id;
        }
    }
    free(nodes);
    return n;
}
cw_entity_id cw_ui_hit(cw_world *w, const cw_ui_components *c, float x, float y, bool buttons_only) {
    if (!w || !cw_ui_ready(c)) return CW_INVALID_ENTITY;
    cw_entity_id ids[4096];
    size_t n = cw_ui_collect(w, c, ids, 4096);
    for (size_t i = n; i-- > 0;) {
        cw_entity_id id = ids[i];
        if (!cw_comp_has(w, id, c->layout)) continue;
        const cw_ui_layout *l = cw_comp_ptr(w, id, c->layout);
        if (!l->shown) continue;
        if (buttons_only) {
            if (!cw_comp_has(w, id, c->button)) continue;
            const cw_ui_button *b = cw_comp_ptr(w, id, c->button);
            if (!b->enabled) continue;
        }
        if (x >= l->x && y >= l->y && x < l->x + l->w && y < l->y + l->h) return id;
    }
    return CW_INVALID_ENTITY;
}

/* --- interaction --- */
static float ease_to(float value, float target, float dt) {   /* most of the way in about a tenth of a second */
    float k = dt * 14.0f;
    if (k > 1) k = 1;
    value += (target - value) * k;
    return fabsf(value - target) < 0.002f ? target : value;
}
void cw_ui_update(cw_world *w, const cw_ui_components *c, const cw_ui_input *in, float dt) {
    if (!w || !cw_ui_ready(c)) return;
    if (!(dt >= 0)) dt = 0;
    cw_entity_id hot = in ? cw_ui_hit(w, c, in->x, in->y, true) : CW_INVALID_ENTITY;
    cw_query q = cw_query_iter(w, cw_mask(&c->layout, 1));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        cw_ui_layout *l = cw_comp_ptr(w, id, c->layout);
        l->clicked = false;
        bool was_hovered = l->hovered, was_pressed = l->pressed;
        bool button = cw_comp_has(w, id, c->button) && ((const cw_ui_button *)cw_comp_ptr(w, id, c->button))->enabled && l->shown;
        if (!button || !in) l->hovered = l->pressed = false;
        else {
            l->hovered = id == hot;
            if (in->pressed && l->hovered) l->pressed = true;
            if (in->released) {
                if (l->pressed && l->hovered) l->clicked = true;
                l->pressed = false;
            }
            if (!in->down) l->pressed = false;
        }
        l->since = (l->hovered != was_hovered || l->pressed != was_pressed) ? 0 : l->since + dt;
        l->hover_t = ease_to(l->hover_t, l->hovered ? 1.0f : 0.0f, dt);
        l->press_t = ease_to(l->press_t, l->pressed ? 1.0f : 0.0f, dt);
    }
}
bool cw_ui_clicked(cw_world *w, const cw_ui_components *c, cw_entity_id id) {
    if (!w || !cw_ui_ready(c) || !cw_world_is_valid(w, id) || !cw_comp_has(w, id, c->layout)) return false;
    return ((const cw_ui_layout *)cw_comp_ptr(w, id, c->layout))->clicked;
}
cw_entity_id cw_ui_clicked_action(cw_world *w, const cw_ui_components *c, int32_t action) {
    if (!w || !cw_ui_ready(c)) return CW_INVALID_ENTITY;
    cw_component_id both[2] = {c->layout, c->button};
    cw_query q = cw_query_iter(w, cw_mask(both, 2));
    cw_entity_id id;
    while (cw_query_next(&q, &id)) {
        const cw_ui_layout *l = cw_comp_ptr(w, id, c->layout);
        const cw_ui_button *b = cw_comp_ptr(w, id, c->button);
        if (l->clicked && b->action == action) return id;
    }
    return CW_INVALID_ENTITY;
}
bool cw_ui_set_text(cw_world *w, const cw_ui_components *c, cw_entity_id id, const char *text) {
    if (!w || !cw_ui_ready(c) || !text || !cw_world_is_valid(w, id) || !cw_comp_has(w, id, c->text)) return false;
    cw_ui_text *t = cw_comp_ptr(w, id, c->text);
    snprintf(t->text, sizeof(t->text), "%s", text);
    return true;
}
bool cw_ui_set_textf(cw_world *w, const cw_ui_components *c, cw_entity_id id, const char *fmt, ...) {
    char buffer[CW_UI_TEXT_CAPACITY];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return cw_ui_set_text(w, c, id, buffer);
}
