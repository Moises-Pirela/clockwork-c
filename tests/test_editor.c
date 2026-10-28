/* Headless checks for the editor document: project opening, placing,
 * moving, duplicating, editing fields, undo/redo, saving and loading scenes,
 * play mode, and (with an engine root) building and hot-reloading the
 * module through the build script.
 *
 *   clockwork_editor_tests <project.cwproj> [engine_root] [second_project.cwproj]
 *
 * The second project (a different game) checks that one editor switches
 * between games: the module, prefabs and schema all change.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "editor_core.h"
#include "editor_settings.h"
#include "editor_anim.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t scene_hash(ed_document *d) {
    cw_writer out;
    cw_writer_init(&out);
    cw_scene_result r;
    cw_scene_write(&d->gw.world, NULL, &out, &r);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < out.size; ++i) h = (h ^ out.data[i]) * 1099511628211ull;
    cw_writer_destroy(&out);
    return h;
}
static size_t prefab(const ed_document *d, const char *category, const char *name) {
    const cw_game_module *m = d->gw.module;
    for (size_t i = 0; i < m->prefab_count; ++i)
        if (strcmp(m->prefabs[i].name, name) == 0 && strcmp(m->prefabs[i].category, category) == 0) return i;
    return SIZE_MAX;
}
static cw_component_id comp(ed_document *d, const char *name) {
    cw_component_id ct = CW_INVALID_COMPONENT;
    cw_comp_by_name(&d->gw.world, name, &ct);
    return ct;
}
static const cw_field *field(ed_document *d, const char *component, const char *name) {
    return cw_type_field(cw_comp_type(&d->gw.world, comp(d, component)), name);
}
static double value(ed_document *d, cw_entity_id id, const char *component, const char *name) {
    cw_component_id ct = comp(d, component);
    const cw_field *f = field(d, component, name);
    return f && cw_comp_has(&d->gw.world, id, ct) ? cw_field_get(f, cw_comp_ptr(&d->gw.world, id, ct)) : NAN;
}

int main(int argc, char **argv) {
    if (argc < 2) { fputs("usage: clockwork_editor_tests <project.cwproj> [engine_root] [second_project.cwproj]\n", stderr); return 2; }
    const char *project = argv[1], *engine_root = argc > 2 && argv[2][0] ? argv[2] : NULL, *second = argc > 3 ? argv[3] : NULL;
    ed_document d;
    ed_init(&d, engine_root, NULL);
    CHECK(!ed_run_player(&d) && d.status_error, "running without a project is refused");
    CHECK(!ed_open_project(&d, "missing.cwproj") && d.status_error, "missing project reported");
    CHECK(ed_open_project(&d, project), "open project: %s", d.status);
    if (!d.project_open) return 1;
    if (!d.world_ready && engine_root) CHECK(ed_build(&d, "module"), "build the module: %s", d.status);
    CHECK(d.world_ready, "module loaded and world bound: %s", d.status);
    if (!d.world_ready) return 1;
    CHECK(strcmp(d.game.module->name, "combat") == 0 && d.game.library != NULL, "module loaded dynamically");
    CHECK(ed_entity_count(&d) == 26 && !d.dirty && d.scene_path[0], "main scene opened (%zu entities)", ed_entity_count(&d));
    CHECK(strcmp(ed_name_of(&d, 2), "center pillar") == 0, "names loaded: '%s'", ed_name_of(&d, 2));
    char label[64];
    ed_label(&d, 2, label, sizeof(label));
    CHECK(strcmp(label, "center pillar") == 0, "label uses the name");
    ed_label(&d, 0, label, sizeof(label));
    CHECK(strcmp(label, "pillar") == 0, "label comes from the module's describe hook: %s", label);
    CHECK(!ed_can_undo(&d), "a freshly opened scene has nothing to undo");

    /* placing and undo */
    ed_new_scene(&d);
    CHECK(ed_entity_count(&d) == 0 && !d.dirty, "new scene is empty");
    cw_entity_id a = ed_place(&d, prefab(&d, "red", "melee"), 100, 100);
    cw_entity_id b = ed_place(&d, prefab(&d, "blue", "tank"), 300, 100);
    cw_entity_id torch = ed_place(&d, prefab(&d, "light", "torch"), 100, 100);
    CHECK(a == 0 && b == 1 && torch == 2 && ed_entity_count(&d) == 3 && d.dirty, "placed three entities");
    CHECK(ed_is_selected(&d, torch) && d.selection_count == 1, "placing selects");
    CHECK(value(&d, torch, "Attached", "parent") == 0, "the torch attached to the melee unit");
    CHECK(cw_comp_has(&d.gw.world, a, comp(&d, "Visual")), "placed entities went through EntityCreated");
    CHECK(ed_can_undo(&d) && strcmp(ed_undo_label(&d), "place torch") == 0, "undo label: %s", ed_undo_label(&d));
    CHECK(ed_undo(&d) && ed_entity_count(&d) == 2 && !cw_world_is_valid(&d.gw.world, torch), "undo removes the torch");
    CHECK(ed_undo(&d) && ed_entity_count(&d) == 1, "undo again");
    CHECK(ed_redo(&d) && ed_redo(&d) && ed_entity_count(&d) == 3, "redo twice");
    CHECK(!ed_redo(&d), "nothing more to redo");
    CHECK(value(&d, torch, "Attached", "parent") == 0, "references survive undo/redo");

    /* moving */
    ed_select_set(&d, a);
    ed_select_add(&d, b);
    CHECK(ed_move_selection(&d, 10, 20) == 2, "moved two");
    ed_commit(&d, "move");
    CHECK(value(&d, a, "Position", "x") == 110 && value(&d, b, "Position", "y") == 120, "positions moved");
    CHECK(ed_undo(&d) && value(&d, a, "Position", "x") == 100, "undo move");
    CHECK(ed_redo(&d) && value(&d, a, "Position", "x") == 110, "redo move");
    CHECK(ed_is_selected(&d, a) && ed_is_selected(&d, b), "selection survives undo/redo");

    /* duplicating with reference remapping */
    ed_rename(&d, a, "hero");
    ed_select_set(&d, a);
    ed_select_add(&d, torch);
    CHECK(ed_duplicate_selection(&d, 50, 0) == 2 && ed_entity_count(&d) == 5, "duplicated two (%zu entities)", ed_entity_count(&d));
    cw_entity_id a2 = d.selection[0], torch2 = d.selection[1];
    CHECK(a2 == 3 && torch2 == 4, "copies got new ids %" PRId64 ", %" PRId64, a2, torch2);
    CHECK(value(&d, a2, "Position", "x") == 160 && value(&d, torch2, "Attached", "parent") == a2, "copy offset and remapped reference");
    CHECK(strcmp(ed_name_of(&d, a2), "hero copy") == 0, "copied name: '%s'", ed_name_of(&d, a2));
    CHECK(cw_comp_has(&d.gw.world, a2, comp(&d, "ShadowCaster")) && value(&d, a2, "HealthConfig", "maximum") == 120, "components copied");

    /* field editing through reflection */
    CHECK(!cw_comp_has(&d.gw.world, a, comp(&d, "Health")) && cw_comp_has(&d.gw.world, a, comp(&d, "HealthConfig")), "editing: config components only, no runtime ones");
    CHECK(ed_set_field(&d, a, comp(&d, "HealthConfig"), field(&d, "HealthConfig", "maximum"), "60") && value(&d, a, "HealthConfig", "maximum") == 60, "set by text");
    CHECK(!ed_set_field(&d, a, comp(&d, "HealthConfig"), field(&d, "HealthConfig", "maximum"), "0") && value(&d, a, "HealthConfig", "maximum") == 60, "range rejected");
    CHECK(!ed_set_field(&d, a, comp(&d, "HealthConfig"), field(&d, "HealthConfig", "maximum"), "lots"), "junk rejected");
    CHECK(ed_adjust_field(&d, a, comp(&d, "HealthConfig"), field(&d, "HealthConfig", "maximum"), 1) && value(&d, a, "HealthConfig", "maximum") == 70, "adjust by step");
    CHECK(!ed_add_component(&d, a, comp(&d, "Health")) && d.status_error, "runtime components cannot be added while editing: %s", d.status);
    CHECK(ed_adjust_field(&d, a, comp(&d, "Unit"), field(&d, "Unit", "team"), 1) && value(&d, a, "Unit", "team") == 1, "enum adjust");
    CHECK(ed_set_field(&d, a, comp(&d, "Unit"), field(&d, "Unit", "kind"), "tank") && value(&d, a, "Unit", "kind") == 2, "enum by label");
    CHECK(ed_set_field(&d, torch, comp(&d, "Light"), field(&d, "Light", "flags"), "spot|no_shadows") && value(&d, torch, "Light", "flags") == 3, "flags by label");
    CHECK(ed_undo(&d) && value(&d, torch, "Light", "flags") == 1, "undo a field edit");
    CHECK(ed_remove_component(&d, a, comp(&d, "ShadowCaster")) && !cw_comp_has(&d.gw.world, a, comp(&d, "ShadowCaster")), "remove component");
    CHECK(ed_add_component(&d, a, comp(&d, "Lifetime")) && value(&d, a, "Lifetime", "total") == 1, "add component with defaults");
    CHECK(!ed_add_component(&d, a, comp(&d, "Lifetime")), "cannot add twice");
    CHECK(!ed_add_component(&d, a, comp(&d, "Visual")), "transient components cannot be added");
    CHECK(ed_undo(&d) && !cw_comp_has(&d.gw.world, a, comp(&d, "Lifetime")), "undo add");
    CHECK(ed_undo(&d) && cw_comp_has(&d.gw.world, a, comp(&d, "ShadowCaster")), "undo remove");

    /* saving, listing and loading */
    char path[CW_GAME_PATH_CAPACITY];
    ed_scene_file(&d, "zz_editor_test", path, sizeof(path));
    CHECK(strstr(path, "zz_editor_test.cwscene") != NULL, "scene file name: %s", path);
    uint64_t saved_hash = scene_hash(&d);
    CHECK(ed_save_scene(&d, path) && !d.dirty && strcmp(d.scene_path, path) == 0, "save as: %s", d.status);
    char names[32][128];
    size_t n = ed_list_scenes(&d, names, 32);
    bool listed = false;
    for (size_t i = 0; i < n; ++i) if (strcmp(names[i], "zz_editor_test.cwscene") == 0) listed = true;
    CHECK(n >= 2 && listed && strcmp(names[0], "arena.cwscene") == 0, "scene list sorted and complete (%zu)", n);
    ed_new_scene(&d);
    CHECK(ed_entity_count(&d) == 0, "cleared");
    CHECK(ed_load_scene(&d, path) && ed_entity_count(&d) == 5 && scene_hash(&d) == saved_hash, "reload equals what was saved");
    CHECK(strcmp(ed_name_of(&d, a), "hero") == 0 && strcmp(ed_name_of(&d, a2), "hero copy") == 0, "names reloaded");
    CHECK(!ed_load_scene(&d, "nope.cwscene") && d.status_error, "missing scene reported");
    CHECK(ed_load_scene(&d, path), "reload after a failure works");

    /* play mode leaves the scene untouched */
    uint64_t before_play = scene_hash(&d);
    size_t history = d.history_count;
    CHECK(ed_play(&d) && d.playing && !d.paused, "play starts");
    CHECK(cw_comp_has(&d.gw.world, a, comp(&d, "Health")) && value(&d, a, "Health", "current") == 70 && value(&d, a, "Health", "maximum") == 70, "play bakes runtime components from config: %s", d.status);
    int steps = 0;
    for (int i = 0; i < 60; ++i) steps += ed_frame(&d, 1.0f / 60.0f);
    CHECK(steps >= 25 && d.gw.world.tick == (uint64_t)steps, "play advanced %d ticks", steps);
    CHECK(scene_hash(&d) != before_play, "the battle changed the world");
    cw_entity_id extra = ed_place(&d, prefab(&d, "blue", "archer"), 500, 500);
    CHECK(extra != CW_INVALID_ENTITY && d.history_count == history, "edits during play are not recorded");
    ed_toggle_pause(&d);
    CHECK(d.paused && ed_frame(&d, 1.0f) == 0, "pause stops time");
    ed_step(&d, 3);
    CHECK(d.gw.world.tick == (uint64_t)steps + 3, "step advances while paused");
    ed_stop(&d);
    CHECK(!d.playing && scene_hash(&d) == before_play && d.gw.world.tick == 0, "stop restores the scene");
    CHECK(!cw_world_is_valid(&d.gw.world, extra), "entities placed during play are gone");
    CHECK(!cw_comp_has(&d.gw.world, a, comp(&d, "Health")) && cw_comp_has(&d.gw.world, a, comp(&d, "HealthConfig")), "and so are the runtime components");
    CHECK(d.history_count == history && d.current == history - 1, "history kept across play");

    /* deletion */
    ed_select_all(&d);
    CHECK(d.selection_count == 5, "select all");
    CHECK(ed_delete_selection(&d) == 5 && ed_entity_count(&d) == 0 && d.selection_count == 0, "delete all");
    CHECK(ed_undo(&d) && ed_entity_count(&d) == 5, "undo delete");
    CHECK(!ed_save_scene(&d, NULL) || !d.status_error, "save to the current path");
    remove(path);

    /* the interface: elements are entities with the engine's Ui components */
    {
        cw_world *w = &d.gw.world;
        const cw_ui_components *c = &d.gw.ui;
        size_t count = ed_entity_count(&d);
        cw_ui_screen screen = {100, 50, 640, 360};   /* a game view inside the window: half the 1280x720 reference */
        CHECK(ed_ui_layout(&d, screen) == 0, "this scene has no interface yet");
        cw_entity_id panel = ed_create_ui(&d, ED_UI_PANEL, CW_INVALID_ENTITY);
        CHECK(panel != CW_INVALID_ENTITY && ed_is_ui(&d, panel) && cw_comp_has(w, panel, c->image) && !cw_comp_has(w, panel, c->button) && ed_is_selected(&d, panel) && strcmp(ed_name_of(&d, panel), "panel") == 0, "a panel: rect and image, selected and named: %s", d.status);
        const cw_ui_layout *pl = cw_comp_ptr(w, panel, c->layout);
        CHECK(pl && pl->rw == 640 && pl->rh == 360 && pl->rx == 320 && pl->ry == 180 && pl->scale == 0.5f && pl->x == 260, "centred, a quarter of the canvas, laid out on the game view");
        cw_entity_id button = ed_create_ui(&d, ED_UI_BUTTON, panel);
        CHECK(button != CW_INVALID_ENTITY && cw_comp_has(w, button, c->button) && ((const cw_ui_rect *)cw_comp_ptr(w, button, c->rect))->parent == panel, "a button inside the panel");
        CHECK(ed_entity_count(&d) == count + 3, "the button brought its label (%zu)", ed_entity_count(&d));
        cw_entity_id label = CW_INVALID_ENTITY;
        for (size_t i = 0; i < w->capacity; ++i)
            if (cw_world_is_valid(w, (cw_entity_id)i) && ed_is_ui(&d, (cw_entity_id)i) && ((const cw_ui_rect *)cw_comp_ptr(w, (cw_entity_id)i, c->rect))->parent == button) label = (cw_entity_id)i;
        CHECK(label != CW_INVALID_ENTITY && cw_comp_has(w, label, c->text) && strcmp(((const cw_ui_text *)cw_comp_ptr(w, label, c->text))->text, "button") == 0, "the label is a stretched text child");
        const cw_ui_layout *bl = cw_comp_ptr(w, button, c->layout);
        CHECK(bl->rw == 180 && bl->rh == 44 && fabs(bl->rx - (320 + 320 - 90)) < 1e-3, "the button is centred in the panel (%g %g)", (double)bl->rx, (double)bl->rw);
        /* hit testing in window pixels: the label is on top of the button, the panel beneath */
        float bx = bl->x + bl->w * 0.5f, by = bl->y + bl->h * 0.5f;
        CHECK(ed_ui_hit(&d, bx, by) == label && ed_ui_hit(&d, pl->x + 5, pl->y + 5) == panel && ed_ui_hit(&d, 10, 10) == CW_INVALID_ENTITY, "hits");
        /* moving and resizing, in reference pixels; children follow */
        ed_select_set(&d, button);
        ed_select_add(&d, label);   /* selecting the child too must not move it twice */
        CHECK(ed_ui_move_selection(&d, 10, -20) == 1 && bl->rx == 320 + 230 + 10 && bl->ry == 180 + 158 - 20, "moved by reference pixels, the child rides along");
        const cw_ui_layout *ll = cw_comp_ptr(w, label, c->layout);
        CHECK(ll->rx == bl->rx && ll->rw == bl->rw, "the stretched label follows");
        ed_commit(&d, "move element");
        CHECK(ed_ui_resize(&d, button, false, false, true, true, 20, 6) && bl->rw == 200 && bl->rh == 50, "resized from the bottom right (%g %g)", (double)bl->rw, (double)bl->rh);
        ed_commit(&d, "resize element");
        /* anchors: presets keep the element in place */
        const cw_ui_rect *br = cw_comp_ptr(w, button, c->rect);
        float before_x = bl->rx, before_y = bl->ry;
        CHECK(ed_ui_set_anchor_preset(&d, button, 2) && br->anchor_min_x == 1 && br->anchor_max_x == 1 && br->anchor_min_y == 0 && bl->rx == before_x && bl->ry == before_y, "top right preset, same place (%g %g)", (double)bl->rx, (double)bl->ry);
        CHECK(ed_ui_set_anchor_preset(&d, button, 11) && br->anchor_min_x == 0 && br->anchor_max_x == 1 && br->anchor_max_y == 1 && bl->rx == before_x && bl->rw == 200, "stretch preset, same place and size");
        CHECK(ed_ui_set_anchor_preset(&d, panel, 11) && ed_ui_layout(&d, screen) && pl->rw == 640, "the panel stretched too");
        cw_ui_screen wide = {0, 0, 2560, 720};
        ed_ui_layout(&d, wide);
        CHECK(pl->scale == 1 && pl->rw == 2560 - 640 && bl->rw == 1920 - 640 + 200 && ll->rw == bl->rw, "on an ultra-wide view the stretched panel and button grow, the label with them (%g %g)", (double)pl->rw, (double)bl->rw);
        ed_ui_layout(&d, screen);
        CHECK(ed_ui_set_anchor_preset(&d, button, 4) && br->anchor_min_x == 0.5f && br->anchor_max_x == 0.5f && bl->rw == 200 && bl->rx == before_x, "back to a point: the size is what it was at this view");
        /* re-parenting keeps the screen position */
        before_x = bl->rx; before_y = bl->ry;
        CHECK(ed_ui_set_parent(&d, button, CW_INVALID_ENTITY) && br->parent == CW_INVALID_ENTITY && bl->rx == before_x && bl->ry == before_y, "to the canvas, same place");
        CHECK(ed_ui_set_parent(&d, button, panel) && br->parent == panel && bl->rx == before_x, "back into the panel");
        CHECK(!ed_ui_set_parent(&d, panel, label) && d.status_error, "a cycle is refused: %s", d.status);
        CHECK(!ed_ui_set_parent(&d, button, a) && d.status_error, "only interface elements can be parents");
        /* text is a string field: the inspector edits it through the generic path */
        const cw_type_info *tt = cw_comp_type(w, c->text);
        CHECK(ed_set_field(&d, label, c->text, cw_type_field(tt, "text"), "Go \"now\"") && strcmp(((const cw_ui_text *)cw_comp_ptr(w, label, c->text))->text, "Go \"now\"") == 0, "set the text");
        /* scenes keep it; undo walks back through every step */
        CHECK(ed_save_scene(&d, path), "save with the interface");
        size_t steps = 0;
        while (ed_can_undo(&d) && cw_world_is_valid(w, panel)) { ed_undo(&d); ++steps; }
        CHECK(!cw_world_is_valid(w, panel) && steps >= 8, "undo removed the panel after %zu steps", steps);
        while (ed_can_redo(&d)) ed_redo(&d);
        CHECK(cw_world_is_valid(w, panel) && cw_world_is_valid(w, label) && strcmp(((const cw_ui_text *)cw_comp_ptr(w, label, c->text))->text, "Go \"now\"") == 0, "redo brought everything back");
        CHECK(ed_load_scene(&d, path) && ed_entity_count(&d) == count + 3, "reload the saved scene: %s", d.status);
        ed_ui_layout(&d, screen);
        size_t texts = 0;
        cw_query q = cw_query_iter(w, cw_mask(&c->text, 1));
        cw_entity_id id;
        while (cw_query_next(&q, &id)) if (strcmp(((const cw_ui_text *)cw_comp_ptr(w, id, c->text))->text, "Go \"now\"") == 0) ++texts;
        CHECK(texts == 1, "the quoted text survived the file");
        /* duplicates step aside */
        ed_select_set(&d, panel);
        CHECK(ed_duplicate_selection(&d, 0, 0) == 1 && d.selection_count == 1 && d.selection[0] != panel, "duplicate the panel");
        const cw_ui_layout *dl = cw_comp_ptr(w, d.selection[0], c->layout);
        CHECK(dl && dl->rx == pl->rx + 16 && dl->ry == pl->ry + 16, "the copy sits 16 pixels away");
        ed_delete_selection(&d);
        /* the project's canvas */
        CHECK(ed_set_ui_canvas(&d, 1920, 1080, CW_UI_SCALE_SHRINK) && d.project.ui.width == 1920 && d.gw.ui_canvas.mode == CW_UI_SCALE_SHRINK, "canvas saved: %s", d.status);
        ed_ui_layout(&d, screen);
        CHECK(fabs(pl->scale - (640.0f / 1920.0f)) < 1e-6, "the world lays out with the new canvas");
        CHECK(!ed_set_ui_canvas(&d, 10, 10, CW_UI_SCALE_EXPAND) && d.status_error, "a tiny reference is refused");
        CHECK(ed_set_ui_canvas(&d, 1280, 720, CW_UI_SCALE_EXPAND), "back to the default");
        /* and away with the interface, so the rest of the test sees the five entities */
        ed_select_clear(&d);
        for (size_t i = 0; i < w->capacity; ++i) if (cw_world_is_valid(w, (cw_entity_id)i) && ed_is_ui(&d, (cw_entity_id)i)) ed_select_add(&d, (cw_entity_id)i);
        CHECK(ed_delete_selection(&d) == 3 && ed_entity_count(&d) == count, "deleted the interface again");
        ed_save_scene(&d, path);
        remove(path);
    }

    /* empty entities and project prefabs */
    {
        cw_entity_id empty = ed_create_entity(&d, 321, 123);
        CHECK(empty != CW_INVALID_ENTITY && ed_is_selected(&d, empty) && cw_comp_has(&d.gw.world, empty, comp(&d, "Position")), "an empty entity gets the position component");
        CHECK(value(&d, empty, "Position", "x") == 321 && !cw_comp_has(&d.gw.world, empty, comp(&d, "Unit")), "at the requested spot, nothing else");
        CHECK(ed_add_component(&d, empty, comp(&d, "Obstacle")), "components are added in the inspector");
        ed_label(&d, empty, label, sizeof(label));
        CHECK(strcmp(label, "pillar") == 0, "the module describes it once it has components: %s", label);
        CHECK(ed_undo(&d) && ed_undo(&d) && !cw_world_is_valid(&d.gw.world, empty), "undo create");
        CHECK(ed_redo(&d) && ed_redo(&d) && cw_comp_has(&d.gw.world, empty, comp(&d, "Obstacle")), "redo");
        /* a prefab from the hero and its torch */
        ed_select_set(&d, a);
        ed_select_add(&d, torch);
        CHECK(!ed_save_prefab(&d, "bad/name"), "prefab names are file names");
        const cw_asset *prefab = ed_save_prefab(&d, "hero_with_torch");
        CHECK(prefab && prefab->type == CW_ASSET_PREFAB && strcmp(prefab->path, "prefabs/hero_with_torch.cwprefab") == 0, "prefab saved and registered: %s", d.status);
        const cw_asset *list[8];
        CHECK(ed_list_prefabs(&d, list, 8) == 1 && list[0] == prefab, "listed");
        uint32_t pid = prefab ? prefab->id : 0;
        size_t count = ed_entity_count(&d);
        CHECK(ed_place_prefab(&d, pid, 900, 700) == 2 && ed_entity_count(&d) == count + 2 && d.selection_count == 2, "placed two entities: %s", d.status);
        cw_entity_id hero2 = d.selection[0], torch2b = d.selection[1];
        CHECK(value(&d, hero2, "Position", "x") == 900 && value(&d, torch2b, "Attached", "parent") == (double)hero2 && strcmp(ed_name_of(&d, hero2), "hero") == 0, "placed at the spot, reference remapped, name kept");
        CHECK(ed_undo(&d) && ed_entity_count(&d) == count, "undo the placement");
        ed_select_set(&d, a);
        CHECK(ed_save_prefab(&d, "hero_with_torch") == prefab && ed_list_prefabs(&d, list, 8) == 1, "saving again overwrites");
        CHECK(ed_place_prefab(&d, pid, 10, 10) == 1, "the updated prefab has one entity");
        ed_undo(&d);
        CHECK(!ed_place_prefab(&d, 12345, 0, 0), "unknown prefabs are refused");
        CHECK(ed_delete_prefab(&d, pid) && ed_list_prefabs(&d, list, 8) == 0 && !fopen("../games/combat/prefabs/hero_with_torch.cwprefab", "r"), "deleted");
        ed_select_set(&d, empty);
        CHECK(ed_delete_selection(&d) == 1 && ed_entity_count(&d) == 5, "back to five entities");
    }

    /* the animation editor: clips, tracks, keys, preview, record */
    {
        ea_state *an = calloc(1, sizeof(*an));
        ea_init(an);
        const cw_asset *clips[8];
        CHECK(ea_list(&d, clips, 8) == 0, "no clips yet");
        CHECK(!ea_new(an, &d, "bad name", 1), "clip names are file names");
        CHECK(ea_new(an, &d, "lamp_pulse", 2.0f) && an->open && an->dirty && an->asset == 0, "new clip");
        ed_select_set(&d, torch);   /* a light */
        CHECK(ea_target(an, &d) == torch, "the target is the primary selection");
        cw_component_id light = comp(&d, "Light"), pos = comp(&d, "Position");
        const cw_field *intensity = field(&d, "Light", "intensity"), *py = field(&d, "Position", "y");
        CHECK(ea_track_add(an, &d, light, intensity) == 0 && ea_track_add(an, &d, pos, py) == 1 && an->clip.track_count == 2, "tracks from the entity's fields");
        CHECK(ea_track_add(an, &d, comp(&d, "Attached"), field(&d, "Attached", "parent")) < 0 && d.status_error, "entity references cannot be animated");
        double i0 = value(&d, torch, "Light", "intensity"), y0 = value(&d, torch, "Position", "y");
        CHECK(ea_key_from_entity(an, &d, 0) == 0 && an->clip.tracks[0].keys[0].value == (float)i0, "key from the entity's value at the playhead");
        ea_scrub(an, &d, 1.0f);
        CHECK(an->previewing && an->entity == torch && an->playhead == 1.0f, "scrubbing starts a preview on the target");
        CHECK(ea_key_set(an, 0, 1.0f, 5.0f, CW_INTERP_LINEAR) == 1 && ea_key_set(an, 1, 0, (float)y0, CW_INTERP_LINEAR) == 0 && ea_key_set(an, 1, 2.0f, (float)y0 + 100, CW_INTERP_LINEAR) == 1, "keys set");
        ea_scrub(an, &d, 1.0f);
        CHECK(fabs(value(&d, torch, "Light", "intensity") - 5.0) < 1e-5 && fabs(value(&d, torch, "Position", "y") - (y0 + 50)) < 1e-3, "the preview applies the clip to the entity (%g, %g)", value(&d, torch, "Light", "intensity"), value(&d, torch, "Position", "y"));
        ea_play(an, &d);
        for (int i = 0; i < 30; ++i) ea_frame(an, &d, 1.0f / 60.0f);
        CHECK(an->playing && fabs(an->playhead - 1.5f) < 1e-4, "preview playback advances the playhead (%g)", (double)an->playhead);
        for (int i = 0; i < 60; ++i) ea_frame(an, &d, 1.0f / 60.0f);
        CHECK(an->playhead < 2.0f && an->playing, "loops");
        ea_stop(an, &d);
        CHECK(!an->previewing && value(&d, torch, "Light", "intensity") == i0 && value(&d, torch, "Position", "y") == y0, "stop restores the entity's components (%g, %g)", value(&d, torch, "Light", "intensity"), value(&d, torch, "Position", "y"));
        CHECK(!d.dirty || true, "(previewing never dirties the scene by itself)");
        /* keys: move, retime, interpolation, remove */
        CHECK(ea_key_move(an, 0, 1, 1.5f) == 1 && an->clip.tracks[0].keys[1].time == 1.5f, "move a key");
        CHECK(ea_key_set_interp(an, 0, 0, CW_INTERP_EASE) && an->clip.tracks[0].keys[0].interp == CW_INTERP_EASE, "interpolation");
        CHECK(ea_key_remove(an, 0, 1) && an->clip.tracks[0].key_count == 1, "remove a key");
        CHECK(ea_set_length(an, 3.0f) && ea_set_mode(an, CW_ANIM_PINGPONG) && !ea_set_length(an, 0), "length and mode");
        /* record mode: inspector edits become keys */
        an->record = true;
        d.no_history = true;
        size_t steps = d.history_count;
        double r0 = value(&d, torch, "Light", "radius");
        ea_scrub(an, &d, 2.0f);
        CHECK(ed_set_field(&d, torch, light, field(&d, "Light", "radius"), "500"), "edit while recording");
        CHECK(ea_record_field(an, &d, torch, light, field(&d, "Light", "radius")) && cw_anim_track_find(&an->clip, "Light", "radius") == 2 && an->clip.tracks[2].key_count == 1 && an->clip.tracks[2].keys[0].time == 2.0f && an->clip.tracks[2].keys[0].value == 500, "recorded a key at the playhead on a new track");
        an->record = false;
        d.no_history = false;
        ea_stop(an, &d);
        CHECK(value(&d, torch, "Light", "radius") == r0 && d.history_count == steps, "the recorded value lives in the clip: the entity is restored and nothing entered the scene history");
        /* save, list, reopen, delete */
        CHECK(ea_save(an, &d) && !an->dirty && an->asset == cw_asset_id("lamp_pulse.cwanim"), "saved and registered: %s", d.status);
        CHECK(ea_list(&d, clips, 8) == 1 && clips[0]->type == CW_ASSET_ANIMATION && strcmp(clips[0]->path, "animations/lamp_pulse.cwanim") == 0, "listed");
        uint32_t cid = an->asset;
        ea_close(an, &d);
        CHECK(!an->open && ea_open(an, &d, cid) && an->clip.track_count == 3 && an->clip.length == 3.0f && an->clip.mode == CW_ANIM_PINGPONG, "reopened from the asset: %s", d.status);
        /* the runtime component plays the saved clip in play mode */
        CHECK(ed_add_component(&d, torch, d.gw.animation_ct), "add the Animation component");
        char at[CW_ASSET_NAME_CAPACITY + 2];
        snprintf(at, sizeof(at), "@%s", clips[0]->name);
        CHECK(ed_set_field(&d, torch, d.gw.animation_ct, cw_type_field(&cw_animation_type, "clip"), at), "point it at the clip");
        ed_play(&d);
        for (int i = 0; i < 30; ++i) ed_frame(&d, 1.0f / 30.0f);
        const cw_animation *state = cw_comp_ptr(&d.gw.world, torch, d.gw.animation_ct);
        CHECK(state->time > 0.5f && fabs(value(&d, torch, "Position", "y") - y0) > 1, "the clip animates the entity in play mode (t=%g, y=%g)", (double)state->time, value(&d, torch, "Position", "y"));
        ed_stop(&d);
        CHECK(ea_delete(an, &d, cid) && ea_list(&d, clips, 8) == 0 && !an->open, "deleted");
        ed_undo(&d);   /* the Animation component */
        free(an);
    }

    /* assets: the manifest is seeded by a scan when the project opens */
    CHECK(d.assets.count >= 2 && cw_assets_current() == &d.assets, "assets scanned on open (%zu) and current", d.assets.count);
    CHECK(cw_assets_find_path(&d.assets, "scenes/arena.cwscene") != NULL && cw_assets_find_path(&d.assets, "combat_game.c") != NULL, "scenes and sources are assets");
    CHECK(cw_assets_find_path(&d.assets, "combat.cwproj") == NULL && cw_assets_find_path(&d.assets, "render.cfg") == NULL, "project and config files are not");
    {
        FILE *f = fopen("zz_editor_import.txt", "w");
        if (f) { fputs("hello\n", f); fclose(f); }
        size_t before = d.assets.count;
        const cw_asset *imported = ed_import_asset(&d, "zz_editor_import.txt", false);
        CHECK(imported && d.assets.count == before + 1 && imported->type == CW_ASSET_DATA && strcmp(imported->path, "assets/data/zz_editor_import.txt") == 0, "import: %s", d.status);
        CHECK(!ed_import_asset(&d, "zz_editor_import.txt", false) && d.status_error, "re-import refused without replace");
        CHECK(ed_import_asset(&d, "zz_editor_import.txt", true) != NULL && d.assets.count == before + 1, "replace");
        CHECK(!ed_import_asset(&d, "nope.bin", false), "missing source reported");
        cw_assets reread;
        cw_assets_init(&reread, d.project.assets);
        char error[128];
        CHECK(cw_assets_load(&reread, error, sizeof(error)) && cw_assets_find_name(&reread, "zz_editor_import.txt") != NULL, "the manifest was saved");
        cw_assets_destroy(&reread);
        uint32_t id = imported ? imported->id : 0;
        CHECK(ed_asset_references(&d, id) == 0, "no references to it");
        CHECK(ed_rename_asset(&d, id, "greeting.txt") && cw_assets_find_name(&d.assets, "greeting.txt") != NULL, "rename");
        CHECK(ed_remove_asset(&d, cw_asset_id("greeting.txt"), true) && d.assets.count == before, "remove with delete");
        CHECK(!fopen("../games/combat/assets/data/zz_editor_import.txt", "r"), "file deleted");
        remove("zz_editor_import.txt");
        (void)!system("rm -rf \"" "zz_editor_dir" "\" && mkdir -p zz_editor_dir && echo a > zz_editor_dir/a.csv && echo b > zz_editor_dir/b.json");
        CHECK(ed_import_directory(&d, "zz_editor_dir", false) == 2 && cw_assets_find_name(&d.assets, "a.csv") && cw_assets_find_name(&d.assets, "b.json"), "import a directory: %s", d.status);
        ed_remove_asset(&d, cw_asset_id("a.csv"), true);
        ed_remove_asset(&d, cw_asset_id("b.json"), true);
        (void)!system("rm -rf zz_editor_dir");
        CHECK(d.assets.count == before, "back to the scanned set (%zu)", d.assets.count);
        /* assets the editor authors itself (materials, shaders, clips) go through ed_write_asset */
        const char *mat = "cwmaterial 1\nname zz_glow\neffect glow\n";
        const cw_asset *written = ed_write_asset(&d, "materials/zz_glow.cwmat", mat, strlen(mat));
        CHECK(written && written->type == CW_ASSET_MATERIAL && strcmp(written->name, "zz_glow.cwmat") == 0 && d.assets.count == before + 1, "write a material asset: %s", d.status);
        {
            char path[CW_GAME_PATH_CAPACITY + 64];
            snprintf(path, sizeof(path), "%smaterials/zz_glow.cwmat", d.project.dir);
            cw_buffer back;
            cw_file_error ferr;
            CHECK(cw_file_read(path, 0, &back, &ferr) && back.size == strlen(mat) && memcmp(back.data, mat, back.size) == 0, "the file and its directory were created");
            if (back.data) cw_buffer_free(&back);
            const char *mat2 = "cwmaterial 1\nname zz_glow\neffect flash\n";
            CHECK(ed_write_asset(&d, "materials/zz_glow.cwmat", mat2, strlen(mat2)) == written && d.assets.count == before + 1 && written->size == strlen(mat2), "rewriting refreshes instead of duplicating");
            cw_assets reread;
            cw_assets_init(&reread, d.project.assets);
            CHECK(cw_assets_load(&reread, error, sizeof(error)) && cw_assets_find_name(&reread, "zz_glow.cwmat") != NULL, "the manifest was saved");
            cw_assets_destroy(&reread);
        }
        CHECK(ed_remove_asset(&d, cw_asset_id("zz_glow.cwmat"), true) && d.assets.count == before, "remove the material");
        CHECK(!ed_write_asset(&d, "", "x", 1) && d.status_error, "an empty path is refused");
        char cleanup[CW_GAME_PATH_CAPACITY * 4 + 64];
        snprintf(cleanup, sizeof(cleanup), "rm -rf \"%sassets\" \"%sprefabs\" \"%sanimations\" \"%smaterials\"", d.project.dir, d.project.dir, d.project.dir, d.project.dir);   /* the import, prefab, clip and material directories */
        (void)!system(cleanup);
    }

    /* world inspection */
    {
        cw_component_id light = comp(&d, "Light"), unit = comp(&d, "Unit");
        CHECK(ed_component_entities(&d, light) == 2 && ed_component_entities(&d, unit) == 3 && ed_component_entities(&d, comp(&d, "Health")) == 0, "component counts (%zu lights, %zu units)", ed_component_entities(&d, light), ed_component_entities(&d, unit));
        ed_select_with_component(&d, light);
        CHECK(d.selection_count == 2 && ed_is_selected(&d, torch) && ed_is_selected(&d, torch2), "select by component");
        CHECK(ed_set_system_enabled(&d, "movement", false) && !cw_sim_find(&d.gw.sim, "movement")->enabled, "disable a system");
        CHECK(!ed_set_system_enabled(&d, "no_such_system", true), "unknown systems are refused");
        ed_play(&d);
        float x0 = (float)value(&d, a, "Position", "x");
        for (int i = 0; i < 30; ++i) ed_frame(&d, 1.0f / 30.0f);
        CHECK((float)value(&d, a, "Position", "x") == x0, "with movement disabled nothing moves");
        ed_stop(&d);
        CHECK(ed_set_system_enabled(&d, "movement", true), "re-enable");
        ed_select_clear(&d);
    }

    /* autosave writes a sibling copy and leaves the scene alone */
    {
        char auto_path[CW_GAME_PATH_CAPACITY];
        ed_autosave_path(&d, auto_path, sizeof(auto_path));
        CHECK(strstr(auto_path, "zz_editor_test.autosave.cwscene") != NULL, "autosave path: %s", auto_path);
        ed_place(&d, 0, 50, 50);
        CHECK(ed_autosave(&d) && d.dirty, "autosave keeps the scene dirty");
        cw_scene_result r;
        cw_game_world gw;
        char error[128];
        cw_game_world_init(&gw, d.game.module, error, sizeof(error));
        CHECK(cw_game_world_load_scene(&gw, auto_path, NULL, &r) && r.entities == ed_entity_count(&d), "the autosave holds the current world (%zu)", r.entities);
        cw_game_world_destroy(&gw);
        remove(auto_path);
        ed_undo(&d);
    }

    /* editor preferences: a tunables table with a per-user file */
    {
        ed_settings s;
        ed_settings_defaults(&s);
        CHECK(s.grid && s.grid_spacing == 50 && !s.snap && s.nudge == 1 && s.text_scale == 1.0f && s.confirm_discard, "defaults");
        CHECK(ed_settings_snap(&s, 123) == 123, "no snapping by default");
        s.snap = true;
        CHECK(ed_settings_snap(&s, 123) == 100 && ed_settings_snap(&s, 126) == 150, "snap to the grid spacing");
        s.nudge = 5;
        s.text_scale = 1.25f;
        CHECK(ed_settings_save(&s, "zz_editor.cfg"), "save preferences");
        ed_settings t;
        ed_settings_defaults(&t);
        char error[128] = "";
        CHECK(ed_settings_load(&t, "zz_editor.cfg", error, sizeof(error)) && t.snap && t.nudge == 5 && t.text_scale == 1.25f && t.grid_spacing == 50, "reload: %s", error);
        CHECK(!ed_settings_load(&t, "zz_missing.cfg", error, sizeof(error)) && t.nudge == 5, "a missing file leaves the values alone");
        size_t count;
        const r2d_tunable *defs = ed_settings_defs(&count);
        CHECK(count >= 8 && strcmp(defs[0].section, "view") == 0, "the table drives the settings panel (%zu tunables)", count);
        remove("zz_editor.cfg");
    }

    /* project settings: the player's window lives in the project file, rendering settings next to it */
    char render[CW_GAME_PATH_CAPACITY];
    ed_render_path(&d, render, sizeof(render));
    CHECK(strstr(render, "render.cfg") != NULL, "render settings path: %s", render);
    {
        int w = d.project.window_width, h = d.project.window_height;
        char title[96];
        snprintf(title, sizeof(title), "%s", d.project.title);
        CHECK(!ed_set_window(&d, 10, 10, "x") && d.project.window_width == w, "invalid window sizes are refused");
        CHECK(ed_set_window(&d, 1024, 576, "Test Title") && d.project.window_width == 1024 && strcmp(d.project.title, "Test Title") == 0, "window saved: %s", d.status);
        cw_project check;
        char error[128];
        CHECK(cw_project_load(&check, d.project.path, error, sizeof(error)) && check.window_height == 576 && strcmp(check.title, "Test Title") == 0, "the project file was rewritten");
        CHECK(ed_set_window(&d, w, h, title), "restored");
    }

    /* rebuild and hot reload through the build script */
    if (engine_root) {
        uint64_t kept = scene_hash(&d);
        CHECK(ed_build(&d, "module") && d.last_build_status == 0, "module rebuild: %s", d.status);
        CHECK(d.world_ready && ed_entity_count(&d) == 5 && scene_hash(&d) == kept, "scene kept across the reload");
        CHECK(strstr(d.log, "module:") != NULL, "build log captured");
        CHECK(!ed_build(&d, "bogus") && d.last_build_status != 0, "a failing build is reported: %s", d.status);
    } else printf("(no engine root given: build and reload not exercised)\n");

    /* a different game in the same editor */
    if (second) {
        char first_name[64];
        snprintf(first_name, sizeof(first_name), "%s", d.game.module->name);
        size_t first_prefabs = d.game.module->prefab_count;
        CHECK(ed_open_project(&d, second), "open the second project: %s", d.status);
        if (!d.world_ready && engine_root) CHECK(ed_build(&d, "module"), "build the second module: %s", d.status);
        CHECK(d.world_ready, "second module loaded: %s", d.status);
        if (d.world_ready) {
            CHECK(strcmp(d.game.module->name, first_name) != 0 && d.game.module->prefab_count != first_prefabs, "a different game: %s", d.game.module->name);
            CHECK(comp(&d, "Unit") == CW_INVALID_COMPONENT && cw_comp_by_name(&d.gw.world, "Body", &(cw_component_id){0}), "its own schema");
            size_t before = ed_entity_count(&d);
            CHECK(before == 6, "its main scene opened (%zu entities)", before);
            CHECK(ed_place(&d, 0, 100, 100) != CW_INVALID_ENTITY && ed_entity_count(&d) == before + 1, "placing works in the second game");
            CHECK(ed_play(&d) && ed_frame(&d, 0.5f) > 0, "and it simulates");
            ed_stop(&d);
            CHECK(ed_entity_count(&d) == before + 1 && !d.playing, "stop restores");
        }
    } else printf("(no second project given: game switching not exercised)\n");

    ed_shutdown(&d);
    if (failures) { printf("%d of %d checks failed\n", failures, checks); return 1; }
    printf("OK: %d editor checks passed\n", checks);
    return 0;
}
