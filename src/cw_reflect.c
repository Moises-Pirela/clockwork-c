#include "cw_reflect.h"
#include "cw_assets.h"
#include "cw_internal.h"
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdio.h>

static const char *const type_names[CW_FIELD_TYPES] = {
    "bool", "u8", "i32", "u32", "f32", "entity", "enum", "flags", "string"
};
const char *cw_field_type_name(cw_field_type type) {
    return type >= 0 && type < CW_FIELD_TYPES ? type_names[type] : "?";
}
size_t cw_field_size(cw_field_type type) {
    switch (type) {
        case CW_FIELD_BOOL: return sizeof(bool);
        case CW_FIELD_U8: case CW_FIELD_ENUM: case CW_FIELD_FLAGS: return sizeof(uint8_t);
        case CW_FIELD_I32: return sizeof(int32_t);
        case CW_FIELD_U32: return sizeof(uint32_t);
        case CW_FIELD_F32: return sizeof(float);
        case CW_FIELD_ENTITY: return sizeof(cw_entity_id);
        default: return 0;
    }
}
static bool field_valid(const cw_field *f, size_t size) {
    if (!f->name || !*f->name || f->type < 0 || f->type >= CW_FIELD_TYPES) return false;
    size_t n = f->type == CW_FIELD_STRING ? f->capacity : (f->type == CW_FIELD_BYTES ? f->capacity + sizeof(uint16_t) : cw_field_size(f->type));
    if (f->type == CW_FIELD_STRING && (f->capacity < 2 || f->capacity > 4096)) return false;
    if (f->type == CW_FIELD_BYTES && (f->capacity < 1 || f->capacity > CW_BYTES_MAX)) return false;
    if (f->offset > size || size - f->offset < n) return false;
    if ((f->type == CW_FIELD_ENUM || f->type == CW_FIELD_FLAGS)
        && (!f->names || !f->name_count || f->name_count > 8u * (f->type == CW_FIELD_FLAGS ? 1u : 32u))) return false;
    if (f->type == CW_FIELD_ENUM && f->name_count > 255) return false;
    if (f->semantic == CW_SEM_ASSET && f->type != CW_FIELD_U32) return false;
    if (f->type == CW_FIELD_ENUM || f->type == CW_FIELD_FLAGS)
        for (unsigned i = 0; i < f->name_count; ++i)
            if (!f->names[i] || !*f->names[i] || strpbrk(f->names[i], " \t\r\n=|\"")) return false;
    if (strpbrk(f->name, " \t\r\n=|\"")) return false;
    return f->min <= f->max;
}
bool cw_type_valid(const cw_type_info *t) {
    if (!t || !t->name || !*t->name || strpbrk(t->name, " \t\r\n=\"")) return false;
    if (!t->alignment || (t->alignment & (t->alignment - 1))) return false;
    if (t->field_count && !t->fields) return false;
    size_t size = t->size ? t->size : 1;
    for (size_t i = 0; i < t->field_count; ++i) {
        if (!field_valid(&t->fields[i], size)) return false;
        for (size_t j = 0; j < i; ++j)
            if (strcmp(t->fields[i].name, t->fields[j].name) == 0) return false;
    }
    if ((t->flags & CW_TYPE_CONFIG) && (!t->generates || !*t->generates || !t->bake)) return false;
    if (!(t->flags & CW_TYPE_CONFIG) && (t->generates || t->bake)) return false;
    if ((t->flags & CW_TYPE_RUNTIME) && (t->flags & (CW_TYPE_CONFIG | CW_TYPE_TRANSIENT))) return false;
    return true;
}
const cw_field *cw_type_field(const cw_type_info *t, const char *name) {
    if (!t || !name) return NULL;
    for (size_t i = 0; i < t->field_count; ++i)
        if (strcmp(t->fields[i].name, name) == 0) return &t->fields[i];
    return NULL;
}
bool cw_reflect_register(cw_world *w, cw_component_id ct, const cw_type_info *t) {
    if (!cw_type_valid(t)) return false;
    if (!cw_comp_register(w, ct, t->size, t->alignment, t->name, NULL)) return false;
    cw_comp_set_type(w, ct, t);
    if (t->flags & CW_TYPE_TRANSIENT) cw_comp_set_transient(w, ct, true);
    else cw_comp_set_codec(w, ct, cw_reflect_codec());
    return true;
}

/* --- values --- */
void cw_type_default(const cw_type_info *t, void *value) {
    if (!t || !value) return;
    if (t->defaults) memcpy(value, t->defaults, t->size);
    else memset(value, 0, t->size);
}
static size_t bounded_len(const char *s, size_t capacity) { const char *e = memchr(s, 0, capacity); return e ? (size_t)(e - s) : capacity; }
static const void *at(const void *value, const cw_field *f) { return (const unsigned char *)value + f->offset; }
static void *at_mut(void *value, const cw_field *f) { return (unsigned char *)value + f->offset; }
static bool ranged(const cw_field *f) { return f->min < f->max; }
typedef CW_BYTES(1) blob_header;   /* the layout every CW_BYTES(N) shares: the size, then the data */
static uint16_t blob_size(const cw_field *f, const void *value) {
    uint16_t n = ((const blob_header *)at(value, f))->size;
    return n > f->capacity ? (uint16_t)f->capacity : n;
}
static uint8_t *blob_data(const cw_field *f, void *value) { return ((blob_header *)at_mut(value, f))->data; }
static void blob_set(const cw_field *f, void *value, const void *data, size_t n) {
    blob_header *b = at_mut(value, f);
    if (n > f->capacity) n = f->capacity;
    b->size = (uint16_t)n;
    if (n) memcpy(b->data, data, n);
    memset(b->data + n, 0, f->capacity - n);   /* deterministic bytes beyond the size */
}
static const char b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
size_t cw_base64_encode(const void *data, size_t size, char *out, size_t capacity) {
    const uint8_t *p = data;
    size_t needed = (size + 2) / 3 * 4;
    if (!out || capacity <= needed) { if (out && capacity) out[0] = 0; return 0; }
    size_t o = 0;
    for (size_t i = 0; i < size; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < size ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < size ? p[i + 2] : 0);
        out[o++] = b64_chars[(v >> 18) & 63];
        out[o++] = b64_chars[(v >> 12) & 63];
        out[o++] = i + 1 < size ? b64_chars[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < size ? b64_chars[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}
static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
size_t cw_base64_decode(const char *text, void *out, size_t capacity) {
    if (!text) return SIZE_MAX;
    uint8_t *dst = out;
    size_t len = strlen(text), o = 0;
    if (len % 4) return SIZE_MAX;
    for (size_t i = 0; i < len; i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            char c = text[i + k];
            if (c == '=' && i + 4 == len && k >= 2) { v[k] = 0; ++pad; continue; }
            if (pad) return SIZE_MAX;   /* padding only at the very end */
            v[k] = b64_value(c);
            if (v[k] < 0) return SIZE_MAX;
        }
        uint32_t bits = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 | (uint32_t)v[3];
        uint8_t bytes[3] = {(uint8_t)(bits >> 16), (uint8_t)(bits >> 8), (uint8_t)bits};
        for (int k = 0; k < 3 - pad; ++k) {
            if (o >= capacity) return SIZE_MAX;
            if (dst) dst[o] = bytes[k];
            ++o;
        }
    }
    return o;
}

double cw_field_get(const cw_field *f, const void *value) {
    if (!f || !value) return 0;
    switch (f->type) {
        case CW_FIELD_BOOL: return *(const bool *)at(value, f) ? 1 : 0;
        case CW_FIELD_U8: case CW_FIELD_ENUM: case CW_FIELD_FLAGS: return *(const uint8_t *)at(value, f);
        case CW_FIELD_I32: return *(const int32_t *)at(value, f);
        case CW_FIELD_U32: return *(const uint32_t *)at(value, f);
        case CW_FIELD_F32: return *(const float *)at(value, f);
        case CW_FIELD_ENTITY: return (double)*(const cw_entity_id *)at(value, f);
        case CW_FIELD_STRING: return (double)bounded_len((const char *)at(value, f), f->capacity);
        case CW_FIELD_BYTES: return blob_size(f, value);
        default: return 0;
    }
}
static double clamp_range(const cw_field *f, double v) {
    if (ranged(f)) v = v < f->min ? f->min : (v > f->max ? f->max : v);
    return v;
}
bool cw_field_set(const cw_field *f, void *value, double number) {
    if (!f || !value || isnan(number)) return false;
    double v = clamp_range(f, number);
    switch (f->type) {
        case CW_FIELD_BOOL: *(bool *)at_mut(value, f) = v != 0; return true;
        case CW_FIELD_U8:
            v = v < 0 ? 0 : (v > 255 ? 255 : floor(v + 0.5));
            *(uint8_t *)at_mut(value, f) = (uint8_t)v;
            return true;
        case CW_FIELD_ENUM:
            v = v < 0 ? 0 : (v > f->name_count - 1 ? f->name_count - 1 : floor(v + 0.5));
            *(uint8_t *)at_mut(value, f) = (uint8_t)v;
            return true;
        case CW_FIELD_FLAGS: {
            unsigned mask = f->name_count >= 8 ? 0xffu : (1u << f->name_count) - 1u;
            v = v < 0 ? 0 : (v > 255 ? 255 : floor(v + 0.5));
            *(uint8_t *)at_mut(value, f) = (uint8_t)((unsigned)v & mask);
            return true;
        }
        case CW_FIELD_I32:
            v = v < INT32_MIN ? INT32_MIN : (v > INT32_MAX ? INT32_MAX : floor(v + 0.5));
            *(int32_t *)at_mut(value, f) = (int32_t)v;
            return true;
        case CW_FIELD_U32:
            v = v < 0 ? 0 : (v > UINT32_MAX ? UINT32_MAX : floor(v + 0.5));
            *(uint32_t *)at_mut(value, f) = (uint32_t)v;
            return true;
        case CW_FIELD_F32:
            if (v > FLT_MAX) v = FLT_MAX;
            if (v < -FLT_MAX) v = -FLT_MAX;
            *(float *)at_mut(value, f) = (float)v;
            return true;
        case CW_FIELD_ENTITY:
            v = floor(v + 0.5);
            if (v < -1) v = -1;
            if (v > 9007199254740992.0) v = 9007199254740992.0;
            *(cw_entity_id *)at_mut(value, f) = (cw_entity_id)v;
            return true;
        default: return false;
    }
}
float cw_field_step(const cw_field *f) {
    if (!f) return 1;
    if (f->step > 0) return f->step;
    switch (f->type) {
        case CW_FIELD_F32: return cw_sem_is_angle(f->semantic) ? 0.0872665f : (ranged(f) && f->max - f->min <= 10 ? 0.1f : 1.0f);
        default: return 1;
    }
}
bool cw_field_adjust(const cw_field *f, void *value, int direction) {
    if (!f || !value || !direction) return false;
    switch (f->type) {
        case CW_FIELD_BOOL: *(bool *)at_mut(value, f) = !*(bool *)at_mut(value, f); return true;
        case CW_FIELD_ENUM: {
            uint8_t *v = at_mut(value, f);
            unsigned n = f->name_count;
            *v = (uint8_t)((*v + (direction > 0 ? 1u : n - 1u)) % n);
            return true;
        }
        case CW_FIELD_FLAGS: {
            unsigned bit = direction > 0 ? (unsigned)direction - 1u : 0u;
            if (bit >= f->name_count) return false;
            *(uint8_t *)at_mut(value, f) ^= (uint8_t)(1u << bit);
            return true;
        }
        default:
            return cw_field_set(f, value, cw_field_get(f, value) + (double)cw_field_step(f) * (direction > 0 ? 1 : -1));
    }
}
bool cw_field_check(const cw_field *f, const void *value) {
    if (!f || !value) return false;
    double v = cw_field_get(f, value);
    switch (f->type) {
        case CW_FIELD_F32: if (!isfinite(*(const float *)at(value, f))) return false; break;
        case CW_FIELD_ENUM: if (v >= f->name_count) return false; break;
        case CW_FIELD_FLAGS:
            if (f->name_count < 8 && ((unsigned)v >> f->name_count)) return false;
            break;
        case CW_FIELD_ENTITY: if (v < -1) return false; break;
        case CW_FIELD_STRING: return memchr(at(value, f), 0, f->capacity) != NULL;   /* terminated */
        case CW_FIELD_BYTES: return ((const blob_header *)at(value, f))->size <= f->capacity;
        default: break;
    }
    return !ranged(f) || (v >= f->min && v <= f->max);
}
bool cw_type_check(const cw_type_info *t, const void *value) {
    if (!t || !value) return false;
    for (size_t i = 0; i < t->field_count; ++i)
        if (!cw_field_check(&t->fields[i], value)) return false;
    return !t->validate || t->validate(value);
}
size_t cw_field_format(const cw_field *f, const void *value, char *out, size_t capacity) {
    if (!out || !capacity) return 0;
    out[0] = 0;
    if (!f || !value) return 0;
    int n = 0;
    switch (f->type) {
        case CW_FIELD_BOOL: n = snprintf(out, capacity, "%s", *(const bool *)at(value, f) ? "true" : "false"); break;
        case CW_FIELD_U8: n = snprintf(out, capacity, "%u", *(const uint8_t *)at(value, f)); break;
        case CW_FIELD_I32: n = snprintf(out, capacity, "%d", *(const int32_t *)at(value, f)); break;
        case CW_FIELD_U32: {
            uint32_t v = *(const uint32_t *)at(value, f);
            const cw_asset *asset = f->semantic == CW_SEM_ASSET ? cw_assets_find(cw_assets_current(), v) : NULL;
            if (asset) n = snprintf(out, capacity, "@%s", asset->name);
            else if (f->semantic == CW_SEM_ASSET && v == 0) n = snprintf(out, capacity, "none");
            else n = snprintf(out, capacity, "%u", v);
            break;
        }
        case CW_FIELD_F32: n = snprintf(out, capacity, "%.9g", (double)*(const float *)at(value, f)); break;
        case CW_FIELD_ENTITY: n = snprintf(out, capacity, "%lld", (long long)*(const cw_entity_id *)at(value, f)); break;
        case CW_FIELD_STRING: n = snprintf(out, capacity, "%.*s", (int)f->capacity, (const char *)at(value, f)); break;
        case CW_FIELD_BYTES: n = (int)cw_base64_encode(((const blob_header *)at(value, f))->data, blob_size(f, value), out, capacity); break;
        case CW_FIELD_ENUM: {
            unsigned v = *(const uint8_t *)at(value, f);
            n = v < f->name_count ? snprintf(out, capacity, "%s", f->names[v]) : snprintf(out, capacity, "%u", v);
            break;
        }
        case CW_FIELD_FLAGS: {
            unsigned v = *(const uint8_t *)at(value, f);
            size_t len = 0;
            if (!v) { n = snprintf(out, capacity, "none"); break; }
            for (unsigned i = 0; i < 8 && len < capacity; ++i) {
                if (!(v & (1u << i))) continue;
                int k = i < f->name_count ? snprintf(out + len, capacity - len, "%s%s", len ? "|" : "", f->names[i])
                                          : snprintf(out + len, capacity - len, "%sbit%u", len ? "|" : "", i);
                if (k < 0) break;
                len += (size_t)k;
            }
            n = (int)len;
            break;
        }
        default: break;
    }
    if (n < 0) { out[0] = 0; return 0; }
    return (size_t)n < capacity ? (size_t)n : capacity - 1;
}
static bool parse_number(const char *text, double *out) {
    char *end;
    errno = 0;
    double v = strtod(text, &end);
    if (end == text || *end || errno == ERANGE || !isfinite(v)) return false;
    *out = v;
    return true;
}
bool cw_field_parse(const cw_field *f, void *value, const char *text) {
    if (!f || !value || !text) return false;
    double v;
    switch (f->type) {
        case CW_FIELD_STRING: {   /* cut to what fits */
            char *dst = at_mut(value, f);
            size_t n = strlen(text);
            if (n >= f->capacity) n = f->capacity - 1;
            memcpy(dst, text, n);
            dst[n] = 0;
            return true;
        }
        case CW_FIELD_BYTES: {
            uint8_t bytes[CW_BYTES_MAX];
            size_t n = cw_base64_decode(text, bytes, f->capacity);
            if (n == SIZE_MAX) return false;
            blob_set(f, value, bytes, n);
            return true;
        }
        case CW_FIELD_BOOL:
            if (strcmp(text, "true") == 0 || strcmp(text, "1") == 0) { *(bool *)at_mut(value, f) = true; return true; }
            if (strcmp(text, "false") == 0 || strcmp(text, "0") == 0) { *(bool *)at_mut(value, f) = false; return true; }
            return false;
        case CW_FIELD_ENUM:
            for (unsigned i = 0; i < f->name_count; ++i)
                if (strcmp(f->names[i], text) == 0) { *(uint8_t *)at_mut(value, f) = (uint8_t)i; return true; }
            if (!parse_number(text, &v) || v < 0 || v >= f->name_count || v != floor(v)) return false;
            *(uint8_t *)at_mut(value, f) = (uint8_t)v;
            return true;
        case CW_FIELD_FLAGS: {
            if (strcmp(text, "none") == 0) { *(uint8_t *)at_mut(value, f) = 0; return true; }
            if (parse_number(text, &v)) {
                unsigned mask = f->name_count >= 8 ? 0xffu : (1u << f->name_count) - 1u;
                if (v < 0 || v > 255 || v != floor(v) || ((unsigned)v & ~mask)) return false;
                *(uint8_t *)at_mut(value, f) = (uint8_t)v;
                return true;
            }
            unsigned bits = 0;
            const char *p = text;
            while (*p) {
                const char *end = strchr(p, '|');
                size_t len = end ? (size_t)(end - p) : strlen(p);
                bool found = false;
                for (unsigned i = 0; i < f->name_count && !found; ++i)
                    if (strlen(f->names[i]) == len && memcmp(f->names[i], p, len) == 0) { bits |= 1u << i; found = true; }
                if (!found) return false;
                p += len + (end ? 1 : 0);
            }
            *(uint8_t *)at_mut(value, f) = (uint8_t)bits;
            return true;
        }
        case CW_FIELD_U32:
            if (f->semantic == CW_SEM_ASSET) {
                if (text[0] == '@') { *(uint32_t *)at_mut(value, f) = cw_asset_id(text + 1); return true; }
                if (strcmp(text, "none") == 0) { *(uint32_t *)at_mut(value, f) = 0; return true; }
            }
            /* fall through */
        default:
            if (!parse_number(text, &v)) return false;
            if (f->type != CW_FIELD_F32 && v != floor(v)) return false;
            if (ranged(f) && (v < f->min || v > f->max)) return false;
            return cw_field_set(f, value, v);
    }
}

/* --- generic codec --- */
bool cw_reflect_marshal(cw_world *w, cw_entity_id id, cw_component_id ct, void *data, size_t capacity, size_t *size) {
    const cw_type_info *t = cw_comp_type(w, ct);
    const void *value = cw_comp_ptr(w, id, ct);
    if (!t || !value || !size) return false;
    cw_writer out;
    cw_writer_init_fixed(&out, data, capacity);
    for (size_t i = 0; i < t->field_count; ++i) {
        const cw_field *f = &t->fields[i];
        switch (f->type) {
            case CW_FIELD_BOOL: cw_writer_u8(&out, *(const bool *)at(value, f) ? 1 : 0); break;
            case CW_FIELD_U8: case CW_FIELD_ENUM: case CW_FIELD_FLAGS: cw_writer_u8(&out, *(const uint8_t *)at(value, f)); break;
            case CW_FIELD_I32: cw_writer_i32(&out, *(const int32_t *)at(value, f)); break;
            case CW_FIELD_U32: cw_writer_u32(&out, *(const uint32_t *)at(value, f)); break;
            case CW_FIELD_F32: cw_writer_f32(&out, *(const float *)at(value, f)); break;
            case CW_FIELD_ENTITY: cw_writer_i64(&out, *(const cw_entity_id *)at(value, f)); break;
            case CW_FIELD_STRING: {
                size_t n = bounded_len((const char *)at(value, f), f->capacity);
                cw_writer_u16(&out, (uint16_t)n);
                cw_writer_bytes(&out, at(value, f), n);
                break;
            }
            case CW_FIELD_BYTES: {
                uint16_t n = blob_size(f, value);
                cw_writer_u16(&out, n);
                cw_writer_bytes(&out, ((const blob_header *)at(value, f))->data, n);
                break;
            }
            default: return false;
        }
    }
    *size = out.size;
    return cw_writer_ok(&out);
}
bool cw_reflect_unmarshal(cw_world *w, cw_entity_id id, cw_component_id ct, const void *data, size_t size) {
    const cw_type_info *t = cw_comp_type(w, ct);
    void *slot = cw_comp_ptr(w, id, ct);
    if (!t || !slot || (!data && size)) return false;
    unsigned char local[1024];
    void *value = t->size <= sizeof(local) ? local : malloc(t->size);
    cw_require(value != NULL);
    cw_type_default(t, value);
    cw_reader in = cw_reader_make(data, size);
    for (size_t i = 0; i < t->field_count; ++i) {
        const cw_field *f = &t->fields[i];
        uint8_t u8 = 0;
        switch (f->type) {
            case CW_FIELD_BOOL: cw_reader_u8(&in, &u8); *(bool *)at_mut(value, f) = u8 != 0; break;
            case CW_FIELD_U8: case CW_FIELD_ENUM: case CW_FIELD_FLAGS: cw_reader_u8(&in, (uint8_t *)at_mut(value, f)); break;
            case CW_FIELD_I32: cw_reader_i32(&in, (int32_t *)at_mut(value, f)); break;
            case CW_FIELD_U32: cw_reader_u32(&in, (uint32_t *)at_mut(value, f)); break;
            case CW_FIELD_F32: cw_reader_f32(&in, (float *)at_mut(value, f)); break;
            case CW_FIELD_ENTITY: cw_reader_i64(&in, (int64_t *)at_mut(value, f)); break;
            case CW_FIELD_STRING: {
                uint16_t n = 0;
                cw_reader_u16(&in, &n);
                char *dst = at_mut(value, f);
                if (n >= f->capacity) { in.error = true; break; }   /* longer than the field: not ours */
                if (!cw_reader_bytes(&in, dst, n)) break;
                dst[n] = 0;
                break;
            }
            case CW_FIELD_BYTES: {
                uint16_t n = 0;
                cw_reader_u16(&in, &n);
                if (n > f->capacity) { in.error = true; break; }
                if (!cw_reader_bytes(&in, blob_data(f, value), n)) break;
                ((blob_header *)at_mut(value, f))->size = n;
                memset(blob_data(f, value) + n, 0, f->capacity - n);
                break;
            }
            default: break;
        }
    }
    bool ok = cw_reader_at_end(&in) && cw_type_check(t, value);
    if (ok) memcpy(slot, value, t->size);
    if (value != local) free(value);
    return ok;
}
cw_codec cw_reflect_codec(void) {
    cw_codec codec = {cw_reflect_marshal, cw_reflect_unmarshal};
    return codec;
}

/* --- entity helpers --- */
static bool find_semantic(cw_world *w, cw_entity_id id, cw_field_semantic sem, cw_component_id *ct_out, const cw_field **f_out) {
    if (!cw_world_is_valid(w, id)) return false;
    for (unsigned c = 0; c < CW_MAX_COMPONENTS; ++c) {
        const cw_type_info *t = cw_comp_type(w, (cw_component_id)c);
        if (!t || !cw_comp_has(w, id, (cw_component_id)c)) continue;
        for (size_t i = 0; i < t->field_count; ++i) {
            if (t->fields[i].semantic != sem) continue;
            *ct_out = (cw_component_id)c;
            *f_out = &t->fields[i];
            return true;
        }
    }
    return false;
}
bool cw_reflect_position(cw_world *w, cw_entity_id id, float *x, float *y) {
    cw_component_id cx, cy;
    const cw_field *fx, *fy;
    if (!find_semantic(w, id, CW_SEM_POSITION_X, &cx, &fx) || !find_semantic(w, id, CW_SEM_POSITION_Y, &cy, &fy)) return false;
    if (x) *x = (float)cw_field_get(fx, cw_comp_ptr(w, id, cx));
    if (y) *y = (float)cw_field_get(fy, cw_comp_ptr(w, id, cy));
    return true;
}
bool cw_reflect_set_position(cw_world *w, cw_entity_id id, float x, float y) {
    cw_component_id cx, cy;
    const cw_field *fx, *fy;
    if (!find_semantic(w, id, CW_SEM_POSITION_X, &cx, &fx) || !find_semantic(w, id, CW_SEM_POSITION_Y, &cy, &fy)) return false;
    cw_field_set(fx, cw_comp_ptr(w, id, cx), x);
    cw_field_set(fy, cw_comp_ptr(w, id, cy), y);
    cw_comp_touch(w, id, cx);
    if (cy != cx) cw_comp_touch(w, id, cy);
    return true;
}
bool cw_reflect_position3(cw_world *w, cw_entity_id id, float *x, float *y, float *z) {
    if (!cw_reflect_position(w, id, x, y)) return false;
    cw_component_id cz;
    const cw_field *fz;
    if (z) *z = find_semantic(w, id, CW_SEM_POSITION_Z, &cz, &fz) ? (float)cw_field_get(fz, cw_comp_ptr(w, id, cz)) : 0;
    return true;
}
bool cw_reflect_set_position3(cw_world *w, cw_entity_id id, float x, float y, float z) {
    if (!cw_reflect_set_position(w, id, x, y)) return false;
    cw_component_id cz;
    const cw_field *fz;
    if (find_semantic(w, id, CW_SEM_POSITION_Z, &cz, &fz)) {
        cw_field_set(fz, cw_comp_ptr(w, id, cz), z);
        cw_comp_touch(w, id, cz);
    }
    return true;
}
static bool triple_get(cw_world *w, cw_entity_id id, cw_field_semantic first, float fallback, float *a, float *b, float *c) {
    float *out[3] = {a, b, c};
    bool any = false;
    for (int i = 0; i < 3; ++i) {
        cw_component_id ct;
        const cw_field *f;
        bool found = find_semantic(w, id, (cw_field_semantic)(first + i), &ct, &f);
        if (out[i]) *out[i] = found ? (float)cw_field_get(f, cw_comp_ptr(w, id, ct)) : fallback;
        any = any || found;
    }
    return any;
}
static bool triple_set(cw_world *w, cw_entity_id id, cw_field_semantic first, float a, float b, float c) {
    float in[3] = {a, b, c};
    bool any = false;
    for (int i = 0; i < 3; ++i) {
        cw_component_id ct;
        const cw_field *f;
        if (!find_semantic(w, id, (cw_field_semantic)(first + i), &ct, &f)) continue;
        cw_field_set(f, cw_comp_ptr(w, id, ct), in[i]);
        cw_comp_touch(w, id, ct);
        any = true;
    }
    return any;
}
bool cw_reflect_rotation3(cw_world *w, cw_entity_id id, float *rx, float *ry, float *rz) { return triple_get(w, id, CW_SEM_ROTATION_X, 0, rx, ry, rz); }
bool cw_reflect_set_rotation3(cw_world *w, cw_entity_id id, float rx, float ry, float rz) { return triple_set(w, id, CW_SEM_ROTATION_X, rx, ry, rz); }
bool cw_reflect_size3(cw_world *w, cw_entity_id id, float *sx, float *sy, float *sz) { return triple_get(w, id, CW_SEM_SIZE_X, 1, sx, sy, sz); }
bool cw_reflect_set_size3(cw_world *w, cw_entity_id id, float sx, float sy, float sz) { return triple_set(w, id, CW_SEM_SIZE_X, sx, sy, sz); }
bool cw_reflect_has_position_z(cw_world *w, cw_entity_id id) {
    cw_component_id cz;
    const cw_field *fz;
    return find_semantic(w, id, CW_SEM_POSITION_Z, &cz, &fz);
}
float cw_reflect_radius(cw_world *w, cw_entity_id id, float fallback) {
    cw_component_id ct;
    const cw_field *f;
    if (!find_semantic(w, id, CW_SEM_RADIUS, &ct, &f)) return fallback;
    float r = (float)cw_field_get(f, cw_comp_ptr(w, id, ct));
    return r > 0 ? r : fallback;
}
size_t cw_reflect_remap(cw_world *w, cw_entity_id id, cw_entity_map_fn map, void *user) {
    if (!cw_world_is_valid(w, id) || !map) return 0;
    size_t changed = 0;
    for (unsigned c = 0; c < CW_MAX_COMPONENTS; ++c) {
        const cw_type_info *t = cw_comp_type(w, (cw_component_id)c);
        if (!t || !cw_comp_has(w, id, (cw_component_id)c)) continue;
        void *value = cw_comp_ptr(w, id, (cw_component_id)c);
        for (size_t i = 0; i < t->field_count; ++i) {
            const cw_field *f = &t->fields[i];
            if (f->type != CW_FIELD_ENTITY) continue;
            cw_entity_id *ref = at_mut(value, f);
            cw_entity_id mapped = map(*ref, user);
            if (mapped == *ref) continue;
            *ref = mapped;
            ++changed;
        }
        if (changed) cw_comp_touch(w, id, (cw_component_id)c);
    }
    return changed;
}
size_t cw_reflect_bake(cw_world *w, cw_entity_id id) {
    if (!cw_world_is_valid(w, id)) return 0;
    size_t made = 0;
    for (unsigned c = 0; c < CW_MAX_COMPONENTS; ++c) {
        const cw_type_info *t = cw_comp_type(w, (cw_component_id)c);
        if (!t || !(t->flags & CW_TYPE_CONFIG) || !cw_comp_has(w, id, (cw_component_id)c)) continue;
        cw_component_id rt;
        if (!cw_comp_by_name(w, t->generates, &rt) || cw_comp_has(w, id, rt)) continue;
        const cw_type_info *r = cw_comp_type(w, rt);
        if (!r) continue;
        unsigned char local[1024];
        void *value = r->size <= sizeof(local) ? local : malloc(r->size);
        cw_require(value != NULL);
        cw_type_default(r, value);
        t->bake(w, id, cw_comp_ptr(w, id, (cw_component_id)c), value);
        if (cw_type_check(r, value) && cw_comp_set(w, id, rt, value, r->size)) ++made;
        if (value != local) free(value);
    }
    return made;
}
bool cw_reflect_has_bakes(const cw_world *w) {
    for (unsigned c = 0; w && c < CW_MAX_COMPONENTS; ++c) {
        const cw_type_info *t = cw_comp_type(w, (cw_component_id)c);
        if (t && (t->flags & CW_TYPE_CONFIG)) return true;
    }
    return false;
}
size_t cw_reflect_copy_entity(cw_world *w, cw_entity_id from, cw_entity_id to) {
    if (!cw_world_is_valid(w, from) || !cw_world_is_valid(w, to) || from == to) return 0;
    size_t copied = 0;
    for (unsigned c = 0; c < CW_MAX_COMPONENTS; ++c) {
        cw_component_id ct = (cw_component_id)c;
        if (!cw_comp_has(w, from, ct) || cw_comp_is_transient(w, ct)) continue;
        const void *value = cw_comp_ptr(w, from, ct);
        if (cw_comp_set(w, to, ct, value, w->pools[ct].elem_size)) ++copied;
    }
    return copied;
}
