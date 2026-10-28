#include "cw_scene.h"
#include "cw_internal.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static bool fail(cw_scene_result *r, int line, const char *fmt, ...) {
    if (r) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(r->message, sizeof(r->message), fmt, args);
        va_end(args);
        r->ok = false;
        r->line = line;
    }
    return false;
}
static void result_clear(cw_scene_result *r) {
    if (!r) return;
    memset(r, 0, sizeof(*r));
    r->ok = true;
}

/* --- writing --- */
static void write_text(cw_writer *out, const char *s) { cw_writer_bytes(out, s, strlen(s)); }
static void write_quoted(cw_writer *out, const char *s) {
    cw_writer_u8(out, '"');
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { cw_writer_u8(out, '\\'); cw_writer_u8(out, c); }
        else if (c < 0x20 || c == 0x7f) cw_writer_u8(out, ' ');
        else cw_writer_u8(out, c);
    }
    cw_writer_u8(out, '"');
}
static bool write_impl(cw_world *w, const cw_entity_id *ids, size_t id_count, const cw_scene_save_config *config,
                       cw_writer *out, cw_scene_result *result) {
    result_clear(result);
    if (!w || !out) return fail(result, 0, "no world or writer");
    cw_scene_save_config cfg = {0};
    if (config) cfg = *config;
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "cwscene %d\n", CW_SCENE_FORMAT_VERSION);
    write_text(out, buffer);
    if (cfg.game && *cfg.game) {
        snprintf(buffer, sizeof(buffer), "game %s %" PRIu32 "\n", cfg.game, cfg.scene_version);
        write_text(out, buffer);
    }
    if (!ids) {
        snprintf(buffer, sizeof(buffer), "tick %" PRIu64 "\nnext_id %" PRId64 "\n", w->tick, w->next_id);
        write_text(out, buffer);
    }
    size_t total = ids ? id_count : w->capacity;
    for (size_t i = 0; i < total; ++i) {
        cw_entity_id id = ids ? ids[i] : (cw_entity_id)i;
        if (!cw_world_is_valid(w, id)) { if (ids) return fail(result, 0, "entity %" PRId64 " is not alive", id); continue; }
        const cw_entity *e = &w->entities[id];
        if (!cw_query_visible(*e)) continue;
        snprintf(buffer, sizeof(buffer), "\nentity %" PRId64, id);
        write_text(out, buffer);
        char name[CW_SCENE_NAME_CAPACITY];
        if (cfg.name_of && cfg.name_of(cfg.user, id, name, sizeof(name)) && name[0]) {
            cw_writer_u8(out, ' ');
            write_quoted(out, name);
        }
        cw_writer_u8(out, '\n');
        if (result) ++result->entities;
        for (unsigned c = 0; c < CW_MAX_COMPONENTS; ++c) {
            cw_component_id ct = (cw_component_id)c;
            if (!cw_comp_has(w, id, ct) || cw_comp_is_transient(w, ct)) continue;
            const cw_type_info *t = cw_comp_type(w, ct);
            if (!t) return fail(result, 0, "component %s on entity %" PRId64 " has no reflection", cw_comp_name(w, ct), id);
            if (t->flags & CW_TYPE_RUNTIME) continue;   /* generated from config at run time, never authored */
            write_text(out, "  ");
            write_text(out, t->name);
            const void *value = cw_comp_ptr(w, id, ct);
            for (size_t f = 0; f < t->field_count; ++f) {
                char text[160];
                cw_field_format(&t->fields[f], value, text, sizeof(text));
                write_text(out, " ");
                write_text(out, t->fields[f].name);
                write_text(out, "=");
                if (t->fields[f].type == CW_FIELD_STRING) write_quoted(out, text);   /* text="hello world" */
                else write_text(out, text);
            }
            cw_writer_u8(out, '\n');
            if (result) ++result->components;
        }
    }
    return cw_writer_ok(out) || fail(result, 0, "writer overflow");
}
bool cw_scene_write(cw_world *w, const cw_scene_save_config *config, cw_writer *out, cw_scene_result *result) {
    return write_impl(w, NULL, 0, config, out, result);
}
bool cw_scene_write_entities(cw_world *w, const cw_entity_id *ids, size_t count, const cw_scene_save_config *config,
                             cw_writer *out, cw_scene_result *result) {
    if (!ids) { result_clear(result); return fail(result, 0, "no entities"); }
    return write_impl(w, ids, count, config, out, result);
}

/* --- reading --- */
typedef struct {
    const char *data;
    size_t size, offset;
    int line;
} scanner;
/* Next line without its terminator; false at the end. */
static bool next_line(scanner *s, const char **start, size_t *length) {
    if (s->offset >= s->size) return false;
    const char *p = s->data + s->offset;
    size_t remaining = s->size - s->offset;
    const char *nl = memchr(p, '\n', remaining);
    size_t len = nl ? (size_t)(nl - p) : remaining;
    s->offset += len + (nl ? 1 : 0);
    ++s->line;
    if (len && p[len - 1] == '\r') --len;
    *start = p;
    *length = len;
    return true;
}
enum { MAX_TOKENS = 64, TOKEN_CAPACITY = 224 };
typedef struct { char text[TOKEN_CAPACITY]; bool quoted; } token;
/* Splits a line into whitespace-separated tokens; a "..." token keeps
 * spaces and understands \" and \\. A '#' outside quotes starts a comment. */
static int tokenize(const char *p, size_t len, token *out, int capacity, bool *too_long) {
    int n = 0;
    size_t i = 0;
    *too_long = false;
    while (i < len) {
        while (i < len && isspace((unsigned char)p[i])) ++i;
        if (i >= len || p[i] == '#') break;
        if (n == capacity) return -1;
        token *t = &out[n++];
        size_t k = 0;
        t->quoted = p[i] == '"';
        if (t->quoted) {
            ++i;
            while (i < len && p[i] != '"') {
                char c = p[i++];
                if (c == '\\' && i < len) c = p[i++];
                if (k + 1 < TOKEN_CAPACITY) t->text[k++] = c; else *too_long = true;
            }
            if (i >= len) return -2;   /* unterminated */
            ++i;
        } else {
            while (i < len && !isspace((unsigned char)p[i])) {
                if (p[i] == '"') {   /* key="a value with spaces": the quoted part keeps its spaces */
                    ++i;
                    while (i < len && p[i] != '"') {
                        char c = p[i++];
                        if (c == '\\' && i < len) c = p[i++];
                        if (k + 1 < TOKEN_CAPACITY) t->text[k++] = c; else *too_long = true;
                    }
                    if (i >= len) return -2;
                    ++i;
                    continue;
                }
                if (k + 1 < TOKEN_CAPACITY) t->text[k++] = p[i]; else *too_long = true;
                ++i;
            }
        }
        t->text[k] = 0;
    }
    return n;
}
static bool parse_i64(const char *text, int64_t *out) {
    char *end;
    errno = 0;
    long long v = strtoll(text, &end, 10);
    if (end == text || *end || errno == ERANGE) return false;
    *out = v;
    return true;
}
static bool world_fresh(const cw_world *w) {
    return cw_world_entity_count(w) == 0 && w->next_id == 0 && w->recycled_ids.count == 0 && w->pending_destroy.count == 0;
}
static void warn(cw_scene_result *r, int line) {
    if (!r) return;
    ++r->warnings;
    r->line = line;
}
typedef struct { cw_entity_id from, to; } id_pair;
typedef struct { const id_pair *pairs; size_t count; } id_pairs;
static cw_entity_id map_pair(cw_entity_id ref, void *user) {
    const id_pairs *m = user;
    for (size_t i = 0; i < m->count; ++i) if (m->pairs[i].from == ref) return m->pairs[i].to;
    return ref;
}
static bool read_impl(cw_world *w, const char *text, size_t size, const cw_scene_load_config *config, cw_scene_result *result,
                      bool instantiate, bool place, float px, float py, cw_entity_id *out, size_t out_capacity, size_t *out_count) {
    result_clear(result);
    if (out_count) *out_count = 0;
    if (!w || (!text && size)) return fail(result, 0, "no world or text");
    if (!instantiate && !world_fresh(w)) return fail(result, 0, "the world is not fresh");
    cw_scene_load_config cfg = {0};
    if (config) cfg = *config;
    id_pair *pairs = NULL;
    size_t pair_capacity = 0;
    size_t max_entities = cfg.max_entities ? cfg.max_entities : CW_SNAPSHOT_DEFAULT_MAX_ENTITIES;
    cw_entity_id max_id = cfg.max_entity_id ? cfg.max_entity_id : CW_SNAPSHOT_DEFAULT_MAX_ENTITY_ID;

    scanner s = {text, size, 0, 0};
    const char *line;
    size_t len;
    bool header = false, ok = true;
    int64_t tick = 0, next_id = 0;
    cw_entity_id current = CW_INVALID_ENTITY;
    size_t entities = 0;
    unsigned char local[1024];
    void *value = local;
    size_t value_capacity = sizeof(local);
    token tokens[MAX_TOKENS];

    while (ok && next_line(&s, &line, &len)) {
        bool too_long;
        int n = tokenize(line, len, tokens, MAX_TOKENS, &too_long);
        if (n == -1) { ok = fail(result, s.line, "too many values on one line"); break; }
        if (n == -2) { ok = fail(result, s.line, "unterminated string"); break; }
        if (too_long) { ok = fail(result, s.line, "value too long"); break; }
        if (n == 0) continue;
        const char *key = tokens[0].text;
        bool indented = isspace((unsigned char)line[0]);
        if (!header) {
            if (strcmp(key, "cwscene") != 0 || n != 2) { ok = fail(result, s.line, "not a scene file (expected 'cwscene 1')"); break; }
            int64_t v;
            if (!parse_i64(tokens[1].text, &v) || v != CW_SCENE_FORMAT_VERSION) { ok = fail(result, s.line, "unsupported scene version %s", tokens[1].text); break; }
            header = true;
            continue;
        }
        if (!indented && strcmp(key, "game") == 0) {
            if (n < 2) { ok = fail(result, s.line, "'game' needs a name"); break; }
            if (cfg.game && strcmp(cfg.game, tokens[1].text) != 0) { ok = fail(result, s.line, "scene is for game '%s', not '%s'", tokens[1].text, cfg.game); break; }
            continue;
        }
        if (!indented && strcmp(key, "tick") == 0) {
            if (n != 2 || !parse_i64(tokens[1].text, &tick) || tick < 0) { ok = fail(result, s.line, "bad tick"); break; }
            continue;
        }
        if (!indented && strcmp(key, "next_id") == 0) {
            if (n != 2 || !parse_i64(tokens[1].text, &next_id) || next_id < 0 || next_id > max_id + 1) { ok = fail(result, s.line, "bad next_id"); break; }
            continue;
        }
        if (!indented && strcmp(key, "entity") == 0) {
            int64_t id;
            if (n < 2 || n > 3 || !parse_i64(tokens[1].text, &id)) { ok = fail(result, s.line, "bad entity line"); break; }
            if (id < 0 || id > max_id) { ok = fail(result, s.line, "entity id %" PRId64 " out of range", id); break; }
            if (entities >= max_entities) { ok = fail(result, s.line, "too many entities"); break; }
            if (instantiate) {
                if (entities == pair_capacity) {
                    pair_capacity = pair_capacity ? pair_capacity * 2 : 16;
                    pairs = cw_resize(pairs, pair_capacity, sizeof(*pairs));
                }
                for (size_t k = 0; k < entities; ++k)
                    if (pairs[k].from == id) { ok = fail(result, s.line, "entity %" PRId64 " appears twice", id); break; }
                if (!ok) break;
                cw_entity_id fresh = cw_world_create_entity(w);
                pairs[entities].from = id;
                pairs[entities].to = fresh;
                if (out && entities < out_capacity) out[entities] = fresh;
                id = fresh;
            } else if (!cw_world_restore_entity(w, id)) { ok = fail(result, s.line, "entity %" PRId64 " appears twice", id); break; }
            ++entities;
            current = id;
            if (n == 3 && cfg.on_name && tokens[2].text[0]) cfg.on_name(cfg.user, id, tokens[2].text);
            continue;
        }
        if (!indented) { ok = fail(result, s.line, "unknown directive '%s'", key); break; }
        /* a component line */
        if (current == CW_INVALID_ENTITY) { ok = fail(result, s.line, "component before any entity"); break; }
        cw_component_id ct;
        if (!cw_comp_by_name(w, key, &ct)) {
            if (cfg.ignore_unknown_components) { warn(result, s.line); continue; }
            ok = fail(result, s.line, "unknown component '%s'", key);
            break;
        }
        const cw_type_info *t = cw_comp_type(w, ct);
        if (!t) {
            if (n != 1) { ok = fail(result, s.line, "component '%s' has no reflection but carries fields", key); break; }
            cw_comp_add(w, current, ct);
            if (result) ++result->components;
            continue;
        }
        if (t->size > value_capacity) {
            value = value == local ? malloc(t->size) : realloc(value, t->size);
            cw_require(value != NULL);
            value_capacity = t->size;
        }
        cw_type_default(t, value);
        for (int i = 1; i < n && ok; ++i) {
            char *eq = strchr(tokens[i].text, '=');
            if (!eq) { ok = fail(result, s.line, "expected field=value, got '%s'", tokens[i].text); break; }
            *eq = 0;
            const cw_field *f = cw_type_field(t, tokens[i].text);
            if (!f) { warn(result, s.line); continue; }
            if (!cw_field_parse(f, value, eq + 1)) ok = fail(result, s.line, "bad value '%s' for %s.%s", eq + 1, t->name, f->name);
        }
        if (!ok) break;
        if (!cw_type_check(t, value)) { ok = fail(result, s.line, "invalid %s on entity %" PRId64, t->name, current); break; }
        cw_comp_set(w, current, ct, value, t->size);
        if (result) ++result->components;
    }
    if (value != local) free(value);
    if (ok && !header) ok = fail(result, 0, "empty file");
    if (!ok) {
        if (instantiate) {
            for (size_t k = 0; k < entities; ++k) cw_world_mark_destroy(w, pairs[k].to);
            cw_world_sweep(w);
        } else cw_world_reset(w);
        free(pairs);
        return false;
    }
    if (instantiate) {
        id_pairs map = {pairs, entities};
        for (size_t k = 0; k < entities; ++k) cw_reflect_remap(w, pairs[k].to, map_pair, &map);
        float ax, ay;
        if (place && entities && cw_reflect_position(w, pairs[0].to, &ax, &ay)) {
            float dx = px - ax, dy = py - ay;
            for (size_t k = 0; k < entities; ++k) {
                float x, y;
                if (cw_reflect_position(w, pairs[k].to, &x, &y)) cw_reflect_set_position(w, pairs[k].to, x + dx, y + dy);
            }
        }
        if (out_count) *out_count = entities;
    } else {
        if (w->next_id < next_id) w->next_id = next_id;
        w->tick = cfg.keep_tick ? (uint64_t)tick : 0;
    }
    free(pairs);
    if (result) { result->entities = entities; result->ok = true; }
    return true;
}
bool cw_scene_read(cw_world *w, const char *text, size_t size, const cw_scene_load_config *config, cw_scene_result *result) {
    return read_impl(w, text, size, config, result, false, false, 0, 0, NULL, 0, NULL);
}
bool cw_scene_instantiate(cw_world *w, const char *text, size_t size, const cw_scene_load_config *config,
                          bool place, float x, float y, cw_entity_id *out, size_t capacity, size_t *count,
                          cw_scene_result *result) {
    return read_impl(w, text, size, config, result, true, place, x, y, out, capacity, count);
}

bool cw_scene_save_file(cw_world *w, const char *path, const cw_scene_save_config *config, cw_scene_result *result) {
    cw_writer out;
    cw_writer_init(&out);
    if (!cw_scene_write(w, config, &out, result)) { cw_writer_destroy(&out); return false; }
    cw_file_error error;
    bool ok = cw_file_write(path, out.data, out.size, &error);
    cw_writer_destroy(&out);
    if (!ok) return fail(result, 0, "cannot write %s: %s%s%s", path, cw_file_status_string(error.status),
                         error.errnum ? ": " : "", error.errnum ? strerror(error.errnum) : "");
    return true;
}
bool cw_scene_load_file(cw_world *w, const char *path, const cw_scene_load_config *config, cw_scene_result *result) {
    result_clear(result);
    cw_buffer image;
    cw_file_error error;
    if (!cw_file_read(path, CW_SCENE_DEFAULT_MAX_SIZE, &image, &error))
        return fail(result, 0, "cannot read %s: %s%s%s", path, cw_file_status_string(error.status),
                    error.errnum ? ": " : "", error.errnum ? strerror(error.errnum) : "");
    bool ok = cw_scene_read(w, (const char *)image.data, image.size, config, result);
    cw_buffer_free(&image);
    return ok;
}
