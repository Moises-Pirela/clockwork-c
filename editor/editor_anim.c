#ifndef _WIN32
#define _DEFAULT_SOURCE 1
#endif
#define _CRT_SECURE_NO_WARNINGS 1
#include "editor_anim.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir(p, 0755)
#endif

void ea_init(ea_state *a) {
    memset(a, 0, sizeof(*a));
    a->speed = 1;
    a->entity = CW_INVALID_ENTITY;
    a->selected_track = a->selected_key = -1;
}

/* --- preview bookkeeping --- */
static void preview_begin(ea_state *a, ed_document *d, cw_entity_id id) {
    if (a->previewing || !d->world_ready || !cw_world_is_valid(&d->gw.world, id)) return;
    cw_world *w = &d->gw.world;
    a->saved_count = 0;
    for (size_t i = 0; i < a->clip.track_count && a->saved_count < CW_ANIM_MAX_TRACKS; ++i) {
        cw_component_id ct;
        if (!cw_comp_by_name(w, a->clip.tracks[i].component, &ct) || !cw_comp_has(w, id, ct)) continue;
        bool known = false;
        for (size_t k = 0; k < a->saved_count; ++k) if (a->saved[k].ct == ct) known = true;
        if (known) continue;
        size_t size = w->pools[ct].elem_size;
        if (size > sizeof(a->saved[0].value)) continue;
        ea_saved *s = &a->saved[a->saved_count++];
        s->ct = ct;
        s->size = size;
        memcpy(s->value, cw_comp_ptr(w, id, ct), size);
    }
    a->previewing = true;
    a->entity = id;
}
static void preview_end(ea_state *a, ed_document *d) {
    if (!a->previewing) return;
    cw_world *w = &d->gw.world;
    if (d->world_ready && cw_world_is_valid(w, a->entity))
        for (size_t k = 0; k < a->saved_count; ++k)
            if (cw_comp_has(w, a->entity, a->saved[k].ct)) cw_comp_set(w, a->entity, a->saved[k].ct, a->saved[k].value, a->saved[k].size);
    a->previewing = false;
    a->playing = false;
    a->saved_count = 0;
    a->entity = CW_INVALID_ENTITY;
}
/* a track added while previewing must be restorable too */
static void preview_track(ea_state *a, ed_document *d, const char *component) {
    if (!a->previewing) return;
    cw_world *w = &d->gw.world;
    cw_component_id ct;
    if (!cw_comp_by_name(w, component, &ct) || !cw_comp_has(w, a->entity, ct)) return;
    for (size_t k = 0; k < a->saved_count; ++k) if (a->saved[k].ct == ct) return;
    if (a->saved_count == CW_ANIM_MAX_TRACKS || w->pools[ct].elem_size > sizeof(a->saved[0].value)) return;
    ea_saved *s = &a->saved[a->saved_count++];
    s->ct = ct;
    s->size = w->pools[ct].elem_size;
    memcpy(s->value, cw_comp_ptr(w, a->entity, ct), s->size);
}
static void apply_now(ea_state *a, ed_document *d) {
    if (!a->previewing || !d->world_ready) return;
    cw_anim_apply(&a->clip, &d->gw.world, a->entity, a->playhead);
}

/* --- clips --- */
size_t ea_list(const ed_document *d, const cw_asset **out, size_t capacity) {
    size_t n = 0;
    for (size_t i = 0; d->project_open && i < d->assets.count && n < capacity; ++i)
        if (d->assets.items[i].type == CW_ASSET_ANIMATION && !d->assets.items[i].missing) out[n++] = &d->assets.items[i];
    return n;
}
bool ea_new(ea_state *a, ed_document *d, const char *name, float length) {
    if (!d->world_ready) { ed_status(d, true, "open a project first"); return false; }
    if (!name || !*name || strpbrk(name, "/\\\"\r\n ")) { ed_status(d, true, "give the clip a file name (no spaces, slashes or quotes)"); return false; }
    ea_stop(a, d);
    cw_anim_clip_init(&a->clip, name, length);
    a->open = true;
    a->asset = 0;
    a->dirty = true;
    a->playhead = 0;
    a->selected_track = a->selected_key = -1;
    ed_status(d, false, "new clip %s (%.2fs): add tracks from the target entity's fields, then save", name, (double)a->clip.length);
    return true;
}
bool ea_open(ea_state *a, ed_document *d, uint32_t asset_id) {
    const cw_asset *asset = cw_assets_find(&d->assets, asset_id);
    if (!asset || asset->type != CW_ASSET_ANIMATION) { ed_status(d, true, "no such animation"); return false; }
    char path[CW_GAME_PATH_CAPACITY], error[256];
    cw_assets_resolve(&d->assets, asset->path, path, sizeof(path));
    cw_anim_clip clip;
    if (!cw_anim_load(&clip, path, error, sizeof(error))) { ed_status(d, true, "%s: %s", asset->name, error); return false; }
    ea_stop(a, d);
    a->clip = clip;
    a->open = true;
    a->asset = asset_id;
    a->dirty = false;
    a->playhead = 0;
    a->selected_track = a->selected_key = -1;
    ed_status(d, false, "opened clip %s: %zu track%s, %.2fs, %s", a->clip.name, a->clip.track_count, a->clip.track_count == 1 ? "" : "s",
              (double)a->clip.length, cw_anim_mode_name(a->clip.mode));
    return true;
}
bool ea_save(ea_state *a, ed_document *d) {
    if (!a->open || !d->project_open) { ed_status(d, true, "no clip to save"); return false; }
    char rel[CW_GAME_PATH_CAPACITY], full[CW_GAME_PATH_CAPACITY], dir[CW_GAME_PATH_CAPACITY], error[256];
    snprintf(rel, sizeof(rel), "animations/%s.cwanim", a->clip.name);
    cw_assets_resolve(&d->assets, "animations", dir, sizeof(dir));
    make_dir(dir);
    cw_assets_resolve(&d->assets, rel, full, sizeof(full));
    if (!cw_anim_save(&a->clip, full, error, sizeof(error))) { ed_status(d, true, "%s", error); return false; }
    const cw_asset *asset = cw_assets_find_path(&d->assets, rel);
    if (asset) cw_assets_refresh(&d->assets);
    else asset = cw_assets_add(&d->assets, rel, NULL, error, sizeof(error));
    if (!asset) { ed_status(d, true, "clip written but not registered: %s", error); return false; }
    ed_save_assets(d);
    a->asset = asset->id;
    a->dirty = false;
    cw_game_world_reload_animations(&d->gw);   /* running Animation components pick up the new version */
    ed_status(d, false, "clip %s saved (%zu tracks)", asset->name, a->clip.track_count);
    return true;
}
bool ea_delete(ea_state *a, ed_document *d, uint32_t asset_id) {
    const cw_asset *asset = cw_assets_find(&d->assets, asset_id);
    if (!asset || asset->type != CW_ASSET_ANIMATION) return false;
    if (a->open && a->asset == asset_id) ea_close(a, d);
    cw_game_world_reload_animations(&d->gw);
    return ed_remove_asset(d, asset_id, true);
}
void ea_close(ea_state *a, ed_document *d) {
    ea_stop(a, d);
    a->open = false;
    a->asset = 0;
    a->dirty = false;
    a->selected_track = a->selected_key = -1;
}
bool ea_set_length(ea_state *a, float length) {
    if (!a->open || !(length > 0) || length > 3600) return false;
    a->clip.length = length;
    if (a->playhead > length) a->playhead = length;
    a->dirty = true;
    return true;
}
bool ea_set_mode(ea_state *a, cw_anim_mode mode) {
    if (!a->open || mode < 0 || mode >= CW_ANIM_MODES) return false;
    a->clip.mode = mode;
    a->dirty = true;
    return true;
}

/* --- tracks and keys --- */
cw_entity_id ea_target(const ea_state *a, const ed_document *d) {
    if (a->previewing) return a->entity;
    return ed_primary(d);
}
const cw_field *ea_track_field(const ea_state *a, const ed_document *d, size_t track, cw_component_id *ct_out) {
    if (!a->open || track >= a->clip.track_count || !d->world_ready) return NULL;
    cw_entity_id id = ea_target(a, d);
    cw_component_id ct;
    const cw_anim_track *t = &a->clip.tracks[track];
    if (!cw_comp_by_name(&d->gw.world, t->component, &ct) || !cw_world_is_valid(&d->gw.world, id) || !cw_comp_has(&d->gw.world, id, ct)) return NULL;
    const cw_field *f = cw_type_field(cw_comp_type(&d->gw.world, ct), t->field);
    if (f && ct_out) *ct_out = ct;
    return f;
}
int ea_track_add(ea_state *a, ed_document *d, cw_component_id ct, const cw_field *field) {
    if (!a->open) { ed_status(d, true, "open or create a clip first"); return -1; }
    const cw_type_info *t = d->world_ready ? cw_comp_type(&d->gw.world, ct) : NULL;
    if (!t || !field || !cw_anim_field_animatable(field)) { ed_status(d, true, "that field cannot be animated"); return -1; }
    int at = cw_anim_track_add(&a->clip, t->name, field->name);
    if (at < 0) { ed_status(d, true, "the clip is full (%d tracks)", CW_ANIM_MAX_TRACKS); return -1; }
    preview_track(a, d, t->name);
    a->dirty = true;
    a->selected_track = at;
    a->selected_key = -1;
    ed_status(d, false, "track %s.%s", t->name, field->name);
    return at;
}
bool ea_track_remove(ea_state *a, size_t track) {
    if (!a->open || !cw_anim_track_remove(&a->clip, track)) return false;
    a->dirty = true;
    a->selected_track = a->selected_key = -1;
    return true;
}
int ea_key_set(ea_state *a, size_t track, float time, float value, cw_interp interp) {
    if (!a->open || track >= a->clip.track_count) return -1;
    int at = cw_anim_key_set(&a->clip.tracks[track], time, value, interp);
    if (at >= 0) { a->dirty = true; a->selected_track = (int)track; a->selected_key = at; }
    return at;
}
int ea_key_from_entity(ea_state *a, ed_document *d, size_t track) {
    cw_component_id ct;
    const cw_field *f = ea_track_field(a, d, track, &ct);
    if (!f) { ed_status(d, true, "the target entity has no %s.%s", a->open && track < a->clip.track_count ? a->clip.tracks[track].component : "?", a->open && track < a->clip.track_count ? a->clip.tracks[track].field : "?"); return -1; }
    float value = (float)cw_field_get(f, cw_comp_ptr(&d->gw.world, ea_target(a, d), ct));
    int at = ea_key_set(a, track, a->playhead, value, CW_INTERP_LINEAR);
    if (at >= 0) ed_status(d, false, "key %s.%s = %g at %.2fs", a->clip.tracks[track].component, a->clip.tracks[track].field, (double)value, (double)a->playhead);
    return at;
}
bool ea_key_remove(ea_state *a, size_t track, size_t key) {
    if (!a->open || track >= a->clip.track_count || !cw_anim_key_remove(&a->clip.tracks[track], key)) return false;
    a->dirty = true;
    a->selected_key = -1;
    return true;
}
int ea_key_move(ea_state *a, size_t track, size_t key, float time) {
    if (!a->open || track >= a->clip.track_count) return -1;
    int at = cw_anim_key_move(&a->clip.tracks[track], key, time);
    if (at >= 0) { a->dirty = true; a->selected_key = at; }
    return at;
}
bool ea_key_set_interp(ea_state *a, size_t track, size_t key, cw_interp interp) {
    if (!a->open || track >= a->clip.track_count || key >= a->clip.tracks[track].key_count) return false;
    a->clip.tracks[track].keys[key].interp = interp;
    a->dirty = true;
    return true;
}
bool ea_record_field(ea_state *a, ed_document *d, cw_entity_id id, cw_component_id ct, const cw_field *field) {
    if (!a->open || !a->record || !field || !cw_anim_field_animatable(field)) return false;
    if (id != ea_target(a, d)) return false;
    const cw_type_info *t = cw_comp_type(&d->gw.world, ct);
    if (!t || !cw_comp_has(&d->gw.world, id, ct)) return false;
    if (!a->previewing) preview_begin(a, d, id);
    int track = cw_anim_track_add(&a->clip, t->name, field->name);
    if (track < 0) return false;
    preview_track(a, d, t->name);
    float value = (float)cw_field_get(field, cw_comp_ptr(&d->gw.world, id, ct));
    int at = cw_anim_key_set(&a->clip.tracks[track], a->playhead, value, CW_INTERP_LINEAR);
    if (at < 0) return false;
    a->dirty = true;
    a->selected_track = track;
    a->selected_key = at;
    ed_status(d, false, "recorded %s.%s = %g at %.2fs", t->name, field->name, (double)value, (double)a->playhead);
    return true;
}

/* --- preview --- */
void ea_scrub(ea_state *a, ed_document *d, float time) {
    if (!a->open) return;
    if (time < 0) time = 0;
    if (time > a->clip.length) time = a->clip.length;
    a->playhead = time;
    if (!a->previewing) {
        cw_entity_id id = ed_primary(d);
        if (id == CW_INVALID_ENTITY) return;
        preview_begin(a, d, id);
    }
    apply_now(a, d);
}
void ea_play(ea_state *a, ed_document *d) {
    if (!a->open) return;
    if (!a->previewing) {
        cw_entity_id id = ed_primary(d);
        if (id == CW_INVALID_ENTITY) { ed_status(d, true, "select an entity to preview the clip on"); return; }
        preview_begin(a, d, id);
    }
    a->playing = true;
    apply_now(a, d);
}
void ea_pause(ea_state *a) { a->playing = false; }
void ea_stop(ea_state *a, ed_document *d) {
    a->playing = false;
    preview_end(a, d);
}
void ea_frame(ea_state *a, ed_document *d, float dt) {
    if (!a->open || !a->playing || !a->previewing) return;
    if (!d->world_ready || !cw_world_is_valid(&d->gw.world, a->entity)) { ea_stop(a, d); return; }
    float t = a->playhead + dt * a->speed;
    bool finished = false;
    a->playhead = cw_anim_wrap(&a->clip, t, &finished);
    if (finished) a->playing = false;
    apply_now(a, d);
}
