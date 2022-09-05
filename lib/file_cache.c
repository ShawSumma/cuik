#include <cuik.h>
#include "front/parser.h"

struct Cuik_FileCache {
    mtx_t lock;
    NL_Strmap(TokenStream) table;
};

CUIK_API Cuik_FileCache* cuik_fscache_create(void) {
    Cuik_FileCache* c = HEAP_ALLOC(sizeof(Cuik_FileCache));
    memset(c, 0, sizeof(Cuik_FileCache));
    mtx_init(&c->lock, mtx_plain);
    return c;
}

CUIK_API void cuik_fscache_destroy(Cuik_FileCache* restrict c) {
    mtx_destroy(&c->lock);
    HEAP_FREE(c);
}

CUIK_API void cuik_fscache_put(Cuik_FileCache* restrict c, const char* filepath, const TokenStream* tokens) {
    mtx_lock(&c->lock);
    nl_strmap_put_cstr(c->table, filepath, *tokens);
    mtx_unlock(&c->lock);
}

CUIK_API bool cuik_fscache_lookup(Cuik_FileCache* restrict c, const char* filepath, TokenStream* out_tokens) {
    mtx_lock(&c->lock);
    ptrdiff_t search = nl_strmap_get_cstr(c->table, filepath);
    if (search >= 0) {
        if (out_tokens) *out_tokens = c->table[search];
    }
    mtx_unlock(&c->lock);

    return (search >= 0);
}
