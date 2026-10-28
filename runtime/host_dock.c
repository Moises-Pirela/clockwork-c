#define _CRT_SECURE_NO_WARNINGS 1
#include "host_dock.h"
#include "host_ui.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * nodes
 * ------------------------------------------------------------------------- */
static int node_alloc(hd_dock *d, bool leaf) {
    for (int i = 0; i < HD_MAX_NODES; ++i) {
        if (d->nodes[i].used) continue;
        hd_node *n = &d->nodes[i];
        memset(n, 0, sizeof(*n));
        n->used = true;
        n->leaf = leaf;
        n->ratio = 0.5f;
        n->parent = n->child[0] = n->child[1] = -1;
        return i;
    }
    return -1;
}
static void node_free(hd_dock *d, int i) { if (i >= 0) d->nodes[i].used = false; }
static void replace_child(hd_dock *d, int parent, int old, int with) {
    if (parent < 0) { d->root = with; if (with >= 0) d->nodes[with].parent = -1; return; }
    hd_node *p = &d->nodes[parent];
    if (p->child[0] == old) p->child[0] = with; else if (p->child[1] == old) p->child[1] = with;
    if (with >= 0) d->nodes[with].parent = parent;
}
static int first_leaf(const hd_dock *d, int node) {
    while (node >= 0 && !d->nodes[node].leaf) node = d->nodes[node].child[0];
    return node;
}
static void leaf_remove_tab(hd_node *n, int panel) {
    for (int i = 0; i < n->tab_count; ++i) {
        if (n->tabs[i] != panel) continue;
        memmove(n->tabs + i, n->tabs + i + 1, (size_t)(n->tab_count - i - 1) * sizeof(*n->tabs));
        --n->tab_count;
        if (n->active >= n->tab_count) n->active = n->tab_count ? n->tab_count - 1 : 0;
        if (n->active > i) --n->active;
        return;
    }
}
/* Removes a panel from its leaf; an emptied non-root leaf collapses into its sibling. */
static void undock(hd_dock *d, int panel) {
    hd_panel *p = &d->panels[panel];
    if (!p->open || p->leaf < 0) { p->open = false; p->leaf = -1; return; }
    hd_node *leaf = &d->nodes[p->leaf];
    leaf_remove_tab(leaf, panel);
    int node = p->leaf;
    p->open = false;
    p->leaf = -1;
    if (leaf->tab_count || leaf->parent < 0) return;
    int parent = leaf->parent;
    hd_node *pn = &d->nodes[parent];
    int sibling = pn->child[0] == node ? pn->child[1] : pn->child[0];
    replace_child(d, pn->parent, parent, sibling);
    node_free(d, node);
    node_free(d, parent);
    d->changed = true;
}
static void dock_into(hd_dock *d, int panel, int target, hd_side side, float ratio) {
    hd_panel *p = &d->panels[panel];
    if (target < 0) target = first_leaf(d, d->root);
    if (target < 0) return;
    hd_node *t = &d->nodes[target];
    if (side == HD_CENTER || !t->tab_count) {
        if (t->tab_count < HD_MAX_TABS) {
            t->tabs[t->tab_count] = panel;
            t->active = t->tab_count++;
            p->open = true;
            p->leaf = target;
        }
        d->changed = true;
        return;
    }
    int leaf = node_alloc(d, true), split = node_alloc(d, false);
    if (leaf < 0 || split < 0) { node_free(d, leaf); node_free(d, split); return; }
    hd_node *n = &d->nodes[leaf], *s = &d->nodes[split];
    n->tabs[0] = panel;
    n->tab_count = 1;
    n->active = 0;
    s->vertical = side == HD_LEFT || side == HD_RIGHT;
    bool first = side == HD_LEFT || side == HD_TOP;
    s->child[0] = first ? leaf : target;
    s->child[1] = first ? target : leaf;
    s->ratio = first ? ratio : 1.0f - ratio;
    if (s->ratio < 0.1f) s->ratio = 0.1f;
    if (s->ratio > 0.9f) s->ratio = 0.9f;
    int parent = t->parent;
    replace_child(d, parent, target, split);
    d->nodes[target].parent = split;
    n->parent = split;
    p->open = true;
    p->leaf = leaf;
    d->changed = true;
}

/* ---------------------------------------------------------------------------
 * api
 * ------------------------------------------------------------------------- */
void hd_init(hd_dock *d) {
    memset(d, 0, sizeof(*d));
    d->root = node_alloc(d, true);
    d->tab_height = 24;
    d->gutter = 6;
    d->drag_panel = d->press_panel = d->drag_split = d->hover_leaf = -1;
}
int hd_find(const hd_dock *d, const char *id) {
    for (int i = 0; i < d->panel_count && id; ++i)
        if (strcmp(d->panels[i].id, id) == 0) return i;
    return -1;
}
int hd_add_panel(hd_dock *d, const char *id, const char *title, hd_panel_fn draw, void *user, bool transparent) {
    if (d->panel_count == HD_MAX_PANELS || hd_find(d, id) >= 0) return -1;
    hd_panel *p = &d->panels[d->panel_count];
    memset(p, 0, sizeof(*p));
    snprintf(p->id, sizeof(p->id), "%s", id);
    snprintf(p->title, sizeof(p->title), "%s", title ? title : id);
    p->draw = draw;
    p->user = user;
    p->transparent = transparent;
    p->leaf = -1;
    return d->panel_count++;
}
bool hd_dock_panel(hd_dock *d, const char *id, hd_side side, float ratio, const char *relative_to) {
    int panel = hd_find(d, id);
    if (panel < 0) return false;
    int target = -1;
    if (relative_to) {
        int rel = hd_find(d, relative_to);
        if (rel < 0 || !d->panels[rel].open) return false;
        target = d->panels[rel].leaf;
    }
    if (d->panels[panel].open) undock(d, panel);
    if (target < 0) {
        /* relative to the whole dock space: split the root */
        if (side != HD_CENTER && d->root >= 0 && (!d->nodes[d->root].leaf || d->nodes[d->root].tab_count)) {
            int leaf = node_alloc(d, true), split = node_alloc(d, false);
            if (leaf < 0 || split < 0) { node_free(d, leaf); node_free(d, split); return false; }
            hd_node *n = &d->nodes[leaf], *s = &d->nodes[split];
            n->tabs[0] = panel; n->tab_count = 1; n->active = 0;
            s->vertical = side == HD_LEFT || side == HD_RIGHT;
            bool first = side == HD_LEFT || side == HD_TOP;
            s->child[0] = first ? leaf : d->root;
            s->child[1] = first ? d->root : leaf;
            s->ratio = first ? ratio : 1.0f - ratio;
            d->nodes[d->root].parent = split;
            n->parent = split;
            s->parent = -1;
            d->root = split;
            d->panels[panel].open = true;
            d->panels[panel].leaf = leaf;
            d->changed = true;
            return true;
        }
        target = first_leaf(d, d->root);
        side = HD_CENTER;
    }
    dock_into(d, panel, target, side, ratio);
    return d->panels[panel].open;
}
void hd_close(hd_dock *d, const char *id) {
    int panel = hd_find(d, id);
    if (panel >= 0 && d->panels[panel].open) { undock(d, panel); d->changed = true; }
}
void hd_show(hd_dock *d, const char *id) {
    int panel = hd_find(d, id);
    if (panel < 0) return;
    if (d->panels[panel].open) { hd_focus(d, id); return; }
    /* prefer a leaf that holds opaque panels, else split the root on the right */
    int best = -1;
    for (int i = 0; i < HD_MAX_NODES; ++i) {
        const hd_node *n = &d->nodes[i];
        if (!n->used || !n->leaf || !n->tab_count) continue;
        if (d->panels[n->tabs[0]].transparent) continue;
        if (best < 0 || n->tab_count < d->nodes[best].tab_count) best = i;
    }
    if (best >= 0) dock_into(d, panel, best, HD_CENTER, 0);
    else hd_dock_panel(d, id, HD_RIGHT, 0.25f, NULL);
}
void hd_focus(hd_dock *d, const char *id) {
    int panel = hd_find(d, id);
    if (panel < 0 || !d->panels[panel].open) return;
    hd_node *n = &d->nodes[d->panels[panel].leaf];
    for (int i = 0; i < n->tab_count; ++i) if (n->tabs[i] == panel) n->active = i;
}
bool hd_is_open(const hd_dock *d, const char *id) {
    int panel = hd_find(d, id);
    return panel >= 0 && d->panels[panel].open;
}
bool hd_panel_rect(const hd_dock *d, const char *id, r2d_rect *content) {
    int panel = hd_find(d, id);
    if (panel < 0 || !d->panels[panel].open) return false;
    const hd_node *n = &d->nodes[d->panels[panel].leaf];
    if (n->tabs[n->active] != panel) return false;
    if (content) *content = n->content;
    return true;
}
void hd_clear(hd_dock *d) {
    for (int i = 0; i < d->panel_count; ++i) { d->panels[i].open = false; d->panels[i].leaf = -1; }
    for (int i = 0; i < HD_MAX_NODES; ++i) d->nodes[i].used = false;
    d->root = node_alloc(d, true);
    d->drag_panel = d->press_panel = d->drag_split = d->hover_leaf = -1;
    d->changed = true;
}

/* ---------------------------------------------------------------------------
 * frame
 * ------------------------------------------------------------------------- */
static void layout(hd_dock *d, int node, r2d_rect r) {
    hd_node *n = &d->nodes[node];
    n->rect = r;
    if (n->leaf) {
        float th = n->tab_count ? d->tab_height : 0;
        n->content = r2d_rect_make(r.x, r.y + th, r.w, r.h - th > 0 ? r.h - th : 0);
        return;
    }
    float g = d->gutter;
    if (n->vertical) {
        float w0 = (r.w - g) * n->ratio;
        layout(d, n->child[0], r2d_rect_make(r.x, r.y, w0, r.h));
        layout(d, n->child[1], r2d_rect_make(r.x + w0 + g, r.y, r.w - w0 - g, r.h));
    } else {
        float h0 = (r.h - g) * n->ratio;
        layout(d, n->child[0], r2d_rect_make(r.x, r.y, r.w, h0));
        layout(d, n->child[1], r2d_rect_make(r.x, r.y + h0 + g, r.w, r.h - h0 - g));
    }
}
static r2d_rect splitter_rect(const hd_dock *d, const hd_node *n) {
    float g = d->gutter;
    if (n->vertical) {
        float x = d->nodes[n->child[0]].rect.x + d->nodes[n->child[0]].rect.w;
        return r2d_rect_make(x, n->rect.y, g, n->rect.h);
    }
    float y = d->nodes[n->child[0]].rect.y + d->nodes[n->child[0]].rect.h;
    return r2d_rect_make(n->rect.x, y, n->rect.w, g);
}
static bool closable(const hd_dock *d, int panel) { return !d->panels[panel].transparent; }
static float tab_width(const hd_dock *d, int panel, bool active) {
    return r2d_text_width(d->panels[panel].title) + 18 + (active && closable(d, panel) ? 16 : 0);
}
/* tabs shrink together when the bar is too narrow for all of them */
static float tab_scale(const hd_dock *d, const hd_node *n) {
    float total = 2;
    for (int i = 0; i < n->tab_count; ++i) total += tab_width(d, n->tabs[i], i == n->active) + 2;
    return total > n->rect.w && total > 0 ? n->rect.w / total : 1.0f;
}
static r2d_rect tab_rect(const hd_dock *d, const hd_node *n, int index) {
    float scale = tab_scale(d, n), x = n->rect.x + 2;
    for (int i = 0; i < index; ++i) x += tab_width(d, n->tabs[i], i == n->active) * scale + 2;
    return r2d_rect_make(x, n->rect.y + 2, tab_width(d, n->tabs[index], index == n->active) * scale, d->tab_height - 4);
}
static hd_side drop_side(r2d_rect r, r2d_vec2 m) {
    float fx = (m.x - r.x) / (r.w > 0 ? r.w : 1), fy = (m.y - r.y) / (r.h > 0 ? r.h : 1);
    if (fx > 0.25f && fx < 0.75f && fy > 0.25f && fy < 0.75f) return HD_CENTER;
    float dl = fx, dr = 1 - fx, dt = fy, db = 1 - fy;
    float best = dl;
    hd_side side = HD_LEFT;
    if (dr < best) { best = dr; side = HD_RIGHT; }
    if (dt < best) { best = dt; side = HD_TOP; }
    if (db < best) { side = HD_BOTTOM; }
    return side;
}
static r2d_rect drop_preview(r2d_rect r, hd_side side) {
    switch (side) {
        case HD_LEFT: return r2d_rect_make(r.x, r.y, r.w * 0.4f, r.h);
        case HD_RIGHT: return r2d_rect_make(r.x + r.w * 0.6f, r.y, r.w * 0.4f, r.h);
        case HD_TOP: return r2d_rect_make(r.x, r.y, r.w, r.h * 0.4f);
        case HD_BOTTOM: return r2d_rect_make(r.x, r.y + r.h * 0.6f, r.w, r.h * 0.4f);
        default: return r;
    }
}
static void draw_leaf(hd_dock *d, int node, r2d_vec2 mouse) {
    hd_node *n = &d->nodes[node];
    if (!n->tab_count) {
        r2d_draw_rect(n->rect, r2d_rgba(30, 33, 40, 200));
        r2d_draw_text(r2d_v2(n->rect.x + 8, n->rect.y + 8), hui_dim, "empty: drop a tab here");
        r2d_ui_block(n->rect);
        return;
    }
    int active_panel = n->tabs[n->active];
    const hd_panel *active = &d->panels[active_panel];
    /* tab bar */
    r2d_rect bar = r2d_rect_make(n->rect.x, n->rect.y, n->rect.w, d->tab_height);
    r2d_draw_rect(bar, r2d_rgb(24, 26, 32));
    r2d_ui_block(bar);
    for (int i = 0; i < n->tab_count; ++i) {
        int panel = n->tabs[i];
        r2d_rect t = tab_rect(d, n, i);
        bool is_active = i == n->active, hover = r2d_rect_contains(t, mouse);
        r2d_draw_rect(t, is_active ? r2d_rgb(44, 48, 58) : (hover ? r2d_rgb(38, 41, 50) : r2d_rgb(30, 33, 40)));
        if (is_active) r2d_draw_rect(r2d_rect_make(t.x, t.y, t.w, 2), hui_accent);
        /* the title, cut to what fits */
        char title[40];
        float glyph = r2d_text_width("M") > 0 ? r2d_text_width("M") : 8;
        float room = t.w - 12 - (is_active && closable(d, panel) ? 14 : 0);
        size_t max_chars = room > glyph ? (size_t)(room / glyph) : 1;
        snprintf(title, sizeof(title), "%.*s", (int)(max_chars < sizeof(title) - 1 ? max_chars : sizeof(title) - 1), d->panels[panel].title);
        r2d_draw_text(r2d_v2(t.x + 6, t.y + (t.h - r2d_text_height()) * 0.5f), is_active ? hui_white : hui_dim, title);
        if (is_active && closable(d, panel)) {
            r2d_rect x = r2d_rect_make(t.x + t.w - 16, t.y + 2, 14, t.h - 4);
            bool xhover = r2d_rect_contains(x, mouse);
            r2d_draw_text(r2d_v2(x.x + 3, x.y + (x.h - r2d_text_height()) * 0.5f), xhover ? hui_red : hui_dim, "x");
            if (xhover && r2d_mouse_pressed(R2D_MOUSE_LEFT)) { d->press_panel = -1; undock(d, panel); d->changed = true; return; }
        }
        if (hover && r2d_mouse_pressed(R2D_MOUSE_LEFT) && d->drag_panel < 0) {
            n->active = i;
            d->press_panel = panel;
            d->press_at = mouse;
            active_panel = panel;
            active = &d->panels[panel];
        }
    }
    /* content */
    if (!active->transparent) {
        r2d_draw_rect(n->content, r2d_rgb(22, 24, 29));   /* opaque: whatever the host draws underneath stays hidden */
        r2d_ui_panel(n->content);
    }
    if (active->draw && n->content.w > 8 && n->content.h > 8) active->draw(n->content, active->user);
}
static void draw_node(hd_dock *d, int node, r2d_vec2 mouse) {
    hd_node *n = &d->nodes[node];
    if (n->leaf) { draw_leaf(d, node, mouse); return; }
    draw_node(d, n->child[0], mouse);
    draw_node(d, n->child[1], mouse);
    r2d_rect s = splitter_rect(d, n);
    bool hover = r2d_rect_contains(r2d_rect_expand(s, 2), mouse) || d->drag_split == node;
    r2d_draw_rect(s, hover ? r2d_rgba(120, 130, 160, 160) : r2d_rgba(60, 64, 76, 120));
    r2d_ui_block(r2d_rect_expand(s, 2));
    if (hover && r2d_mouse_pressed(R2D_MOUSE_LEFT) && d->drag_split < 0 && d->press_panel < 0) d->drag_split = node;
}
void hd_frame(hd_dock *d, r2d_rect area) {
    ++d->frame;
    if (d->root < 0) return;
    r2d_set_space(R2D_SPACE_SCREEN);
    r2d_vec2 mouse = r2d_mouse();
    /* splitter drags */
    if (d->drag_split >= 0) {
        hd_node *s = &d->nodes[d->drag_split];
        if (!s->used || s->leaf || !r2d_mouse_down(R2D_MOUSE_LEFT)) { if (d->drag_split >= 0) d->changed = true; d->drag_split = -1; }
        else {
            float t = s->vertical ? (mouse.x - s->rect.x) / (s->rect.w > 0 ? s->rect.w : 1) : (mouse.y - s->rect.y) / (s->rect.h > 0 ? s->rect.h : 1);
            s->ratio = r2d_clampf(t, 0.08f, 0.92f);
        }
    }
    /* tab press -> drag */
    if (d->press_panel >= 0) {
        if (!r2d_mouse_down(R2D_MOUSE_LEFT)) d->press_panel = -1;
        else if (r2d_distance(mouse, d->press_at) > 6) { d->drag_panel = d->press_panel; d->press_panel = -1; }
    }
    layout(d, d->root, area);
    draw_node(d, d->root, mouse);
    /* tab drag: pick the target and drop */
    d->hover_leaf = -1;
    if (d->drag_panel >= 0) {
        for (int i = 0; i < HD_MAX_NODES; ++i) {
            const hd_node *n = &d->nodes[i];
            if (n->used && n->leaf && r2d_rect_contains(n->rect, mouse)) { d->hover_leaf = i; d->hover_side = drop_side(n->rect, mouse); break; }
        }
        if (d->hover_leaf >= 0) {
            r2d_rect p = drop_preview(d->nodes[d->hover_leaf].rect, d->hover_side);
            r2d_draw_rect(p, r2d_rgba(120, 160, 230, 60));
            r2d_draw_rect_outline(p, 2, r2d_rgba(150, 190, 255, 200));
        }
        const char *title = d->panels[d->drag_panel].title;
        r2d_rect tag = r2d_rect_make(mouse.x + 12, mouse.y + 12, r2d_text_width(title) + 16, d->tab_height - 4);
        r2d_draw_rect(tag, r2d_rgba(44, 48, 58, 230));
        r2d_draw_rect_outline(tag, 1, hui_accent);
        r2d_draw_text(r2d_v2(tag.x + 8, tag.y + (tag.h - r2d_text_height()) * 0.5f), hui_white, title);
        if (!r2d_mouse_down(R2D_MOUSE_LEFT)) {
            int panel = d->drag_panel, target = d->hover_leaf;
            d->drag_panel = -1;
            if (target >= 0) {
                hd_node *t = &d->nodes[target];
                bool own = d->panels[panel].leaf == target;
                if (!(own && d->hover_side == HD_CENTER) && !(own && t->tab_count == 1)) {
                    undock(d, panel);
                    /* the target may have collapsed away with the panel's old leaf */
                    if (!d->nodes[target].used || !d->nodes[target].leaf) target = first_leaf(d, d->root);
                    dock_into(d, panel, target, d->hover_side, 0.4f);
                    d->changed = true;
                }
            }
        }
    }
    if (d->changed) layout(d, d->root, area);   /* rects stay consistent for hd_panel_rect */
}

/* ---------------------------------------------------------------------------
 * persistence
 * ------------------------------------------------------------------------- */
static void save_node(const hd_dock *d, int node, int depth, FILE *f) {
    const hd_node *n = &d->nodes[node];
    for (int i = 0; i < depth; ++i) fputs("  ", f);
    if (n->leaf) {
        fprintf(f, "leaf %d", n->active);
        for (int i = 0; i < n->tab_count; ++i) fprintf(f, " %s", d->panels[n->tabs[i]].id);
        fputc('\n', f);
        return;
    }
    fprintf(f, "split %c %.4f\n", n->vertical ? 'v' : 'h', (double)n->ratio);
    save_node(d, n->child[0], depth + 1, f);
    save_node(d, n->child[1], depth + 1, f);
}
bool hd_save(const hd_dock *d, const char *path) {
    if (!path || !*path || d->root < 0) return false;
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fputs("hdlayout 1\n", f);
    save_node(d, d->root, 0, f);
    return fclose(f) == 0;
}
typedef struct { char **lines; int count, next; } line_reader;
static int load_node(hd_dock *d, line_reader *r, int parent) {
    if (r->next >= r->count) return -1;
    char *line = r->lines[r->next++];
    while (*line && isspace((unsigned char)*line)) ++line;
    if (strncmp(line, "leaf ", 5) == 0) {
        int node = node_alloc(d, true);
        if (node < 0) return -1;
        hd_node *n = &d->nodes[node];
        n->parent = parent;
        char *p = line + 5;
        n->active = (int)strtol(p, &p, 10);
        for (;;) {
            while (*p && isspace((unsigned char)*p)) ++p;
            if (!*p) break;
            char *end = p;
            while (*end && !isspace((unsigned char)*end)) ++end;
            char saved = *end;
            *end = 0;
            int panel = hd_find(d, p);
            *end = saved;
            if (panel >= 0 && !d->panels[panel].open && n->tab_count < HD_MAX_TABS) {
                n->tabs[n->tab_count++] = panel;
                d->panels[panel].open = true;
                d->panels[panel].leaf = node;
            }
            p = end;
        }
        if (n->active < 0 || n->active >= n->tab_count) n->active = 0;
        return node;
    }
    if (strncmp(line, "split ", 6) == 0) {
        char dir = 0;
        float ratio = 0.5f;
        if (sscanf(line + 6, " %c %f", &dir, &ratio) != 2) return -1;
        int node = node_alloc(d, false);
        if (node < 0) return -1;
        hd_node *n = &d->nodes[node];
        n->parent = parent;
        n->vertical = dir == 'v';
        n->ratio = r2d_clampf(ratio, 0.08f, 0.92f);
        n->child[0] = load_node(d, r, node);
        n->child[1] = load_node(d, r, node);
        if (n->child[0] < 0 || n->child[1] < 0) return -1;
        return node;
    }
    return -1;
}
/* Drops empty leaves left by unknown panels so every leaf holds tabs (except a lone root). */
static int prune(hd_dock *d, int node) {
    hd_node *n = &d->nodes[node];
    if (n->leaf) return n->tab_count ? node : -1;
    int a = prune(d, n->child[0]), b = prune(d, n->child[1]);
    if (a >= 0 && b >= 0) { n->child[0] = a; n->child[1] = b; d->nodes[a].parent = node; d->nodes[b].parent = node; return node; }
    node_free(d, node);
    int keep = a >= 0 ? a : b;
    if (keep >= 0) d->nodes[keep].parent = n->parent;
    return keep;
}
bool hd_load(hd_dock *d, const char *path) {
    FILE *f = path && *path ? fopen(path, "r") : NULL;
    if (!f) return false;
    char *lines[HD_MAX_NODES * 2];
    int count = 0;
    char buffer[512];
    bool header = false;
    while (count < HD_MAX_NODES * 2 && fgets(buffer, sizeof(buffer), f)) {
        if (!header) { header = true; if (strncmp(buffer, "hdlayout 1", 10) != 0) { fclose(f); return false; } continue; }
        size_t len = strlen(buffer);
        while (len && isspace((unsigned char)buffer[len - 1])) buffer[--len] = 0;
        if (!len) continue;
        lines[count] = malloc(len + 1);
        if (!lines[count]) break;
        memcpy(lines[count++], buffer, len + 1);
    }
    fclose(f);
    /* build into a scratch dock so a bad file leaves the current layout alone */
    hd_dock scratch = *d;
    for (int i = 0; i < HD_MAX_NODES; ++i) scratch.nodes[i].used = false;
    for (int i = 0; i < scratch.panel_count; ++i) { scratch.panels[i].open = false; scratch.panels[i].leaf = -1; }
    line_reader r = {lines, count, 0};
    int root = load_node(&scratch, &r, -1);
    bool ok = root >= 0;
    if (ok) {
        root = prune(&scratch, root);
        if (root < 0) root = node_alloc(&scratch, true);
        scratch.root = root;
        scratch.nodes[root].parent = -1;
        /* leaves record their panels; make sure panel->leaf indexes survived pruning */
        for (int i = 0; i < HD_MAX_NODES; ++i) {
            const hd_node *n = &scratch.nodes[i];
            if (!n->used || !n->leaf) continue;
            for (int t = 0; t < n->tab_count; ++t) scratch.panels[n->tabs[t]].leaf = i;
        }
        for (int i = 0; i < scratch.panel_count; ++i)
            if (scratch.panels[i].open && (scratch.panels[i].leaf < 0 || !scratch.nodes[scratch.panels[i].leaf].used)) scratch.panels[i].open = false;
        scratch.drag_panel = scratch.press_panel = scratch.drag_split = scratch.hover_leaf = -1;
        *d = scratch;
        d->changed = true;
    }
    for (int i = 0; i < count; ++i) free(lines[i]);
    return ok;
}
