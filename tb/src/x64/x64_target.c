#include "x64.h"
#include <tb_x64.h>
#include "x64_emitter.h"
#include "x64_disasm.c"

enum {
    // register classes
    REG_CLASS_GPR,
    REG_CLASS_XMM,
    REG_CLASS_COUNT,
};

#include "../codegen_impl.h"

enum {
    //   OP reg, imm
    TILE_HAS_IMM = 1,

    // mov rax, [LOCAL/GLOBAL]
    TILE_FOLDED_BASE = 2,

    TILE_INDEXED = 4,

    //   cmp a, b
    //   jcc cond
    TILE_CMP_JCC = 8,
};

typedef struct {
    TB_Node *base;
    int stride;
    int offset;
} AuxAddress;

static const struct ParamDesc {
    int chkstk_limit;
    int gpr_count;
    int xmm_count;
    uint16_t caller_saved_xmms; // XMM0 - XMMwhatever
    uint16_t caller_saved_gprs; // bitfield

    GPR gprs[6];
} param_descs[] = {
    // win64
    { 4096,    4, 4, 6, WIN64_ABI_CALLER_SAVED,   { RCX, RDX, R8,  R9,  0,  0 } },
    // system v
    { INT_MAX, 6, 4, 5, SYSV_ABI_CALLER_SAVED,    { RDI, RSI, RDX, RCX, R8, R9 } },
    // syscall
    { INT_MAX, 6, 4, 5, SYSCALL_ABI_CALLER_SAVED, { RDI, RSI, RDX, R10, R8, R9 } },
};

enum {
    ALL_GPRS            = 0xFFFF & ~((1 << RBP) | (1 << RSP)),
    ALL_GPRS_NO_RAX_RDX = 0xFFFF & ~((1 << RBP) | (1 << RSP) | (1 << RAX) | (1 << RDX)),
    ALL_GPRS_NO_RCX     = 0xFFFF & ~((1 << RBP) | (1 << RSP) | (1 << RCX)),
};

// *out_mask of 0 means no mask
static TB_X86_DataType legalize_int(TB_DataType dt, uint64_t* out_mask) {
    assert(dt.type == TB_INT || dt.type == TB_PTR);
    if (dt.type == TB_PTR) return *out_mask = 0, TB_X86_TYPE_QWORD;

    TB_X86_DataType t = TB_X86_TYPE_NONE;
    int bits = 0;

    if (dt.data <= 8) bits = 8, t = TB_X86_TYPE_BYTE;
    else if (dt.data <= 16) bits = 16, t = TB_X86_TYPE_WORD;
    else if (dt.data <= 32) bits = 32, t = TB_X86_TYPE_DWORD;
    else if (dt.data <= 64) bits = 64, t = TB_X86_TYPE_QWORD;

    assert(bits != 0 && "TODO: large int support");
    uint64_t mask = dt.data == 0 ? 0 :  ~UINT64_C(0) >> (64 - dt.data);

    *out_mask = (dt.data == bits) ? 0 : mask;
    return t;
}

static TB_X86_DataType legalize_int2(TB_DataType dt) {
    uint64_t m;
    return legalize_int(dt, &m);
}

static TB_X86_DataType legalize_float(TB_DataType dt) {
    assert(dt.type == TB_FLOAT);
    return (dt.data == TB_FLT_64 ? TB_X86_TYPE_SSE_SD : TB_X86_TYPE_SSE_SS);
}

static TB_X86_DataType legalize(TB_DataType dt) {
    if (dt.type == TB_FLOAT) {
        return legalize_float(dt);
    } else {
        uint64_t m;
        return legalize_int(dt, &m);
    }
}

static bool try_for_imm32(int bits, TB_Node* n, int32_t* out_x) {
    if (n->type != TB_INTEGER_CONST) {
        return false;
    }

    TB_NodeInt* i = TB_NODE_GET_EXTRA(n);
    if (bits > 32) {
        bool sign = (i->value >> 31ull) & 1;
        uint64_t top = i->value >> 32ull;

        // if the sign matches the rest of the top bits, we can sign extend just fine
        if (top != (sign ? 0xFFFFFFFF : 0)) {
            return false;
        }
    }

    *out_x = i->value;
    return true;
}

static void init_ctx(Ctx* restrict ctx, TB_ABI abi) {
    ctx->sched = greedy_scheduler;
    ctx->regalloc = tb__lsra;

    ctx->abi_index = abi == TB_ABI_SYSTEMV ? 1 : 0;

    // currently only using 16 GPRs and 16 XMMs, AVX gives us
    // 32 YMMs (which double as XMMs) and later on APX will do
    // 32 GPRs.
    ctx->num_regs[0] = 16;
    ctx->num_regs[1] = 16;

    uint16_t all_gprs = 0xFFFF & ~(1 << RSP);
    if (ctx->features.gen & TB_FEATURE_FRAME_PTR) {
        all_gprs &= ~(1 << RBP);
    }

    ctx->normie_mask[0] = REGMASK(GPR, all_gprs);
    ctx->normie_mask[1] = REGMASK(GPR, (1u << 16) - 1);

    // mark GPR callees (technically includes RSP but since it's
    // never conventionally allocated we should never run into issues)
    ctx->callee_saved[0] = ~param_descs[ctx->abi_index].caller_saved_gprs;

    // mark XMM callees
    ctx->callee_saved[1] = 0;
    FOREACH_N(i, param_descs[ctx->abi_index].caller_saved_xmms, 16) {
        ctx->callee_saved[1] |= (1ull << i);
    }

    TB_FunctionPrototype* proto = ctx->f->prototype;
    if (proto->has_varargs) {
        const GPR* parameter_gprs = param_descs[ctx->abi_index].gprs;

        // spill the rest of the parameters (assumes they're all in the GPRs)
        size_t gpr_count = param_descs[ctx->abi_index].gpr_count;
        size_t extra_param_count = proto->param_count > gpr_count ? 0 : gpr_count - proto->param_count;
        ctx->stack_usage += (extra_param_count * 8);
    }
}

static RegMask normie_mask(Ctx* restrict ctx, TB_DataType dt) {
    return ctx->normie_mask[dt.type == TB_FLOAT];
}

// x86 can do a lot of fancy address computation crap in one operand so we'll
// track that tiling here.
//
// in_count is all the inputs that go alongside this operand
static TileInput* isel_addr(Ctx* restrict ctx, Tile* t, TB_Node* n, int extra_cnt) {
    int32_t offset = 0;
    TB_Node* base = n;
    TB_Node* index = NULL;
    int64_t stride = 0;
    bool has_tmp = 0;

    if (base->type == TB_MEMBER_ACCESS) {
        offset = TB_NODE_GET_EXTRA_T(n, TB_NodeMember)->offset;
        base = base->inputs[1];
    }

    if (base->type == TB_ARRAY_ACCESS) {
        stride = TB_NODE_GET_EXTRA_T(base, TB_NodeArray)->stride;
        index = base->inputs[2];
        base = base->inputs[1];

        if (stride == 1) {
            // no scaling required
        } else if (tb_is_power_of_two(stride)) {
            int scale = tb_ffs(stride) - 1;

            // we can only fit a 2bit shift amount in an LEA, after
            // that we just defer to an explicit shift op.
            if (scale > 3) {
                has_tmp = true;
            }
        } else {
            // needs a proper multiply (we may wanna invest in a few special patterns
            // for reducing simple multiplies into shifts)
            //
            //   a * 24 => (a * 8) * 3
            //                b    * 3 => b<<1 + b
            //
            // thus
            //
            //   LEA b,   [a * 8]
            //   LEA dst, [b * 2 + b]
            has_tmp = true;
        }
    }

    int in_cap = extra_cnt + (index?1:0);
    if (base->type != TB_LOCAL && base->type != TB_SYMBOL) {
        in_cap += 1;
    }

    // construct tile now
    t->ins = tb_arena_alloc(tmp_arena, in_cap * sizeof(TileInput));
    t->in_count = in_cap;

    int in_count = 0;
    if (base->type == TB_LOCAL) {
        try_init_stack_slot(ctx, base);
        t->flags |= TILE_FOLDED_BASE;
    } else if (base->type == TB_SYMBOL) {
        t->flags |= TILE_FOLDED_BASE;
    } else {
        t->ins[in_count].src = get_tile(ctx, base, true)->interval;
        t->ins[in_count].mask = ctx->normie_mask[0];
        in_count++;
    }

    if (index) {
        t->ins[in_count].src = get_tile(ctx, index, true)->interval;
        t->ins[in_count].mask = ctx->normie_mask[0];
        t->flags |= TILE_INDEXED;
        in_count++;
    }

    AuxAddress* aux = tb_arena_alloc(tmp_arena, sizeof(AuxAddress));
    aux->base = base;
    aux->stride = stride;
    aux->offset = offset;
    t->aux = aux;

    return &t->ins[in_cap - extra_cnt];
}

static RegMask isel_node(Ctx* restrict ctx, Tile* dst, TB_Node* n) {
    switch (n->type) {
        // no inputs
        case TB_START:
        case TB_REGION:
        case TB_TRAP:
        case TB_UNREACHABLE:
        case TB_DEBUGBREAK:
        case TB_SAFEPOINT_NOP:
        return REGEMPTY;

        case TB_LOCAL: {
            TB_NodeLocal* local = TB_NODE_GET_EXTRA(n);
            try_init_stack_slot(ctx, n);
            return ctx->normie_mask[0];
        }

        case TB_VA_START: {
            assert(ctx->module->target_abi == TB_ABI_WIN64 && "How does va_start even work on SysV?");

            // on Win64 va_start just means whatever is one parameter away from
            // the parameter you give it (plus in Win64 the parameters in the stack
            // are 8bytes, no fanciness like in SysV):
            // void printf(const char* fmt, ...) {
            //     va_list args;
            //     va_start(args, fmt); // args = ((char*) &fmt) + 8;
            //     ...
            // }
            return ctx->normie_mask[0];
        }

        case TB_LOAD:
        isel_addr(ctx, dst, n->inputs[2], 0);
        return ctx->normie_mask[0];

        case TB_STORE: {
            TileInput* ins = isel_addr(ctx, dst, n->inputs[2], 1);
            ins[0].src = get_tile(ctx, n->inputs[3], true)->interval;
            ins[0].mask = normie_mask(ctx, n->inputs[3]->dt);
            return REGEMPTY;
        }

        case TB_ARRAY_ACCESS:
        case TB_MEMBER_ACCESS:
        isel_addr(ctx, dst, n, 0);
        return ctx->normie_mask[0];

        case TB_POISON:
        case TB_SIGN_EXT:
        case TB_ZERO_EXT:
        case TB_INT2PTR:
        case TB_PTR2INT:
        case TB_TRUNCATE:
        case TB_INT2FLOAT:
        case TB_FLOAT2INT:
        case TB_UINT2FLOAT:
        case TB_FLOAT2UINT:
        tile_broadcast_ins(ctx, dst, n, 1, 2, normie_mask(ctx, n->inputs[1]->dt));
        return normie_mask(ctx, n->dt);

        case TB_SYMBOL:
        return ctx->normie_mask[0];

        case TB_PHI:
        if (n->dt.type == TB_MEMORY) return REGEMPTY;
        else return normie_mask(ctx, n->dt);

        case TB_END: {
            static int ret_gprs[2] = { RAX, RDX };

            int rets = n->input_count - 3;
            TileInput* ins = tile_set_ins(ctx, dst, n, 3, n->input_count);

            assert(rets <= 2 && "At most 2 return values :(");
            FOREACH_N(i, 0, rets) {
                TB_DataType dt = n->inputs[3+i]->dt;
                if (dt.type == TB_FLOAT) {
                    ins[i].mask = REGMASK(XMM, 1 << i);
                } else {
                    ins[i].mask = REGMASK(GPR, 1 << ret_gprs[i]);
                }
            }
            return REGEMPTY;
        }

        case TB_PROJ: {
            int i = TB_NODE_GET_EXTRA_T(n, TB_NodeProj)->index;
            if (n->inputs[0]->type == TB_START) {
                // function params are ABI crap
                const struct ParamDesc* params = &param_descs[ctx->abi_index];
                return REGMASK(GPR, i >= 3 ? (1u << params->gprs[i - 3]) : 0);
            } else if (n->inputs[0]->type == TB_CALL) {
                if (i == 2) return REGMASK(GPR, 1 << RAX);
                else if (i == 3) return REGMASK(GPR, 1 << RDX);
                else return REGEMPTY;
            } else if (n->inputs[0]->type == TB_BRANCH) {
                return REGEMPTY;
            } else {
                tb_todo();
            }
        }

        // binary ops
        case TB_AND:
        case TB_OR:
        case TB_XOR:
        case TB_ADD:
        case TB_SUB:
        case TB_MUL: {
            int32_t x;
            if (try_for_imm32(n->dt.data, n->inputs[2], &x)) {
                tile_broadcast_ins(ctx, dst, n, 1, 2, ctx->normie_mask[0]);
                dst->flags |= TILE_HAS_IMM;
            } else {
                tile_broadcast_ins(ctx, dst, n, 1, n->input_count, ctx->normie_mask[0]);
            }
            return ctx->normie_mask[0];
        }

        case TB_SHL:
        case TB_SHR:
        case TB_SAR: {
            int32_t x;
            if (try_for_imm32(n->inputs[2]->dt.data, n->inputs[2], &x) && x >= 0 && x < 64) {
                tile_broadcast_ins(ctx, dst, n, 1, 2, ctx->normie_mask[0]);
                dst->flags |= TILE_HAS_IMM;
                return ctx->normie_mask[0];
            } else {
                TileInput* ins = tile_set_ins(ctx, dst, n, 1, 3);
                ins[0].mask = REGMASK(GPR, ALL_GPRS_NO_RCX);
                ins[1].mask = REGMASK(GPR, 1 << RCX);
                return ctx->normie_mask[0];
            }
        }

        case TB_UDIV:
        case TB_SDIV:
        case TB_UMOD:
        case TB_SMOD: {
            TileInput* ins = tile_set_ins(ctx, dst, n, 1, 3);
            ins[0].mask = REGMASK(GPR, 1 << RAX);
            ins[1].mask = ctx->normie_mask[0];
            if (n->type == TB_UDIV || n->type == TB_SDIV) {
                return REGMASK(GPR, 1 << RAX);
            } else {
                return REGMASK(GPR, 1 << RDX);
            }
        }

        case TB_INTEGER_CONST:
        return ctx->normie_mask[0];

        case TB_BRANCH: {
            TB_Node* cmp = n->inputs[1];
            if (cmp->type >= TB_CMP_EQ && cmp->type <= TB_CMP_FLE) {
                dst->flags |= TILE_CMP_JCC;

                TB_DataType cmp_dt = TB_NODE_GET_EXTRA_T(n->inputs[1], TB_NodeCompare)->cmp_dt;
                RegMask rm = normie_mask(ctx, cmp_dt);

                int32_t x;
                if (try_for_imm32(cmp_dt.type == TB_PTR ? 64 : cmp_dt.data, cmp->inputs[2], &x)) {
                    tile_broadcast_ins(ctx, dst, cmp, 1, 2, rm);
                    dst->flags |= TILE_HAS_IMM;
                } else {
                    tile_broadcast_ins(ctx, dst, cmp, 1, 3, rm);
                }
            } else {
                TileInput* ins = tile_set_ins(ctx, dst, n, 1, n->input_count);
                ins[0].mask = normie_mask(ctx, n->inputs[1]->dt);
            }
            return REGEMPTY;
        }

        case TB_CALL: {
            int end_of_reg_params = n->input_count > 7 ? 7 : n->input_count;

            // system calls don't count, we track this for ABI
            // and stack allocation purposes.
            int param_count = n->input_count - 3;
            if (ctx->caller_usage < param_count) {
                ctx->caller_usage = param_count;
            }

            TileInput* ins;
            if (n->inputs[2]->type == TB_SYMBOL) {
                // CALL symbol
                ins = tile_set_ins(ctx, dst, n, 3, n->input_count);
            } else {
                // CALL r/m
                ins = tile_set_ins(ctx, dst, n, 2, n->input_count);
                ins[0].mask = ctx->normie_mask[0];
                ins += 1;
            }

            const struct ParamDesc* abi = &param_descs[ctx->abi_index];
            FOREACH_N(i, 0, param_count) {
                if (i < 4) {
                    ins[i].mask = REGMASK(GPR, 1u << abi->gprs[i]);
                } else {
                    ins[i].mask = ctx->normie_mask[0];
                }
            }

            return REGEMPTY;
        }

        default:
        tb_todo();
        return REGEMPTY;
    }
}

static void emit_epilogue(Ctx* restrict ctx, TB_CGEmitter* e, int stack_usage) {
    // add rsp, N
    if (stack_usage > 0) {
        if (stack_usage == (int8_t)stack_usage) {
            EMIT1(&ctx->emit, rex(true, 0x00, RSP, 0));
            EMIT1(&ctx->emit, 0x83);
            EMIT1(&ctx->emit, mod_rx_rm(MOD_DIRECT, 0x00, RSP));
            EMIT1(&ctx->emit, (int8_t) stack_usage);
        } else {
            EMIT1(&ctx->emit, rex(true, 0x00, RSP, 0));
            EMIT1(&ctx->emit, 0x81);
            EMIT1(&ctx->emit, mod_rx_rm(MOD_DIRECT, 0x00, RSP));
            EMIT4(&ctx->emit, stack_usage);
        }
    }

    // pop rbp (if we even used the frameptr)
    if ((ctx->features.gen & TB_FEATURE_FRAME_PTR) && stack_usage > 0) {
        EMIT1(&ctx->emit, 0x58 + RBP);
    }
}

static Val op_at(Ctx* ctx, LiveInterval* l) {
    if (l->is_spill) {
        return val_stack(ctx->stack_usage - ctx->spills[l->id]);
    } else {
        assert(l->assigned >= 0);
        return val_gpr(l->assigned);
    }
}

static GPR op_gpr_at(LiveInterval* l) {
    assert(!l->is_spill);
    return l->assigned;
}

static Val op_indirect_at(LiveInterval* l) {
    assert(!l->is_spill);
    return val_base_disp(l->assigned, 0);
}

static bool clobbers(Ctx* restrict ctx, Tile* t, uint64_t clobbers[MAX_REG_CLASSES]) {
    if (t->n) switch (t->n->type) {
        case TB_UDIV:
        case TB_SDIV:
        case TB_UMOD:
        case TB_SMOD:
        clobbers[0] = 1u << RDX;
        clobbers[1] = 0;
        return true;
    }

    return false;
}

static Val parse_memory_op(Ctx* restrict ctx, TB_CGEmitter* e, Tile* t, TB_Node* addr) {
    Val ptr;
    if (addr->type == TB_LOCAL) {
        int pos = get_stack_slot(ctx, addr);
        ptr = val_stack(ctx->stack_usage - pos);
    } else if (addr->type == TB_SYMBOL) {
        TB_Symbol* sym = TB_NODE_GET_EXTRA_T(addr, TB_NodeSymbol)->sym;
        ptr = val_global(sym, 0);
    } else {
        tb_todo();
    }

    return ptr;
}

static void pre_emit(Ctx* restrict ctx, TB_CGEmitter* e) {
    size_t caller_usage = 0;
    if (ctx->abi_index == 0) {
        caller_usage = ctx->caller_usage;
        if (caller_usage > 0 && caller_usage < 4) {
            caller_usage = 4;
        }
    }

    size_t usage = ctx->stack_usage + (caller_usage * 8);

    // Align stack usage to 16bytes + 8 to accommodate for the RIP being pushed by CALL
    ctx->stack_usage = align_up(usage, 16) + 8;
}

// compute effective address operand
static Val emit_addr(Ctx* restrict ctx, TB_CGEmitter* e, Tile* t) {
    int in_count = 0;
    Val ea = { .type = VAL_MEM, .index = GPR_NONE };
    AuxAddress* aux = t->aux;
    if (t->flags & TILE_FOLDED_BASE) {
        if (aux->base->type == TB_LOCAL) {
            int pos = get_stack_slot(ctx, aux->base);
            ea.reg = RSP;
            ea.imm = ctx->stack_usage - pos;
        } else {
            tb_todo();
        }
    } else {
        ea.reg = op_at(ctx, t->ins[in_count].src).reg;
        in_count++;
    }

    if (t->flags & TILE_INDEXED) {
        int index = op_gpr_at(t->ins[in_count++].src);

        int stride = aux->stride;
        if (tb_is_power_of_two(stride)) {
            int scale = tb_ffs(stride) - 1;
            if (scale > 3) {
                int dst = op_gpr_at(t->interval);
                Val dst_op = val_gpr(dst);
                if (dst != index) {
                    Val index_op = val_gpr(index);
                    inst2(e, MOV, &dst_op, &index_op, TB_X86_TYPE_QWORD);
                }

                Val imm = val_imm(scale);
                inst2(e, SHL, &dst_op, &imm, TB_X86_TYPE_QWORD);
                index = dst;
            }
        } else {
            __debugbreak();
        }

        ea.index = index;
    }

    ea.imm += aux->offset;
    return ea;
}

static void emit_tile(Ctx* restrict ctx, TB_CGEmitter* e, Tile* t) {
    if (t->tag == TILE_SPILL_MOVE) {
        Val dst = op_at(ctx, t->interval);
        Val src = op_at(ctx, t->ins[0].src);
        if (!is_value_match(&dst, &src)) {
            inst2(e, MOV, &dst, &src, TB_X86_TYPE_QWORD);
        }
    } else if (t->tag == TILE_GOTO) {
        MachineBB* mbb = node_to_bb(ctx, t->succ);
        if (ctx->fallthrough != mbb->id) {
            EMIT1(e, 0xE9); EMIT4(e, 0);
            tb_emit_rel32(e, &e->labels[mbb->id], GET_CODE_POS(e) - 4);
        }
    } else {
        TB_Node* n = t->n;
        switch (n->type) {
            // prologue
            case TB_START: {
                int stack_usage = ctx->stack_usage;

                // save frame pointer (if applies)
                if ((ctx->features.gen & TB_FEATURE_FRAME_PTR) && stack_usage > 0) {
                    EMIT1(e, 0x50 + RBP);

                    // mov rbp, rsp
                    EMIT1(e, rex(true, RSP, RBP, 0));
                    EMIT1(e, 0x89);
                    EMIT1(e, mod_rx_rm(MOD_DIRECT, RSP, RBP));
                }

                // inserts a chkstk call if we use too much stack
                if (stack_usage >= param_descs[ctx->abi_index].chkstk_limit) {
                    assert(ctx->f->super.module->chkstk_extern);
                    Val sym = val_global(ctx->f->super.module->chkstk_extern, 0);
                    Val imm = val_imm(stack_usage);
                    Val rax = val_gpr(RAX);
                    Val rsp = val_gpr(RSP);

                    inst2(e, MOV, &rax, &imm, TB_X86_TYPE_DWORD);
                    inst1(e, CALL, &sym, TB_X86_TYPE_QWORD);
                    inst2(e, SUB, &rsp, &rax, TB_X86_TYPE_QWORD);
                } else if (stack_usage > 0) {
                    if (stack_usage == (int8_t)stack_usage) {
                        // sub rsp, stack_usage
                        EMIT1(e, rex(true, 0x00, RSP, 0));
                        EMIT1(e, 0x83);
                        EMIT1(e, mod_rx_rm(MOD_DIRECT, 0x05, RSP));
                        EMIT1(e, stack_usage);
                    } else {
                        // sub rsp, stack_usage
                        EMIT1(e, rex(true, 0x00, RSP, 0));
                        EMIT1(e, 0x81);
                        EMIT1(e, mod_rx_rm(MOD_DIRECT, 0x05, RSP));
                        EMIT4(e, stack_usage);
                    }
                }

                // handle unknown parameters (if we have varargs)
                TB_FunctionPrototype* proto = ctx->f->prototype;
                if (proto->has_varargs) {
                    const GPR* parameter_gprs = param_descs[ctx->abi_index].gprs;

                    // spill the rest of the parameters (assumes they're all in the GPRs)
                    size_t gpr_count = param_descs[ctx->abi_index].gpr_count;
                    size_t extra_param_count = proto->param_count > gpr_count ? 0 : gpr_count - proto->param_count;

                    FOREACH_N(i, 0, extra_param_count) {
                        size_t param_num = proto->param_count + i;

                        int dst_pos = 16 + (param_num * 8);
                        Val src = val_gpr(parameter_gprs[param_num]);

                        Val dst = val_base_disp(RSP, ctx->stack_usage - dst_pos);
                        inst2(e, MOV, &dst, &src, TB_X86_TYPE_QWORD);
                    }
                }
                ctx->prologue_length = e->count;
                break;
            }
            // epilogue
            case TB_END: {
                size_t pos = e->count;
                emit_epilogue(ctx, e, ctx->stack_usage);
                EMIT1(&ctx->emit, 0xC3);
                ctx->epilogue_length = e->count - pos;
                break;
            }
            case TB_TRAP: {
                EMIT1(&ctx->emit, 0x0F);
                EMIT1(&ctx->emit, 0x0B);
                break;
            }
            case TB_DEBUGBREAK: {
                EMIT1(&ctx->emit, 0xCC);
                break;
            }
            // projections don't manage their own work, that's the
            // TUPLE node's job.
            case TB_PROJ: break;
            case TB_REGION: break;
            case TB_PHI: break;
            case TB_UNREACHABLE: break;

            case TB_LOAD: {
                Val dst = op_at(ctx, t->interval);
                Val ea = emit_addr(ctx, e, t);
                inst2(e, MOV, &dst, &ea, legalize_int2(n->dt));
                break;
            }
            case TB_STORE: {
                Val ea = emit_addr(ctx, e, t);
                Val src;
                if (t->flags & TILE_HAS_IMM) {
                    assert(n->inputs[2]->type == TB_INTEGER_CONST);
                    TB_NodeInt* i = TB_NODE_GET_EXTRA(n->inputs[2]);
                    src = val_imm(i->value);
                } else {
                    src = op_at(ctx, t->ins[t->in_count - 1].src);
                }

                inst2(e, MOV, &ea, &src, legalize_int2(n->inputs[2]->dt));
                break;
            }
            case TB_LOCAL:
            case TB_MEMBER_ACCESS:
            case TB_ARRAY_ACCESS: {
                Val dst = op_at(ctx, t->interval);
                Val ea = emit_addr(ctx, e, t);
                inst2(e, LEA, &dst, &ea, TB_X86_TYPE_QWORD);
                break;
            }
            case TB_VA_START: {
                TB_FunctionPrototype* proto = ctx->f->prototype;
                size_t gpr_count = param_descs[ctx->abi_index].gpr_count;

                Val dst = op_at(ctx, t->interval);
                Val ea = val_stack(ctx->stack_usage - (16 + proto->param_count*8));
                inst2(e, LEA, &dst, &ea, TB_X86_TYPE_QWORD);
                break;
            }

            case TB_SAFEPOINT_NOP: {
                TB_NodeSafepoint* loc = TB_NODE_GET_EXTRA(n);
                TB_Location l = {
                    .file = loc->file,
                    .line = loc->line,
                    .column = loc->column,
                    .pos = GET_CODE_POS(e)
                };

                size_t top = dyn_array_length(ctx->locations);
                if (top == 0 || (ctx->locations[top - 1].pos != l.pos && !is_same_location(&l, &ctx->locations[top - 1]))) {
                    dyn_array_put(ctx->locations, l);
                }
                break;
            }

            case TB_INTEGER_CONST: {
                uint64_t x = TB_NODE_GET_EXTRA_T(n, TB_NodeInt)->value;
                TB_X86_DataType dt = legalize_int2(n->dt);

                Val dst = op_at(ctx, t->interval);
                if (x == 0) {
                    // xor reg, reg
                    inst2(e, XOR, &dst, &dst, dt);
                } else {
                    Val src = val_imm(x);
                    inst2(e, MOV, &dst, &src, dt);
                }
                break;
            }
            case TB_SIGN_EXT: {
                // unconventional sizes do:
                //   SHL dst, x
                //   SAR dst, x
                //
                // where x is 'reg_width - val_width'
                TB_DataType src_dt = n->inputs[1]->dt;
                int bits_in_type = src_dt.type == TB_PTR ? 64 : src_dt.data;

                TB_X86_DataType dt = legalize_int2(n->dt);
                if (dt <= TB_X86_TYPE_DWORD) {
                    dt = TB_X86_TYPE_DWORD;
                }
                int dst_bits = dt == TB_X86_TYPE_QWORD ? 64 : 32;

                Val dst = op_at(ctx, t->interval);
                Val src = op_at(ctx, t->ins[0].src);

                int op = 0;
                switch (bits_in_type) {
                    case 8:  op = MOVSXB; break;
                    case 16: op = MOVSXW; break;
                    case 32: op = MOVSXD; break;
                    default: {
                        if (!is_value_match(&dst, &src)) {
                            inst2(e, MOV, &dst, &src, dt);
                        }

                        Val imm = val_imm(dst_bits - bits_in_type);
                        inst2(e, SHL, &dst, &imm, dt);
                        inst2(e, SAR, &dst, &imm, dt);
                        break;
                    }
                }

                if (op) {
                    inst2(e, op, &dst, &src, dt);
                }
                break;
            }
            case TB_TRUNCATE:
            case TB_ZERO_EXT:
            case TB_INT2PTR: {
                TB_X86_DataType dt = legalize_int2(n->dt);

                Val dst = op_at(ctx, t->interval);
                Val lhs = op_at(ctx, t->ins[0].src);
                if (!is_value_match(&dst, &lhs)) {
                    inst2(e, MOV, &dst, &lhs, dt);
                }
                break;
            }
            case TB_SYMBOL: {
                TB_Symbol* sym = TB_NODE_GET_EXTRA_T(n, TB_NodeSymbol)->sym;

                Val dst = op_at(ctx, t->interval);
                Val src = val_global(sym, 0);
                inst2(e, LEA, &dst, &src, TB_X86_TYPE_QWORD);
                break;
            }
            case TB_AND:
            case TB_OR:
            case TB_XOR:
            case TB_ADD:
            case TB_SUB: {
                const static InstType ops[] = { AND, OR, XOR, ADD, SUB };
                InstType op = ops[n->type - TB_AND];
                TB_X86_DataType dt = legalize_int2(n->dt);

                Val dst = op_at(ctx, t->interval);
                Val lhs = op_at(ctx, t->ins[0].src);

                if (!is_value_match(&dst, &lhs)) {
                    // we'd rather do LEA addition than mov+add, but if it's add by itself it's fine
                    if (n->type == TB_ADD && (dt == TB_X86_TYPE_DWORD || dt == TB_X86_TYPE_QWORD)) {
                        if (t->flags & TILE_HAS_IMM) {
                            assert(n->inputs[2]->type == TB_INTEGER_CONST);
                            TB_NodeInt* i = TB_NODE_GET_EXTRA(n->inputs[2]);

                            // lea dst, [lhs + imm]
                            Val ea = val_base_disp(lhs.reg, i->value);
                            inst2(e, LEA, &dst, &ea, dt);
                            break;
                        }
                    }

                    inst2(e, MOV, &dst, &lhs, dt);
                }

                if (t->flags & TILE_HAS_IMM) {
                    assert(n->inputs[2]->type == TB_INTEGER_CONST);
                    TB_NodeInt* i = TB_NODE_GET_EXTRA(n->inputs[2]);

                    Val rhs = val_imm(i->value);
                    inst2(e, op, &dst, &rhs, dt);
                } else {
                    Val rhs = op_at(ctx, t->ins[1].src);
                    inst2(e, op, &dst, &rhs, dt);
                }
                break;
            }
            case TB_MUL: {
                TB_X86_DataType dt = legalize_int2(n->dt);

                Val dst = op_at(ctx, t->interval);
                Val lhs = op_at(ctx, t->ins[0].src);

                if (t->flags & TILE_HAS_IMM) {
                    assert(n->inputs[2]->type == TB_INTEGER_CONST);
                    TB_NodeInt* i = TB_NODE_GET_EXTRA(n->inputs[2]);

                    inst2(e, IMUL3, &dst, &lhs, dt);
                    if (dt == TB_X86_TYPE_WORD) {
                        EMIT2(e, i->value);
                    } else {
                        EMIT4(e, i->value);
                    }
                } else {
                    if (!is_value_match(&dst, &lhs)) {
                        inst2(e, MOV, &dst, &lhs, dt);
                    }

                    Val rhs = op_at(ctx, t->ins[1].src);
                    inst2(e, IMUL, &dst, &rhs, dt);
                }
                break;
            }
            case TB_SHL:
            case TB_SHR:
            case TB_SAR: {
                TB_X86_DataType dt = legalize_int2(n->dt);

                Val dst = op_at(ctx, t->interval);
                Val lhs = op_at(ctx, t->ins[0].src);
                if (!is_value_match(&dst, &lhs)) {
                    inst2(e, MOV, &dst, &lhs, dt);
                }

                InstType op;
                switch (n->type) {
                    case TB_SHL: op = SHL; break;
                    case TB_SHR: op = SHR; break;
                    case TB_SAR: op = SAR; break;
                    default: tb_todo();
                }

                if (t->flags & TILE_HAS_IMM) {
                    assert(n->inputs[2]->type == TB_INTEGER_CONST);
                    TB_NodeInt* i = TB_NODE_GET_EXTRA(n->inputs[2]);

                    Val rhs = val_imm(i->value);
                    inst2(e, op, &dst, &rhs, dt);
                } else {
                    Val rcx = val_gpr(RCX);
                    inst2(e, op, &dst, &rcx, TB_X86_TYPE_DWORD);
                }
                break;
            }
            case TB_UDIV:
            case TB_SDIV:
            case TB_UMOD:
            case TB_SMOD: {
                bool is_signed = (n->type == TB_SDIV || n->type == TB_SMOD);
                bool is_div    = (n->type == TB_UDIV || n->type == TB_SDIV);

                TB_DataType dt = n->dt;

                // if signed:
                //   cqo/cdq (sign extend RAX into RDX)
                // else:
                //   xor rdx, rdx
                if (is_signed) {
                    if (n->dt.data > 32) {
                        EMIT1(e, 0x48);
                    }
                    EMIT1(e, 0x99);
                } else {
                    Val rdx = val_gpr(RDX);
                    inst2(e, XOR, &rdx, &rdx, TB_X86_TYPE_DWORD);
                }

                Val rhs = op_at(ctx, t->ins[1].src);
                inst1(e, is_signed ? IDIV : DIV, &rhs, legalize_int2(dt));
                break;
            }
            case TB_CALL: {
                // we've already placed the register params in their slots, now we're missing
                // stack params which go into [rsp + 0x20 + (i-4)*8] where i is the param index.
                int stack_params = (n->inputs[2]->type == TB_SYMBOL ? 0 : 1) + param_descs[ctx->abi_index].gpr_count;
                FOREACH_N(i, stack_params, t->in_count) {
                    TB_X86_DataType dt = TB_X86_TYPE_QWORD; // legalize_int2(t->ins[i].);

                    Val dst = val_stack(0x20 + (i - 4) * 8);
                    Val src = op_at(ctx, t->ins[i].src);
                    inst2(e, MOV, &dst, &src, dt);
                }

                if (n->inputs[2]->type == TB_SYMBOL) {
                    TB_Symbol* sym = TB_NODE_GET_EXTRA_T(n->inputs[2], TB_NodeSymbol)->sym;

                    Val target = val_global(sym, 0);
                    inst1(e, CALL, &target, TB_X86_TYPE_QWORD);
                } else {
                    Val target = op_at(ctx, t->ins[0].src);
                    inst1(e, CALL, &target, TB_X86_TYPE_QWORD);
                }
                break;
            }
            case TB_BRANCH: {
                TB_NodeBranch* br = TB_NODE_GET_EXTRA(n);

                // the arena on the function should also be available at this time, we're
                // in the TB_Passes
                TB_Arena* arena = ctx->f->arena;
                TB_ArenaSavepoint sp = tb_arena_save(arena);
                int* succ = tb_arena_alloc(arena, br->succ_count * sizeof(int));

                // fill successors
                bool has_default = false;
                for (User* u = n->users; u; u = u->next) {
                    if (u->n->type == TB_PROJ) {
                        int index = TB_NODE_GET_EXTRA_T(u->n, TB_NodeProj)->index;
                        TB_Node* succ_n = cfg_next_bb_after_cproj(u->n);

                        if (index == 0) {
                            has_default = !cfg_is_unreachable(succ_n);
                        }

                        MachineBB* mbb = node_to_bb(ctx, succ_n);
                        succ[index] = mbb->id;
                    }
                }

                TB_DataType dt = n->inputs[1]->dt;
                if (br->succ_count == 1) {
                    assert(0 && "degenerate branch? that's odd");
                } else if (br->succ_count == 2) {
                    int naw = succ[1], yea = succ[0];

                    Cond cc;
                    if (t->flags & TILE_CMP_JCC) {
                        TB_Node* cmp = n->inputs[1];
                        TB_DataType cmp_dt = TB_NODE_GET_EXTRA_T(cmp, TB_NodeCompare)->cmp_dt;
                        if (TB_IS_FLOAT_TYPE(cmp_dt)) {
                            tb_todo();
                        } else {
                            Val a = op_at(ctx, t->ins[0].src);
                            if (t->flags & TILE_HAS_IMM) {
                                assert(cmp->inputs[2]->type == TB_INTEGER_CONST);
                                TB_NodeInt* i = TB_NODE_GET_EXTRA(cmp->inputs[2]);

                                Val b = val_imm(i->value);
                                inst2(e, CMP, &a, &b, legalize_int2(cmp_dt));
                            } else {
                                Val b = op_at(ctx, t->ins[1].src);
                                inst2(e, CMP, &a, &b, legalize_int2(cmp_dt));
                            }

                            switch (cmp->type) {
                                case TB_CMP_EQ:  cc = E;  break;
                                case TB_CMP_NE:  cc = NE; break;
                                case TB_CMP_SLT: cc = L;  break;
                                case TB_CMP_SLE: cc = LE; break;
                                case TB_CMP_ULT: cc = B;  break;
                                case TB_CMP_ULE: cc = BE; break;
                                default: tb_unreachable();
                            }
                        }
                    } else {
                        Val src = op_at(ctx, t->ins[0].src);
                        inst2(e, TEST, &src, &src, legalize_int2(dt));
                        cc = NE;
                    }

                    // if flipping avoids a jmp, do that
                    if (ctx->fallthrough == yea) {
                        // jcc false
                        EMIT1(e, 0x0F); EMIT1(e, 0x80 + (cc ^ 1)); EMIT4(e, 0);
                        tb_emit_rel32(e, &e->labels[naw], GET_CODE_POS(e) - 4);
                    } else {
                        // jcc true
                        EMIT1(e, 0x0F); EMIT1(e, 0x80 + cc); EMIT4(e, 0);
                        tb_emit_rel32(e, &e->labels[yea], GET_CODE_POS(e) - 4);
                        if (ctx->fallthrough != naw) {
                            // jmp false
                            EMIT1(e, 0xE9); EMIT4(e, 0);
                            tb_emit_rel32(e, &e->labels[naw], GET_CODE_POS(e) - 4);
                        }
                    }
                } else {
                    tb_todo();
                }

                tb_arena_restore(arena, sp);
                break;
            }

            default: tb_todo();
        }
    }
}

static void post_emit(Ctx* restrict ctx, TB_CGEmitter* e) {
    // pad to 16bytes
    static const uint8_t nops[8][8] = {
        { 0x90 },
        { 0x66, 0x90 },
        { 0x0F, 0x1F, 0x00 },
        { 0x0F, 0x1F, 0x40, 0x00 },
        { 0x0F, 0x1F, 0x44, 0x00, 0x00 },
        { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 },
        { 0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00 },
        { 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
    };

    size_t pad = 16 - (ctx->emit.count & 15);
    if (pad < 16) {
        ctx->nop_pads = pad;

        uint8_t* dst = tb_cgemit_reserve(&ctx->emit, pad);
        tb_cgemit_commit(&ctx->emit, pad);

        if (pad > 8) {
            size_t rem = pad - 8;
            memset(dst, 0x66, rem);
            pad -= rem, dst += rem;
        }
        memcpy(dst, nops[pad - 1], pad);
    }
}

static void emit_win64eh_unwind_info(TB_Emitter* e, TB_FunctionOutput* out_f, uint64_t stack_usage) {
    size_t patch_pos = e->count;
    UnwindInfo unwind = {
        .version = 1,
        .flags = 0, // UNWIND_FLAG_EHANDLER,
        .prolog_length = out_f->prologue_length,
        .code_count = 0,
    };
    tb_outs(e, sizeof(UnwindInfo), &unwind);

    size_t code_count = 0;
    if (stack_usage > 0) {
        UnwindCode codes[] = {
            // sub rsp, stack_usage
            { .code_offset = 4, .unwind_op = UNWIND_OP_ALLOC_SMALL, .op_info = (stack_usage / 8) - 1 },
        };
        tb_outs(e, sizeof(codes), codes);
        code_count += 1;
    }

    tb_patch1b(e, patch_pos + offsetof(UnwindInfo, code_count), code_count);
}

#define E(fmt, ...) tb_asm_print(e, fmt, ## __VA_ARGS__)
static void disassemble(TB_CGEmitter* e, Disasm* restrict d, int bb, size_t pos, size_t end) {
    if (bb >= 0) {
        E(".bb%d:\n", bb);
    }

    while (pos < end) {
        while (d->loc != d->end && d->loc->pos == pos) {
            E("  // %s : line %d\n", d->loc->file->path, d->loc->line);
            d->loc++;
        }

        TB_X86_Inst inst;
        if (!tb_x86_disasm(&inst, end - pos, &e->data[pos])) {
            E("  ERROR\n");
            pos += 1; // skip ahead once... cry
            continue;
        }

        const char* mnemonic = tb_x86_mnemonic(&inst);
        E("  ");
        if (inst.flags & TB_X86_INSTR_REP) {
            E("rep ");
        }
        if (inst.flags & TB_X86_INSTR_LOCK) {
            E("lock ");
        }
        E("%s", mnemonic);
        if (inst.data_type >= TB_X86_TYPE_SSE_SS && inst.data_type <= TB_X86_TYPE_SSE_PD) {
            static const char* strs[] = { "ss", "sd", "ps", "pd" };
            E(strs[inst.data_type - TB_X86_TYPE_SSE_SS]);
        }
        E(" ");

        bool mem = true, imm = true;
        for (int i = 0; i < 4; i++) {
            if (inst.regs[i] == -1) {
                if (mem && (inst.flags & TB_X86_INSTR_USE_MEMOP)) {
                    if (i > 0) E(", ");

                    mem = false;

                    if (inst.flags & TB_X86_INSTR_USE_RIPMEM) {
                        bool is_label = inst.opcode == 0xE8 || inst.opcode == 0xE9
                            || (inst.opcode >= 0x70   && inst.opcode <= 0x7F)
                            || (inst.opcode >= 0x0F80 && inst.opcode <= 0x0F8F);

                        if (!is_label) E("[");

                        if (d->patch && d->patch->pos == pos + inst.length - 4) {
                            const TB_Symbol* target = d->patch->target;

                            if (target->name[0] == 0) {
                                E("sym%p", target);
                            } else {
                                E("%s", target->name);
                            }
                            d->patch = d->patch->next;
                        } else {
                            uint32_t target = pos + inst.length + inst.disp;
                            int bb = tb_emit_get_label(e, target);
                            uint32_t landed = e->labels[bb] & 0x7FFFFFFF;

                            if (landed != target) {
                                E(".bb%d + %d", bb, (int)target - (int)landed);
                            } else {
                                E(".bb%d", bb);
                            }
                        }

                        if (!is_label) E("]");
                    } else {
                        E("%s [", tb_x86_type_name(inst.data_type));
                        if (inst.base != 255) {
                            E("%s", tb_x86_reg_name(inst.base, TB_X86_TYPE_QWORD));
                        }

                        if (inst.index != 255) {
                            E(" + %s*%d", tb_x86_reg_name(inst.index, TB_X86_TYPE_QWORD), 1 << inst.scale);
                        }

                        if (inst.disp > 0) {
                            E(" + %d", inst.disp);
                        } else if (inst.disp < 0) {
                            E(" - %d", -inst.disp);
                        }

                        E("]");
                    }
                } else if (imm && (inst.flags & (TB_X86_INSTR_IMMEDIATE | TB_X86_INSTR_ABSOLUTE))) {
                    if (i > 0) E(", ");

                    imm = false;
                    if (inst.flags & TB_X86_INSTR_ABSOLUTE) {
                        E("%#llx", inst.abs);
                    } else {
                        E("%#x", inst.imm);
                    }
                } else {
                    break;
                }
            } else {
                if (i > 0) {
                    E(", ");

                    // special case for certain ops with two data types
                    if (inst.flags & TB_X86_INSTR_TWO_DATA_TYPES) {
                        E("%s", tb_x86_reg_name(inst.regs[i], inst.data_type2));
                        continue;
                    }
                }

                E("%s", tb_x86_reg_name(inst.regs[i], inst.data_type));
            }
        }
        E("\n");

        pos += inst.length;
    }
}
#undef E

static size_t emit_call_patches(TB_Module* restrict m, TB_FunctionOutput* out_f) {
    size_t r = 0;
    uint32_t src_section = out_f->section;

    for (TB_SymbolPatch* patch = out_f->first_patch; patch; patch = patch->next) {
        if (patch->target->tag == TB_SYMBOL_FUNCTION) {
            uint32_t dst_section = ((TB_Function*) patch->target)->output->section;

            // you can't do relocations across sections
            if (src_section == dst_section) {
                assert(patch->pos < out_f->code_size);

                // x64 thinks of relative addresses as being relative
                // to the end of the instruction or in this case just
                // 4 bytes ahead hence the +4.
                size_t actual_pos = out_f->code_pos + patch->pos + 4;

                uint32_t p = ((TB_Function*) patch->target)->output->code_pos - actual_pos;
                memcpy(&out_f->code[patch->pos], &p, sizeof(uint32_t));

                r += 1;
                patch->internal = true;
            }
        }
    }

    return out_f->patch_count - r;
}

ICodeGen tb__x64_codegen = {
    .minimum_addressable_size = 8,
    .pointer_size = 64,
    .emit_win64eh_unwind_info = emit_win64eh_unwind_info,
    .emit_call_patches  = emit_call_patches,
    .get_data_type_size = get_data_type_size,
    .compile_function   = compile_function,
};
