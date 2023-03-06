#include "../x64/x64.h"
#include "../x64/x64_emitter.h"

enum {
    CG_REGISTER_CLASSES = 2
};

enum {
    REG_CLASS_GPR,
    REG_CLASS_XMM
};

typedef enum X86_DataType {
    X86_TYPE_NONE = 0,

    X86_TYPE_BYTE,     // 1
    X86_TYPE_WORD,     // 2
    X86_TYPE_DWORD,    // 4
    X86_TYPE_QWORD,    // 8

    X86_TYPE_PBYTE,   // int8 x 16 = 16
    X86_TYPE_PWORD,   // int16 x 8 = 16
    X86_TYPE_PDWORD,  // int32 x 4 = 16
    X86_TYPE_PQWORD,  // int64 x 2 = 16

    X86_TYPE_SSE_SS,  // float32 x 1 = 4
    X86_TYPE_SSE_SD,  // float64 x 1 = 8
    X86_TYPE_SSE_PS,  // float32 x 4 = 16
    X86_TYPE_SSE_PD,  // float64 x 2 = 16

    X86_TYPE_XMMWORD, // the generic idea of them
} X86_DataType;

typedef enum X86_InstType {
    // mov    lea    add
    X86_FIRST_INST2    = 0,
    // movps    ucomiss
    X86_FIRST_INST2SSE = 256,
    // call [rcx]     div
    X86_FIRST_UNARY    = 0x1000,
} X86_InstType;

typedef enum X86_InstrFlags {
    // uses xmm registers for the reg array
    X86_INSTR_XMMREG = (1u << 0u),

    // r/m is a memory operand
    X86_INSTR_USE_MEMOP = (1u << 1u),

    // r/m is a rip-relative address (X86_INSTR_USE_MEMOP is always set when this is set)
    X86_INSTR_USE_RIPMEM = (1u << 2u),

    // LOCK prefix is present
    X86_INSTR_LOCK = (1u << 3u),

    // uses a signed immediate
    X86_INSTR_IMMEDIATE = (1u << 4u),

    // absolute means it's using the 64bit immediate (cannot be applied while a memory operand is active)
    X86_INSTR_ABSOLUTE = (1u << 5u),

    // set if the r/m can be found on the right hand side
    X86_INSTR_DIRECTION = (1u << 6u),

    // uses the second data type because the instruction is weird like MOVSX or MOVZX
    X86_INSTR_TWO_DATA_TYPES = (1u << 7u)
} X86_InstrFlags;

typedef struct Inst {
    X86_InstType type;

    X86_DataType data_type  : 8;
    X86_DataType data_type2 : 8;
    // X86_Segment segment  : 8;
    X86_InstrFlags flags    : 8;
    uint8_t length;

    // normal operands
    int8_t regs[4];

    // immediate operand
    //   imm for INSTR_IMMEDIATE
    //   abs for INSTR_ABSOLUTE
    union {
        int32_t  imm;
        uint64_t abs;
    };

    // memory operand
    struct {
        int8_t base;
        int8_t index;
        Scale scale;
        int32_t disp;
    } mem;
} Inst;

#include "generic_cg.h"

// references an allocated
#define REF(x) (-((x) + 2))

#define SUBMIT_INST_RR(op, dt, a, b) (seq->insts[seq->inst_count++] = (Inst){ X86_FIRST_INST2 + op, .data_type = dt, .regs = { (a), (b), GPR_NONE, GPR_NONE } })
#define SUBMIT_INST_RI(op, dt, a, b) (seq->insts[seq->inst_count++] = (Inst){ X86_FIRST_INST2 + op, .flags = X86_INSTR_IMMEDIATE, .data_type = dt, .regs = { (a), GPR_NONE, GPR_NONE, GPR_NONE }, .imm = (b) })

// OP a
#define SUBMIT_INST_R(op, dt, a) (seq->insts[seq->inst_count++] = (Inst){ (X86_InstType) (op), .data_type = dt, .regs = { (a), GPR_NONE, GPR_NONE, GPR_NONE } })

#define SUBMIT(i) (seq->insts[seq->inst_count++] = (i))
static Inst inst_rr(int op, X86_DataType dt, int lhs, int rhs) {
    return (Inst){
        X86_FIRST_INST2 + op,
        .flags = X86_INSTR_USE_MEMOP,
        .data_type = dt,
        .regs = { lhs, rhs, GPR_NONE, GPR_NONE }
    };
}

static Inst inst_mr(int op, X86_DataType dt, Val mem, int rhs) {
    return (Inst){
        X86_FIRST_INST2 + op,
        .flags = X86_INSTR_USE_MEMOP,
        .data_type = dt,
        .regs = { GPR_NONE, rhs, GPR_NONE, GPR_NONE },
        .mem = { mem.mem.base, mem.mem.index, mem.mem.scale, mem.mem.disp }
    };
}

static Inst inst_rm(int op, X86_DataType dt, int lhs, Val mem) {
    return (Inst){
        X86_FIRST_INST2 + op,
        .flags = X86_INSTR_USE_MEMOP,
        .data_type = dt,
        .regs = { lhs, GPR_NONE, GPR_NONE, GPR_NONE },
        .mem = { mem.mem.base, mem.mem.index, mem.mem.scale, mem.mem.disp }
    };
}

static Inst inst_rb(int op, X86_DataType dt, int lhs, int base, int disp) {
    return (Inst){
        X86_FIRST_INST2 + op,
        .flags = X86_INSTR_USE_MEMOP,
        .data_type = dt,
        .regs = { lhs, GPR_NONE, GPR_NONE, GPR_NONE },
        .mem = { base, GPR_NONE, SCALE_X1, disp }
    };
}

static int classify_reg_class(TB_DataType dt) {
    return dt.type == TB_FLOAT ? REG_CLASS_XMM : REG_CLASS_GPR;
}

static int8_t resolve_ref(Sequence* restrict seq, int8_t x) {
    if (x < -1) return seq->defs[-x - 2].reg;
    return x;
}

// *out_mask of 0 means no mask
static X86_DataType legalize_int(TB_DataType dt, uint64_t* out_mask) {
    assert(dt.type == TB_INT || dt.type == TB_PTR);
    if (dt.type == TB_PTR) return *out_mask = 0, X86_TYPE_QWORD;

    X86_DataType t = X86_TYPE_NONE;
    int bits = 0;

    if (dt.data <= 8) bits = 8, t = X86_TYPE_BYTE;
    else if (dt.data <= 16) bits = 16, t = X86_TYPE_WORD;
    else if (dt.data <= 32) bits = 32, t = X86_TYPE_DWORD;
    else if (dt.data <= 64) bits = 64, t = X86_TYPE_QWORD;

    assert(bits != 0 && "TODO: large int support");
    uint64_t mask = ~UINT64_C(0) >> (64 - dt.data);

    *out_mask = (dt.data == bits) ? 0 : mask;
    return t;
}

static void print_operand(Val* v) {
    switch (v->type) {
        case VAL_GPR: printf("%s", GPR_NAMES[v->reg]); break;
        case VAL_IMM: printf("%d", v->imm); break;
        case VAL_MEM: {
            printf("[%s + %d]", GPR_NAMES[v->mem.base], v->mem.disp);
            break;
        }
        default: tb_todo();
    }
}

static Val spill_to_stack_slot(Ctx* restrict ctx, Sequence* restrict seq, TB_Node* n, Val* src) {
    if (src && src->type == VAL_MEM && src->mem.base == RBP) {
        return *src;
    }

    Val dst;
    if (n->type == TB_PARAM) {
        int id = TB_NODE_GET_EXTRA_T(n, TB_NodeParam)->id;

        // TODO(NeGate): this is win64 specific... maybe?
        dst = val_stack(TB_TYPE_PTR, 16 + (id * 8));
    } else {
        // allocate new stack slot
        int pos = STACK_ALLOC(8, 8);
        dst = val_stack(TB_TYPE_PTR, pos);
        dst.mem.is_rvalue = true;
    }

    if (src != NULL) {
        INST2(!is_lvalue(src) ? MOV : LEA, &dst, src, n->dt);
        if (ctx->emit_asm) {
            printf("  MOV ");
            print_operand(&dst);
            printf(", ");
            print_operand(src);

            printf(" \x1b[32m# spill r%d\x1b[0m\n", NAME(n));
        }
    }

    return dst;
}

static Val isel(Ctx* restrict ctx, Sequence* restrict seq, TB_Node* n) {
    TB_NodeTypeEnum type = n->type;
    switch (type) {
        case TB_INTEGER_CONST: {
            TB_NodeInt* i = TB_NODE_GET_EXTRA(n);
            assert(i->num_words == 1);

            uint64_t x = i->words[0];
            assert(fits_into_int32(x));
            return val_imm(n->dt, x);
        }

        case TB_SIGN_EXT: {
            uint64_t mask;
            X86_DataType t = legalize_int(n->dt, &mask);

            if (mask) {
                // int shift_amt = dst_bits_in_type - bits_in_type;
                //
                // shl dst, shift_amt
                // sar dst, shift_amt
                tb_todo();
            }

            // (sxt (load ptr)) => movsx dst, [ptr]
            if (try_tile(ctx, n->inputs[0], TB_LOAD)) {
                int addr = DEF(n, .based = n->inputs[0]->inputs[0], .reg_class = REG_CLASS_GPR);
                int dst = DEF(n, .reg_class = REG_CLASS_GPR);

                SUBMIT(inst_rb(MOVSXD, t, REF(dst), REF(addr), 0));
                return val_gpr(n->dt, REF(dst));
            }

            tb_todo();
            return val_gpr(n->dt, RAX);
        }

        case TB_NOT:
        case TB_NEG: {
            uint64_t mask;
            X86_DataType t = legalize_int(n->dt, &mask);

            int dst = DEF(n, .based = n->inputs[0], .reg_class = REG_CLASS_GPR);
            SUBMIT_INST_R(type == TB_NOT ? NOT : NEG, t, REF(dst));

            if (mask) SUBMIT_INST_RI(AND, t, REF(dst), mask);
            return val_gpr(n->dt, REF(dst));
        }

        case TB_AND:
        case TB_OR:
        case TB_XOR:
        case TB_ADD:
        case TB_SUB: {
            const static Inst2Type ops[]  = { AND, OR, XOR, ADD, SUB };
            Inst2Type op = ops[type - TB_AND];

            uint64_t mask;
            X86_DataType t = legalize_int(n->dt, &mask);

            int dst = DEF(n, .based = n->inputs[0], .reg_class = REG_CLASS_GPR);
            Val* b = &GET_VAL(n->inputs[1]);
            if (b->type == VAL_IMM) {
                // $ add dst, b->imm
                SUBMIT_INST_RI(op, t, REF(dst), b->imm);
            } else {
                int other = DEF(n->inputs[1], .reg_class = REG_CLASS_GPR);
                SUBMIT_INST_RR(op, t, REF(dst), REF(other));
            }

            if (mask) SUBMIT_INST_RI(AND, t, REF(dst), mask);
            return val_gpr(n->dt, REF(dst));
        }
        case TB_CMP_EQ:
        case TB_CMP_NE:
        case TB_CMP_SLT:
        case TB_CMP_SLE:
        case TB_CMP_ULT:
        case TB_CMP_ULE:
        case TB_CMP_FLT:
        case TB_CMP_FLE: {
            TB_DataType cmp_dt = TB_NODE_GET_EXTRA_T(n, TB_NodeCompare)->cmp_dt;
            assert(cmp_dt.width == 0 && "TODO: Implement vector compares");

            assert(!TB_IS_FLOAT_TYPE(cmp_dt) && "TODO");

            uint64_t mask;
            X86_DataType t = legalize_int(cmp_dt, &mask);
            assert(mask == 0);

            int dst = DEF(n->inputs[0], .reg_class = REG_CLASS_GPR);
            Val* b = &GET_VAL(n->inputs[1]);

            Cond cc = -1;
            bool invert = false;
            if (b->type == VAL_IMM) {
                SUBMIT_INST_RI(CMP, t, REF(dst), b->imm);
            } else {
                int other = DEF(n->inputs[1], .reg_class = REG_CLASS_GPR);
                SUBMIT_INST_RR(CMP, t, REF(dst), REF(other));
            }

            switch (type) {
                case TB_CMP_EQ: cc = E; break;
                case TB_CMP_NE: cc = NE; break;
                case TB_CMP_SLT: cc = invert ? G : L; break;
                case TB_CMP_SLE: cc = invert ? GE : LE; break;
                case TB_CMP_ULT: cc = invert ? A : B; break;
                case TB_CMP_ULE: cc = invert ? NB : BE; break;
                default: tb_unreachable();
            }
            return val_flags(cc);
        }

        case TB_LOAD: {
            int dst = DEF(n, .based = n->inputs[0], .reg_class = REG_CLASS_GPR, .load = true);
            return val_gpr(n->dt, REF(dst));
        }
        case TB_STORE: {
            uint64_t mask;
            X86_DataType t = legalize_int(n->dt, &mask);

            Liveness* restrict ni = get_liveness(ctx, n->inputs[1]);
            if (n->inputs[0]->type == TB_LOCAL && n->inputs[1]->type == TB_PARAM && ni->user_count == 1) {
                // we want to use the stack slot for this local.
                // we don't even need to fill the TB_PARAM since
                // it's only used here.
                int id = TB_NODE_GET_EXTRA_T(n->inputs[1], TB_NodeParam)->id;
                GET_VAL(n->inputs[0]) = val_stack(TB_TYPE_PTR, 16 + (id * 8));
            }

            Val* peep = &GET_VAL(n->inputs[0]);
            Inst inst;
            if (peep->type == VAL_MEM && !peep->mem.is_rvalue && peep->mem.index == GPR_NONE) {
                inst = inst_mr(MOV, t, *peep, GPR_NONE);
            } else {
                int addr = DEF(n->inputs[0], .reg_class = REG_CLASS_GPR);
                inst = inst_mr(MOV, t, val_base_disp(TB_TYPE_PTR, REF(addr), 0), GPR_NONE);
            }

            Val* peep2 = &GET_VAL(n->inputs[1]);
            if (peep2->type == VAL_IMM) {
                inst.flags |= X86_INSTR_IMMEDIATE;
                inst.imm = peep2->imm;
            } else {
                int src = DEF(n->inputs[1], .reg_class = REG_CLASS_GPR);
                inst.regs[1] = src;
            }

            SUBMIT(inst);
            return (Val){ 0 };
        }

        case TB_PARAM: {
            int id = TB_NODE_GET_EXTRA_T(n, TB_NodeParam)->id;
            if (id >= 4) {
                return val_stack(TB_TYPE_PTR, 16 + (id * 8));
            } else {
                return val_gpr(n->dt, WIN64_GPR_PARAMETERS[id]);
            }
        }

        case TB_LOCAL: {
            // allocate stack slot
            TB_NodeLocal* local = TB_NODE_GET_EXTRA(n);

            int pos = STACK_ALLOC(local->size, local->align);
            return val_stack(TB_TYPE_PTR, pos);
        }

        case TB_NULL:
        case TB_PHI:
        case TB_BRANCH:
        case TB_RET:
        return (Val){ 0 };

        default: tb_todo();
    }
}

static void copy_value(Ctx* restrict ctx, Val* dst, Val* src, TB_DataType dt, bool load, TB_Node* n, const char* reason) {
    if (load) {
        Val s = *src;
        if (s.type == VAL_GPR) {
            s = val_base_disp(TB_TYPE_PTR, s.gpr, 0);
        }

        INST2(MOV, dst, &s, dt);
        if (ctx->emit_asm) {
            printf("  MOV ");
            print_operand(dst);
            printf(", ");
            print_operand(&s);
            printf(" \x1b[32m# copy\x1b[0m\n");
        }
    } else {
        INST2(!is_lvalue(src) ? MOV : LEA, dst, src, dt);
        if (ctx->emit_asm) {
            printf("  %s ", !is_lvalue(src) ? "MOV" : "LEA");
            print_operand(dst);
            printf(", ");
            print_operand(src);
            printf(" \x1b[32m# %s r%d\x1b[0m\n", reason, NAME(n));
        }
    }
}

static void emit_sequence(Ctx* restrict ctx, Sequence* restrict seq, TB_Node* n) {
    FOREACH_N(i, 0, seq->inst_count) {
        Inst* restrict inst = &seq->insts[i];

        // prefixes
        if (inst->flags & X86_INSTR_LOCK) EMIT1(&ctx->emit, 0xF0);

        bool has_mem_op = inst->flags & X86_INSTR_USE_MEMOP;
        bool has_immediate = inst->flags & (X86_INSTR_IMMEDIATE | X86_INSTR_ABSOLUTE);

        int op_count = 4;
        Val operands[4];
        FOREACH_N(j, 0, 4) {
            /*X86_DataType dt = inst->data_type;
            if ((inst->flags & X86_INSTR_TWO_DATA_TYPES) != 0 && j == 1) {
                dt = inst->data_type2;
            }*/

            if (inst->regs[j] == GPR_NONE) {
                // GPR_NONE is either exit or a placeholder if we've got crap
                if (has_mem_op) {
                    has_mem_op = false;

                    // resolve  any DEF references
                    int8_t base = resolve_ref(seq, inst->mem.base);
                    int8_t index = resolve_ref(seq, inst->mem.index);

                    operands[j] = (Val){ .type = VAL_MEM, .mem = { .base = base, index, inst->mem.scale, inst->mem.disp } };
                } else if (has_immediate) {
                    has_immediate = false;

                    assert((inst->flags & X86_INSTR_ABSOLUTE) == 0);
                    operands[j] = (Val){ .type = VAL_IMM, .imm = inst->imm };
                } else {
                    op_count = j;
                    break;
                }
            } else {
                int8_t reg = resolve_ref(seq, inst->regs[j]);
                operands[j] = (Val){ .type = VAL_GPR, .gpr = reg };
            }
        }

        // decode data type
        TB_DataType dt = TB_TYPE_VOID;
        switch (inst->data_type) {
            case X86_TYPE_BYTE:  dt = TB_TYPE_I8;  break;
            case X86_TYPE_WORD:  dt = TB_TYPE_I16; break;
            case X86_TYPE_DWORD: dt = TB_TYPE_I32; break;
            case X86_TYPE_QWORD: dt = TB_TYPE_I64; break;
            default: tb_todo();
        }

        // decode inst type -> op
        if (inst->type >= X86_FIRST_UNARY) {
            assert(op_count == 1);

            INST1((Inst1) inst->type, &operands[0]);
        } else if (inst->type >= X86_FIRST_INST2SSE) {
            uint8_t flags = 0;
            tb_todo(); // fill flags

            assert(op_count == 2);
            INST2SSE(inst->type - X86_FIRST_INST2SSE, &operands[0], &operands[1], flags);
        } else if (inst->type >= X86_FIRST_INST2) {
            assert(op_count == 2);
            INST2((Inst2Type) inst->type, &operands[0], &operands[1], dt);
        } else {
            tb_todo();
        }

        if (ctx->emit_asm) {
            #define A(x) case X86_FIRST_INST2 + x: printf("  " #x " "); break
            switch ((int) inst->type) {
                A(NEG);
                A(NOT);

                A(ADD);
                A(AND);
                A(OR);
                A(SUB);
                A(XOR);
                A(CMP);
                A(MOV);

                A(MOVSXB);
                A(MOVSXW);
                A(MOVSXD);
                A(MOVZXB);
                A(MOVZXW);
                default: tb_todo();
            }
            #undef A

            FOREACH_N(j, 0, op_count) {
                if (j) printf(", ");
                print_operand(&operands[j]);
            }
            printf("\n");
        }
    }

    if (n == NULL) {
        return;
    }

    // Handle terminators
    if (n->type == TB_BRANCH) {
        TB_NodeBranch* br = TB_NODE_GET_EXTRA(n);
        if (br->count == 0) {
            if (ctx->fallthrough != br->default_label) {
                JMP(br->default_label);
                if (ctx->emit_asm) {
                    printf("  JMP L%d\n", br->default_label);
                }
            }
        } else if (br->count == 1) {
            // if-like branch
            Val* v = &GET_VAL(n->inputs[0]);
            assert(v->type == VAL_FLAGS);

            JCC(v->cond, br->targets[0].value);
            if (ctx->emit_asm) printf("  J%s L%d\n", COND_NAMES[v->cond], br->targets[0].value);

            if (ctx->fallthrough != br->default_label) {
                JMP(br->default_label);
                if (ctx->emit_asm) printf("  JMP L%d\n", br->default_label);
            }
        } else {
            Val* v = &GET_VAL(n->inputs[0]);
            assert(v->type == VAL_GPR);

            // TODO(NeGate): generate jump tables...
            TB_DataType dt = n->inputs[0]->dt;
            FOREACH_N(i, 0, br->count) {
                Val operand = val_imm(dt, br->targets[i].key);

                // TODO(NeGate): print instructions
                INST2(CMP, v, &operand, dt);
                JCC(E, br->targets[i].value);
            }

            if (ctx->fallthrough != br->default_label) {
                JMP(br->default_label);
                if (ctx->emit_asm) printf("  JMP L%d\n", br->default_label);
            }
        }
    } else if (n->type == TB_RET) {
        Val* v = &GET_VAL(n->inputs[0]);
        Val rax = val_gpr(TB_TYPE_I64, RAX);

        if (!is_value_gpr(v, RAX)) {
            INST2(!is_lvalue(v) ? MOV : LEA, &rax, v, n->inputs[0]->dt);
            if (ctx->emit_asm) {
                printf("  %s RAX, ", !is_lvalue(v) ? "MOV" : "LEA");
                print_operand(v);
                printf("\n");
            }
        } else {
            printf("  \x1b[32m#   return already in RAX\x1b[0m\n");
        }

        if (ctx->fallthrough != -1) {
            ret_jmp(&ctx->emit);
            if (ctx->emit_asm) printf("  JMP .ret\n");
        }
    }
}

static void patch_local_labels(Ctx* restrict ctx) {
    FOREACH_N(i, 0, ctx->emit.ret_patch_count) {
        uint32_t pos = ctx->emit.ret_patches[i];
        PATCH4(&ctx->emit, pos, GET_CODE_POS(&ctx->emit) - (pos + 4));
    }

    FOREACH_N(i, 0, ctx->emit.label_patch_count) {
        uint32_t pos = ctx->emit.label_patches[i].pos;
        uint32_t target_lbl = ctx->emit.label_patches[i].target_lbl;

        PATCH4(&ctx->emit, pos, ctx->emit.labels[target_lbl] - (pos + 4));
    }
}

static size_t emit_prologue(uint8_t* out, uint64_t saved, uint64_t stack_usage) {
    return 0;
}

static size_t emit_epilogue(uint8_t* out, uint64_t saved, uint64_t stack_usage) {
    out[0] = 0xC3;
    return 1;
}

static size_t emit_call_patches(TB_Module* restrict m) {
    size_t r = 0;
    FOREACH_N(i, 0, m->max_threads) {
        TB_SymbolPatch* patches = m->thread_info[i].symbol_patches;

        dyn_array_for(j, patches) {
            TB_SymbolPatch* patch = &patches[j];

            if (patch->target->tag == TB_SYMBOL_FUNCTION) {
                TB_FunctionOutput* out_f = patch->source->output;
                assert(out_f && "Patch cannot be applied to function with no compiled output");

                // x64 thinks of relative addresses as being relative
                // to the end of the instruction or in this case just
                // 4 bytes ahead hence the +4.
                size_t actual_pos = out_f->code_pos + out_f->prologue_length + patch->pos + 4;

                uint32_t p = ((TB_Function*) patch->target)->output->code_pos - actual_pos;
                memcpy(&out_f->code[out_f->prologue_length + patch->pos], &p, sizeof(uint32_t));
                r += 1;
            }
        }
    }

    return r;
}

#if _MSC_VER
_Pragma("warning (push)") _Pragma("warning (disable: 4028)")
#endif
ICodeGen tb__x64_codegen = {
    .minimum_addressable_size = 8,
    .pointer_size = 64,

    .emit_call_patches  = emit_call_patches,
    .get_data_type_size = get_data_type_size,
    .emit_prologue      = emit_prologue,
    .emit_epilogue      = emit_epilogue,

    .fast_path = compile_function,
    //.complex_path = x64_complex_compile_function
};
#if _MSC_VER
_Pragma("warning (pop)")
#endif
