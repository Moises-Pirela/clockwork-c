/* editor_anim: the animation editor's model, on top of the editor document.
 *
 * One clip is open at a time (a project animation asset, or a new unsaved
 * one). Its tracks animate fields of the primary selected entity's
 * components: tracks are added from the entity's animatable fields, keys
 * are set at the playhead from the entity's current values, moved, retimed
 * and re-interpolated. Scrubbing and preview playback apply the clip to the
 * entity in place; the components it touches are remembered at preview
 * start and restored when preview ends, so the authored scene is never
 * changed by looking at an animation. Record mode turns inspector edits
 * into keys at the playhead. Clips are saved to animations/<name>.cwanim
 * and registered as assets, which is how an Animation component refers to
 * them at run time.
 */
#ifndef EDITOR_ANIM_H
#define EDITOR_ANIM_H

#include "editor_core.h"

typedef struct {
    cw_component_id ct;
    unsigned char value[1024];
    size_t size;
} ea_saved;

typedef struct {
    cw_anim_clip clip;
    bool open;                 /* a clip is loaded in the editor */
    uint32_t asset;            /* its asset id, 0 while unsaved */
    bool dirty;
    float playhead;            /* seconds */
    bool playing, record;
    float speed;
    /* preview */
    bool previewing;
    cw_entity_id entity;       /* the entity being previewed */
    ea_saved saved[CW_ANIM_MAX_TRACKS];
    size_t saved_count;
    int selected_track, selected_key;   /* -1 when none */
} ea_state;

void ea_init(ea_state *a);
/* --- clips --- */
bool ea_new(ea_state *a, ed_document *d, const char *name, float length);   /* unsaved until ea_save */
bool ea_open(ea_state *a, ed_document *d, uint32_t asset_id);
bool ea_save(ea_state *a, ed_document *d);                                  /* writes and registers */
bool ea_delete(ea_state *a, ed_document *d, uint32_t asset_id);
void ea_close(ea_state *a, ed_document *d);
size_t ea_list(const ed_document *d, const cw_asset **out, size_t capacity);
bool ea_set_length(ea_state *a, float length);
bool ea_set_mode(ea_state *a, cw_anim_mode mode);
/* --- tracks and keys --- */
/* The entity the clip is edited against: the primary selection (or the
 * preview entity while previewing). */
cw_entity_id ea_target(const ea_state *a, const ed_document *d);
int ea_track_add(ea_state *a, ed_document *d, cw_component_id ct, const cw_field *field);   /* index or -1 */
bool ea_track_remove(ea_state *a, size_t track);
/* Sets a key on `track` at the playhead with the target's current value. */
int ea_key_from_entity(ea_state *a, ed_document *d, size_t track);
int ea_key_set(ea_state *a, size_t track, float time, float value, cw_interp interp);
bool ea_key_remove(ea_state *a, size_t track, size_t key);
int ea_key_move(ea_state *a, size_t track, size_t key, float time);
bool ea_key_set_interp(ea_state *a, size_t track, size_t key, cw_interp interp);
/* Record mode: called by the host after an inspector edit; sets a key for
 * that field at the playhead when recording. */
bool ea_record_field(ea_state *a, ed_document *d, cw_entity_id id, cw_component_id ct, const cw_field *field);
/* Resolves a track's field on the target; NULL when it has no such component/field. */
const cw_field *ea_track_field(const ea_state *a, const ed_document *d, size_t track, cw_component_id *ct);
/* --- preview --- */
void ea_scrub(ea_state *a, ed_document *d, float time);   /* moves the playhead and applies the clip to the target */
void ea_play(ea_state *a, ed_document *d);
void ea_pause(ea_state *a);
void ea_stop(ea_state *a, ed_document *d);                /* ends the preview: the target's components are restored */
void ea_frame(ea_state *a, ed_document *d, float dt);     /* advances preview playback */

#endif
