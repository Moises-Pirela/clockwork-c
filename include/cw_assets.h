/* cw_assets: the project's asset registry.
 *
 * An asset is a file that belongs to a project (a scene, an image, a sound,
 * a data table, a config, a source file...) recorded in the project's
 * manifest (`assets.cwassets`, next to the project file) with a stable
 * name, a type guessed from its extension and its path relative to the
 * project. Importing copies a file from anywhere into the project's asset
 * directory and records it; scanning records files that are already there.
 *
 * Assets are referenced by id: cw_asset_id(name) is a pure hash of the
 * name, so a component can hold a uint32_t asset reference (a field with
 * the CW_SEM_ASSET semantic) that scenes write as `@name`, the editor shows
 * and picks by name, and game code resolves to a path at run time with
 * cw_assets_path against the manifest the host loaded (cw_assets_current).
 * No table is needed to compute an id, only to resolve one.
 */
#ifndef CW_ASSETS_H
#define CW_ASSETS_H

#include "clockwork.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CW_ASSET_NAME_CAPACITY 96
#define CW_ASSET_PATH_CAPACITY 512
#define CW_ASSETS_FORMAT_VERSION 1

typedef enum {
    CW_ASSET_OTHER, CW_ASSET_SCENE, CW_ASSET_IMAGE, CW_ASSET_AUDIO, CW_ASSET_FONT,
    CW_ASSET_DATA, CW_ASSET_CONFIG, CW_ASSET_SOURCE, CW_ASSET_PREFAB, CW_ASSET_ANIMATION, CW_ASSET_MATERIAL, CW_ASSET_SHADER, CW_ASSET_TYPES
} cw_asset_type;

typedef struct {
    uint32_t id;                               /* cw_asset_id(name) */
    cw_asset_type type;
    char name[CW_ASSET_NAME_CAPACITY];         /* unique within the manifest; usually the file name */
    char path[CW_ASSET_PATH_CAPACITY];         /* relative to the manifest's directory, '/' separated */
    uint64_t size;                             /* bytes when last seen; 0 when missing */
    bool missing;                              /* the file was not found at the last refresh */
} cw_asset;

typedef struct {
    char path[CW_ASSET_PATH_CAPACITY];         /* the manifest file */
    char dir[CW_ASSET_PATH_CAPACITY];          /* its directory, with a trailing '/' */
    cw_asset *items;
    size_t count, capacity;
    bool dirty;
} cw_assets;

uint32_t cw_asset_id(const char *name);        /* FNV-1a; 0 for NULL or "" (no asset) */
cw_asset_type cw_asset_type_of(const char *filename);
const char *cw_asset_type_name(cw_asset_type type);
const char *cw_asset_type_dir(cw_asset_type type);   /* import subdirectory: "scenes", "images", ... */
/* What kind of asset a CW_SEM_ASSET field wants, by its name: material ->
 * materials; sprite, image, texture -> images; clip, animation -> clips;
 * shader -> shaders; sound, audio, music -> audio; font; prefab; scene,
 * level -> scenes. Anything else is CW_ASSET_OTHER: any asset goes. */
cw_asset_type cw_asset_type_for_field(const char *field_name);

void cw_assets_init(cw_assets *a, const char *manifest_path);
void cw_assets_destroy(cw_assets *a);
bool cw_assets_load(cw_assets *a, char *error, size_t error_capacity);   /* a missing manifest is an empty registry */
bool cw_assets_save(cw_assets *a, char *error, size_t error_capacity);
void cw_assets_refresh(cw_assets *a);          /* re-checks sizes and presence */

const cw_asset *cw_assets_find(const cw_assets *a, uint32_t id);
const cw_asset *cw_assets_find_name(const cw_assets *a, const char *name);
const cw_asset *cw_assets_find_path(const cw_assets *a, const char *relative_path);
/* Absolute path of an asset; false when the id is unknown. */
bool cw_assets_path(const cw_assets *a, uint32_t id, char *out, size_t capacity);
void cw_assets_resolve(const cw_assets *a, const char *relative_path, char *out, size_t capacity);

/* Records a file already inside the project. `name` NULL takes the file
 * name; a taken name gets a numeric suffix. Returns the entry. */
const cw_asset *cw_assets_add(cw_assets *a, const char *relative_path, const char *name, char *error, size_t error_capacity);
/* Copies `source_file` into <project>/assets/<type dir>/ (or `subdir` when
 * given, relative to the project) and records it. An existing file of the
 * same name is overwritten only with `replace`. */
const cw_asset *cw_assets_import(cw_assets *a, const char *source_file, const char *subdir, bool replace,
                                 char *error, size_t error_capacity);
/* Records every file under `relative_dir` (recursively) that is not yet
 * registered; returns how many were added. */
size_t cw_assets_scan(cw_assets *a, const char *relative_dir);
bool cw_assets_rename(cw_assets *a, uint32_t id, const char *name);   /* changes the id */
bool cw_assets_remove(cw_assets *a, uint32_t id, bool delete_file);

/* The registry game code resolves against: cw_assets_of(world) in
 * cw_game.h reaches the one the host attached to the world (a module loaded
 * as a shared library has its own copy of the engine, so a process-global
 * would not reach it). cw_assets_current is the host-side global the scene
 * format uses to write ids as @name. */
void cw_assets_set_current(const cw_assets *a);
const cw_assets *cw_assets_current(void);

#ifdef __cplusplus
}
#endif
#endif
