#include "ir_gen.h"
#include <futex.h>
#include "../targets/targets.h"

// Maps param_num -> TB_Node*
static thread_local TB_Node** parameter_map;

static thread_local Cuik_Type* function_type;
static thread_local const char* function_name;

static thread_local TB_PassingRule func_return_rule;

// For aggregate returns
static _Thread_local TB_Attrib* scope_attrib;

static void emit_location(TranslationUnit* tu, TB_Function* func, SourceLoc loc);

static uint64_t get_ir_ordinal(TranslationUnit* tu, Stmt* stmt) {
    return ((uint64_t) tu->local_ordinal << 32ull) | stmt->decl.local_ordinal;
}

static TB_Symbol* get_external(CompilationUnit* restrict cu, const char* name) {
    // if this is the first time we've seen this name, add it to the table
    cuik_lock_compilation_unit(cu);

    TB_Symbol* result = NULL;
    ptrdiff_t search = nl_map_get_cstr(cu->export_table, name);
    if (search >= 0) {
        // Figure out what the symbol is and link it together
        result = cu->export_table[search].v;
    } else {
        // Always creates a real external... for now
        result = (TB_Symbol*) tb_extern_create(cu->ir_mod, -1, name, TB_EXTERNAL_SO_LOCAL);
    }

    cuik_unlock_compilation_unit(cu);
    return result;
}

static TB_Global* place_external(CompilationUnit* restrict cu, Stmt* s, TB_DebugType* dbg_type, TB_Linkage linkage) {
    if (s->flags & STMT_FLAGS_IS_EXPORTED) {
        cuik_lock_compilation_unit(cu);
        const char* name = s->decl.name;

        TB_Global* result = NULL;
        ptrdiff_t search = nl_map_get_cstr(cu->export_table, name);
        if (search >= 0) {
            // transmute
            TB_Symbol* s = cu->export_table[search].v;
            if (s->tag == TB_SYMBOL_GLOBAL) {
                result = tb_extern_transmute((TB_External*) s, dbg_type, linkage);
            } else {
                assert(s->tag == TB_SYMBOL_EXTERNAL);
                result = (TB_Global*) s;
            }
        } else {
            result = tb_global_create(cu->ir_mod, -1, name, dbg_type, linkage);
            ((TB_Symbol*) result)->ordinal = s->decl.local_ordinal;
            nl_map_put_cstr(cu->export_table, name, (TB_Symbol*) result);
        }

        cuik_unlock_compilation_unit(cu);
        return result;
    } else {
        return tb_global_create(cu->ir_mod, -1, s->decl.name, dbg_type, linkage);
    }
}

static void fallthrough_label(TB_Function* func, TB_Node* target) {
    TB_Node* curr = tb_inst_get_control(func);
    if (curr != NULL) {
        tb_inst_goto(func, target);
    }

    tb_inst_set_control(func, target);
}

// in theory, multiple threads can race here but the meme is that they
// all produce the same values so we can just let them race lmao.
TB_DebugType* cuik__as_tb_debug_type(TB_Module* mod, Cuik_Type* t) {
    TB_DebugType* old_val = atomic_load(&t->debug_type);
    if (old_val != NULL) {
        return old_val;
    }

    TB_DebugType* result = NULL;
    switch (t->kind) {
        case KIND_VOID:
        result = tb_debug_get_void(mod);
        break;

        case KIND_BOOL:
        result = tb_debug_get_bool(mod);
        break;

        case KIND_CHAR: case KIND_SHORT: case KIND_INT: case KIND_LONG: case KIND_LLONG:
        result = tb_debug_get_integer(mod, !t->is_unsigned, t->size * 8);
        break;

        case KIND_FLOAT:
        result = tb_debug_get_float(mod, TB_FLT_32);
        break;

        case KIND_DOUBLE:
        result = tb_debug_get_float(mod, TB_FLT_64);
        break;

        case KIND_ENUM:
        result = tb_debug_get_integer(mod, true, 32);
        break;

        case KIND_PTR:
        result = tb_debug_create_ptr(mod, cuik__as_tb_debug_type(mod, cuik_canonical_type(t->ptr_to)));
        break;

        case KIND_ARRAY:
        result = tb_debug_create_array(mod, cuik__as_tb_debug_type(mod, cuik_canonical_type(t->array.of)), t->array.count);
        break;

        case KIND_STRUCT:
        case KIND_UNION: {
            Member* kids = t->record.kids;
            size_t count = t->record.kid_count;
            const char* tag = t->also_known_as ? t->also_known_as : t->record.name;

            result = t->kind == KIND_STRUCT ? tb_debug_create_struct(mod, -1, tag) : tb_debug_create_union(mod, -1, tag);
            atomic_exchange(&t->debug_type, result);

            TB_DebugType** list = tb_debug_record_begin(result, count);
            for (size_t i = 0; i < count; i++) {
                Member* member = &kids[i];

                TB_DebugType* base = cuik__as_tb_debug_type(mod, cuik_canonical_type(member->type));
                TB_DebugType* field = NULL;
                if (member->name == NULL) {
                    // if we have unnamed members we just do _N where N is just ticked by the counter
                    char buf[8];
                    snprintf(buf, 8, "_%zu", i);

                    field = tb_debug_create_field(mod, base, -1, buf, member->offset);
                } else {
                    field = tb_debug_create_field(mod, base, -1, member->name, member->offset);
                }

                list[i] = field;
            }

            tb_debug_record_end(result, t->size, t->align);
            return result;
        }

        case KIND_FUNC: {
            bool has_return = cuik_canonical_type(t->func.return_type)->kind != KIND_VOID;
            result = tb_debug_create_func(mod, TB_STDCALL, t->func.param_count, has_return, t->func.has_varargs);

            if (has_return) {
                *tb_debug_func_returns(result) = cuik__as_tb_debug_type(mod, cuik_canonical_type(t->func.return_type));
            }

            TB_DebugType** params = tb_debug_func_params(result);
            Param* param_list = t->func.param_list;
            for (size_t i = 0; i < t->func.param_count; i++) {
                TB_DebugType* type = cuik__as_tb_debug_type(mod, cuik_canonical_type(param_list[i].type));

                if (param_list[i].name == NULL) {
                    // if we have unnamed members we just do _N where N is just ticked by the counter
                    char buf[8];
                    snprintf(buf, 8, "arg%zu", i);

                    params[i] = tb_debug_create_field(mod, type, -1, buf, 0);
                } else {
                    params[i] = tb_debug_create_field(mod, type, -1, param_list[i].name, 0);
                }
            }
            break;
        }

        default:
        abort(); // TODO
    }

    assert(result);
    atomic_exchange(&t->debug_type, result);
    return result;
}

static TB_Node* cast_reg(TB_Function* func, TB_Node* reg, const Cuik_Type* src, const Cuik_Type* dst) {
    if (dst->kind == KIND_VOID) {
        return reg;
    }

    // Cast into correct type
    if (src->kind == KIND_ARRAY && dst->kind == KIND_BOOL) {
        reg = tb_inst_bool(func, true);
    } else if (src->kind != KIND_BOOL && dst->kind == KIND_BOOL) {
        TB_DataType dt = reg->dt;
        TB_Node* comparand;
        if (dt.type == TB_FLOAT && dt.data == TB_FLT_32) {
            comparand = tb_inst_float32(func, 0.0f);
        } else if (dt.type == TB_FLOAT && dt.data == TB_FLT_64) {
            comparand = tb_inst_float64(func, 0.0);
        } else {
            comparand = tb_inst_uint(func, dt, 0);
        }

        if (dt.type == TB_INT && tb_node_is_constant_zero(reg)) {
            reg = tb_inst_uint(func, TB_TYPE_BOOL, 0);
        } else if (dt.type == TB_INT && tb_node_is_constant_non_zero(reg)) {
            reg = tb_inst_uint(func, TB_TYPE_BOOL, 1);
        } else {
            reg = tb_inst_cmp_ne(func, reg, comparand);
        }
    } else if (src->kind == KIND_BOOL && cuik_type_is_integer(dst)) {
        reg = tb_inst_zxt(func, reg, ctype_to_tbtype(dst));
    } else if (cuik_type_is_integer(src) && cuik_type_is_integer(dst)) {
        if (dst->size > src->size) {
            // up-casts
            if (src->is_unsigned) {
                reg = tb_inst_zxt(func, reg, ctype_to_tbtype(dst));
            } else {
                reg = tb_inst_sxt(func, reg, ctype_to_tbtype(dst));
            }
        } else if (dst->size < src->size) {
            // down-casts
            reg = tb_inst_trunc(func, reg, ctype_to_tbtype(dst));
        }
    } else if (cuik_type_is_integer(src) && dst->kind == KIND_FUNC) {
        // integer -> function
        reg = tb_inst_int2ptr(func, reg);
    } else if (src->kind == KIND_FUNC && cuik_type_is_integer(dst)) {
        // function -> integer
        reg = tb_inst_ptr2int(func, reg, ctype_to_tbtype(dst));
    } else if (cuik_type_is_integer(src) && dst->kind == KIND_PTR) {
        reg = tb_inst_int2ptr(func, reg);
    } else if (src->kind == KIND_PTR && cuik_type_is_integer(dst)) {
        reg = tb_inst_ptr2int(func, reg, ctype_to_tbtype(dst));
    } else if (src->kind == KIND_PTR && dst->kind == KIND_PTR) {
        /* TB has opaque pointers, nothing needs to be done. */
    } else if (src->kind == KIND_FLOAT && dst->kind == KIND_DOUBLE) {
        TB_DataType dt = reg->dt;

        if (!(dt.type == TB_FLOAT && dt.data == TB_FLT_64 && dt.width == 0)) {
            reg = tb_inst_fpxt(func, reg, TB_TYPE_F64);
        }
    } else if (src->kind == KIND_DOUBLE && dst->kind == KIND_FLOAT) {
        TB_DataType dt = reg->dt;

        if (!(dt.type == TB_FLOAT && dt.data == TB_FLT_32 && dt.width == 0)) {
            reg = tb_inst_trunc(func, reg, TB_TYPE_F32);
        }
    } else if (cuik_type_is_float(src) && cuik_type_is_integer(dst)) {
        reg = tb_inst_float2int(func, reg, ctype_to_tbtype(dst), !dst->is_unsigned);
    } else if (cuik_type_is_integer(src) && cuik_type_is_float(dst)) {
        reg = tb_inst_int2float(func, reg, ctype_to_tbtype(dst), !src->is_unsigned);
    }

    assert(reg);
    return reg;
}

static TB_Node* cvt2rval(TranslationUnit* tu, TB_Function* func, IRVal* v) {
    Cuik_Type* dst = cuik_canonical_type(v->cast_type);
    Cuik_Type* src = cuik_canonical_type(v->type);
    bool is_volatile = CUIK_QUAL_TYPE_HAS(v->type, CUIK_QUAL_VOLATILE);

    TB_Node* reg = 0;
    switch (v->value_type) {
        case RVALUE: {
            reg = v->reg;
            break;
        }
        case RVALUE_PHI: {
            fallthrough_label(func, v->phi.if_true);
            tb_inst_goto(func, v->phi.merger);

            tb_inst_set_control(func, v->phi.if_false);
            tb_inst_goto(func, v->phi.merger);

            TB_Node* one = tb_inst_bool(func, true);
            TB_Node* zero = tb_inst_bool(func, false);

            tb_inst_set_control(func, v->phi.merger);
            reg = tb_inst_phi2(func, v->phi.merger, one, zero);
            break;
        }
        case LVALUE: {
            // Implicit array to pointer
            if (src->kind == KIND_ARRAY || src->kind == KIND_FUNC) {
                // just pass the address don't load
                src = dst;
                reg = v->reg;
            } else {
                reg = tb_inst_load(func, ctype_to_tbtype(src), v->reg, src->align, is_volatile);
            }
            break;
        }
        case LVALUE_BITS: {
            uint64_t mask = (UINT64_MAX >> (64ull - v->bits.width));
            TB_DataType dt = ctype_to_tbtype(src);

            reg = tb_inst_load(func, dt, v->reg, src->align, is_volatile);
            if (v->bits.width != (src->size * 8)) {
                reg = tb_inst_and(func, reg, tb_inst_uint(func, dt, mask));
            }

            if (v->bits.offset) {
                reg = tb_inst_shr(func, reg, tb_inst_uint(func, dt, v->bits.offset));
            }
            break;
        }
        default:
        abort();
    }

    if (dst->kind == KIND_VOID) {
        return 0;
    }

    return src != dst ? cast_reg(func, reg, src, dst) : reg;
}

TB_Node* cvt2lval(TranslationUnit* tu, TB_Function* func, IRVal* v) {
    if (v->value_type == LVALUE) {
        return v->reg;
    } else if (v->value_type == RVALUE) {
        Cuik_Type* t = cuik_canonical_type(v->cast_type);
        bool is_volatile = CUIK_QUAL_TYPE_HAS(v->cast_type, CUIK_QUAL_VOLATILE);

        // spawn a lil temporary
        TB_DataType dt = v->reg->dt;

        TB_Node* temporary = tb_inst_local(func, t->size, t->align);
        tb_inst_store(func, dt, temporary, v->reg, t->align, is_volatile);
        return temporary;
    } else {
        abort();
    }
}

static int pass_parameter(TranslationUnit* tu, TB_Function* func, TB_PassingRule rule, IRVal arg, bool is_vararg, TB_Node** out_param) {
    Cuik_Type* arg_type = cuik_canonical_type(arg.type);
    bool is_volatile = CUIK_QUAL_TYPE_HAS(arg.type, CUIK_QUAL_VOLATILE);

    switch (rule) {
        case TB_PASSING_INDIRECT: {
            // const pass-by-value is considered as a const ref
            // since it doesn't mutate
            TB_Node* arg_addr = TB_NULL_REG;
            switch (arg.value_type) {
                case LVALUE:
                arg_addr = arg.reg;
                break;
                case RVALUE: {
                    // spawn a lil temporary
                    TB_CharUnits size = arg_type->size;
                    TB_CharUnits align = arg_type->align;
                    TB_DataType dt = arg.reg->dt;

                    arg_addr = tb_inst_local(func, size, align);
                    tb_inst_store(func, dt, arg_addr, arg.reg, align, is_volatile);
                    break;
                }
                default:
                break;
            }
            assert(arg_addr);

            // TODO(NeGate): we might wanna define some TB instruction
            // for killing locals since some have really limited lifetimes
            TB_CharUnits size = arg_type->size;
            TB_CharUnits align = arg_type->align;

            if (0 /* arg_type->is_const */) {
                out_param[0] = arg_addr;
            } else {
                TB_Node* temp_slot = tb_inst_local(func, size, align);
                TB_Node* size_reg = tb_inst_uint(func, TB_TYPE_I64, size);

                tb_inst_memcpy(func, temp_slot, arg_addr, size_reg, align, is_volatile);

                out_param[0] = temp_slot;
            }

            return 1;
        }
        case TB_PASSING_DIRECT: {
            if (arg_type->kind == KIND_STRUCT || arg_type->kind == KIND_UNION) {
                return 1;
            } else {
                TB_Node* n = cvt2rval(tu, func, &arg);
                TB_DataType dt = n->dt;

                if (is_vararg && dt.type == TB_FLOAT && dt.data == TB_FLT_64 && dt.width == 0) {
                    // convert any float variadic arguments into integers
                    n = tb_inst_bitcast(func, n, TB_TYPE_I64);
                }

                out_param[0] = n;
                return 1;
            }
        }

        default:
        assert(0 && "TODO");
        return 0;
    }
}

int count_max_tb_init_objects(InitNode* root_node) {
    int sum = root_node->kids_count;
    for (InitNode* k = root_node->kid; k != NULL; k = k->next) {
        sum += count_max_tb_init_objects(k);
        if (k->expr && get_root_subexpr(k->expr)->op == EXPR_ADDR) sum += 1;
    }

    return sum;
}

static void eval_local_initializer(TranslationUnit* tu, TB_Function* func, TB_Node* addr, InitNode* n) {
    if (n->kid != NULL) {
        for (InitNode* k = n->kid; k != NULL; k = k->next) {
            eval_local_initializer(tu, func, addr, k);
        }
    } else {
        Cuik_Type* child_type = cuik_canonical_type(n->type);
        int offset = n->offset;

        TB_Node* val = irgen_as_rvalue(tu, func, n->expr);
        TB_DataType dt = val->dt;

        Cuik_Type* type = cuik_canonical_type(get_root_type(n->expr));
        if (n->mode == INIT_ARRAY && n->count > 1) {
            size_t size = child_type->size;
            size_t count = n->count;

            for (size_t i = 0; i < count; i++) {
                TB_Node* addr_offset = tb_inst_member_access(func, addr, n->offset + (i * size));
                tb_inst_store(func, dt, addr_offset, val, type->align, false);
            }
        } else if (type->kind == KIND_ARRAY && child_type->kind == KIND_ARRAY) {
            TB_Node* addr_offset = tb_inst_member_access(func, addr, n->offset);
            tb_inst_memcpy(func, addr_offset, val, tb_inst_uint(func, TB_TYPE_I64, type->size), type->align, false);
        } else {
            TB_Node* addr_offset = tb_inst_member_access(func, addr, n->offset);
            tb_inst_store(func, dt, addr_offset, val, type->align, false);
        }
    }
}

static void gen_local_initializer(TranslationUnit* tu, TB_Function* func, TB_Node* addr, Cuik_Type* type, InitNode* root_node) {
    TB_Node* size_reg = tb_inst_uint(func, TB_TYPE_I64, type->size);
    TB_Node* val_reg = tb_inst_uint(func, TB_TYPE_I8, 0);
    tb_inst_memset(func, addr, val_reg, size_reg, type->align, false);

    eval_local_initializer(tu, func, addr, root_node);
}

TB_ModuleSection* get_variable_storage(TB_Module* m, const Attribs* attrs, bool is_const) {
    if (attrs->is_tls) {
        return tb_module_get_tls(m);
    } else if (is_const) {
        return tb_module_get_rdata(m);
    } else {
        return tb_module_get_data(m);
    }
}

static void eval_global_initializer(TranslationUnit* tu, TB_Global* g, InitNode* n, int offset);
static void gen_global_initializer(TranslationUnit* tu, TB_Global* g, Cuik_Type* type, Cuik_Expr* e, size_t offset) {
    assert(type != NULL);
    size_t type_size = type->size;

    // defaults to zeros because that's how TB initializers work
    if (e == NULL) {
        return;
    }

    // string literals
    Subexpr* s = get_root_subexpr(e);
    if (s->op == EXPR_STR || s->op == EXPR_WSTR) {
        size_t len = s->str.end - s->str.start;

        if (type->kind == KIND_PTR) {
            uint32_t hash = murmur3_32(s->str.start, len);

            TB_Global* dummy = tb_global_create(tu->ir_mod, 0, NULL, NULL, TB_LINKAGE_PRIVATE);
            ((TB_Symbol*) dummy)->ordinal = ((uint64_t) tu->local_ordinal << 32ull) | hash;
            tb_global_set_storage(tu->ir_mod, tb_module_get_rdata(tu->ir_mod), dummy, len, cuik_canonical_type(type->ptr_to)->align, 1);

            char* dst = tb_global_add_region(tu->ir_mod, dummy, 0, len);
            memcpy(dst, s->str.start, len);

            tb_global_add_symbol_reloc(tu->ir_mod, g, offset, (TB_Symbol*) dummy);
        } else {
            char* dst = tb_global_add_region(tu->ir_mod, g, offset, type->size);
            memcpy(dst, s->str.start, len);
        }
        return;
    }

    // try to emit global initializer
    if (s->op == EXPR_INITIALIZER) {
        Subexpr* s = get_root_subexpr(e);
        eval_global_initializer(tu, g, s->init.root, offset);
        return;
    }

    // try to emit constant integer + constant addresses
    Cuik_ConstVal value;
    if (const_eval(NULL, e, &value)) {
        uint64_t int_form = 0;
        if (value.tag == CUIK_CONST_ADDR) {
            Stmt* stmt = e->exprs[value.s.base].sym.stmt;
            assert((stmt->op == STMT_GLOBAL_DECL || stmt->op == STMT_FUNC_DECL) && "could not resolve as constant initializer");

            tb_global_add_symbol_reloc(tu->ir_mod, g, offset, stmt->backing.s);
            int_form = value.s.offset;
        } else if (value.tag == CUIK_CONST_INT) {
            int_form = value.i;
        } else if (value.tag == CUIK_CONST_FLOAT) {
            Cuik_TypeKind kind = cuik_canonical_type(e->cast_types[e->count - 1])->kind;
            if (kind == KIND_DOUBLE) {
                typedef union { double f; uint64_t u; } F64U64;
                int_form = (F64U64){ value.f }.u;
            } else if (kind == KIND_FLOAT) {
                typedef union { float f; uint32_t u; } F32U32;
                int_form = (F32U32){ value.f }.u;
            } else {
                assert(0 && "TODO");
            }
        } else {
            assert(0 && "TODO");
        }

        if (int_form != 0) {
            uint8_t* region = tb_global_add_region(tu->ir_mod, g, offset, type_size);

            if (TARGET_NEEDS_BYTESWAP(tu->target)) {
                // reverse copy
                uint8_t* src = (uint8_t*) &int_form;
                size_t top = type_size - 1;

                for (size_t i = 0; i < type_size; i++) {
                    region[i] = src[top - i];
                }
            } else {
                memcpy(region, &int_form, type->size);
            }
        }
        return;
    }

    fprintf(stderr, "internal compiler error: cannot compile global initializer as constant (%s).\n", tu->filepath);
    abort();
}

static void eval_global_initializer(TranslationUnit* tu, TB_Global* g, InitNode* n, int offset) {
    if (n->kid != NULL) {
        for (InitNode* k = n->kid; k != NULL; k = k->next) {
            eval_global_initializer(tu, g, k, offset);
        }
    } else {
        Cuik_Type* child_type = cuik_canonical_type(n->type);
        gen_global_initializer(tu, g, child_type, n->expr, offset + n->offset);
    }
}

static void insert_label(TB_Function* func) {
    TB_Node* last = tb_inst_get_control(func);
    if (last == NULL) {
        tb_inst_set_control(func, tb_inst_region(func));
    }
}

#define GET_TYPE() (_->types[e - _->exprs])
#define GET_CAST_TYPE() (_->cast_types[e - _->exprs])
#define GET_ARG(i) args[i]
#define RVAL(i) cvt2rval(tu, func, &args[i])
static IRVal irgen_subexpr(TranslationUnit* tu, TB_Function* func, Cuik_Expr* _, Subexpr* e, int arg_count, IRVal* args) {
    switch (e->op) {
        case EXPR_CHAR:
        case EXPR_WCHAR: {
            TB_DataType dt = ctype_to_tbtype(cuik_canonical_type(GET_TYPE()));
            return (IRVal){
                .value_type = RVALUE,
                .reg = tb_inst_uint(func, dt, e->char_lit)
            };
        }
        case EXPR_INT: {
            Cuik_Type* t = cuik_canonical_type(GET_TYPE());
            TB_DataType dt = ctype_to_tbtype(t);

            if (t->kind == KIND_FLOAT) {
                return (IRVal){
                    .value_type = RVALUE,
                    .reg = tb_inst_float32(func, e->int_lit.lit),
                };
            } else if (t->kind == KIND_DOUBLE) {
                return (IRVal){
                    .value_type = RVALUE,
                    .reg = tb_inst_float64(func, e->int_lit.lit),
                };
            } else if (t->is_unsigned) {
                return (IRVal){
                    .value_type = RVALUE,
                    .reg = tb_inst_uint(func, dt, e->int_lit.lit),
                };
            } else {
                // TODO(NeGate): maybe this should use tb_inst_sint?
                return (IRVal){
                    .value_type = RVALUE,
                    .reg = tb_inst_uint(func, dt, e->int_lit.lit),
                };
            }
        }
        case EXPR_SIZEOF: {
            Cuik_Type* src = cuik_canonical_type(GET_ARG(0).type);
            return (IRVal){
                .value_type = RVALUE,
                .reg = tb_inst_sint(func, TB_TYPE_I64, src->size),
            };
        }
        case EXPR_SIZEOF_T: {
            Cuik_Type* src = cuik_canonical_type(e->x_of_type.type);
            return (IRVal){
                .value_type = RVALUE,
                .reg = tb_inst_sint(func, TB_TYPE_I64, src->size),
            };
        }
        case EXPR_ENUM: {
            return (IRVal){
                .value_type = RVALUE,
                .reg = tb_inst_sint(func, TB_TYPE_I32, e->enum_val.num->value),
            };
        }
        case EXPR_FLOAT32:
        case EXPR_FLOAT64: {
            bool is_float32 = cuik_canonical_type(GET_CAST_TYPE())->kind == KIND_FLOAT;

            return (IRVal){
                .value_type = RVALUE,
                .reg = is_float32
                    ? tb_inst_float32(func, e->float_lit)
                    : tb_inst_float64(func, e->float_lit),
            };
        }
        case EXPR_STR:
        case EXPR_WSTR: {
            // The string is preprocessed to be a flat and nice byte buffer by the semantics pass
            size_t len = e->str.end - e->str.start;
            uint32_t hash = murmur3_32(e->str.start, len);

            TB_Global* dummy = tb_global_create(tu->ir_mod, 0, NULL, NULL, TB_LINKAGE_PRIVATE);
            ((TB_Symbol*) dummy)->ordinal = ((uint64_t) tu->local_ordinal << 32ull) | hash;
            tb_global_set_storage(tu->ir_mod, tb_module_get_rdata(tu->ir_mod), dummy, len, 1, 1);

            char* dst = tb_global_add_region(tu->ir_mod, dummy, 0, len);
            memcpy(dst, e->str.start, len);

            return (IRVal){
                .value_type = RVALUE,
                .reg = tb_inst_get_symbol_address(func, (TB_Symbol*) dummy)
            };
        }
        case EXPR_INITIALIZER: {
            Cuik_Type* type = cuik_canonical_type(e->init.type);
            TB_Node* addr = tb_inst_local(func, type->size, type->align);

            gen_local_initializer(tu, func, addr, type, e->init.root);

            return (IRVal){
                .value_type = LVALUE,
                .reg = addr
            };
        }
        case EXPR_VA_ARG: {
            IRVal src = GET_ARG(0);
            assert(src.value_type == LVALUE);

            // post-increment
            // NOTE: this assumes pointer size is 64bit
            TB_Node* pre = tb_inst_load(func, TB_TYPE_PTR, src.reg, 8, false);
            TB_Node* post = tb_inst_member_access(func, pre, 8);
            tb_inst_store(func, TB_TYPE_PTR, src.reg, post, 8, false);

            return (IRVal){
                .value_type = LVALUE,
                .reg = pre
            };
        }
        case EXPR_BUILTIN_SYMBOL: {
            return (IRVal){
                .value_type = LVALUE_EXPR,
                .e = e,
            };
        }
        case EXPR_SYMBOL: {
            Stmt* stmt = e->sym.stmt;
            assert(stmt->op == STMT_DECL || stmt->op == STMT_LABEL || stmt->op == STMT_GLOBAL_DECL || stmt->op == STMT_FUNC_DECL);

            Cuik_Type* type = cuik_canonical_type(stmt->decl.type);
            if (stmt->op == STMT_LABEL) {
                if (stmt->backing.r == NULL) {
                    stmt->backing.r = tb_inst_region(func);
                }

                return (IRVal){
                    .value_type = LVALUE_LABEL,
                    .reg = stmt->backing.r,
                };
            } else if (stmt->op == STMT_FUNC_DECL) {
                return (IRVal){
                    .value_type = LVALUE,
                    .reg = tb_inst_get_symbol_address(func, stmt->backing.s),
                };
            } else if (type->kind == KIND_FUNC || stmt->op == STMT_GLOBAL_DECL || (stmt->op == STMT_DECL && stmt->decl.attrs.is_static)) {
                if (stmt->backing.s == NULL) {
                    // check if it's defined by another TU
                    // functions are external by default
                    const char* name = (const char*) stmt->decl.name;

                    if (tu->parent != NULL) {
                        stmt->backing.s = get_external(tu->parent, name);
                    } else {
                        stmt->backing.e = tb_extern_create(tu->ir_mod, -1, name, TB_EXTERNAL_SO_LOCAL);
                    }
                }

                assert(stmt->backing.s != NULL);
                return (IRVal){
                    .value_type = LVALUE,
                    .reg = tb_inst_get_symbol_address(func, stmt->backing.s),
                };
            } else {
                return (IRVal){
                    .value_type = LVALUE,
                    .reg = stmt->backing.r,
                };
            }
        }
        case EXPR_PARAM: {
            int param_num = e->param_num;
            TB_Node* reg = parameter_map[param_num];

            Cuik_Type* arg_type = cuik_canonical_type(function_type->func.param_list[param_num].type);
            assert(arg_type != NULL);

            return (IRVal){
                .value_type = LVALUE,
                .reg = reg
            };
        }
        case EXPR_DOT_R: {
            IRVal lhs = GET_ARG(0);
            assert(lhs.value_type == LVALUE);

            Member* member = e->dot_arrow.member;
            assert(member != NULL);

            if (member->is_bitfield) {
                return (IRVal){
                    .value_type = LVALUE_BITS,
                    .bits = {
                        .reg = tb_inst_member_access(func, lhs.reg, e->dot_arrow.offset),
                        .offset = member->bit_offset,
                        .width = member->bit_width,
                    },
                };
            } else {
                return (IRVal){
                    .value_type = LVALUE,
                    .reg = tb_inst_member_access(func, lhs.reg, e->dot_arrow.offset),
                };
            }
        }
        case EXPR_ARROW_R: {
            TB_Node* src = RVAL(0);

            Member* member = e->dot_arrow.member;
            assert(member != NULL);

            if (member->is_bitfield) {
                return (IRVal){
                    .value_type = LVALUE_BITS,
                    .bits = {
                        .reg = tb_inst_member_access(func, src, e->dot_arrow.offset),
                        .offset = member->bit_offset,
                        .width = member->bit_width,
                    },
                };
            } else {
                return (IRVal){
                    .value_type = LVALUE,
                    .reg = tb_inst_member_access(func, src, e->dot_arrow.offset),
                };
            }
        }
        case EXPR_SUBSCRIPT: {
            TB_Node* base  = RVAL(0);
            TB_Node* index = RVAL(1);

            int stride = cuik_canonical_type(GET_TYPE())->size;
            if (stride == 0) stride = 1;

            return (IRVal){
                .value_type = LVALUE,
                .reg = tb_inst_array_access(func, base, index, stride),
            };
        }
        case EXPR_DEREF: {
            TB_Node* reg = RVAL(0);
            if (cuik_canonical_type(GET_TYPE())->kind == KIND_FUNC) {
                return (IRVal){
                    .value_type = RVALUE,
                    .reg = reg,
                };
            } else {
                return (IRVal){
                    .value_type = LVALUE,
                    .reg = reg,
                };
            }
        }
        case EXPR_CALL: {
            int arg_count = e->call.param_count;

            IRVal* target = &GET_ARG(0);
            if (target->value_type == LVALUE_EXPR && target->e->op == EXPR_BUILTIN_SYMBOL) {
                const char* name = (const char*) target->e->builtin_sym.name;
                TB_Node* val = tu->target->compile_builtin(tu, func, name, arg_count, args);

                return (IRVal){
                    .value_type = RVALUE,
                    .reg = val,
                };
            }

            Cuik_Type* return_type = cuik_canonical_type(GET_TYPE());

            // point at which it stops being know which parameter types we're
            // mapping to, if it's arg_count then there's really none
            size_t varargs_cutoff = arg_count;
            Cuik_Type* func_type = cuik_canonical_type(target->type);
            if (func_type->kind == KIND_PTR) {
                func_type = cuik_canonical_type(func_type->ptr_to);
            }

            if (func_type->func.has_varargs) {
                varargs_cutoff = func_type->func.param_count;
            }

            // Resolve call target
            //
            // NOTE(NeGate): Could have been resized in the parameter's irgen_expr
            // so we reload the pointer.
            TB_Node* target_node = RVAL(0);

            // generate custom prototype for function type
            TB_DebugType* dbg = cuik__as_tb_debug_type(tu->ir_mod, func_type);
            TB_FunctionPrototype* call_prototype = tb_prototype_from_dbg(tu->ir_mod, dbg);

            // pass parameters
            TB_PassingRule return_rule = TB_PASSING_DIRECT;
            if (return_type->kind != KIND_VOID) {
                TB_DebugType* ret_dbg = tb_debug_func_returns(dbg)[0];
                return_rule = tb_get_passing_rule_from_dbg(tu->ir_mod, ret_dbg, true);
            }

            size_t real_arg_count = call_prototype->param_count;
            size_t ir_arg_count = 0;
            TB_Node** ir_args = tls_push(real_arg_count * sizeof(TB_Node*));
            if (return_rule == TB_PASSING_INDIRECT) {
                ir_args[ir_arg_count++] = tb_inst_local(func, return_type->size, return_type->align);
            }

            TB_DebugType** params = tb_debug_func_params(dbg);
            for (size_t i = 0; i < arg_count; i++) {
                TB_PassingRule rule = tb_get_passing_rule_from_dbg(tu->ir_mod, tb_debug_field_type(params[i]), false);
                ir_arg_count += pass_parameter(tu, func, rule, args[i + 1], i >= varargs_cutoff, &ir_args[ir_arg_count]);
            }
            assert(ir_arg_count == real_arg_count);

            TB_MultiOutput out = tb_inst_call(func, call_prototype, target_node, real_arg_count, ir_args);
            tls_restore(ir_args);

            if (out.count == 0) {
                return (IRVal){
                    .value_type = RVALUE,
                    .reg = NULL,
                };
            } else if (return_rule == TB_PASSING_INDIRECT) {
                return (IRVal){
                    .value_type = LVALUE,
                    .reg = out.single,
                };
            } else if (out.count > 1) {
                assert(0 && "TODO: multiple return ABI stuff");
            } else {
                TB_Node* ret = out.single;

                if (return_type->kind == KIND_STRUCT || return_type->kind == KIND_UNION) {
                    // spawn a lil temporary
                    TB_Node* addr = tb_inst_local(func, return_type->size, return_type->align);
                    tb_inst_store(func, ret->dt, addr, ret, return_type->align, false);

                    return (IRVal){
                        .value_type = LVALUE,
                        .reg = addr,
                    };
                } else {
                    return (IRVal){
                        .value_type = RVALUE,
                        .reg = ret,
                    };
                }
            }
        }
        case EXPR_ADDR: {
            IRVal src = GET_ARG(0);

            assert(src.value_type == LVALUE);
            src.value_type = RVALUE;
            return src;
        }
        case EXPR_CAST: {
            TB_Node* src = RVAL(0);
            Cuik_Type* t = cuik_canonical_type(e->cast.type);

            // stuff like ((void) x)
            if (t->kind == KIND_VOID) {
                return (IRVal){.value_type = RVALUE, .reg = 0};
            }

            return (IRVal){
                .value_type = RVALUE,
                .reg = src
            };
        }
        case EXPR_NOT: {
            return (IRVal){
                .value_type = RVALUE,
                .reg = tb_inst_not(func, RVAL(0)),
            };
        }
        case EXPR_NEGATE: {
            return (IRVal){
                .value_type = RVALUE,
                .reg = tb_inst_neg(func, RVAL(0)),
            };
        }
        case EXPR_LOGICAL_NOT: {
            TB_Node* src = RVAL(0);
            return (IRVal){
                .value_type = RVALUE,
                .reg = tb_inst_cmp_eq(func, src, tb_inst_uint(func, src->dt, 0)),
            };
        }
        case EXPR_LOGICAL_AND:
        case EXPR_LOGICAL_OR: {
            // a && b
            //
            //          if (a) { goto try_rhs } else { goto false }
            // try_rhs: if (b) { goto true    } else { goto false }
            //
            //
            // a || b
            //
            //          if (a) { goto true    } else { goto try_rhs }
            // try_rhs: if (b) { goto true    } else { goto false }
            bool is_and = (e->op == EXPR_LOGICAL_AND);
            TB_Node* try_rhs_lbl = tb_inst_region(func);

            // Eval first operand
            IRVal a = irgen_expr(tu, func, e->logical_binop.left);

            TB_Node *true_lbl, *false_lbl;
            if (a.value_type == RVALUE_PHI) {
                // chain with previous phi.
                // OR  chains on false,
                // AND chains on true
                if (is_and) {
                    tb_inst_set_control(func, a.phi.if_true);
                    tb_inst_goto(func, try_rhs_lbl);

                    true_lbl = tb_inst_region(func);
                    false_lbl = a.phi.if_false;
                } else {
                    tb_inst_set_control(func, a.phi.if_false);
                    tb_inst_goto(func, try_rhs_lbl);

                    true_lbl = a.phi.if_true;
                    false_lbl = tb_inst_region(func);
                }
            } else {
                true_lbl = tb_inst_region(func);
                false_lbl = tb_inst_region(func);

                TB_Node* a_reg = cvt2rval(tu, func, &a);
                if (is_and) {
                    tb_inst_if(func, a_reg, try_rhs_lbl, false_lbl);
                } else {
                    tb_inst_if(func, a_reg, true_lbl, try_rhs_lbl);
                }
            }

            // Eval second operand
            tb_inst_set_control(func, try_rhs_lbl);

            TB_Node* b = irgen_as_rvalue(tu, func, e->logical_binop.right);
            tb_inst_if(func, b, true_lbl, false_lbl);

            TB_Node* merge = tb_inst_region(func);
            tb_inst_set_control(func, merge);

            return (IRVal){
                .value_type = RVALUE_PHI,
                .phi = { true_lbl, false_lbl, merge },
            };
        }
        case EXPR_PTRADD:
        case EXPR_PTRSUB: {
            TB_Node* l = RVAL(e->ptrop.flipped);
            TB_Node* r = RVAL(!e->ptrop.flipped);

            Cuik_Type* type = cuik_canonical_type(GET_TYPE());

            // pointer arithmatic
            int dir = e->op == EXPR_PTRADD ? 1 : -1;
            int stride = cuik_canonical_type(type->ptr_to)->size;

            assert(stride);
            return (IRVal){
                .value_type = RVALUE,
                .reg = tb_inst_array_access(func, l, r, dir * stride),
            };
        }
        case EXPR_PTRDIFF: {
            TB_Node* l = RVAL(0);
            TB_Node* r = RVAL(1);

            Cuik_Type* type = cuik_canonical_type(GET_ARG(0).cast_type);
            int stride = cuik_canonical_type(type->ptr_to)->size;

            // TODO(NeGate): consider a ptrdiff operation in TB
            l = tb_inst_ptr2int(func, l, TB_TYPE_I64);
            r = tb_inst_ptr2int(func, r, TB_TYPE_I64);

            TB_Node* diff = tb_inst_sub(func, l, r, TB_ARITHMATIC_NSW | TB_ARITHMATIC_NUW);
            TB_Node* diff_in_elems = tb_inst_div(func, diff, tb_inst_sint(func, diff->dt, stride), true);

            return (IRVal){
                .value_type = RVALUE,
                .reg = diff_in_elems,
            };
        }
        case EXPR_COMMA: {
            TB_Node* r = RVAL(1);
            return (IRVal){
                .value_type = RVALUE,
                .reg = r,
            };
        }
        case EXPR_PLUS:
        case EXPR_MINUS:
        case EXPR_TIMES:
        case EXPR_SLASH:
        case EXPR_PERCENT:
        case EXPR_AND:
        case EXPR_OR:
        case EXPR_XOR:
        case EXPR_SHL:
        case EXPR_SHR: {
            TB_Node* l = RVAL(0);
            TB_Node* r = RVAL(1);
            Cuik_Type* restrict type = cuik_canonical_type(GET_TYPE());

            TB_Node* data;
            if (type->kind == KIND_FLOAT || type->kind == KIND_DOUBLE) {
                switch (e->op) {
                    case EXPR_PLUS:  data = tb_inst_fadd(func, l, r); break;
                    case EXPR_MINUS: data = tb_inst_fsub(func, l, r); break;
                    case EXPR_TIMES: data = tb_inst_fmul(func, l, r); break;
                    case EXPR_SLASH: data = tb_inst_fdiv(func, l, r); break;
                    default: TODO();
                }
            } else {
                TB_ArithmeticBehavior ab = type->is_unsigned ? 0 : TB_ARITHMATIC_NSW;

                switch (e->op) {
                    case EXPR_PLUS:    data = tb_inst_add(func, l, r, ab);                 break;
                    case EXPR_MINUS:   data = tb_inst_sub(func, l, r, ab);                 break;
                    case EXPR_TIMES:   data = tb_inst_mul(func, l, r, ab);                 break;
                    case EXPR_SLASH:   data = tb_inst_div(func, l, r, !type->is_unsigned); break;
                    case EXPR_PERCENT: data = tb_inst_mod(func, l, r, !type->is_unsigned); break;
                    case EXPR_AND:     data = tb_inst_and(func, l, r);                     break;
                    case EXPR_OR:      data = tb_inst_or(func, l, r);                      break;
                    case EXPR_XOR:     data = tb_inst_xor(func, l, r);                     break;
                    case EXPR_SHL:     data = tb_inst_shl(func, l, r, ab);                 break;
                    case EXPR_SHR:     data = type->is_unsigned ? tb_inst_shr(func, l, r) : tb_inst_sar(func, l, r); break;
                    default: TODO();
                }

                if (type->kind == KIND_BOOL) {
                    // convert into proper bool
                    data = tb_inst_cmp_ne(func, data, tb_inst_uint(func, TB_TYPE_BOOL, 0));
                }
            }

            return (IRVal){
                .value_type = RVALUE,
                .reg = data,
            };
        }
        case EXPR_CMPEQ:
        case EXPR_CMPNE: {
            TB_Node* l = RVAL(0);
            TB_Node* r = RVAL(1);

            return (IRVal){
                .value_type = RVALUE,
                .reg = (e->op == EXPR_CMPEQ ? tb_inst_cmp_eq : tb_inst_cmp_ne)(func, l, r),
            };
        }
        case EXPR_CMPGT:
        case EXPR_CMPGE:
        case EXPR_CMPLT:
        case EXPR_CMPLE: {
            TB_Node* l = RVAL(0);
            TB_Node* r = RVAL(1);

            Cuik_Type* type = cuik_canonical_type(GET_ARG(0).cast_type);
            TB_Node* data;
            if (type->kind == KIND_FLOAT || type->kind == KIND_DOUBLE) {
                switch (e->op) {
                    case EXPR_CMPGT: data = tb_inst_cmp_fgt(func, l, r); break;
                    case EXPR_CMPGE: data = tb_inst_cmp_fge(func, l, r); break;
                    case EXPR_CMPLT: data = tb_inst_cmp_flt(func, l, r); break;
                    case EXPR_CMPLE: data = tb_inst_cmp_fle(func, l, r); break;
                    default: TODO();
                }
            } else if (type->kind == KIND_PTR) {
                switch (e->op) {
                    case EXPR_CMPGT: data = tb_inst_cmp_igt(func, l, r, false); break;
                    case EXPR_CMPGE: data = tb_inst_cmp_ige(func, l, r, false); break;
                    case EXPR_CMPLT: data = tb_inst_cmp_ilt(func, l, r, false); break;
                    case EXPR_CMPLE: data = tb_inst_cmp_ile(func, l, r, false); break;
                    default: TODO();
                }
            } else {
                switch (e->op) {
                    case EXPR_CMPGT: data = tb_inst_cmp_igt(func, l, r, !type->is_unsigned); break;
                    case EXPR_CMPGE: data = tb_inst_cmp_ige(func, l, r, !type->is_unsigned); break;
                    case EXPR_CMPLT: data = tb_inst_cmp_ilt(func, l, r, !type->is_unsigned); break;
                    case EXPR_CMPLE: data = tb_inst_cmp_ile(func, l, r, !type->is_unsigned); break;
                    default: TODO();
                }
            }

            return (IRVal){
                .value_type = RVALUE,
                .reg = data,
            };
        }
        case EXPR_PRE_INC:
        case EXPR_PRE_DEC:
        case EXPR_POST_INC:
        case EXPR_POST_DEC: {
            bool is_inc = (e->op == EXPR_PRE_INC || e->op == EXPR_POST_INC);
            bool is_pre = (e->op == EXPR_PRE_INC || e->op == EXPR_PRE_DEC);

            Cuik_Type* type = cuik_canonical_type(GET_TYPE());
            bool is_volatile = CUIK_QUAL_TYPE_HAS(GET_TYPE(), CUIK_QUAL_VOLATILE);

            IRVal address = GET_ARG(0);
            assert(address.value_type == LVALUE && "unsupported increment/decrement value");

            TB_DataType dt = ctype_to_tbtype(type);

            TB_Node* loaded = TB_NULL_REG;
            if (CUIK_QUAL_TYPE_HAS(GET_TYPE(), CUIK_QUAL_ATOMIC)) {
                TB_Node* stride;
                if (type->kind == KIND_PTR) {
                    // pointer arithmatic
                    stride = tb_inst_uint(func, TB_TYPE_PTR, cuik_canonical_type(type->ptr_to)->size);
                } else {
                    stride = tb_inst_uint(func, ctype_to_tbtype(type), 1);
                }

                loaded = is_inc
                    ? tb_inst_atomic_add(func, address.reg, stride, TB_MEM_ORDER_SEQ_CST)
                    : tb_inst_atomic_sub(func, address.reg, stride, TB_MEM_ORDER_SEQ_CST);

                // for pre-op atomics we can just stop here since we've done the arithmatic.
                //
                // for post-op we need to redo the arithmatic on the loaded value (it's
                // already been done to the value in memory so we don't writeback)
                if (is_pre) {
                    return (IRVal){
                        .value_type = RVALUE, .reg = loaded
                    };
                }
            } else {
                loaded = cvt2rval(tu, func, &address);
            }

            TB_Node* operation;
            if (type->kind == KIND_PTR) {
                // pointer arithmatic
                int32_t stride = cuik_canonical_type(type->ptr_to)->size;

                operation = is_inc
                    ? tb_inst_member_access(func, loaded,  stride)
                    : tb_inst_member_access(func, loaded, -stride);
            } else {
                TB_Node* one = tb_inst_uint(func, dt, 1);
                TB_ArithmeticBehavior ab = type->is_unsigned ? 0 : TB_ARITHMATIC_NSW;

                operation = is_inc
                    ? tb_inst_add(func, loaded, one, ab)
                    : tb_inst_sub(func, loaded, one, ab);
            }

            // writeback (the atomic form does this all in one go... as atomics do)
            if (!CUIK_QUAL_TYPE_HAS(GET_TYPE(), CUIK_QUAL_ATOMIC)) {
                assert(address.value_type == LVALUE);
                tb_inst_store(func, dt, address.reg, operation, type->align, is_volatile);
            }

            return (IRVal){
                .value_type = RVALUE, .reg = is_pre ? operation : loaded
            };
        }
        case EXPR_ASSIGN:
        case EXPR_PLUS_ASSIGN:
        case EXPR_MINUS_ASSIGN:
        case EXPR_TIMES_ASSIGN:
        case EXPR_SLASH_ASSIGN:
        case EXPR_PERCENT_ASSIGN:
        case EXPR_AND_ASSIGN:
        case EXPR_OR_ASSIGN:
        case EXPR_XOR_ASSIGN:
        case EXPR_SHL_ASSIGN:
        case EXPR_SHR_ASSIGN: {
            Cuik_Type* type = cuik_canonical_type(GET_TYPE());
            bool is_volatile = CUIK_QUAL_TYPE_HAS(GET_TYPE(), CUIK_QUAL_VOLATILE);

            if (CUIK_QUAL_TYPE_HAS(GET_TYPE(), CUIK_QUAL_ATOMIC)) {
                // Load inputs
                IRVal rhs = GET_ARG(1);
                IRVal lhs = GET_ARG(0);
                assert(lhs.value_type == LVALUE);

                if (type->kind == KIND_STRUCT || type->kind == KIND_UNION) {
                    // Implement big atomic copy
                    abort();
                } else if (type->kind == KIND_FLOAT || type->kind == KIND_DOUBLE) {
                    TB_Node* r = cvt2rval(tu, func, &rhs);

                    // float assignment can be done atomic by using the normal
                    // integer stuff
                    if (e->op == EXPR_ASSIGN) {
                        tb_inst_atomic_xchg(func, lhs.reg, r, TB_MEM_ORDER_SEQ_CST);

                        return (IRVal){
                            .value_type = RVALUE,
                            .reg = r,
                        };
                    } else {
                        // floats don't really have any atomic operations so just
                        // emulate them all using CAS:
                        //
                        // fn atomic_operate(p: *T, a: T): T
                        //   done := false
                        //   while not done
                        //     value = *p # doesn't really need to be atomic
                        //     done = cas(ptr=p, old_val=value, new_val=operate(value, a))
                        //
                        //   return value + a
                        //
                        assert(0 && "TODO");
                    }
                } else {
                    TB_Node* r = cvt2rval(tu, func, &rhs);

                    if (e->op == EXPR_ASSIGN) {
                        tb_inst_atomic_xchg(func, lhs.reg, r, TB_MEM_ORDER_SEQ_CST);

                        return (IRVal){
                            .value_type = RVALUE,
                            .reg = r,
                        };
                    } else if (e->op == EXPR_PLUS_ASSIGN) {
                        TB_Node* op = tb_inst_atomic_add(func, lhs.reg, r, TB_MEM_ORDER_SEQ_CST);
                        op = tb_inst_add(func, op, r, 0);

                        return (IRVal){
                            .value_type = RVALUE,
                            .reg = op,
                        };
                    } else if (e->op == EXPR_MINUS_ASSIGN) {
                        TB_Node* op = tb_inst_atomic_sub(func, lhs.reg, r, TB_MEM_ORDER_SEQ_CST);
                        op = tb_inst_sub(func, op, r, 0);

                        return (IRVal){
                            .value_type = RVALUE,
                            .reg = op,
                        };
                    } else if (e->op == EXPR_AND_ASSIGN) {
                        TB_Node* op = tb_inst_atomic_and(func, lhs.reg, r, TB_MEM_ORDER_SEQ_CST);
                        op = tb_inst_and(func, op, r);

                        return (IRVal){
                            .value_type = RVALUE,
                            .reg = op,
                        };
                    } else {
                        assert(0 && "TODO atomic operation not ready");
                    }
                }
            } else {
                // Load inputs
                IRVal lhs = GET_ARG(0);

                // don't do this conversion for ASSIGN, since it won't be needing it
                TB_Node* l = e->op == EXPR_ASSIGN ? NULL : cvt2rval(tu, func, &lhs);

                IRVal rhs = GET_ARG(1);

                // Try pointer arithmatic
                if (e->op == EXPR_PLUS_ASSIGN && type->kind == KIND_PTR) {
                    int dir = e->op == EXPR_PLUS_ASSIGN ? 1 : -1;
                    int stride = cuik_canonical_type(type->ptr_to)->size;
                    assert(stride);

                    TB_Node* r = cvt2rval(tu, func, &rhs);
                    TB_Node* arith = tb_inst_array_access(func, l, r, dir * stride);

                    assert(lhs.value_type == LVALUE);
                    tb_inst_store(func, TB_TYPE_PTR, lhs.reg, arith, type->align, is_volatile);
                    return lhs;
                }

                TB_DataType dt = ctype_to_tbtype(type);

                TB_Node* data = TB_NULL_REG;
                if (type->kind == KIND_STRUCT || type->kind == KIND_UNION) {
                    if (e->op != EXPR_ASSIGN) abort();

                    TB_Node* size_reg = tb_inst_uint(func, TB_TYPE_I64, type->size);
                    tb_inst_memcpy(func, lhs.reg, rhs.reg, size_reg, type->align, is_volatile);
                    data = rhs.reg;
                } else if (type->kind == KIND_FLOAT || type->kind == KIND_DOUBLE) {
                    TB_Node* r = cvt2rval(tu, func, &rhs);

                    switch (e->op) {
                        case EXPR_ASSIGN:       data = r;                        break;
                        case EXPR_PLUS_ASSIGN:  data = tb_inst_fadd(func, l, r); break;
                        case EXPR_MINUS_ASSIGN: data = tb_inst_fsub(func, l, r); break;
                        case EXPR_TIMES_ASSIGN: data = tb_inst_fmul(func, l, r); break;
                        case EXPR_SLASH_ASSIGN: data = tb_inst_fdiv(func, l, r); break;
                        default: assert(0 && "TODO");
                    }

                    assert(lhs.value_type == LVALUE);
                    tb_inst_store(func, dt, lhs.reg, data, type->align, is_volatile);
                } else {
                    TB_Node* r = cvt2rval(tu, func, &rhs);
                    TB_ArithmeticBehavior ab = type->is_unsigned ? 0 : TB_ARITHMATIC_NSW;

                    switch (e->op) {
                        case EXPR_ASSIGN:         data = r;                                           break;
                        case EXPR_PLUS_ASSIGN:    data = tb_inst_add(func, l, r, ab);                 break;
                        case EXPR_MINUS_ASSIGN:   data = tb_inst_sub(func, l, r, ab);                 break;
                        case EXPR_TIMES_ASSIGN:   data = tb_inst_mul(func, l, r, ab);                 break;
                        case EXPR_SLASH_ASSIGN:   data = tb_inst_div(func, l, r, !type->is_unsigned); break;
                        case EXPR_PERCENT_ASSIGN: data = tb_inst_mod(func, l, r, !type->is_unsigned); break;
                        case EXPR_AND_ASSIGN:     data = tb_inst_and(func, l, r);                     break;
                        case EXPR_OR_ASSIGN:      data = tb_inst_or(func, l, r);                      break;
                        case EXPR_XOR_ASSIGN:     data = tb_inst_xor(func, l, r);                     break;
                        case EXPR_SHL_ASSIGN:     data = tb_inst_shl(func, l, r, ab);                 break;
                        case EXPR_SHR_ASSIGN:     data = type->is_unsigned ? tb_inst_shr(func, l, r) : tb_inst_sar(func, l, r); break;
                        default: assert(0 && "TODO");
                    }

                    if (lhs.value_type == LVALUE_BITS && lhs.bits.width != (type->size * 8)) {
                        // NOTE(NeGate): the semantics around volatile bitfields are janky at best
                        TB_Node* old_value = tb_inst_load(func, dt, lhs.reg, type->align, is_volatile);

                        // mask out the space for our bitfield member
                        uint64_t clear_mask = ~((UINT64_MAX >> (64ull - lhs.bits.width)) << lhs.bits.offset);
                        old_value = tb_inst_and(func, old_value, tb_inst_uint(func, dt, ~clear_mask));

                        // mask source value and position it correctly
                        uint64_t insert_mask = (UINT64_MAX >> (64ull - lhs.bits.width));
                        data = tb_inst_and(func, data, tb_inst_uint(func, dt, insert_mask));

                        if (lhs.bits.offset) {
                            // nuw & nsw are used since we statically know that the bits.offset won't overflow without some sort of UB
                            data = tb_inst_shl(func, data, tb_inst_uint(func, dt, lhs.bits.offset), TB_ARITHMATIC_NSW | TB_ARITHMATIC_NUW);
                        }

                        // merge
                        data = tb_inst_or(func, old_value, data);
                    } else {
                        assert(lhs.value_type == LVALUE);
                    }

                    tb_inst_store(func, dt, lhs.reg, data, type->align, is_volatile);

                    if (e->op == EXPR_ASSIGN) {
                        assert(data);
                        return (IRVal){
                            .value_type = RVALUE,
                            .reg = data
                        };
                    }
                }

                return lhs;
            }
        }

        case EXPR_TERNARY: {
            Cuik_Type* type = cuik_canonical_type(GET_TYPE());
            TB_DataType dt = ctype_to_tbtype(type);

            TB_Node* cond = RVAL(0);

            TB_Node* if_true = tb_inst_region(func);
            TB_Node* if_false = tb_inst_region(func);
            TB_Node* exit = tb_inst_region(func);

            tb_inst_if(func, cond, if_true, if_false);

            TB_Node* true_val;
            {
                tb_inst_set_control(func, if_true);
                true_val = irgen_as_rvalue(tu, func, e->ternary.left);
                tb_inst_goto(func, exit);
            }

            TB_Node* false_val;
            {
                tb_inst_set_control(func, if_false);
                false_val = irgen_as_rvalue(tu, func, e->ternary.right);
                tb_inst_goto(func, exit);
            }
            tb_inst_set_control(func, exit);

            return (IRVal){
                .value_type = RVALUE,
                .reg = tb_inst_phi2(func, exit, true_val, false_val),
            };
        }

        default:
        log_error("Failed to compile subexpression: %s", cuik_get_expr_name(e));
        return (IRVal){ 0 };
    }
}
#undef GET_CAST_TYPE
#undef GET_TYPE
#undef GET_ARG
#undef RVAL

static IRVal irgen_expr(TranslationUnit* tu, TB_Function* func, Cuik_Expr* e) {
    IRVal stack[1024];
    size_t top = 0;
    Subexpr* exprs = e->exprs;

    size_t i = 0;
    for (; i < e->count; i++) {
        Subexpr* s = &exprs[i];

        // once we know this we can organize the top slice of the stack as the inputs
        int arity = cuik_get_expr_arity(s);
        top -= arity;
        IRVal* args = &stack[top];

        assert(top < 1024 && "Too complex of a constant expression");
        stack[top] = irgen_subexpr(tu, func, e, s, arity, args);
        stack[top].type = e->types[i];
        stack[top].cast_type = e->cast_types[i];
        top += 1;
    }

    assert(top == 1);
    return stack[0];
}

static void emit_location(TranslationUnit* tu, TB_Function* func, SourceLoc loc) {
    if (!tu->has_tb_debug_info) {
        return;
    }

    // TODO(NeGate): Fix this up later!!!
    static thread_local TB_FileID last_file_id;
    static thread_local const char* last_filepath;

    ResolvedSourceLoc rloc = cuikpp_find_location(&tu->tokens, loc);
    if (rloc.file->filename[0] != '<') {
        if (rloc.file->filename != last_filepath) {
            last_filepath = rloc.file->filename;
            last_file_id = tb_file_create(tu->ir_mod, rloc.file->filename);
        }

        tb_inst_set_location(func, last_file_id, rloc.line);
    }
}

static TB_Node* irgen_as_rvalue(TranslationUnit* tu, TB_Function* func, Cuik_Expr* e) {
    IRVal v = irgen_expr(tu, func, e);
    return cvt2rval(tu, func, &v);
}

static void irgen_stmt(TranslationUnit* tu, TB_Function* func, Stmt* restrict s) {
    if (s == NULL) return;

    insert_label(func);
    emit_location(tu, func, s->loc.start);

    switch (s->op) {
        case STMT_NONE: {
            break;
        }
        case STMT_LABEL: {
            if (s->backing.r == 0) {
                s->backing.r = tb_inst_region(func);
            }

            fallthrough_label(func, s->backing.r);
            break;
        }
        case STMT_GOTO: {
            IRVal target = irgen_expr(tu, func, s->goto_.target);

            if (target.value_type == LVALUE_LABEL) {
                tb_inst_goto(func, target.reg);
            } else {
                // TODO(NeGate): Handle computed goto case
                abort();
            }
            break;
        }
        case STMT_COMPOUND: {
            Stmt** kids = s->compound.kids;
            size_t count = s->compound.kids_count;

            for (size_t i = 0; i < count; i++) {
                irgen_stmt(tu, func, kids[i]);
            }
            break;
        }
        case STMT_DECL: {
            Attribs attrs = s->decl.attrs;

            Cuik_Type* type = cuik_canonical_type(s->decl.type);
            Cuik_TypeKind kind = type->kind;
            int size = type->size, align = type->align;

            if (attrs.is_static) {
                // Static initialization
                char* name = tls_push(1024);
                int name_len = snprintf(name, 1024, "%s$%s", function_name, s->decl.name);
                if (name_len < 0 || name_len >= 1024) {
                    assert(0 && "temporary global name too long!");
                }

                TB_DebugType* dbg_type = NULL;
                if (tu->has_tb_debug_info) {
                    dbg_type = cuik__as_tb_debug_type(tu->ir_mod, cuik_canonical_type(s->decl.type));
                }

                TB_Global* g = place_external(tu->parent, s, dbg_type, TB_LINKAGE_PRIVATE);
                tls_restore(name);

                TB_ModuleSection* section = get_variable_storage(tu->ir_mod, &attrs, s->decl.type.raw & CUIK_QUAL_CONST);

                int max_tb_objects = 0;
                if (s->decl.initial != NULL) {
                    Subexpr* initial = get_root_subexpr(s->decl.initial);

                    if (initial->op == EXPR_ADDR) {
                        max_tb_objects = 2;
                    } else if (initial->op == EXPR_INITIALIZER) {
                        max_tb_objects = count_max_tb_init_objects(initial->init.root);
                    } else {
                        max_tb_objects = 1;
                    }
                }

                tb_global_set_storage(tu->ir_mod, section, g, type->size, type->align, max_tb_objects);
                gen_global_initializer(tu, g, type, s->decl.initial, 0);

                if (attrs.is_tls) {
                    tb_module_set_tls_index(tu->ir_mod, -1, "_tls_index");
                }

                s->backing.g = g;
                break;
            }

            if (kind == KIND_FUNC) {
                break;
            }

            TB_Node* addr = tb_inst_local(func, size, align);
            if (tu->has_tb_debug_info && s->decl.name != NULL) {
                TB_Attrib* a = tb_function_attrib_variable(func, -1, s->decl.name, cuik__as_tb_debug_type(tu->ir_mod, type));
                tb_node_append_attrib(addr, a);
            }

            if (s->decl.initial) {
                Subexpr* e = get_root_subexpr(s->decl.initial);
                if (e->op == EXPR_INITIALIZER) {
                    gen_local_initializer(tu, func, addr, type, e->init.root);
                } else {
                    if (kind == KIND_ARRAY && (e->op == EXPR_STR || e->op == EXPR_WSTR)) {
                        IRVal v = irgen_expr(tu, func, s->decl.initial);
                        TB_Node* size_reg = tb_inst_uint(func, TB_TYPE_I64, size);

                        tb_inst_memcpy(func, addr, v.reg, size_reg, align, false);
                    } else if (kind == KIND_STRUCT || kind == KIND_UNION) {
                        IRVal v = irgen_expr(tu, func, s->decl.initial);
                        TB_Node* size_reg = tb_inst_uint(func, TB_TYPE_I64, size);

                        tb_inst_memcpy(func, addr, v.reg, size_reg, align, false);
                    } else {
                        TB_Node* v = irgen_as_rvalue(tu, func, s->decl.initial);

                        tb_inst_store(func, ctype_to_tbtype(type), addr, v, align, false);
                    }
                }
            } else {
                /* uninitialized */
            }

            s->backing.r = addr;
            break;
        }
        case STMT_EXPR: {
            irgen_expr(tu, func, s->expr.expr);
            break;
        }
        case STMT_RETURN: {
            if (s->return_.expr == NULL) {
                tb_inst_ret(func, 0, NULL);
                break;
            }

            IRVal v = irgen_expr(tu, func, s->return_.expr);

            Cuik_Type* type = cuik_canonical_type(get_root_cast(s->return_.expr));
            if (func_return_rule == TB_PASSING_INDIRECT) {
                // returning aggregates just copies into the first parameter
                // which is agreed to be a caller owned buffer.
                int size = type->size;
                int align = type->align;

                TB_Node* dst_address = tb_inst_param(func, 0);
                TB_Node* size_reg = tb_inst_uint(func, TB_TYPE_I64, size);

                tb_inst_memcpy(func, dst_address, v.reg, size_reg, align, false);
                tb_inst_ret(func, 1, &dst_address);
            } else {
                TB_Node* r = TB_NULL_REG;
                if (v.value_type == LVALUE) {
                    // Implicit array to pointer
                    if (type->kind == KIND_ARRAY) {
                        r = v.reg;
                    } else if (type->kind == KIND_STRUCT || type->kind == KIND_UNION) {
                        assert(type->size <= 8);
                        TB_DataType dt = { { TB_INT, 0, type->size * 8 } };

                        r = tb_inst_load(func, dt, v.reg, type->align, false);
                    }
                }

                // if it wasn't set before, resolve it now
                if (r == TB_NULL_REG) r = cvt2rval(tu, func, &v);

                tb_inst_ret(func, 1, &r);
            }
            break;
        }
        case STMT_IF: {
            IRVal cond = irgen_expr(tu, func, s->if_.cond);

            TB_Node *if_true, *if_false, *exit;
            if (cond.value_type == RVALUE_PHI) {
                exit = cond.phi.merger;
                if_true = cond.phi.if_true;
                if_false = cond.phi.if_false;
            } else {
                exit = tb_inst_region(func);
                if_true = tb_inst_region(func);
                if_false = tb_inst_region(func);

                // Cast to bool
                tb_inst_if(func, cvt2rval(tu, func, &cond), if_true, if_false);
            }

            tb_inst_set_control(func, if_true);
            irgen_stmt(tu, func, s->if_.body);

            if (s->if_.next) {
                if (tb_inst_get_control(func) != NULL) {
                    tb_inst_goto(func, exit);
                }

                tb_inst_set_control(func, if_false);
                irgen_stmt(tu, func, s->if_.next);
            } else {
                fallthrough_label(func, if_false);
            }
            fallthrough_label(func, exit);
            break;
        }
        case STMT_WHILE: {
            TB_Node* header = tb_inst_region(func);
            TB_Node* body = tb_inst_region(func);
            TB_Node* exit = tb_inst_region(func);

            s->backing.loop[0] = header;
            s->backing.loop[1] = exit;
            fallthrough_label(func, header);

            emit_location(tu, func, get_root_subexpr(s->while_.cond)->loc.start);
            TB_Node* cond = irgen_as_rvalue(tu, func, s->while_.cond);
            tb_inst_if(func, cond, body, exit);

            tb_inst_set_control(func, body);
            if (s->while_.body) {
                emit_location(tu, func, s->while_.body->loc.start);
                irgen_stmt(tu, func, s->while_.body);
            }

            fallthrough_label(func, header);
            tb_inst_set_control(func, exit);
            break;
        }
        case STMT_DO_WHILE: {
            TB_Node* latch = tb_inst_region(func);
            TB_Node* body = tb_inst_region(func);
            TB_Node* exit = tb_inst_region(func);

            s->backing.loop[0] = body;
            s->backing.loop[1] = exit;

            fallthrough_label(func, body);
            if (s->do_while.body) {
                emit_location(tu, func, s->do_while.body->loc.start);
                irgen_stmt(tu, func, s->do_while.body);
            }

            fallthrough_label(func, latch);
            TB_Node* cond = irgen_as_rvalue(tu, func, s->do_while.cond);
            tb_inst_if(func, cond, body, exit);
            tb_inst_set_control(func, exit);
            break;
        }
        case STMT_FOR: {
            TB_Node* header = tb_inst_region(func);
            TB_Node* body = tb_inst_region(func);
            TB_Node* next = tb_inst_region(func);
            TB_Node* exit = tb_inst_region(func);

            s->backing.loop[0] = next;
            s->backing.loop[1] = exit;

            if (s->for_.first) {
                emit_location(tu, func, s->for_.first->loc.start);
                irgen_stmt(tu, func, s->for_.first);
                tb_inst_goto(func, header);
            }
            fallthrough_label(func, header);

            if (s->for_.cond) {
                TB_Node* cond = irgen_as_rvalue(tu, func, s->for_.cond);
                tb_inst_if(func, cond, body, exit);
            } else {
                tb_inst_goto(func, body);
            }

            tb_inst_set_control(func, body);
            irgen_stmt(tu, func, s->for_.body);

            if (s->for_.next) {
                fallthrough_label(func, next);
                emit_location(tu, func, get_root_subexpr(s->for_.next)->loc.start);
                irgen_expr(tu, func, s->for_.next);
            } else {
                emit_location(tu, func, s->loc.start);
                fallthrough_label(func, next);
            }

            fallthrough_label(func, header);
            tb_inst_set_control(func, exit);
            break;
        }
        case STMT_CONTINUE: {
            tb_inst_goto(func, s->continue_.target->backing.loop[0]);
            break;
        }
        case STMT_BREAK: {
            tb_inst_goto(func, s->break_.target->backing.loop[1]);
            break;
        }
        case STMT_DEFAULT: {
            fallthrough_label(func, s->backing.r);
            irgen_stmt(tu, func, s->default_.body);
            break;
        }
        case STMT_CASE: {
            assert(s->backing.r);
            while (s->case_.body && s->case_.body->op == STMT_CASE) {
                fallthrough_label(func, s->backing.r);
                s = s->case_.body;
            }

            fallthrough_label(func, s->backing.r);
            irgen_stmt(tu, func, s->case_.body);
            break;
        }
        case STMT_SWITCH: {
            Stmt* head = s->switch_.next;

            size_t entry_count = 0;
            TB_SwitchEntry* entries = tls_save();

            TB_Node* default_label = 0;
            while (head) {
                // reserve label
                assert(head->op == STMT_CASE || head->op == STMT_DEFAULT);

                TB_Node* label = tb_inst_region(func);
                head->backing.r = label;

                if (head->op == STMT_CASE) {
                    assert(head->case_.key_max < UINT32_MAX);

                    intmax_t end = head->case_.key_max;
                    for (intmax_t i = head->case_.key; i <= end; i++) {
                        tls_push(sizeof(TB_SwitchEntry));
                        entries[entry_count++] = (TB_SwitchEntry){ .key = i, .value = label };
                    }
                } else if (head->op == STMT_DEFAULT) {
                    assert(default_label == 0);
                    default_label = label;
                }

                // default or case both fit
                head = head->case_.next;
            }

            TB_Node* break_label = tb_inst_region(func);

            s->backing.loop[0] = NULL;
            s->backing.loop[1] = break_label;

            // default to fallthrough
            if (!default_label) {
                default_label = break_label;
            }

            TB_Node* key = irgen_as_rvalue(tu, func, s->switch_.condition);

            tb_inst_branch(func, key->dt, key, default_label, entry_count, entries);

            tb_inst_set_control(func, tb_inst_region(func));
            irgen_stmt(tu, func, s->switch_.body);

            fallthrough_label(func, break_label);
            break;
        }

        default: TODO();
    }
}

TB_Symbol* cuikcg_top_level(TranslationUnit* restrict tu, TB_Module* m, TB_Arena* arena, Stmt* restrict s) {
    if (s->op == STMT_FUNC_DECL) {
        if ((s->decl.attrs.is_static || s->decl.attrs.is_inline) && !s->decl.attrs.is_used) {
            return NULL;
        }

        Cuik_Type* type = cuik_canonical_type(s->decl.type);
        assert(type->kind == KIND_FUNC);

        // Clear temporary storage
        tls_init();
        assert(type);

        TB_Function* func = s->backing.f;

        // we'll be using the debug info to construct our ABI compliant prototype
        TB_DebugType* dbg_type = cuik__as_tb_debug_type(m, type);
        TB_DebugType** dbg_params = tb_debug_func_params(dbg_type);

        size_t param_count;
        parameter_map = tb_function_set_prototype_from_dbg(func, dbg_type, arena, &param_count);

        if (cuik_canonical_type(type->func.return_type)->kind != KIND_VOID) {
            TB_DebugType* dbg_ret = tb_debug_func_returns(dbg_type)[0];
            func_return_rule = tb_get_passing_rule_from_dbg(tu->ir_mod, dbg_ret, true);
        } else {
            func_return_rule = TB_PASSING_DIRECT;
        }

        // compile body
        {
            function_type = type;
            function_name = s->decl.name;

            irgen_stmt(tu, func, s->decl.initial_as_stmt);

            function_name = NULL;
            function_type = 0;
        }

        // append return if none exists
        if (tb_inst_get_control(func) != NULL) {
            if (strcmp(s->decl.name, "main") == 0) {
                TB_Node* exit_status = tb_inst_uint(func, TB_TYPE_I32, 0);
                tb_inst_ret(func, 1, &exit_status);
            } else {
                tb_inst_ret(func, 0, NULL);
            }
        }

        //tb_inst_set_scope(func, old_tb_scope);
        return (TB_Symbol*) func;
    } else if (s->flags & STMT_FLAGS_HAS_IR_BACKING) {
        Cuik_Type* type = cuik_canonical_type(s->decl.type);
        Subexpr* initial = get_root_subexpr(s->decl.initial);

        TB_ModuleSection* section = get_variable_storage(tu->ir_mod, &s->decl.attrs, s->decl.type.raw & CUIK_QUAL_CONST);
        int max_tb_objects;
        if (initial == NULL) {
            tb_global_set_storage(tu->ir_mod, section, (TB_Global*) s->backing.s, type->size, type->align, 0);
            return s->backing.s;
        } else if (initial->op == EXPR_ADDR) {
            max_tb_objects = 2;
        } else if (initial->op == EXPR_INITIALIZER) {
            max_tb_objects = count_max_tb_init_objects(initial->init.root);
        } else {
            max_tb_objects = 1;
        }

        tb_global_set_storage(tu->ir_mod, section, (TB_Global*) s->backing.s, type->size, type->align, max_tb_objects);
        gen_global_initializer(tu, (TB_Global*) s->backing.s, type, s->decl.initial, 0);
        return s->backing.s;
    }

    return NULL;
}

TB_Module* cuik_get_tb_module(TranslationUnit* restrict tu) {
    return tu->ir_mod;
}

typedef struct {
    TB_Module* mod;
    TranslationUnit* tu;

    Stmt** stmts;
    size_t count;

    Futex* remaining;
} IRAllocTask;

static void ir_alloc_task(void* task) {
    CUIK_TIMED_BLOCK("ir_alloc_task") {
        IRAllocTask t = *(IRAllocTask*) task;

        for (size_t i = 0; i < t.count; i++) {
            Stmt* s = t.stmts[i];
            if ((s->flags & STMT_FLAGS_HAS_IR_BACKING) == 0) continue;

            if (s->op == STMT_FUNC_DECL) {
                TB_Linkage linkage = s->decl.attrs.is_static ? TB_LINKAGE_PRIVATE : TB_LINKAGE_PUBLIC;
                TB_ComdatType comdat = s->decl.attrs.is_inline ? TB_COMDAT_MATCH_ANY : TB_COMDAT_NONE;
                TB_Function* func = tb_function_create(t.tu->ir_mod, -1, s->decl.name, linkage, comdat);

                s->backing.f = func;
                s->backing.s->ordinal = get_ir_ordinal(t.tu, s);
            } else if (s->decl.attrs.is_used && !s->decl.attrs.is_typedef) {
                Cuik_Type* type = cuik_canonical_type(s->decl.type);
                bool is_external_sym = (type->kind == KIND_FUNC && s->decl.initial_as_stmt == NULL);
                if (s->decl.attrs.is_extern) is_external_sym = true;

                const char* name = s->decl.name;
                if (!is_external_sym) {
                    // if we have a TB module, fill it up with declarations
                    if (s->decl.attrs.is_tls) {
                        tb_module_set_tls_index(t.tu->ir_mod, -1, "_tls_index");
                    }

                    TB_Linkage linkage = s->decl.attrs.is_static ? TB_LINKAGE_PRIVATE : TB_LINKAGE_PUBLIC;
                    TB_DebugType* dbg_type = NULL;
                    if (t.tu->has_tb_debug_info) {
                        dbg_type = cuik__as_tb_debug_type(t.tu->ir_mod, cuik_canonical_type(s->decl.type));
                    }

                    s->backing.g = place_external(t.tu->parent, s, dbg_type, linkage);
                    s->backing.s->ordinal = get_ir_ordinal(t.tu, s);
                }
            }
        }

        if (t.remaining != NULL) {
            futex_dec(t.remaining);
        }
    }
}

void cuikcg_allocate_ir(TranslationUnit* restrict tu, Cuik_IThreadpool* restrict thread_pool, TB_Module* m) {
    // we actually fill the remaining count while we dispatch tasks, it's ok for it to hit 0
    // occasionally (very rare realistically).
    enum { BATCH_SIZE = 65536 };

    size_t count = dyn_array_length(tu->top_level_stmts);
    Futex remaining = (count + (BATCH_SIZE - 1)) / BATCH_SIZE;

    Stmt** top_level = tu->top_level_stmts;
    tu->ir_mod = m;

    for (size_t i = 0; i < count; i += BATCH_SIZE) {
        size_t end = i + BATCH_SIZE;
        if (end >= count) end = count;

        IRAllocTask t = {
            .mod = m,
            .tu = tu,
            .stmts = &top_level[i],
            .count = end - i,
            .remaining = &remaining,
        };

        if (thread_pool) {
            CUIK_CALL(thread_pool, submit, ir_alloc_task, sizeof(t), &t);
        } else {
            ir_alloc_task(&t);
        }
    }

    if (thread_pool) futex_wait_eq(&remaining, 0);
}

void cuikcg_allocate_ir2(TranslationUnit* tu, TB_Module* m) {
    size_t count = dyn_array_length(tu->top_level_stmts);
    tu->ir_mod = m;

    IRAllocTask t = {
        .mod = m,
        .tu = tu,
        .stmts = tu->top_level_stmts,
        .count = count,
    };
    ir_alloc_task(&t);
}
