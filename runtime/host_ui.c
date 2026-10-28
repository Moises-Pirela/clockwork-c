#include "host_ui.h"
#include <stdio.h>
#include <string.h>

const r2d_color hui_yellow = {255, 214, 80, 255}, hui_white = {240, 242, 246, 255};
const r2d_color hui_red = {255, 120, 110, 255}, hui_green = {150, 220, 160, 255};
const r2d_color hui_accent = {90, 100, 120, 255}, hui_plain = {58, 64, 76, 255}, hui_dim = {150, 155, 165, 255};

static struct {
    const void *focus;
    char edit[256];
    float time;
} S;

static struct { bool active; float top, bottom; } clip;
void hui_clip(float top, float bottom) { clip.active = true; clip.top = top; clip.bottom = bottom; }
void hui_unclip(void) { clip.active = false; }
bool hui_visible(r2d_rect r) { return !clip.active || (r.y >= clip.top && r.y + r.h <= clip.bottom); }
static bool text_visible(r2d_vec2 pos) { return hui_visible(r2d_rect_make(pos.x, pos.y, 1, r2d_text_height())); }
void hui_label(r2d_vec2 pos, const char *text) { if (text_visible(pos)) r2d_ui_label(pos, text); }
r2d_rect hui_row(hui_layout *l, float h) {
    r2d_rect r = r2d_rect_make(l->x, l->y, l->w, h);
    l->y += h + 4;
    return r;
}
r2d_rect hui_split(r2d_rect r, int i, int n) {
    float gap = 4, w = (r.w - gap * (float)(n - 1)) / (float)n;
    return r2d_rect_make(r.x + (float)i * (w + gap), r.y, w, r.h);
}
void hui_heading(hui_layout *l, const char *text) {
    l->y += 3;
    if (text_visible(r2d_v2(l->x, l->y))) r2d_ui_label_dim(r2d_v2(l->x, l->y), text);
    l->y += r2d_text_height() + 5;
}
void hui_line(hui_layout *l, r2d_color c, const char *text) {
    if (text_visible(r2d_v2(l->x, l->y))) r2d_ui_label_colored(r2d_v2(l->x, l->y), c, text);
    l->y += r2d_text_height() + 3;
}
bool hui_button(r2d_rect r, const char *label, bool highlighted) {
    if (!hui_visible(r)) return false;
    return r2d_ui_button_colored(r, label, highlighted ? hui_accent : hui_plain);
}
void hui_frame(float dt) { S.time += dt; }
bool hui_focused(void) { return S.focus != NULL; }
const void *hui_focus_key(void) { return S.focus; }
void hui_blur(void) { S.focus = NULL; }

bool hui_textfield(r2d_rect r, const void *key, char *buffer, size_t capacity) {
    bool focused = S.focus == key, committed = false, visible = hui_visible(r);
    if (visible) r2d_ui_block(r);
    bool over = visible && r2d_rect_contains(r, r2d_mouse());
    if (r2d_mouse_pressed(R2D_MOUSE_LEFT)) {
        if (over && !focused) {
            S.focus = key;
            focused = true;
            snprintf(S.edit, sizeof(S.edit), "%s", buffer);
        } else if (!over && focused) {
            snprintf(buffer, capacity, "%s", S.edit);
            S.focus = NULL;
            focused = false;
            committed = true;
        }
    }
    if (focused) {
        size_t n;
        const uint32_t *chars = r2d_chars(&n);
        for (size_t i = 0; i < n; ++i) {
            size_t len = strlen(S.edit);
            if (chars[i] < 128 && len + 1 < sizeof(S.edit) && len + 1 < capacity) { S.edit[len] = (char)chars[i]; S.edit[len + 1] = 0; }
        }
        for (unsigned remaining = r2d_key_typed_count(SAPP_KEYCODE_BACKSPACE); remaining > 0 && S.edit[0]; --remaining) S.edit[strlen(S.edit) - 1] = 0;
        if ((r2d_modifiers() & (SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SUPER)) && r2d_key_pressed(SAPP_KEYCODE_A)) S.edit[0] = 0;   /* clear */
        if (r2d_key_pressed(SAPP_KEYCODE_ENTER) || r2d_key_pressed(SAPP_KEYCODE_KP_ENTER) || r2d_key_pressed(SAPP_KEYCODE_TAB)) {
            snprintf(buffer, capacity, "%s", S.edit);
            S.focus = NULL;
            committed = true;
        } else if (r2d_key_pressed(SAPP_KEYCODE_ESCAPE)) S.focus = NULL;
    }
    if (!visible) return committed;
    r2d_draw_rect(r, focused ? r2d_rgb(28, 30, 36) : r2d_rgb(40, 44, 52));
    r2d_draw_rect_outline(r, 1, focused ? hui_yellow : r2d_rgb(80, 86, 100));
    const char *shown = focused ? S.edit : buffer;
    char clipped[256];
    float glyph = r2d_text_width("M") > 0 ? r2d_text_width("M") : 8;
    size_t max_chars = (size_t)((r.w - 8) / glyph), len = strlen(shown);
    if (len > max_chars && max_chars > 0) snprintf(clipped, sizeof(clipped), "%s", shown + (len - max_chars));
    else snprintf(clipped, sizeof(clipped), "%s", shown);
    r2d_draw_text(r2d_v2(r.x + 4, r.y + (r.h - r2d_text_height()) * 0.5f), hui_white, clipped);
    if (focused && ((int)(S.time * 2) & 1)) {
        float cx = r.x + 4 + r2d_text_width(clipped) + 1;
        r2d_draw_line(r2d_v2(cx, r.y + 4), r2d_v2(cx, r.y + r.h - 4), 1, hui_white);
    }
    return committed;
}
