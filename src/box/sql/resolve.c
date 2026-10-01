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

/*
 *
 * This file contains routines used for walking the parser tree and
 * resolve all identifiers by associating them with a particular
 * table and column.
 */
#include "sqlInt.h"
#include "resolve.h"
#include <stdlib.h>
#include <string.h>
#include "box/schema.h"
#include "box/coll_id_cache.h"

/**
 * Names visible at some point of the resolve phase. It mirrors the legacy
 * NameContext: every SELECT resolves its clauses in scopes of its own, and a
 * name that is not found in a scope is looked up in the enclosing one.
 */
struct sql_resolve_scope {
	/** Sources whose columns are visible, or NULL. */
	const struct rast_source_list *sources;
	/** Result columns whose aliases are visible, or NULL. */
	const struct rast_expr_list *aliases;
	/** Enclosing scope, or NULL if its names are not visible. */
	struct sql_resolve_scope *parent;
	/** Whether an aggregate function is allowed at the current point. */
	bool is_agg_allowed;
	/**
	 * SELECT flags describing the aggregate functions of the SELECT found
	 * while resolving the expressions of this scope, directly or through
	 * the aliases of the result columns.
	 */
	uint32_t agg_flags;
	/**
	 * Whether an unqualified name was met outside of the arguments of an
	 * aggregate function while resolving the expressions of this scope.
	 */
	bool has_unaggregated_name;
	/**
	 * The least number of subqueries between the expressions resolved in
	 * this scope and the SELECTs whose sources they refer to, outside of
	 * their subqueries, directly or through the aliases of the result
	 * columns. It is reset to UINT32_MAX before resolving an expression
	 * the references of which are needed.
	 */
	uint32_t ref_depth;
	/**
	 * Number of names found in this scope or in the scopes enclosing it,
	 * while resolving the expressions of this scope. Used to find out if a
	 * subquery refers to names of the enclosing SELECTs.
	 */
	uint32_t ref_count;
};

/** State threaded through the AST resolve phase. */
struct sql_resolve_context {
	/** Parser the statement is resolved for. */
	struct Parse *parser;
	/** Region the resolved AST is allocated on. */
	struct region *region;
	/** Current scope of names. */
	struct sql_resolve_scope *scope;
	/** Number of the result columns named automatically so far. */
	uint32_t autoname_count;
	/** Number of the SELECTs enclosing the one being resolved. */
	uint32_t select_depth;
	/** Whether the statement being resolved has a resolved form yet. */
	bool can_resolve;
};

/**
 * Resolve the given AST column expression into the given rast_expr.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_expr(struct sql_resolve_context *ctx, const struct ast_expr *ast,
	     struct rast_expr *res);

/**
 * Resolve the given AST SELECT statement nested in an expression into the given
 * rast_select.
 */
static int
resolve_nested_select(struct sql_resolve_context *ctx,
		      const struct ast_select *ast, struct rast_select *res);

/**
 * Sources of a SELECT being built from its resolved form, visible to the
 * expressions of the SELECT and of its subqueries.
 */
struct rast_build_scope {
	/**
	 * Built sources of the SELECT. NULL if the SELECT has no sources or
	 * if its expressions are built only to be compared.
	 */
	struct SrcList *src;
	/** Scope of the enclosing SELECT, or NULL. */
	const struct rast_build_scope *parent;
};

/**
 * Create an expression from the given resolved expression AST in the given
 * scope of sources.
 */
static struct Expr *
expr_from_rast(struct Parse *parser, const struct rast_build_scope *scope,
	       const struct rast_expr *expr);

/**
 * Create a SELECT statement from the given resolved SELECT, which is a
 * subquery of a SELECT with the given scope of sources, or is not a subquery
 * if the scope is NULL.
 *
 * Returns NULL on error.
 */
static struct Select *
select_from_rast_in(struct Parse *parser,
		    const struct rast_build_scope *parent,
		    struct rast_select *select);

/**
 * Create a source list from the given resolved source list and assign cursors
 * to its sources.
 *
 * Returns NULL if the list is empty.
 */
static struct SrcList *
src_list_from_rast(struct Parse *parser, const struct rast_source_list *list);

/*
 * Walk the expression tree pExpr and increase the aggregate function
 * depth (the Expr.op2 field) by N on every TK_AGG_FUNCTION node.
 * This needs to occur when copying a TK_AGG_FUNCTION node from an
 * outer query into an inner subquery.
 *
 * incrAggFunctionDepth(pExpr,n) is the main routine.  incrAggDepth(..)
 * is a helper function - a callback for the tree walker.
 */
static int
incrAggDepth(Walker * pWalker, Expr * pExpr)
{
	if (pExpr->op == TK_AGG_FUNCTION)
		pExpr->op2 += pWalker->u.n;
	return WRC_Continue;
}

static void
incrAggFunctionDepth(Expr * pExpr, int N)
{
	if (N > 0) {
		Walker w;
		memset(&w, 0, sizeof(w));
		w.xExprCallback = incrAggDepth;
		w.u.n = N;
		sqlWalkExpr(&w, pExpr);
	}
}

/*
 * Turn the pExpr expression into an alias for the iCol-th column of the
 * result set in pEList.
 *
 * If the reference is followed by a COLLATE operator, then make sure
 * the COLLATE operator is preserved.  For example:
 *
 *     SELECT a+b, c+d FROM t1 ORDER BY 1 COLLATE nocase;
 *
 * Should be transformed into:
 *
 *     SELECT a+b, c+d FROM t1 ORDER BY (a+b) COLLATE nocase;
 *
 * The nSubquery parameter specifies how many levels of subquery the
 * alias is removed from the original expression.  The usual value is
 * zero but it might be more if the alias is contained within a subquery
 * of the original expression.  The Expr.op2 field of TK_AGG_FUNCTION
 * structures must be increased by the nSubquery amount.
 *
 * @param parser Parser context.
 * @param pEList A result set.
 * @param iCol A column in the result set.  0..pEList->nExpr-1.
 * @param pExpr Transform this into an alias to the result set.
 * @param zType "GROUP" or "ORDER" or "".
 * @param nSubquery Number of subqueries that the label is moving.
 */
static void
resolveAlias(struct Parse *parser, struct ExprList *pEList, int iCol,
	     struct Expr *pExpr, const char *zType, int nSubquery)
{
	Expr *pOrig;		/* The iCol-th column of the result set */
	Expr *pDup;		/* Copy of pOrig */

	assert(iCol >= 0 && iCol < pEList->nExpr);
	pOrig = pEList->a[iCol].pExpr;
	assert(pOrig != 0);
	pDup = sqlExprDup(pOrig, 0);
	if (pDup == 0)
		return;
	if (zType[0] != 'G')
		incrAggFunctionDepth(pDup, nSubquery);
	if (pExpr->op == TK_COLLATE)
		pDup = sql_expr_new_collate(parser, pDup, pExpr->v.id);
	/*
	 * The alias may be in the ON clause of a LEFT JOIN, so the copy must
	 * be a part of the join restriction too.
	 */
	if (ExprHasProperty(pExpr, EP_FromJoin))
		sql_expr_set_join(pDup, pExpr->iRightJoinTable);

	/* Before calling sql_expr_delete(), set the EP_Static flag. This
	 * prevents ExprDelete() from deleting the Expr structure itself,
	 * allowing it to be repopulated by the memcpy() on the following line.
	 * The pExpr->u.zToken might point into memory that will be freed by the
	 * sql_xfree(pDup) on the last line of this block, so be sure to
	 * make a copy of the token before doing the sql_xfree().
	 */
	ExprSetProperty(pExpr, EP_Static);
	sql_expr_delete(pExpr);
	memcpy(pExpr, pDup, sizeof(*pExpr));
	if (pExpr->u.zToken != NULL) {
		assert((pExpr->flags & (EP_Reduced | EP_TokenOnly)) == 0);
		pExpr->u.zToken = sql_xstrdup(pExpr->u.zToken);
		pExpr->flags |= EP_MemToken;
	} else if (pExpr->op == TK_STRING) {
		pExpr->v.s = sql_xstrdup(pExpr->v.s);
		pExpr->flags |= EP_MemToken;
	} else if (pExpr->op == TK_RAISE) {
		pExpr->v.raise = sql_xstrdup(pExpr->v.raise);
		pExpr->flags |= EP_MemToken;
	} else if (pExpr->op == TK_BLOB) {
		pExpr->v.z = sql_xstrndup(pExpr->v.z, pExpr->v.n);
		pExpr->flags |= EP_MemToken;
	}
	sql_xfree(pDup);
}

/**
 * Return TRUE if the name occurs anywhere in the USING clause.
 * Return FALSE if the USING clause is NULL or if it does not contain the name.
 */
static bool
nameInUsingClause(struct IdList *pUsing, const char *zCol, const char *old_col)
{
	if (pUsing == NULL)
		return false;
	for (int k = 0; k < pUsing->nId; k++) {
		if (strcmp(pUsing->a[k].zName, zCol) == 0)
			return true;
	}
	if (old_col == NULL)
		return false;
	for (int i = 0; i < pUsing->nId; i++) {
		if (strcmp(pUsing->a[i].zName, old_col) == 0)
			return true;
	}
	return false;
}

/**
 * Check if the given part of a span, with the given length, is the given name
 * or, if it is not NULL, the given legacy name.
 */
static bool
span_part_matches(const char *part, size_t len, const char *name,
		  const char *old_name)
{
	if (strlen(name) == len && strncmp(part, name, len) == 0)
		return true;
	return old_name != NULL && strlen(old_name) == len &&
	       strncmp(part, old_name, len) == 0;
}

bool
sqlMatchSpanName(const char *span, const char *col, const char *tab,
		 const char *old_col, const char *old_tab)
{
	const char *dot = strchr(span, '.');
	assert(dot != NULL);
	if (tab != NULL && !span_part_matches(span, dot - span, tab, old_tab))
		return false;
	return col == NULL ||
	       span_part_matches(dot + 1, strlen(dot + 1), col, old_col);
}

/**
 * Count the expressions of the given list whose spans match the given column
 * name, qualified with the given table name if it is not NULL, and set the
 * position of the last one. As for the fields of a space, the legacy names
 * are used only if the names match no span.
 */
static int
sql_count_span_matches(const struct ExprList *list, const char *tab_name,
		       const char *col_name, const char *old_tab,
		       const char *old_col, int *pos)
{
	int count = 0;
	for (int j = 0; j < list->nExpr; j++) {
		if (sqlMatchSpanName(list->a[j].zSpan, col_name, tab_name,
				     NULL, NULL)) {
			++count;
			*pos = j;
		}
	}
	if (count > 0 || (old_tab == NULL && old_col == NULL))
		return count;
	for (int j = 0; j < list->nExpr; j++) {
		if (sqlMatchSpanName(list->a[j].zSpan, col_name, tab_name,
				     old_col, old_tab)) {
			++count;
			*pos = j;
		}
	}
	return count;
}

/**
 * This function counts the number of occurrences of name in space.
 */
static int
sql_count_column_in_space_def(const struct space_def *space_def,
			      const char *name, const char *old_name)
{
	int count = 0;
	for (uint32_t i = 0; i < space_def->field_count; ++i) {
		if (strcmp(space_def->fields[i].name, name) == 0)
			count++;
	}
	if (count == 0 && old_name != NULL) {
		for (uint32_t i = 0; i < space_def->field_count; ++i) {
			if (strcmp(space_def->fields[i].name, old_name) == 0)
				count++;
		}
	}
	return count;
}

/*
 * Given the name of a column of the form X.Y.Z or Y.Z or just Z, look up
 * that name in the set of source tables in pSrcList and make the pExpr
 * expression node refer back to that source column.  The following changes
 * are made to pExpr:
 *
 *    pExpr->iTable        Set to the cursor number for the table obtained
 *                         from pSrcList.
 *    pExpr->space_def     Points to the space_def structure of X.Y
 *                         (even if X and/or Y are implied.)
 *    pExpr->iColumn       Set to the column number within the table.
 *    pExpr->op            Set to TK_COLUMN_REF.
 *    pExpr->pLeft         Any expression this points to is deleted
 *    pExpr->pRight        Any expression this points to is deleted.
 *
 * Name is of the form Y.Z or Z.
 * The zTable variable is the name of the table (the "Y").  This
 * value can be NULL.  If zTable is NULL it
 * means that the form of the name is Z and that columns from any table
 * can be used.
 *
 * If the name cannot be resolved unambiguously, leave an error message
 * in pParse and return WRC_Abort.  Return WRC_Prune on success.
 */
static int
lookupName(struct Parse *pParse, struct Expr *pExpr, struct NameContext *pNC)
{
	const char *zTab = NULL;
	const char *zCol = NULL;
	char *old_tab = NULL;
	char *old_col = NULL;
	const struct Expr *col = NULL;
	if (pExpr->op == TK_DOT) {
		assert(pExpr->pLeft->op == TK_ID && pExpr->pRight->op == TK_ID);
		zTab = pExpr->pLeft->u.zToken;
		if ((pExpr->pLeft->flags & EP_Lookup2) != 0)
			old_tab = sql_legacy_name_new0(zTab);
		zCol = pExpr->pRight->u.zToken;
		if ((pExpr->pRight->flags & EP_Lookup2) != 0)
			old_col = sql_legacy_name_new0(zCol);
		col = pExpr->pRight;
	} else {
		assert(pExpr->op == TK_ID);
		zCol = pExpr->u.zToken;
		if ((pExpr->flags & EP_Lookup2) != 0)
			old_col = sql_legacy_name_new0(zCol);
		col = pExpr;
	}

	int i, j;		/* Loop counters */
	int cnt = 0;		/* Number of matching column names */
	int cntTab = 0;		/* Number of matching table names */
	int nSubquery = 0;	/* How many levels of subquery */
	struct SrcList_item *pItem;	/* Use for looping over pSrcList items */
	struct SrcList_item *pMatch = 0;	/* The matching pSrcList item */
	NameContext *pTopNC = pNC;	/* First namecontext in the list */
	int isTrigger = 0;	/* True if resolved to a trigger column */

	assert(pNC);		/* the name context cannot be NULL. */
	assert(zCol);		/* The Z in X.Y.Z cannot be NULL */
	assert(!ExprHasProperty(pExpr, EP_TokenOnly | EP_Reduced));

	/* Initialize the node to no-match */
	pExpr->iTable = -1;
	pExpr->space_def = NULL;
	ExprSetVVAProperty(pExpr, EP_NoReduce);

	/* Start at the inner-most context and move outward until a match is found */
	while (pNC && cnt == 0) {
		ExprList *pEList;
		SrcList *pSrcList = pNC->pSrcList;

		if (pSrcList) {
			for (i = 0, pItem = pSrcList->a; i < pSrcList->nSrc;
			     i++, pItem++) {
				assert(pItem->space != NULL &&
				       pItem->space->def->name != NULL);
				struct space_def *space_def = pItem->space->def;
				assert(space_def->field_count > 0);
				/*
				 * A column of a parenthesized list of sources
				 * is looked up by its span, and if it is not
				 * found, a qualified name is looked up among
				 * the fields of the list as usual.
				 */
				if (pItem->pSelect
				    && (pItem->pSelect->
					selFlags & SF_NestedFrom) != 0) {
					pEList = pItem->pSelect->pEList;
					int hit = sql_count_span_matches(
						pEList, zTab, zCol, old_tab,
						old_col, &j);
					if (hit > 0) {
						cnt += hit;
						cntTab = 2;
						pMatch = pItem;
						pExpr->iColumn = j;
					}
					if (hit > 0 || zTab == 0)
						continue;
				}
				if (zTab) {
					const char *zTabName =
					    pItem->zAlias ? pItem->
					    zAlias : space_def->name;
					assert(zTabName != 0);
					if (strcmp(zTabName, zTab) != 0 &&
					    (old_tab == NULL ||
					     strcmp(zTabName, old_tab) != 0))
						continue;
				}
				if (0 == (cntTab++)) {
					pMatch = pItem;
				}
				uint32_t fieldno =
					sql_fieldno_by_expr(pItem->space, col);
				if (fieldno == UINT32_MAX)
					continue;
				j = fieldno;
				/*
				 * If there has been exactly one prior match and
				 * this match is for the right-hand table of a
				 * NATURAL JOIN or is in a USING clause, then
				 * skip this match.
				 */
				if (cnt == 1 &&
				    ((pItem->fg.jointype & JT_NATURAL) != 0 ||
				     nameInUsingClause(pItem->pUsing, zCol,
						       old_col)))
					continue;
				int count =
					sql_count_column_in_space_def(space_def,
								      zCol,
								      old_col);
				cnt = cnt + count;
				pMatch = pItem;
				pExpr->iColumn = (i16) j;
			}
			if (pMatch) {
				pExpr->iTable = pMatch->iCursor;
				pExpr->space_def = pMatch->space->def;
				/* RIGHT JOIN not (yet) supported */
				assert((pMatch->fg.jointype & JT_RIGHT) == 0);
				if ((pMatch->fg.jointype & JT_LEFT) != 0) {
					ExprSetProperty(pExpr, EP_CanBeNull);
				}
			}
		}
		/* if( pSrcList ) */
		/* If we have not already resolved the name, then maybe
		 * it is a new.* or old.* trigger argument reference
		 */
		if (zTab != NULL && cntTab == 0 &&
		    pParse->triggered_space != NULL) {
			int op = pParse->eTriggerOp;
			assert(op == TK_DELETE || op == TK_UPDATE
			       || op == TK_INSERT);
			struct space_def *space_def = NULL;
			if (op != TK_DELETE && sqlStrICmp("new", zTab) == 0) {
				pExpr->iTable = 1;
				space_def = pParse->triggered_space->def;
			} else if (op != TK_INSERT
				   && sqlStrICmp("old", zTab) == 0) {
				pExpr->iTable = 0;
				space_def = pParse->triggered_space->def;
			}

			if (space_def != NULL) {
				int iCol;
				cntTab++;
				for (iCol = 0; iCol <
				     (int)space_def->field_count; iCol++) {
					const char *name =
						space_def->fields[iCol].name;
					if (strcmp(name, zCol) == 0 ||
					    (old_col != NULL &&
					     strcmp(name, old_col) == 0))
						break;
				}
				if (iCol < (int)space_def->field_count) {
					cnt++;
					uint64_t *mask = pExpr->iTable == 0 ?
							 &pParse->oldmask :
							 &pParse->newmask;
					column_mask_set_fieldno(mask, iCol);
					pExpr->iColumn = iCol;
					pExpr->space_def = space_def;
					isTrigger = 1;
				}
			}
		}

		/*
		 * If the input is of the form Z (not Y.Z or X.Y.Z) then the name Z
		 * might refer to an result-set alias.  This happens, for example, when
		 * we are resolving names in the WHERE clause of the following command:
		 *
		 *     SELECT a+b AS x FROM table WHERE x<10;
		 *
		 * In cases like this, replace pExpr with a copy of the expression that
		 * forms the result set entry ("a+b" in the example) and return immediately.
		 * Note that the expression in the result set should have already been
		 * resolved by the time the WHERE clause is resolved.
		 *
		 * The ability to use an output result-set column in the WHERE, GROUP BY,
		 * or HAVING clauses, or as part of a larger expression in the ORDER BY
		 * clause is not standard SQL.  This is a (goofy) sql extension, that
		 * is supported for backwards compatibility only.
		 */
		if ((pEList = pNC->pEList) != 0 && zTab == 0 && cnt == 0) {
			for (j = 0; j < pEList->nExpr; j++) {
				char *zAs = pEList->a[j].zName;
				if (zAs != 0 && (strcmp(zAs, zCol) == 0 ||
						 (old_col != NULL &&
						  strcmp(zAs, old_col) == 0))) {
					Expr *pOrig;
					assert(pExpr->pLeft == 0
					       && pExpr->pRight == 0);
					assert(pExpr->x.pList == 0);
					assert(pExpr->x.pSelect == 0);
					pOrig = pEList->a[j].pExpr;
					const char *err = "misuse of aliased "\
							  "aggregate %s";
					if ((pNC->ncFlags & NC_AllowAgg) == 0
					    && ExprHasProperty(pOrig, EP_Agg)) {
						diag_set(ClientError,
							 ER_SQL_PARSER_GENERIC,
							 tt_sprintf(err, zAs));
						pParse->is_aborted = true;
						goto lookupname_end;
					}
					if (sqlExprVectorSize(pOrig) != 1) {
						diag_set(ClientError,
							 ER_SQL_PARSER_GENERIC,
							 "row value misused");
						pParse->is_aborted = true;
						goto lookupname_end;
					}
					resolveAlias(pParse, pEList, j, pExpr,
						     "", nSubquery);
					pParse->alias_count++;
					if (pParse->is_aborted)
						goto lookupname_end;
					cnt = 1;
					pMatch = 0;
					assert(zTab == 0);
					goto lookupname_end;
				}
			}
		}

		/* Advance to the next name context.  The loop will exit when either
		 * we have a match (cnt>0) or when we run out of name contexts.
		 */
		if (cnt == 0) {
			pNC = pNC->pNext;
			nSubquery++;
		}
	}

	/*
	 * cnt==0 means there was not match.  cnt>1 means there were two or
	 * more matches.  Either way, we have an error.
	 */
	if (cnt > 1) {
		const char *err;
		if (zTab) {
			err = tt_sprintf("ambiguous column name: %s.%s", zTab,
					 zCol);
		} else {
			err = tt_sprintf("ambiguous column name: %s", zCol);
		}
		diag_set(ClientError, ER_SQL_PARSER_GENERIC, err);
		pParse->is_aborted = true;
		pTopNC->nErr++;
	}
	if (cnt == 0) {
		if (zTab == NULL) {
			diag_set(ClientError, ER_SQL_CANT_RESOLVE_FIELD, zCol);
		} else {
			diag_set(ClientError, ER_NO_SUCH_FIELD_NAME_IN_SPACE,
				 zCol, zTab);
		}
		pParse->is_aborted = true;
		pTopNC->nErr++;
	}

	/* If a column from a table in pSrcList is referenced, then record
	 * this fact in the pSrcList.a[].colUsed bitmask.  Column 0 causes
	 * bit 0 to be set.  Column 1 sets bit 1.  And so forth.  If the
	 * column number is greater than the number of bits in the bitmask
	 * then set the high-order bit of the bitmask.
	 */
	if (pExpr->iColumn >= 0 && pMatch != 0) {
		int n = pExpr->iColumn;
		if (n >= BMS) {
			n = BMS - 1;
		}
		assert(pMatch->iCursor == pExpr->iTable);
		pMatch->colUsed |= ((Bitmask) 1) << n;
	}

	/* Clean up and return
	 */
	sql_expr_delete(pExpr->pLeft);
	pExpr->pLeft = 0;
	sql_expr_delete(pExpr->pRight);
	pExpr->pRight = 0;
	pExpr->op = (isTrigger ? TK_TRIGGER : TK_COLUMN_REF);
 lookupname_end:
	sql_xfree(old_tab);
	sql_xfree(old_col);
	if (cnt == 1 && !pParse->is_aborted) {
		assert(pNC != 0);
		/* Increment the nRef value on all name contexts from TopNC up to
		 * the point where the name matched.
		 */
		for (;;) {
			assert(pTopNC != 0);
			pTopNC->nRef++;
			if (pTopNC == pNC)
				break;
			pTopNC = pTopNC->pNext;
		}
		return WRC_Prune;
	} else {
		return WRC_Abort;
	}
}

struct Expr *
sql_expr_new_column(struct SrcList *src_list, int src_idx, int column)
{
	struct Expr *expr = sql_expr_new_anon(TK_COLUMN_REF);
	struct SrcList_item *item = &src_list->a[src_idx];
	expr->space_def = item->space->def;
	expr->iTable = item->iCursor;
	expr->iColumn = column;
	item->colUsed |= ((Bitmask) 1) << (column >= BMS ? BMS - 1 : column);
	ExprSetProperty(expr, EP_Resolved);
	return expr;
}

/*
 * Expression p should encode a floating point value between 1.0 and 0.0.
 * Return 1024 times this value.  Or return -1 if p is not a floating point
 * value between 1.0 and 0.0.
 */
static int
exprProbability(Expr * p)
{
	if (p->op != TK_FLOAT)
		return -1;
	double r = p->v.f;
	if (r < 0.0 || r > 1.0)
		return -1;
	return (int)(r * 134217728.0);
}

/*
 * This routine is callback for sqlWalkExpr().
 *
 * Resolve symbolic names into TK_COLUMN_REF operators for the current
 * node in the expression tree.  Return 0 to continue the search down
 * the tree or 2 to abort the tree walk.
 *
 * This routine also does error checking and name resolution for
 * function names.  The operator for aggregate functions is changed
 * to TK_AGG_FUNCTION.
 */
static int
resolveExprStep(Walker * pWalker, Expr * pExpr)
{
	NameContext *pNC;
	Parse *pParse;

	pNC = pWalker->u.pNC;
	assert(pNC != 0);
	pParse = pNC->pParse;
	assert(pParse == pWalker->pParse);

	if (ExprHasProperty(pExpr, EP_Resolved))
		return WRC_Prune;
	ExprSetProperty(pExpr, EP_Resolved);
#ifndef NDEBUG
	if (pNC->pSrcList && pNC->pSrcList->nAlloc > 0) {
		SrcList *pSrcList = pNC->pSrcList;
		int i;
		for (i = 0; i < pNC->pSrcList->nSrc; i++) {
			assert(pSrcList->a[i].iCursor >= 0
			       && pSrcList->a[i].iCursor < pParse->nTab);
		}
	}
#endif
	assert((pNC->ncFlags & NC_IdxExpr) == 0 ||
	       sqlExprSkipCollate(pExpr)->op == TK_ID);
	switch (pExpr->op) {
		/* A lone identifier is the name of a column.
		 */
	case TK_ID:{
			if ((pNC->ncFlags & NC_AllowAgg) != 0)
				pNC->ncFlags |= NC_HasUnaggregatedId;
			return lookupName(pParse, pExpr, pNC);
		}

	/* A table name and column name: ID.ID. */
	case TK_DOT:
		return lookupName(pParse, pExpr, pNC);

		/* Resolve function names
		 */
	case TK_FUNCTION:{
			ExprList *pList = pExpr->x.pList;	/* The argument list */
			int n = pList ? pList->nExpr : 0;	/* Number of arguments */
			int nId;	/* Number of characters in function name */
			const char *zId;	/* The function name. */

			assert(!ExprHasProperty(pExpr, EP_xIsSelect));
			zId = pExpr->u.zToken;
			nId = sqlStrlen30(zId);
			uint32_t flags = sql_func_flags(pExpr);
			bool is_agg = (flags & SQL_FUNC_AGG) != 0;
			if ((flags & SQL_FUNC_UNLIKELY) != 0 && n == 2) {
				ExprSetProperty(pExpr, EP_Unlikely | EP_Skip);
				pExpr->iTable =
					exprProbability(pList->a[1].pExpr);
				if (pExpr->iTable < 0) {
					diag_set(IllegalParams,
						 "second argument to "
						 "LIKELIHOOD() must be a "
						 "constant between 0.0 and "
						 "1.0");
					pParse->is_aborted = true;
					pNC->nErr++;
					return WRC_Abort;
				}
			} else if ((flags & SQL_FUNC_UNLIKELY) != 0) {
				ExprSetProperty(pExpr, EP_Unlikely | EP_Skip);
				/*
				 * unlikely() probability is
				 * 0.0625, likely() is 0.9375
				 */
				pExpr->iTable = zId[0] == 'u' || zId[0] == 'U' ?
						8388608 : 125829120;
			}
			if (is_agg && (pNC->ncFlags & NC_AllowAgg) == 0) {
				const char *err =
					tt_sprintf("misuse of aggregate "\
						   "function %.*s()", nId, zId);
				diag_set(ClientError, ER_SQL_PARSER_GENERIC, err);
				pParse->is_aborted = true;
				pNC->nErr++;
				is_agg = 0;
			}
			if (is_agg)
				pNC->ncFlags &= ~NC_AllowAgg;
			sqlWalkExprList(pWalker, pList);
			if (pParse->is_aborted)
				break;
			if (is_agg) {
				NameContext *pNC2 = pNC;
				pExpr->op = TK_AGG_FUNCTION;
				pExpr->op2 = 0;
				while (pNC2
				       && !sqlFunctionUsesThisSrc(pExpr,
								      pNC2->
								      pSrcList))
				{
					pExpr->op2++;
					pNC2 = pNC2->pNext;
				}
				if (pNC2) {
					pNC2->ncFlags |= NC_HasAgg;
					if ((flags & (SQL_FUNC_MIN |
						      SQL_FUNC_MAX)) != 0)
						pNC2->ncFlags |= NC_MinMaxAgg;
				}
				pNC->ncFlags |= NC_AllowAgg;
			}
			struct func *func = sql_func_find(pExpr);
			if (func == NULL) {
				pParse->is_aborted = true;
				pNC->nErr++;
				return WRC_Abort;
			}
			pExpr->type = func->def->returns;
			/*
			 * In case a user-defined aggregate function was called,
			 * the result type will be the result type of the
			 * FINALIZE part of the function.
			 */
			if (func->def->language != FUNC_LANGUAGE_SQL_BUILTIN &&
			    func->def->aggregate == FUNC_AGGREGATE_GROUP) {
				const char *name = func->def->name;
				struct func *finalize = sql_func_finalize(name);
				if (finalize != NULL)
					pExpr->type = finalize->def->returns;
			}
			if (func->def->is_deterministic)
				ExprSetProperty(pExpr, EP_ConstFunc);
			return WRC_Prune;
		}
	case TK_SELECT:
	case TK_EXISTS:
	case TK_IN:{
			if (ExprHasProperty(pExpr, EP_xIsSelect)) {
				int nRef = pNC->nRef;
				sqlWalkSelect(pWalker, pExpr->x.pSelect);
				assert(pNC->nRef >= nRef);
				if (nRef != pNC->nRef) {
					ExprSetProperty(pExpr, EP_VarSelect);
					pNC->ncFlags |= NC_VarSelect;
				}
			}
			break;
		}
	case TK_BETWEEN:
	case TK_EQ:
	case TK_NE:
	case TK_LT:
	case TK_LE:
	case TK_GT:
	case TK_GE:{
			int nLeft, nRight;
			assert(pExpr->pLeft != 0);
			nLeft = sqlExprVectorSize(pExpr->pLeft);
			if (pExpr->op == TK_BETWEEN) {
				nRight =
				    sqlExprVectorSize(pExpr->x.pList->a[0].
							  pExpr);
				if (nRight == nLeft) {
					nRight =
					    sqlExprVectorSize(pExpr->x.
								  pList->a[1].
								  pExpr);
				}
			} else {
				assert(pExpr->pRight != 0);
				nRight = sqlExprVectorSize(pExpr->pRight);
			}
			if (nLeft != nRight) {
				diag_set(ClientError, ER_SQL_COLUMN_COUNT,
					 nLeft, nRight);
				pParse->is_aborted = true;
			}
			break;
		}
	case TK_CASE:{
			if (pExpr->pLeft == NULL)
				break;
			int nLeft = sqlExprVectorSize(pExpr->pLeft);
			struct ExprList *list = pExpr->x.pList;
			for (int i = 0; i + 1 < list->nExpr; i += 2) {
				struct Expr *e = list->a[i].pExpr;
				int nWhen = sqlExprVectorSize(e);
				if (nWhen == nLeft)
					continue;
				diag_set(ClientError, ER_SQL_COLUMN_COUNT,
					 nLeft, nWhen);
				pParse->is_aborted = true;
				break;
			}
			break;
		}
	}
	return pParse->is_aborted ? WRC_Abort : WRC_Continue;
}

/*
 * pEList is a list of expressions which are really the result set of the
 * a SELECT statement.  pE is a term in an ORDER BY or GROUP BY clause.
 * This routine checks to see if pE is a simple identifier which corresponds
 * to the AS-name of one of the terms of the expression list.  If it is,
 * this routine return an integer between 1 and N where N is the number of
 * elements in pEList, corresponding to the matching entry.  If there is
 * no match, or if pE is not a simple identifier, then this routine
 * return 0.
 *
 * pEList has been resolved.  pE has not.
 */
static int
resolveAsName(Parse * pParse,	/* Parsing context for error messages */
	      ExprList * pEList,	/* List of expressions to scan */
	      Expr * pE		/* Expression we are trying to match */
    )
{
	int i;			/* Loop counter */

	UNUSED_PARAMETER(pParse);

	if (pE->op == TK_ID) {
		char *zCol = pE->u.zToken;
		for (i = 0; i < pEList->nExpr; i++) {
			char *zAs = pEList->a[i].zName;
			if (zAs != 0 && strcmp(zAs, zCol) == 0) {
				return i + 1;
			}
		}
	}
	return 0;
}

/*
 * pE is a pointer to an expression which is a single term in the
 * ORDER BY of a compound SELECT.  The expression has not been
 * name resolved.
 *
 * At the point this routine is called, we already know that the
 * ORDER BY term is not an integer index into the result set.  That
 * case is handled by the calling routine.
 *
 * Attempt to match pE against result set columns in the left-most
 * SELECT statement.  Return the index i of the matching column,
 * as an indication to the caller that it should sort by the i-th column.
 * The left-most column is 1.  In other words, the value returned is the
 * same integer value that would be used in the SQL statement to indicate
 * the column.
 *
 * If there is no match, return 0.  Return -1 if an error occurs.
 */
static int
resolveOrderByTermToExprList(Parse * pParse,	/* Parsing context for error messages */
			     Select * pSelect,	/* The SELECT statement with the ORDER BY clause */
			     Expr * pE	/* The specific ORDER BY term */
    )
{
	int i;			/* Loop counter */
	ExprList *pEList;	/* The columns of the result set */
	NameContext nc;		/* Name context for resolving pE */
	int rc;			/* Return code from subprocedures */

	assert(sqlExprIsInteger(pE, &i) == 0);
	pEList = pSelect->pEList;

	/* Resolve all names in the ORDER BY term expression
	 */
	memset(&nc, 0, sizeof(nc));
	nc.pParse = pParse;
	nc.pSrcList = pSelect->pSrc;
	nc.pEList = pEList;
	nc.ncFlags = NC_AllowAgg;
	nc.nErr = 0;
	rc = sqlResolveExprNames(&nc, pE);
	if (rc)
		return 0;

	/* Try to match the ORDER BY expression against an expression
	 * in the result set.  Return an 1-based index of the matching
	 * result-set entry.
	 */
	for (i = 0; i < pEList->nExpr; i++) {
		if (sqlExprCompare(pEList->a[i].pExpr, pE, -1) < 2) {
			return i + 1;
		}
	}

	/* If no match, return 0. */
	return 0;
}

/*
 * Analyze the ORDER BY clause in a compound SELECT statement.   Modify
 * each term of the ORDER BY clause is a constant integer between 1
 * and N where N is the number of columns in the compound SELECT.
 *
 * ORDER BY terms that are already an integer between 1 and N are
 * unmodified.  ORDER BY terms that are integers outside the range of
 * 1 through N generate an error.  ORDER BY terms that are expressions
 * are matched against result set expressions of compound SELECT
 * beginning with the left-most SELECT and working toward the right.
 * At the first match, the ORDER BY expression is transformed into
 * the integer column number.
 *
 * Return the number of errors seen.
 */
static int
resolveCompoundOrderBy(Parse * pParse,	/* Parsing context.  Leave error messages here */
		       Select * pSelect	/* The SELECT statement containing the ORDER BY */
    )
{
	int i;
	ExprList *pOrderBy;
	ExprList *pEList;
	int moreToDo = 1;

	pOrderBy = pSelect->pOrderBy;
	if (pOrderBy == 0)
		return 0;
	if (pOrderBy->nExpr > SQL_MAX_COLUMN) {
		diag_set(ClientError, ER_SQL_PARSER_LIMIT,
			 "The number of terms in ORDER BY clause",
			 pOrderBy->nExpr, SQL_MAX_COLUMN);
		pParse->is_aborted = true;
		return 1;
	}
	for (i = 0; i < pOrderBy->nExpr; i++) {
		pOrderBy->a[i].done = 0;
	}
	pSelect->pNext = 0;
	while (pSelect->pPrior) {
		pSelect->pPrior->pNext = pSelect;
		pSelect = pSelect->pPrior;
	}
	while (pSelect && moreToDo) {
		struct ExprList_item *pItem;
		moreToDo = 0;
		pEList = pSelect->pEList;
		assert(pEList != 0);
		for (i = 0, pItem = pOrderBy->a; i < pOrderBy->nExpr;
		     i++, pItem++) {
			int iCol = -1;
			Expr *pE, *pDup;
			if (pItem->done)
				continue;
			pE = sqlExprSkipCollate(pItem->pExpr);
			if (sqlExprIsInteger(pE, &iCol)) {
				if (iCol <= 0 || iCol > pEList->nExpr) {
					const char *err =
						"Error at ORDER BY in place "\
						"%d: term out of range - "\
						"should be between 1 and %d";
					err = tt_sprintf(err, i + 1,
							 pEList->nExpr);
					diag_set(ClientError,
						 ER_SQL_PARSER_GENERIC, err);
					pParse->is_aborted = true;
					return 1;
				}
			} else {
				iCol = resolveAsName(pParse, pEList, pE);
				if (iCol == 0) {
					pDup = sqlExprDup(pE, 0);
					assert(pDup != NULL);
					iCol = resolveOrderByTermToExprList(
						pParse, pSelect, pDup);
					sql_expr_delete(pDup);
				}
			}
			if (iCol > 0) {
				/* Convert the ORDER BY term into an integer column number iCol,
				 * taking care to preserve the COLLATE clause if it exists
				 */
				struct Expr *pNew =
					sql_expr_new_anon(TK_INTEGER);
				pNew->v.u = iCol;
				pNew->type = FIELD_TYPE_INTEGER;
				if (pItem->pExpr == pE) {
					pItem->pExpr = pNew;
				} else {
					Expr *pParent = pItem->pExpr;
					assert(pParent->op == TK_COLLATE);
					while (pParent->pLeft->op == TK_COLLATE)
						pParent = pParent->pLeft;
					assert(pParent->pLeft == pE);
					pParent->pLeft = pNew;
				}
				sql_expr_delete(pE);
				pItem->u.x.iOrderByCol = (u16) iCol;
				pItem->done = 1;
			} else {
				moreToDo = 1;
			}
		}
		pSelect = pSelect->pNext;
	}
	for (i = 0; i < pOrderBy->nExpr; i++) {
		if (pOrderBy->a[i].done == 0) {
			const char *err = "Error at ORDER BY in place %d: "\
					  "term does not match any column in "\
					  "the result set";
			diag_set(ClientError, ER_SQL_PARSER_GENERIC,
				 tt_sprintf(err, i + 1));
			pParse->is_aborted = true;
			return 1;
		}
	}
	return 0;
}

/*
 * Check every term in the ORDER BY or GROUP BY clause pOrderBy of
 * the SELECT statement pSelect.  If any term is reference to a
 * result set expression (as determined by the ExprList.a.u.x.iOrderByCol
 * field) then convert that term into a copy of the corresponding result set
 * column.
 *
 * @retval 0 On success, not 0 elsewhere.
 */
int
sqlResolveOrderGroupBy(Parse * pParse,	/* Parsing context.  Leave error messages here */
			   Select * pSelect,	/* The SELECT statement containing the clause */
			   ExprList * pOrderBy,	/* The ORDER BY or GROUP BY clause to be processed */
			   const char *zType	/* "ORDER" or "GROUP" */
    )
{
	int i;
	ExprList *pEList;
	struct ExprList_item *pItem;

	if (pOrderBy == NULL)
		return 0;
	if (pOrderBy->nExpr > SQL_MAX_COLUMN) {
		const char *err = tt_sprintf("The number of terms in %s BY "\
					     "clause", zType);
		diag_set(ClientError, ER_SQL_PARSER_LIMIT, err,
			 pOrderBy->nExpr, SQL_MAX_COLUMN);
		pParse->is_aborted = true;
		return 1;
	}
	pEList = pSelect->pEList;
	assert(pEList != 0);	/* sqlSelectNew() guarantees this */
	for (i = 0, pItem = pOrderBy->a; i < pOrderBy->nExpr; i++, pItem++) {
		if (pItem->u.x.iOrderByCol) {
			if (pItem->u.x.iOrderByCol > pEList->nExpr) {
				const char *err = "Error at %s BY in place "\
						  "%d: term out of range - "\
						  "should be between 1 and %d";
				err = tt_sprintf(err, zType, i + 1,
						 pEList->nExpr);
				diag_set(ClientError, ER_SQL_PARSER_GENERIC,
					 err);
				pParse->is_aborted = true;
				return 1;
			}
			resolveAlias(pParse, pEList, pItem->u.x.iOrderByCol - 1,
				     pItem->pExpr, zType, 0);
			pParse->alias_count++;
			if (pParse->is_aborted)
				return 1;
		}
		/*
		 * Currently, we cannot disallow ANY here, as values of ANY type
		 * may be resolved later.
		 */
		enum field_type type = sql_expr_type(pItem->pExpr);
		if (!field_type1_contains_type2(FIELD_TYPE_SCALAR, NULL,
						type, NULL) &&
		    type != FIELD_TYPE_ANY) {
			diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
				 field_type_strs[type], "comparable type");
			pParse->is_aborted = true;
			return -1;
		}
	}
	return 0;
}

/*
 * pOrderBy is an ORDER BY or GROUP BY clause in SELECT statement pSelect.
 * The Name context of the SELECT statement is pNC.  zType is either
 * "ORDER" or "GROUP" depending on which type of clause pOrderBy is.
 *
 * This routine resolves each term of the clause into an expression.
 * If the order-by term is an integer I between 1 and N (where N is the
 * number of columns in the result set of the SELECT) then the expression
 * in the resolution is a copy of the I-th result-set expression.  If
 * the order-by term is an identifier that corresponds to the AS-name of
 * a result-set expression, then the term resolves to a copy of the
 * result-set expression.  Otherwise, the expression is resolved in
 * the usual way - using sqlResolveExprNames().
 *
 * @retval 0 On success, not 0 elsewhere.
 */
static int
resolveOrderGroupBy(NameContext * pNC,	/* The name context of the SELECT statement */
		    Select * pSelect,	/* The SELECT statement holding pOrderBy */
		    ExprList * pOrderBy,	/* An ORDER BY or GROUP BY clause to resolve */
		    const char *zType	/* Either "ORDER" or "GROUP", as appropriate */
    )
{
	int i, j;		/* Loop counters */
	int iCol;		/* Column number */
	struct ExprList_item *pItem;	/* A term of the ORDER BY clause */
	Parse *pParse;		/* Parsing context */
	int nResult;		/* Number of terms in the result set */

	if (pOrderBy == 0)
		return 0;
	nResult = pSelect->pEList->nExpr;
	pParse = pNC->pParse;
	for (i = 0, pItem = pOrderBy->a; i < pOrderBy->nExpr; i++, pItem++) {
		Expr *pE = pItem->pExpr;
		Expr *pE2 = sqlExprSkipCollate(pE);
		if (zType[0] != 'G') {
			iCol = resolveAsName(pParse, pSelect->pEList, pE2);
			if (iCol > 0) {
				/* If an AS-name match is found, mark this ORDER BY column as being
				 * a copy of the iCol-th result-set column.  The subsequent call to
				 * sqlResolveOrderGroupBy() will convert the expression to a
				 * copy of the iCol-th result-set expression.
				 */
				pItem->u.x.iOrderByCol = (u16) iCol;
				continue;
			}
		}
		if (sqlExprIsInteger(pE2, &iCol)) {
			/* The ORDER BY term is an integer constant.  Again, set the column
			 * number so that sqlResolveOrderGroupBy() will convert the
			 * order-by term to a copy of the result-set expression
			 */
			if (iCol < 1 || iCol > 0xffff) {
				const char *err = "Error at %s BY in place "\
						  "%d: term out of range - "\
						  "should be between 1 and %d";
				err = tt_sprintf(err, zType, i + 1, nResult);
				diag_set(ClientError, ER_SQL_PARSER_GENERIC,
					 err);
				pParse->is_aborted = true;
				return 1;
			}
			pItem->u.x.iOrderByCol = (u16) iCol;
			continue;
		}

		/* Otherwise, treat the ORDER BY term as an ordinary expression */
		pItem->u.x.iOrderByCol = 0;
		if (sqlResolveExprNames(pNC, pE)) {
			return 1;
		}
		for (j = 0; j < pSelect->pEList->nExpr; j++) {
			if (sqlExprCompare
			    (pE, pSelect->pEList->a[j].pExpr, -1) == 0) {
				pItem->u.x.iOrderByCol = j + 1;
			}
		}
	}
	return sqlResolveOrderGroupBy(pParse, pSelect, pOrderBy, zType);
}

/*
 * Resolve names in the SELECT statement p and all of its descendants.
 */
static int
resolveSelectStep(Walker * pWalker, Select * p)
{
	NameContext *pOuterNC;	/* Context that contains this SELECT */
	NameContext sNC;	/* Name context of this SELECT */
	int isCompound;		/* True if p is a compound select */
	int nCompound;		/* Number of compound terms processed so far */
	Parse *pParse;		/* Parsing context */
	int i;			/* Loop counter */
	ExprList *pGroupBy;	/* The GROUP BY clause */
	Select *pLeftmost;	/* Left-most of SELECT of a compound */

	assert(p != 0);
	if (p->selFlags & SF_Resolved) {
		return WRC_Prune;
	}
	pOuterNC = pWalker->u.pNC;
	pParse = pWalker->pParse;

	/* Normally sqlSelectExpand() will be called first and will have
	 * already expanded this SELECT.  However, if this is a subquery within
	 * an expression, sqlResolveExprNames() will be called without a
	 * prior call to sqlSelectExpand().  When that happens, let
	 * sqlSelectPrep() do all of the processing for this SELECT.
	 * sqlSelectPrep() will invoke both sqlSelectExpand() and
	 * this routine in the correct order.
	 */
	if ((p->selFlags & SF_Expanded) == 0) {
		sqlSelectPrep(pParse, p, pOuterNC);
		return pParse->is_aborted ? WRC_Abort : WRC_Prune;
	}

	isCompound = p->pPrior != 0;
	nCompound = 0;
	pLeftmost = p;
	while (p) {
		assert((p->selFlags & SF_Expanded) != 0);
		assert((p->selFlags & SF_Resolved) == 0);
		p->selFlags |= SF_Resolved;

		/* Resolve the expressions in the LIMIT and OFFSET clauses. These
		 * are not allowed to refer to any names, so pass an empty NameContext.
		 */
		memset(&sNC, 0, sizeof(sNC));
		sNC.pParse = pParse;
		if (sqlResolveExprNames(&sNC, p->pLimit) ||
		    sqlResolveExprNames(&sNC, p->pOffset)) {
			return WRC_Abort;
		}

		/* If the SF_Converted flags is set, then this Select object was
		 * was created by the convertCompoundSelectToSubquery() function.
		 * In this case the ORDER BY clause (p->pOrderBy) should be resolved
		 * as if it were part of the sub-query, not the parent. This block
		 * moves the pOrderBy down to the sub-query. It will be moved back
		 * after the names have been resolved.
		 */
		if (p->selFlags & SF_Converted) {
			Select *pSub = p->pSrc->a[0].pSelect;
			assert(p->pSrc->nSrc == 1 && p->pOrderBy);
			assert(pSub->pPrior && pSub->pOrderBy == 0);
			pSub->pOrderBy = p->pOrderBy;
			p->pOrderBy = 0;
		}

		/* Recursively resolve names in all subqueries
		 */
		for (i = 0; i < p->pSrc->nSrc; i++) {
			struct SrcList_item *pItem = &p->pSrc->a[i];
			if (pItem->pSelect) {
				NameContext *pNC;	/* Used to iterate name contexts */
				int nRef = 0;	/* Refcount for pOuterNC and outer contexts */

				/* Count the total number of references to pOuterNC and all of its
				 * parent contexts. After resolving references to expressions in
				 * pItem->pSelect, check if this value has changed. If so, then
				 * SELECT statement pItem->pSelect must be correlated. Set the
				 * pItem->fg.isCorrelated flag if this is the case.
				 */
				for (pNC = pOuterNC; pNC; pNC = pNC->pNext)
					nRef += pNC->nRef;

				sqlResolveSelectNames(pParse,
							  pItem->pSelect,
							  pOuterNC);
				if (pParse->is_aborted)
					return WRC_Abort;

				for (pNC = pOuterNC; pNC; pNC = pNC->pNext)
					nRef -= pNC->nRef;
				assert(pItem->fg.isCorrelated == 0
				       && nRef <= 0);
				pItem->fg.isCorrelated = (nRef != 0);
			}
		}

		/* Set up the local name-context to pass to sqlResolveExprNames() to
		 * resolve the result-set expression list.
		 */
		sNC.ncFlags = NC_AllowAgg;
		sNC.pSrcList = p->pSrc;
		sNC.pNext = pOuterNC;
		if (sqlResolveExprListNames(&sNC, p->pEList) != 0)
			return WRC_Abort;

		/*
		 * If there are no aggregate functions in the
		 * result-set, and no GROUP BY or HAVING
		 * expression, do not allow aggregates in any
		 * of the other expressions.
		 */
		assert((p->selFlags & SF_Aggregate) == 0);
		pGroupBy = p->pGroupBy;
		if (pGroupBy != NULL || p->pHaving != NULL ||
		    (sNC.ncFlags & NC_HasAgg) != 0) {
			assert(NC_MinMaxAgg == SF_MinMaxAgg);
			p->selFlags |=
			    SF_Aggregate | (sNC.ncFlags & NC_MinMaxAgg);
		} else {
			sNC.ncFlags &= ~NC_AllowAgg;
		}

		/*
		 * Add the output column list to the name-context
		 * before parsing the other expressions in the
		 * SELECT statement. This is so that expressions
		 * in the WHERE clause (etc.) can refer to
		 * expressions by aliases in the result set.
		 *
		 * Minor point: If this is the case, then the
		 * expression will be re-evaluated for each
		 * reference to it.
		 */
		sNC.pEList = p->pEList;
		/*
		 * If a HAVING clause is present, then there must
		 * be a GROUP BY clause or aggregate function
		 * should be specified.
		 */
		if (p->pHaving != NULL && pGroupBy == NULL) {
			sNC.ncFlags |= NC_AllowAgg;
			if (sqlResolveExprNames(&sNC, p->pHaving) != 0)
				return WRC_Abort;
			if ((sNC.ncFlags & NC_HasAgg) == 0 ||
			    (sNC.ncFlags & NC_HasUnaggregatedId) != 0) {
				diag_set(ClientError, ER_SQL_EXECUTE, "HAVING "
					 "argument must appear in the GROUP BY "
					 "clause or be used in an aggregate "
					 "function");
				pParse->is_aborted = true;
				return WRC_Abort;
			}
			/*
			 * Aggregate functions may return only
			 * one tuple, so user-defined LIMITs have
			 * no sense (most DBs don't support such
			 * LIMIT but there is no reason to
			 * restrict it directly).
			 */
			sql_expr_delete(p->pLimit);
			p->pLimit = sql_expr_new(TK_INTEGER, NULL);
			p->pLimit->v.u = 1;
			p->pLimit->type = FIELD_TYPE_INTEGER;
		} else {
			if (sqlResolveExprNames(&sNC, p->pHaving))
				return WRC_Abort;
		}
		if (sqlResolveExprNames(&sNC, p->pWhere))
			return WRC_Abort;

		/* The ORDER BY and GROUP BY clauses may not refer to terms in
		 * outer queries
		 */
		sNC.pNext = 0;
		sNC.ncFlags |= NC_AllowAgg;

		/* If this is a converted compound query, move the ORDER BY clause from
		 * the sub-query back to the parent query. At this point each term
		 * within the ORDER BY clause has been transformed to an integer value.
		 * These integers will be replaced by copies of the corresponding result
		 * set expressions by the call to resolveOrderGroupBy() below.
		 */
		if (p->selFlags & SF_Converted) {
			Select *pSub = p->pSrc->a[0].pSelect;
			p->pOrderBy = pSub->pOrderBy;
			pSub->pOrderBy = 0;
		}

		/* Process the ORDER BY clause for singleton SELECT statements.
		 * The ORDER BY clause for compounds SELECT statements is handled
		 * below, after all of the result-sets for all of the elements of
		 * the compound have been resolved.
		 *
		 * If there is an ORDER BY clause on a term of a compound-select other
		 * than the right-most term, then that is a syntax error.  But the error
		 * is not detected until much later, and so we need to go ahead and
		 * resolve those symbols on the incorrect ORDER BY for consistency.
		 */
		if (isCompound <= nCompound	/* Defer right-most ORDER BY of a compound */
		    && resolveOrderGroupBy(&sNC, p, p->pOrderBy, "ORDER")
		    ) {
			return WRC_Abort;
		}

		/* Resolve the GROUP BY clause.  At the same time, make sure
		 * the GROUP BY clause does not contain aggregate functions.
		 */
		if (pGroupBy) {
			struct ExprList_item *pItem;

			if (resolveOrderGroupBy(&sNC, p, pGroupBy,
						"GROUP") != 0)
				return WRC_Abort;
			const char *err_msg = "aggregate functions are not "\
					      "allowed in the GROUP BY clause";
			for (i = 0, pItem = pGroupBy->a; i < pGroupBy->nExpr;
			     i++, pItem++) {
				if (ExprHasProperty(pItem->pExpr, EP_Agg)) {
					diag_set(ClientError,
						 ER_SQL_PARSER_GENERIC,
						 err_msg);
					pParse->is_aborted = true;
					return WRC_Abort;
				}
			}
		}

		/* If this is part of a compound SELECT, check that it has the right
		 * number of expressions in the select list.
		 */
		if (p->pNext && p->pEList->nExpr != p->pNext->pEList->nExpr) {
			if (p->pNext->selFlags & SF_Values) {
				diag_set(ClientError, ER_SQL_PARSER_GENERIC,
					 "all VALUES must have the same "\
					 "number of terms");
			} else {
				const char *err =
					"SELECTs to the left and right of %s "\
					"do not have the same number of "\
					"result columns";
				const char *op =
					sql_select_op_name(p->pNext->op);
				diag_set(ClientError, ER_SQL_PARSER_GENERIC,
					 tt_sprintf(err, op));
			}
			pParse->is_aborted = true;
			return WRC_Abort;
		}
		/* Advance to the next term of the compound
		 */
		p = p->pPrior;
		nCompound++;
	}

	/* Resolve the ORDER BY on a compound SELECT after all terms of
	 * the compound have been resolved.
	 */
	if (isCompound && resolveCompoundOrderBy(pParse, pLeftmost)) {
		return WRC_Abort;
	}

	return WRC_Prune;
}

/*
 * This routine walks an expression tree and resolves references to
 * table columns and result-set columns.  At the same time, do error
 * checking on function usage and set a flag if any aggregate functions
 * are seen.
 *
 * To resolve table columns references we look for nodes (or subtrees) of the
 * form X.Y.Z or Y.Z or just Z where
 *
 *      X:   The name of a database.  Ex:  "main" or "temp" or
 *           the symbolic name assigned to an ATTACH-ed database.
 *
 *      Y:   The name of a table in a FROM clause.  Or in a trigger
 *           one of the special names "old" or "new".
 *
 *      Z:   The name of a column in table Y.
 *
 * The node at the root of the subtree is modified as follows:
 *
 *    Expr.op        Changed to TK_COLUMN_REF
 *    Expr.pTab      Points to the Table object for X.Y
 *    Expr.iColumn   The column index in X.Y.  -1 for the rowid.
 *    Expr.iTable    The VDBE cursor number for X.Y
 *
 *
 * To resolve result-set references, look for expression nodes of the
 * form Z (with no X and Y prefix) where the Z matches the right-hand
 * size of an AS clause in the result-set of a SELECT.  The Z expression
 * is replaced by a copy of the left-hand side of the result-set expression.
 * Table-name and function resolution occurs on the substituted expression
 * tree.  For example, in:
 *
 *      SELECT a+b AS x, c+d AS y FROM t1 ORDER BY x;
 *
 * The "x" term of the order by is replaced by "a+b" to render:
 *
 *      SELECT a+b AS x, c+d AS y FROM t1 ORDER BY a+b;
 *
 * Function calls are checked to make sure that the function is
 * defined and that the correct number of arguments are specified.
 * If the function is an aggregate function, then the NC_HasAgg flag is
 * set and the opcode is changed from TK_FUNCTION to TK_AGG_FUNCTION.
 * If an expression contains aggregate functions then the EP_Agg
 * property on the expression is set.
 *
 * An error message is left in pParse if anything is amiss.  The number
 * if errors is returned.
 */
int
sqlResolveExprNames(NameContext * pNC,	/* Namespace to resolve expressions in. */
			Expr * pExpr	/* The expression to be analyzed. */
    )
{
	u16 savedHasAgg;
	Walker w;

	if (pExpr == 0)
		return 0;
	if (sqlExprCheckHeight(pNC->pParse, pExpr->nHeight) != 0)
		return 1;
	savedHasAgg = pNC->ncFlags & (NC_HasAgg | NC_MinMaxAgg);
	pNC->ncFlags &= ~(NC_HasAgg | NC_MinMaxAgg);
	w.pParse = pNC->pParse;
	w.xExprCallback = resolveExprStep;
	w.xSelectCallback = resolveSelectStep;
	w.xSelectCallback2 = 0;
	w.walkerDepth = 0;
	w.eCode = 0;
	w.u.pNC = pNC;
	uint32_t alias_count = w.pParse->alias_count;
	sqlWalkExpr(&w, pExpr);
	if (w.pParse->alias_count != alias_count && !w.pParse->is_aborted) {
		sql_expr_update_height(pExpr);
		sqlExprCheckHeight(w.pParse, pExpr->nHeight);
	}
	if (pNC->nErr > 0 || w.pParse->is_aborted) {
		ExprSetProperty(pExpr, EP_Error);
	}
	if (pNC->ncFlags & NC_HasAgg) {
		ExprSetProperty(pExpr, EP_Agg);
	}
	pNC->ncFlags |= savedHasAgg;
	return ExprHasProperty(pExpr, EP_Error);
}

/*
 * Resolve all names for all expression in an expression list.  This is
 * just like sqlResolveExprNames() except that it works for an expression
 * list rather than a single expression.
 */
int
sqlResolveExprListNames(NameContext * pNC,	/* Namespace to resolve expressions in. */
			    ExprList * pList	/* The expression list to be analyzed. */
    )
{
	int i;
	if (pList) {
		for (i = 0; i < pList->nExpr; i++) {
			if (sqlResolveExprNames(pNC, pList->a[i].pExpr))
				return WRC_Abort;
		}
	}
	return WRC_Continue;
}

/*
 * Resolve all names in all expressions of a SELECT and in all
 * decendents of the SELECT, including compounds off of p->pPrior,
 * subqueries in expressions, and subqueries used as FROM clause
 * terms.
 *
 * See sqlResolveExprNames() for a description of the kinds of
 * transformations that occur.
 *
 * All SELECT statements should have been expanded using
 * sqlSelectExpand() prior to invoking this routine.
 */
void
sqlResolveSelectNames(Parse * pParse,	/* The parser context */
			  Select * p,	/* The SELECT statement being coded. */
			  NameContext * pOuterNC	/* Name context for parent SELECT statement */
    )
{
	Walker w;

	assert(p != 0);
	memset(&w, 0, sizeof(w));
	w.xExprCallback = resolveExprStep;
	w.xSelectCallback = resolveSelectStep;
	w.pParse = pParse;
	w.u.pNC = pOuterNC;
	sqlWalkSelect(&w, p);
}

void
sql_resolve_self_reference(struct Parse *parser, struct space_def *def,
			   struct Expr *expr)
{
	if (sqlExprSkipCollate(expr)->op != TK_ID) {
		diag_set(ClientError, ER_INDEX_DEF_UNSUPPORTED, "Expressions");
		parser->is_aborted = true;
		return;
	}

	/* Fake SrcList for parser->new_space */
	SrcList sSrc;
	/* Name context for parser->new_space  */
	NameContext sNC;

	memset(&sNC, 0, sizeof(sNC));
	memset(&sSrc, 0, sizeof(sSrc));
	sSrc.nSrc = 1;
	sSrc.a[0].zName = def->name;
	struct space tmp_space;
	memset(&tmp_space, 0, sizeof(tmp_space));
	tmp_space.def = def;
	sSrc.a[0].space = &tmp_space;
	sSrc.a[0].iCursor = -1;
	sNC.pParse = parser;
	sNC.pSrcList = &sSrc;
	sNC.ncFlags = NC_IdxExpr;
	sqlResolveExprNames(&sNC, expr);
}

int
sql_coll_id(uint32_t *id, const char *name, uint32_t len)
{
	char *name_str = sql_name_new(name, len);
	struct coll_id *coll_id = coll_by_name(name_str, strlen(name_str));
	sql_xfree(name_str);
	if (coll_id != NULL) {
		*id = coll_id->id;
		return 0;
	}
	if (name[0] != '"') {
		name_str = sql_legacy_name_new(name, len);
		coll_id = coll_by_name(name_str, strlen(name_str));
		sql_xfree(name_str);
		if (coll_id != NULL) {
			*id = coll_id->id;
			return 0;
		}
	}
	name_str = sql_name_new(name, len);
	diag_set(ClientError, ER_NO_SUCH_COLLATION, name_str);
	sql_xfree(name_str);
	return -1;
}

/**
 * Resolve an AST INTEGER expression into the given rast_expr.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_integer(struct sql_resolve_context *ctx, const struct ast_expr *ast,
		struct rast_expr *res)
{
	uint32_t used = region_used(ctx->region);
	char *str = xregion_alloc(ctx->region, ast->len + 1);
	memcpy(str, ast->str, ast->len);
	str[ast->len] = '\0';
	if (sql_uint_from_str(&res->u, str) != 0)
		return -1;
	region_truncate(ctx->region, used);
	res->type = SQL_TYPE_INTEGER;
	res->is_neg = false;
	res->height = 1;
	return 0;
}

/**
 * Fold a unary minus into a resolved INTEGER expression.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_integer_uminus(struct rast_expr *res)
{
	if (res->is_neg) {
		/* Compute -u in unsigned to avoid signed overflow. */
		res->u = ~res->u + 1;
		res->is_neg = false;
		return 0;
	}
	if (res->u == 0)
		return 0;
	int64_t value;
	if (sql_neg_uint(&value, res->u) != 0)
		return -1;
	res->u = (uint64_t)value;
	res->is_neg = true;
	return 0;
}

/**
 * Resolve an AST DECIMAL expression into the given rast_expr.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_decimal(struct sql_resolve_context *ctx, const struct ast_expr *ast,
		struct rast_expr *res)
{
	res->dec = xregion_alloc_object(ctx->region, typeof(*res->dec));
	uint32_t used = region_used(ctx->region);
	char *str = xregion_alloc(ctx->region, ast->len + 1);
	memcpy(str, ast->str, ast->len);
	str[ast->len] = '\0';
	if (sql_dec_from_str(res->dec, str) != 0)
		return -1;
	region_truncate(ctx->region, used);
	res->type = SQL_TYPE_DECIMAL;
	res->height = 1;
	return 0;
}

/**
 * Resolve an AST STRING expression into the given rast_expr. The string is
 * dequoted into region memory.
 */
static void
resolve_string(struct sql_resolve_context *ctx, const struct ast_expr *ast,
	       struct rast_expr *res)
{
	char *str = xregion_alloc(ctx->region, ast->len);
	memcpy(str, ast->str, ast->len);
	res->n = sql_dequote(str, ast->len);
	res->z = str;
	res->type = SQL_TYPE_STRING;
	res->height = 1;
}

/**
 * Resolve an AST BLOB expression into the given rast_expr. The hex literal is
 * decoded into region memory.
 */
static void
resolve_varbinary(struct sql_resolve_context *ctx, const struct ast_expr *ast,
		  struct rast_expr *res)
{
	assert(ast->str[0] == 'x' || ast->str[0] == 'X');
	assert(ast->str[1] == '\'' && ast->str[ast->len - 1] == '\'');
	assert(ast->len > 2 && ast->len % 2 == 1);
	uint32_t n = (ast->len - 3) / 2;
	char *z = NULL;
	if (n > 0) {
		z = xregion_alloc(ctx->region, n);
		for (uint32_t i = 0; i < n; ++i) {
			z[i] = sqlHexToInt(ast->str[2 + i * 2]) << 4 |
			       sqlHexToInt(ast->str[3 + i * 2]);
		}
	}
	res->z = z;
	res->n = n;
	res->type = SQL_TYPE_VARBINARY;
	res->height = 1;
}

/**
 * Resolve an AST RAISE expression into the given rast_expr. The error message,
 * if any, is dequoted into region memory.
 */
static void
resolve_raise(struct sql_resolve_context *ctx, const struct ast_expr *ast,
	      struct rast_expr *res)
{
	res->action = ast->on_conflict_action;
	if (res->action != ON_CONFLICT_ACTION_IGNORE) {
		char *str = xregion_alloc(ctx->region, ast->left->len);
		memcpy(str, ast->left->str, ast->left->len);
		res->raise_len = sql_dequote(str, ast->left->len);
		res->raise = str;
	}
	res->height = 1;
	res->type = SQL_TYPE_UNKNOWN;
}

/**
 * Check that the given expression tree height is less than or equal to the
 * maximum expression depth allowed. If it is not, set a diag.
 *
 * Returns 0 on success and -1 on error.
 */
static int
expr_check_height(int height)
{
	if (height <= SQL_MAX_EXPR_DEPTH)
		return 0;
	diag_set(ClientError, ER_SQL_PARSER_LIMIT, "Number of nodes "
		 "in expression tree", height, SQL_MAX_EXPR_DEPTH);
	return -1;
}

/**
 * Return true if the given AST expression is a constant FALSE, matching the
 * folding done by sql_and_expr_new()/exprAlwaysFalse() at expression creation:
 * a FALSE literal, possibly parenthesized, or an AND with a constant-FALSE
 * operand.
 */
static bool
ast_is_false(const struct ast_expr *ast)
{
	while (ast->op == TK_PARENTHESES)
		ast = ast->left;
	if (ast->op == TK_FALSE)
		return true;
	if (ast->op == TK_AND)
		return ast_is_false(ast->left) || ast_is_false(ast->right);
	return false;
}

/**
 * Number of columns in a resolved expression: the length of a row-value VECTOR,
 * the number of columns of a scalar subquery, or 1 otherwise. EXISTS is a
 * scalar and so has a single column.
 */
static uint32_t
rast_vector_size(const struct rast_expr *res)
{
	if (res->op == TK_VECTOR)
		return res->list.len;
	if (res->op == TK_SELECT)
		return res->select->columns.len;
	return 1;
}

/**
 * Maximum height among the expressions of a resolved SELECT and of the SELECTs
 * to the left of it in a compound SELECT.
 */
static uint32_t
rast_select_height(const struct rast_select *select)
{
	uint32_t height = 0;
	for (const struct rast_select *p = select; p != NULL; p = p->prior) {
		for (uint32_t i = 0; i < p->columns.len; ++i)
			height = MAX(height, p->columns.exprs[i].expr.height);
		if (p->limit != NULL)
			height = MAX(height, p->limit->height);
		if (p->offset != NULL)
			height = MAX(height, p->offset->height);
		if (p->where != NULL)
			height = MAX(height, p->where->height);
		if (p->having != NULL)
			height = MAX(height, p->having->height);
		for (uint32_t i = 0; i < p->group_by.len; ++i)
			height = MAX(height, p->group_by.exprs[i].expr.height);
		for (uint32_t i = 0; i < p->order_by.len; ++i)
			height = MAX(height, p->order_by.exprs[i].expr.height);
	}
	return height;
}

/**
 * Resolve the operands of an AST binary expression into the given rast_expr
 * and set its height. The result type is left to the caller.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_binary(struct sql_resolve_context *ctx, const struct ast_expr *ast,
	       struct rast_expr *res)
{
	res->left = xregion_alloc_object(ctx->region, struct rast_expr);
	if (resolve_expr(ctx, ast->left, res->left) != 0)
		return -1;
	if (!ctx->can_resolve)
		return 0;
	res->right = xregion_alloc_object(ctx->region, struct rast_expr);
	if (resolve_expr(ctx, ast->right, res->right) != 0)
		return -1;
	if (!ctx->can_resolve)
		return 0;
	res->height = MAX(res->left->height, res->right->height) + 1;
	return expr_check_height(res->height);
}

/**
 * Resolve the operands of an AST BETWEEN expression into the given rast_expr.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_between(struct sql_resolve_context *ctx, const struct ast_expr *ast,
		struct rast_expr *res)
{
	res->between.value =
		xregion_alloc_object(ctx->region, struct rast_expr);
	if (resolve_expr(ctx, ast->left, res->between.value) != 0)
		return -1;
	if (!ctx->can_resolve)
		return 0;
	assert(ast->list->len == 2);
	const struct stailq_entry *node = ast->list->head.first.value;
	const struct stailq_entry *next = node->next.value;
	const struct ast_expr_list_entry *lower =
		stailq_entry(node, struct ast_expr_list_entry, link);
	const struct ast_expr_list_entry *upper =
		stailq_entry(next, struct ast_expr_list_entry, link);
	res->between.lower =
		xregion_alloc_object(ctx->region, struct rast_expr);
	if (resolve_expr(ctx, lower->expr, res->between.lower) != 0)
		return -1;
	if (!ctx->can_resolve)
		return 0;
	res->between.upper =
		xregion_alloc_object(ctx->region, struct rast_expr);
	if (resolve_expr(ctx, upper->expr, res->between.upper) != 0)
		return -1;
	if (!ctx->can_resolve)
		return 0;
	uint32_t n_left = rast_vector_size(res->between.value);
	uint32_t n_right = rast_vector_size(res->between.lower);
	if (n_right == n_left)
		n_right = rast_vector_size(res->between.upper);
	if (n_left != n_right) {
		diag_set(ClientError, ER_SQL_COLUMN_COUNT, n_left, n_right);
		return -1;
	}
	res->height = MAX(res->between.value->height,
			  MAX(res->between.lower->height,
			      res->between.upper->height)) + 1;
	return expr_check_height(res->height);
}

/**
 * Resolve every expression of the given AST list into the pre-allocated array
 * of rast_expr, which must have room for the whole list. The maximum height
 * among the entries is returned via height.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_operand_list(struct sql_resolve_context *ctx,
		     const struct ast_expr_list *ast, struct rast_expr *exprs,
		     uint32_t *height)
{
	uint32_t max = 0;
	uint32_t i = 0;
	for (const struct stailq_entry *node = ast->head.first.value;
	     node != NULL; node = node->next.value) {
		const struct ast_expr_list_entry *entry =
			stailq_entry(node, struct ast_expr_list_entry, link);
		if (resolve_expr(ctx, entry->expr, &exprs[i]) != 0)
			return -1;
		if (!ctx->can_resolve)
			return 0;
		max = MAX(max, exprs[i].height);
		i++;
	}
	*height = max;
	return 0;
}

/**
 * Resolve an AST IN expression into the given rast_expr. Its right-hand side is
 * either a subquery, which is not resolvable yet, or a list of values, which
 * the legacy expression creation folds: an empty list becomes a constant FALSE
 * with the left operand not evaluated, a single-value list becomes an equality,
 * and a longer list stays an IN over the value list. The result type is
 * BOOLEAN.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_in(struct sql_resolve_context *ctx, const struct ast_expr *ast,
	   struct rast_expr *res)
{
	const struct ast_expr *rhs = ast->right;
	if (rhs->op == TK_SELECT) {
		res->in.value =
			xregion_alloc_object(ctx->region, struct rast_expr);
		if (resolve_expr(ctx, ast->left, res->in.value) != 0)
			return -1;
		if (!ctx->can_resolve)
			return 0;
		res->in.select =
			xregion_alloc_object(ctx->region, struct rast_select);
		if (resolve_nested_select(ctx, rhs->select,
					  res->in.select) != 0)
			return -1;
		if (!ctx->can_resolve)
			return 0;
		res->type = SQL_TYPE_BOOLEAN;
		res->height = MAX(res->in.value->height,
				  rast_select_height(res->in.select)) + 1;
		return expr_check_height(res->height);
	}
	assert(rhs->op == TK_VECTOR);
	uint32_t len = rhs->list == NULL ? 0 : rhs->list->len;
	if (len == 0) {
		res->op = TK_FALSE;
		res->b = false;
		res->type = SQL_TYPE_BOOLEAN;
		res->height = 1;
		return 0;
	}
	if (len == 1) {
		res->op = TK_EQ;
		res->left = xregion_alloc_object(ctx->region, struct rast_expr);
		if (resolve_expr(ctx, ast->left, res->left) != 0)
			return -1;
		if (!ctx->can_resolve)
			return 0;
		const struct stailq_entry *node = rhs->list->head.first.value;
		const struct ast_expr_list_entry *entry =
			stailq_entry(node, struct ast_expr_list_entry, link);
		res->right =
			xregion_alloc_object(ctx->region, struct rast_expr);
		if (resolve_expr(ctx, entry->expr, res->right) != 0)
			return -1;
		if (!ctx->can_resolve)
			return 0;
		res->type = SQL_TYPE_BOOLEAN;
		res->height = MAX(res->left->height, res->right->height) + 1;
		return expr_check_height(res->height);
	}
	res->in.value = xregion_alloc_object(ctx->region, struct rast_expr);
	if (resolve_expr(ctx, ast->left, res->in.value) != 0)
		return -1;
	if (!ctx->can_resolve)
		return 0;
	res->in.len = rhs->list->len;
	res->in.exprs = xregion_alloc_array(ctx->region, struct rast_expr,
					    res->in.len);
	uint32_t height;
	if (resolve_operand_list(ctx, rhs->list, res->in.exprs, &height) != 0)
		return -1;
	if (!ctx->can_resolve)
		return 0;
	res->type = SQL_TYPE_BOOLEAN;
	res->height = MAX(res->in.value->height, height) + 1;
	return expr_check_height(res->height);
}

/**
 * Resolve an AST CASE expression into the given rast_expr. The optional operand
 * is followed by the WHEN and THEN expressions in pairs and the optional ELSE
 * expression, all held in the AST list. The result type is left UNKNOWN and is
 * computed from the branches when it is needed, as the legacy code does.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_case(struct sql_resolve_context *ctx, const struct ast_expr *ast,
	     struct rast_expr *res)
{
	uint32_t value_height = 0;
	if (ast->left != NULL) {
		res->cs.value =
			xregion_alloc_object(ctx->region, struct rast_expr);
		if (resolve_expr(ctx, ast->left, res->cs.value) != 0)
			return -1;
		if (!ctx->can_resolve)
			return 0;
		value_height = res->cs.value->height;
	}
	res->cs.len = ast->list->len;
	res->cs.exprs = xregion_alloc_array(ctx->region, struct rast_expr,
					    res->cs.len);
	uint32_t height;
	if (resolve_operand_list(ctx, ast->list, res->cs.exprs, &height) != 0)
		return -1;
	if (!ctx->can_resolve)
		return 0;
	if (res->cs.value != NULL) {
		uint32_t n_value = rast_vector_size(res->cs.value);
		for (uint32_t i = 0; i + 1 < res->cs.len; i += 2) {
			uint32_t n_when = rast_vector_size(&res->cs.exprs[i]);
			if (n_when != n_value) {
				diag_set(ClientError, ER_SQL_COLUMN_COUNT,
					 n_value, n_when);
				return -1;
			}
		}
	}
	res->height = MAX(value_height, height) + 1;
	return expr_check_height(res->height);
}

/**
 * Resolve an AST ARRAY or MAP constructor, or a row-value VECTOR, into the
 * given rast_expr and set its height. The elements are held in the AST list,
 * which is empty for an empty constructor. A map holds its keys and values in
 * pairs. The result type is left to the caller.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_constructor(struct sql_resolve_context *ctx,
		    const struct ast_expr *ast, struct rast_expr *res)
{
	res->list.len = ast->list == NULL ? 0 : ast->list->len;
	if (res->list.len == 0) {
		res->height = 1;
		return 0;
	}
	res->list.exprs = xregion_alloc_array(ctx->region, struct rast_expr,
					      res->list.len);
	uint32_t height;
	if (resolve_operand_list(ctx, ast->list, res->list.exprs, &height) != 0)
		return -1;
	if (!ctx->can_resolve)
		return 0;
	res->height = height + 1;
	return expr_check_height(res->height);
}

/**
 * Resolve an AST subscript expression into the given rast_expr and set its
 * height. The base value being subscripted is held in the AST left operand and
 * the keys in the AST list, which always holds at least one key. The result
 * type is always ANY, as the legacy code and sql_expr_type() report. Whether
 * the base is a map or an array and whether the keys are valid is checked later
 * at bytecode generation, exactly as the legacy path does.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_getitem(struct sql_resolve_context *ctx, const struct ast_expr *ast,
		struct rast_expr *res)
{
	res->getitem.value =
		xregion_alloc_object(ctx->region, struct rast_expr);
	if (resolve_expr(ctx, ast->left, res->getitem.value) != 0)
		return -1;
	if (!ctx->can_resolve)
		return 0;
	res->getitem.len = ast->list->len;
	res->getitem.exprs = xregion_alloc_array(ctx->region, struct rast_expr,
						 res->getitem.len);
	uint32_t height;
	if (resolve_operand_list(ctx, ast->list, res->getitem.exprs,
				 &height) != 0)
		return -1;
	if (!ctx->can_resolve)
		return 0;
	res->type = SQL_TYPE_ANY;
	res->height = MAX(res->getitem.value->height, height) + 1;
	return expr_check_height(res->height);
}

/**
 * Set the given rast_expr to a reference to the given result column of a
 * SELECT the given number of subqueries up.
 */
static void
resolve_alias(const struct rast_expr_list *columns, uint32_t col,
	      uint32_t depth, struct rast_expr *res)
{
	assert(col < columns->len);
	const struct rast_expr *expr = &columns->exprs[col].expr;
	res->op = TK_AS;
	res->alias.expr = expr;
	res->alias.depth = depth;
	res->type = expr->type;
	res->height = expr->height;
}

/**
 * Find the result column with the given alias among the given ones. The name
 * matches the alias as is or, as in the legacy code, in its legacy form.
 *
 * Returns the position of the column, starting from 0, or -1 if not found.
 */
static int
find_alias(const struct rast_expr_list *columns, const char *name,
	   const char *legacy_name)
{
	for (uint32_t i = 0; i < columns->len; ++i) {
		const char *alias = columns->exprs[i].name;
		if (alias != NULL && (strcmp(alias, name) == 0 ||
				      (legacy_name != NULL &&
				       strcmp(alias, legacy_name) == 0)))
			return i;
	}
	return -1;
}

/**
 * Return the name of the given source: its alias, if any, or the name of its
 * space.
 */
static const char *
rast_source_name(const struct rast_source *src)
{
	if (src->alias != NULL)
		return src->alias;
	struct space *space = space_by_id(src->space_id);
	assert(space != NULL);
	return space->def->name;
}

/**
 * Find the field with the given name in the given source. A qualified name
 * matches only the source with the given name, which is its alias, if any, or
 * the name of its space. As in the legacy code, an unquoted name is also looked
 * up in its legacy form.
 *
 * Returns the number of the field, or UINT32_MAX if not found.
 */
static uint32_t
find_source_field(const struct rast_source *src, const char *tab_name,
		  const char *legacy_tab_name, const char *name,
		  const char *legacy_name)
{
	struct space *space = space_by_id(src->space_id);
	assert(space != NULL);
	if (tab_name != NULL) {
		const char *src_name = rast_source_name(src);
		if (strcmp(src_name, tab_name) != 0 &&
		    (legacy_tab_name == NULL ||
		     strcmp(src_name, legacy_tab_name) != 0))
			return UINT32_MAX;
	}
	uint32_t fieldno = sql_space_fieldno(space, name);
	if (fieldno == UINT32_MAX && legacy_name != NULL)
		fieldno = sql_space_fieldno(space, legacy_name);
	return fieldno;
}

/**
 * Set the given rast_expr to a reference to the given field of the given source
 * of the SELECT of the given scope, which is the given number of subqueries up
 * from the current scope, and count the reference.
 */
static void
resolve_source_column(struct sql_resolve_context *ctx,
		      const struct sql_resolve_scope *scope, uint32_t depth,
		      uint32_t source, uint32_t fieldno, struct rast_expr *res)
{
	for (struct sql_resolve_scope *s = ctx->scope; s != scope->parent;
	     s = s->parent)
		++s->ref_count;
	ctx->scope->ref_depth = MIN(ctx->scope->ref_depth, depth);
	const struct rast_source *src = &scope->sources->sources[source];
	struct space *space = space_by_id(src->space_id);
	assert(space != NULL && fieldno < space->def->field_count);
	res->op = TK_COLUMN_REF;
	res->column.depth = depth;
	res->column.source = source;
	res->column.fieldno = fieldno;
	res->column.field_type = space->def->fields[fieldno].type;
	res->type = sql_type_from_field_type(res->column.field_type);
}

/**
 * Resolve an AST column reference: a column name, optionally qualified with a
 * source name, into the given rast_expr. As in the legacy code, the name is
 * looked up in the scopes from the current one outwards; in every scope first
 * among the columns of the sources, then, if the name is not qualified, among
 * the aliases of the result columns. If the reference is not resolved, the
 * error is reported with the same message as the legacy code does.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_column_ref(struct sql_resolve_context *ctx, const struct ast_expr *ast,
		   struct rast_expr *res)
{
	const char *tab_name = NULL;
	const char *legacy_tab_name = NULL;
	const struct ast_expr *col = ast;
	if (ast->op == TK_DOT) {
		assert(ast->left->op == TK_ID && ast->right->op == TK_ID);
		const struct ast_expr *tab = ast->left;
		tab_name = sql_name_temp(ctx->region, tab->str, tab->len);
		if (tab->str[0] != '"') {
			legacy_tab_name = sql_legacy_name_temp(ctx->region,
							       tab->str,
							       tab->len);
		}
		col = ast->right;
	} else {
		assert(ast->op == TK_ID);
		/* As in the legacy code, only unqualified names count. */
		if (ctx->scope->is_agg_allowed)
			ctx->scope->has_unaggregated_name = true;
	}
	const char *name = sql_name_temp(ctx->region, col->str, col->len);
	const char *legacy_name = NULL;
	if (col->str[0] != '"') {
		legacy_name = sql_legacy_name_temp(ctx->region, col->str,
						   col->len);
	}
	/* As in the legacy code, a qualified name is a node of height 2. */
	res->height = ast->op == TK_DOT ? 2 : 1;
	uint32_t depth = 0;
	for (struct sql_resolve_scope *scope = ctx->scope;
	     scope != NULL; scope = scope->parent, ++depth) {
		const struct rast_source_list *sources = scope->sources;
		for (uint32_t i = 0; sources != NULL && i < sources->len; ++i) {
			uint32_t fieldno =
				find_source_field(&sources->sources[i],
						  tab_name, legacy_tab_name,
						  name, legacy_name);
			if (fieldno == UINT32_MAX)
				continue;
			resolve_source_column(ctx, scope, depth, i, fieldno,
					      res);
			return 0;
		}
		if (tab_name != NULL || scope->aliases == NULL)
			continue;
		int pos = find_alias(scope->aliases, name, legacy_name);
		if (pos < 0)
			continue;
		const struct rast_expr_list_entry *entry =
			&scope->aliases->exprs[pos];
		if (entry->has_agg && !scope->is_agg_allowed) {
			diag_set(ClientError, ER_SQL_PARSER_GENERIC,
				 tt_sprintf("misuse of aliased aggregate %s",
					    entry->name));
			return -1;
		}
		if (rast_vector_size(&entry->expr) != 1) {
			diag_set(ClientError, ER_SQL_PARSER_GENERIC,
				 "row value misused");
			return -1;
		}
		for (struct sql_resolve_scope *s = ctx->scope;
		     s != scope->parent; s = s->parent)
			++s->ref_count;
		if (entry->has_agg)
			scope->agg_flags |= SF_Aggregate;
		/*
		 * A reference to a result column is a copy of its expression,
		 * so the references of the expression are taken into account.
		 */
		if (entry->ref_depth != UINT32_MAX) {
			ctx->scope->ref_depth = MIN(ctx->scope->ref_depth,
						    entry->ref_depth + depth);
		}
		resolve_alias(scope->aliases, pos, depth, res);
		return 0;
	}
	if (tab_name == NULL)
		diag_set(ClientError, ER_SQL_CANT_RESOLVE_FIELD, name);
	else
		diag_set(ClientError, ER_NO_SUCH_FIELD_NAME_IN_SPACE, name,
			 tab_name);
	return -1;
}

/**
 * Resolve an AST scalar subquery or EXISTS expression into the given rast_expr
 * and set its height. The subquery is resolvable only when it is itself a
 * resolvable FROM-less SELECT; otherwise the whole expression falls back. The
 * result type is left unknown and is computed from the subquery when needed, as
 * the legacy code does. The result type is left to the caller.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_subquery(struct sql_resolve_context *ctx, const struct ast_expr *ast,
		 struct rast_expr *res)
{
	bool outer = ctx->can_resolve;
	res->select = xregion_alloc_object(ctx->region, struct rast_select);
	if (resolve_nested_select(ctx, ast->select, res->select) != 0)
		return -1;
	ctx->can_resolve = ctx->can_resolve && outer;
	if (!ctx->can_resolve)
		return 0;
	res->height = rast_select_height(res->select) + 1;
	return expr_check_height(res->height);
}

/**
 * Return the exact type of the given resolved expression. It mirrors
 * sql_expr_type() applied to the expression built from the resolved one, since
 * the type of some expressions is computed from their operands only when it is
 * needed.
 */
static enum field_type
rast_expr_field_type(const struct rast_expr *expr);

/**
 * Determine the highest type between the given type and the type of the given
 * resolved CASE branch. Mirrors sql_highest_field_type().
 */
static enum field_type
rast_highest_field_type(enum field_type type, const struct rast_expr *expr)
{
	if (type == FIELD_TYPE_ANY)
		return FIELD_TYPE_ANY;
	if (expr->op == TK_NULL)
		return type;
	return sql_field_type_highest(type, rast_expr_field_type(expr));
}

/** Return the exact type of the given resolved CASE expression. */
static enum field_type
rast_case_field_type(const struct rast_expr *expr)
{
	const struct rast_expr *exprs = expr->cs.exprs;
	uint32_t len = expr->cs.len;
	/*
	 * The WHEN and THEN expressions go in pairs, so the THEN expressions
	 * have odd indexes.
	 */
	uint32_t i = 1;
	while (i < len && exprs[i].op == TK_NULL)
		i += 2;
	if (i >= len)
		return FIELD_TYPE_ANY;
	enum field_type type = rast_expr_field_type(&exprs[i]);
	for (i += 2; i < len; i += 2)
		type = rast_highest_field_type(type, &exprs[i]);
	/* The ELSE expression is optional. */
	if (len % 2 == 0)
		return type;
	return rast_highest_field_type(type, &exprs[len - 1]);
}

static enum field_type
rast_expr_field_type(const struct rast_expr *expr)
{
	while (expr->op == TK_COLLATE ||
	       (expr->op == TK_FUNCTION && expr->func.is_unlikely)) {
		if (expr->op == TK_COLLATE)
			expr = expr->coll.expr;
		else
			expr = &expr->func.args[0];
	}
	switch (expr->op) {
	case TK_SELECT: {
		const struct rast_expr_list *columns = &expr->select->columns;
		return rast_expr_field_type(&columns->exprs[0].expr);
	}
	case TK_PLUS:
	case TK_MINUS:
	case TK_STAR:
	case TK_SLASH:
	case TK_REM:
	case TK_BITAND:
	case TK_BITOR:
	case TK_LSHIFT:
	case TK_RSHIFT:
		return sql_field_type_result(rast_expr_field_type(expr->right),
					     rast_expr_field_type(expr->left));
	case TK_GETITEM:
		return FIELD_TYPE_ANY;
	case TK_CONCAT:
		return FIELD_TYPE_STRING;
	case TK_CASE:
		return rast_case_field_type(expr);
	case TK_LT:
	case TK_GT:
	case TK_EQ:
	case TK_LE:
	case TK_GE:
	case TK_NE:
	case TK_NOT:
	case TK_AND:
	case TK_OR:
	case TK_ISNULL:
	case TK_NOTNULL:
	case TK_BETWEEN:
	case TK_EXISTS:
	case TK_IN:
		return FIELD_TYPE_BOOLEAN;
	case TK_UMINUS:
	case TK_UPLUS:
	case TK_BITNOT:
		return rast_expr_field_type(expr->expr);
	case TK_FUNCTION:
		return expr->func.returns;
	case TK_AS:
		return rast_expr_field_type(expr->alias.expr);
	case TK_COLUMN_REF:
		return expr->column.field_type;
	}
	return sql_type_to_field_type(expr->type);
}

/**
 * Return the probability of the first argument of LIKELIHOOD() to be true,
 * scaled by 2^27, or -1 if the second argument is not a constant between 0.0
 * and 1.0. The second argument is given both in the AST and in the resolved
 * form. Mirrors exprProbability(): a literal with a unary operator is not a
 * constant there, so the AST is checked, since the resolved form folds it.
 */
static int
rast_probability(const struct ast_expr *ast, const struct rast_expr *expr)
{
	while (ast->op == TK_PARENTHESES)
		ast = ast->left;
	if (ast->op != TK_FLOAT)
		return -1;
	assert(expr->op == TK_FLOAT);
	if (expr->f < 0.0 || expr->f > 1.0)
		return -1;
	return (int)(expr->f * 134217728.0);
}

/**
 * Resolve an AST function call into the given rast_expr. The name is held in
 * the AST left operand and the arguments in the list of the AST right operand,
 * which is NULL for a call with an asterisk. The function is looked up by the
 * name and the types of the resolved arguments, and its result type becomes the
 * type of the expression.
 *
 * The arguments are resolved before any check of the function itself, so an
 * error in an argument is reported instead of an error of the function. An
 * aggregate function is not allowed inside the arguments of another aggregate
 * function.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_function(struct sql_resolve_context *ctx, const struct ast_expr *ast,
		 struct rast_expr *res)
{
	const struct ast_expr *name = ast->left;
	res->func.name = sql_name_temp(ctx->region, name->str, name->len);
	bool is_legacy = name->str[0] != '"';
	const struct ast_expr_list *list = NULL;
	if (ast->right != NULL) {
		res->func.is_distinct = ast->right->op == TK_DISTINCT;
		list = ast->right->list;
	}
	res->func.n_args = list == NULL ? 0 : list->len;
	if (res->func.n_args > SQL_MAX_FUNCTION_ARG) {
		const char *err = tt_sprintf("Number of arguments to "
					     "function %s", res->func.name);
		diag_set(ClientError, ER_SQL_PARSER_LIMIT, err,
			 res->func.n_args, SQL_MAX_FUNCTION_ARG);
		return -1;
	}
	uint32_t flags = sql_func_flags_by_name(res->func.name, is_legacy);
	res->func.is_agg = (flags & SQL_FUNC_AGG) != 0;
	uint32_t height = 0;
	if (res->func.n_args > 0) {
		res->func.args = xregion_alloc_array(ctx->region,
						     struct rast_expr,
						     res->func.n_args);
		bool is_agg_allowed = ctx->scope->is_agg_allowed;
		if (res->func.is_agg)
			ctx->scope->is_agg_allowed = false;
		uint32_t ref_depth = ctx->scope->ref_depth;
		ctx->scope->ref_depth = UINT32_MAX;
		int rc = resolve_operand_list(ctx, list, res->func.args,
					      &height);
		ctx->scope->is_agg_allowed = is_agg_allowed;
		/*
		 * As in the legacy code, an aggregate function belongs to the
		 * nearest SELECT whose sources its arguments refer to, if any.
		 */
		if (res->func.is_agg && ctx->scope->ref_depth != UINT32_MAX)
			res->func.agg_depth = ctx->scope->ref_depth;
		ctx->scope->ref_depth = MIN(ctx->scope->ref_depth, ref_depth);
		if (rc != 0)
			return -1;
		if (!ctx->can_resolve)
			return 0;
	}
	if ((flags & SQL_FUNC_UNLIKELY) != 0) {
		res->func.is_unlikely = true;
		if (res->func.n_args == 2) {
			const struct stailq_entry *node =
				list->head.first.value->next.value;
			const struct ast_expr_list_entry *entry =
				stailq_entry(node, struct ast_expr_list_entry,
					     link);
			res->func.probability =
				rast_probability(entry->expr,
						 &res->func.args[1]);
			if (res->func.probability < 0) {
				diag_set(IllegalParams, "second argument to "
					 "LIKELIHOOD() must be a constant "
					 "between 0.0 and 1.0");
				return -1;
			}
		} else {
			/* unlikely() is 0.0625, likely() is 0.9375. */
			char c = res->func.name[0];
			res->func.probability = c == 'u' || c == 'U' ?
						8388608 : 125829120;
		}
	}
	struct sql_resolve_scope *owner = ctx->scope;
	for (uint32_t i = 0; i < res->func.agg_depth; ++i)
		owner = owner->parent;
	/*
	 * Unlike the legacy code, which reports it only when generating the
	 * code, an aggregate function is also rejected where the SELECT it
	 * belongs to does not allow it.
	 */
	if (res->func.is_agg &&
	    (!ctx->scope->is_agg_allowed || !owner->is_agg_allowed)) {
		diag_set(ClientError, ER_SQL_PARSER_GENERIC,
			 tt_sprintf("misuse of aggregate function %s()",
				    res->func.name));
		return -1;
	}
	struct sql_func_arg *args = NULL;
	if (res->func.n_args > 0) {
		args = xregion_alloc_array(ctx->region, struct sql_func_arg,
					   res->func.n_args);
	}
	for (uint32_t i = 0; i < res->func.n_args; ++i) {
		const struct rast_expr *arg = &res->func.args[i];
		while (arg->op == TK_COLLATE)
			arg = arg->coll.expr;
		args[i].op = arg->op;
		args[i].type = rast_expr_field_type(arg);
	}
	struct func *func = sql_func_find_by_name(res->func.name, is_legacy,
						  args, res->func.n_args);
	if (func == NULL)
		return -1;
	/* The name of the found function is looked up exactly from now on. */
	size_t len = strlen(func->def->name);
	res->func.name = xregion_alloc(ctx->region, len + 1);
	memcpy(res->func.name, func->def->name, len + 1);
	res->func.returns = func->def->returns;
	/*
	 * The result type of a user-defined aggregate function is the result
	 * type of its FINALIZE part.
	 */
	if (func->def->language != FUNC_LANGUAGE_SQL_BUILTIN &&
	    func->def->aggregate == FUNC_AGGREGATE_GROUP) {
		struct func *finalize = sql_func_finalize(res->func.name);
		if (finalize != NULL)
			res->func.returns = finalize->def->returns;
	}
	res->func.is_deterministic = func->def->is_deterministic;
	if (res->func.is_agg) {
		owner->agg_flags |= SF_Aggregate;
		if ((flags & (SQL_FUNC_MIN | SQL_FUNC_MAX)) != 0)
			owner->agg_flags |= SF_MinMaxAgg;
	}
	res->type = sql_type_from_field_type(res->func.returns);
	res->height = height + 1;
	return expr_check_height(res->height);
}

/**
 * Turn the given resolved operand of a unary plus or minus into an operand of
 * a new expression of the given operation built in its place. Used when the
 * operand is not a numeric literal the operator can be folded into.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_unary_arith(struct rast_expr *res, uint8_t op, struct region *region)
{
	struct rast_expr *expr = xregion_alloc_object(region, struct rast_expr);
	*expr = *res;
	res->expr = expr;
	res->op = op;
	res->type = expr->type;
	res->height = expr->height + 1;
	return expr_check_height(res->height);
}

static int
resolve_expr(struct sql_resolve_context *ctx, const struct ast_expr *ast,
	     struct rast_expr *res)
{
	memset(res, 0, sizeof(*res));
	res->op = ast->op;
	switch (ast->op) {
	case TK_PARENTHESES:
		while (ast->op == TK_PARENTHESES)
			ast = ast->left;
		return resolve_expr(ctx, ast, res);
	case TK_NULL:
		res->type = SQL_TYPE_SCALAR;
		res->height = 1;
		break;
	case TK_TRUE:
		res->b = true;
		res->type = SQL_TYPE_BOOLEAN;
		res->height = 1;
		break;
	case TK_FALSE:
	case TK_UNKNOWN:
		res->b = false;
		res->type = SQL_TYPE_BOOLEAN;
		res->height = 1;
		break;
	case TK_INTEGER:
		if (resolve_integer(ctx, ast, res) != 0)
			return -1;
		break;
	case TK_FLOAT:
		sqlAtoF(ast->str, &res->f, ast->len);
		res->type = SQL_TYPE_DOUBLE;
		res->height = 1;
		break;
	case TK_DECIMAL:
		if (resolve_decimal(ctx, ast, res) != 0)
			return -1;
		break;
	case TK_STRING:
		resolve_string(ctx, ast, res);
		break;
	case TK_BLOB:
		resolve_varbinary(ctx, ast, res);
		break;
	case TK_RAISE:
		resolve_raise(ctx, ast, res);
		break;
	case TK_NOT:
	case TK_BITNOT:
	case TK_ISNULL:
	case TK_NOTNULL:
		res->expr = xregion_alloc_object(ctx->region, struct rast_expr);
		if (resolve_expr(ctx, ast->left, res->expr) != 0)
			return -1;
		if (!ctx->can_resolve)
			break;
		res->type = ast->op == TK_BITNOT ? res->expr->type :
			    SQL_TYPE_BOOLEAN;
		res->height = res->expr->height + 1;
		if (expr_check_height(res->height) != 0)
			return -1;
		break;
	case TK_CAST:
		res->expr = xregion_alloc_object(ctx->region, struct rast_expr);
		if (resolve_expr(ctx, ast->left, res->expr) != 0)
			return -1;
		if (!ctx->can_resolve)
			break;
		res->type = ast->type;
		res->height = res->expr->height + 1;
		if (expr_check_height(res->height) != 0)
			return -1;
		break;
	case TK_LT:
	case TK_LE:
	case TK_GT:
	case TK_GE:
	case TK_EQ:
	case TK_NE: {
		if (resolve_binary(ctx, ast, res) != 0)
			return -1;
		if (!ctx->can_resolve)
			break;
		uint32_t size_left = rast_vector_size(res->left);
		uint32_t size_right = rast_vector_size(res->right);
		if (size_left != size_right) {
			diag_set(ClientError, ER_SQL_COLUMN_COUNT, size_left,
				 size_right);
			return -1;
		}
		res->type = SQL_TYPE_BOOLEAN;
		break;
	}
	case TK_OR:
		if (resolve_binary(ctx, ast, res) != 0)
			return -1;
		res->type = SQL_TYPE_BOOLEAN;
		break;
	case TK_AND:
		if (ast_is_false(ast->left) || ast_is_false(ast->right)) {
			res->op = TK_FALSE;
			res->b = false;
			res->type = SQL_TYPE_BOOLEAN;
			res->height = 1;
			break;
		}
		if (resolve_binary(ctx, ast, res) != 0)
			return -1;
		res->type = SQL_TYPE_BOOLEAN;
		break;
	case TK_CONCAT:
		if (resolve_binary(ctx, ast, res) != 0)
			return -1;
		res->type = SQL_TYPE_STRING;
		break;
	case TK_PLUS:
	case TK_MINUS:
	case TK_STAR:
	case TK_SLASH:
	case TK_REM:
	case TK_BITAND:
	case TK_BITOR:
	case TK_LSHIFT:
	case TK_RSHIFT:
		if (resolve_binary(ctx, ast, res) != 0)
			return -1;
		/* The type stays UNKNOWN and is computed at codegen. */
		break;
	case TK_UPLUS:
		if (resolve_expr(ctx, ast->left, res) != 0)
			return -1;
		if (!ctx->can_resolve)
			break;
		if (res->op != TK_INTEGER && res->op != TK_FLOAT &&
		    res->op != TK_DECIMAL)
			return resolve_unary_arith(res, ast->op, ctx->region);
		break;
	case TK_UMINUS:
		if (resolve_expr(ctx, ast->left, res) != 0)
			return -1;
		if (!ctx->can_resolve)
			break;
		switch (res->op) {
		case TK_INTEGER:
			if (resolve_integer_uminus(res) != 0)
				return -1;
			break;
		case TK_FLOAT:
			res->f = -res->f;
			break;
		case TK_DECIMAL:
			decimal_minus(res->dec, res->dec);
			break;
		default:
			return resolve_unary_arith(res, ast->op, ctx->region);
		}
		break;
	case TK_BETWEEN:
		if (resolve_between(ctx, ast, res) != 0)
			return -1;
		res->type = SQL_TYPE_BOOLEAN;
		break;
	case TK_IN:
		if (resolve_in(ctx, ast, res) != 0)
			return -1;
		break;
	case TK_CASE:
		if (resolve_case(ctx, ast, res) != 0)
			return -1;
		break;
	case TK_ARRAY:
		if (resolve_constructor(ctx, ast, res) != 0)
			return -1;
		res->type = SQL_TYPE_ARRAY;
		break;
	case TK_MAP:
		if (resolve_constructor(ctx, ast, res) != 0)
			return -1;
		res->type = SQL_TYPE_MAP;
		break;
	case TK_VECTOR:
		if (resolve_constructor(ctx, ast, res) != 0)
			return -1;
		res->type = SQL_TYPE_ANY;
		break;
	case TK_GETITEM:
		if (resolve_getitem(ctx, ast, res) != 0)
			return -1;
		break;
	case TK_COLLATE:
		if (sql_coll_id(&res->coll.id, ast->right->str,
				ast->right->len) != 0)
			return -1;
		res->coll.expr =
			xregion_alloc_object(ctx->region, struct rast_expr);
		if (resolve_expr(ctx, ast->left, res->coll.expr) != 0)
			return -1;
		if (!ctx->can_resolve)
			break;
		res->type = SQL_TYPE_SCALAR;
		res->height = res->coll.expr->height + 1;
		if (expr_check_height(res->height) != 0)
			return -1;
		break;
	case TK_SELECT:
	case TK_EXISTS:
		if (resolve_subquery(ctx, ast, res) != 0)
			return -1;
		break;
	case TK_ID:
	case TK_DOT:
		return resolve_column_ref(ctx, ast, res);
	case TK_FUNCTION:
		if (resolve_function(ctx, ast, res) != 0)
			return -1;
		break;
	case TK_LEADING:
	case TK_TRAILING:
	case TK_BOTH:
		/* The TRIM keywords are integer arguments of TRIM(). */
		res->op = TK_INTEGER;
		res->u = ast->op == TK_LEADING ? TRIM_LEADING :
			 ast->op == TK_TRAILING ? TRIM_TRAILING : TRIM_BOTH;
		res->type = SQL_TYPE_INTEGER;
		res->height = 1;
		break;
	default:
		ctx->can_resolve = false;
		break;
	}
	return 0;
}

/** Check if the given AST expression is `*` or `table.*`. */
static bool
ast_is_asterisk(const struct ast_expr *ast)
{
	return ast->op == TK_ASTERISK ||
	       (ast->op == TK_DOT && ast->right->op == TK_ASTERISK);
}

/**
 * Check if the given AST asterisk is expanded over the given source. As in the
 * legacy code, `table.*` matches only the source with exactly the given name.
 */
static bool
asterisk_matches_source(struct sql_resolve_context *ctx,
			const struct ast_expr *ast,
			const struct rast_source *src)
{
	if (ast->op == TK_ASTERISK)
		return true;
	const char *name = sql_name_temp(ctx->region, ast->left->str,
					 ast->left->len);
	return strcmp(name, rast_source_name(src)) == 0;
}

/** Return the number of the fields of the space of the given source. */
static uint32_t
rast_source_field_count(const struct rast_source *src)
{
	struct space *space = space_by_id(src->space_id);
	assert(space != NULL);
	return space->def->field_count;
}

/**
 * Return the name of a column an asterisk is expanded into: the name of the
 * given field, qualified with the given name of its source if the name is full.
 */
static const char *
asterisk_column_name(struct region *region, const char *src_name,
		     const char *field, bool is_full)
{
	size_t prefix_len = is_full ? strlen(src_name) + 1 : 0;
	size_t field_len = strlen(field);
	char *res = xregion_alloc(region, prefix_len + field_len + 1);
	if (is_full) {
		memcpy(res, src_name, prefix_len - 1);
		res[prefix_len - 1] = '.';
	}
	memcpy(res + prefix_len, field, field_len + 1);
	return res;
}

/**
 * Set the names of the columns an asterisk is expanded into over the given
 * source, starting from the given entry. The names are qualified with the name
 * of the source if they are full.
 *
 * Returns 0 on success and -1 on error.
 */
static int
name_asterisk_columns(struct sql_resolve_context *ctx,
		      const struct rast_source *src, bool is_full,
		      struct rast_expr_list_entry *entries)
{
	struct space *space = space_by_id(src->space_id);
	assert(space != NULL);
	const char *src_name = rast_source_name(src);
	for (uint32_t i = 0; i < space->def->field_count; ++i) {
		const char *name = asterisk_column_name(
			ctx->region, src_name, space->def->fields[i].name,
			is_full);
		if (sql_check_identifier_name(name, strlen(name)) != 0)
			return -1;
		entries[i].name = name;
	}
	return 0;
}

/**
 * Set the given entries to references to the columns an asterisk is expanded
 * into over the source with the given position. As in the legacy code, the
 * references are qualified with the name of the source if the names are full,
 * and are unqualified names otherwise.
 */
static void
resolve_asterisk_columns(struct sql_resolve_context *ctx, uint32_t source,
			 bool is_full, struct rast_expr_list_entry *entries)
{
	const struct rast_source *src = &ctx->scope->sources->sources[source];
	uint32_t count = rast_source_field_count(src);
	for (uint32_t i = 0; i < count; ++i) {
		resolve_source_column(ctx, ctx->scope, 0, source, i,
				      &entries[i].expr);
		entries[i].expr.height = is_full ? 2 : 1;
		entries[i].ref_depth = 0;
	}
	if (!is_full && ctx->scope->is_agg_allowed)
		ctx->scope->has_unaggregated_name = true;
}

/**
 * Set the names of the columns of the given AST column list of a SELECT with
 * the given sources into the given rast_expr_list, allocating its entries. As
 * in the legacy code, a column without an alias is named automatically unless
 * it is a column reference, and every asterisk is expanded into the columns of
 * the sources it matches, named after their fields, qualified with the name of
 * the source if full column names are on. The legacy code reports the last
 * asterisk that matched no source, so the same is done here.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_column_names(struct sql_resolve_context *ctx,
		     const struct ast_expr_list *ast,
		     const struct rast_source_list *sources,
		     struct rast_expr_list *res)
{
	uint32_t len = 0;
	const struct ast_expr *unmatched = NULL;
	for (const struct stailq_entry *node = ast->head.first.value;
	     node != NULL; node = node->next.value) {
		const struct ast_expr *e =
			stailq_entry(node, struct ast_expr_list_entry,
				     link)->expr;
		if (!ast_is_asterisk(e)) {
			++len;
			continue;
		}
		uint32_t count = 0;
		for (uint32_t i = 0; i < sources->len; ++i) {
			const struct rast_source *src = &sources->sources[i];
			if (asterisk_matches_source(ctx, e, src))
				count += rast_source_field_count(src);
		}
		if (count == 0)
			unmatched = e;
		len += count;
	}
	res->len = len;
	res->exprs = NULL;
	if (len > 0) {
		res->exprs = xregion_alloc_array(ctx->region,
						 typeof(*res->exprs), len);
		memset(res->exprs, 0, sizeof(*res->exprs) * len);
	}

	bool is_full = (ctx->parser->sql_flags & SQL_FullColNames) != 0;
	uint32_t j = 0;
	for (const struct stailq_entry *node = ast->head.first.value;
	     node != NULL; node = node->next.value) {
		const struct ast_expr_list_entry *entry =
			stailq_entry(node, struct ast_expr_list_entry, link);
		if (ast_is_asterisk(entry->expr)) {
			for (uint32_t i = 0; i < sources->len; ++i) {
				const struct rast_source *src =
					&sources->sources[i];
				if (!asterisk_matches_source(ctx, entry->expr,
							     src))
					continue;
				if (name_asterisk_columns(ctx, src, is_full,
							  &res->exprs[j]) != 0)
					return -1;
				j += rast_source_field_count(src);
			}
			continue;
		}
		struct rast_expr_list_entry *expr = &res->exprs[j++];
		if (entry->name.n > 0) {
			expr->name = sql_name_temp(ctx->region, entry->name.z,
						   entry->name.n);
			if (sql_check_identifier_name(expr->name,
						      strlen(expr->name)) != 0)
				return -1;
			expr->legacy_name = sql_legacy_name_temp(ctx->region,
								 entry->name.z,
								 entry->name.n);
			continue;
		}
		const struct ast_expr *e = entry->expr;
		while (e->op == TK_PARENTHESES)
			e = e->left;
		if (e->op == TK_ID || e->op == TK_DOT)
			continue;
		const char *name =
			sql_generate_column_name(++ctx->autoname_count);
		expr->name = sql_name_temp(ctx->region, name, strlen(name));
	}
	assert(j == len);
	if (unmatched == NULL)
		return 0;
	if (unmatched->op == TK_ASTERISK) {
		diag_set(ClientError, ER_SQL_SELECT_WILDCARD);
		return -1;
	}
	diag_set(ClientError, ER_NO_SUCH_SPACE,
		 sql_name_temp(ctx->region, unmatched->left->str,
			       unmatched->left->len));
	return -1;
}

/**
 * Resolve every expression of the given AST column list into the entries of
 * the given rast_expr_list. As in the legacy code, an asterisk is expanded
 * into references to the columns, which are qualified with the name of the
 * source if full column names are on.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_column_list(struct sql_resolve_context *ctx,
		    const struct ast_expr_list *ast, struct rast_expr_list *res)
{
	const struct rast_source_list *sources = ctx->scope->sources;
	bool is_full = (ctx->parser->sql_flags & SQL_FullColNames) != 0;
	uint32_t j = 0;
	for (const struct stailq_entry *node = ast->head.first.value;
	     node != NULL; node = node->next.value) {
		const struct ast_expr_list_entry *entry =
			stailq_entry(node, struct ast_expr_list_entry, link);
		if (ast_is_asterisk(entry->expr)) {
			for (uint32_t i = 0; i < sources->len; ++i) {
				const struct rast_source *src =
					&sources->sources[i];
				if (!asterisk_matches_source(ctx, entry->expr,
							     src))
					continue;
				resolve_asterisk_columns(ctx, i, is_full,
							 &res->exprs[j]);
				j += rast_source_field_count(src);
			}
			continue;
		}
		struct rast_expr_list_entry *expr = &res->exprs[j++];
		uint32_t agg_flags = ctx->scope->agg_flags;
		ctx->scope->agg_flags = 0;
		ctx->scope->ref_depth = UINT32_MAX;
		if (resolve_expr(ctx, entry->expr, &expr->expr) != 0)
			return -1;
		if (!ctx->can_resolve)
			return 0;
		expr->has_agg = ctx->scope->agg_flags != 0;
		ctx->scope->agg_flags |= agg_flags;
		expr->ref_depth = ctx->scope->ref_depth;
		expr->span = entry->expr->str;
		expr->span_len = entry->expr->len;
	}
	return 0;
}

/**
 * Check if the given AST expression is an integer literal, possibly with unary
 * plus and minus and parentheses, which the legacy code folds into a literal.
 */
static bool
ast_is_integer(const struct ast_expr *ast)
{
	while (ast->op == TK_PARENTHESES || ast->op == TK_UPLUS ||
	       ast->op == TK_UMINUS)
		ast = ast->left;
	return ast->op == TK_INTEGER;
}

/**
 * Replace the given ORDER BY term with a reference to the given result column.
 * As in the legacy code, only the outermost collation of the term is kept.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_order_by_col(struct sql_resolve_context *ctx,
		     const struct rast_expr_list *columns, uint32_t col,
		     struct rast_expr *res)
{
	if (res->op != TK_COLLATE) {
		resolve_alias(columns, col, 0, res);
		return 0;
	}
	uint32_t id = res->coll.id;
	memset(res, 0, sizeof(*res));
	res->op = TK_COLLATE;
	res->coll.id = id;
	res->coll.expr = xregion_alloc_object(ctx->region, struct rast_expr);
	memset(res->coll.expr, 0, sizeof(*res->coll.expr));
	resolve_alias(columns, col, 0, res->coll.expr);
	res->type = SQL_TYPE_SCALAR;
	res->height = res->coll.expr->height + 1;
	return expr_check_height(res->height);
}

/**
 * Report an error for an ORDER BY or GROUP BY term out of the range of the
 * columns.
 */
static void
order_by_range_error(const char *clause, uint32_t pos, uint32_t len)
{
	diag_set(ClientError, ER_SQL_PARSER_GENERIC,
		 tt_sprintf("Error at %s BY in place %u: term out of range - "
			    "should be between 1 and %u", clause, pos + 1,
			    len));
}

/**
 * Find the result column among the given ones equal to the given resolved
 * expression, as the legacy code does: either the last one equal exactly, or
 * the first one equal ignoring the collations. Until the expressions can be
 * compared in the resolved form, they are compared in the legacy form.
 *
 * Returns the position of the column, starting from 1, or 0 if not found.
 */
static uint32_t
find_equal_column(struct sql_resolve_context *ctx,
		  const struct rast_expr_list *columns,
		  const struct rast_expr *expr, bool is_exact)
{
	struct Parse *parser = ctx->parser;
	/*
	 * The expressions are built only to be compared, so their sources
	 * are not built, and the cursors their subqueries take are released.
	 */
	int cursor_count = parser->nTab;
	uint32_t len = ctx->select_depth + 1;
	struct rast_build_scope *scopes =
		xregion_alloc_array(ctx->region, typeof(*scopes), len);
	for (uint32_t i = 0; i < len; ++i) {
		scopes[i].src = NULL;
		scopes[i].parent = i + 1 < len ? &scopes[i + 1] : NULL;
	}
	struct Expr *e = expr_from_rast(parser, scopes, expr);
	uint32_t res = 0;
	for (uint32_t i = 0; i < columns->len; ++i) {
		struct Expr *col = expr_from_rast(parser, scopes,
						  &columns->exprs[i].expr);
		int cmp = is_exact ? sqlExprCompare(e, col, -1) :
			  sqlExprCompare(col, e, -1);
		sql_expr_delete(col);
		if (is_exact ? cmp == 0 : cmp < 2) {
			res = i + 1;
			if (!is_exact)
				break;
		}
	}
	sql_expr_delete(e);
	parser->nTab = cursor_count;
	return res;
}

/**
 * Resolve the given AST ORDER BY or GROUP BY list of a SELECT with the given
 * resolved columns into the given rast_expr_list. The clause is "ORDER" or
 * "GROUP". As in the legacy code, an integer term and an ORDER BY term which is
 * an alias of a result column are references to the result column, and any
 * other term is an expression, in which the aliases of the result columns are
 * visible. The terms are checked in two passes, in the same order as the legacy
 * code does.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_order_by(struct sql_resolve_context *ctx, const char *clause,
		 const struct ast_expr_list *ast,
		 const struct rast_expr_list *columns,
		 struct rast_expr_list *res)
{
	if (ast == NULL)
		return 0;
	bool is_order_by = clause[0] == 'O';
	res->exprs = xregion_alloc_array(ctx->region, typeof(*res->exprs),
					 ast->len);
	res->len = ast->len;
	memset(res->exprs, 0, sizeof(*res->exprs) * res->len);

	uint32_t i = 0;
	for (const struct stailq_entry *node = ast->head.first.value;
	     node != NULL; node = node->next.value, ++i) {
		const struct ast_expr_list_entry *entry =
			stailq_entry(node, struct ast_expr_list_entry, link);
		struct rast_expr_list_entry *item = &res->exprs[i];
		item->order = entry->order;
		const struct ast_expr *term = entry->expr;
		while (term->op == TK_PARENTHESES || term->op == TK_COLLATE)
			term = term->left;
		if (is_order_by && term->op == TK_ID) {
			const char *name = sql_name_temp(ctx->region, term->str,
							 term->len);
			int col = find_alias(columns, name, NULL);
			if (col >= 0) {
				item->order_by_col = col + 1;
				const struct ast_expr *coll = entry->expr;
				while (coll->op == TK_PARENTHESES)
					coll = coll->left;
				if (coll->op == TK_COLLATE) {
					item->expr.op = TK_COLLATE;
					if (sql_coll_id(&item->expr.coll.id,
							coll->right->str,
							coll->right->len) != 0)
						return -1;
				}
				continue;
			}
		}
		uint32_t agg_flags = ctx->scope->agg_flags;
		ctx->scope->agg_flags = 0;
		if (resolve_expr(ctx, entry->expr, &item->expr) != 0)
			return -1;
		if (!ctx->can_resolve)
			return 0;
		item->has_agg = ctx->scope->agg_flags != 0;
		ctx->scope->agg_flags |= agg_flags;
		const struct rast_expr *value = &item->expr;
		while (value->op == TK_COLLATE)
			value = value->coll.expr;
		/* As sqlExprIsInteger(), accept only 32-bit integers. */
		if (ast_is_integer(term) &&
		    (value->is_neg ? (int64_t)value->u >= INT32_MIN :
		     value->u <= INT32_MAX)) {
			assert(value->op == TK_INTEGER);
			if (value->is_neg || value->u == 0 ||
			    value->u > 0xffff) {
				order_by_range_error(clause, i, columns->len);
				return -1;
			}
			item->order_by_col = value->u;
			continue;
		}
		/*
		 * As in the legacy code, a term equal to a result column refers
		 * to the last such result column.
		 */
		item->order_by_col = find_equal_column(ctx, columns,
						       &item->expr, true);
	}
	if (res->len > SQL_MAX_COLUMN) {
		diag_set(ClientError, ER_SQL_PARSER_LIMIT,
			 tt_sprintf("The number of terms in %s BY clause",
				    clause), (int)res->len, SQL_MAX_COLUMN);
		return -1;
	}
	for (i = 0; i < res->len; ++i) {
		struct rast_expr_list_entry *item = &res->exprs[i];
		if (item->order_by_col > columns->len) {
			order_by_range_error(clause, i, columns->len);
			return -1;
		}
		if (item->order_by_col > 0) {
			uint32_t col = item->order_by_col - 1;
			if (resolve_order_by_col(ctx, columns, col,
						 &item->expr) != 0)
				return -1;
			item->has_agg = columns->exprs[col].has_agg;
		}
		enum field_type type = rast_expr_field_type(&item->expr);
		if (!field_type1_contains_type2(FIELD_TYPE_SCALAR, NULL, type,
						NULL) &&
		    type != FIELD_TYPE_ANY) {
			diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
				 field_type_strs[type], "comparable type");
			return -1;
		}
	}
	return 0;
}

/**
 * Resolve the given AST LIMIT or OFFSET expression into a new rast_expr, or set
 * the result to NULL if there is no expression. As in the legacy code, the
 * expression cannot refer to any name or use an aggregate function.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_limit(struct sql_resolve_context *ctx, const struct ast_expr *ast,
	      struct rast_expr **res)
{
	*res = NULL;
	if (ast == NULL)
		return 0;
	*res = xregion_alloc_object(ctx->region, struct rast_expr);
	return resolve_expr(ctx, ast, *res);
}

/**
 * Check if the given AST expression has a COLLATE clause outside of its
 * subqueries.
 */
static bool
ast_expr_has_collate(const struct ast_expr *ast)
{
	if (ast == NULL)
		return false;
	switch (ast->op) {
	case TK_COLLATE:
		return true;
	case TK_SELECT:
	case TK_EXISTS:
		return false;
	case TK_CAST:
	case TK_RAISE:
		return ast_expr_has_collate(ast->left);
	case TK_BETWEEN:
	case TK_CASE:
	case TK_ARRAY:
	case TK_MAP:
	case TK_VECTOR:
	case TK_DISTINCT:
	case TK_GETITEM: {
		if (ast_expr_has_collate(ast->left))
			return true;
		if (ast->list == NULL)
			return false;
		for (const struct stailq_entry *node =
			ast->list->head.first.value;
		     node != NULL; node = node->next.value) {
			const struct ast_expr_list_entry *entry =
				stailq_entry(node, struct ast_expr_list_entry,
					     link);
			if (ast_expr_has_collate(entry->expr))
				return true;
		}
		return false;
	}
	default:
		return ast_expr_has_collate(ast->left) ||
		       ast_expr_has_collate(ast->right);
	}
}

/**
 * Check if the given AST FROM clause can be resolved: it is empty or consists
 * of a single table.
 */
static bool
can_resolve_sources(const struct ast_source_list *ast)
{
	if (ast == NULL)
		return true;
	if (ast->len != 1)
		return false;
	const struct ast_source *src =
		stailq_entry(ast->head.first.value, struct ast_source, link);
	return src->select == NULL && src->name.n > 0 &&
	       src->join_on == NULL && src->join_using == NULL;
}

/**
 * Resolve the given AST source, which is a table, into the given rast_source.
 * The space is looked up and checked as in sql_lookup_space(), and the index
 * given by INDEXED BY as in sqlIndexedByLookup(). A view falls back.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_source(struct sql_resolve_context *ctx, const struct ast_source *ast,
	       struct rast_source *res)
{
	const char *name = sql_name_temp(ctx->region, ast->name.z,
					 ast->name.n);
	struct space *space = space_by_name0(name);
	if (space == NULL && ast->name.z[0] != '"') {
		space = space_by_name0(sql_legacy_name_temp(ctx->region,
							    ast->name.z,
							    ast->name.n));
	}
	if (space == NULL) {
		diag_set(ClientError, ER_NO_SUCH_SPACE, name);
		return -1;
	}
	if (space->def->opts.is_view) {
		ctx->can_resolve = false;
		return 0;
	}
	if (sql_space_def_check_format(space->def) != 0)
		return -1;
	if (space->index_count == 0) {
		diag_set(ClientError, ER_UNSUPPORTED, "SQL",
			 "spaces without primary key");
		return -1;
	}
	res->space_id = space->def->id;
	res->alias = NULL;
	if (ast->alias.n > 0) {
		res->alias = sql_name_temp(ctx->region, ast->alias.z,
					   ast->alias.n);
	}
	res->index_id = UINT32_MAX;
	/* NOT INDEXED is a token of length 1 without text, see parse.y. */
	res->is_not_indexed = ast->indexed_by.n == 1 &&
			      ast->indexed_by.z == NULL;
	if (ast->indexed_by.n > 0 && !res->is_not_indexed) {
		res->index_id = sql_index_id_by_token(space, &ast->indexed_by);
		if (res->index_id == UINT32_MAX) {
			diag_set(ClientError, ER_NO_SUCH_INDEX_NAME,
				 sql_name_temp(ctx->region, ast->indexed_by.z,
					       ast->indexed_by.n),
				 space->def->name);
			return -1;
		}
	}
	res->disallow_scan = ast->disallow_scan;
	return 0;
}

/**
 * Resolve the given AST FROM clause into the given rast_source_list.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_sources(struct sql_resolve_context *ctx,
		const struct ast_source_list *ast, struct rast_source_list *res)
{
	if (ast == NULL)
		return 0;
	res->len = ast->len;
	res->sources = xregion_alloc_array(ctx->region, typeof(*res->sources),
					   res->len);
	uint32_t i = 0;
	for (const struct stailq_entry *node = ast->head.first.value;
	     node != NULL; node = node->next.value) {
		const struct ast_source *src =
			stailq_entry(node, struct ast_source, link);
		if (resolve_source(ctx, src, &res->sources[i++]) != 0)
			return -1;
		if (!ctx->can_resolve)
			return 0;
	}
	return 0;
}

/**
 * Check if the given SELECT, which may be a compound one, can be resolved. The
 * legacy code turns a compound SELECT, which is not UNION ALL, with a COLLATE
 * clause in its ORDER BY into a SELECT from a subquery.
 */
static bool
can_resolve(const struct ast_select *ast)
{
	if (!can_resolve_sources(ast->sources) || ast->with != NULL)
		return false;
	if (ast->link.next == &ast->link)
		return true;
	bool is_union_all = true;
	const struct rlist *link = &ast->link;
	do {
		const struct ast_select *part =
			rlist_entry(link, struct ast_select, link);
		if (!can_resolve_sources(part->sources))
			return false;
		if (part->op != TK_SELECT && part->op != TK_ALL)
			is_union_all = false;
		link = link->next;
	} while (link != &ast->link);
	if (is_union_all || ast->order_by == NULL)
		return true;
	for (const struct stailq_entry *node = ast->order_by->head.first.value;
	     node != NULL; node = node->next.value) {
		const struct ast_expr_list_entry *entry =
			stailq_entry(node, struct ast_expr_list_entry, link);
		if (ast_expr_has_collate(entry->expr))
			return false;
	}
	return true;
}

/**
 * Set up the given rast_select for the given AST SELECT statement, which is not
 * compound or is a part of a compound one: resolve its sources and set the
 * names of its columns, expanding the asterisks, as the legacy code does at
 * the same point.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_select_names(struct sql_resolve_context *ctx,
		     const struct ast_select *ast, struct rast_select *res)
{
	memset(res, 0, sizeof(*res));
	res->flags = ast->flags;
	res->op = ast->op;
	if (resolve_sources(ctx, ast->sources, &res->sources) != 0)
		return -1;
	if (!ctx->can_resolve)
		return 0;
	return resolve_column_names(ctx, ast->columns, &res->sources,
				    &res->columns);
}

/**
 * Resolve the clauses of the given AST SELECT statement, which is not compound
 * or is a part of a compound one, into the given rast_select, each in the scope
 * of names the legacy code resolves it in. The ORDER BY of the rightmost part
 * of a compound SELECT belongs to the whole compound SELECT and is not resolved
 * here.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_select_clauses(struct sql_resolve_context *ctx,
		       const struct ast_select *ast, struct rast_select *res,
		       bool is_compound_head)
{
	/* The legacy code resolves LIMIT and OFFSET before the columns. */
	struct sql_resolve_scope *outer = ctx->scope;
	struct sql_resolve_scope limit_scope = {0};
	ctx->scope = &limit_scope;
	if (resolve_limit(ctx, ast->limit, &res->limit) != 0)
		return -1;
	if (!ctx->can_resolve)
		return 0;
	if (resolve_limit(ctx, ast->offset, &res->offset) != 0)
		return -1;
	if (!ctx->can_resolve)
		return 0;
	struct sql_resolve_scope scope = {
		.sources = &res->sources,
		.parent = outer,
		.is_agg_allowed = true,
	};
	ctx->scope = &scope;
	/*
	 * A clause may turn out to be not resolvable, in which case the whole
	 * SELECT falls back to the legacy path, which does its own checks.
	 */
	int rc = resolve_column_list(ctx, ast->columns, &res->columns);
	if (rc != 0 || !ctx->can_resolve)
		return rc;
	/* Mirrors the legacy aggregate detection in resolveSelectStep(). */
	res->flags |= scope.agg_flags;
	if (ast->group_by != NULL || ast->having != NULL)
		res->flags |= SF_Aggregate;
	if (ast->having != NULL) {
		struct sql_resolve_scope having_scope = {
			.sources = &res->sources,
			.aliases = &res->columns,
			.parent = outer,
			.is_agg_allowed = true,
		};
		ctx->scope = &having_scope;
		res->having = xregion_alloc_object(ctx->region,
						   struct rast_expr);
		rc = resolve_expr(ctx, ast->having, res->having);
		if (rc != 0 || !ctx->can_resolve)
			return rc;
		/*
		 * Without GROUP BY, the SELECT must be an aggregate one, and
		 * neither HAVING nor, as in the legacy code, the result columns
		 * can refer to names outside of arguments of aggregate
		 * functions.
		 */
		if (ast->group_by == NULL &&
		    ((having_scope.agg_flags | scope.agg_flags) == 0 ||
		     having_scope.has_unaggregated_name ||
		     scope.has_unaggregated_name)) {
			diag_set(ClientError, ER_SQL_EXECUTE, "HAVING argument "
				 "must appear in the GROUP BY clause or be "
				 "used in an aggregate function");
			return -1;
		}
	}
	if (ast->where != NULL) {
		/*
		 * WHERE can refer to the columns of the sources, to the result
		 * columns and to the names of the enclosing SELECTs, but cannot
		 * use aggregate functions.
		 */
		struct sql_resolve_scope where_scope = {
			.sources = &res->sources,
			.aliases = &res->columns,
			.parent = outer,
		};
		ctx->scope = &where_scope;
		res->where = xregion_alloc_object(ctx->region,
						  struct rast_expr);
		rc = resolve_expr(ctx, ast->where, res->where);
		if (rc != 0 || !ctx->can_resolve)
			return rc;
	}
	/*
	 * The ORDER BY and GROUP BY terms can refer to the columns of the
	 * sources and to the result columns, but not to the names of the
	 * enclosing SELECTs.
	 */
	struct sql_resolve_scope order_by_scope = {
		.sources = &res->sources,
		.aliases = &res->columns,
		.is_agg_allowed = true,
	};
	ctx->scope = &order_by_scope;
	if (!is_compound_head) {
		rc = resolve_order_by(ctx, "ORDER", ast->order_by,
				      &res->columns, &res->order_by);
		if (rc != 0 || !ctx->can_resolve)
			return rc;
	}
	struct sql_resolve_scope group_by_scope = {
		.sources = &res->sources,
		.aliases = &res->columns,
		.is_agg_allowed = true,
	};
	ctx->scope = &group_by_scope;
	rc = resolve_order_by(ctx, "GROUP", ast->group_by, &res->columns,
			      &res->group_by);
	if (rc != 0 || !ctx->can_resolve)
		return rc;
	for (uint32_t i = 0; i < res->group_by.len; ++i) {
		if (res->group_by.exprs[i].has_agg) {
			diag_set(ClientError, ER_SQL_PARSER_GENERIC,
				 "aggregate functions are not allowed in the "
				 "GROUP BY clause");
			return -1;
		}
	}
	/*
	 * Mirrors the legacy check in sqlSelectExpand() (select.c), which is
	 * skipped once this SELECT is marked resolved.
	 */
	if (res->columns.len > SQL_MAX_COLUMN) {
		diag_set(ClientError, ER_SQL_PARSER_LIMIT, "The number of "
			 "columns in result set", (int)res->columns.len,
			 SQL_MAX_COLUMN);
		return -1;
	}
	return 0;
}

/**
 * Find the result column the given AST ORDER BY term of a compound SELECT
 * refers to among the result columns of the given part of the compound SELECT.
 * As in the legacy code, the term is an integer, an alias of a result column or
 * an expression equal to a result column, which is resolved in the scope of
 * the sources and the aliases of the part.
 *
 * Returns the position of the column, starting from 1, or 0 if not found or if
 * the term cannot be resolved, which is marked in the given flag. Returns -1
 * on error.
 */
static int
find_compound_order_by_col(struct sql_resolve_context *ctx,
			   const struct ast_expr *ast, uint32_t pos,
			   const struct rast_select *part, bool *is_failed)
{
	const struct rast_expr_list *columns = &part->columns;
	while (ast->op == TK_PARENTHESES || ast->op == TK_COLLATE)
		ast = ast->left;
	struct rast_expr expr;
	memset(&expr, 0, sizeof(expr));
	if (ast_is_integer(ast)) {
		if (resolve_expr(ctx, ast, &expr) != 0)
			return -1;
		assert(expr.op == TK_INTEGER);
		/* As sqlExprIsInteger(), accept only 32-bit integers. */
		bool is_int32 = expr.is_neg ? (int64_t)expr.u >= INT32_MIN :
				expr.u <= INT32_MAX;
		if (is_int32) {
			if (expr.is_neg || expr.u == 0 ||
			    expr.u > columns->len) {
				order_by_range_error("ORDER", pos,
						     columns->len);
				return -1;
			}
			return expr.u;
		}
	} else if (ast->op == TK_ID) {
		const char *name = sql_name_temp(ctx->region, ast->str,
						 ast->len);
		int col = find_alias(columns, name, NULL);
		if (col >= 0)
			return col + 1;
	}
	struct sql_resolve_scope scope = {
		.sources = &part->sources,
		.aliases = columns,
		.is_agg_allowed = true,
	};
	struct sql_resolve_scope *outer = ctx->scope;
	ctx->scope = &scope;
	int rc = resolve_expr(ctx, ast, &expr);
	ctx->scope = outer;
	if (rc != 0) {
		/*
		 * As in the legacy code, the error is reported only if the
		 * term matches a column of another part of the SELECT.
		 */
		*is_failed = true;
		return 0;
	}
	if (!ctx->can_resolve)
		return 0;
	/*
	 * As in the legacy code, once a term is not resolved, no term matches
	 * a column as an expression.
	 */
	if (*is_failed)
		return 0;
	return find_equal_column(ctx, columns, &expr, false);
}

/**
 * Resolve the given AST ORDER BY list of the given compound SELECT. As in the
 * legacy code, every term is matched against the result columns of the parts
 * of the compound SELECT, from left to right, until it matches one, and is
 * replaced with the position of the matched column.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_compound_order_by(struct sql_resolve_context *ctx,
			  const struct ast_expr_list *ast,
			  struct rast_select *select)
{
	if (ast == NULL)
		return 0;
	if (ast->len > SQL_MAX_COLUMN) {
		diag_set(ClientError, ER_SQL_PARSER_LIMIT,
			 "The number of terms in ORDER BY clause",
			 (int)ast->len, SQL_MAX_COLUMN);
		return -1;
	}
	struct rast_expr_list *res = &select->order_by;
	res->exprs = xregion_alloc_array(ctx->region, typeof(*res->exprs),
					 ast->len);
	res->len = ast->len;
	memset(res->exprs, 0, sizeof(*res->exprs) * res->len);
	const struct ast_expr **terms =
		xregion_alloc_array(ctx->region, typeof(*terms), ast->len);
	uint32_t i = 0;
	for (const struct stailq_entry *node = ast->head.first.value;
	     node != NULL; node = node->next.value) {
		const struct ast_expr_list_entry *entry =
			stailq_entry(node, struct ast_expr_list_entry, link);
		res->exprs[i].order = entry->order;
		terms[i++] = entry->expr;
	}
	uint32_t count = 0;
	for (const struct rast_select *part = select; part != NULL;
	     part = part->prior)
		++count;
	const struct rast_select **parts =
		xregion_alloc_array(ctx->region, typeof(*parts), count);
	i = count;
	for (const struct rast_select *part = select; part != NULL;
	     part = part->prior)
		parts[--i] = part;

	bool is_failed = false;
	uint32_t left = res->len;
	for (uint32_t p = 0; p < count && left > 0; ++p) {
		for (i = 0; i < res->len; ++i) {
			struct rast_expr_list_entry *item = &res->exprs[i];
			if (item->order_by_col > 0)
				continue;
			int col = find_compound_order_by_col(ctx, terms[i], i,
							     parts[p],
							     &is_failed);
			if (col < 0)
				return -1;
			if (!ctx->can_resolve)
				return 0;
			if (col == 0)
				continue;
			item->order_by_col = col;
			--left;
		}
	}
	for (i = 0; i < res->len; ++i) {
		if (res->exprs[i].order_by_col > 0)
			continue;
		diag_set(ClientError, ER_SQL_PARSER_GENERIC,
			 tt_sprintf("Error at ORDER BY in place %u: term does "
				    "not match any column in the result set",
				    i + 1));
		return -1;
	}
	/* The last error of the terms is reported, as in the legacy code. */
	if (is_failed)
		return -1;
	for (i = 0; i < res->len; ++i) {
		struct rast_expr_list_entry *item = &res->exprs[i];
		const struct ast_expr *coll = terms[i];
		while (coll->op == TK_PARENTHESES)
			coll = coll->left;
		struct rast_expr *expr = &item->expr;
		if (coll->op == TK_COLLATE) {
			expr->op = TK_COLLATE;
			if (sql_coll_id(&expr->coll.id, coll->right->str,
					coll->right->len) != 0)
				return -1;
			struct rast_expr *value =
				xregion_alloc_object(ctx->region,
						     struct rast_expr);
			memset(value, 0, sizeof(*value));
			expr->coll.expr = value;
			expr->type = SQL_TYPE_SCALAR;
			expr->height = 2;
			expr = value;
		}
		/*
		 * As in the legacy code, the term is replaced with the
		 * position of the column.
		 */
		expr->op = TK_INTEGER;
		expr->u = item->order_by_col;
		expr->type = SQL_TYPE_INTEGER;
		expr->height = 1;
	}
	return 0;
}

/**
 * Check that no part of the given compound SELECT but the rightmost one has
 * ORDER BY or LIMIT. The legacy code does it when generating the code, from
 * right to left. As in the legacy code, HAVING without GROUP BY counts as
 * LIMIT.
 *
 * Returns 0 on success and -1 on error.
 */
static int
check_compound_parts(const struct rast_select *select)
{
	for (const struct rast_select *next = select; next->prior != NULL;
	     next = next->prior) {
		const struct rast_select *part = next->prior;
		const char *clause = NULL;
		if (part->order_by.len > 0)
			clause = "ORDER BY";
		else if (part->limit != NULL ||
			 (part->having != NULL && part->group_by.len == 0))
			clause = "LIMIT";
		if (clause == NULL)
			continue;
		diag_set(ClientError, ER_SQL_PARSER_GENERIC,
			 tt_sprintf("%s clause should come after %s not before",
				    clause, sql_select_op_name(next->op)));
		return -1;
	}
	return 0;
}

/**
 * Resolve the given AST SELECT statement, which may be a compound one, into the
 * given rast_select. As in the legacy code, the columns of all the parts of a
 * compound SELECT are named first, and then the parts are resolved, from right
 * to left.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_compound_select(struct sql_resolve_context *ctx,
			const struct ast_select *ast, struct rast_select *res)
{
	ctx->can_resolve = can_resolve(ast);
	if (!ctx->can_resolve)
		return 0;
	const struct rlist *head = &ast->link;
	bool is_compound = head->next != head;
	/* The legacy code does not limit the number of rows of VALUES. */
	if (is_compound && (ast->flags & SF_MultiValue) == 0) {
		uint32_t count = 1;
		for (const struct rlist *l = head->next; l != head; l = l->next)
			++count;
		if (count > SQL_MAX_COMPOUND_SELECT) {
			diag_set(ClientError, ER_SQL_PARSER_LIMIT, "The number "
				 "of UNION or EXCEPT or INTERSECT operations",
				 count, SQL_MAX_COMPOUND_SELECT);
			return -1;
		}
	}
	if (resolve_select_names(ctx, ast, res) != 0)
		return -1;
	if (!ctx->can_resolve)
		return 0;
	struct rast_select *next = res;
	for (const struct rlist *l = head->prev; l != head; l = l->prev) {
		const struct ast_select *part =
			rlist_entry(l, struct ast_select, link);
		next->prior = xregion_alloc_object(ctx->region,
						   struct rast_select);
		if (resolve_select_names(ctx, part, next->prior) != 0)
			return -1;
		if (!ctx->can_resolve)
			return 0;
		next = next->prior;
	}

	struct sql_resolve_scope *outer = ctx->scope;
	if (resolve_select_clauses(ctx, ast, res, is_compound) != 0)
		return -1;
	if (!ctx->can_resolve)
		return 0;
	next = res;
	for (const struct rlist *l = head->prev; l != head; l = l->prev) {
		const struct ast_select *part =
			rlist_entry(l, struct ast_select, link);
		struct rast_select *cur = next->prior;
		ctx->scope = outer;
		if (resolve_select_clauses(ctx, part, cur, false) != 0)
			return -1;
		if (!ctx->can_resolve)
			return 0;
		if (cur->columns.len == next->columns.len) {
			next = cur;
			continue;
		}
		if ((next->flags & SF_Values) != 0) {
			diag_set(ClientError, ER_SQL_PARSER_GENERIC,
				 "all VALUES must have the same number of "
				 "terms");
		} else {
			diag_set(ClientError, ER_SQL_PARSER_GENERIC,
				 tt_sprintf("SELECTs to the left and right of "
					    "%s do not have the same number "
					    "of result columns",
					    sql_select_op_name(next->op)));
		}
		return -1;
	}
	if (!is_compound)
		return 0;
	ctx->scope = outer;
	if (resolve_compound_order_by(ctx, ast->order_by, res) != 0)
		return -1;
	if (!ctx->can_resolve)
		return 0;
	return check_compound_parts(res);
}

/**
 * Resolve the given AST SELECT statement into the given rast_select.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_select(struct sql_resolve_context *ctx, const struct ast_select *ast,
	       struct rast_select *res)
{
	struct sql_resolve_scope *outer = ctx->scope;
	int rc = resolve_compound_select(ctx, ast, res);
	ctx->scope = outer;
	return rc;
}

static int
resolve_nested_select(struct sql_resolve_context *ctx,
		      const struct ast_select *ast, struct rast_select *res)
{
	assert(ctx->scope != NULL);
	uint32_t ref_count = ctx->scope->ref_count;
	++ctx->select_depth;
	int rc = resolve_select(ctx, ast, res);
	--ctx->select_depth;
	if (rc != 0)
		return -1;
	res->is_correlated = ctx->scope->ref_count != ref_count;
	return 0;
}

/** Build the resolved AST for the given parsed statement. */
struct sql_rast *
sql_resolve_ast(struct Parse *parser, const struct sql_ast *ast)
{
	struct sql_resolve_context ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.parser = parser;
	ctx.region = &parser->region;
	ctx.can_resolve = true;

	struct sql_rast *rast = xregion_alloc_object(ctx.region, typeof(*rast));
	memset(rast, 0, sizeof(*rast));
	rast->type = ast->type;
	switch (rast->type) {
	case SQL_AST_SELECT:
		if (resolve_select(&ctx, ast->select, &rast->select) != 0) {
			parser->is_aborted = true;
			return NULL;
		}
		break;
	default:
		ctx.can_resolve = false;
		break;
	}
	rast->is_resolved = ctx.can_resolve;
	return rast;
}

/**
 * Create an expression from the given resolved expression AST.
 *
 * This function cannot fail because the expressions have already been
 * evaluated during the resolution process.
 */
static struct Expr *
expr_from_rast(struct Parse *parser, const struct rast_build_scope *scope,
	       const struct rast_expr *expr)
{
	if (expr->op == TK_AS) {
		/*
		 * As in the legacy code, a reference to a result column is a
		 * copy of its expression, which is built in the scope of the
		 * SELECT the result column belongs to.
		 */
		const struct rast_build_scope *s = scope;
		for (uint32_t i = 0; i < expr->alias.depth; ++i)
			s = s->parent;
		struct Expr *res = expr_from_rast(parser, s, expr->alias.expr);
		incrAggFunctionDepth(res, expr->alias.depth);
		return res;
	}
	struct Expr *res = NULL;
	switch (expr->op) {
	case TK_NULL:
		res = sql_expr_new_empty(expr->op, 0);
		res->flags |= EP_Leaf;
		break;
	case TK_TRUE:
	case TK_FALSE:
	case TK_UNKNOWN:
		res = sql_expr_new_empty(expr->op, 0);
		res->flags |= EP_Leaf;
		res->v.b = expr->b;
		break;
	case TK_INTEGER:
		res = sql_expr_new_empty(expr->op, 0);
		res->flags |= EP_Leaf;
		res->v.u = expr->u;
		if (expr->is_neg)
			res->flags |= EP_Negative;
		break;
	case TK_FLOAT:
		res = sql_expr_new_empty(expr->op, 0);
		res->flags |= EP_Leaf;
		res->v.f = expr->f;
		break;
	case TK_DECIMAL:
		res = sql_expr_new_empty(expr->op, sizeof(decimal_t));
		res->flags |= EP_Leaf;
		res->v.d = (decimal_t *)&res[1];
		*res->v.d = *expr->dec;
		break;
	case TK_STRING:
		res = sql_expr_new_string(expr->z, expr->n);
		break;
	case TK_BLOB:
		res = sql_expr_new_empty(expr->op, expr->n);
		res->flags |= EP_Leaf;
		res->v.z = (char *)&res[1];
		res->v.n = expr->n;
		if (expr->n > 0)
			memcpy(res->v.z, expr->z, expr->n);
		break;
	case TK_RAISE:
		if (expr->action != ON_CONFLICT_ACTION_IGNORE) {
			res = sql_expr_new_empty(expr->op, expr->raise_len + 1);
			res->v.raise = (char *)&res[1];
			memcpy(res->v.raise, expr->raise, expr->raise_len);
			res->v.raise[expr->raise_len] = '\0';
		} else {
			res = sql_expr_new_anon(expr->op);
		}
		res->op = TK_RAISE;
		res->v.action = expr->action;
		break;
	case TK_NOT:
	case TK_BITNOT:
	case TK_ISNULL:
	case TK_NOTNULL:
	case TK_UMINUS:
	case TK_UPLUS:
	case TK_CAST:
		res = sql_expr_new_empty(expr->op, 0);
		res->pLeft = expr_from_rast(parser, scope, expr->expr);
		res->flags |= EP_Propagate & res->pLeft->flags;
		break;
	case TK_LT:
	case TK_LE:
	case TK_GT:
	case TK_GE:
	case TK_EQ:
	case TK_NE:
	case TK_OR:
	case TK_AND:
	case TK_CONCAT:
	case TK_PLUS:
	case TK_MINUS:
	case TK_STAR:
	case TK_SLASH:
	case TK_REM:
	case TK_BITAND:
	case TK_BITOR:
	case TK_LSHIFT:
	case TK_RSHIFT:
		res = sql_expr_new_empty(expr->op, 0);
		res->pLeft = expr_from_rast(parser, scope, expr->left);
		res->flags |= EP_Propagate & res->pLeft->flags;
		res->pRight = expr_from_rast(parser, scope, expr->right);
		res->flags |= EP_Propagate & res->pRight->flags;
		break;
	case TK_BETWEEN: {
		res = sql_expr_new_empty(expr->op, 0);
		res->pLeft = expr_from_rast(parser, scope,
					    expr->between.value);
		res->flags |= EP_Propagate & res->pLeft->flags;
		struct Expr *lower =
			expr_from_rast(parser, scope, expr->between.lower);
		struct Expr *upper =
			expr_from_rast(parser, scope, expr->between.upper);
		struct ExprList *list = sql_expr_list_append(NULL, lower);
		list = sql_expr_list_append(list, upper);
		res->x.pList = list;
		res->flags |= EP_Propagate & sqlExprListFlags(list);
		break;
	}
	case TK_IN: {
		res = sql_expr_new_empty(expr->op, 0);
		res->pLeft = expr_from_rast(parser, scope, expr->in.value);
		res->flags |= EP_Propagate & res->pLeft->flags;
		if (expr->in.select != NULL) {
			res->x.pSelect = select_from_rast_in(parser, scope,
							     expr->in.select);
			ExprSetProperty(res, EP_xIsSelect | EP_Subquery);
			if (expr->in.select->is_correlated)
				ExprSetProperty(res, EP_VarSelect);
			break;
		}
		struct ExprList *list = NULL;
		for (uint32_t i = 0; i < expr->in.len; ++i) {
			struct Expr *e = expr_from_rast(parser, scope,
							&expr->in.exprs[i]);
			list = sql_expr_list_append(list, e);
		}
		res->x.pList = list;
		res->flags |= EP_Propagate & sqlExprListFlags(list);
		break;
	}
	case TK_CASE: {
		if (expr->cs.value != NULL) {
			res = sql_expr_new_empty(expr->op, 0);
			res->pLeft = expr_from_rast(parser, scope,
						    expr->cs.value);
			res->flags |= EP_Propagate & res->pLeft->flags;
		} else {
			res = sql_expr_new_anon(expr->op);
		}
		struct ExprList *list = NULL;
		for (uint32_t i = 0; i < expr->cs.len; ++i) {
			struct Expr *e = expr_from_rast(parser, scope,
							&expr->cs.exprs[i]);
			list = sql_expr_list_append(list, e);
		}
		res->x.pList = list;
		res->flags |= EP_Propagate & sqlExprListFlags(list);
		break;
	}
	case TK_VECTOR:
	case TK_ARRAY:
	case TK_MAP: {
		res = sql_expr_new_anon(expr->op);
		struct ExprList *list = NULL;
		for (uint32_t i = 0; i < expr->list.len; ++i) {
			struct Expr *e = expr_from_rast(parser, scope,
							&expr->list.exprs[i]);
			list = sql_expr_list_append(list, e);
		}
		res->x.pList = list;
		res->flags |= EP_Propagate & sqlExprListFlags(list);
		break;
	}
	case TK_COLLATE:
		res = sql_expr_new_empty(expr->op, 0);
		res->pLeft = expr_from_rast(parser, scope, expr->coll.expr);
		res->flags |= EP_Collate | EP_Skip;
		res->v.id = expr->coll.id;
		break;
	case TK_GETITEM: {
		res = sql_expr_new_anon(expr->op);
		struct ExprList *list = NULL;
		for (uint32_t i = 0; i < expr->getitem.len; ++i) {
			struct Expr *e =
				expr_from_rast(parser, scope,
					       &expr->getitem.exprs[i]);
			list = sql_expr_list_append(list, e);
		}
		struct Expr *value =
			expr_from_rast(parser, scope, expr->getitem.value);
		list = sql_expr_list_append(list, value);
		res->x.pList = list;
		res->flags |= EP_Propagate & sqlExprListFlags(list);
		break;
	}
	case TK_SELECT:
	case TK_EXISTS:
		res = sql_expr_new_anon(expr->op);
		res->x.pSelect = select_from_rast_in(parser, scope,
						     expr->select);
		ExprSetProperty(res, EP_xIsSelect | EP_Subquery);
		if (expr->select->is_correlated)
			ExprSetProperty(res, EP_VarSelect);
		break;
	case TK_COLUMN_REF: {
		const struct rast_build_scope *s = scope;
		for (uint32_t i = 0; i < expr->column.depth; ++i)
			s = s->parent;
		if (s->src != NULL) {
			res = sql_expr_new_column(s->src, expr->column.source,
						  expr->column.fieldno);
			break;
		}
		/*
		 * The expression is built only to be compared, so the source
		 * is identified by its position only.
		 */
		uint32_t level = 0;
		for (const struct rast_build_scope *p = s->parent; p != NULL;
		     p = p->parent)
			++level;
		res = sql_expr_new_anon(TK_COLUMN_REF);
		res->iTable = -2 - (int)(level * BMS + expr->column.source);
		res->iColumn = expr->column.fieldno;
		break;
	}
	case TK_FUNCTION: {
		size_t len = strlen(expr->func.name);
		res = sql_expr_new_empty(expr->op, len + 1);
		res->u.zToken = (char *)&res[1];
		memcpy(res->u.zToken, expr->func.name, len + 1);
		if (expr->func.is_distinct)
			res->flags |= EP_Distinct;
		struct ExprList *list = NULL;
		for (uint32_t i = 0; i < expr->func.n_args; ++i) {
			struct Expr *e = expr_from_rast(parser, scope,
							&expr->func.args[i]);
			list = sql_expr_list_append(list, e);
		}
		res->x.pList = list;
		res->flags |= EP_Propagate & sqlExprListFlags(list);
		if (expr->func.is_agg) {
			res->op = TK_AGG_FUNCTION;
			res->op2 = expr->func.agg_depth;
		}
		if (expr->func.is_unlikely) {
			res->flags |= EP_Unlikely | EP_Skip;
			res->iTable = expr->func.probability;
		}
		if (expr->func.is_deterministic)
			res->flags |= EP_ConstFunc;
		break;
	}
	default:
		unreachable();
	}
	if (expr->op == TK_FUNCTION)
		res->type = expr->func.returns;
	else
		res->type = sql_type_to_field_type(expr->type);
	res->nHeight = expr->height;
	/*
	 * The whole subtree is already fully resolved, so the legacy resolver
	 * must not walk into it again.
	 */
	ExprSetProperty(res, EP_Resolved);
	return res;
}

/**
 * Create an expression list from the given resolved expression AST list in the
 * given scope of sources.
 *
 * Returns NULL if the list is empty or on error.
 */
static struct ExprList *
expr_list_from_rast(struct Parse *parser, const struct rast_build_scope *scope,
		    struct rast_expr_list *list)
{
	if (list->len == 0)
		return NULL;
	struct ExprList *res = NULL;
	for (uint32_t i = 0; i < list->len; ++i) {
		struct rast_expr_list_entry *entry = &list->exprs[i];
		struct Expr *expr = expr_from_rast(parser, scope, &entry->expr);
		if (expr == NULL)
			break;
		res = sql_expr_list_append(res, expr);
		struct ExprList_item *item = &res->a[res->nExpr - 1];
		if (entry->name != NULL) {
			item->zName = sql_xstrdup(entry->name);
			item->legacy_name = sql_xstrdup(entry->legacy_name);
		}
		if (entry->span != NULL)
			sqlExprListSetSpan(res, entry->span, entry->span_len);
		if (entry->order != SORT_ORDER_ASC)
			sqlExprListSetSortOrder(res, entry->order);
		item->u.x.iOrderByCol = entry->order_by_col;
	}
	if (parser->is_aborted) {
		sql_expr_list_delete(res);
		return NULL;
	}
	return res;
}

static struct SrcList *
src_list_from_rast(struct Parse *parser, const struct rast_source_list *list)
{
	struct SrcList *res = NULL;
	for (uint32_t i = 0; i < list->len; ++i) {
		const struct rast_source *src = &list->sources[i];
		struct space *space = space_by_id(src->space_id);
		assert(space != NULL);
		res = sql_src_list_append(res, NULL);
		struct SrcList_item *item = &res->a[res->nSrc - 1];
		item->zName = sql_xstrdup(space->def->name);
		if (src->alias != NULL)
			item->zAlias = sql_xstrdup(src->alias);
		item->space = space;
		item->fg.disallow_scan = src->disallow_scan;
		item->fg.notIndexed = src->is_not_indexed;
		if (src->index_id != UINT32_MAX) {
			item->pIBIndex = space_index(space, src->index_id)->def;
			item->indexed_by = sql_xstrdup(item->pIBIndex->name);
			item->fg.isIndexedBy = true;
		}
	}
	if (res != NULL)
		sqlSrcListAssignCursors(parser, res);
	return res;
}

/**
 * Create a SELECT statement from the given part of a resolved compound SELECT,
 * without the parts to the left of it, see select_from_rast_in().
 *
 * Returns NULL on error.
 */
static struct Select *
select_part_from_rast(struct Parse *parser,
		      const struct rast_build_scope *parent,
		      struct rast_select *select)
{
	struct rast_build_scope scope = {
		.src = src_list_from_rast(parser, &select->sources),
		.parent = parent,
	};
	struct ExprList *cols = expr_list_from_rast(parser, &scope,
						    &select->columns);
	if (parser->is_aborted) {
		sqlSrcListDelete(scope.src);
		return NULL;
	}
	/*
	 * The SELECT is already fully resolved, so the legacy resolver
	 * (sqlSelectPrep()) must not walk this SELECT again.
	 */
	uint32_t flags = select->flags | SF_Resolved | SF_HasTypeInfo;
	struct Expr *where = NULL;
	if (select->where != NULL)
		where = expr_from_rast(parser, &scope, select->where);
	struct ExprList *group_by = expr_list_from_rast(parser, &scope,
							&select->group_by);
	struct Expr *having = NULL;
	if (select->having != NULL)
		having = expr_from_rast(parser, &scope, select->having);
	struct ExprList *order_by = expr_list_from_rast(parser, &scope,
							&select->order_by);
	struct Expr *limit = NULL;
	if (select->having != NULL && select->group_by.len == 0) {
		/*
		 * As in the legacy code, an aggregate SELECT without GROUP BY
		 * returns one row at most, so LIMIT is replaced.
		 */
		limit = sql_expr_new(TK_INTEGER, NULL);
		limit->v.u = 1;
		limit->type = FIELD_TYPE_INTEGER;
	} else if (select->limit != NULL) {
		limit = expr_from_rast(parser, &scope, select->limit);
	}
	struct Expr *offset = NULL;
	if (select->offset != NULL)
		offset = expr_from_rast(parser, &scope, select->offset);
	struct Select *res = sqlSelectNew(cols, scope.src, where, group_by,
					  having, order_by, flags, limit,
					  offset);
	res->op = select->op;
	return res;
}

static struct Select *
select_from_rast_in(struct Parse *parser,
		    const struct rast_build_scope *parent,
		    struct rast_select *select)
{
	/*
	 * The parts are built in the order the legacy code expands them in,
	 * so that the cursors are assigned in the same order: the sources
	 * first, then the subqueries of the clauses, then the SELECT to the
	 * left. The parts are not built recursively, since VALUES may have
	 * any number of rows.
	 */
	struct Select *res = NULL;
	struct Select *next = NULL;
	for (struct rast_select *p = select; p != NULL; p = p->prior) {
		struct Select *part = select_part_from_rast(parser, parent, p);
		if (parser->is_aborted) {
			sql_select_delete(part);
			sql_select_delete(res);
			return NULL;
		}
		if (next == NULL) {
			res = part;
		} else {
			next->pPrior = part;
			part->pNext = next;
		}
		next = part;
	}
	return res;
}

struct Select *
select_from_rast(struct Parse *parser, struct rast_select *select)
{
	return select_from_rast_in(parser, NULL, select);
}
