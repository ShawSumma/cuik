#include <hashes.h>

static Lattice TOP_IN_THE_SKY   = { LATTICE_TOP };
static Lattice BOT_IN_THE_SKY   = { LATTICE_BOT };
static Lattice CTRL_IN_THE_SKY  = { LATTICE_CTRL };
static Lattice XCTRL_IN_THE_SKY = { LATTICE_XCTRL };
static Lattice TUP_IN_THE_SKY   = { LATTICE_TUPLE };

static Lattice* lattice_from_dt(LatticeUniverse* uni, TB_DataType dt);

static uint32_t lattice_hash(void* a) {
    return tb__murmur3_32(a, sizeof(Lattice));
}

static bool lattice_cmp(void* a, void* b) {
    Lattice *aa = a, *bb = b;
    return aa->tag == bb->tag ? memcmp(aa, bb, sizeof(Lattice)) == 0 : false;
}

static bool lattice_is_const_int(Lattice* l) { return l->_int.min == l->_int.max; }
static bool lattice_is_const(Lattice* l) { return l->tag == LATTICE_INT && l->_int.min == l->_int.max; }

static void lattice_universe_grow(LatticeUniverse* uni, size_t top) {
    size_t new_cap = tb_next_pow2(top + 16);
    uni->types = tb_platform_heap_realloc(uni->types, new_cap * sizeof(Lattice*));

    // clear new space
    FOREACH_N(i, uni->type_cap, new_cap) {
        uni->types[i] = &TOP_IN_THE_SKY;
    }

    uni->type_cap = new_cap;
}

static bool lattice_universe_map_progress(LatticeUniverse* uni, TB_Node* n, Lattice* l) {
    // reserve cap, slow path :p
    if (UNLIKELY(n->gvn >= uni->type_cap)) {
        lattice_universe_grow(uni, n->gvn);
    }

    Lattice* old = uni->types[n->gvn];
    uni->types[n->gvn] = l;
    return old != l;
}

static void lattice_universe_map(LatticeUniverse* uni, TB_Node* n, Lattice* l) {
    // reserve cap, slow path :p
    if (UNLIKELY(n->gvn >= uni->type_cap)) {
        lattice_universe_grow(uni, n->gvn);
    }

    uni->types[n->gvn] = l;
}

Lattice* lattice_universe_get(LatticeUniverse* uni, TB_Node* n) {
    // reserve cap, slow path :p
    if (UNLIKELY(n->gvn >= uni->type_cap)) {
        lattice_universe_grow(uni, n->gvn);
    }

    assert(uni->types[n->gvn] != NULL);
    return uni->types[n->gvn];
}

static Lattice* lattice_intern(LatticeUniverse* uni, Lattice l) {
    Lattice* k = nl_hashset_get2(&uni->pool, &l, lattice_hash, lattice_cmp);
    if (k != NULL) {
        return k;
    }

    // allocate new node
    k = tb_arena_alloc(uni->arena, sizeof(Lattice));
    memcpy(k, &l, sizeof(l));
    nl_hashset_put2(&uni->pool, k, lattice_hash, lattice_cmp);
    return k;
}

LatticeTrifecta lattice_truthy(Lattice* l) {
    switch (l->tag) {
        case LATTICE_INT:
        if (l->_int.min == l->_int.max) {
            return l->_int.min ? LATTICE_KNOWN_TRUE : LATTICE_KNOWN_FALSE;
        }
        return LATTICE_UNKNOWN;

        case LATTICE_FLOAT32:
        case LATTICE_FLOAT64:
        return LATTICE_UNKNOWN;

        case LATTICE_POINTER:
        return l->_ptr.trifecta;

        default:
        return LATTICE_UNKNOWN;
    }
}

static int64_t lattice_int_min(int bits) { return 1ll << (bits - 1); }
static int64_t lattice_int_max(int bits) { return (1ll << (bits - 1)) - 1; }
static uint64_t lattice_uint_max(int bits) { return UINT64_MAX >> (64 - bits); }

static Lattice* lattice_from_dt(LatticeUniverse* uni, TB_DataType dt) {
    switch (dt.type) {
        case TB_INT: {
            assert(dt.data <= 64);
            return lattice_intern(uni, (Lattice){ LATTICE_INT, ._int = { 0, lattice_uint_max(dt.data) } });
        }

        case TB_FLOAT: {
            assert(dt.data == TB_FLT_32 || dt.data == TB_FLT_64);
            return lattice_intern(uni, (Lattice){ dt.data == TB_FLT_64 ? LATTICE_FLOAT64 : LATTICE_FLOAT32, ._float = { LATTICE_UNKNOWN } });
        }

        case TB_PTR: return lattice_intern(uni, (Lattice){ LATTICE_POINTER, ._ptr = { LATTICE_UNKNOWN } });
        case TB_CONTROL: return &CTRL_IN_THE_SKY;
        case TB_TUPLE: return &TUP_IN_THE_SKY;
        default: return &BOT_IN_THE_SKY;
    }
}

// known X ^ known X => known X or
// known X ^ unknown => unknown (commutative btw)
#define TRIFECTA_MEET(a, b) ((a).trifecta == (b).trifecta ? (a).trifecta : LATTICE_UNKNOWN)

#define MASK_UPTO(pos) (~UINT64_C(0) >> (64 - pos))
#define BEXTR(src,pos) (((src) >> (pos)) & 1)
uint64_t tb__sxt(uint64_t src, uint64_t src_bits, uint64_t dst_bits) {
    uint64_t sign_bit = BEXTR(src, src_bits-1);
    uint64_t mask = MASK_UPTO(dst_bits) & ~MASK_UPTO(src_bits);

    uint64_t dst = src & ~mask;
    return dst | (sign_bit ? mask : 0);
}

static bool lattice_signed(LatticeInt* l) { return l->min > l->max; }

static LatticeInt lattice_into_unsigned(LatticeInt i) {
    if (i.min > i.max) { SWAP(uint64_t, i.min, i.max); }
    return i;
}

static Lattice* lattice_gimme_int(LatticeUniverse* uni, int64_t min, int64_t max) {
    assert(min <= max);
    return lattice_intern(uni, (Lattice){ LATTICE_INT, ._int = { min, max } });
}

static Lattice* lattice_gimme_uint(LatticeUniverse* uni, uint64_t min, uint64_t max) {
    assert(min <= max);
    return lattice_intern(uni, (Lattice){ LATTICE_INT, ._int = { min, max } });
}

static bool l_add_overflow(uint64_t x, uint64_t y, uint64_t mask, uint64_t* out) {
    *out = (x + y) & mask;
    return x && *out < x;
}

static bool l_mul_overflow(uint64_t x, uint64_t y, uint64_t mask, uint64_t* out) {
    *out = (x * y) & mask;
    return x && *out < x;
}

static bool l_sub_overflow(uint64_t x, uint64_t y, uint64_t mask, uint64_t* out) {
    *out = (x - y) & mask;
    return x && *out > x;
}

static bool wrapped_int_lt(int64_t x, int64_t y, int bits) {
    return (int64_t)tb__sxt(x, bits, 64) < (int64_t)tb__sxt(y, bits, 64);
}

static LatticeInt lattice_meet_int(LatticeInt a, LatticeInt b, TB_DataType dt) {
    // [amin, amax] ^ [bmin, bmax] => [min(amin, bmin), max(amax, bmax)]
    int bits = dt.data;
    uint64_t mask = tb__mask(dt.data);

    bool aas = a.min > a.max;
    bool bbs = b.min > b.max;
    if (aas && bbs) {
        if (wrapped_int_lt(b.min, a.min, bits)) a.min = b.min;
        if (wrapped_int_lt(a.max, b.max, bits)) a.max = b.max;
    } else {
        if (!aas && !bbs) {
            a = lattice_into_unsigned(a);
            b = lattice_into_unsigned(b);
        }

        if (b.min < a.min) a.min = b.min;
        if (a.max < b.max) a.max = b.max;
    }

    a.known_zeros &= b.known_zeros;
    a.known_ones &= b.known_ones;
    return a;
}

// generates the greatest lower bound between a and b
static Lattice* lattice_meet(LatticeUniverse* uni, Lattice* a, Lattice* b, TB_DataType dt) {
    // it's commutative, so let's simplify later code this way
    if (a->tag > b->tag) {
        SWAP(Lattice*, a, b);
    }

    switch (a->tag) {
        case LATTICE_BOT: return &BOT_IN_THE_SKY;
        case LATTICE_TOP: return &TOP_IN_THE_SKY;

        case LATTICE_CTRL:
        case LATTICE_XCTRL: {
            // ctrl  ^ ctrl   = ctrl
            // ctrl  ^ xctrl  = bot
            // xctrl ^ xctrl  = xctrl
            return a == b ? a : &BOT_IN_THE_SKY;
        }

        case LATTICE_INT: {
            if (b->tag != LATTICE_INT) {
                return &BOT_IN_THE_SKY;
            }

            LatticeInt i = lattice_meet_int(a->_int, b->_int, dt);
            return lattice_intern(uni, (Lattice){ LATTICE_INT, ._int = i });
        }

        case LATTICE_FLOAT32:
        case LATTICE_FLOAT64: {
            if (b->tag != a->tag) {
                return &BOT_IN_THE_SKY;
            }

            LatticeFloat f = { .trifecta = TRIFECTA_MEET(a->_float, b->_float) };
            return lattice_intern(uni, (Lattice){ a->tag, ._float = f });
        }

        case LATTICE_POINTER: {
            if (b->tag != LATTICE_INT) {
                return &BOT_IN_THE_SKY;
            }

            LatticePointer p = { .trifecta = TRIFECTA_MEET(a->_ptr, b->_ptr) };
            return lattice_intern(uni, (Lattice){ LATTICE_POINTER, ._ptr = p });
        }

        default: tb_todo();
    }
}
