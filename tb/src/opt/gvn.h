#include "../tb_internal.h"

static size_t extra_bytes(TB_Node* n) {
    switch (n->type) {
        case TB_ICONST: return sizeof(TB_NodeInt);
        case TB_F32CONST: return sizeof(TB_NodeFloat32);
        case TB_F64CONST: return sizeof(TB_NodeFloat64);
        case TB_SYMBOL:        return sizeof(TB_NodeSymbol);
        case TB_LOCAL:         return sizeof(TB_NodeLocal);

        case TB_LOOKUP: {
            TB_NodeLookup* l = TB_NODE_GET_EXTRA(n);
            return sizeof(TB_NodeLookup) + (l->entry_count * sizeof(TB_LookupEntry));
        }

        case TB_BRANCH:
        case TB_AFFINE_LATCH:
        return sizeof(TB_NodeBranch);

        case TB_SAFEPOINT_POLL:
        return sizeof(TB_NodeSafepoint);

        case TB_AND:
        case TB_OR:
        case TB_XOR:
        case TB_ADD:
        case TB_SUB:
        case TB_MUL:
        case TB_SHL:
        case TB_SHR:
        case TB_SAR:
        case TB_ROL:
        case TB_ROR:
        case TB_UDIV:
        case TB_SDIV:
        case TB_UMOD:
        case TB_SMOD:
        return sizeof(TB_NodeBinopInt);

        case TB_MEMBER_ACCESS:
        return sizeof(TB_NodeMember);

        case TB_ARRAY_ACCESS:
        return sizeof(TB_NodeArray);

        case TB_CALLGRAPH:
        case TB_NEVER_BRANCH:
        case TB_TRUNCATE:
        case TB_UINT2FLOAT:
        case TB_FLOAT_TRUNC:
        case TB_FLOAT2UINT:
        case TB_TAG_INT2FLOAT:
        case TB_FLOAT2INT:
        case TB_FLOAT_EXT:
        case TB_SIGN_EXT:
        case TB_ZERO_EXT:
        case TB_BITCAST:
        case TB_FADD:
        case TB_FSUB:
        case TB_FMUL:
        case TB_FDIV:
        case TB_FMAX:
        case TB_FMIN:
        case TB_NEG:
        case TB_PHI:
        case TB_CLZ:
        case TB_CTZ:
        case TB_ADC:
        case TB_VA_START:
        case TB_POISON:
        case TB_SELECT:
        case TB_MERGEMEM:
        case TB_DEAD:
        case TB_NULL:
        case TB_UNREACHABLE:
        case TB_DEBUGBREAK:
        case TB_CYCLE_COUNTER:
        case TB_MULPAIR:
        case TB_READ:
        case TB_WRITE:
        case TB_ROOT:
        case TB_RETURN:
        case TB_MACH_MOVE:
        case TB_MACH_FRAME_PTR:
        return 0;

        case TB_MACH_SYMBOL:
        return sizeof(TB_NodeMachSymbol);

        case TB_SPLITMEM:
        return sizeof(TB_NodeMemSplit);

        case TB_REGION:
        case TB_NATURAL_LOOP:
        case TB_AFFINE_LOOP:
        return sizeof(TB_NodeRegion);

        case TB_CALL:
        case TB_SYSCALL:
        return sizeof(TB_NodeCall);

        case TB_TAILCALL:
        return sizeof(TB_NodeTailcall);

        case TB_LOAD:
        case TB_STORE:
        case TB_MEMCPY:
        case TB_MEMSET:
        return sizeof(TB_NodeMemAccess);

        case TB_ATOMIC_LOAD:
        case TB_ATOMIC_XCHG:
        case TB_ATOMIC_ADD:
        case TB_ATOMIC_SUB:
        case TB_ATOMIC_AND:
        case TB_ATOMIC_XOR:
        case TB_ATOMIC_OR:
        case TB_ATOMIC_CAS:
        return sizeof(TB_NodeAtomic);

        case TB_CMP_EQ:
        case TB_CMP_NE:
        case TB_CMP_ULT:
        case TB_CMP_ULE:
        case TB_CMP_SLT:
        case TB_CMP_SLE:
        case TB_CMP_FLT:
        case TB_CMP_FLE:
        return sizeof(TB_NodeCompare);

        case TB_PREFETCH:
        return sizeof(TB_NodePrefetch);

        case TB_PROJ:
        return sizeof(TB_NodeProj);

        case TB_BRANCH_PROJ:
        return sizeof(TB_NodeBranchProj);

        case TB_MACH_COPY:
        return sizeof(TB_NodeMachCopy);

        case TB_MACH_PROJ:
        return sizeof(TB_NodeMachProj);

        default: {
            int family = n->type / 0x100;
            assert(family >= 1 && family < TB_ARCH_MAX);
            return tb_codegen_families[family].extra_bytes(n);
        }
    }
}

uint32_t gvn_hash(void* a) {
    TB_Node* n = a;
    size_t extra = extra_bytes(n);
    uint32_t h = n->type + n->dt.raw + n->input_count + extra;

    // locals can't be put into the GVN table
    assert(n->type != TB_LOCAL);

    FOR_N(i, 0, n->input_count) {
        h += n->inputs[i] ? n->inputs[i]->gvn : 0;
    }

    // fnv1a the extra space
    uint32_t* extra_arr = (uint32_t*) n->extra;
    FOR_N(i, 0, extra / 4) {
        h += extra_arr[i];
    }

    FOR_N(i, extra & ~0x7, extra) {
        h += n->extra[i];
    }

    // fib hashing amirite
    return ((uint64_t) h * 11400714819323198485llu) >> 32llu;
}

bool gvn_compare(void* a, void* b) {
    TB_Node *x = a, *y = b;

    // early outs
    if (x->type != y->type || x->input_count != y->input_count || x->dt.raw != y->dt.raw) {
        return false;
    }

    // match up inputs
    FOR_N(i, 0, x->input_count) {
        if (x->inputs[i] != y->inputs[i]) {
            return false;
        }
    }

    size_t extra = extra_bytes(x);
    return extra == 0 || memcmp(x->extra, y->extra, extra) == 0;
}
