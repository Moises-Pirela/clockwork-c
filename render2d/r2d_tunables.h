/* r2d_tunables: data-driven parameters backed by a plain-text config file.
 *
 * A tunable set binds a table of parameter definitions (section, key, type,
 * range, default, help) to the fields of a C struct. The set can then be
 * filled from INI-style text or a file, written back out with the same
 * layout (comments carry the help text and ranges), inspected and adjusted
 * generically (which is what a tweak UI needs), and polled for on-disk
 * changes to hot-reload while the program runs.
 *
 * File format:
 *   # comment            (also ;)
 *   [section]
 *   key = value          value: number, true/false/on/off/yes/no, an enum
 *                        choice name, or free text for string tunables
 * Unknown sections and keys are reported, not fatal. The module has no
 * dependency on the renderer.
 */
#ifndef R2D_TUNABLES_H
#define R2D_TUNABLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { R2D_TUNE_FLOAT, R2D_TUNE_INT, R2D_TUNE_BOOL, R2D_TUNE_ENUM, R2D_TUNE_STRING } r2d_tune_type;

typedef struct {
    const char *section, *key;
    r2d_tune_type type;
    size_t offset;             /* of the field inside the target struct */
    float def, min, max, step; /* float/int: default, range and UI step; bool: def */
    const char *choices;       /* enum: "name0|name1|..." stored as int index; string: unused */
    size_t capacity;           /* string: bytes available at offset */
    const char *help;          /* written as a comment when saving */
} r2d_tunable;

typedef struct {
    const r2d_tunable *defs;
    size_t count;
    void *target;
} r2d_tunable_set;

/* Reset every field to its default (strings become empty). */
void r2d_tunables_reset(const r2d_tunable_set *set);
/* Apply `key = value` lines; returns the number of values applied. Problems
 * (unknown keys, bad values) are summarised in `error` when given; the
 * function still applies everything it understands. */
size_t r2d_tunables_apply_text(const r2d_tunable_set *set, const char *text, size_t length, char *error, size_t error_capacity);
bool r2d_tunables_load(const r2d_tunable_set *set, const char *path, char *error, size_t error_capacity);
/* Render the whole set as config text (with header and help comments). */
size_t r2d_tunables_format(const r2d_tunable_set *set, const char *header, char *out, size_t capacity);
bool r2d_tunables_save(const r2d_tunable_set *set, const char *path, const char *header);

/* Generic access. */
const r2d_tunable *r2d_tunables_find(const r2d_tunable_set *set, const char *section, const char *key);
bool r2d_tunables_set(const r2d_tunable_set *set, const r2d_tunable *def, const char *value);
size_t r2d_tunables_get(const r2d_tunable_set *set, const r2d_tunable *def, char *out, size_t capacity);
float r2d_tunables_get_number(const r2d_tunable_set *set, const r2d_tunable *def);   /* float/int/bool/enum */
void r2d_tunables_set_number(const r2d_tunable_set *set, const r2d_tunable *def, float value);   /* clamped */
void r2d_tunables_adjust(const r2d_tunable_set *set, const r2d_tunable *def, int direction);  /* +-step, enums cycle, bools toggle */
const char *r2d_tunables_choice_name(const r2d_tunable *def, int index, char *out, size_t capacity);
int r2d_tunables_choice_count(const r2d_tunable *def);

/* File watching: returns true (and updates *stamp) when the file's
 * modification time or size differs from *stamp. Start with *stamp = 0. */
bool r2d_tunables_file_changed(const char *path, uint64_t *stamp);

#ifdef __cplusplus
}
#endif
#endif
