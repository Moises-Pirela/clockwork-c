#ifndef _WIN32
#define _DEFAULT_SOURCE 1
#endif
#include "cw_internal.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

static size_t page_size(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
#else
    long page = sysconf(_SC_PAGESIZE);
    cw_require(page > 0);
    return (size_t)page;
#endif
}
static size_t page_round(size_t bytes) {
    size_t page = page_size();
    return cw_mul(cw_add(bytes, page - 1) / page, page);
}
static void *vm_reserve(size_t bytes) {
#ifdef _WIN32
    void *ptr = VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_NOACCESS);
    cw_require(ptr != NULL);
#else
    void *ptr = mmap(NULL, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    cw_require(ptr != MAP_FAILED);
#endif
    return ptr;
}
static void vm_commit(void *ptr, size_t bytes) {
    if (!bytes) return;
#ifdef _WIN32
    cw_require(VirtualAlloc(ptr, bytes, MEM_COMMIT, PAGE_READWRITE) != NULL);
#else
    cw_require(mprotect(ptr, bytes, PROT_READ | PROT_WRITE) == 0);
#endif
}
static void vm_release(void *ptr, size_t bytes) {
    if (!ptr) return;
#ifdef _WIN32
    (void)bytes;
    cw_require(VirtualFree(ptr, 0, MEM_RELEASE) != 0);
#else
    cw_require(munmap(ptr, bytes) == 0);
#endif
}
static void ids_reserve(cw_id_list *ids, size_t count) {
    if (count <= ids->capacity) return;
    size_t capacity = ids->capacity ? cw_mul(ids->capacity, 2) : 16;
    if (capacity < count) capacity = count;
    ids->data = cw_resize(ids->data, capacity, sizeof(*ids->data));
    ids->capacity = capacity;
}
static void ids_push(cw_id_list *ids, cw_entity_id id) {
    ids_reserve(ids, cw_add(ids->count, 1));
    ids->data[ids->count++] = id;
}
static void ids_remove(cw_id_list *ids, cw_entity_id id) {
    for (size_t i = 0; i < ids->count; ++i) {
        if (ids->data[i] != id) continue;
        memmove(ids->data + i, ids->data + i + 1,
                (ids->count - i - 1) * sizeof(*ids->data));
        --ids->count;
        return;
    }
}
static cw_entity empty_entity(void) {
    cw_entity e = {0};
    e.id = CW_INVALID_ENTITY;
    return e;
}
static void pool_reserve(cw_component_pool *p, size_t count) {
    if (count <= p->capacity) return;
    size_t needed = page_round(cw_mul(count, p->elem_size));
    if (needed > p->reserved_bytes) {
        size_t reserved = p->reserved_bytes ? cw_mul(p->reserved_bytes, 2)
            : cw_mul(CW_POOL_RESERVE_ENTITIES, p->elem_size);
        if (reserved < needed) reserved = needed;
        reserved = page_round(reserved);
        void *data = vm_reserve(reserved);
        vm_commit(data, needed);
        if (p->data) {
            memcpy(data, p->data, cw_mul(p->capacity, p->elem_size));
            vm_release(p->data, p->reserved_bytes);
        }
        p->data = data;
        p->reserved_bytes = reserved;
    } else if (needed > p->committed_bytes) {
        vm_commit((unsigned char *)p->data + p->committed_bytes,
                  needed - p->committed_bytes);
    }
    size_t words = cw_add(count, 63) / 64;
    p->bitset = cw_resize(p->bitset, words, sizeof(*p->bitset));
    memset(p->bitset + p->bitset_count, 0,
           (words - p->bitset_count) * sizeof(*p->bitset));
    p->bitset_count = words;
    p->committed_bytes = needed;
    p->capacity = count;
}
static void *pool_slot(cw_component_pool *p, size_t id) {
    return (unsigned char *)p->data + id * p->elem_size;
}
static void pool_clear(cw_component_pool *p, size_t id) {
    void *slot = pool_slot(p, id);
    if (p->clear) p->clear(slot);
    else memset(slot, 0, p->elem_size);
}
static void comp_mark(cw_world *w, cw_entity_id id, cw_component_id ct) {
    size_t i = (size_t)id;
    w->pools[ct].bitset[i >> 6] |= UINT64_C(1) << (i & 63);
    cw_sig_set(&w->entities[i].signature, ct);
}
static void entity_clear(cw_world *w, cw_entity_id id) {
    for (unsigned ct = 0; ct < CW_MAX_COMPONENTS; ++ct)
        cw_comp_remove(w, id, (cw_component_id)ct);
}

bool cw_sig_set(cw_signature *sig, cw_component_id ct) {
    if (!sig || ct >= CW_MAX_COMPONENTS) return false;
    sig->words[ct >> 6] |= UINT64_C(1) << (ct & 63);
    return true;
}
bool cw_sig_clear(cw_signature *sig, cw_component_id ct) {
    if (!sig || ct >= CW_MAX_COMPONENTS) return false;
    sig->words[ct >> 6] &= ~(UINT64_C(1) << (ct & 63));
    return true;
}
bool cw_sig_has(cw_signature sig, cw_component_id ct) {
    return ct < CW_MAX_COMPONENTS && (sig.words[ct >> 6] & (UINT64_C(1) << (ct & 63))) != 0;
}
cw_signature cw_mask(const cw_component_id *types, size_t count) {
    cw_signature mask = {0};
    if (types) for (size_t i = 0; i < count; ++i) cw_sig_set(&mask, types[i]);
    return mask;
}
bool cw_entity_is_active(cw_entity e) { return (e.flags & CW_ACTIVE) != 0; }
bool cw_query_visible(cw_entity e) {
    return cw_entity_is_active(e) && !(e.flags & CW_TO_DESTROY);
}
void cw_world_init(cw_world *w, void *user) {
    cw_require(w != NULL);
    memset(w, 0, sizeof(*w));
    w->user = user;
    w->last_created = CW_INVALID_ENTITY;
    cw_world_ensure_capacity(w, 0);
}
void cw_world_destroy(cw_world *w) {
    if (!w) return;
    cw_world_reset(w);
    free(w->entities);
    free(w->recycled_ids.data);
    free(w->pending_destroy.data);
    free(w->created.data);
    for (unsigned i = 0; i < CW_MAX_COMPONENTS; ++i) {
        cw_component_pool *p = &w->pools[i];
        vm_release(p->data, p->reserved_bytes);
        free(p->bitset);
        free(p->name);
    }
    memset(w, 0, sizeof(*w));
}
void cw_world_ensure_capacity(cw_world *w, cw_entity_id id) {
    if (!w || id < 0) return;
    cw_require((uint64_t)id < SIZE_MAX);
    if ((size_t)id < w->capacity) return;
    size_t target = cw_mul(cw_add((size_t)id / CW_CHUNK_SIZE, 1), CW_CHUNK_SIZE);
    w->entities = cw_resize(w->entities, target, sizeof(*w->entities));
    for (size_t i = w->capacity; i < target; ++i) w->entities[i] = empty_entity();
    for (unsigned i = 0; i < CW_MAX_COMPONENTS; ++i)
        if (w->pools[i].elem_size) pool_reserve(&w->pools[i], target);
    w->capacity = target;
}
cw_entity_id cw_world_create_entity(cw_world *w) {
    if (!w) return CW_INVALID_ENTITY;
    cw_entity_id id;
    if (w->recycled_ids.count) id = w->recycled_ids.data[--w->recycled_ids.count];
    else {
        cw_require(w->next_id < INT64_MAX);
        id = w->next_id++;
    }
    cw_world_ensure_capacity(w, id);
    w->entities[id] = empty_entity();
    w->entities[id].id = id;
    w->entities[id].flags = CW_ACTIVE;
    w->last_created = id;
    ids_push(&w->created, id);
    return id;
}
bool cw_world_restore_entity(cw_world *w, cw_entity_id id) {
    if (!w || id < 0 || id == INT64_MAX) return false;
    cw_world_ensure_capacity(w, id);
    cw_entity *e = &w->entities[id];
    if (cw_entity_is_active(*e)) {
        e->flags &= (uint8_t)~CW_TO_DESTROY;
        ids_remove(&w->pending_destroy, id);
        return false;
    }
    *e = empty_entity();
    e->id = id;
    e->flags = CW_ACTIVE;
    ids_remove(&w->recycled_ids, id);
    if (id >= w->next_id) w->next_id = id + 1;
    w->last_created = id;
    ids_push(&w->created, id);
    return true;
}
bool cw_world_rebuild_entity(cw_world *w, cw_entity_id id) {
    if (!cw_world_is_valid(w, id)) return false;
    entity_clear(w, id);
    w->entities[id].flags = CW_ACTIVE;
    ids_remove(&w->pending_destroy, id);
    ids_push(&w->created, id);
    return true;
}
void cw_world_mark_destroy(cw_world *w, cw_entity_id id) {
    if (!cw_world_is_valid(w, id) || (w->entities[id].flags & CW_TO_DESTROY)) return;
    w->entities[id].flags |= CW_TO_DESTROY;
    ids_push(&w->pending_destroy, id);
}
bool cw_world_is_valid(const cw_world *w, cw_entity_id id) {
    return w && id >= 0 && (uint64_t)id < w->capacity && cw_entity_is_active(w->entities[id]);
}
size_t cw_world_entity_count(const cw_world *w) {
    size_t n = 0;
    if (w) for (size_t i = 0; i < w->capacity; ++i) n += cw_entity_is_active(w->entities[i]);
    return n;
}
void cw_world_reserve_entities(cw_world *w, size_t count) {
    if (!w) return;
    size_t additional = count > w->recycled_ids.count ? count - w->recycled_ids.count : 0;
    if (additional) {
        cw_require((uint64_t)additional <= (uint64_t)(INT64_MAX - w->next_id));
        cw_world_ensure_capacity(w, w->next_id + (cw_entity_id)additional - 1);
    }
    ids_reserve(&w->created, cw_add(w->created.count, count));
}
void cw_world_sweep(cw_world *w) {
    if (!w) return;
    for (size_t i = 0; i < w->pending_destroy.count; ++i) {
        cw_entity_id id = w->pending_destroy.data[i];
        if (!cw_world_is_valid(w, id) || !(w->entities[id].flags & CW_TO_DESTROY)) continue;
        entity_clear(w, id);
        w->entities[id] = empty_entity();
        if (w->recycle_ids) ids_push(&w->recycled_ids, id);
    }
    w->pending_destroy.count = 0;
}
void cw_world_reset(cw_world *w) {
    if (!w) return;
    for (size_t i = 0; i < w->capacity; ++i)
        if (cw_entity_is_active(w->entities[i])) cw_world_mark_destroy(w, (cw_entity_id)i);
    cw_world_sweep(w);
    w->recycled_ids.count = w->created.count = 0;
    w->next_id = 0;
    w->last_created = CW_INVALID_ENTITY;
    w->tick = 0;
}
cw_id_view cw_world_created(const cw_world *w) {
    cw_id_view view = {w ? w->created.data : NULL, w ? w->created.count : 0};
    return view;
}
cw_id_view cw_world_pending_destroy(const cw_world *w) {
    cw_id_view view = {w ? w->pending_destroy.data : NULL, w ? w->pending_destroy.count : 0};
    return view;
}

cw_component_id cw_comp_block(cw_world *w, size_t count) {
    if (!w || !count || count > CW_MAX_COMPONENTS - w->next_block) return CW_INVALID_COMPONENT;
    cw_component_id base = (cw_component_id)w->next_block;
    w->next_block += (unsigned)count;
    return base;
}
cw_component_id cw_cid(cw_component_id base, unsigned offset) {
    if (base >= CW_MAX_COMPONENTS || offset >= CW_MAX_COMPONENTS - (unsigned)base)
        return CW_INVALID_COMPONENT;
    return (cw_component_id)(base + offset);
}
bool cw_comp_register(cw_world *w, cw_component_id ct, size_t size,
                      size_t alignment, const char *name, cw_component_fn clear) {
    if (!w || ct >= CW_MAX_COMPONENTS || w->pools[ct].elem_size) return false;
    if (!alignment || (alignment & (alignment - 1)) || alignment > page_size()) return false;
    if (!size) size = 1;
    if (size % alignment) return false;
    cw_component_pool *p = &w->pools[ct];
    p->elem_size = size;
    p->alignment = alignment;
    p->clear = clear;
    p->name = cw_copy_string(name && *name ? name : "?");
    pool_reserve(p, w->capacity > CW_CHUNK_SIZE ? w->capacity : CW_CHUNK_SIZE);
    return true;
}
bool cw_comp_register_at(cw_world *w, cw_component_id base, unsigned offset,
                         size_t size, size_t alignment, const char *name, cw_component_fn clear) {
    cw_component_id ct = cw_cid(base, offset);
    return w && ct < w->next_block && cw_comp_register(w, ct, size, alignment, name, clear);
}
void *cw_comp_ptr(cw_world *w, cw_entity_id id, cw_component_id ct) {
    if (!cw_registered(w, ct) || !cw_world_is_valid(w, id)) return NULL;
    return pool_slot(&w->pools[ct], (size_t)id);
}
void *cw_comp_get(cw_world *w, cw_entity_id id, cw_component_id ct, size_t size) {
    if (!cw_registered(w, ct) || w->pools[ct].elem_size != (size ? size : 1)) return NULL;
    return cw_comp_ptr(w, id, ct);
}
bool cw_comp_set(cw_world *w, cw_entity_id id, cw_component_id ct,
                 const void *value, size_t size) {
    void *slot = cw_comp_get(w, id, ct, size);
    if (!slot || (!value && size)) return false;
    if (size) memmove(slot, value, size);
    comp_mark(w, id, ct);
    return true;
}
bool cw_comp_add(cw_world *w, cw_entity_id id, cw_component_id ct) {
    void *slot = cw_comp_ptr(w, id, ct);
    if (!slot) return false;
    if (!cw_comp_has(w, id, ct) && w->pools[ct].init) w->pools[ct].init(slot);
    comp_mark(w, id, ct);
    return true;
}
bool cw_comp_remove(cw_world *w, cw_entity_id id, cw_component_id ct) {
    if (!cw_registered(w, ct) || !cw_comp_has(w, id, ct)) return false;
    pool_clear(&w->pools[ct], (size_t)id);
    w->pools[ct].bitset[(size_t)id >> 6] &= ~(UINT64_C(1) << ((size_t)id & 63));
    cw_sig_clear(&w->entities[id].signature, ct);
    return true;
}
bool cw_comp_has(const cw_world *w, cw_entity_id id, cw_component_id ct) {
    return cw_world_is_valid(w, id) && cw_sig_has(w->entities[id].signature, ct);
}
bool cw_comp_set_init(cw_world *w, cw_component_id ct, cw_component_fn init) {
    if (!cw_registered(w, ct)) return false;
    w->pools[ct].init = init;
    return true;
}
const char *cw_comp_name(const cw_world *w, cw_component_id ct) {
    return cw_registered(w, ct) ? w->pools[ct].name : "?";
}
bool cw_comp_by_name(const cw_world *w, const char *name, cw_component_id *ct) {
    if (!w || !name || !ct) return false;
    for (unsigned i = 0; i < CW_MAX_COMPONENTS; ++i) {
        if (w->pools[i].elem_size && strcmp(w->pools[i].name, name) == 0) {
            *ct = (cw_component_id)i;
            return true;
        }
    }
    return false;
}
size_t cw_registered_components(const cw_world *w, cw_component_id *out, size_t capacity) {
    size_t n = 0;
    if (w) for (unsigned i = 0; i < CW_MAX_COMPONENTS; ++i) {
        if (!w->pools[i].elem_size) continue;
        if (out && n < capacity) out[n] = (cw_component_id)i;
        ++n;
    }
    return n;
}
bool cw_comp_set_codec(cw_world *w, cw_component_id ct, cw_codec codec) {
    if (!cw_registered(w, ct)) return false;
    w->pools[ct].codec = codec;
    return true;
}
bool cw_comp_set_transient(cw_world *w, cw_component_id ct, bool transient) {
    if (!cw_registered(w, ct)) return false;
    w->pools[ct].transient = transient;
    return true;
}
bool cw_comp_is_transient(const cw_world *w, cw_component_id ct) {
    return cw_registered(w, ct) && w->pools[ct].transient;
}
bool cw_comp_marshal(cw_world *w, cw_entity_id id, cw_component_id ct,
                     void *data, size_t capacity, size_t *size) {
    if (size) *size = 0;
    if (!size || (!data && capacity) || !cw_registered(w, ct) || !cw_comp_has(w, id, ct)) return false;
    return w->pools[ct].codec.marshal && w->pools[ct].codec.marshal(w, id, ct, data, capacity, size);
}
bool cw_comp_unmarshal(cw_world *w, cw_entity_id id, cw_component_id ct,
                       const void *data, size_t size) {
    if ((!data && size) || !cw_comp_ptr(w, id, ct) || !w->pools[ct].codec.unmarshal) return false;
    if (!w->pools[ct].codec.unmarshal(w, id, ct, data, size)) return false;
    comp_mark(w, id, ct);
    return true;
}

static unsigned trailing_zeroes(uint64_t bits) {
    unsigned n = 0;
    if (!(bits & UINT64_C(0xffffffff))) { n += 32; bits >>= 32; }
    if (!(bits & UINT64_C(0xffff))) { n += 16; bits >>= 16; }
    if (!(bits & UINT64_C(0xff))) { n += 8; bits >>= 8; }
    if (!(bits & UINT64_C(0xf))) { n += 4; bits >>= 4; }
    if (!(bits & UINT64_C(0x3))) { n += 2; bits >>= 2; }
    if (!(bits & UINT64_C(0x1))) ++n;
    return n;
}
cw_query cw_query_iter(cw_world *w, cw_signature mask) {
    cw_query q = {0};
    q.world = w;
    q.mask = mask;
    return q;
}
bool cw_query_next(cw_query *q, cw_entity_id *id) {
    if (id) *id = CW_INVALID_ENTITY;
    if (!q || !q->world || !id) return false;
    cw_world *w = q->world;
    for (;;) {
        while (q->current_word) {
            unsigned bit = trailing_zeroes(q->current_word);
            q->current_word &= q->current_word - 1;
            size_t candidate = q->base + bit;
            if (candidate >= w->capacity || !cw_query_visible(w->entities[candidate])) continue;
            bool matches = true;
            for (unsigned i = 0; i < CW_SIGNATURE_WORDS; ++i)
                if ((w->entities[candidate].signature.words[i] & q->mask.words[i]) != q->mask.words[i])
                    matches = false;
            if (!matches) continue;
            *id = (cw_entity_id)candidate;
            return true;
        }
        if (q->word_index >= w->capacity / 64) return false;
        uint64_t combined = UINT64_MAX;
        for (unsigned wi = 0; wi < CW_SIGNATURE_WORDS && combined; ++wi) {
            for (uint64_t cm = q->mask.words[wi]; cm && combined; cm &= cm - 1) {
                unsigned ct = wi * 64 + trailing_zeroes(cm);
                cw_component_pool *p = &w->pools[ct];
                combined &= q->word_index < p->bitset_count ? p->bitset[q->word_index] : 0;
            }
        }
        q->base = q->word_index++ * 64;
        q->current_word = combined;
    }
}
size_t cw_query_collect(cw_world *w, cw_signature mask, cw_entity_id *out, size_t capacity) {
    cw_query q = cw_query_iter(w, mask);
    cw_entity_id id;
    size_t n = 0;
    while (cw_query_next(&q, &id)) {
        if (out && n < capacity) out[n] = id;
        ++n;
    }
    return n;
}
