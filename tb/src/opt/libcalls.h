
// this is a peephole lmao
static TB_Node* try_libcalls_fold(TB_Function* f, TB_OptQueue* restrict queue, TB_Node* n) {
    const TB_Symbol* sym = TB_NODE_GET_EXTRA_T(n->inputs[1], TB_NodeSymbol)->sym;

    // wacky? our memcpy shouldn't be allowed to lower into the builtin since
    // it might self reference?
    if (sym == &f->super) {
        return NULL;
    }

    char* fname = sym->name;
    if (strcmp(fname, "memcpy") == 0) {
        TB_Node* n2 = tb_alloc_node(f, TB_MEMCPY, TB_TYPE_VOID, 4, sizeof(TB_NodeMemAccess));
        set_input(queue, n2, n->inputs[0], 0); // control
        set_input(queue, n2, n->inputs[2], 1); // dst
        set_input(queue, n2, n->inputs[3], 2); // val
        set_input(queue, n2, n->inputs[4], 3); // size

        // memcpy returns the destination pointer, convert any non-control users
        // and pass them to dst
        TB_Node* dst_ptr = n->inputs[2];

        for (User* use = find_users(queue, n); use; use = use->next) {
            if (use->slot != 0 || (use->slot == 0 && !tb_uses_effects(use->n))) {
                tb_transmute_to_pass(queue, use->n, dst_ptr);
                tb_optqueue_mark(queue, use->n, true);
            }
        }

        return n2;
    } else {
        return NULL;
    }
}
