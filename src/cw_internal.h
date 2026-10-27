#ifndef CW_INTERNAL_H
#define CW_INTERNAL_H
#include "clockwork.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void cw_require(bool condition) {
    if (!condition) {
        fputs("clockwork: allocation failure, size overflow, or invalid lifecycle use\n", stderr);
        abort();
    }
}
static inline size_t cw_mul(size_t a, size_t b) {
    cw_require(!b || a <= SIZE_MAX / b);
    return a * b;
}
static inline size_t cw_add(size_t a, size_t b) {
    cw_require(a <= SIZE_MAX - b);
    return a + b;
}
static inline void *cw_resize(void *ptr, size_t count, size_t size) {
    void *result = realloc(ptr, cw_mul(count, size));
    cw_require(result != NULL);
    return result;
}
static inline char *cw_copy_string(const char *s) {
    size_t n = cw_add(strlen(s ? s : ""), 1);
    char *copy = cw_resize(NULL, n, 1);
    memcpy(copy, s ? s : "", n);
    return copy;
}
static inline bool cw_registered(const cw_world *w, cw_component_id ct) {
    return w && ct < CW_MAX_COMPONENTS && w->pools[ct].elem_size != 0;
}
#endif
