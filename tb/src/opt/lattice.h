#include <hashes.h>
#include <math.h>

static Lattice TOP_IN_THE_SKY    = { LATTICE_TOP    };
static Lattice BOT_IN_THE_SKY    = { LATTICE_BOT    };
static Lattice CTRL_IN_THE_SKY   = { LATTICE_CTRL   };
static Lattice XNULL_IN_THE_SKY  = { LATTICE_XNULL  };
static Lattice NULL_IN_THE_SKY   = { LATTICE_NULL   };
static Lattice FLT32_IN_THE_SKY  = { LATTICE_FLT32  };
static Lattice FLT64_IN_THE_SKY  = { LATTICE_FLT64  };
static Lattice NAN32_IN_THE_SKY  = { LATTICE_NAN32  };
static Lattice NAN64_IN_THE_SKY  = { LATTICE_NAN64  };
static Lattice XNAN32_IN_THE_SKY = { LATTICE_XNAN32 };
static Lattice XNAN64_IN_THE_SKY = { LATTICE_XNAN64 };
static Lattice ANYMEM_IN_THE_SKY = { LATTICE_ANYMEM };
static Lattice ALLMEM_IN_THE_SKY = { LATTICE_ALLMEM };
static Lattice PTR_IN_THE_SKY    = { LATTICE_PTR    };
static Lattice FALSE_IN_THE_SKY  = { LATTICE_INT, ._int = {  0, 0, 1, 0 } };
static Lattice TRUE_IN_THE_SKY   = { LATTICE_INT, ._int = {  1, 1, 0, 1 } };
static Lattice BOOL_IN_THE_SKY   = { LATTICE_INT, ._int = { -1, 0, 0, 0 } };

static Lattice* lattice_from_dt(TB_Function* f, TB_DataType dt);

static uint32_t lattice_hash(void* a) {
    Lattice* l = a;
    uint64_t h = l->tag + 1000;
    switch (l->tag) {
        case LATTICE_TUPLE: {
            // just add pointers together, what's the worst that could happen
            FOR_N(i, 0, l->_elem_count) {
                h += (uintptr_t) l->elems[i];
            }
            h += l->_elem_count;
            break;
        }

        case LATTICE_INT:
        h += l->_int.min,         h += l->_int.max;
        h += l->_int.known_zeros, h += l->_int.known_ones, h += l->_int.widen;
        break;

        case LATTICE_PTRCON:
        h += (uintptr_t) l->_ptr;
        break;

        case LATTICE_MEM_SLICE:
        h += l->_alias_n;
        FOR_N(i, 0, l->_alias_n) { h += l->alias[i]; }
        break;

        case LATTICE_FLTCON32: memcpy(&h, &l->_f32, sizeof(float));  break;
        case LATTICE_FLTCON64: memcpy(&h, &l->_f64, sizeof(double)); break;

        // no fields
        case LATTICE_BOT:   case LATTICE_TOP:
        case LATTICE_FLT32: case LATTICE_FLT64:
        case LATTICE_NAN32: case LATTICE_XNAN32:
        case LATTICE_NAN64: case LATTICE_XNAN64:
        case LATTICE_NULL:  case LATTICE_XNULL:
        case LATTICE_CTRL:  case LATTICE_PTR:
        case LATTICE_ALLMEM:case LATTICE_ANYMEM:
        break;

        default: tb_todo();
    }

    // fib hashing amirite
    return (h * 11400714819323198485llu) >> 32llu;
}

static bool lattice_cmp(void* a, void* b) {
    Lattice *aa = a, *bb = b;
    if (aa->tag != bb->tag) { return false; }

    switch (aa->tag) {
        case LATTICE_TUPLE:
        if (aa->_elem_count != bb->_elem_count) { return false; }
        FOR_N(i, 0, aa->_elem_count) {
            if (aa->elems[i] != bb->elems[i]) { return false; }
        }
        return true;

        case LATTICE_INT:
        return memcmp(&aa->_int, &bb->_int, sizeof(LatticeInt)) == 0;

        case LATTICE_PTRCON:
        return aa->_ptr == bb->_ptr;

        case LATTICE_FLTCON32:
        return memcmp(&aa->_f32, &bb->_f32, sizeof(float)) == 0;

        case LATTICE_FLTCON64:
        return memcmp(&aa->_f64, &bb->_f64, sizeof(double)) == 0;

        case LATTICE_MEM_SLICE:
        if (aa->_alias_n != bb->_alias_n) { return false; }
        FOR_N(i, 0, aa->_alias_n) {
            if (aa->alias[i] != bb->alias[i]) { return false; }
        }
        return true;

        // no fields
        default: return true;
    }
}

static bool lattice_is_const_int(Lattice* l) { return l->_int.min == l->_int.max; }
static bool lattice_is_const(Lattice* l) { return l->tag == LATTICE_INT && l->_int.min == l->_int.max; }

static void latuni_grow(TB_Function* f, size_t top) {
    size_t new_cap = tb_next_pow2(top + 16);
    f->types = tb_platform_heap_realloc(f->types, new_cap * sizeof(Lattice*));
    // clear new space
    FOR_N(i, f->type_cap, new_cap) { f->types[i] = NULL; }
    f->type_cap = new_cap;
}

static bool latuni_set_progress(TB_Function* f, TB_Node* n, Lattice* l) {
    if (UNLIKELY(n->gvn >= f->type_cap)) { latuni_grow(f, n->gvn); }
    Lattice* old = f->types[n->gvn];
    f->types[n->gvn] = l;
    return old != l;
}

static void latuni_set(TB_Function* f, TB_Node* n, Lattice* l) {
    if (UNLIKELY(n->gvn >= f->type_cap)) { latuni_grow(f, n->gvn); }
    f->types[n->gvn] = l;
}

Lattice* latuni_get(TB_Function* f, TB_Node* n) {
    if (UNLIKELY(n->gvn >= f->type_cap)) { latuni_grow(f, n->gvn); }
    if (f->types[n->gvn] == NULL) { f->types[n->gvn] = lattice_from_dt(f, n->dt); }
    return f->types[n->gvn];
}

static Lattice* lattice_intern(TB_Function* f, Lattice l) {
    assert(l.tag != LATTICE_TUPLE);
    Lattice* k = nl_hashset_get2(&f->type_interner, &l, lattice_hash, lattice_cmp);
    if (k != NULL) { return k; }

    // allocate new node
    k = tb_arena_alloc(f->arena, sizeof(Lattice));
    memcpy(k, &l, sizeof(l));
    nl_hashset_put2(&f->type_interner, k, lattice_hash, lattice_cmp);
    return k;
}

static bool lattice_top_or_bot(Lattice* l) {
    return l->tag <= LATTICE_TOP;
}

Lattice* lattice_truthy(Lattice* l) {
    switch (l->tag) {
        case LATTICE_INT:
        if (l->_int.min == l->_int.max) {
            return l->_int.min ? &TRUE_IN_THE_SKY : &FALSE_IN_THE_SKY;
        }
        return &BOT_IN_THE_SKY;

        case LATTICE_NAN32: return &FALSE_IN_THE_SKY;
        case LATTICE_NAN64: return &FALSE_IN_THE_SKY;

        case LATTICE_NULL:  return &FALSE_IN_THE_SKY;
        case LATTICE_XNULL: return &TRUE_IN_THE_SKY;

        default:
        return &BOT_IN_THE_SKY;
    }
}

static uint64_t lattice_int_min(int bits) { return (1ll << (bits - 1)) | ~tb__mask(bits); }
static uint64_t lattice_int_max(int bits) { return (1ll << (bits - 1)) - 1; }
static uint64_t lattice_uint_max(int bits) { return UINT64_MAX >> (64 - bits); }

static Lattice* lattice_from_dt(TB_Function* f, TB_DataType dt) {
    switch (dt.type) {
        case TB_INT: {
            assert(dt.data <= 64);
            if (dt.data == 0) {
                return &BOT_IN_THE_SKY;
            } else if (dt.data == 1) {
                return &BOOL_IN_THE_SKY;
            }

            return lattice_intern(f, (Lattice){ LATTICE_INT, ._int = { lattice_int_min(dt.data), lattice_int_max(dt.data) } });
        }

        case TB_FLOAT: {
            assert(dt.data == TB_FLT_32 || dt.data == TB_FLT_64);
            return dt.data == TB_FLT_64 ? &FLT64_IN_THE_SKY : &FLT32_IN_THE_SKY;
        }

        case TB_PTR: {
            return &PTR_IN_THE_SKY;
        }

        case TB_MEMORY: {
            return &ALLMEM_IN_THE_SKY;
        }

        case TB_CONTROL: return &CTRL_IN_THE_SKY;
        default: return &BOT_IN_THE_SKY;
    }
}

static Lattice* lattice_branch_goto(TB_Function* f, int succ_count, int taken) {
    size_t size = sizeof(Lattice) + succ_count*sizeof(Lattice*);
    Lattice* l = tb_arena_alloc(f->arena, size);
    *l = (Lattice){ LATTICE_TUPLE, ._elem_count = succ_count };
    FOR_N(i, 0, succ_count) {
        l->elems[i] = i == taken ? &CTRL_IN_THE_SKY : &TOP_IN_THE_SKY;
    }

    Lattice* k = nl_hashset_put2(&f->type_interner, l, lattice_hash, lattice_cmp);
    if (k) {
        tb_arena_free(f->arena, l, size);
        return k;
    } else {
        return l;
    }
}

static Lattice* lattice_tuple_from_node(TB_Function* f, TB_Node* n) {
    assert(n->dt.type == TB_TUPLE);

    // count projs
    int projs = 0;
    FOR_USERS(u, n) {
        if (USERN(u)->type == TB_PROJ) projs++;
    }

    size_t size = sizeof(Lattice) + projs*sizeof(Lattice*);
    Lattice* l = tb_arena_alloc(f->arena, size);
    *l = (Lattice){ LATTICE_TUPLE, ._elem_count = projs };
    FOR_USERS(u, n) {
        if (USERN(u)->type != TB_PROJ) continue;
        int index = TB_NODE_GET_EXTRA_T(USERN(u), TB_NodeProj)->index;
        l->elems[index] = lattice_from_dt(f, USERN(u)->dt);
    }

    Lattice* k = nl_hashset_put2(&f->type_interner, l, lattice_hash, lattice_cmp);
    if (k) {
        tb_arena_free(f->arena, l, size);
        return k;
    } else {
        return l;
    }
}

#define MASK_UPTO(pos) (UINT64_MAX >> (64 - pos))
#define BEXTR(src,pos) (((src) >> (pos)) & 1)
uint64_t tb__sxt(uint64_t src, uint64_t src_bits, uint64_t dst_bits) {
    uint64_t sign_bit = BEXTR(src, src_bits-1);
    uint64_t mask = MASK_UPTO(dst_bits) & ~MASK_UPTO(src_bits);

    uint64_t dst = src & ~mask;
    return dst | (sign_bit ? mask : 0);
}

static bool lattice_signed(LatticeInt* l) { return l->min > l->max; }

static LatticeInt lattice_into_unsigned(LatticeInt i, int bits) {
    if (i.min > i.max) {
        i.min = 0;
        i.max = lattice_uint_max(bits);
        return i;
    } else {
        return i;
    }
}

static Lattice* lattice_gimme_int(TB_Function* f, int64_t min, int64_t max) {
    assert(min <= max);
    return lattice_intern(f, (Lattice){ LATTICE_INT, ._int = { min, max } });
}

static Lattice* lattice_gimme_uint(TB_Function* f, uint64_t min, uint64_t max) {
    assert(min <= max);
    return lattice_intern(f, (Lattice){ LATTICE_INT, ._int = { min, max } });
}

static Lattice* lattice_alias(TB_Function* f, int alias_idx) {
    size_t alias_n = (alias_idx + 64) / 64;
    size_t size = sizeof(Lattice) + alias_n*sizeof(Lattice*);
    Lattice* l = tb_arena_alloc(f->arena, size);
    *l = (Lattice){ LATTICE_MEM_SLICE, ._alias_n = alias_n };

    // just flip aliases
    FOR_N(i, 0, alias_n) { l->alias[i] = 0; }
    l->alias[alias_idx / 64] |= 1ull << (alias_idx % 64);

    Lattice* k = nl_hashset_put2(&f->type_interner, l, lattice_hash, lattice_cmp);
    if (k) {
        tb_arena_free(f->arena, l, size);
        return k;
    } else {
        return l;
    }
}

static Lattice* lattice_dual(TB_Function* f, Lattice* type) {
    switch (type->tag) {
        case LATTICE_BOT: {
            return &TOP_IN_THE_SKY;
        }

        case LATTICE_INT: {
            LatticeInt i = type->_int;
            return lattice_intern(f, (Lattice){ LATTICE_INT, ._int = { i.max, i.min, ~i.known_zeros, ~i.known_ones, INT_WIDEN_LIMIT - i.widen } });
        }

        case LATTICE_ALLMEM: return &ANYMEM_IN_THE_SKY;
        case LATTICE_ANYMEM: return &ALLMEM_IN_THE_SKY;

        case LATTICE_MEM_SLICE: {
            size_t size = sizeof(Lattice) + type->_alias_n*sizeof(Lattice*);
            Lattice* l = tb_arena_alloc(f->arena, size);
            *l = (Lattice){ LATTICE_TUPLE, ._alias_n = type->_alias_n };

            // just flip aliases
            FOR_N(i, 0, type->_alias_n) { l->alias[i] = ~type->alias[i]; }

            Lattice* k = nl_hashset_put2(&f->type_interner, l, lattice_hash, lattice_cmp);
            if (k) {
                tb_arena_free(f->arena, l, size);
                return k;
            } else {
                return l;
            }
        }

        case LATTICE_TUPLE: {
            size_t size = sizeof(Lattice) + type->_elem_count*sizeof(Lattice*);
            Lattice* l = tb_arena_alloc(f->arena, size);
            *l = (Lattice){ LATTICE_TUPLE, ._elem_count = type->_elem_count };
            FOR_N(i, 0, type->_elem_count) {
                l->elems[i] = lattice_dual(f, type->elems[i]);
            }

            Lattice* k = nl_hashset_put2(&f->type_interner, l, lattice_hash, lattice_cmp);
            if (k) {
                tb_arena_free(f->arena, l, size);
                return k;
            } else {
                return l;
            }
        }

        default:
        return type;
    }
}

// greatest lower bound between a and b, note that we expect interned lattices here
static Lattice* lattice_meet(TB_Function* f, Lattice* a, Lattice* b) {
    // a ^ a = a
    if (a == b) return a;

    // it's commutative, so let's simplify later code this way.
    // now a will always be smaller tag than b.
    if (a->tag > b->tag) {
        SWAP(Lattice*, a, b);
    }

    switch (a->tag) {
        case LATTICE_BOT: return &BOT_IN_THE_SKY;
        case LATTICE_TOP: return b;

        case LATTICE_INT: {
            if (b->tag != LATTICE_INT) {
                return &BOT_IN_THE_SKY;
            }

            // [amin, amax] /\ [bmin, bmax] => [min(amin, bmin), max(amax, bmax)]
            LatticeInt i = {
                .min         = TB_MIN(a->_int.min, b->_int.min),
                .max         = TB_MAX(a->_int.max, b->_int.max),
                .known_zeros = a->_int.known_zeros & b->_int.known_zeros,
                .known_ones  = a->_int.known_ones  & b->_int.known_ones,
                .widen       = TB_MAX(a->_int.widen, b->_int.widen),
            };

            return lattice_intern(f, (Lattice){ LATTICE_INT, ._int = i });
        }

        // here's the cases we need to handle here for floats (a is rows):
        //   Fc means float constant, N is NaN and ~N is not-NaN
        //
        //    F  N ~N Fc
        //  F
        //  N F
        // ~N F  F
        // Fc *  *  *  *
        //
        // * Fc meeting with things has to worry about mismatching constants and which
        //   bucket the constant fits into (nans or not).
        case LATTICE_FLT32: case LATTICE_NAN32: case LATTICE_XNAN32: return &FLT32_IN_THE_SKY;
        case LATTICE_FLT64: case LATTICE_NAN64: case LATTICE_XNAN64: return &FLT64_IN_THE_SKY;
        case LATTICE_FLTCON32: if (b->tag == a->tag) {
            bool anan = isnan(a->_f32), bnan = isnan(b->_f32);
            return anan == bnan ? (anan ? &NAN32_IN_THE_SKY : &XNAN32_IN_THE_SKY) : &FLT32_IN_THE_SKY;
        } else {
            return &BOT_IN_THE_SKY;
        }
        case LATTICE_FLTCON64: if (b->tag == a->tag) {
            bool anan = isnan(a->_f64), bnan = isnan(b->_f64);
            return anan == bnan ? (anan ? &NAN64_IN_THE_SKY : &XNAN64_IN_THE_SKY) : &FLT64_IN_THE_SKY;
        } else {
            return &BOT_IN_THE_SKY;
        }

        // all cases that reached down here are bottoms
        case LATTICE_PTR:
        case LATTICE_NULL:
        return &PTR_IN_THE_SKY;

        // ~null ^ sym = ~null
        case LATTICE_XNULL: {
            if (b->tag == LATTICE_PTRCON) {
                return a;
            } else {
                return &BOT_IN_THE_SKY;
            }
        }

        // symA ^ symB = ~null
        case LATTICE_PTRCON: {
            if (b->tag == LATTICE_PTRCON) {
                assert(a->_ptr != b->_ptr);
                return &XNULL_IN_THE_SKY;
            } else {
                return &BOT_IN_THE_SKY;
            }
        }

        // already handled ctrl ^ ctrl, top ^ any, bot ^ any so everything else is bot
        case LATTICE_CTRL:
        return &BOT_IN_THE_SKY;

        case LATTICE_ALLMEM: return &ALLMEM_IN_THE_SKY;
        case LATTICE_ANYMEM: return b;
        case LATTICE_MEM_SLICE: {
            if (b->tag != LATTICE_MEM_SLICE) { return &BOT_IN_THE_SKY; }
            size_t alias_n = TB_MAX(a->_alias_n, b->_alias_n);

            size_t size = sizeof(Lattice) + alias_n*sizeof(Lattice*);
            Lattice* l = tb_arena_alloc(f->arena, size);
            *l = (Lattice){ LATTICE_MEM_SLICE, ._alias_n = alias_n };

            FOR_N(i, 0, a->_alias_n) { l->alias[i] = a->alias[i]; }
            FOR_N(i, a->_alias_n, alias_n) { l->alias[i] = 0; }
            FOR_N(i, 0, b->_alias_n) { l->alias[i] |= b->alias[i]; }

            Lattice* k = nl_hashset_put2(&f->type_interner, l, lattice_hash, lattice_cmp);
            if (k) {
                tb_arena_free(f->arena, l, size);
                return k;
            } else {
                return l;
            }
        }

        case LATTICE_TUPLE: {
            if (b->tag != LATTICE_TUPLE || a->_elem_count != b->_elem_count) {
                return &BOT_IN_THE_SKY;
            }

            size_t size = sizeof(Lattice) + a->_elem_count*sizeof(Lattice*);
            Lattice* l = tb_arena_alloc(f->arena, size);
            *l = (Lattice){ LATTICE_TUPLE, ._elem_count = a->_elem_count };
            FOR_N(i, 0, a->_elem_count) {
                l->elems[i] = lattice_meet(f, a->elems[i], b->elems[i]);
            }

            Lattice* k = nl_hashset_put2(&f->type_interner, l, lattice_hash, lattice_cmp);
            if (k) {
                tb_arena_free(f->arena, l, size);
                return k;
            } else {
                return l;
            }
        }

        default: tb_todo();
    }
}

// least upper bound between a and b
static Lattice* lattice_join(TB_Function* f, Lattice* a, Lattice* b) {
    a = lattice_dual(f, a);
    b = lattice_dual(f, b);
    return lattice_dual(f, lattice_meet(f, a, b));
}
