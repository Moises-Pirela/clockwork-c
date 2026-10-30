/* Headless checks for the launcher: the registry, scanning the games
 * directory, creating a project from the template (and, with an engine
 * root, building its module and loading its scene), launch commands.
 *
 *   clockwork_launcher_tests <engine_root> <scratch_dir>
 */
#define _CRT_SECURE_NO_WARNINGS 1
#ifndef _WIN32
#define _DEFAULT_SOURCE 1
#endif
#include "launcher_core.h"
#include "cw_assets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void setenv_compat(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

int main(int argc, char **argv) {
    if (argc < 3) { fputs("usage: clockwork_launcher_tests <engine_root> <scratch_dir>\n", stderr); return 2; }
    const char *root = argv[1], *scratch = argv[2];
    char registry[CW_GAME_PATH_CAPACITY];
    snprintf(registry, sizeof(registry), "%s/launcher_test.cwlist", scratch);
    remove(registry);
    setenv_compat("CW_PROJECTS", registry);
    setenv_compat("CW_EDITOR", "/nonexistent/clockwork_editor");

    lc_launcher *l = calloc(1, sizeof(*l));
    lc_init(l, root, "/tools/");
    CHECK(strcmp(l->registry, registry) == 0, "registry from the environment: %s", l->registry);
    CHECK(strcmp(l->editor, "/nonexistent/clockwork_editor") == 0 && strstr(l->player, "/tools/clockwork_player"), "tool paths: %s, %s", l->editor, l->player);
    lc_load(l);
    CHECK(l->count >= 2, "scanned the games directory: %zu projects", l->count);
    int combat = -1, orbit = -1;
    for (size_t i = 0; i < l->count; ++i) {
        if (l->entries[i].loadable && strcmp(l->entries[i].project.name, "combat") == 0) combat = (int)i;
        if (l->entries[i].loadable && strcmp(l->entries[i].project.name, "orbit") == 0) orbit = (int)i;
    }
    CHECK(combat >= 0 && orbit >= 0, "combat and orbit registered");
    if (combat < 0 || orbit < 0) return 1;
    CHECK(l->entries[combat].scene_count >= 1 && strcmp(l->entries[combat].scenes[0], "arena.cwscene") == 0, "scenes listed (%zu)", l->entries[combat].scene_count);
    CHECK(l->entries[combat].modified > 0, "modification time");
    FILE *f = fopen(registry, "r");
    CHECK(f != NULL, "registry written after the scan");
    if (f) fclose(f);

    /* ordering and persistence */
    lc_touch(l, (size_t)orbit);
    CHECK(strcmp(l->entries[0].project.name, "orbit") == 0, "touch moves to the top");
    CHECK(lc_save(l), "save");
    lc_launcher *again = calloc(1, sizeof(*again));
    lc_init(again, root, NULL);
    lc_load(again);
    CHECK(again->count == l->count && strcmp(again->entries[0].project.name, "orbit") == 0, "order persisted (%zu)", again->count);
    CHECK(lc_find(again, l->entries[0].path) == 0 && lc_find(again, "nope.cwproj") == -1, "find");
    free(again);

    /* a broken entry stays visible with its error */
    char broken[CW_GAME_PATH_CAPACITY];
    snprintf(broken, sizeof(broken), "%s/broken.cwproj", scratch);
    f = fopen(broken, "w");
    if (f) { fputs("cwproject 1\nname broken\n", f); fclose(f); }
    CHECK(lc_add(l, broken) >= 0, "a project file that does not parse is still registered");
    int b = lc_find(l, broken);
    CHECK(b >= 0 && !l->entries[b].loadable && l->entries[b].error[0], "and reports why: %s", b >= 0 ? l->entries[b].error : "");
    CHECK(!lc_open_in_editor(l, (size_t)b, NULL) && l->status_error, "cannot open a broken project");
    CHECK(lc_remove(l, (size_t)b) && lc_find(l, broken) < 0, "remove");
    remove(broken);
    CHECK(lc_add(l, "/nonexistent/x.cwproj") < 0, "missing files are not registered");
    CHECK(lc_add(l, l->entries[1].path) == 1, "adding a known project returns its index");

    /* launch commands (not executed) */
    char cmd[2048];
    lc_editor_command(l, 0, "system.cwscene", cmd, sizeof(cmd));
    CHECK(strstr(cmd, "clockwork_editor\" --project \"") && strstr(cmd, "orbit.cwproj\" --scene \"") && strstr(cmd, "system.cwscene"), "editor command: %s", cmd);
    lc_player_command(l, 0, NULL, cmd, sizeof(cmd));
#ifdef _WIN32
    CHECK(strstr(cmd, "clockwork_player.exe\" --project \"") && !strstr(cmd, "--scene"), "player command: %s", cmd);
#else
    CHECK(strstr(cmd, "clockwork_player\" --project \"") && !strstr(cmd, "--scene"), "player command: %s", cmd);
#endif
    CHECK(!lc_open_in_editor(l, 0, NULL) && strstr(l->status, "not found"), "a missing editor is reported: %s", l->status);

    /* creating a project from the template */
    CHECK(lc_name_valid("my_game") && !lc_name_valid("1bad") && !lc_name_valid("a-b") && !lc_name_valid("") && !lc_name_valid("has space"), "name rules");
    size_t made = 99;
    CHECK(!lc_create(l, scratch, "bad name", LC_TEMPLATE_2D, &made), "invalid names are refused");
    CHECK(!lc_create(l, "/nonexistent-dir", "fine", LC_TEMPLATE_2D, &made), "missing parent refused");
    char dir[CW_GAME_PATH_CAPACITY];
    snprintf(dir, sizeof(dir), "%s/lc_test_game", scratch);
    char rm[CW_GAME_PATH_CAPACITY + 16];
#ifdef _WIN32
    snprintf(rm, sizeof(rm), "rmdir /s /q \"%s\"", dir);
#else
    snprintf(rm, sizeof(rm), "rm -rf \"%s\"", dir);
#endif
    (void)!system(rm);
    CHECK(lc_create(l, scratch, "lc_test_game", LC_TEMPLATE_2D, &made), "create: %s", l->status);
    CHECK(made == 0 && l->count >= 3 && strcmp(l->entries[0].project.name, "lc_test_game") == 0, "new project at the top");
    CHECK(!lc_create(l, scratch, "lc_test_game", LC_TEMPLATE_2D, &made), "creating twice is refused");
    lc_entry *e = &l->entries[0];
    CHECK(e->loadable && !e->module_built && e->scene_count == 1 && strcmp(e->scenes[0], "main.cwscene") == 0, "fresh project: loadable, unbuilt, one scene");
    CHECK(e->project.source_count == 1 && strstr(e->project.sources[0], "lc_test_game_game.c"), "source named after the project");
    CHECK(!lc_run(l, 0, NULL) && strstr(l->status, "build"), "running an unbuilt project asks for a build");
    /* the generated source carries the project name */
    cw_buffer src;
    cw_file_error ferr;
    CHECK(cw_file_read(e->project.sources[0], 0, &src, &ferr), "read the generated source");
    CHECK(strstr((const char *)src.data, "\"lc_test_game\"") && !strstr((const char *)src.data, "\"starter\""), "module name substituted");
    cw_buffer_free(&src);
    /* the starter image and material come along with the template */
    {
        char path[CW_GAME_PATH_CAPACITY + 64];
        snprintf(path, sizeof(path), "%s/assets/images/ball.png", e->project.dir);
        FILE *source_file = fopen(path, "rb");
        CHECK(source_file != NULL, "template image copied to %s", path);
        if (source_file) fclose(source_file);
        snprintf(path, sizeof(path), "%s/materials/ball.cwmat", e->project.dir);
        source_file = fopen(path, "rb");
        CHECK(source_file != NULL, "template material copied");
        if (source_file) fclose(source_file);
        cw_assets assets;
        cw_assets_init(&assets, e->project.assets);
        size_t found = cw_assets_scan(&assets, "");
        CHECK(found >= 4 && cw_assets_find_name(&assets, "ball.png") && cw_assets_find_name(&assets, "ball.cwmat"), "scanning the new project registers the starter assets (%zu)", found);
        cw_assets_destroy(&assets);
    }

    /* build it and load its scene through the engine */
    CHECK(lc_build(l, 0) && e->module_built, "build the new project: %s", l->status);
    if (e->module_built) {
        cw_game game;
        char error[256];
        CHECK(cw_game_open(&game, e->project.module, error, sizeof(error)), "open the built module: %s", error);
        if (game.module) {
            CHECK(strcmp(game.module->name, "lc_test_game") == 0 && game.module->prefab_count == 2, "it is the template game named after the project");
            cw_game_world gw;
            CHECK(cw_game_world_init(&gw, game.module, error, sizeof(error)), "bind");
            cw_scene_result r;
            CHECK(cw_game_world_load_scene(&gw, e->project.main_scene, NULL, &r) && r.entities == 8, "the template scene loads: %s", r.message);
            cw_game_world_settle(&gw);
            cw_game_world_tick(&gw, 120);
            CHECK(cw_world_entity_count(&gw.world) == 8, "and simulates");
            /* the starter interface: a title, a bounce button and its label; clicking the button reverses the movers */
            cw_ui_screen screen = {0, 0, 1280, 720};
            cw_game_world_set_ui_screen(&gw, screen);
            CHECK(cw_game_world_ui_layout(&gw) == 3, "three interface elements");
            cw_entity_id button = cw_ui_hit(&gw.world, &gw.ui, 1200, 30, true);
            CHECK(button != CW_INVALID_ENTITY, "the bounce button sits in the top right corner");
            cw_component_id mover = CW_INVALID_COMPONENT;
            for (unsigned c = 0; c < CW_MAX_COMPONENTS && mover == CW_INVALID_COMPONENT; ++c) { const char *n = cw_comp_name(&gw.world, (cw_component_id)c); if (n && strcmp(n, "Mover") == 0) mover = (cw_component_id)c; }
            const cw_type_info *mt = mover != CW_INVALID_COMPONENT ? cw_comp_type(&gw.world, mover) : NULL;
            const cw_field *vx = mt ? cw_type_field(mt, "vx") : NULL;
            double before = vx && cw_comp_has(&gw.world, 0, mover) ? cw_field_get(vx, cw_comp_ptr(&gw.world, 0, mover)) : 0;
            cw_game_world_ui_input(&gw, 1200, 30, true, true, false);
            cw_game_world_ui_input(&gw, 1200, 30, false, false, true);
            cw_game_world_tick(&gw, 1);
            double after = vx && cw_comp_has(&gw.world, 0, mover) ? cw_field_get(vx, cw_comp_ptr(&gw.world, 0, mover)) : 0;
            CHECK(vx && before != 0 && after == -before, "the game reacted to the click: the ball's vx went from %g to %g", before, after);
            cw_game_world_destroy(&gw);
        }
        cw_game_close(&game);
    }
    (void)!system(rm);

    /* the 3D template: a renderer 3d project with brushes, lights and materials */
    {
        char dir3[CW_GAME_PATH_CAPACITY], rm3[CW_GAME_PATH_CAPACITY + 16];
        snprintf(dir3, sizeof(dir3), "%s/lc_test_level", scratch);
#ifdef _WIN32
        snprintf(rm3, sizeof(rm3), "rmdir /s /q \"%s\"", dir3);
#else
        snprintf(rm3, sizeof(rm3), "rm -rf \"%s\"", dir3);
#endif
        (void)!system(rm3);
        CHECK(strcmp(lc_template_dir(LC_TEMPLATE_3D), "launcher/template3d") == 0 && strcmp(lc_template_name(LC_TEMPLATE_3D), "3D") == 0, "the 3D template's names");
        CHECK(lc_create(l, scratch, "lc_test_level", LC_TEMPLATE_3D, &made), "create a 3D project: %s", l->status);
        lc_entry *e3 = &l->entries[0];
        CHECK(e3->loadable && e3->project.renderer == CW_RENDERER_3D && strcmp(e3->project.name, "lc_test_level") == 0, "the new project is viewed in 3D");
        char path[CW_GAME_PATH_CAPACITY + 64];
        snprintf(path, sizeof(path), "%s/materials/bricks.cwmat", e3->project.dir);
        FILE *f3 = fopen(path, "rb");
        CHECK(f3 != NULL, "the 3D template's materials came along");
        if (f3) fclose(f3);
        cw_assets assets;
        cw_assets_init(&assets, e3->project.assets);
        CHECK(cw_assets_load(&assets, NULL, 0) && cw_assets_find_name(&assets, "grass.cwmat") && cw_assets_find_name(&assets, "tiles.png"), "its manifest lists the materials and images for the player");
        cw_assets_destroy(&assets);
        CHECK(lc_build(l, 0) && e3->module_built, "build the 3D project: %s", l->status);
        if (e3->module_built) {
            cw_game game;
            char error[256];
            CHECK(cw_game_open(&game, e3->project.module, error, sizeof(error)), "open the built 3D module: %s", error);
            if (game.module) {
                CHECK(strcmp(game.module->name, "lc_test_level") == 0 && game.module->prefab_count == 6, "the 3D starter named after the project");
                cw_game_world gw;
                CHECK(cw_game_world_init(&gw, game.module, error, sizeof(error)), "bind");
                cw_scene_result r;
                CHECK(cw_game_world_load_scene(&gw, e3->project.main_scene, NULL, &r) && r.entities == 17 && !r.warnings, "the 3D scene loads: %s (%zu warnings)", r.message, r.warnings);
                cw_game_world_settle(&gw);
                CHECK(cw_comp_has(&gw.world, 5, gw.brush_ct) && cw_brush_clip_count(cw_comp_ptr(&gw.world, 5, gw.brush_ct)) == 1, "its ramp is a wedge brush");
                CHECK(cw_comp_has(&gw.world, 11, gw.light3d_ct) && ((cw_light3d *)cw_comp_ptr(&gw.world, 11, gw.light3d_ct))->shadows, "its sun casts shadows");
                CHECK(cw_comp_has(&gw.world, 15, gw.sky3d_ct) && ((cw_sky3d *)cw_comp_ptr(&gw.world, 15, gw.sky3d_ct))->sun_intensity == 3, "its sky is a gradient with a sun");
                CHECK(cw_comp_has(&gw.world, 16, gw.decal3d_ct) && ((cw_decal3d *)cw_comp_ptr(&gw.world, 16, gw.decal3d_ct))->material == cw_asset_id("scorch.cwmat"), "its scorch is a decal");
                cw_game_world_tick(&gw, 60);
                CHECK(cw_world_entity_count(&gw.world) == 17, "and simulates");
                cw_game_world_destroy(&gw);
            }
            cw_game_close(&game);
        }
        (void)!system(rm3);
    }
    remove(registry);
    free(l);
    if (failures) { printf("%d of %d checks failed\n", failures, checks); return 1; }
    printf("OK: %d launcher checks passed\n", checks);
    return 0;
}
