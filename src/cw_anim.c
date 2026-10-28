#define _CRT_SECURE_NO_WARNINGS 1
#include "cw_anim.h"
#include "cw_assets.h"
#include "cw_internal.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>

static const char *const interp_names[CW_INTERP_COUNT] = {"linear", "step", "ease", "ease_in", "ease_out"};
static const char *const mode_names[CW_ANIM_MODES] = {"once", "loop", "pingpong"};
const char *cw_interp_name(cw_interp i) { return i >= 0 && i < CW_INTERP_COUNT ? interp_names[i] : "?"; }
bool cw_interp_parse(const char *text, cw_interp *out) {
    for (int i = 0; text && i < CW_INTERP_COUNT; ++i)
        if (strcmp(interp_names[i], text) == 0) { *out = (cw_interp)i; return true; }
    return false;
}
const char *cw_anim_mode_name(cw_anim_mode m) { return m >= 0 && m < CW_ANIM_MODES ? mode_names[m] : "?"; }
bool cw_anim_mode_parse(const char *text, cw_anim_mode *out) {
    for (int i = 0; text && i < CW_ANIM_MODES; ++i)
        if (strcmp(mode_names[i], text) == 0) { *out = (cw_anim_mode)i; return true; }
    return false;
}
static void copy_name(char *dst, size_t capacity, const char *src) {
    size_t n = strlen(src ? src : "");
    if (n >= capacity) n = capacity - 1;
    memcpy(dst, src ? src : "", n);
    dst[n] = 0;
}

/* --- authoring --- */
void cw_anim_clip_init(cw_anim_clip *clip, const char *name, float length) {
    memset(clip, 0, sizeof(*clip));
    copy_name(clip->name, sizeof(clip->name), name && *name ? name : "clip");
    clip->length = length > 0 ? length : 1.0f;
    clip->mode = CW_ANIM_LOOP;
}
int cw_anim_track_find(const cw_anim_clip *clip, const char *component, const char *field) {
    for (size_t i = 0; clip && component && field && i < clip->track_count; ++i)
        if (strcmp(clip->tracks[i].component, component) == 0 && strcmp(clip->tracks[i].field, field) == 0) return (int)i;
    return -1;
}
int cw_anim_track_add(cw_anim_clip *clip, const char *component, const char *field) {
    if (!clip || !component || !*component || !field || !*field) return -1;
    int at = cw_anim_track_find(clip, component, field);
    if (at >= 0) return at;
    if (clip->track_count == CW_ANIM_MAX_TRACKS) return -1;
    cw_anim_track *t = &clip->tracks[clip->track_count];
    memset(t, 0, sizeof(*t));
    copy_name(t->component, sizeof(t->component), component);
    copy_name(t->field, sizeof(t->field), field);
    return (int)clip->track_count++;
}
bool cw_anim_track_remove(cw_anim_clip *clip, size_t index) {
    if (!clip || index >= clip->track_count) return false;
    memmove(clip->tracks + index, clip->tracks + index + 1, (clip->track_count - index - 1) * sizeof(*clip->tracks));
    --clip->track_count;
    return true;
}
int cw_anim_key_find(const cw_anim_track *track, float time) {
    for (size_t i = 0; track && i < track->key_count; ++i)
        if (fabsf(track->keys[i].time - time) < 0.0005f) return (int)i;
    return -1;
}
int cw_anim_key_set(cw_anim_track *track, float time, float value, cw_interp interp) {
    if (!track || !isfinite(time) || !isfinite(value) || time < 0) return -1;
    int at = cw_anim_key_find(track, time);
    if (at >= 0) {
        track->keys[at].value = value;
        track->keys[at].interp = interp;
        return at;
    }
    if (track->key_count == CW_ANIM_MAX_KEYS) return -1;
    size_t i = 0;
    while (i < track->key_count && track->keys[i].time < time) ++i;
    memmove(track->keys + i + 1, track->keys + i, (track->key_count - i) * sizeof(*track->keys));
    track->keys[i].time = time;
    track->keys[i].value = value;
    track->keys[i].interp = interp;
    ++track->key_count;
    return (int)i;
}
bool cw_anim_key_remove(cw_anim_track *track, size_t index) {
    if (!track || index >= track->key_count) return false;
    memmove(track->keys + index, track->keys + index + 1, (track->key_count - index - 1) * sizeof(*track->keys));
    --track->key_count;
    return true;
}
int cw_anim_key_move(cw_anim_track *track, size_t index, float time) {
    if (!track || index >= track->key_count || !isfinite(time)) return -1;
    if (time < 0) time = 0;
    cw_anim_key key = track->keys[index];
    int clash = cw_anim_key_find(track, time);
    if (clash >= 0 && (size_t)clash != index) return -1;   /* another key sits there */
    cw_anim_key_remove(track, index);
    return cw_anim_key_set(track, time, key.value, key.interp);
}

/* --- evaluation --- */
float cw_anim_ease(cw_interp interp, float t) {
    if (t <= 0) return interp == CW_INTERP_STEP ? 0 : 0;
    if (t >= 1) return 1;
    switch (interp) {
        case CW_INTERP_STEP: return 0;
        case CW_INTERP_EASE: return t * t * (3 - 2 * t);
        case CW_INTERP_EASE_IN: return t * t;
        case CW_INTERP_EASE_OUT: return 1 - (1 - t) * (1 - t);
        default: return t;
    }
}
float cw_anim_track_eval(const cw_anim_track *track, float time) {
    if (!track || !track->key_count) return 0;
    if (time <= track->keys[0].time) return track->keys[0].value;
    size_t last = track->key_count - 1;
    if (time >= track->keys[last].time) return track->keys[last].value;
    size_t i = 0;
    while (i + 1 < track->key_count && track->keys[i + 1].time <= time) ++i;
    const cw_anim_key *a = &track->keys[i], *b = &track->keys[i + 1];
    float span = b->time - a->time;
    float t = span > 0 ? (time - a->time) / span : 1;
    return a->value + (b->value - a->value) * cw_anim_ease(a->interp, t);
}
float cw_anim_wrap(const cw_anim_clip *clip, float time, bool *finished) {
    if (finished) *finished = false;
    if (!clip || clip->length <= 0) return 0;
    float len = clip->length;
    switch (clip->mode) {
        case CW_ANIM_LOOP: {
            float t = fmodf(time, len);
            return t < 0 ? t + len : t;
        }
        case CW_ANIM_PINGPONG: {
            float cycle = fmodf(fabsf(time), 2 * len);
            return cycle <= len ? cycle : 2 * len - cycle;
        }
        default:
            if (time >= len) { if (finished) *finished = true; return len; }
            return time < 0 ? 0 : time;
    }
}
bool cw_anim_field_animatable(const cw_field *f) {
    if (!f) return false;
    switch (f->type) {
        case CW_FIELD_F32: case CW_FIELD_I32: case CW_FIELD_U8: case CW_FIELD_BOOL: case CW_FIELD_ENUM: case CW_FIELD_FLAGS: return f->semantic != CW_SEM_ASSET;
        case CW_FIELD_U32: return f->semantic != CW_SEM_ASSET;
        default: return false;
    }
}
size_t cw_anim_apply(const cw_anim_clip *clip, cw_world *w, cw_entity_id id, float time) {
    if (!clip || !cw_world_is_valid(w, id)) return 0;
    size_t applied = 0;
    for (size_t i = 0; i < clip->track_count; ++i) {
        const cw_anim_track *t = &clip->tracks[i];
        if (!t->key_count) continue;
        cw_component_id ct;
        if (!cw_comp_by_name(w, t->component, &ct) || !cw_comp_has(w, id, ct)) continue;
        const cw_type_info *type = cw_comp_type(w, ct);
        const cw_field *f = cw_type_field(type, t->field);
        if (!f || !cw_anim_field_animatable(f)) continue;
        float v = cw_anim_track_eval(t, time);
        if (f->type != CW_FIELD_F32) v = roundf(v);   /* discrete fields step between keys */
        if (cw_field_set(f, cw_comp_ptr(w, id, ct), v)) { cw_comp_touch(w, id, ct); ++applied; }
    }
    return applied;
}

/* --- text --- */
static void text(cw_writer *out, const char *s) { cw_writer_bytes(out, s, strlen(s)); }
bool cw_anim_write(const cw_anim_clip *clip, cw_writer *out) {
    if (!clip || !out) return false;
    char line[160];
    snprintf(line, sizeof(line), "cwanim %d\nname %s\nlength %.9g\nmode %s\n", CW_ANIM_FORMAT_VERSION, clip->name, (double)clip->length, cw_anim_mode_name(clip->mode));
    text(out, line);
    for (size_t i = 0; i < clip->track_count; ++i) {
        const cw_anim_track *t = &clip->tracks[i];
        snprintf(line, sizeof(line), "track %s %s\n", t->component, t->field);
        text(out, line);
        for (size_t k = 0; k < t->key_count; ++k) {
            snprintf(line, sizeof(line), "  key %.9g %.9g %s\n", (double)t->keys[k].time, (double)t->keys[k].value, cw_interp_name(t->keys[k].interp));
            text(out, line);
        }
    }
    return cw_writer_ok(out);
}
static void set_error(char *error, size_t capacity, int line, const char *message) {
    if (error && capacity) snprintf(error, capacity, "line %d: %s", line, message);
}
bool cw_anim_parse(cw_anim_clip *clip, const char *data, size_t size, char *error, size_t error_capacity) {
    if (!clip || (!data && size)) { set_error(error, error_capacity, 0, "no clip or text"); return false; }
    cw_anim_clip_init(clip, "clip", 1);
    const char *p = data, *end = data + size;
    bool header = false;
    int line_no = 0;
    cw_anim_track *track = NULL;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        ++line_no;
        char line[256];
        if (len >= sizeof(line)) { set_error(error, error_capacity, line_no, "line too long"); return false; }
        memcpy(line, p, len);
        line[len] = 0;
        p += len + (nl ? 1 : 0);
        char *s = line;
        while (*s && isspace((unsigned char)*s)) ++s;
        char *e = s + strlen(s);
        while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
        if (!*s || *s == '#') continue;
        char a[CW_ANIM_NAME_CAPACITY] = "", b[CW_ANIM_NAME_CAPACITY] = "", c[CW_ANIM_NAME_CAPACITY] = "";
        char key[16];
        int n = sscanf(s, "%15s %63s %63s %63s", key, a, b, c);
        if (n < 1) continue;
        if (!header) {
            if (strcmp(key, "cwanim") != 0 || strcmp(a, "1") != 0) { set_error(error, error_capacity, line_no, "not a clip (expected 'cwanim 1')"); return false; }
            header = true;
        } else if (strcmp(key, "name") == 0) { const char *v = s + 4; while (*v && isspace((unsigned char)*v)) ++v; copy_name(clip->name, sizeof(clip->name), v); }
        else if (strcmp(key, "length") == 0) {
            char *stop;
            double v = strtod(a, &stop);
            if (stop == a || *stop || !(v > 0) || v > 3600) { set_error(error, error_capacity, line_no, "bad length"); return false; }
            clip->length = (float)v;
        } else if (strcmp(key, "mode") == 0) {
            if (!cw_anim_mode_parse(a, &clip->mode)) { set_error(error, error_capacity, line_no, "bad mode"); return false; }
        } else if (strcmp(key, "track") == 0) {
            if (n < 3) { set_error(error, error_capacity, line_no, "track needs a component and a field"); return false; }
            int at = cw_anim_track_add(clip, a, b);
            if (at < 0) { set_error(error, error_capacity, line_no, "too many tracks"); return false; }
            track = &clip->tracks[at];
        } else if (strcmp(key, "key") == 0) {
            if (!track) { set_error(error, error_capacity, line_no, "key before any track"); return false; }
            char *stop;
            double t = strtod(a, &stop);
            if (stop == a || *stop || !isfinite(t) || t < 0) { set_error(error, error_capacity, line_no, "bad key time"); return false; }
            double v = strtod(b, &stop);
            if (stop == b || *stop || !isfinite(v)) { set_error(error, error_capacity, line_no, "bad key value"); return false; }
            cw_interp interp = CW_INTERP_LINEAR;
            if (n >= 4 && !cw_interp_parse(c, &interp)) { set_error(error, error_capacity, line_no, "bad interpolation"); return false; }
            if (cw_anim_key_set(track, (float)t, (float)v, interp) < 0) { set_error(error, error_capacity, line_no, "too many keys"); return false; }
        } else { set_error(error, error_capacity, line_no, "unknown directive"); return false; }
    }
    if (!header) { set_error(error, error_capacity, 0, "empty file"); return false; }
    return true;
}
bool cw_anim_save(const cw_anim_clip *clip, const char *path, char *error, size_t error_capacity) {
    cw_writer out;
    cw_writer_init(&out);
    cw_anim_write(clip, &out);
    cw_file_error ferr;
    bool ok = cw_file_write(path, out.data, out.size, &ferr);
    cw_writer_destroy(&out);
    if (!ok && error && error_capacity) snprintf(error, error_capacity, "cannot write %s: %s", path, cw_file_status_string(ferr.status));
    return ok;
}
bool cw_anim_load(cw_anim_clip *clip, const char *path, char *error, size_t error_capacity) {
    cw_buffer data;
    cw_file_error ferr;
    if (!cw_file_read(path, 16 << 20, &data, &ferr)) {
        if (error && error_capacity) snprintf(error, error_capacity, "cannot read %s: %s", path, cw_file_status_string(ferr.status));
        return false;
    }
    bool ok = cw_anim_parse(clip, (const char *)data.data, data.size, error, error_capacity);
    cw_buffer_free(&data);
    return ok;
}

/* --- runtime --- */
const char *const cw_anim_mode_override_names[4] = {"clip", "once", "loop", "pingpong"};
static const cw_field animation_fields[] = {
    CW_FIELD_SEM(cw_animation, clip, CW_FIELD_U32, CW_SEM_ASSET),
    CW_FIELD_RANGE(cw_animation, speed, CW_FIELD_F32, -10, 10, 0.1f),
    CW_FIELD(cw_animation, playing, CW_FIELD_BOOL),
    CW_FIELD_NAMED(cw_animation, mode, CW_FIELD_ENUM, cw_anim_mode_override_names, 4),
    CW_FIELD_RANGE_SEM(cw_animation, time, CW_FIELD_F32, 0, 3600, 0.1f, CW_SEM_RUNTIME),
};
static const cw_animation animation_defaults = {0, 1.0f, true, 0, 0};
const cw_type_info cw_animation_type = {
    "Animation", sizeof(cw_animation), _Alignof(cw_animation), animation_fields, 5, &animation_defaults, NULL, 0, NULL, NULL
};

void cw_anim_cache_init(cw_anim_cache *cache) { memset(cache, 0, sizeof(*cache)); }
void cw_anim_cache_destroy(cw_anim_cache *cache) { free(cache->items); memset(cache, 0, sizeof(*cache)); }
void cw_anim_cache_clear(cw_anim_cache *cache) { cache->count = 0; }
static cw_anim_entry *cache_find(cw_anim_cache *cache, uint32_t id) {
    for (size_t i = 0; i < cache->count; ++i) if (cache->items[i].id == id) return &cache->items[i];
    return NULL;
}
static cw_anim_entry *cache_push(cw_anim_cache *cache, uint32_t id) {
    if (cache->count == cache->capacity) {
        cache->capacity = cache->capacity ? cache->capacity * 2 : 8;
        cache->items = cw_resize(cache->items, cache->capacity, sizeof(*cache->items));
    }
    cw_anim_entry *e = &cache->items[cache->count++];
    memset(e, 0, sizeof(*e));
    e->id = id;
    return e;
}
void cw_anim_cache_put(cw_anim_cache *cache, uint32_t id, const cw_anim_clip *clip) {
    if (!cache || !id || !clip) return;
    cw_anim_entry *e = cache_find(cache, id);
    if (!e) e = cache_push(cache, id);
    e->clip = *clip;
    e->missing = false;
}
const cw_anim_clip *cw_anim_cache_get(cw_anim_cache *cache, const cw_assets *assets, uint32_t id) {
    if (!cache || !id) return NULL;
    cw_anim_entry *e = cache_find(cache, id);
    if (e) return e->missing ? NULL : &e->clip;
    e = cache_push(cache, id);
    char path[CW_ASSET_PATH_CAPACITY];
    if (!cw_assets_path(assets, id, path, sizeof(path)) || !cw_anim_load(&e->clip, path, NULL, 0)) { e->missing = true; return NULL; }
    return &e->clip;
}
bool cw_anim_step(cw_world *w, cw_entity_id id, cw_component_id ct, cw_anim_cache *cache, const cw_assets *assets, float dt) {
    cw_animation *a = cw_comp_has(w, id, ct) ? cw_comp_ptr(w, id, ct) : NULL;
    if (!a || !a->clip) return false;
    const cw_anim_clip *clip = cw_anim_cache_get(cache, assets, a->clip);
    if (!clip) return false;
    cw_anim_clip view = *clip;
    if (a->mode >= 1 && a->mode <= 3) view.mode = (cw_anim_mode)(a->mode - 1);
    if (a->playing) a->time += dt * a->speed;
    bool finished = false;
    float t = cw_anim_wrap(&view, a->time, &finished);
    if (finished) { a->playing = false; a->time = view.length; }
    if (view.mode == CW_ANIM_LOOP && a->time >= view.length) a->time = t;   /* keep the number bounded */
    cw_anim_apply(&view, w, id, t);
    return true;
}
