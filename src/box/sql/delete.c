/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "box/box.h"
#include "box/schema.h"
#include "sqlInt.h"
#include "resolve.h"
#include "tarantoolInt.h"

void
sql_materialize_select(struct Parse *parse, struct Select *select, int cursor)
{
	struct SelectDest dest;
	sqlSelectDestInit(&dest, SRT_EphemTab, cursor, ++parse->nMem);
	sqlSelect(parse, select, &dest);
	sql_select_delete(select);
}

void
sql_table_truncate(struct Parse *parse, const struct space *space)
{
	struct Vdbe *v = sqlGetVdbe(parse);
	sqlVdbeAddOp2(v, OP_Clear, space->def->id, true);
}

/**
 * Generate the code deleting the rows of the given space, which is the only
 * source of the given list with the cursors assigned, matching the given WHERE
 * clause, which is resolved, firing the given triggers.
 */
static void
vdbe_emit_delete(struct Parse *parse, struct SrcList *tab_list,
		 struct space *space, struct Expr *where,
		 struct sql_trigger *trigger_list);

void
sql_emit_delete(struct Parse *parse, struct rast_delete *del)
{
	struct rast_source *target = &del->target.sources[0];
	struct space *space = space_by_id(target->space_id);
	struct sql_trigger *trigger_list =
		sql_triggers_exist(space->def, TK_DELETE, NULL,
				   parse->sql_flags, NULL);
	bool is_view = space->def->opts.is_view;
	target->cursor = parse->nTab++;
	parse->nTab += space->index_count;
	struct SrcList *tab_list = src_list_from_rast_target(parse,
							     &del->target);
	struct Vdbe *v = sqlGetVdbe(parse);
	sqlVdbeCountChanges(v);
	if (is_view) {
		struct Select *select = select_from_rast(parse,
							 del->view_select);
		if (parse->is_aborted) {
			sqlSrcListDelete(tab_list);
			return;
		}
		sql_materialize_select(parse, select, target->cursor);
	}
	struct Expr *where = NULL;
	if (del->where != NULL)
		where = expr_from_rast_in(parse, tab_list, del->where);
	if (!parse->is_aborted)
		vdbe_emit_delete(parse, tab_list, space, where, trigger_list);
	sqlSrcListDelete(tab_list);
	sql_expr_delete(where);
}

/**
 * Check if the given expression contains a correlated subquery outside of its
 * subqueries.
 */
static bool
expr_has_var_select(struct Expr *expr)
{
	if (expr == NULL)
		return false;
	if (ExprHasProperty(expr, EP_VarSelect))
		return true;
	if (ExprHasProperty(expr, EP_TokenOnly | EP_Leaf))
		return false;
	if (expr_has_var_select(expr->pLeft) ||
	    expr_has_var_select(expr->pRight))
		return true;
	if (ExprHasProperty(expr, EP_xIsSelect) || expr->x.pList == NULL)
		return false;
	for (int i = 0; i < expr->x.pList->nExpr; ++i) {
		if (expr_has_var_select(expr->x.pList->a[i].pExpr))
			return true;
	}
	return false;
}

static void
vdbe_emit_delete(struct Parse *parse, struct SrcList *tab_list,
		 struct space *space, struct Expr *where,
		 struct sql_trigger *trigger_list)
{
	struct Vdbe *v = parse->pVdbe;
	int tab_cursor = tab_list->a[0].iCursor;
	bool is_view = space->def->opts.is_view;
	/*
	 * True if there are triggers or correlated subqueries in the WHERE
	 * clause.
	 */
	bool is_complex = trigger_list != NULL || expr_has_var_select(where);

	/* Special case: A DELETE without a WHERE clause deletes
	 * everything. It is easier just to erase the whole table.
	 */
	if (where == NULL && !is_complex) {
		assert(!is_view);

		sqlVdbeAddOp1(v, OP_Clear, space->def->id);
		sqlVdbeChangeP5(v, OPFLAG_NCHANGE);

		/* Do not start Tarantool's transaction in case of
		 * truncate optimization. This is workaround until
		 * system tables cannot be changes inside a
		 * transaction (_truncate).
		 */
		parse->initiateTTrans = false;
	} else {
		uint16_t wcf = WHERE_ONEPASS_DESIRED | WHERE_DUPLICATES_OK |
			WHERE_SEEK_TABLE;
		wcf |= (is_complex ? 0 : WHERE_ONEPASS_MULTIROW);
		/* Create an ephemeral table used to hold all
		 * primary keys for rows to be deleted. Since VIEW
		 * is held in ephemeral table, there is no PK for
		 * it, so columns should be loaded manually.
		 */
		int reg_eph = ++parse->nMem;
		int reg_pk = parse->nMem + 1;
		int pk_len = is_view ? space->def->field_count + 1 :
			     space->index[0]->def->key_def->part_count;
		/*
		 * This increase should not be necessary, but when filling
		 * the tab_cursor cursors, an error occurs that causes
		 * one more cursor to be filled than expected.
		 */
		parse->nTab++;
		int eph_cursor = parse->nTab++;
		struct sql_space_info *info;
		if (is_view) {
			/*
			 * At this stage SELECT is already materialized
			 * into ephemeral table, which has one additional
			 * tail field (except for ones specified in view
			 * format) which contains sequential ids. These ids
			 * are required since selected values may turn out to
			 * be non-unique. For instance:
			 * CREATE TABLE t (id INT PRIMARY KEY, a INT);
			 * INSERT INTO t VALUES (1, 1), (2, 1), (3, 1);
			 * CREATE VIEW v AS SELECT a FROM t;
			 * Meanwhile ephemeral tables feature PK covering ALL
			 * fields of format, so without id materialization
			 * processing is impossible. Then, results of
			 * materialization are transferred to the table being
			 * created below. So to fit tuples in it we must
			 * account that id field as well. That's why pk_len
			 * has one field more than view format.
			 */
			info = sql_space_info_new_from_space_def(space->def);
		} else {
                        assert(space->index_count > 0);
			struct index_def *index_def = space->index[0]->def;
			info = sql_space_info_new_from_index_def(index_def,
								 false);
		}
		parse->nMem += pk_len;
		int addr_eph_open = sqlVdbeAddOp4(v, OP_OpenTEphemeral, reg_eph,
						  0, 0, (char *)info,
						  P4_DYNAMIC);
		sqlVdbeAddOp3(v, OP_IteratorOpen, eph_cursor, 0, reg_eph);

		/* Construct a query to find the primary key for
		 * every row to be deleted, based on the WHERE
		 * clause. Set variable one_pass to indicate the
		 * strategy used to implement this delete:
		 *
		 * ONEPASS_OFF:    Two-pass approach - use a FIFO
		 * for PK values.
		 * ONEPASS_SINGLE: One-pass approach - at most one
		 * row deleted.
		 * ONEPASS_MULTI:  One-pass approach - any number
		 * of rows may be deleted.
		 */
		struct WhereInfo *winfo =
		    sqlWhereBegin(parse, tab_list, where, NULL, NULL, wcf,
				      tab_cursor + 1);
		if (winfo == NULL)
			return;

		/* The write cursors opened by WHERE_ONEPASS */
		int one_pass_cur[2];
		int one_pass = sqlWhereOkOnePass(winfo, one_pass_cur);
		assert(one_pass != ONEPASS_MULTI);
		/* Tarantool: see comment in
		 * sqlWhereOkOnePass.
		 */
		/* assert(is_complex || one_pass != ONEPASS_OFF); */

		/* Extract the primary key for the current row */
		if (!is_view) {
			struct key_def *def = space->index[0]->def->key_def;
			for (int i = 0; i < pk_len; i++) {
				struct key_part *part = &def->parts[i];
				sqlVdbeAddOp3(v, OP_Column, tab_cursor,
					      part->fieldno, reg_pk + i);
			}
		} else {
			for (int i = 0; i < pk_len; i++) {
				sqlVdbeAddOp3(v, OP_Column, tab_cursor,
						  i, reg_pk + i);
			}
		}

		int reg_key;
		int key_len;
		if (one_pass != ONEPASS_OFF) {
			/* For ONEPASS, no need to store the
			 * primary-key. There is only one, so just
			 * keep it in its register(s) and fall
			 * through to the delete code.
			 */
			reg_key = reg_pk;
			/* OP_Found will use an unpacked key */
			key_len = pk_len;
			sqlVdbeChangeToNoop(v, addr_eph_open);
			sqlVdbeChangeToNoop(v, addr_eph_open + 1);
		} else {
			/* Add the PK key for this row to the
			 * temporary table.
			 */
			reg_key = ++parse->nMem;
			/* Zero tells OP_Found to use a composite
			 * key.
			 */
			key_len = 0;
			sqlVdbeAddOp3(v, OP_MakeRecord, reg_pk, pk_len,
				      reg_key);
			/* Set flag to save memory allocating one
			 * by malloc.
			 */
			sqlVdbeChangeP5(v, 1);
			sqlVdbeAddOp2(v, OP_IdxInsert, reg_key, reg_eph);
		}

		/* If this DELETE cannot use the ONEPASS strategy,
		 * this is the end of the WHERE loop.
		 */
		int addr_bypass = 0;
		if (one_pass != ONEPASS_OFF)
			addr_bypass = sqlVdbeMakeLabel(v);
		else
			sqlWhereEnd(winfo);

		/* Unless this is a view, open cursors for the
		 * table we are deleting from and all its indices.
		 * If this is a view, then the only effect this
		 * statement has is to fire the INSTEAD OF
		 * triggers.
		 */
		if (!is_view) {
			int iAddrOnce = 0;
			if (one_pass == ONEPASS_MULTI)
				iAddrOnce = sqlVdbeAddOp0(v, OP_Once);
			int reg = ++parse->nMem;
			sqlVdbeAddOp2(v, OP_OpenSpace, reg, space->def->id);
			sqlVdbeAddOp3(v, OP_IteratorOpen, tab_cursor, 0, reg);
			VdbeComment((v, "%s", space->index[0]->def->name));

			if (one_pass == ONEPASS_MULTI)
				sqlVdbeJumpHere(v, iAddrOnce);
		}

		/* Set up a loop over the primary-keys that were
		 * found in the where-clause loop above.
		 */
		int addr_loop = 0;
		if (one_pass != ONEPASS_OFF) {
			/* OP_Found will use an unpacked key. */
			assert(key_len == pk_len);
			sqlVdbeAddOp4Int(v, OP_NotFound, tab_cursor,
					     addr_bypass, reg_key, key_len);
		} else {
			addr_loop = sqlVdbeAddOp1(v, OP_Rewind, eph_cursor);
			sqlVdbeAddOp2(v, OP_RowData, eph_cursor, reg_key);
		}

		/* Delete the row */
		int idx_noseek = -1;
		if (!is_complex && one_pass_cur[1] != tab_cursor
		    /* Tarantool: as far as ONEPASS is disabled,
		     * there's no index w/o need of seeking.
		     */
		    && one_pass != ONEPASS_OFF)
			idx_noseek = one_pass_cur[1];

		sql_generate_row_delete(parse, space, trigger_list, tab_cursor,
					reg_key, key_len, true,
					ON_CONFLICT_ACTION_DEFAULT, one_pass,
					idx_noseek);

		/* End of the loop over all primary-keys. */
		if (one_pass != ONEPASS_OFF) {
			sqlVdbeResolveLabel(v, addr_bypass);
			sqlWhereEnd(winfo);
		} else {
			sqlVdbeAddOp2(v, OP_Next, eph_cursor,
					  addr_loop + 1);
			sqlVdbeJumpHere(v, addr_loop);
		}
	}
}

void
sql_generate_row_delete(struct Parse *parse, struct space *space,
			struct sql_trigger *trigger_list, int cursor,
			int reg_pk, short npk, bool need_update_count,
			enum on_conflict_action onconf, u8 mode,
			int idx_noseek)
{
	struct Vdbe *v = parse->pVdbe;
	/* Vdbe is guaranteed to have been allocated by this
	 * stage.
	 */
	assert(v != NULL);

	/* Seek cursor iCur to the row to delete. If this row no
	 * longer exists (this can happen if a trigger program has
	 * already deleted it), do not attempt to delete it or
	 * fire any DELETE triggers.
	 */
	int label = sqlVdbeMakeLabel(v);
	if (mode == ONEPASS_OFF)
		sqlVdbeAddOp4Int(v, OP_NotFound, cursor, label, reg_pk, npk);

	int first_old_reg = 0;
	/* If there are any triggers to fire, allocate a range of registers to
	 * use for the old.* references in the triggers.
	 */
	if (space != NULL && trigger_list != NULL) {
		/* Mask of OLD.* columns in use */
		/* TODO: Could use temporary registers here. */
		uint64_t mask =
			sql_trigger_colmask(parse, trigger_list, 0, false,
					    TRIGGER_BEFORE | TRIGGER_AFTER,
					    space, onconf);
		assert(space != NULL);
		first_old_reg = parse->nMem + 1;
		parse->nMem += (1 + (int)space->def->field_count);

		/* Populate the OLD.* pseudo-table register array.
		 * These values will be used by any BEFORE and
		 * AFTER triggers that exist.
		 */
		sqlVdbeAddOp2(v, OP_Copy, reg_pk, first_old_reg);
		for (int i = 0; i < (int)space->def->field_count; i++) {
			if (column_mask_fieldno_is_set(mask, i)) {
				sqlVdbeAddOp3(v, OP_Column, cursor, i,
					      first_old_reg + i + 1);
			}
		}

		/* Invoke BEFORE DELETE trigger programs. */
		int addr_start = sqlVdbeCurrentAddr(v);
		vdbe_code_row_trigger(parse, trigger_list, TK_DELETE, NULL,
				      TRIGGER_BEFORE, space, first_old_reg,
				      onconf, label);

		/* If any BEFORE triggers were coded, then seek
		 * the cursor to the row to be deleted again. It
		 * may be that the BEFORE triggers moved the
		 * cursor or of already deleted the row that the
		 * cursor was pointing to.
		 */
		if (addr_start < sqlVdbeCurrentAddr(v)) {
			sqlVdbeAddOp4Int(v, OP_NotFound, cursor, label,
					     reg_pk, npk);
		}
	}

	/* Delete the index and table entries. Skip this step if
	 * table is really a view (in which case the only effect
	 * of the DELETE statement is to fire the INSTEAD OF
	 * triggers).
	 */
	if (space == NULL || !space->def->opts.is_view) {
		uint8_t p5 = 0;
		sqlVdbeAddOp2(v, OP_Delete, cursor,
				  (need_update_count ? OPFLAG_NCHANGE : 0));
		if (mode != ONEPASS_OFF)
			sqlVdbeChangeP5(v, OPFLAG_AUXDELETE);

		if (idx_noseek >= 0)
			sqlVdbeAddOp1(v, OP_Delete, idx_noseek);

		if (mode == ONEPASS_MULTI)
			p5 |= OPFLAG_SAVEPOSITION;
		sqlVdbeChangeP5(v, p5);
	}

	if (space != NULL) {
		/* Invoke AFTER DELETE trigger programs. */
		vdbe_code_row_trigger(parse, trigger_list, TK_DELETE, 0,
				      TRIGGER_AFTER, space, first_old_reg,
				      onconf, label);
	}

	/* Jump here if the row had already been deleted before
	 * any BEFORE trigger programs were invoked. Or if a trigger program
	 * throws a RAISE(IGNORE) exception.
	 */
	sqlVdbeResolveLabel(v, label);
}
