#define _CRT_SECURE_NO_WARNINGS 1
#include "r2d_tunables.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#define r2d_stat_struct struct _stat64
#define r2d_stat _stat64
#else
#define r2d_stat_struct struct stat
#define r2d_stat stat
#endif

static void *field(const r2d_tunable_set *set, const r2d_tunable *d) { return (char *)set->target + d->offset; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static bool equal_ci(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}
static const char *trim(const char *s, size_t *len) {
    while (*len && isspace((unsigned char)s[0])) { ++s; --*len; }
    while (*len && isspace((unsigned char)s[*len - 1])) --*len;
    return s;
}

int r2d_tunables_choice_count(const r2d_tunable *d) {
    if (!d || !d->choices || !*d->choices) return 0;
    int n = 1;
    for (const char *c = d->choices; *c; ++c) n += *c == '|';
    return n;
}
const char *r2d_tunables_choice_name(const r2d_tunable *d, int index, char *out, size_t capacity) {
    if (!out || !capacity) return "";
    out[0] = 0;
    if (!d || !d->choices || index < 0) return out;
    const char *c = d->choices;
    for (int i = 0; i < index; ++i) {
        c = strchr(c, '|');
        if (!c) return out;
        ++c;
    }
    const char *end = strchr(c, '|');
    size_t len = end ? (size_t)(end - c) : strlen(c);
    if (len >= capacity) len = capacity - 1;
    memcpy(out, c, len);
    out[len] = 0;
    return out;
}
static int choice_index(const r2d_tunable *d, const char *name, size_t len) {
    int n = r2d_tunables_choice_count(d);
    char buffer[64];
    for (int i = 0; i < n; ++i) {
        r2d_tunables_choice_name(d, i, buffer, sizeof(buffer));
        if (strlen(buffer) == len && strncmp(buffer, name, len) == 0) return i;
        /* case-insensitive fallback */
        bool same = strlen(buffer) == len;
        for (size_t k = 0; same && k < len; ++k) same = tolower((unsigned char)buffer[k]) == tolower((unsigned char)name[k]);
        if (same) return i;
    }
    return -1;
}

void r2d_tunables_reset(const r2d_tunable_set *set) {
    if (!set || !set->target) return;
    for (size_t i = 0; i < set->count; ++i) {
        const r2d_tunable *d = &set->defs[i];
        void *f = field(set, d);
        switch (d->type) {
            case R2D_TUNE_FLOAT: *(float *)f = d->def; break;
            case R2D_TUNE_INT: *(int *)f = (int)d->def; break;
            case R2D_TUNE_BOOL: *(bool *)f = d->def != 0; break;
            case R2D_TUNE_ENUM: *(int *)f = (int)d->def; break;
            case R2D_TUNE_STRING: if (d->capacity) ((char *)f)[0] = 0; break;
        }
    }
}
const r2d_tunable *r2d_tunables_find(const r2d_tunable_set *set, const char *section, const char *key) {
    if (!set || !section || !key) return NULL;
    for (size_t i = 0; i < set->count; ++i)
        if (equal_ci(set->defs[i].section, section) && equal_ci(set->defs[i].key, key)) return &set->defs[i];
    return NULL;
}
static bool set_from_span(const r2d_tunable_set *set, const r2d_tunable *d, const char *value, size_t len) {
    void *f = field(set, d);
    char buffer[128];
    if (len >= sizeof(buffer)) len = sizeof(buffer) - 1;
    memcpy(buffer, value, len);
    buffer[len] = 0;
    switch (d->type) {
        case R2D_TUNE_FLOAT:
        case R2D_TUNE_INT: {
            char *end;
            float v = strtof(buffer, &end);
            if (end == buffer || *end) return false;
            if (d->min < d->max) v = clampf(v, d->min, d->max);
            if (d->type == R2D_TUNE_FLOAT) *(float *)f = v; else *(int *)f = (int)floorf(v + 0.5f);
            return true;
        }
        case R2D_TUNE_BOOL:
            if (equal_ci(buffer, "true") || equal_ci(buffer, "on") || equal_ci(buffer, "yes") || equal_ci(buffer, "1")) { *(bool *)f = true; return true; }
            if (equal_ci(buffer, "false") || equal_ci(buffer, "off") || equal_ci(buffer, "no") || equal_ci(buffer, "0")) { *(bool *)f = false; return true; }
            return false;
        case R2D_TUNE_ENUM: {
            int index = choice_index(d, buffer, len);
            if (index < 0) {
                char *end;
                long n = strtol(buffer, &end, 10);
                if (end == buffer || *end || n < 0 || n >= r2d_tunables_choice_count(d)) return false;
                index = (int)n;
            }
            *(int *)f = index;
            return true;
        }
        case R2D_TUNE_STRING:
            if (!d->capacity) return false;
            snprintf((char *)f, d->capacity, "%s", buffer);
            return true;
    }
    return false;
}
bool r2d_tunables_set(const r2d_tunable_set *set, const r2d_tunable *d, const char *value) {
    if (!set || !d || !value) return false;
    size_t len = strlen(value);
    const char *v = trim(value, &len);
    return set_from_span(set, d, v, len);
}
size_t r2d_tunables_get(const r2d_tunable_set *set, const r2d_tunable *d, char *out, size_t capacity) {
    if (!out || !capacity) return 0;
    out[0] = 0;
    if (!set || !d) return 0;
    const void *f = field(set, d);
    int n = 0;
    switch (d->type) {
        case R2D_TUNE_FLOAT: {
            float v = *(const float *)f;
            n = snprintf(out, capacity, fabsf(v - floorf(v)) < 1e-6f && fabsf(v) < 1e6f ? "%.1f" : "%.4g", (double)v);
            break;
        }
        case R2D_TUNE_INT: n = snprintf(out, capacity, "%d", *(const int *)f); break;
        case R2D_TUNE_BOOL: n = snprintf(out, capacity, "%s", *(const bool *)f ? "true" : "false"); break;
        case R2D_TUNE_ENUM: r2d_tunables_choice_name(d, *(const int *)f, out, capacity); n = (int)strlen(out); break;
        case R2D_TUNE_STRING: n = snprintf(out, capacity, "%s", d->capacity ? (const char *)f : ""); break;
    }
    return n < 0 ? 0 : (size_t)n;
}
float r2d_tunables_get_number(const r2d_tunable_set *set, const r2d_tunable *d) {
    if (!set || !d) return 0;
    const void *f = field(set, d);
    switch (d->type) {
        case R2D_TUNE_FLOAT: return *(const float *)f;
        case R2D_TUNE_INT: case R2D_TUNE_ENUM: return (float)*(const int *)f;
        case R2D_TUNE_BOOL: return *(const bool *)f ? 1.0f : 0.0f;
        default: return 0;
    }
}
void r2d_tunables_set_number(const r2d_tunable_set *set, const r2d_tunable *d, float value) {
    if (!set || !d) return;
    void *f = field(set, d);
    switch (d->type) {
        case R2D_TUNE_FLOAT: *(float *)f = d->min < d->max ? clampf(value, d->min, d->max) : value; break;
        case R2D_TUNE_INT: *(int *)f = (int)floorf((d->min < d->max ? clampf(value, d->min, d->max) : value) + 0.5f); break;
        case R2D_TUNE_BOOL: *(bool *)f = value != 0; break;
        case R2D_TUNE_ENUM: {
            int n = r2d_tunables_choice_count(d), v = (int)floorf(value + 0.5f);
            if (n > 0) { v %= n; if (v < 0) v += n; }
            *(int *)f = v;
            break;
        }
        default: break;
    }
}
void r2d_tunables_adjust(const r2d_tunable_set *set, const r2d_tunable *d, int direction) {
    if (!set || !d || !direction) return;
    float current = r2d_tunables_get_number(set, d);
    switch (d->type) {
        case R2D_TUNE_FLOAT: r2d_tunables_set_number(set, d, current + (d->step > 0 ? d->step : 0.1f) * (float)direction); break;
        case R2D_TUNE_INT: r2d_tunables_set_number(set, d, current + (d->step > 0 ? d->step : 1.0f) * (float)direction); break;
        case R2D_TUNE_BOOL: r2d_tunables_set_number(set, d, current != 0 ? 0.0f : 1.0f); break;
        case R2D_TUNE_ENUM: r2d_tunables_set_number(set, d, current + (float)(direction > 0 ? 1 : -1)); break;
        default: break;
    }
}

static void note(char *error, size_t capacity, size_t *used, const char *fmt, const char *a, const char *b) {
    if (!error || !capacity) return;
    if (*used >= capacity - 1) return;
    int n = snprintf(error + *used, capacity - *used, fmt, a, b);
    if (n > 0) *used += (size_t)n < capacity - *used ? (size_t)n : capacity - *used - 1;
}
size_t r2d_tunables_apply_text(const r2d_tunable_set *set, const char *text, size_t length, char *error, size_t error_capacity) {
    if (error && error_capacity) error[0] = 0;
    if (!set || !text) return 0;
    size_t applied = 0, used = 0;
    char section[64] = "";
    const char *p = text, *end = text + length;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char *line = trim(p, &len);
        p = nl ? nl + 1 : end;
        if (!len || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            const char *close = memchr(line, ']', len);
            size_t n = close ? (size_t)(close - line - 1) : len - 1;
            if (n >= sizeof(section)) n = sizeof(section) - 1;
            memcpy(section, line + 1, n);
            section[n] = 0;
            continue;
        }
        const char *eq = memchr(line, '=', len);
        if (!eq) { char spec[80]; snprintf(spec, sizeof(spec), "%.*s", (int)(len > 60 ? 60 : len), line); note(error, error_capacity, &used, "bad line '%s'%s; ", spec, ""); continue; }
        size_t klen = (size_t)(eq - line), vlen = len - klen - 1;
        for (size_t k = 0; k < vlen; ++k)   /* inline comments */
            if (eq[1 + k] == '#' || eq[1 + k] == ';') { vlen = k; break; }
        const char *key = trim(line, &klen), *value = trim(eq + 1, &vlen);
        char kbuf[64];
        snprintf(kbuf, sizeof(kbuf), "%.*s", (int)(klen > 63 ? 63 : klen), key);
        const r2d_tunable *d = r2d_tunables_find(set, section, kbuf);
        if (!d) { note(error, error_capacity, &used, "unknown %s.%s; ", section, kbuf); continue; }
        if (!set_from_span(set, d, value, vlen)) { note(error, error_capacity, &used, "bad value for %s.%s; ", section, kbuf); continue; }
        ++applied;
    }
    if (used > 2 && error) error[used - 2] = 0;   /* drop the trailing "; " */
    return applied;
}
bool r2d_tunables_load(const r2d_tunable_set *set, const char *path, char *error, size_t error_capacity) {
    if (error && error_capacity) error[0] = 0;
    if (!set || !path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) { if (error) snprintf(error, error_capacity, "cannot open %s", path); return false; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > (1 << 20)) { fclose(f); if (error) snprintf(error, error_capacity, "%s is not a readable config", path); return false; }
    char *text = malloc((size_t)size + 1);
    if (!text) { fclose(f); return false; }
    size_t got = fread(text, 1, (size_t)size, f);
    fclose(f);
    r2d_tunables_apply_text(set, text, got, error, error_capacity);
    free(text);
    return true;
}
size_t r2d_tunables_format(const r2d_tunable_set *set, const char *header, char *out, size_t capacity) {
    if (!out || !capacity) return 0;
    out[0] = 0;
    if (!set) return 0;
    size_t used = 0;
#define EMIT(...) do { if (used < capacity) { int n_ = snprintf(out + used, capacity - used, __VA_ARGS__); if (n_ > 0) used += (size_t)n_; } } while (0)
    if (header && *header) EMIT("# %s\n", header);
    const char *section = NULL;
    char value[128], range[96];
    for (size_t i = 0; i < set->count; ++i) {
        const r2d_tunable *d = &set->defs[i];
        if (!section || strcmp(section, d->section) != 0) {
            section = d->section;
            EMIT("\n[%s]\n", section);
        }
        r2d_tunables_get(set, d, value, sizeof(value));
        range[0] = 0;
        if (d->type == R2D_TUNE_FLOAT || d->type == R2D_TUNE_INT) {
            if (d->min < d->max) snprintf(range, sizeof(range), " [%g .. %g]", (double)d->min, (double)d->max);
        } else if (d->type == R2D_TUNE_ENUM) {
            snprintf(range, sizeof(range), " [%s]", d->choices ? d->choices : "");
        } else if (d->type == R2D_TUNE_BOOL) {
            snprintf(range, sizeof(range), " [true/false]");
        }
        if (d->help || range[0]) EMIT("# %s%s\n", d->help ? d->help : "", range);
        EMIT("%s = %s\n", d->key, value);
    }
#undef EMIT
    return used < capacity ? used : capacity - 1;
}
bool r2d_tunables_save(const r2d_tunable_set *set, const char *path, const char *header) {
    if (!set || !path) return false;
    size_t capacity = 64 * 1024;
    char *text = malloc(capacity);
    if (!text) return false;
    size_t n = r2d_tunables_format(set, header, text, capacity);
    FILE *f = fopen(path, "wb");
    bool ok = f != NULL && fwrite(text, 1, n, f) == n;
    if (f) ok = fclose(f) == 0 && ok;
    free(text);
    return ok;
}
bool r2d_tunables_file_changed(const char *path, uint64_t *stamp) {
    if (!path || !stamp) return false;
    r2d_stat_struct st;
    if (r2d_stat(path, &st) != 0) return false;
    uint64_t now = ((uint64_t)st.st_mtime << 20) ^ (uint64_t)st.st_size;
    if (now == *stamp) return false;
    *stamp = now;
    return true;
}
