// This is gonna get complicated but we can push through :p


////////////////////////////////
// x86-64
////////////////////////////////
// Our two ABIs are System-V and Win64, important to note that
// returns go through 0=RAX, 1=RDX so references to these in terms
// of returns will mean that.

// we finna retrofit systemv terms onto
// windows, it's not too big of a deal.
typedef enum {
    RG_NONE,
    // GPRs
    RG_INTEGER,
    // vector registers
    RG_SSE, RG_SSEUP,
    // stack slot
    RG_MEMORY,
} RegClass;

static int debug_type_size(TB_ABI abi, TB_DebugType* t) {
    switch (t->tag) {
        case TB_DEBUG_TYPE_VOID: return 0;
        case TB_DEBUG_TYPE_BOOL: return 1;
        case TB_DEBUG_TYPE_UINT: return (t->int_bits + 7) / 8;
        case TB_DEBUG_TYPE_INT:  return (t->int_bits + 7) / 8;

        case TB_DEBUG_TYPE_FUNCTION: return 8;
        case TB_DEBUG_TYPE_ARRAY:    return 8;
        case TB_DEBUG_TYPE_POINTER:  return 8;

        case TB_DEBUG_TYPE_FLOAT: {
            switch (t->float_fmt) {
                case TB_FLT_32: return 4;
                case TB_FLT_64: return 8;
            }
        }

        case TB_DEBUG_TYPE_STRUCT:
        case TB_DEBUG_TYPE_UNION:
        return t->record.size;

        default: tb_todo();
    }
    return 0;
}

static int debug_type_align(TB_ABI abi, TB_DebugType* t) {
    if (t->tag == TB_DEBUG_TYPE_STRUCT || t->tag == TB_DEBUG_TYPE_UNION) {
        return t->record.align;
    }

    return debug_type_size(abi, t);
}

static RegClass classify_reg(TB_ABI abi, TB_DebugType* t) {
    switch (abi) {
        // [https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention]
        // A scalar return value that can fit into 64
        // bits, including the __m64 type, is returned
        // through RAX.
        case TB_ABI_WIN64: {
            if (debug_type_size(abi, t) > 8) {
                return RG_MEMORY;
            }

            return t->tag == TB_DEBUG_TYPE_FLOAT ? RG_SSE : RG_INTEGER;
        }

        default: tb_todo();
    }
}

static TB_DataType debug_type_to_tb(TB_DebugType* t) {
    switch (t->tag) {
        case TB_DEBUG_TYPE_VOID: return TB_TYPE_VOID;
        case TB_DEBUG_TYPE_BOOL: return TB_TYPE_I8;
        case TB_DEBUG_TYPE_UINT: return TB_TYPE_INTN(t->int_bits);
        case TB_DEBUG_TYPE_INT:  return TB_TYPE_INTN(t->int_bits);

        case TB_DEBUG_TYPE_FUNCTION: return TB_TYPE_PTR;
        case TB_DEBUG_TYPE_ARRAY:    return TB_TYPE_PTR;
        case TB_DEBUG_TYPE_POINTER:  return TB_TYPE_PTR;

        case TB_DEBUG_TYPE_FLOAT: return (TB_DataType){ { TB_FLOAT, 0, t->float_fmt } };

        default: tb_assert(0, "todo"); return TB_TYPE_VOID;
    }
}

TB_Node** tb_function_set_prototype_from_dbg(TB_Function* f, TB_DebugType* dbg, TB_Arena* arena, size_t* out_param_count) {
    tb_assert(dbg->tag == TB_DEBUG_TYPE_FUNCTION, "type has to be a function");
    tb_assert(dbg->func.return_count <= 1, "C can't do multiple returns and thus we can't lower it into C from here, try tb_function_set_prototype and do it manually");

    f->arena = arena;
    TB_ABI abi = TB_ABI_WIN64;

    // aggregate return means the first parameter will be used for
    // a pointer to where the output should be written.
    //
    // TODO(NeGate): it's uninitialized by default but we don't communicate
    // this to the IR yet.
    RegClass is_aggregate_return = RG_NONE;
    TB_PrototypeParam ret = { TB_TYPE_PTR };
    if (dbg->func.return_count == 1) {
        is_aggregate_return = classify_reg(abi, dbg->func.returns[0]);

        ret.dt = debug_type_to_tb(dbg->func.returns[0]);
        ret.debug_type = dbg->func.returns[0];
    }

    // estimate the number of parameters:
    // * in win64 this is easy, parameters don't split.
    // * in sysv this is a nightmare, structs are usually
    // the culprit because they can be split up.
    size_t param_count = dbg->func.param_count;
    TB_DebugType** param_list = dbg->func.params;
    if (abi == TB_ABI_SYSTEMV) {
        tb_todo();
    }

    // build up prototype param types
    size_t return_count = dbg->func.return_count;
    size_t size = sizeof(TB_FunctionPrototype) + ((param_count + return_count) * sizeof(TB_PrototypeParam));
    TB_FunctionPrototype* p = arena_alloc(&tb__arena2, size, _Alignof(TB_FunctionPrototype));
    if (dbg->func.param_count > 0) {
        p->call_conv = dbg->func.cc;
        p->has_varargs = dbg->func.has_varargs;
        p->return_count = return_count;
        p->param_count = param_count;

        FOREACH_N(i, 0, dbg->func.param_count) {
            TB_DebugType* type = param_list[i]->field.type;
            RegClass rg = classify_reg(abi, type);

            TB_PrototypeParam param = {
                .name = param_list[i]->field.name,
                .debug_type = type,
                .dt = rg == RG_MEMORY ? TB_TYPE_PTR : debug_type_to_tb(type),
            };

            p->params[i] = param;
        }

        if (p->return_count == 1) {
            p->params[p->param_count] = ret;
        }
    }
    tb_function_set_prototype(f, p, arena);

    // reassemble values
    TB_Node** params = NULL;
    if (dbg->func.param_count > 0) {
        params = f->arena->alloc(f->arena, sizeof(TB_Node*) * param_count, _Alignof(TB_Node*));

        FOREACH_N(i, 0, param_count) {
            TB_DebugType* type = param_list[i]->field.type;
            const char* name = param_list[i]->field.name;

            int size = debug_type_size(abi, type);
            int align = debug_type_align(abi, type);

            // place values into memory
            TB_Node* v = tb_inst_param(f, i);

            RegClass rg = classify_reg(abi, param_list[i]->field.type);
            if (rg == RG_MEMORY) {
                params[i] = v;
            } else {
                TB_Node* slot = tb_inst_local(f, size, align);
                tb_inst_store(f, p->params[i].dt, slot, v, align, false);
                params[i] = slot;
            }

            // mark debug info
            tb_node_append_attrib(params[i], tb_function_attrib_variable(f, -1, name, type));
        }
    }

    *out_param_count = param_count;
    return params;
}
