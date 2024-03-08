
static bool is_node_ready(TB_Passes* p, TB_BasicBlock* bb, Set* done, TB_Node* n) {
    FOREACH_N(i, 0, n->input_count) {
        TB_Node* in = n->inputs[i];
        if (in && p->scheduled[in->gvn] == bb && !set_get(done, in->gvn)) {
            return false;
        }
    }
    return true;
}

void list_scheduler(TB_Passes* p, TB_CFG* cfg, Worklist* ws, DynArray(PhiVal*) phi_vals, TB_BasicBlock* bb, TB_Node** id2node, TB_GetLatency get_lat) {
    assert(phi_vals == NULL && "TODO");

    TB_Function* f = p->f;
    TB_ArenaSavepoint sp = tb_arena_save(tmp_arena);

    TB_Node* end = bb->end;
    Set done = set_create_in_arena(tmp_arena, f->node_count);

    size_t sched_count = 0;
    TB_Node** sched = tb_arena_alloc(tmp_arena, bb->items.count * sizeof(TB_Node*));

    TB_OPTDEBUG(SCHEDULE)(printf("BB %d\n", bb->id));

    // first block has access to root's users
    if (bb->id == 0) {
        TB_Node* root = f->root_node;
        set_put(&done, root->gvn);
        FOR_USERS(u, root) { set_put(&done, u->n->gvn); }
    } else {
        set_put(&done, bb->start->gvn);
        FOR_USERS(u, bb->start) if (u->n->type == TB_PHI) {
            TB_OPTDEBUG(SCHEDULE)(printf("  DISPATCH: "), print_node_sexpr(u->n, 0), printf("\n"));
            sched[sched_count++] = u->n;
            set_put(&done, u->n->gvn);
        }
    }

    // initial items (everything used by the live-ins)
    nl_hashset_for(e, &bb->items) {
        TB_Node* n = *e;
        if (!set_get(&done, n->gvn) && p->scheduled[n->gvn] == bb && is_node_ready(p, bb, &done, n)) {
            TB_OPTDEBUG(SCHEDULE)(printf("  READY: "), print_node_sexpr(n, 0), printf("\n"));
            worklist_push(ws, n);
        }
    }

    while (dyn_array_length(ws->items) > cfg->block_count) {
        // find highest priority item
        TB_Node* best = NULL;
        int best_lat  = 0;
        FOREACH_N(i, cfg->block_count, dyn_array_length(ws->items)) {
            TB_Node* n = ws->items[i];
            if (!is_node_ready(p, bb, &done, n)) continue;
            int lat = get_lat(f, n);
            if (best_lat > lat) continue;
            best = ws->items[i];
            best_lat = lat;
            worklist_remove(ws, best);
            dyn_array_remove(ws->items, i);
            break;
        }

        TB_OPTDEBUG(SCHEDULE)(printf("  DISPATCH: "), print_node_sexpr(best, 0), printf("\n"));

        assert(best && best_lat > 0);
        sched[sched_count++] = best;
        set_put(&done, best->gvn);

        // make sure to place all projections directly after their tuple node
        if (best->dt.type == TB_TUPLE) {
            FOR_USERS(u, best) if (u->n->type == TB_PROJ) {
                assert(!set_get(&done, u->n->gvn));
                sched[sched_count++] = u->n;
                set_put(&done, u->n->gvn);
            }
        }

        // now that the op is retired, try to ready up users
        if (best != end) {
            FOR_USERS(u, best) {
                if (!set_get(&done, u->n->gvn) && p->scheduled[u->n->gvn] == bb && is_node_ready(p, bb, &done, u->n)) {
                    TB_OPTDEBUG(SCHEDULE)(printf("    READY: "), print_node_sexpr(u->n, 0), printf("\n"));
                    worklist_push(ws, u->n);
                }
            }
        }
    }

    dyn_array_set_length(ws->items, cfg->block_count + sched_count);
    memcpy(ws->items + cfg->block_count, sched, sched_count * sizeof(TB_Node*));

    tb_arena_restore(tmp_arena, sp);
}


