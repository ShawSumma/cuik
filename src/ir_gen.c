#include "ir_gen.h"

enum { LVALUE, RVALUE };

typedef struct IRVal {
	int value_type;
	TB_Register reg;
	TypeIndex type;
} IRVal;

TB_Module* mod;

static TB_DataType ctype_to_tbtype(const Type* t) {
	switch (t->kind) {
		case KIND_VOID: return TB_TYPE_VOID;
		case KIND_BOOL: return TB_TYPE_BOOL;
		case KIND_CHAR: return TB_TYPE_I8;
		case KIND_SHORT: return TB_TYPE_I16;
		case KIND_INT: return TB_TYPE_I32;
		case KIND_LONG: return TB_TYPE_I64;
		case KIND_FLOAT: return TB_TYPE_F32;
		case KIND_DOUBLE: return TB_TYPE_F64;
		case KIND_ENUM: return TB_TYPE_I32;
		case KIND_PTR: return TB_TYPE_PTR;
		case KIND_FUNC: return TB_TYPE_PTR;
		default: abort(); // TODO
	}
}

static void cvt_l2r(TB_Function* func, IRVal* restrict v, TypeIndex dst_type) {
	if (v->value_type == LVALUE) {
		v->value_type = RVALUE;
		
		Type* src = &type_arena.data[v->type];
		v->reg = tb_inst_load(func, ctype_to_tbtype(src), v->reg, src->align);
		
		if (v->type != dst_type) {
			Type* dst = &type_arena.data[dst_type];
			
			if (src->kind >= KIND_CHAR &&
				src->kind <= KIND_LONG &&
				dst->kind >= KIND_CHAR && 
				dst->kind <= KIND_LONG) {
				// if it's an integer, handle some implicit casts
				if (dst->kind > src->kind) {
					// up-casts
					if (dst->is_unsigned) v->reg = tb_inst_zxt(func, v->reg, ctype_to_tbtype(dst));
					else v->reg = tb_inst_sxt(func, v->reg, ctype_to_tbtype(dst));
				} else if (dst->kind < src->kind) {
					// down-casts
					v->reg = tb_inst_trunc(func, v->reg, ctype_to_tbtype(dst));
				}
			}
		}
	}
}

static IRVal gen_expr(TB_Function* func, ExprIndex e) {
	TypeIndex type_index = expr_arena.data[e].type;
	Type* restrict type = &type_arena.data[type_index];
	
	switch (expr_arena.data[e].op) {
		case EXPR_NUM: {
			return (IRVal) {
				.value_type = RVALUE,
				.type = type_index,
				.reg = tb_inst_iconst(func, ctype_to_tbtype(type), expr_arena.data[e].num)
			};
		}
		case EXPR_VAR: {
			StmtIndex stmt = expr_arena.data[e].var;
			
			return (IRVal) {
				.value_type = LVALUE,
				.type = type_index,
				.reg = stmt_arena.data[stmt].backing.r
			};
		}
		case EXPR_ADDR: {
			IRVal src = gen_expr(func, expr_arena.data[e].unary_op.src);
			assert(src.value_type == LVALUE);
			src.value_type = RVALUE;
			
			return src;
		}
		case EXPR_DEREF: {
			IRVal src = gen_expr(func, expr_arena.data[e].unary_op.src);
			cvt_l2r(func, &src, type_index);
			
			assert(type_arena.data[src.type].kind == KIND_PTR);
			
			return (IRVal) {
				.value_type = LVALUE,
				.type = type_index,
				.reg = src.reg
			};
		}
		case EXPR_SUBSCRIPT: {
			IRVal base = gen_expr(func, expr_arena.data[e].subscript.base);
			IRVal index = gen_expr(func, expr_arena.data[e].subscript.index);
			
			assert(base.value_type == LVALUE);
			
			// the index type is always long... or int maybe
			// if we support 32bit platforms at some point
			cvt_l2r(func, &index, TYPE_ULONG);
			
			int stride = type->size;
			return (IRVal) {
				.value_type = LVALUE,
				.type = type_index,
				.reg = tb_inst_array_access(func, base.reg, index.reg, stride)
			};
		}
		case EXPR_POST_INC:
		case EXPR_POST_DEC: {
			bool is_inc = (expr_arena.data[e].op == EXPR_POST_INC);
			
			IRVal src = gen_expr(func, expr_arena.data[e].unary_op.src);
			assert(src.value_type == LVALUE);
			
			// TODO(NeGate): Implement pointer arithmatic
			assert(type->kind != KIND_PTR);
			
			IRVal loaded = src;
			cvt_l2r(func, &loaded, type_index);
			
			// increment
			TB_DataType dt = ctype_to_tbtype(type);
			TB_ArithmaticBehavior ab = type->is_unsigned ? TB_CAN_WRAP : TB_ASSUME_NSW;
			
			TB_Register one = tb_inst_iconst(func, dt, 1);
			
			TB_Register operation;
			if (is_inc) operation = tb_inst_add(func, dt, loaded.reg, one, ab);
			else operation = tb_inst_sub(func, dt, loaded.reg, one, ab);
			
			tb_inst_store(func, dt, src.reg, operation, type->align);
			
			return (IRVal) {
				.value_type = RVALUE,
				.type = type_index,
				.reg = loaded.reg
			};
		}
		case EXPR_PLUS:
		case EXPR_MINUS:
		case EXPR_TIMES:
		case EXPR_SLASH:
		case EXPR_AND:
		case EXPR_OR:
		case EXPR_XOR:
		case EXPR_SHL:
		case EXPR_SHR: {
			// TODO(NeGate): Implement pointer arithmatic for PLUS and MINUS.
			IRVal l = gen_expr(func, expr_arena.data[e].bin_op.left);
			IRVal r = gen_expr(func, expr_arena.data[e].bin_op.right);
			cvt_l2r(func, &l, type_index);
			cvt_l2r(func, &r, type_index);
			
			TB_DataType dt = ctype_to_tbtype(type);
			TB_Register data;
			if (type->kind == KIND_FLOAT || type->kind == KIND_DOUBLE) {
				switch (expr_arena.data[e].op) {
					case EXPR_PLUS: data = tb_inst_fadd(func, dt, l.reg, r.reg); break;
					case EXPR_MINUS: data = tb_inst_fsub(func, dt, l.reg, r.reg); break;
					case EXPR_TIMES: data = tb_inst_fmul(func, dt, l.reg, r.reg); break;
					case EXPR_SLASH: data = tb_inst_fdiv(func, dt, l.reg, r.reg); break;
					default: abort();
				}
			} else {
				TB_ArithmaticBehavior ab = type->is_unsigned ? TB_CAN_WRAP : TB_ASSUME_NSW;
				
				switch (expr_arena.data[e].op) {
					case EXPR_PLUS: data = tb_inst_add(func, dt, l.reg, r.reg, ab); break;
					case EXPR_MINUS: data = tb_inst_sub(func, dt, l.reg, r.reg, ab); break;
					case EXPR_TIMES: data = tb_inst_mul(func, dt, l.reg, r.reg, ab); break;
					case EXPR_SLASH: data = tb_inst_div(func, dt, l.reg, r.reg, !type->is_unsigned); break;
					case EXPR_AND: data = tb_inst_and(func, dt, l.reg, r.reg); break;
					case EXPR_OR: data = tb_inst_or(func, dt, l.reg, r.reg); break;
					case EXPR_XOR: data = tb_inst_xor(func, dt, l.reg, r.reg); break;
					case EXPR_SHL: data = tb_inst_shl(func, dt, l.reg, r.reg, ab); break;
					case EXPR_SHR: data = type->is_unsigned ? tb_inst_shr(func, dt, l.reg, r.reg) : tb_inst_sar(func, dt, l.reg, r.reg); break;
					default: abort();
				}
			}
			
			return (IRVal) {
				.value_type = RVALUE,
				.type = type_index,
				.reg = data
			};
		}
		case EXPR_ASSIGN:
		case EXPR_PLUS_ASSIGN:
		case EXPR_MINUS_ASSIGN:
		case EXPR_TIMES_ASSIGN:
		case EXPR_SLASH_ASSIGN:
		case EXPR_AND_ASSIGN:
		case EXPR_OR_ASSIGN:
		case EXPR_XOR_ASSIGN:
		case EXPR_SHL_ASSIGN:
		case EXPR_SHR_ASSIGN: {
			IRVal l = gen_expr(func, expr_arena.data[e].bin_op.left);
			IRVal r = gen_expr(func, expr_arena.data[e].bin_op.right);
			cvt_l2r(func, &r, type_index);
			
			assert(l.value_type == LVALUE);
			
			IRVal ld_l;
			if (expr_arena.data[e].op != EXPR_ASSIGN) {
				// don't do this conversion for ASSIGN, since it won't
				// be needing it
				ld_l = l;
				cvt_l2r(func, &ld_l, type_index);
			}
			
			TB_Register data;
			TB_DataType dt = ctype_to_tbtype(type);
			
			if (type->kind == KIND_FLOAT || type->kind == KIND_DOUBLE) {
				switch (expr_arena.data[e].op) {
					case EXPR_ASSIGN: data = r.reg; break;
					case EXPR_PLUS_ASSIGN: data = tb_inst_fadd(func, dt, ld_l.reg, r.reg); break;
					case EXPR_MINUS_ASSIGN: data = tb_inst_fsub(func, dt, ld_l.reg, r.reg); break;
					case EXPR_TIMES_ASSIGN: data = tb_inst_fmul(func, dt, ld_l.reg, r.reg); break;
					case EXPR_SLASH_ASSIGN: data = tb_inst_fdiv(func, dt, ld_l.reg, r.reg); break;
					default: abort();
				}
			} else {
				TB_ArithmaticBehavior ab = type->is_unsigned ? TB_CAN_WRAP : TB_ASSUME_NSW;
				
				switch (expr_arena.data[e].op) {
					case EXPR_ASSIGN: data = r.reg; break;
					case EXPR_PLUS_ASSIGN: data = tb_inst_add(func, dt, ld_l.reg, r.reg, ab); break;
					case EXPR_MINUS_ASSIGN: data = tb_inst_sub(func, dt, ld_l.reg, r.reg, ab); break;
					case EXPR_TIMES_ASSIGN: data = tb_inst_mul(func, dt, ld_l.reg, r.reg, ab); break;
					case EXPR_SLASH_ASSIGN: data = tb_inst_div(func, dt, ld_l.reg, r.reg, !type->is_unsigned); break;
					case EXPR_AND_ASSIGN: data = tb_inst_and(func, dt, ld_l.reg, r.reg); break;
					case EXPR_OR_ASSIGN: data = tb_inst_or(func, dt, ld_l.reg, r.reg); break;
					case EXPR_XOR_ASSIGN: data = tb_inst_xor(func, dt, ld_l.reg, r.reg); break;
					case EXPR_SHL_ASSIGN: data = tb_inst_shl(func, dt, ld_l.reg, r.reg, ab); break;
					case EXPR_SHR_ASSIGN: data = type->is_unsigned ? tb_inst_shr(func, dt, ld_l.reg, r.reg) : tb_inst_sar(func, dt, ld_l.reg, r.reg); break;
					default: abort();
				}
			}
			
			tb_inst_store(func, dt, l.reg, data, type->align);
			return l;
		}
		default: abort();
	}
}

static void gen_stmt(TB_Function* func, StmtIndex s) {
	switch (stmt_arena.data[s].op) {
		case STMT_NONE: {
			break;
		}
		case STMT_COMPOUND: {
			StmtIndexIndex start = stmt_arena.data[s].kids_start;
			StmtIndexIndex end = stmt_arena.data[s].kids_end;
			
			for (StmtIndexIndex i = start; i != end; i++) {
				StmtIndex s = stmt_ref_arena.data[i];
				
				gen_stmt(func, s);
			}
			break;
		}
		case STMT_DECL: {
			//Attribs attrs = stmt_arena.data[s].attrs;
			TypeIndex type_index = stmt_arena.data[s].decl_type;
			Type* restrict type = &type_arena.data[type_index];
			
			TB_Register addr = tb_inst_local(func, type->size, type->align);
			stmt_arena.data[s].backing.r = addr;
			
			if (stmt_arena.data[s].expr) {
				IRVal v = gen_expr(func, stmt_arena.data[s].expr);
				
				if (type->kind == KIND_STRUCT || type->kind == KIND_UNION) {
					TB_Register size_reg = tb_inst_iconst(func, TB_TYPE_I32, type->size);
					
					tb_inst_memcpy(func, addr, v.reg, size_reg, type->align);
				} else {
					cvt_l2r(func, &v, type_index);
					tb_inst_store(func, ctype_to_tbtype(type), addr, v.reg, type->align);
				}
			}
			break;
		}
		case STMT_EXPR: {
			gen_expr(func, stmt_arena.data[s].expr);
			break;
		}
		case STMT_RETURN: {
			ExprIndex e = stmt_arena.data[s].expr;
			
			TypeIndex type_index = expr_arena.data[e].type;
			Type* restrict type = &type_arena.data[type_index];
			
			IRVal v = gen_expr(func, e);
			cvt_l2r(func, &v, type_index);
			
			tb_inst_ret(func, ctype_to_tbtype(type), v.reg);
			break;
		}
		case STMT_IF: {
			TB_Label if_true = tb_inst_new_label_id(func);
			TB_Label if_false = tb_inst_new_label_id(func);
			
			IRVal cond = gen_expr(func, stmt_arena.data[s].expr);
			cvt_l2r(func, &cond, TYPE_BOOL);
			
			tb_inst_if(func, cond.reg, if_true, if_false);
			tb_inst_label(func, if_true);
			gen_stmt(func, stmt_arena.data[s].body);
			
			if (stmt_arena.data[s].body2) {
				TB_Label exit = tb_inst_new_label_id(func);
				tb_inst_goto(func, exit);
				
				tb_inst_label(func, if_false);
				gen_stmt(func, stmt_arena.data[s].body2);
				
				// fallthrough
				tb_inst_label(func, exit);
			} else {
				tb_inst_label(func, if_false);
			}
			break;
		}
		case STMT_WHILE: {
			TB_Label header = tb_inst_new_label_id(func);
			TB_Label body = tb_inst_new_label_id(func);
			TB_Label exit = tb_inst_new_label_id(func);
			
			tb_inst_label(func, header);
			
			IRVal cond = gen_expr(func, stmt_arena.data[s].expr);
			cvt_l2r(func, &cond, TYPE_BOOL);
			
			tb_inst_if(func, cond.reg, body, exit);
			
			tb_inst_label(func, body);
			gen_stmt(func, stmt_arena.data[s].body);
			
			tb_inst_goto(func, header);
			tb_inst_label(func, exit);
			break;
		}
		case STMT_DO_WHILE: {
			TB_Label body = tb_inst_new_label_id(func);
			TB_Label exit = tb_inst_new_label_id(func);
			
			tb_inst_label(func, body);
			
			gen_stmt(func, stmt_arena.data[s].body);
			
			IRVal cond = gen_expr(func, stmt_arena.data[s].expr);
			cvt_l2r(func, &cond, TYPE_BOOL);
			tb_inst_if(func, cond.reg, body, exit);
			
			tb_inst_label(func, exit);
			break;
		}
		case STMT_FOR: {
			
			break;
		}
		case STMT_FOR2:
		__builtin_unreachable();
	}
}

static void gen_func(StmtIndex s) {
	TB_Function* func = tb_function_create(mod, stmt_arena.data[s].decl_name, TB_TYPE_I64);
	stmt_arena.data[s].backing.f = func;
	
	gen_stmt(func, stmt_arena.data[s].body);
	
	tb_function_print(func, stdout);
	tb_module_compile_func(mod, func);
}

void gen_ir(TopLevel tl) {
	for (StmtIndexIndex i = tl.start; i != tl.end; i++) {
		StmtIndex s = stmt_ref_arena.data[i];
		
		if (stmt_arena.data[s].op == STMT_DECL) {
			TypeIndex type = stmt_arena.data[s].decl_type;
			
			if (type_arena.data[type].kind == KIND_FUNC) gen_func(s);
		}
	}
}
