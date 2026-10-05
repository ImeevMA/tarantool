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
#include "tarantoolInt.h"
#include <stdlib.h>
#include <string.h>
#include "box/schema.h"
#include "box/session.h"
#include "box/coll_id_cache.h"
#include "box/tuple_constraint_def.h"

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

/**
 * WITH clause visible at some point of the resolve phase. As in the legacy
 * code, the tables of a WITH clause are visible in its SELECT, and in the
 * SELECTs of the tables themselves, which are resolved for every use.
 */
struct sql_resolve_with {
	/** The WITH clause. */
	const struct ast_with_list *ast;
	/** Dequoted names of the tables, by their positions. */
	const char **names;
	/**
	 * Formats of the errors reported for a reference to a table, by its
	 * position, or NULL if the table can be referred to.
	 */
	const char **errors;
	/** Whether a table is referred to, by its position. */
	bool *is_used;
	/** Enclosing WITH clause, or NULL. */
	struct sql_resolve_with *parent;
};

/** List of the tables of WITH clauses converted by the legacy code. */
struct sql_resolve_converted {
	/** The table of a WITH clause. */
	const struct ast_with_entry *entry;
	/** Next table in the list, or NULL. */
	struct sql_resolve_converted *next;
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
	/** Innermost visible WITH clause, or NULL. */
	struct sql_resolve_with *with;
	/**
	 * WITH clause of the table whose SELECT is resolved next, or NULL, see
	 * resolve_cte_source().
	 */
	struct sql_resolve_with *cte_with;
	/** Position of the table whose SELECT is resolved next. */
	uint32_t cte_pos;
	/** Source of the table whose SELECT is resolved next. */
	struct rast_source *cte_res;
	/** Whether the table whose SELECT is resolved next is recursive. */
	bool cte_is_recursive;
	/**
	 * Source of a recursive table of a WITH clause, if the sources resolved
	 * next are of the rightmost part of its SELECT, see resolve_sources().
	 */
	const struct rast_source *recursive_cte;
	/** Unused tables of WITH clauses converted by the legacy code. */
	struct sql_resolve_converted *converted;
	/**
	 * Whether the SELECT resolved next is the single row of VALUES of an
	 * INSERT, whose values sqlInsert() resolves without allowing aggregate
	 * functions.
	 */
	bool is_insert_row;
	/**
	 * Whether the expression being resolved is the body of an SQL_EXPR
	 * function, whose names refer to the fields of a row by their names,
	 * see sql_func_expr_from_ast().
	 */
	bool is_row_expr;
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
 * Resolve the given AST SELECT statement into the given rast_select.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_select(struct sql_resolve_context *ctx, const struct ast_select *ast,
	       struct rast_select *res);

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
 * Create a source list of a SELECT, which is a subquery of a SELECT with the
 * given scope of sources, from the given resolved source list.
 *
 * Returns NULL if the list is empty or on error.
 */
static struct SrcList *
src_list_from_rast(struct Parse *parser, const struct rast_build_scope *parent,
		   struct rast_source_list *list);

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
	res->right = xregion_alloc_object(ctx->region, struct rast_expr);
	if (resolve_expr(ctx, ast->right, res->right) != 0)
		return -1;
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
	res->between.upper =
		xregion_alloc_object(ctx->region, struct rast_expr);
	if (resolve_expr(ctx, upper->expr, res->between.upper) != 0)
		return -1;
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
		res->in.select =
			xregion_alloc_object(ctx->region, struct rast_select);
		if (resolve_nested_select(ctx, rhs->select,
					  res->in.select) != 0)
			return -1;
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
		const struct stailq_entry *node = rhs->list->head.first.value;
		const struct ast_expr_list_entry *entry =
			stailq_entry(node, struct ast_expr_list_entry, link);
		res->right =
			xregion_alloc_object(ctx->region, struct rast_expr);
		if (resolve_expr(ctx, entry->expr, res->right) != 0)
			return -1;
		res->type = SQL_TYPE_BOOLEAN;
		res->height = MAX(res->left->height, res->right->height) + 1;
		return expr_check_height(res->height);
	}
	res->in.value = xregion_alloc_object(ctx->region, struct rast_expr);
	if (resolve_expr(ctx, ast->left, res->in.value) != 0)
		return -1;
	res->in.len = rhs->list->len;
	res->in.exprs = xregion_alloc_array(ctx->region, struct rast_expr,
					    res->in.len);
	uint32_t height;
	if (resolve_operand_list(ctx, rhs->list, res->in.exprs, &height) != 0)
		return -1;
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
		value_height = res->cs.value->height;
	}
	res->cs.len = ast->list->len;
	res->cs.exprs = xregion_alloc_array(ctx->region, struct rast_expr,
					    res->cs.len);
	uint32_t height;
	if (resolve_operand_list(ctx, ast->list, res->cs.exprs, &height) != 0)
		return -1;
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
	res->getitem.len = ast->list->len;
	res->getitem.exprs = xregion_alloc_array(ctx->region, struct rast_expr,
						 res->getitem.len);
	uint32_t height;
	if (resolve_operand_list(ctx, ast->list, res->getitem.exprs,
				 &height) != 0)
		return -1;
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
 * Return the definition of the given source, describing its name and its
 * fields.
 */
static const struct space_def *
rast_source_def(const struct rast_source *src)
{
	if (src->def != NULL)
		return src->def;
	struct space *space = space_by_id(src->space_id);
	assert(space != NULL);
	return space->def;
}

/**
 * Return the name of the given source: its alias, if any, or the name of its
 * definition.
 */
static const char *
rast_source_name(const struct rast_source *src)
{
	if (src->alias != NULL)
		return src->alias;
	return rast_source_def(src)->name;
}

/**
 * Return the number of the field with exactly the given name in the given
 * definition, or UINT32_MAX if there is none.
 */
static uint32_t
space_def_fieldno(const struct space_def *def, const char *name)
{
	for (uint32_t i = 0; i < def->field_count; ++i) {
		if (strcmp(def->fields[i].name, name) == 0)
			return i;
	}
	return UINT32_MAX;
}

/**
 * Return the number of the field with the given name, or, if not found and the
 * legacy name is not NULL, with the given legacy name, in the space of the
 * given source, or UINT32_MAX if there is none.
 */
static uint32_t
rast_source_fieldno(const struct rast_source *src, const char *name,
		    const char *legacy_name)
{
	const struct space_def *def = rast_source_def(src);
	uint32_t res = space_def_fieldno(def, name);
	if (res == UINT32_MAX && legacy_name != NULL)
		res = space_def_fieldno(def, legacy_name);
	return res;
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
	if (tab_name != NULL) {
		const char *src_name = rast_source_name(src);
		if (strcmp(src_name, tab_name) != 0 &&
		    (legacy_tab_name == NULL ||
		     strcmp(src_name, legacy_tab_name) != 0))
			return UINT32_MAX;
	}
	return rast_source_fieldno(src, name, legacy_name);
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
	const struct space_def *def =
		rast_source_def(&scope->sources->sources[source]);
	assert(fieldno < def->field_count);
	res->op = TK_COLUMN_REF;
	res->column.depth = depth;
	res->column.source = source;
	res->column.fieldno = fieldno;
	res->column.field_type = def->fields[fieldno].type;
	res->type = sql_type_from_field_type(res->column.field_type);
}

/**
 * Report an error for a column name, optionally qualified with the given
 * source name, which refers to more than one column.
 */
static void
ambiguous_column_error(const char *tab_name, const char *name)
{
	const char *err = tab_name == NULL ?
			  tt_sprintf("ambiguous column name: %s", name) :
			  tt_sprintf("ambiguous column name: %s.%s", tab_name,
				     name);
	diag_set(ClientError, ER_SQL_PARSER_GENERIC, err);
}

/**
 * Check if the given USING names of the given source contain the given name
 * or, if it is not NULL, the given legacy name.
 */
static bool
rast_source_uses(const struct rast_source *src, const char *name,
		 const char *legacy_name)
{
	for (uint32_t i = 0; i < src->using_count; ++i) {
		if (strcmp(src->using_names[i], name) == 0)
			return true;
	}
	for (uint32_t i = 0; legacy_name != NULL && i < src->using_count;
	     ++i) {
		if (strcmp(src->using_names[i], legacy_name) == 0)
			return true;
	}
	return false;
}

/**
 * Check if the definition of the given source has a field after the one with
 * the given number with the same name, which only a subquery can have.
 */
static bool
rast_source_has_dup_field(const struct rast_source *src, uint32_t fieldno)
{
	const struct space_def *def = rast_source_def(src);
	const char *name = def->fields[fieldno].name;
	for (uint32_t i = fieldno + 1; i < def->field_count; ++i) {
		if (strcmp(def->fields[i].name, name) == 0)
			return true;
	}
	return false;
}

/** Check if the given source is a parenthesized list of sources. */
static bool
rast_source_is_nested(const struct rast_source *src)
{
	return src->select != NULL && (src->select->flags & SF_NestedFrom) != 0;
}

/**
 * Check if the given part of a span, with the given length, is the given name
 * or, if it is not NULL, the given legacy name.
 */
static bool
span_part_is(const char *part, uint32_t len, const char *name,
	     const char *legacy_name)
{
	if (strlen(name) == len && memcmp(part, name, len) == 0)
		return true;
	return legacy_name != NULL && strlen(legacy_name) == len &&
	       memcmp(part, legacy_name, len) == 0;
}

/**
 * Check if the span of the given column of a parenthesized list of sources,
 * which is `source.column`, matches the given column name, if it is not NULL,
 * qualified with the given source name, if it is not NULL, as
 * sqlMatchSpanName() does. The names also match their legacy forms, if they
 * are not NULL.
 */
static bool
rast_span_matches(const struct rast_expr_list_entry *column,
		  const char *tab_name, const char *legacy_tab_name,
		  const char *name, const char *legacy_name)
{
	const char *span = column->span;
	const char *dot = memchr(span, '.', column->span_len);
	assert(dot != NULL);
	uint32_t tab_len = dot - span;
	if (tab_name != NULL &&
	    !span_part_is(span, tab_len, tab_name, legacy_tab_name))
		return false;
	return name == NULL || span_part_is(dot + 1,
					    column->span_len - tab_len - 1,
					    name, legacy_name);
}

/**
 * Count the fields of the given parenthesized list of sources whose spans match
 * the given names, as sql_count_span_matches() does: the legacy names are used
 * only if the names match no span. Sets the number of the last one.
 */
static uint32_t
count_nested_fields(const struct rast_source *src, const char *tab_name,
		    const char *legacy_tab_name, const char *name,
		    const char *legacy_name, uint32_t *fieldno)
{
	const struct rast_expr_list *columns = &src->select->columns;
	uint32_t count = 0;
	for (uint32_t i = 0; i < columns->len; ++i) {
		if (rast_span_matches(&columns->exprs[i], tab_name, NULL, name,
				      NULL)) {
			++count;
			*fieldno = i;
		}
	}
	if (count > 0 || (legacy_tab_name == NULL && legacy_name == NULL))
		return count;
	for (uint32_t i = 0; i < columns->len; ++i) {
		if (rast_span_matches(&columns->exprs[i], tab_name,
				      legacy_tab_name, name, legacy_name)) {
			++count;
			*fieldno = i;
		}
	}
	return count;
}

/**
 * Find the column with the given name, optionally qualified with the given
 * source name, among the given sources. As in the legacy code, the name must
 * match one column of one source only, but once it matches one, a source
 * joined by NATURAL or by USING with the name is skipped. A column of a
 * parenthesized list of sources is looked up by its span, and if no span
 * matches a qualified name, among the fields of the list.
 *
 * Sets the position of the source, or UINT32_MAX if the column is not found,
 * and the number of the field.
 *
 * Returns 0 on success and -1 if the name is ambiguous.
 */
static int
find_column(const struct rast_source_list *sources, const char *tab_name,
	    const char *legacy_tab_name, const char *name,
	    const char *legacy_name, uint32_t *source, uint32_t *fieldno)
{
	*source = UINT32_MAX;
	uint32_t count = 0;
	for (uint32_t i = 0; i < sources->len; ++i) {
		const struct rast_source *src = &sources->sources[i];
		if (rast_source_is_nested(src)) {
			uint32_t hit = count_nested_fields(src, tab_name,
							   legacy_tab_name,
							   name, legacy_name,
							   fieldno);
			if (hit > 0) {
				count += hit;
				*source = i;
			}
			if (hit > 0 || tab_name == NULL)
				continue;
		}
		uint32_t f = find_source_field(src, tab_name, legacy_tab_name,
					       name, legacy_name);
		if (f == UINT32_MAX)
			continue;
		if (count == 1 && ((src->join_type & JT_NATURAL) != 0 ||
				   rast_source_uses(src, name, legacy_name)))
			continue;
		count += rast_source_has_dup_field(src, f) ? 2 : 1;
		*source = i;
		*fieldno = f;
	}
	if (count <= 1)
		return 0;
	ambiguous_column_error(tab_name, name);
	return -1;
}

/**
 * Check if a source of the given list has the given name, or the given legacy
 * name if it is not NULL.
 */
static bool
sources_have_name(const struct rast_source_list *sources, const char *name,
		  const char *legacy_name)
{
	for (uint32_t i = 0; i < sources->len; ++i) {
		const char *src_name = rast_source_name(&sources->sources[i]);
		if (strcmp(src_name, name) == 0 ||
		    (legacy_name != NULL && strcmp(src_name, legacy_name) == 0))
			return true;
	}
	return false;
}

/**
 * Resolve a column name qualified with the given source name, which is not
 * the name of a source of the given scope or of the scopes inside it, as a
 * column of the OLD or NEW row of the trigger being coded, if any, as the
 * legacy code does: NEW is not available in a DELETE trigger, and OLD is not
 * available in an INSERT trigger. Sets the flag if the source name is the name
 * of a row.
 *
 * Returns true if the column is resolved.
 */
static bool
resolve_trigger_column(struct sql_resolve_context *ctx,
		       const struct sql_resolve_scope *scope,
		       const char *tab_name, const char *name,
		       const char *legacy_name, struct rast_expr *res,
		       bool *is_row)
{
	const struct space *space = ctx->parser->triggered_space;
	if (space == NULL)
		return false;
	int op = ctx->parser->eTriggerOp;
	uint32_t row;
	if (op != TK_DELETE && sqlStrICmp(tab_name, "new") == 0)
		row = 1;
	else if (op != TK_INSERT && sqlStrICmp(tab_name, "old") == 0)
		row = 0;
	else
		return false;
	*is_row = true;
	const struct space_def *def = space->def;
	uint32_t fieldno = space_def_fieldno(def, name);
	if (fieldno == UINT32_MAX && legacy_name != NULL)
		fieldno = space_def_fieldno(def, legacy_name);
	if (fieldno == UINT32_MAX)
		return false;
	for (struct sql_resolve_scope *s = ctx->scope; s != scope->parent;
	     s = s->parent)
		++s->ref_count;
	res->op = TK_TRIGGER;
	res->column.depth = 0;
	res->column.source = row;
	res->column.fieldno = fieldno;
	res->column.field_type = def->fields[fieldno].type;
	res->type = sql_type_from_field_type(res->column.field_type);
	return true;
}

/**
 * Resolve an AST column reference: a column name, optionally qualified with a
 * source name, into the given rast_expr. As in the legacy code, the name is
 * looked up in the scopes from the current one outwards; in every scope first
 * among the columns of the sources, where it must match one column only, then,
 * if the name is not qualified, among the aliases of the result columns. If the
 * reference is not resolved, the error is reported with the same message as
 * the legacy code does.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_column_ref(struct sql_resolve_context *ctx, const struct ast_expr *ast,
		   struct rast_expr *res)
{
	if (ctx->is_row_expr) {
		/*
		 * As in the legacy code, a name is looked up when the
		 * function is called.
		 */
		if (ast->op == TK_DOT) {
			diag_set(ClientError, ER_UNSUPPORTED, "SQL expressions",
				 "reference to spaces");
			return -1;
		}
		res->op = TK_ID;
		res->z = (char *)sql_name_temp(ctx->region, ast->str, ast->len);
		res->n = strlen(res->z);
		res->type = SQL_TYPE_ANY;
		res->height = 1;
		return 0;
	}
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
	/*
	 * Whether the source name is the name of a source of the scopes
	 * looked through or of a row of the trigger being coded.
	 */
	bool is_tab_found = false;
	uint32_t depth = 0;
	for (struct sql_resolve_scope *scope = ctx->scope;
	     scope != NULL; scope = scope->parent, ++depth) {
		uint32_t source = UINT32_MAX;
		uint32_t fieldno = UINT32_MAX;
		if (scope->sources != NULL &&
		    find_column(scope->sources, tab_name, legacy_tab_name,
				name, legacy_name, &source, &fieldno) != 0)
			return -1;
		if (source != UINT32_MAX) {
			resolve_source_column(ctx, scope, depth, source,
					      fieldno, res);
			return 0;
		}
		if (tab_name != NULL && scope->sources != NULL &&
		    sources_have_name(scope->sources, tab_name,
				      legacy_tab_name))
			is_tab_found = true;
		if (tab_name != NULL && !is_tab_found &&
		    resolve_trigger_column(ctx, scope, tab_name, name,
					   legacy_name, res, &is_tab_found))
			return 0;
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
 * and set its height. The result type is left unknown and is computed from the
 * subquery when needed, as the legacy code does. The result type is left to
 * the caller.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_subquery(struct sql_resolve_context *ctx, const struct ast_expr *ast,
		 struct rast_expr *res)
{
	/*
	 * As in the legacy code, a scalar subquery and EXISTS are not allowed
	 * in the body of an SQL_EXPR function, unlike an IN subquery.
	 */
	if (ctx->is_row_expr) {
		diag_set(ClientError, ER_UNSUPPORTED, "SQL expressions",
			 "subselects");
		return -1;
	}
	res->select = xregion_alloc_object(ctx->region, struct rast_select);
	if (resolve_nested_select(ctx, ast->select, res->select) != 0)
		return -1;
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
	case TK_TRIGGER:
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
	/*
	 * As in the legacy code, an aggregate function is called as a regular
	 * one in the body of an SQL_EXPR function.
	 */
	res->func.is_agg = (flags & SQL_FUNC_AGG) != 0 && !ctx->is_row_expr;
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
	case TK_VAR_ANON:
	case TK_VAR_NUM:
	case TK_VAR_NAME:
		res->var.name = ast->str;
		res->var.len = ast->len;
		res->var.num = ast->var.num;
		res->var.base = ast->var.base;
		res->type = SQL_TYPE_BOOLEAN;
		res->height = 1;
		break;
	case TK_NOT:
	case TK_BITNOT:
	case TK_ISNULL:
	case TK_NOTNULL:
		res->expr = xregion_alloc_object(ctx->region, struct rast_expr);
		if (resolve_expr(ctx, ast->left, res->expr) != 0)
			return -1;
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
		if (res->op != TK_INTEGER && res->op != TK_FLOAT &&
		    res->op != TK_DECIMAL)
			return resolve_unary_arith(res, ast->op, ctx->region);
		break;
	case TK_UMINUS:
		if (resolve_expr(ctx, ast->left, res) != 0)
			return -1;
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
		unreachable();
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

/** Return the dequoted source name of the given AST asterisk `table.*`. */
static const char *
asterisk_source_name(struct sql_resolve_context *ctx,
		     const struct ast_expr *ast)
{
	return sql_name_temp(ctx->region, ast->left->str, ast->left->len);
}

/**
 * Check if the given AST asterisk is expanded over the given source. As in the
 * legacy code, `table.*` matches the source with exactly the given name, or a
 * parenthesized list of sources having a column whose span is qualified with
 * exactly the given name.
 */
static bool
asterisk_matches_source(struct sql_resolve_context *ctx,
			const struct ast_expr *ast,
			const struct rast_source *src)
{
	if (ast->op == TK_ASTERISK)
		return true;
	const char *name = asterisk_source_name(ctx, ast);
	if (strcmp(name, rast_source_name(src)) == 0)
		return true;
	if (!rast_source_is_nested(src))
		return false;
	uint32_t unused;
	return count_nested_fields(src, name, NULL, NULL, NULL, &unused) > 0;
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
 * Check if the field with the given number of the source with the given
 * position in the given sources is omitted when the given asterisk is
 * expanded. As in the legacy code, `*` omits the fields of the right source of
 * a NATURAL join present in the sources to the left of it, and the fields
 * named in the USING clause of the right source, and `table.*` over a
 * parenthesized list of sources with another name omits the fields whose spans
 * are not qualified with exactly the given name.
 */
static bool
asterisk_omits_field(struct sql_resolve_context *ctx,
		     const struct ast_expr *ast,
		     const struct rast_source_list *sources, uint32_t source,
		     uint32_t fieldno)
{
	const struct rast_source *src = &sources->sources[source];
	if (ast->op != TK_ASTERISK) {
		if (!rast_source_is_nested(src))
			return false;
		const char *tab_name = asterisk_source_name(ctx, ast);
		if (strcmp(tab_name, rast_source_name(src)) == 0)
			return false;
		const struct rast_expr_list_entry *column =
			&src->select->columns.exprs[fieldno];
		return !rast_span_matches(column, tab_name, NULL, NULL, NULL);
	}
	if (source == 0)
		return false;
	const char *field = rast_source_def(src)->fields[fieldno].name;
	for (uint32_t i = 0; (src->join_type & JT_NATURAL) != 0 && i < source;
	     ++i) {
		if (rast_source_fieldno(&sources->sources[i], field,
					NULL) != UINT32_MAX)
			return true;
	}
	for (uint32_t i = 0; i < src->using_count; ++i) {
		if (strcmp(src->using_names[i], field) == 0)
			return true;
	}
	return false;
}

/**
 * Return the number of the columns the given asterisk is expanded into over
 * the source with the given position in the given sources.
 */
static uint32_t
asterisk_column_count(struct sql_resolve_context *ctx,
		      const struct ast_expr *ast,
		      const struct rast_source_list *sources, uint32_t source)
{
	const struct rast_source *src = &sources->sources[source];
	const struct space_def *def = rast_source_def(src);
	uint32_t res = 0;
	for (uint32_t i = 0; i < def->field_count; ++i) {
		if (!asterisk_omits_field(ctx, ast, sources, source, i))
			++res;
	}
	return res;
}

/**
 * Set the span of the given column of a parenthesized list of sources, which
 * comes from the field with the given number of the given source, as
 * selectExpander() does: it is the span of the field if the source is itself a
 * parenthesized list of sources, and the name of the source and the name of
 * the field joined with a dot otherwise.
 */
static void
set_nested_column_span(struct region *region, const struct rast_source *src,
		       uint32_t fieldno, struct rast_expr_list_entry *column)
{
	if (rast_source_is_nested(src)) {
		const struct rast_expr_list_entry *field =
			&src->select->columns.exprs[fieldno];
		column->span = field->span;
		column->span_len = field->span_len;
		return;
	}
	const char *src_name = rast_source_name(src);
	const char *name = rast_source_def(src)->fields[fieldno].name;
	size_t src_len = strlen(src_name);
	size_t name_len = strlen(name);
	char *span = xregion_alloc(region, src_len + name_len + 1);
	memcpy(span, src_name, src_len);
	span[src_len] = '.';
	memcpy(span + src_len + 1, name, name_len);
	column->span = span;
	column->span_len = src_len + name_len + 1;
}

/**
 * Set the names of the columns the given asterisk is expanded into over the
 * source with the given position in the given sources, starting from the given
 * entry. The names are qualified with the name of the source if they are full.
 * The spans of the columns are set if the SELECT is a parenthesized list of
 * sources.
 *
 * Returns 0 on success and -1 on error.
 */
static int
name_asterisk_columns(struct sql_resolve_context *ctx,
		      const struct ast_expr *ast,
		      const struct rast_source_list *sources, uint32_t source,
		      bool is_full, bool is_nested,
		      struct rast_expr_list_entry *entries)
{
	const struct rast_source *src = &sources->sources[source];
	const struct space_def *def = rast_source_def(src);
	const char *src_name = rast_source_name(src);
	for (uint32_t i = 0; i < def->field_count; ++i) {
		if (asterisk_omits_field(ctx, ast, sources, source, i))
			continue;
		const char *name = asterisk_column_name(ctx->region, src_name,
							def->fields[i].name,
							is_full);
		if (sql_check_identifier_name(name, strlen(name)) != 0)
			return -1;
		entries->name = name;
		if (is_nested)
			set_nested_column_span(ctx->region, src, i, entries);
		++entries;
	}
	return 0;
}

/**
 * Set the given entries to references to the columns the given asterisk is
 * expanded into over the source with the given position. As in the legacy
 * code, a column is a name qualified with the exact name of the source if the
 * names are full or there are several sources, and an unqualified name
 * otherwise, and it is looked up as such. A column of a parenthesized list of
 * sources is referred to by its position, as the names of its columns may be
 * the same.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_asterisk_columns(struct sql_resolve_context *ctx,
			 const struct ast_expr *ast, uint32_t source,
			 bool is_full, struct rast_expr_list_entry *entries)
{
	const struct rast_source_list *sources = ctx->scope->sources;
	const struct rast_source *src = &sources->sources[source];
	const struct space_def *def = rast_source_def(src);
	bool is_nested = rast_source_is_nested(src);
	bool is_qualified = is_full || sources->len > 1;
	const char *tab_name = is_qualified ? rast_source_name(src) : NULL;
	for (uint32_t i = 0; i < def->field_count; ++i) {
		if (asterisk_omits_field(ctx, ast, sources, source, i))
			continue;
		uint32_t found = source;
		uint32_t fieldno = i;
		if (!is_nested &&
		    find_column(sources, tab_name, NULL, def->fields[i].name,
				NULL, &found, &fieldno) != 0)
			return -1;
		assert(found != UINT32_MAX);
		resolve_source_column(ctx, ctx->scope, 0, found, fieldno,
				      &entries->expr);
		entries->expr.height = is_qualified && !is_nested ? 2 : 1;
		(entries++)->ref_depth = 0;
	}
	if (!is_qualified && !is_nested && ctx->scope->is_agg_allowed)
		ctx->scope->has_unaggregated_name = true;
	return 0;
}

/**
 * Set the names of the columns of the given AST column list of a SELECT with
 * the given sources into the given rast_expr_list, allocating its entries. As
 * in the legacy code, a column without an alias is named automatically unless
 * it is a column reference, and every asterisk is expanded into the columns of
 * the sources it matches, named after their fields, qualified with the name of
 * the source if full column names are on. The legacy code reports the last
 * asterisk that matched no source, so the same is done here. If the SELECT is
 * a parenthesized list of sources, the spans of the columns are set too.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_column_names(struct sql_resolve_context *ctx,
		     const struct ast_expr_list *ast,
		     const struct rast_source_list *sources, bool is_nested,
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
		bool is_matched = false;
		for (uint32_t i = 0; i < sources->len; ++i) {
			const struct rast_source *src = &sources->sources[i];
			if (!asterisk_matches_source(ctx, e, src))
				continue;
			is_matched = true;
			len += asterisk_column_count(ctx, e, sources, i);
		}
		if (!is_matched)
			unmatched = e;
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
				if (name_asterisk_columns(ctx, entry->expr,
							  sources, i, is_full,
							  is_nested,
							  &res->exprs[j]) != 0)
					return -1;
				j += asterisk_column_count(ctx, entry->expr,
							   sources, i);
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
				struct rast_expr_list_entry *entries =
					&res->exprs[j];
				if (resolve_asterisk_columns(ctx, entry->expr,
							     i, is_full,
							     entries) != 0)
					return -1;
				j += asterisk_column_count(ctx, entry->expr,
							   sources, i);
			}
			continue;
		}
		struct rast_expr_list_entry *expr = &res->exprs[j++];
		uint32_t agg_flags = ctx->scope->agg_flags;
		ctx->scope->agg_flags = 0;
		ctx->scope->ref_depth = UINT32_MAX;
		if (resolve_expr(ctx, entry->expr, &expr->expr) != 0)
			return -1;
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
	 * The expressions are built only to be compared, so their sources are
	 * not built.
	 */
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
 * Check the join types of the given AST FROM clause, as the legacy code does
 * when converting the FROM clause.
 *
 * Returns 0 on success and -1 on error.
 */
static int
check_join_types(const struct ast_source_list *ast)
{
	for (const struct stailq_entry *node = ast->head.first.value;
	     node != NULL; node = node->next.value) {
		int type = stailq_entry(node, struct ast_source,
					link)->join_type;
		if ((type & JT_INNER) != 0 && (type & JT_OUTER) != 0) {
			diag_set(ClientError, ER_SQL_PARSER_GENERIC,
				 "JOIN cannot be both OUTER and INNER");
			return -1;
		}
		if ((type & JT_OUTER) != 0 &&
		    (type & (JT_LEFT | JT_RIGHT)) != JT_LEFT) {
			diag_set(ClientError, ER_UNSUPPORTED, "Tarantool",
				 "RIGHT and FULL OUTER JOINs");
			return -1;
		}
	}
	return 0;
}

/**
 * Resolve the given AST SELECT statement, which is a source of a SELECT in the
 * current scope, into the given rast_select. As in the legacy code, its names
 * are looked up in the scopes enclosing the SELECT it is a source of.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_source_select(struct sql_resolve_context *ctx,
		      const struct ast_select *ast, struct rast_select *res)
{
	uint32_t ref_count = ctx->scope != NULL ? ctx->scope->ref_count : 0;
	if (resolve_select(ctx, ast, res) != 0)
		return -1;
	res->is_correlated = ctx->scope != NULL &&
			     ctx->scope->ref_count != ref_count;
	return 0;
}

/**
 * Resolve the SELECT of the given view into the given rast_source. As in the
 * legacy code, the view is compiled from its definition for every use.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_view(struct sql_resolve_context *ctx, const struct space *space,
	     struct rast_source *res)
{
	const struct ast_select *ast =
		sql_parse_view_ast(ctx->region, space->def->opts.sql);
	if (ast == NULL)
		return -1;
	res->select = xregion_alloc_object(ctx->region, struct rast_select);
	return resolve_source_select(ctx, ast, res->select);
}

/**
 * Find the space with the given name, as sql_space_by_token() does: an unquoted
 * name is also looked up in its legacy form, unless the given flag is set.
 *
 * Returns NULL and sets an error if not found.
 */
static struct space *
find_space(struct sql_resolve_context *ctx, const struct Token *name,
	   bool is_exact)
{
	const char *str = sql_name_temp(ctx->region, name->z, name->n);
	struct space *space = space_by_name0(str);
	if (space == NULL && !is_exact && name->z[0] != '"') {
		space = space_by_name0(sql_legacy_name_temp(ctx->region,
							    name->z, name->n));
	}
	if (space == NULL)
		diag_set(ClientError, ER_NO_SUCH_SPACE, str);
	return space;
}

/**
 * Look up the space with the given name and check it, as sql_lookup_space()
 * does. The name is looked up as find_space() does.
 *
 * Returns NULL on error.
 */
static struct space *
lookup_space(struct sql_resolve_context *ctx, const struct Token *name,
	     bool is_exact)
{
	struct space *space = find_space(ctx, name, is_exact);
	if (space == NULL)
		return NULL;
	if (sql_space_def_check_format(space->def) != 0)
		return NULL;
	if (space->index_count == 0 && !space->def->opts.is_view) {
		diag_set(ClientError, ER_UNSUPPORTED, "SQL",
			 "spaces without primary key");
		return NULL;
	}
	return space;
}

/**
 * Resolve the given INDEXED BY or NOT INDEXED clause of a source with the given
 * space into the given rast_source. The index is looked up as in
 * sqlIndexedByLookup().
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_indexed_by(struct sql_resolve_context *ctx, struct space *space,
		   const struct Token *indexed_by, struct rast_source *res)
{
	/* NOT INDEXED is a token of length 1 without text, see parse.y. */
	res->is_not_indexed = indexed_by->n == 1 && indexed_by->z == NULL;
	if (indexed_by->n == 0 || res->is_not_indexed)
		return 0;
	res->index_id = sql_index_id_by_token(space, indexed_by);
	if (res->index_id == UINT32_MAX) {
		diag_set(ClientError, ER_NO_SUCH_INDEX_NAME,
			 sql_name_temp(ctx->region, indexed_by->z,
				       indexed_by->n),
			 space->def->name);
		return -1;
	}
	return 0;
}

/**
 * Resolve the given AST source, which is a table or a view, into the given
 * rast_source. The space is looked up and checked as in sql_lookup_space(),
 * the SELECT of a view is resolved, and the index given by INDEXED BY is
 * looked up as in sqlIndexedByLookup().
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_space_source(struct sql_resolve_context *ctx,
		     const struct ast_source *ast, struct rast_source *res)
{
	struct space *space = lookup_space(ctx, &ast->name, false);
	if (space == NULL)
		return -1;
	if (space->def->opts.is_view) {
		if (resolve_view(ctx, space, res) != 0)
			return -1;
	}
	res->space_id = space->def->id;
	return resolve_indexed_by(ctx, space, &ast->indexed_by, res);
}

/**
 * Create the definition of a subquery source with the given name from the
 * given AST and resolved leftmost parts of its SELECT, as selectExpander()
 * does. As in sqlColumnsFromExprList(), a field is named after the given
 * column name, if the names are given, or else after the name of its column,
 * which is a column reference as written if it has no alias. The AST part is
 * not needed if all columns have names. The types of the fields are unknown
 * until the SELECT is built.
 */
static const struct space_def *
subquery_def_new(struct sql_resolve_context *ctx, const char *name,
		 const struct ast_select *ast, const struct rast_select *select,
		 const struct ast_id_list *names)
{
	const struct rast_expr_list *columns = &select->columns;
	size_t name_len = strlen(name);
	size_t size = sizeof(struct space_def) + name_len + 1;
	struct space_def *def = xregion_aligned_alloc(ctx->region, size,
						      alignof(*def));
	memset(def, 0, sizeof(*def));
	memcpy(def->name, name, name_len + 1);
	def->field_count = columns->len;
	def->fields = xregion_alloc_array(ctx->region, struct field_def,
					  columns->len);
	for (uint32_t i = 0; i < columns->len; ++i) {
		struct field_def *field = &def->fields[i];
		*field = field_def_default;
		field->nullable_action = ON_CONFLICT_ACTION_NONE;
		field->is_nullable = true;
		field->name = (char *)columns->exprs[i].name;
	}
	if (names != NULL) {
		assert(names->len == columns->len);
		struct field_def *field = def->fields;
		for (const struct stailq_entry *node = names->head.first.value;
		     node != NULL; node = node->next.value) {
			const struct Token *id =
				&stailq_entry(node, struct ast_id_entry,
					      link)->id;
			(field++)->name = sql_name_temp(ctx->region, id->z,
							id->n);
		}
		return def;
	}
	if (ast == NULL)
		return def;
	uint32_t j = 0;
	for (const struct stailq_entry *node = ast->columns->head.first.value;
	     node != NULL; node = node->next.value) {
		const struct ast_expr *e =
			stailq_entry(node, struct ast_expr_list_entry,
				     link)->expr;
		if (!ast_is_asterisk(e)) {
			struct field_def *field = &def->fields[j++];
			if (field->name != NULL)
				continue;
			while (e->op == TK_PARENTHESES)
				e = e->left;
			if (e->op == TK_DOT)
				e = e->right;
			assert(e->op == TK_ID);
			field->name = sql_name_temp(ctx->region, e->str,
						    e->len);
			continue;
		}
		const struct rast_source_list *sources = &select->sources;
		for (uint32_t i = 0; i < sources->len; ++i) {
			if (asterisk_matches_source(ctx, e,
						    &sources->sources[i]))
				j += asterisk_column_count(ctx, e, sources, i);
		}
	}
	assert(j == columns->len);
	return def;
}

/**
 * Resolve the given AST SELECT of a subquery source into the given
 * rast_source. As in selectExpander(), the source is named after its address.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_subquery_source(struct sql_resolve_context *ctx,
			const struct ast_select *ast, struct rast_source *res)
{
	res->select = xregion_alloc_object(ctx->region, struct rast_select);
	if (resolve_source_select(ctx, ast, res->select) != 0)
		return -1;
	const struct rast_select *select = res->select;
	while (select->prior != NULL)
		select = select->prior;
	const struct ast_select *leftmost =
		rlist_entry(ast->link.next, struct ast_select, link);
	const char *name = tt_sprintf("sql_sq_%llX", (long long)res);
	res->def = subquery_def_new(ctx, name, leftmost, select, NULL);
	return 0;
}

/** Return the table with the given position of the given WITH clause. */
static const struct ast_with_entry *
cte_entry(const struct sql_resolve_with *with, uint32_t pos)
{
	const struct stailq_entry *node = with->ast->head.first.value;
	for (uint32_t i = 0; i < pos; ++i)
		node = node->next.value;
	return stailq_entry(node, struct ast_with_entry, link);
}

/**
 * Check that the number of the given columns of the leftmost part of the
 * SELECT of the table with the given position of the given WITH clause is
 * the number of the column names of the table, if they are given, as
 * withExpand() does.
 *
 * Returns 0 on success and -1 on error.
 */
static int
check_cte_columns(const struct sql_resolve_with *with, uint32_t pos,
		  const struct rast_expr_list *columns)
{
	const struct ast_id_list *names = cte_entry(with, pos)->columns;
	if (names == NULL || names->len == columns->len)
		return 0;
	diag_set(ClientError, ER_SQL_PARSER_GENERIC,
		 tt_sprintf("table %s has %u values for %u columns",
			    with->names[pos], columns->len, names->len));
	return -1;
}

/**
 * Check if the given compound SELECT is turned into a SELECT from a subquery,
 * see convert_compound_select().
 */
static bool
ast_is_converted_compound(const struct ast_select *ast);

/**
 * Find the table with the given name of the visible WITH clauses, as
 * searchWith() does: the innermost WITH clause first.
 *
 * Returns the WITH clause and sets the position of the table, or returns NULL
 * if there is none.
 */
static struct sql_resolve_with *
find_cte(struct sql_resolve_context *ctx, const char *name, uint32_t *pos)
{
	for (struct sql_resolve_with *with = ctx->with; with != NULL;
	     with = with->parent) {
		for (uint32_t i = 0; i < with->ast->len; ++i) {
			if (strcmp(with->names[i], name) == 0) {
				*pos = i;
				return with;
			}
		}
	}
	return NULL;
}

/**
 * Check if the given AST source is a reference to the recursive table of a WITH
 * clause with the given name from its SELECT, as withExpand() checks: the name
 * of the source is the name of the table, compared case-insensitively.
 */
static bool
ast_is_recursive_ref(struct sql_resolve_context *ctx,
		     const struct ast_source *src, const char *name)
{
	if (src->select != NULL || src->name.n == 0)
		return false;
	const char *src_name = sql_name_temp(ctx->region, src->name.z,
					     src->name.n);
	return sqlStrICmp(src_name, name) == 0;
}

/**
 * Return the number of the references to the table of a WITH clause with the
 * given name from the given AST SELECT of the table, as withExpand() counts
 * them: the sources of the rightmost part of a SELECT joined by UNION or UNION
 * ALL, see ast_is_recursive_ref().
 */
static uint32_t
ast_cte_recursive_refs(struct sql_resolve_context *ctx,
		       const struct ast_select *ast, const char *name)
{
	if ((ast->op != TK_ALL && ast->op != TK_UNION) || ast->sources == NULL)
		return 0;
	uint32_t res = 0;
	for (const struct stailq_entry *node = ast->sources->head.first.value;
	     node != NULL; node = node->next.value) {
		const struct ast_source *src =
			stailq_entry(node, struct ast_source, link);
		if (ast_is_recursive_ref(ctx, src, name))
			++res;
	}
	return res;
}

/**
 * Resolve the given AST source, which refers to the table with the given
 * position of the given WITH clause, into the given rast_source, as
 * withExpand() does. As for a view, the SELECT of the table is resolved for
 * every use, in the scope enclosing the SELECT using it and with the given
 * WITH clause innermost. A reference to the table while its SELECT is being
 * resolved is an error. The fields of the table are named after the given
 * column names, if any, or else after the columns of the SELECT.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_cte_source(struct sql_resolve_context *ctx,
		   const struct ast_source *ast, struct sql_resolve_with *with,
		   uint32_t pos, struct rast_source *res)
{
	const char *name = with->names[pos];
	if (with->errors[pos] != NULL) {
		diag_set(ClientError, ER_SQL_PARSER_GENERIC,
			 tt_sprintf(with->errors[pos], name));
		return -1;
	}
	const struct ast_with_entry *entry = cte_entry(with, pos);
	const struct ast_select *body = entry->select;
	uint32_t refs = ast_cte_recursive_refs(ctx, body, name);
	if (refs > 1) {
		diag_set(ClientError, ER_SQL_PARSER_GENERIC,
			 tt_sprintf("multiple references to recursive table: "
				    "%s", name));
		return -1;
	}
	with->is_used[pos] = true;
	with->errors[pos] = "circular reference: %s";
	struct sql_resolve_with *saved = ctx->with;
	ctx->with = with;
	ctx->cte_with = with;
	ctx->cte_pos = pos;
	ctx->cte_res = res;
	ctx->cte_is_recursive = refs > 0;
	res->select = xregion_alloc_object(ctx->region, struct rast_select);
	int rc = resolve_source_select(ctx, body, res->select);
	ctx->with = saved;
	with->errors[pos] = NULL;
	if (rc != 0)
		return rc;
	const struct rast_select *select = res->select;
	while (select->prior != NULL)
		select = select->prior;
	/*
	 * The columns of a converted SELECT are the columns an asterisk is
	 * expanded into, which have names.
	 */
	const struct ast_select *leftmost =
		(select->flags & SF_Converted) != 0 ? NULL :
		rlist_entry(body->link.next, struct ast_select, link);
	/* The definition of a recursive table is created with its SELECT. */
	if (res->def == NULL) {
		res->def = subquery_def_new(ctx, name, leftmost, select,
					    entry->columns);
	}
	res->is_cte = true;
	/* NOT INDEXED is a token of length 1 without text, see parse.y. */
	res->is_not_indexed = ast->indexed_by.n == 1 &&
			      ast->indexed_by.z == NULL;
	if (ast->indexed_by.n > 0 && !res->is_not_indexed) {
		diag_set(ClientError, ER_NO_SUCH_INDEX_NAME,
			 sql_name_temp(ctx->region, ast->indexed_by.z,
				       ast->indexed_by.n), name);
		return -1;
	}
	return 0;
}

/**
 * Resolve the given AST source, which is a table, a view, a subquery or a
 * table of a WITH clause, into the given rast_source. As in the legacy code, a
 * table of a WITH clause is looked up before the spaces.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_source(struct sql_resolve_context *ctx, const struct ast_source *ast,
	       struct rast_source *res)
{
	memset(res, 0, sizeof(*res));
	res->index_id = UINT32_MAX;
	res->cursor = -1;
	struct sql_resolve_with *with = NULL;
	uint32_t pos = 0;
	if (ast->select == NULL) {
		const char *name = sql_name_temp(ctx->region, ast->name.z,
						 ast->name.n);
		with = find_cte(ctx, name, &pos);
	}
	int rc = with != NULL ?
		 resolve_cte_source(ctx, ast, with, pos, res) :
		 ast->select != NULL ?
		 resolve_subquery_source(ctx, ast->select, res) :
		 resolve_space_source(ctx, ast, res);
	if (rc != 0)
		return rc;
	if (ast->alias.n > 0) {
		res->alias = sql_name_temp(ctx->region, ast->alias.z,
					   ast->alias.n);
	}
	res->disallow_scan = ast->disallow_scan;
	return 0;
}

/**
 * Resolve the NATURAL join of the source with the given position in the given
 * sources, as sqlProcessJoin() does: every field of the source is joined to
 * the field with exactly the same name of the leftmost source having one.
 */
static void
resolve_natural_join(struct sql_resolve_context *ctx,
		     struct rast_source_list *sources, uint32_t source)
{
	struct rast_source *right = &sources->sources[source];
	const struct space_def *def = rast_source_def(right);
	right->join_columns =
		xregion_alloc_array(ctx->region, struct rast_join_column,
				    def->field_count);
	for (uint32_t f = 0; f < def->field_count; ++f) {
		const char *name = def->fields[f].name;
		for (uint32_t i = 0; i < source; ++i) {
			uint32_t fieldno =
				rast_source_fieldno(&sources->sources[i], name,
						    NULL);
			if (fieldno == UINT32_MAX)
				continue;
			struct rast_join_column *col =
				right->join_columns + right->join_column_count;
			++right->join_column_count;
			col->left_source = i;
			col->left_fieldno = fieldno;
			col->right_fieldno = f;
			break;
		}
	}
}

/**
 * Resolve the given AST USING clause of the source with the given position in
 * the given sources, as sqlProcessJoin() does: every name is joined to the
 * field of the leftmost source having it. As in the legacy code, a field is
 * looked up by the name as is and, if the name is not quoted, by the name in
 * upper case.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_using(struct sql_resolve_context *ctx, const struct ast_id_list *ast,
	      struct rast_source_list *sources, uint32_t source)
{
	struct rast_source *right = &sources->sources[source];
	right->using_names = xregion_alloc_array(ctx->region, const char *,
						 ast->len);
	right->join_columns = xregion_alloc_array(ctx->region,
						  struct rast_join_column,
						  ast->len);
	for (const struct stailq_entry *node = ast->head.first.value;
	     node != NULL; node = node->next.value) {
		const struct Token *id =
			&stailq_entry(node, struct ast_id_entry, link)->id;
		const char *name = sql_name_temp(ctx->region, id->z, id->n);
		const char *legacy_name = NULL;
		if (id->z[0] != '"') {
			legacy_name = sql_legacy_name_temp(ctx->region, id->z,
							   id->n);
		}
		right->using_names[right->using_count++] = name;
		struct rast_join_column *col =
			&right->join_columns[right->join_column_count++];
		col->right_fieldno = rast_source_fieldno(right, name,
							 legacy_name);
		col->left_fieldno = UINT32_MAX;
		for (uint32_t i = 0; i < source; ++i) {
			col->left_source = i;
			col->left_fieldno =
				rast_source_fieldno(&sources->sources[i], name,
						    legacy_name);
			if (col->left_fieldno != UINT32_MAX)
				break;
		}
		if (col->left_fieldno == UINT32_MAX ||
		    col->right_fieldno == UINT32_MAX) {
			diag_set(ClientError, ER_SQL_PARSER_GENERIC,
				 tt_sprintf("cannot join using column %s - "
					    "column not present in both "
					    "tables", name));
			return -1;
		}
	}
	return 0;
}

/**
 * Resolve the NATURAL joins and the USING clauses of the given AST FROM
 * clause into the given rast_source_list, checking them as sqlProcessJoin()
 * does.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_joins(struct sql_resolve_context *ctx,
	      const struct ast_source_list *ast, struct rast_source_list *res)
{
	uint32_t i = 0;
	for (const struct stailq_entry *node = ast->head.first.value;
	     node != NULL; node = node->next.value, ++i) {
		const struct ast_source *src =
			stailq_entry(node, struct ast_source, link);
		if (i == 0)
			continue;
		if ((src->join_type & JT_NATURAL) != 0) {
			if (src->join_on != NULL || src->join_using != NULL) {
				diag_set(ClientError, ER_SQL_PARSER_GENERIC,
					 "a NATURAL join may not have an ON "
					 "or USING clause");
				return -1;
			}
			resolve_natural_join(ctx, res, i);
		}
		if (src->join_on != NULL && src->join_using != NULL) {
			diag_set(ClientError, ER_SQL_PARSER_GENERIC,
				 "cannot have both ON and USING clauses in "
				 "the same join");
			return -1;
		}
		if (src->join_using != NULL &&
		    resolve_using(ctx, src->join_using, res, i) != 0)
			return -1;
	}
	return 0;
}

/**
 * Resolve the given AST source, which refers to the recursive table of a WITH
 * clause with the given source from the rightmost part of the SELECT of the
 * table, into the given rast_source. As in the legacy code, the source shares
 * the definition of the table, and INDEXED BY of the source is ignored.
 */
static void
resolve_recursive_ref(struct sql_resolve_context *ctx,
		      const struct ast_source *ast,
		      const struct rast_source *cte, struct rast_source *res)
{
	memset(res, 0, sizeof(*res));
	res->index_id = UINT32_MAX;
	res->cursor = -1;
	res->def = cte->def;
	res->recursive = cte;
	if (ast->alias.n > 0) {
		res->alias = sql_name_temp(ctx->region, ast->alias.z,
					   ast->alias.n);
	}
	/* NOT INDEXED is a token of length 1 without text, see parse.y. */
	res->is_not_indexed = ast->indexed_by.n == 1 &&
			      ast->indexed_by.z == NULL;
	res->disallow_scan = ast->disallow_scan;
}

/**
 * Resolve the given AST FROM clause into the given rast_source_list. If the
 * sources are of the rightmost part of the SELECT of a recursive table of a
 * WITH clause, the references to the table are resolved as such.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_sources(struct sql_resolve_context *ctx,
		const struct ast_source_list *ast, struct rast_source_list *res)
{
	const struct rast_source *recursive = ctx->recursive_cte;
	ctx->recursive_cte = NULL;
	if (ast == NULL)
		return 0;
	if (check_join_types(ast) != 0)
		return -1;
	res->len = ast->len;
	res->sources = xregion_alloc_array(ctx->region, typeof(*res->sources),
					   res->len);
	uint32_t i = 0;
	for (const struct stailq_entry *node = ast->head.first.value;
	     node != NULL; node = node->next.value) {
		const struct ast_source *src =
			stailq_entry(node, struct ast_source, link);
		if (recursive != NULL &&
		    ast_is_recursive_ref(ctx, src, recursive->def->name)) {
			resolve_recursive_ref(ctx, src, recursive,
					      &res->sources[i]);
		} else if (resolve_source(ctx, src, &res->sources[i]) != 0) {
			return -1;
		}
		res->sources[i++].join_type = src->join_type;
	}
	return resolve_joins(ctx, ast, res);
}

/**
 * Check if the given compound SELECT is turned into a SELECT from a subquery,
 * as convertCompoundSelectToSubquery() does: it is not UNION ALL and has a
 * COLLATE clause in its ORDER BY.
 */
static bool
ast_is_converted_compound(const struct ast_select *ast)
{
	bool is_union_all = true;
	const struct rlist *link = &ast->link;
	do {
		const struct ast_select *part =
			rlist_entry(link, struct ast_select, link);
		if (part->op != TK_SELECT && part->op != TK_ALL)
			is_union_all = false;
		link = link->next;
	} while (link != &ast->link);
	if (is_union_all || ast->order_by == NULL)
		return false;
	for (const struct stailq_entry *node = ast->order_by->head.first.value;
	     node != NULL; node = node->next.value) {
		const struct ast_expr_list_entry *entry =
			stailq_entry(node, struct ast_expr_list_entry, link);
		if (ast_expr_has_collate(entry->expr))
			return true;
	}
	return false;
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
	bool is_nested = (ast->flags & SF_NestedFrom) != 0;
	return resolve_column_names(ctx, ast->columns, &res->sources,
				    is_nested, &res->columns);
}

/**
 * Resolve the WHERE clause of the given AST SELECT statement and the ON clauses
 * of its sources into the given rast_select. As in the legacy code, the ON
 * clauses and the columns joined by NATURAL or USING are a part of WHERE: they
 * are joined to it with AND in the order of the sources, and if WHERE or the
 * ON clause of an inner join is FALSE, the whole WHERE is FALSE, and the other
 * clauses are not resolved.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_where(struct sql_resolve_context *ctx, const struct ast_select *ast,
	      struct rast_select *res)
{
	bool is_false = ast->where != NULL && ast_is_false(ast->where);
	const struct stailq_entry *first =
		ast->sources != NULL ? ast->sources->head.first.value : NULL;
	for (const struct stailq_entry *node = first; node != NULL;
	     node = node->next.value) {
		const struct ast_source *src =
			stailq_entry(node, struct ast_source, link);
		if (src->join_on != NULL && (src->join_type & JT_OUTER) == 0 &&
		    ast_is_false(src->join_on))
			is_false = true;
	}
	if (is_false) {
		/* The joined columns are dropped from WHERE too. */
		for (uint32_t i = 0; i < res->sources.len; ++i)
			res->sources.sources[i].join_column_count = 0;
		res->where = xregion_alloc_object(ctx->region,
						  struct rast_expr);
		memset(res->where, 0, sizeof(*res->where));
		res->where->op = TK_FALSE;
		res->where->b = false;
		res->where->type = SQL_TYPE_BOOLEAN;
		res->where->height = 1;
		return 0;
	}
	uint32_t height = 0;
	if (ast->where != NULL) {
		res->where = xregion_alloc_object(ctx->region,
						  struct rast_expr);
		if (resolve_expr(ctx, ast->where, res->where) != 0)
			return -1;
		height = res->where->height;
	}
	uint32_t i = 0;
	for (const struct stailq_entry *node = first; node != NULL;
	     node = node->next.value, ++i) {
		const struct ast_source *src =
			stailq_entry(node, struct ast_source, link);
		/* A column of NATURAL or USING joins as `a = b` of height 2. */
		uint32_t count = res->sources.sources[i].join_column_count;
		for (uint32_t j = 0; j < count; ++j)
			height = height == 0 ? 2 : MAX(height, 2) + 1;
		if (src->join_on == NULL)
			continue;
		struct rast_expr *on = xregion_alloc_object(ctx->region,
							    struct rast_expr);
		res->sources.sources[i].on = on;
		if (resolve_expr(ctx, src->join_on, on) != 0)
			return -1;
		height = height == 0 ? on->height :
			 MAX(height, on->height) + 1;
	}
	return expr_check_height(height);
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
	if (resolve_limit(ctx, ast->offset, &res->offset) != 0)
		return -1;
	struct sql_resolve_scope scope = {
		.sources = &res->sources,
		.parent = outer,
		.is_agg_allowed = !ctx->is_insert_row,
	};
	ctx->is_insert_row = false;
	ctx->scope = &scope;
	int rc = resolve_column_list(ctx, ast->columns, &res->columns);
	if (rc != 0)
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
		if (rc != 0)
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
	/*
	 * WHERE can refer to the columns of the sources, to the result columns
	 * and to the names of the enclosing SELECTs, but cannot use aggregate
	 * functions.
	 */
	struct sql_resolve_scope where_scope = {
		.sources = &res->sources,
		.aliases = &res->columns,
		.parent = outer,
	};
	ctx->scope = &where_scope;
	rc = resolve_where(ctx, ast, res);
	if (rc != 0)
		return rc;
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
		if (rc != 0)
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
	if (rc != 0)
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
 * Turn the given resolved compound SELECT, resolved from the given AST one,
 * into a SELECT from a subquery, as convertCompoundSelectToSubquery() does:
 * the compound SELECT without its ORDER BY, LIMIT and OFFSET becomes the only
 * source of a SELECT of all its columns, which takes the ORDER BY, LIMIT and
 * OFFSET. The terms of ORDER BY, already replaced with the positions of the
 * columns, are resolved as in a SELECT that is not compound. The subquery is
 * correlated if the compound SELECT refers to the names of the given scope or
 * the scopes enclosing it, which had the given number of references before.
 *
 * Returns 0 on success and -1 on error.
 */
static int
convert_compound_select(struct sql_resolve_context *ctx,
			const struct ast_select *ast, struct rast_select *res,
			struct sql_resolve_scope *outer, uint32_t ref_count)
{
	struct rast_select *sub = xregion_alloc_object(ctx->region,
						       struct rast_select);
	*sub = *res;
	sub->is_correlated = outer != NULL && outer->ref_count != ref_count;
	memset(res, 0, sizeof(*res));
	res->op = TK_SELECT;
	res->flags = (ast->flags & ~(SF_Compound | SF_Distinct | SF_All |
				     SF_Values)) | SF_Converted;
	res->order_by = sub->order_by;
	res->limit = sub->limit;
	res->offset = sub->offset;
	memset(&sub->order_by, 0, sizeof(sub->order_by));
	sub->limit = NULL;
	sub->offset = NULL;
	res->sources.len = 1;
	res->sources.sources = xregion_alloc_object(ctx->region,
						    struct rast_source);
	struct rast_source *src = &res->sources.sources[0];
	memset(src, 0, sizeof(*src));
	src->index_id = UINT32_MAX;
	src->cursor = -1;
	src->select = sub;
	const struct rast_select *select = sub;
	while (select->prior != NULL)
		select = select->prior;
	const struct ast_select *leftmost =
		rlist_entry(ast->link.next, struct ast_select, link);
	const char *name = tt_sprintf("sql_sq_%llX", (long long)src);
	src->def = subquery_def_new(ctx, name, leftmost, select, NULL);

	struct sql_resolve_scope scope;
	memset(&scope, 0, sizeof(scope));
	scope.sources = &res->sources;
	scope.parent = outer;
	scope.ref_depth = UINT32_MAX;
	ctx->scope = &scope;
	struct ast_expr *asterisk = ast_expr_new(ctx->region, "*", 1,
						 TK_ASTERISK);
	struct ast_expr_list *columns =
		ast_expr_list_append(ctx->region, NULL, asterisk);
	if (resolve_column_names(ctx, columns, &res->sources, false,
				 &res->columns) != 0 ||
	    resolve_column_list(ctx, columns, &res->columns) != 0)
		return -1;
	for (uint32_t i = 0; i < res->order_by.len; ++i) {
		struct rast_expr_list_entry *item = &res->order_by.exprs[i];
		if (resolve_order_by_col(ctx, &res->columns,
					 item->order_by_col - 1,
					 &item->expr) != 0)
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
	struct sql_resolve_with *cte_with = ctx->cte_with;
	uint32_t cte_pos = ctx->cte_pos;
	struct rast_source *cte_res = ctx->cte_res;
	bool is_recursive = ctx->cte_is_recursive;
	ctx->cte_with = NULL;
	ctx->cte_res = NULL;
	ctx->cte_is_recursive = false;
	uint32_t ref_count = ctx->scope != NULL ? ctx->scope->ref_count : 0;
	const struct rlist *head = &ast->link;
	bool is_compound = head->next != head;
	/*
	 * As withExpand() does, the parts of the SELECT of a table of a WITH
	 * clause joined by UNION or UNION ALL to the left of the rightmost one
	 * are set up first, while a reference to the table is a circular one,
	 * and then the rightmost part, while a reference to the table is a
	 * recursive one. The definition of a recursive table is created before
	 * its rightmost part, which refers to it. A converted SELECT is not
	 * compound, and the SELECT of a recursive table is not converted.
	 */
	bool is_rightmost_last = cte_with != NULL &&
				 (ast->op == TK_ALL || ast->op == TK_UNION) &&
				 (is_recursive ||
				  !ast_is_converted_compound(ast));
	const char *subquery_error = is_recursive ?
				     "multiple recursive references: %s" :
				     "recursive reference in a subquery: %s";
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
	if (!is_rightmost_last && resolve_select_names(ctx, ast, res) != 0)
		return -1;
	struct rast_select *prior = NULL;
	struct rast_select **tail = &prior;
	for (const struct rlist *l = head->prev; l != head; l = l->prev) {
		const struct ast_select *part =
			rlist_entry(l, struct ast_select, link);
		*tail = xregion_alloc_object(ctx->region, struct rast_select);
		if (resolve_select_names(ctx, part, *tail) != 0)
			return -1;
		tail = &(*tail)->prior;
	}
	if (is_rightmost_last) {
		const struct rast_select *left = prior;
		while (left->prior != NULL)
			left = left->prior;
		if (check_cte_columns(cte_with, cte_pos, &left->columns) != 0)
			return -1;
		if (is_recursive) {
			const struct ast_select *leftmost =
				rlist_entry(head->next, struct ast_select,
					    link);
			cte_res->def = subquery_def_new(
				ctx, cte_with->names[cte_pos], leftmost, left,
				cte_entry(cte_with, cte_pos)->columns);
			ctx->recursive_cte = cte_res;
		}
		cte_with->errors[cte_pos] = subquery_error;
		int rc = resolve_select_names(ctx, ast, res);
		ctx->recursive_cte = NULL;
		if (rc != 0)
			return -1;
		if (is_recursive)
			res->flags |= SF_Recursive;
	}
	res->prior = prior;
	res->is_rightmost_last = is_rightmost_last;
	if (cte_with != NULL && !is_rightmost_last) {
		const struct rast_select *left = res;
		while (left->prior != NULL)
			left = left->prior;
		if (check_cte_columns(cte_with, cte_pos, &left->columns) != 0)
			return -1;
	}

	struct rast_select *next;
	struct sql_resolve_scope *outer = ctx->scope;
	if (resolve_select_clauses(ctx, ast, res, is_compound) != 0)
		return -1;
	if (is_rightmost_last)
		cte_with->errors[cte_pos] = "circular reference: %s";
	next = res;
	for (const struct rlist *l = head->prev; l != head; l = l->prev) {
		const struct ast_select *part =
			rlist_entry(l, struct ast_select, link);
		struct rast_select *cur = next->prior;
		ctx->scope = outer;
		if (resolve_select_clauses(ctx, part, cur, false) != 0)
			return -1;
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
	if (is_rightmost_last)
		cte_with->errors[cte_pos] = subquery_error;
	ctx->scope = outer;
	if (resolve_compound_order_by(ctx, ast->order_by, res) != 0)
		return -1;
	if (check_compound_parts(res) != 0)
		return -1;
	if (is_recursive || !ast_is_converted_compound(ast))
		return 0;
	return convert_compound_select(ctx, ast, res, outer, ref_count);
}

/**
 * Make the given AST WITH clause the innermost visible one, using the given
 * memory for it. As sqlWithAdd() does, the names of the tables of the clause
 * are checked to be unique.
 *
 * Returns 0 on success and -1 on error.
 */
static int
push_with(struct sql_resolve_context *ctx, const struct ast_with_list *ast,
	  struct sql_resolve_with *res)
{
	res->ast = ast;
	res->names = xregion_alloc_array(ctx->region, const char *, ast->len);
	res->errors = xregion_alloc_array(ctx->region, const char *,
					  ast->len);
	res->is_used = xregion_alloc_array(ctx->region, bool, ast->len);
	const char *duplicate = NULL;
	uint32_t i = 0;
	for (const struct stailq_entry *node = ast->head.first.value;
	     node != NULL; node = node->next.value, ++i) {
		const struct Token *name =
			&stailq_entry(node, struct ast_with_entry, link)->name;
		res->names[i] = sql_name_temp(ctx->region, name->z, name->n);
		res->errors[i] = NULL;
		res->is_used[i] = false;
		for (uint32_t j = 0; j < i; ++j) {
			if (strcmp(res->names[j], res->names[i]) == 0)
				duplicate = res->names[i];
		}
	}
	if (duplicate != NULL) {
		diag_set(ClientError, ER_SQL_PARSER_GENERIC,
			 tt_sprintf("Ambiguous table name in WITH query: %s",
				    duplicate));
		return -1;
	}
	res->parent = ctx->with;
	ctx->with = res;
	return 0;
}

/**
 * Check the SELECTs of the tables of the given WITH clause which have not been
 * used. The legacy code converts them, though it never resolves them, so they
 * are converted the same way to report the same errors. A table of a SELECT
 * resolved several times is converted once.
 *
 * Returns 0 on success and -1 on error.
 */
static int
check_unused_ctes(struct sql_resolve_context *ctx,
		  const struct sql_resolve_with *with)
{
	uint32_t i = 0;
	for (const struct stailq_entry *node = with->ast->head.first.value;
	     node != NULL; node = node->next.value, ++i) {
		if (with->is_used[i])
			continue;
		const struct ast_with_entry *entry =
			stailq_entry(node, struct ast_with_entry, link);
		const struct sql_resolve_converted *c = ctx->converted;
		while (c != NULL && c->entry != entry)
			c = c->next;
		if (c != NULL)
			continue;
		struct sql_resolve_converted *converted =
			xregion_alloc_object(ctx->region,
					     struct sql_resolve_converted);
		converted->entry = entry;
		converted->next = ctx->converted;
		ctx->converted = converted;
		struct Select *select =
			select_from_ast(ctx->parser,
					(struct ast_select *)entry->select);
		if (select == NULL)
			return -1;
		sql_select_delete(select);
	}
	return 0;
}

static int
resolve_select(struct sql_resolve_context *ctx, const struct ast_select *ast,
	       struct rast_select *res)
{
	struct sql_resolve_scope *outer = ctx->scope;
	struct sql_resolve_with *outer_with = ctx->with;
	struct sql_resolve_with with;
	int rc = 0;
	if (ast->with != NULL)
		rc = push_with(ctx, ast->with, &with);
	if (rc == 0)
		rc = resolve_compound_select(ctx, ast, res);
	if (rc == 0 && ast->with != NULL)
		rc = check_unused_ctes(ctx, &with);
	ctx->scope = outer;
	ctx->with = outer_with;
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

/**
 * Return the space whose tuples are copied as they are by the given INSERT
 * with the given resolved SELECT producing its rows, as sqlInsert() and
 * xferOptimization() check: SELECT * from a single space without any other
 * clauses, and no column list and no WITH clauses, or NULL.
 */
static struct space *
insert_xfer_source(const struct ast_insert *ast,
		   const struct rast_select *select)
{
	const struct ast_select *sel = ast->select;
	if (ast->columns != NULL || ast->with != NULL || sel->with != NULL)
		return NULL;
	if (select->prior != NULL || select->sources.len != 1 ||
	    select->where != NULL || select->group_by.len != 0 ||
	    select->order_by.len != 0 || select->limit != NULL ||
	    (select->flags & SF_Distinct) != 0)
		return NULL;
	const struct rast_source *src = &select->sources.sources[0];
	if (src->space_id == 0 || src->is_cte)
		return NULL;
	if (sel->columns == NULL || sel->columns->len != 1)
		return NULL;
	const struct ast_expr_list_entry *col =
		stailq_first_entry(&sel->columns->head,
				   struct ast_expr_list_entry, link);
	if (col->expr->op != TK_ASTERISK)
		return NULL;
	return space_by_id(src->space_id);
}

/**
 * Resolve the given AST INSERT statement into the given rast_insert. As
 * sqlInsert() does, the space and the column list are checked before the
 * rows, and the number of values in a row after them.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_insert(struct sql_resolve_context *ctx, const struct ast_insert *ast,
	       struct rast_insert *res)
{
	memset(res, 0, sizeof(*res));
	res->action = ast->action;
	/*
	 * As targetSrcList() does, the space of a statement of a trigger is
	 * looked up by its exact name.
	 */
	res->space = lookup_space(ctx, &ast->table,
				  ctx->parser->triggered_space != NULL);
	if (res->space == NULL)
		return -1;
	struct space_def *def = res->space->def;
	int tmask;
	if (def->opts.is_view &&
	    sql_triggers_exist(def, TK_INSERT, NULL, ctx->parser->sql_flags,
			       &tmask) == NULL) {
		diag_set(ClientError, ER_ALTER_SPACE, def->name,
			 "space is a view");
		return -1;
	}
	const char *name = sql_name_temp(ctx->region, ast->table.z,
					 ast->table.n);
	if (ast->columns != NULL) {
		res->fieldno_count = ast->columns->len;
		res->fieldnos = xregion_alloc_array(ctx->region, uint32_t,
						    res->fieldno_count);
		uint32_t i = 0;
		for (const struct stailq_entry *node =
			ast->columns->head.first.value;
		     node != NULL; node = node->next.value) {
			const struct Token *id =
				&stailq_entry(node, struct ast_id_entry,
					      link)->id;
			const char *col = sql_name_temp(ctx->region, id->z,
							id->n);
			uint32_t fieldno = space_def_fieldno(def, col);
			if (fieldno == UINT32_MAX && id->z[0] != '"') {
				const char *legacy =
					sql_legacy_name_temp(ctx->region,
							     id->z, id->n);
				fieldno = space_def_fieldno(def, legacy);
			}
			if (fieldno == UINT32_MAX) {
				diag_set(ClientError,
					 ER_NO_SUCH_FIELD_NAME_IN_SPACE, col,
					 name);
				return -1;
			}
			for (uint32_t j = 0; j < i; ++j) {
				if (res->fieldnos[j] != fieldno)
					continue;
				const char *err = "table id list: duplicate "
						  "column name %s";
				diag_set(ClientError, ER_SQL_PARSER_GENERIC,
					 tt_sprintf(err, col));
				return -1;
			}
			res->fieldnos[i++] = fieldno;
		}
	}
	uint32_t value_count = 0;
	if (ast->select != NULL) {
		struct sql_resolve_with *outer_with = ctx->with;
		struct sql_resolve_with with;
		if (ast->with != NULL && push_with(ctx, ast->with, &with) != 0)
			return -1;
		res->select = xregion_alloc_object(ctx->region,
						   struct rast_select);
		const struct rlist *head = &ast->select->link;
		ctx->is_insert_row = (ast->select->flags & SF_Values) != 0 &&
				     head->next == head;
		int rc = resolve_select(ctx, ast->select, res->select);
		if (rc == 0 && ast->with != NULL)
			rc = check_unused_ctes(ctx, &with);
		ctx->with = outer_with;
		if (rc != 0)
			return rc;
		value_count = res->select->columns.len;
		res->xfer_source = insert_xfer_source(ast, res->select);
	}
	if (ast->columns == NULL && value_count != 0 &&
	    value_count != def->field_count) {
		const char *err =
			"table %s has %d columns but %d values were supplied";
		diag_set(ClientError, ER_SQL_PARSER_GENERIC,
			 tt_sprintf(err, name, def->field_count, value_count));
		return -1;
	}
	if (ast->columns != NULL && value_count != res->fieldno_count) {
		const char *err = "%d values for %d columns";
		diag_set(ClientError, ER_SQL_PARSER_GENERIC,
			 tt_sprintf(err, value_count, res->fieldno_count));
		return -1;
	}
	return 0;
}

/**
 * Resolve the given AST CREATE VIEW statement into the given rast_create_view.
 * As in sql_create_view(), the names of the columns, the absence of bound
 * variables and the name of the view are checked before the SELECT, which is
 * resolved without the session SQL flags.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_create_view(struct sql_resolve_context *ctx,
		    const struct ast_create_view *ast,
		    struct rast_create_view *res)
{
	res->name = ast->name;
	res->columns = ast->columns;
	res->if_not_exists = ast->if_not_exists;
	int rc = 0;
	if (ast->columns != NULL) {
		for (const struct stailq_entry *node =
			ast->columns->head.first.value;
		     node != NULL; node = node->next.value) {
			const struct Token *id =
				&stailq_entry(node, struct ast_id_entry,
					      link)->id;
			if (id->n == 0)
				continue;
			const char *name = sql_name_temp(ctx->region, id->z,
							 id->n);
			if (sql_check_identifier_name(name, strlen(name)) != 0)
				rc = -1;
		}
	}
	if (rc != 0)
		return -1;
	if (ctx->parser->nVar > 0) {
		diag_set(ClientError, ER_SQL_PARSER_GENERIC,
			 "Parameters are not allowed in views");
		return -1;
	}
	const char *name = sql_name_temp(ctx->region, ast->name.z,
					 ast->name.n);
	if (sql_check_identifier_name(name, strlen(name)) != 0)
		return -1;
	res->select = xregion_alloc_object(ctx->region, struct rast_select);
	uint32_t sql_flags = ctx->parser->sql_flags;
	ctx->parser->sql_flags = 0;
	rc = resolve_select(ctx, ast->select, res->select);
	ctx->parser->sql_flags = sql_flags;
	return rc;
}

/**
 * Resolve the given AST TRUNCATE statement into the given rast_truncate.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_truncate(struct sql_resolve_context *ctx,
		 const struct ast_truncate *ast, struct rast_truncate *res)
{
	res->space = find_space(ctx, &ast->table, false);
	if (res->space == NULL)
		return -1;
	if (res->space->def->opts.is_view) {
		const char *err = tt_sprintf("can not truncate space '%s' "
					     "because space is a view",
					     res->space->def->name);
		diag_set(ClientError, ER_SQL_EXECUTE, err);
		return -1;
	}
	return 0;
}

/**
 * Resolve the given AST DROP TABLE or DROP VIEW statement into the given
 * rast_drop_table.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_drop_table(const struct ast_drop_table *ast, bool is_view,
		   struct rast_drop_table *res)
{
	res->is_view = is_view;
	res->space = sql_space_by_token(&ast->name);
	if (res->space == NULL) {
		if (ast->if_exists)
			return 0;
		diag_set(ClientError, ER_NO_SUCH_SPACE,
			 sql_tt_name_from_token(&ast->name));
		return -1;
	}
	if (is_view == res->space->def->opts.is_view)
		return 0;
	diag_set(ClientError, ER_DROP_SPACE, sql_tt_name_from_token(&ast->name),
		 is_view ? "use DROP TABLE" : "use DROP VIEW");
	return -1;
}

/**
 * Resolve the given AST DROP INDEX statement into the given rast_drop_index.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_drop_index(const struct ast_drop_index *ast,
		   struct rast_drop_index *res)
{
	res->index_id = UINT32_MAX;
	res->space = sql_space_by_token(&ast->table);
	if (res->space == NULL) {
		if (ast->if_exists)
			return 0;
		diag_set(ClientError, ER_NO_SUCH_SPACE,
			 sql_tt_name_from_token(&ast->table));
		return -1;
	}
	res->index_id = sql_index_id_by_token(res->space, &ast->name);
	if (res->index_id != UINT32_MAX || ast->if_exists)
		return 0;
	diag_set(ClientError, ER_NO_SUCH_INDEX_NAME,
		 sql_tt_name_from_token(&ast->name), res->space->def->name);
	return -1;
}

/**
 * Resolve the given AST ALTER TABLE RENAME statement into the given
 * rast_alter_rename.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_alter_rename(struct sql_resolve_context *ctx,
		     const struct ast_alter_rename *ast,
		     struct rast_alter_rename *res)
{
	res->new_name = sql_name_temp(ctx->region, ast->new_name.z,
				      ast->new_name.n);
	if (space_by_name0(res->new_name) != NULL) {
		diag_set(ClientError, ER_SPACE_EXISTS, res->new_name);
		return -1;
	}
	res->space = sql_space_by_token(&ast->old_name);
	if (res->space != NULL)
		return 0;
	diag_set(ClientError, ER_NO_SUCH_SPACE,
		 sql_tt_name_from_token(&ast->old_name));
	return -1;
}

/**
 * Resolve the tuple constraint with the given name and type of the space of
 * the given rast_drop_constraint. A constraint of any type is looked up among
 * the FOREIGN KEY and CHECK constraints and the indexes, and must be found in
 * only one of them.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_drop_tuple_constraint(const struct Token *name,
			      enum ast_property_type type,
			      struct rast_drop_constraint *res)
{
	const struct space *space = res->space;
	switch (type) {
	case SQL_AST_PROPERTY_CHECK:
		res->ck = sql_tuple_ck_by_token(space, name);
		break;
	case SQL_AST_PROPERTY_FOREIGN_KEY:
		res->fk = sql_tuple_fk_by_token(space, name);
		break;
	case SQL_AST_PROPERTY_PRIMARY_KEY:
		if (sql_index_id_by_token(space, name) == 0)
			res->index_id = 0;
		break;
	case SQL_AST_PROPERTY_UNIQUE:
		res->index_id = sql_index_id_by_token(space, name);
		if (res->index_id == 0)
			res->index_id = UINT32_MAX;
		break;
	default: {
		assert(type == SQL_AST_PROPERTY_ANY);
		res->fk = sql_tuple_fk_by_token(space, name);
		res->ck = sql_tuple_ck_by_token(space, name);
		if (res->fk != NULL && res->ck != NULL) {
			diag_set(ClientError, ER_SQL_EXECUTE,
				 tt_sprintf("ambiguous constraint name: '%s'",
					    res->fk->name));
			return -1;
		}
		res->index_id = sql_index_id_by_token(space, name);
		const struct tuple_constraint_def *def =
			res->fk != NULL ? res->fk : res->ck;
		if (res->index_id != UINT32_MAX && def != NULL) {
			diag_set(ClientError, ER_SQL_EXECUTE,
				 tt_sprintf("ambiguous constraint name: '%s'",
					    def->name));
			return -1;
		}
		break;
	}
	}
	if (res->fk != NULL || res->ck != NULL || res->index_id != UINT32_MAX)
		return 0;
	diag_set(ClientError, ER_NO_SUCH_CONSTRAINT,
		 sql_tt_name_from_token(name), space->def->name);
	return -1;
}

/**
 * Resolve the field constraint with the given name and type of the field of
 * the given rast_drop_constraint. A constraint of any type is looked up among
 * the FOREIGN KEY and CHECK constraints of the field, and must be found in
 * only one of them.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_drop_field_constraint(const struct Token *name,
			      enum ast_property_type type,
			      struct rast_drop_constraint *res)
{
	const struct space *space = res->space;
	const char *field_name = space->def->fields[res->fieldno].name;
	if (type != SQL_AST_PROPERTY_CHECK)
		res->fk = sql_field_fk_by_token(space, res->fieldno, name);
	if (type != SQL_AST_PROPERTY_FOREIGN_KEY)
		res->ck = sql_field_ck_by_token(space, res->fieldno, name);
	if (res->fk != NULL && res->ck != NULL) {
		const char *err = tt_sprintf("ambiguous constraint name: "
					     "'%s.%s'", field_name,
					     res->fk->name);
		diag_set(ClientError, ER_SQL_EXECUTE, err);
		return -1;
	}
	if (res->fk != NULL || res->ck != NULL)
		return 0;
	diag_set(ClientError, ER_NO_SUCH_CONSTRAINT,
		 tt_sprintf("%s.%s", field_name, sql_tt_name_from_token(name)),
		 space->def->name);
	return -1;
}

/**
 * Resolve the given AST ALTER TABLE DROP CONSTRAINT statement into the given
 * rast_drop_constraint.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_drop_constraint(const struct ast_alter_drop_constraint *ast,
			struct rast_drop_constraint *res)
{
	res->fieldno = UINT32_MAX;
	res->fk = NULL;
	res->ck = NULL;
	res->index_id = UINT32_MAX;
	res->space = sql_space_by_token(&ast->table);
	if (res->space == NULL) {
		diag_set(ClientError, ER_NO_SUCH_SPACE,
			 sql_tt_name_from_token(&ast->table));
		return -1;
	}
	if (ast->column.n == 0) {
		return resolve_drop_tuple_constraint(&ast->name, ast->type,
						     res);
	}
	res->fieldno = sql_fieldno_by_token(res->space, &ast->column);
	if (res->fieldno == UINT32_MAX) {
		diag_set(ClientError, ER_NO_SUCH_FIELD_NAME_IN_SPACE,
			 res->space->def->name,
			 sql_tt_name_from_token(&ast->column));
		return -1;
	}
	return resolve_drop_field_constraint(&ast->name, ast->type, res);
}

/**
 * Resolve the given AST PRAGMA statement into the given rast_pragma. The space
 * and the index are not required to exist.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_pragma(struct sql_resolve_context *ctx, const struct ast_pragma *ast,
	       struct rast_pragma *res)
{
	res->space = NULL;
	res->index = NULL;
	if (ast->table_name.n > 0)
		res->space = sql_space_by_token(&ast->table_name);
	if (res->space != NULL && ast->index_name.n > 0) {
		uint32_t index_id = sql_index_id_by_token(res->space,
							  &ast->index_name);
		if (index_id <= res->space->index_id_max)
			res->index = res->space->index_map[index_id];
	}
	const char *name = sql_name_temp(ctx->region, ast->name.z,
					 ast->name.n);
	res->pragma = sql_pragma_by_name(name);
	if (res->pragma != NULL)
		return 0;
	diag_set(ClientError, ER_SQL_NO_SUCH_PRAGMA, name);
	return -1;
}

/**
 * Resolve the given AST CREATE TRIGGER statement into the given
 * rast_create_trigger. The body of the trigger is resolved when the trigger
 * fires.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_create_trigger(struct sql_resolve_context *ctx,
		       const struct ast_create_trigger *ast,
		       struct rast_create_trigger *res)
{
	if (!ast->is_for_each_row) {
		diag_set(ClientError, ER_UNSUPPORTED, "Tarantool SQL",
			 "FOR EACH STATEMENT triggers, please supply "
			 "FOR EACH ROW clause");
		return -1;
	}
	struct ast_trigger_action *act;
	stailq_foreach_entry(act, &ast->actions->head, link) {
		if ((act->op == TK_UPDATE && act->update->indexed_by.n > 0) ||
		    (act->op == TK_DELETE && act->del->indexed_by.n > 0)) {
			diag_set(ClientError, ER_SQL_PARSER_GENERIC,
				 "The INDEXED BY clause is not allowed on "
				 "UPDATE or DELETE statements within triggers");
			return -1;
		}
	}
	res->space_id = sql_space_id_by_token(&ast->table);
	if (res->space_id == BOX_ID_NIL) {
		diag_set(ClientError, ER_NO_SUCH_SPACE,
			 sql_tt_name_from_token(&ast->table));
		return -1;
	}
	const char *name = sql_name_temp(ctx->region, ast->name.z,
					 ast->name.n);
	if (sql_check_identifier_name(name, strlen(name)) != 0)
		return -1;
	res->name = ast->name;
	res->if_not_exists = ast->if_not_exists;
	return 0;
}

/**
 * Find the field with the given name of a column of an index in the given
 * space definition, as lookupName() does. If the given unquoted identifier is
 * not NULL, the name is also looked up in the legacy form of the identifier.
 * A name of more than one field is ambiguous: it is possible for a space being
 * created.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_index_field(struct sql_resolve_context *ctx,
		    const struct space_def *def, const char *name,
		    const struct Token *id, uint32_t *fieldno)
{
	const char *lookup = name;
	if (space_def_fieldno(def, name) == UINT32_MAX && id != NULL)
		lookup = sql_legacy_name_temp(ctx->region, id->z, id->n);
	*fieldno = space_def_fieldno(def, lookup);
	if (*fieldno == UINT32_MAX) {
		diag_set(ClientError, ER_SQL_CANT_RESOLVE_FIELD, name);
		return -1;
	}
	for (uint32_t i = *fieldno + 1; i < def->field_count; ++i) {
		if (strcmp(def->fields[i].name, lookup) == 0) {
			ambiguous_column_error(NULL, name);
			return -1;
		}
	}
	return 0;
}

/**
 * Resolve the given AST column of an index of the space with the given
 * definition into the given rast_index_part, as index_fill_def() does. The
 * column must be a name of a field, possibly with collations, of which the
 * outermost one is used.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_index_part(struct sql_resolve_context *ctx,
		   const struct space_def *def, const struct ast_expr *ast,
		   struct rast_index_part *res)
{
	const struct ast_expr *coll = NULL;
	while (ast->op == TK_PARENTHESES || ast->op == TK_COLLATE) {
		if (ast->op == TK_COLLATE && coll == NULL)
			coll = ast;
		ast = ast->left;
	}
	if (ast->op != TK_ID) {
		diag_set(ClientError, ER_INDEX_DEF_UNSUPPORTED, "Expressions");
		return -1;
	}
	const char *name = sql_name_temp(ctx->region, ast->str, ast->len);
	struct Token id = {ast->str, ast->len, false};
	if (resolve_index_field(ctx, def, name, ast->str[0] != '"' ? &id : NULL,
				&res->fieldno) != 0)
		return -1;
	if (coll == NULL) {
		res->coll_id = def->fields[res->fieldno].coll_id;
		return 0;
	}
	return sql_coll_id(&res->coll_id, coll->right->str, coll->right->len);
}

/**
 * Convert the given AST columns of an index and discard the result: the legacy
 * code converts them first, so the same errors are reported. The AUTOINCREMENT
 * field of the parser, which the conversion checks, is kept.
 *
 * Returns 0 on success and -1 on error.
 */
static int
convert_index_columns(struct sql_resolve_context *ctx,
		      struct ast_expr_list *columns)
{
	int *autoinc_fieldno = ctx->parser->autoinc_fieldno;
	struct ExprList *list = expr_list_from_ast(ctx->parser, columns);
	ctx->parser->autoinc_fieldno = autoinc_fieldno;
	if (list == NULL)
		return -1;
	sql_expr_list_delete(list);
	return 0;
}

/**
 * Resolve the given AST columns of an index of the space with the given
 * definition into an array of rast_index_part, returned via the given
 * pointer.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_index_parts(struct sql_resolve_context *ctx,
		    const struct space_def *def,
		    const struct ast_expr_list *columns,
		    struct rast_index_part **parts)
{
	*parts = xregion_alloc_array(ctx->region, struct rast_index_part,
				     columns->len);
	uint32_t i = 0;
	for (const struct stailq_entry *node = columns->head.first.value;
	     node != NULL; node = node->next.value, ++i) {
		const struct ast_expr_list_entry *entry =
			stailq_entry(node, struct ast_expr_list_entry, link);
		if (resolve_index_part(ctx, def, entry->expr,
				       &(*parts)[i]) != 0)
			return -1;
		/*
		 * As in sql_create_index(), the number of the columns is
		 * reported only if the first one is resolved.
		 */
		if (columns->len > SQL_MAX_COLUMN) {
			diag_set(ClientError, ER_SQL_PARSER_LIMIT,
				 "The number of columns in index",
				 columns->len, SQL_MAX_COLUMN);
			return -1;
		}
	}
	return 0;
}

/**
 * Resolve the index with the given name and columns of the space with the given
 * name into the given rast_create_index, as sql_create_index() does. As in the
 * legacy code, the columns are converted first, so the same errors are
 * reported, and are resolved after the space and the name of the index are
 * checked. If the given flag is set, a missing space or an existing index is
 * not an error, and the space of the result is left NULL.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_index(struct sql_resolve_context *ctx, const struct Token *table,
	      const struct Token *name, struct ast_expr_list *columns,
	      bool if_not_exists, struct rast_create_index *res)
{
	res->name = *name;
	if (convert_index_columns(ctx, columns) != 0)
		return -1;

	struct space *space = find_space(ctx, table, false);
	if (space == NULL) {
		if (!if_not_exists)
			return -1;
		diag_clear(diag_get());
		return 0;
	}
	struct space_def *def = space->def;
	if (def->opts.is_view) {
		diag_set(ClientError, ER_SQL_PARSER_GENERIC,
			 "Views can not be indexed");
		return -1;
	}
	if (sql_space_def_check_format(def) != 0)
		return -1;
	const char *index_name = sql_name_temp(ctx->region, name->z, name->n);
	if (space_index_by_name0(space, index_name) != NULL) {
		if (if_not_exists)
			return 0;
		diag_set(ClientError, ER_INDEX_EXISTS_IN_SPACE, index_name,
			 def->name);
		return -1;
	}
	if (sql_check_identifier_name(index_name, strlen(index_name)) != 0)
		return -1;
	if (space_is_system(space)) {
		diag_set(ClientError, ER_MODIFY_INDEX, index_name, def->name,
			 "can't create index on system space");
		return -1;
	}
	res->part_count = columns->len;
	if (resolve_index_parts(ctx, def, columns, &res->parts) != 0)
		return -1;
	res->space = space;
	return 0;
}

/**
 * Resolve the given AST CREATE INDEX statement into the given
 * rast_create_index.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_create_index(struct sql_resolve_context *ctx,
		     const struct ast_create_index *ast,
		     struct rast_create_index *res)
{
	memset(res, 0, sizeof(*res));
	res->is_unique = ast->is_unique;
	return resolve_index(ctx, &ast->table, &ast->name, ast->columns,
			     ast->if_not_exists, res);
}

/**
 * Resolve the given UNIQUE or PRIMARY KEY constraint added to the space with
 * the given name into the given rast_add_constraint.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_index_constraint(struct sql_resolve_context *ctx,
			 const struct Token *table,
			 const struct ast_property *ast,
			 struct rast_add_constraint *res)
{
	struct rast_create_index index;
	memset(&index, 0, sizeof(index));
	if (resolve_index(ctx, table, &ast->name, ast->columns, false,
			  &index) != 0)
		return -1;
	res->space = index.space;
	res->index.parts = index.parts;
	res->index.part_count = index.part_count;
	res->index.autoinc_fieldno = UINT32_MAX;
	uint32_t i = 0;
	for (const struct stailq_entry *node = ast->columns->head.first.value;
	     node != NULL; node = node->next.value, ++i) {
		if (stailq_entry(node, struct ast_expr_list_entry,
				 link)->autoinc)
			res->index.autoinc_fieldno = index.parts[i].fieldno;
	}
	return 0;
}

/**
 * Check the given names of the columns of a FOREIGN KEY constraint as
 * expr_list_from_ids() does: all the names are checked, so the error is
 * reported for the last invalid one.
 *
 * Returns 0 on success and -1 on error.
 */
static int
check_fk_column_names(struct sql_resolve_context *ctx,
		      const struct ast_id_list *list)
{
	if (list == NULL)
		return 0;
	int rc = 0;
	for (const struct stailq_entry *node = list->head.first.value;
	     node != NULL; node = node->next.value) {
		const struct Token *id =
			&stailq_entry(node, struct ast_id_entry, link)->id;
		const char *name = sql_name_temp(ctx->region, id->z, id->n);
		if (sql_check_identifier_name(name, strlen(name)) != 0)
			rc = -1;
	}
	return rc;
}

/**
 * Find the field with the given name of a column of the FOREIGN KEY constraint
 * with the given name in the given space definition. An unquoted name is also
 * looked up in its legacy form. The given flag is set if it is the definition
 * of the space being created or altered by adding a column, which has its own
 * error message.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_fk_field(struct sql_resolve_context *ctx, const struct space_def *def,
		 const struct Token *id, const char *fk_name,
		 bool is_new_space, uint32_t *fieldno)
{
	const char *name = sql_name_temp(ctx->region, id->z, id->n);
	*fieldno = space_def_fieldno(def, name);
	if (*fieldno == UINT32_MAX && id->z[0] != '"') {
		const char *legacy = sql_legacy_name_temp(ctx->region, id->z,
							  id->n);
		*fieldno = space_def_fieldno(def, legacy);
	}
	if (*fieldno != UINT32_MAX)
		return 0;
	const char *err = is_new_space ?
		tt_sprintf("unknown column %s in foreign key definition",
			   name) :
		tt_sprintf("foreign key refers to nonexistent field %s", name);
	diag_set(ClientError, ER_CREATE_FK_CONSTRAINT, fk_name, err);
	return -1;
}

/**
 * Resolve the given FOREIGN KEY constraint added to the space with the given
 * name into the given rast_add_constraint, in the order of the checks of
 * sql_create_foreign_key(). Without the referenced columns, the columns of
 * the primary key of the referenced space are used.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_foreign_key(struct sql_resolve_context *ctx, const struct Token *table,
		    const struct ast_property *ast,
		    struct rast_add_constraint *res)
{
	const struct ast_foreign_key *fk = &ast->foreign_key;
	if (check_fk_column_names(ctx, fk->columns) != 0 ||
	    check_fk_column_names(ctx, fk->foreign_columns) != 0)
		return -1;
	struct space *space = find_space(ctx, table, false);
	if (space == NULL)
		return -1;
	struct space *parent = find_space(ctx, &fk->foreign_table, false);
	if (parent == NULL)
		return -1;
	const char *name = sql_name_temp(ctx->region, ast->name.z,
					 ast->name.n);
	if (parent->def->opts.is_view) {
		diag_set(ClientError, ER_CREATE_FK_CONSTRAINT, name,
			 "referenced space can't be VIEW");
		return -1;
	}
	uint32_t count = fk->columns->len;
	const struct key_def *pk_def = NULL;
	if (fk->foreign_columns == NULL) {
		if (parent->index_count == 0) {
			diag_set(ClientError, ER_CREATE_FK_CONSTRAINT, name,
				 "referenced space doesn't feature PRIMARY "
				 "KEY");
			return -1;
		}
		pk_def = parent->index[0]->def->key_def;
	}
	if ((pk_def != NULL && pk_def->part_count != count) ||
	    (pk_def == NULL && fk->foreign_columns->len != count)) {
		diag_set(ClientError, ER_CREATE_FK_CONSTRAINT, name,
			 "number of columns in foreign key does not match the "
			 "number of columns in the primary index of "
			 "referenced table");
		return -1;
	}
	res->fk.link_count = count;
	res->fk.links = xregion_alloc_array(ctx->region, struct field_link,
					    count);
	const struct stailq_entry *child = fk->columns->head.first.value;
	const struct stailq_entry *foreign = NULL;
	if (pk_def == NULL)
		foreign = fk->foreign_columns->head.first.value;
	for (uint32_t i = 0; i < count; ++i) {
		struct field_link *link = &res->fk.links[i];
		if (pk_def != NULL) {
			link->parent_field = pk_def->parts[i].fieldno;
		} else {
			const struct Token *id =
				&stailq_entry(foreign, struct ast_id_entry,
					      link)->id;
			if (resolve_fk_field(ctx, parent->def, id, name, false,
					     &link->parent_field) != 0)
				return -1;
			foreign = foreign->next.value;
		}
		const struct Token *id =
			&stailq_entry(child, struct ast_id_entry, link)->id;
		if (resolve_fk_field(ctx, space->def, id, name, false,
				     &link->child_field) != 0)
			return -1;
		child = child->next.value;
	}
	res->space = space;
	res->fk.parent = parent;
	return 0;
}

/**
 * Resolve the given AST ALTER TABLE ADD CONSTRAINT statement into the given
 * rast_add_constraint. The expression of CHECK constraint is not resolved
 * here: it becomes the body of a function, which is resolved when created.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_add_constraint(struct sql_resolve_context *ctx,
		       const struct ast_alter_add_constraint *ast,
		       struct rast_add_constraint *res)
{
	const struct ast_property *con = ast->con;
	memset(res, 0, sizeof(*res));
	res->name = con->name;
	res->type = con->type;
	switch (con->type) {
	case SQL_AST_PROPERTY_CHECK:
		res->check.z = con->expr->str;
		res->check.n = con->expr->len;
		res->space = find_space(ctx, &ast->table, false);
		return res->space != NULL ? 0 : -1;
	case SQL_AST_PROPERTY_FOREIGN_KEY:
		return resolve_foreign_key(ctx, &ast->table, con, res);
	default:
		assert(con->type == SQL_AST_PROPERTY_UNIQUE ||
		       con->type == SQL_AST_PROPERTY_PRIMARY_KEY);
		return resolve_index_constraint(ctx, &ast->table, con, res);
	}
}

/**
 * Return the primary key of the given space, or NULL if there is none. While
 * a space is being built, its first index is not necessarily the primary key.
 */
static struct index *
space_primary_key(const struct space *space)
{
	if (space->index_count == 0 || space->index[0]->def->iid != 0)
		return NULL;
	return space->index[0];
}

/** Copy the given string to the region of the given context. */
static const char *
resolve_strdup(struct sql_resolve_context *ctx, const char *str)
{
	size_t len = strlen(str);
	char *res = xregion_alloc(ctx->region, len + 1);
	memcpy(res, str, len + 1);
	return res;
}

/**
 * Make sure the array of the fields of the given space definition has room for
 * the field with the given number. The array is reallocated on the region.
 */
static void
space_def_reserve_field(struct sql_resolve_context *ctx, struct space_def *def,
			uint32_t fieldno)
{
	assert(fieldno < SQL_MAX_COLUMN);
	if (fieldno < def->exact_field_count)
		return;
	uint32_t count = def->exact_field_count;
	count = count > 0 ? 2 * count : 1;
	struct field_def *fields = xregion_alloc_array(ctx->region,
						       struct field_def, count);
	memcpy(fields, def->fields, sizeof(*fields) * def->exact_field_count);
	for (uint32_t i = count / 2; i < count; ++i)
		memcpy(&fields[i], &field_def_default, sizeof(fields[i]));
	def->fields = fields;
	def->exact_field_count = count;
}

/**
 * Make a shallow copy of the given space on the region of the given context,
 * which a column is added to. The array of the indexes is copied as well,
 * since the indexes of the constraints of the column are added to it.
 */
static struct space *
space_shallow_copy(struct sql_resolve_context *ctx, const struct space *space)
{
	struct space *res = sql_template_space_new(ctx->parser,
						   space->def->name);
	res->index_count = space->index_count;
	res->index_id_max = space->index_id_max;
	res->index = NULL;
	if (space->index_count > 0) {
		res->index = xregion_alloc_array(ctx->region, struct index *,
						 space->index_count);
		memcpy(res->index, space->index,
		       sizeof(struct index *) * space->index_count);
	}
	memcpy(res->def, space->def, sizeof(struct space_def));
	res->def->opts.type = SPACE_TYPE_DATA_TEMPORARY;
	res->def->opts.is_ephemeral = true;
	if (res->def->field_count != 0) {
		res->def->fields = xregion_alloc_array(ctx->region,
						       struct field_def,
						       space->def->field_count);
		memcpy(res->def->fields, space->def->fields,
		       sizeof(struct field_def) * space->def->field_count);
	}
	return res;
}

/**
 * Make the field with the given number the AUTOINCREMENT field. The field is
 * kept by the parser, since the conversion of the columns of a PRIMARY KEY
 * checks it.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_autoinc(struct sql_resolve_context *ctx, uint32_t fieldno)
{
	if (ctx->parser->autoinc_fieldno != NULL) {
		diag_set(ClientError, ER_SQL_PARSER_GENERIC,
			 "Table must feature at most one AUTOINCREMENT field");
		return -1;
	}
	ctx->parser->autoinc_fieldno = xregion_alloc_object(ctx->region, int);
	*ctx->parser->autoinc_fieldno = fieldno;
	return 0;
}

/**
 * Resolve the given CHECK constraint of the space of the given statement. If
 * the given flag is set, it is a constraint of the last field. Only the name
 * of the constraint is resolved: the expression becomes the body of a
 * function, which is resolved when the function is created.
 */
static void
resolve_new_check(struct sql_resolve_context *ctx,
		  struct rast_create_table *res, const struct Token *name,
		  const struct ast_expr *expr, bool is_field)
{
	struct space_def *def = res->space->def;
	uint32_t fieldno = def->field_count - 1;
	const char *ck_name;
	if (name->n != 0) {
		ck_name = sql_name_temp(ctx->region, name->z, name->n);
	} else {
		uint32_t n = 1;
		struct ck_constraint_parse *ck;
		rlist_foreach_entry(ck, &res->checks, link) {
			if (ck->ck_def->is_field_ck == is_field &&
			    (!is_field || ck->ck_def->fieldno == fieldno))
				++n;
		}
		ck_name = is_field ?
			  tt_sprintf("ck_unnamed_%s_%s_%u", def->name,
				     def->fields[fieldno].name, n) :
			  tt_sprintf("ck_unnamed_%s_%u", def->name, n);
	}
	struct ck_constraint_parse *ck =
		ck_constraint_parse_new(ctx->parser, ck_name, expr->str,
					expr->len);
	if (is_field) {
		ck->ck_def->is_field_ck = true;
		ck->ck_def->fieldno = fieldno;
	}
	rlist_add_entry(&res->checks, ck, link);
}

/**
 * Resolve the given FOREIGN KEY constraint with the given name of the space of
 * the given statement, in the order of the checks of the legacy code. Without
 * the columns, it is a constraint of the last field. The referenced columns of
 * a self-referenced constraint are resolved after all the columns of the space,
 * by resolve_self_fkeys().
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_new_fk(struct sql_resolve_context *ctx, struct rast_create_table *res,
	       const struct Token *name, const struct ast_foreign_key *ast)
{
	if (check_fk_column_names(ctx, ast->columns) != 0 ||
	    check_fk_column_names(ctx, ast->foreign_columns) != 0)
		return -1;
	struct space_def *def = res->space->def;
	bool is_field = ast->columns == NULL;
	uint32_t count = is_field ? 1 : ast->columns->len;
	uint32_t fieldno = def->field_count - 1;
	const char *fk_name;
	if (name->n != 0) {
		fk_name = sql_name_temp(ctx->region, name->z, name->n);
	} else {
		uint32_t n = 1;
		struct rast_fk *fk;
		rlist_foreach_entry(fk, &res->fkeys, link) {
			if (fk->def->is_field_fk == is_field &&
			    (!is_field ||
			     fk->def->links[0].child_field == fieldno))
				++n;
		}
		fk_name = resolve_strdup(ctx, is_field ?
			tt_sprintf("fk_unnamed_%s_%s_%u", def->name,
				   def->fields[fieldno].name, n) :
			tt_sprintf("fk_unnamed_%s_%u", def->name, n));
	}
	struct rast_fk *fk = xregion_alloc_object(ctx->region, typeof(*fk));
	memset(fk, 0, sizeof(*fk));

	const struct Token *parent_name = &ast->foreign_table;
	const char *str = sql_name_temp(ctx->region, parent_name->z,
					parent_name->n);
	fk->is_self_referenced = strcmp(str, def->name) == 0;
	if (!fk->is_self_referenced && parent_name->z[0] != '"') {
		str = sql_legacy_name_temp(ctx->region, parent_name->z,
					   parent_name->n);
		fk->is_self_referenced = strcmp(str, def->name) == 0;
	}
	struct space *parent = find_space(ctx, parent_name, false);
	if (parent == NULL) {
		if (!fk->is_self_referenced)
			return -1;
		diag_clear(diag_get());
	}
	if (fk->is_self_referenced)
		fk->selfref_cols = ast->foreign_columns;
	if (!fk->is_self_referenced && parent->def->opts.is_view) {
		diag_set(ClientError, ER_CREATE_FK_CONSTRAINT, fk_name,
			 "referenced space can't be VIEW");
		return -1;
	}
	const struct key_def *pk_def = NULL;
	if (ast->foreign_columns == NULL && !fk->is_self_referenced) {
		if (parent->index_count == 0) {
			diag_set(ClientError, ER_CREATE_FK_CONSTRAINT, fk_name,
				 "referenced space doesn't feature PRIMARY "
				 "KEY");
			return -1;
		}
		pk_def = parent->index[0]->def->key_def;
	}
	if ((ast->foreign_columns != NULL &&
	     ast->foreign_columns->len != count) ||
	    (pk_def != NULL && pk_def->part_count != count)) {
		diag_set(ClientError, ER_CREATE_FK_CONSTRAINT, fk_name,
			 "number of columns in foreign key does not match the "
			 "number of columns in the primary index of "
			 "referenced table");
		return -1;
	}
	fk->def = sql_fk_constraint_def_new(ctx->parser, fk_name, count);
	fk->def->is_field_fk = is_field;
	fk->def->child_id = def->id;
	fk->def->parent_id = parent != NULL ? parent->def->id : 0;
	const struct stailq_entry *child = is_field ? NULL :
					   ast->columns->head.first.value;
	const struct stailq_entry *foreign = NULL;
	if (ast->foreign_columns != NULL)
		foreign = ast->foreign_columns->head.first.value;
	for (uint32_t i = 0; i < count; ++i) {
		struct field_link *link = &fk->def->links[i];
		if (pk_def != NULL) {
			link->parent_field = pk_def->parts[i].fieldno;
		} else if (!fk->is_self_referenced) {
			const struct Token *id =
				&stailq_entry(foreign, struct ast_id_entry,
					      link)->id;
			if (resolve_fk_field(ctx, parent->def, id, fk_name,
					     false, &link->parent_field) != 0)
				return -1;
			foreign = foreign->next.value;
		}
		if (is_field) {
			link->child_field = fieldno;
			break;
		}
		const struct Token *id =
			&stailq_entry(child, struct ast_id_entry, link)->id;
		if (resolve_fk_field(ctx, def, id, fk_name, true,
				     &link->child_field) != 0)
			return -1;
		child = child->next.value;
	}
	rlist_add_entry(&res->fkeys, fk, link);
	return 0;
}

/**
 * Resolve the referenced columns of the self-referenced FOREIGN KEY
 * constraints of the given statement, which is done after all the columns and
 * the constraints of the space are resolved.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_self_fkeys(struct sql_resolve_context *ctx,
		   struct rast_create_table *res)
{
	struct space_def *def = res->space->def;
	struct rast_fk *fk;
	rlist_foreach_entry(fk, &res->fkeys, link) {
		struct fk_constraint_def *fk_def = fk->def;
		if (fk->selfref_cols != NULL) {
			uint32_t i = 0;
			for (const struct stailq_entry *node =
				fk->selfref_cols->head.first.value;
			     node != NULL; node = node->next.value, ++i) {
				const struct Token *id =
					&stailq_entry(node, struct ast_id_entry,
						      link)->id;
				uint32_t *fieldno =
					&fk_def->links[i].parent_field;
				if (resolve_fk_field(ctx, def, id, fk_def->name,
						     true, fieldno) != 0)
					return -1;
			}
		} else if (fk->is_self_referenced) {
			struct key_def *pk_def =
				space_primary_key(res->space)->def->key_def;
			if (pk_def->part_count != fk_def->field_count) {
				diag_set(ClientError, ER_CREATE_FK_CONSTRAINT,
					 fk_def->name, "number of columns in "
					 "foreign key does not match the "
					 "number of columns in the primary "
					 "index of referenced table");
				return -1;
			}
			for (uint32_t i = 0; i < fk_def->field_count; ++i) {
				fk_def->links[i].parent_field =
					pk_def->parts[i].fieldno;
			}
		}
	}
	return 0;
}

/**
 * Resolve the given UNIQUE or PRIMARY KEY constraint with the given name of the
 * space of the given statement, and add its index to the space, in the order
 * of the checks of the legacy code. Without the columns, it is a constraint
 * of the last field.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_new_index(struct sql_resolve_context *ctx,
		  struct rast_create_table *res, const struct Token *name,
		  const struct ast_expr_list *columns, enum sql_index_type type)
{
	struct space *space = res->space;
	struct space_def *def = space->def;
	if (def->opts.is_view) {
		diag_set(ClientError, ER_SQL_PARSER_GENERIC,
			 "Views can not be indexed");
		return -1;
	}
	if (sql_space_def_check_format(def) != 0)
		return -1;
	const char *index_name;
	if (name->n > 0) {
		index_name = sql_name_temp(ctx->region, name->z, name->n);
	} else {
		const char *prefix = type == SQL_INDEX_TYPE_CONSTRAINT_UNIQUE ?
				     "unique" : "pk";
		const char *str = tt_sprintf("%s_unnamed_%s_%u", prefix,
					     def->name, space->index_count + 1);
		index_name = resolve_strdup(ctx, str);
	}
	if (sql_check_identifier_name(index_name, strlen(index_name)) != 0)
		return -1;
	struct rast_index_part *parts;
	uint32_t part_count;
	if (columns == NULL) {
		part_count = 1;
		parts = xregion_alloc_object(ctx->region, typeof(*parts));
		uint32_t last = def->field_count - 1;
		const char *field_name = def->fields[last].name;
		if (resolve_index_field(ctx, def, field_name, NULL,
					&parts->fieldno) != 0)
			return -1;
		parts->coll_id = def->fields[parts->fieldno].coll_id;
	} else {
		part_count = columns->len;
		if (resolve_index_parts(ctx, def, columns, &parts) != 0)
			return -1;
		uint32_t i = 0;
		const struct stailq_entry *node = columns->head.first.value;
		for (; node != NULL; node = node->next.value, ++i) {
			if (stailq_entry(node, struct ast_expr_list_entry,
					 link)->autoinc) {
				int *fieldno =
					xregion_alloc_object(ctx->region, int);
				*fieldno = parts[i].fieldno;
				ctx->parser->autoinc_fieldno = fieldno;
			}
		}
	}
	sql_index_create(ctx->parser, space, index_name, parts, part_count,
			 type, true);
	return ctx->parser->is_aborted ? -1 : 0;
}

/**
 * Resolve the given PRIMARY KEY constraint with the given name of the space of
 * the given statement as resolve_new_index() does, and make the fields of the
 * primary key NOT NULL.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_new_primary_key(struct sql_resolve_context *ctx,
			struct rast_create_table *res, const struct Token *name,
			const struct ast_expr_list *columns)
{
	struct space_def *def = res->space->def;
	if (space_primary_key(res->space) != NULL) {
		diag_set(ClientError, ER_CREATE_SPACE, def->name,
			 "primary key has been already declared");
		return -1;
	}
	if (resolve_new_index(ctx, res, name, columns,
			      SQL_INDEX_TYPE_CONSTRAINT_PK) != 0)
		return -1;
	struct index *pk = space_primary_key(res->space);
	assert(pk != NULL);
	struct key_def *key_def = pk->def->key_def;
	for (uint32_t i = 0; i < key_def->part_count; ++i) {
		uint32_t fieldno = key_def->parts[i].fieldno;
		struct field_def *field = &def->fields[fieldno];
		if (field->nullable_action == ON_CONFLICT_ACTION_DEFAULT) {
			field->nullable_action = ON_CONFLICT_ACTION_ABORT;
			field->is_nullable = false;
		} else if (field->nullable_action != ON_CONFLICT_ACTION_ABORT) {
			diag_set(ClientError, ER_NULLABLE_PRIMARY, def->name);
			return -1;
		}
	}
	return 0;
}

/**
 * Resolve the given DEFAULT of the last field of the space of the given
 * statement. A literal is encoded into the definition of the field, any other
 * expression must be constant and becomes the body of a function. As in the
 * legacy code, the expression is converted to check it.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_default(struct sql_resolve_context *ctx, struct rast_create_table *res,
		struct ast_expr *ast)
{
	struct Expr *expr = expr_from_ast(ctx->parser, ast);
	if (ctx->parser->is_aborted) {
		sql_expr_delete(expr);
		return -1;
	}
	struct space_def *def = res->space->def;
	uint32_t fieldno = def->field_count - 1;
	struct field_def *field = &def->fields[fieldno];
	if (sql_expr_is_term(expr)) {
		uint32_t size;
		field->default_value = sql_expr_term_encode(ctx->region, expr,
							    &size);
		field->default_value_size = size;
		sql_expr_delete(expr);
		return 0;
	}
	bool is_constant = sqlExprIsConstantOrFunction(expr);
	sql_expr_delete(expr);
	if (!is_constant) {
		diag_set(ClientError, ER_CREATE_SPACE, def->name,
			 tt_sprintf("default value of column '%s' is not "
				    "constant", field->name));
		return -1;
	}
	struct rast_default_func *func =
		xregion_alloc_object(ctx->region, typeof(*func));
	func->fieldno = fieldno;
	func->body.z = ast->str;
	func->body.n = ast->len;
	stailq_add_tail_entry(&res->default_funcs, func, link);
	return 0;
}

/**
 * Resolve the given collation of the last field of the space of the given
 * statement. The indexes of the field, which its UNIQUE or PRIMARY KEY
 * constraints have already created, use the collation as well.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_column_collation(struct rast_create_table *res,
			 const struct Token *name)
{
	uint32_t coll_id;
	if (sql_coll_id(&coll_id, name->z, name->n) != 0)
		return -1;
	struct space *space = res->space;
	uint32_t fieldno = space->def->field_count - 1;
	space->def->fields[fieldno].coll_id = coll_id;
	for (uint32_t i = 0; i < space->index_count; ++i) {
		struct key_def *key_def = space->index[i]->def->key_def;
		if (key_def->part_count == 1 &&
		    key_def->parts[0].fieldno == fieldno)
			key_def->parts[0].coll_id = coll_id;
	}
	return 0;
}

/**
 * Set the given NULL or NOT NULL action of the last field of the space of the
 * given statement. It is an error to set different actions.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_nullable_action(struct rast_create_table *res,
			enum on_conflict_action action)
{
	struct space_def *def = res->space->def;
	struct field_def *field = &def->fields[def->field_count - 1];
	if (field->nullable_action != ON_CONFLICT_ACTION_DEFAULT &&
	    field->nullable_action != action) {
		const char *err = tt_sprintf("NULL declaration for column '%s' "
					     "of table '%s' has been already "
					     "set to '%s'", field->name,
					     def->name, on_conflict_action_strs[
						field->nullable_action]);
		diag_set(ClientError, ER_SQL_EXECUTE, err);
		return -1;
	}
	field->nullable_action = action;
	field->is_nullable = action_is_nullable(action);
	return 0;
}

/**
 * Resolve the given property of the last field of the space of the given
 * statement.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_column_property(struct sql_resolve_context *ctx,
			struct rast_create_table *res,
			const struct ast_property *ast)
{
	switch (ast->type) {
	case SQL_AST_PROPERTY_CHECK:
		resolve_new_check(ctx, res, &ast->name, ast->expr, true);
		return 0;
	case SQL_AST_PROPERTY_FOREIGN_KEY:
		return resolve_new_fk(ctx, res, &ast->name, &ast->foreign_key);
	case SQL_AST_PROPERTY_UNIQUE:
		return resolve_new_index(ctx, res, &ast->name, NULL,
					 SQL_INDEX_TYPE_CONSTRAINT_UNIQUE);
	case SQL_AST_PROPERTY_PRIMARY_KEY:
		return resolve_new_primary_key(ctx, res, &ast->name, NULL);
	case SQL_AST_PROPERTY_DEFAULT:
		return resolve_default(ctx, res, ast->expr);
	case SQL_AST_PROPERTY_COLLATE:
		return resolve_column_collation(res, &ast->collate);
	case SQL_AST_PROPERTY_NOT_NULL:
		return resolve_nullable_action(res, ast->action);
	case SQL_AST_PROPERTY_NULL:
		return resolve_nullable_action(res, ON_CONFLICT_ACTION_NONE);
	default:
		unreachable();
	}
	return 0;
}

/**
 * Resolve the given column, which is added to the space of the given
 * statement.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_column(struct sql_resolve_context *ctx, struct rast_create_table *res,
	       const struct ast_column *ast)
{
	struct space_def *def = res->space->def;
	if (def->field_count + 1 > SQL_MAX_COLUMN) {
		diag_set(ClientError, ER_SQL_COLUMN_COUNT_MAX, def->name,
			 def->field_count + 1, SQL_MAX_COLUMN);
		return -1;
	}
	/*
	 * The format can be set in Lua, then exact_field_count can be zero,
	 * but field_count is not.
	 */
	if (def->exact_field_count == 0)
		def->exact_field_count = def->field_count;
	space_def_reserve_field(ctx, def, def->field_count);
	struct field_def *field = &def->fields[def->field_count++];
	memcpy(field, &field_def_default, sizeof(*field));
	field->name = sql_name_temp(ctx->region, ast->name.z, ast->name.n);
	/*
	 * ON_CONFLICT_ACTION_DEFAULT is used to detect attempts to declare
	 * NULL more than once and invalid primary keys.
	 */
	field->nullable_action = ON_CONFLICT_ACTION_DEFAULT;
	field->is_nullable = true;
	field->type = sql_type_to_field_type(ast->type);
	if (ast->properties != NULL) {
		for (const struct stailq_entry *node =
			ast->properties->head.first.value;
		     node != NULL; node = node->next.value) {
			struct ast_property *property =
				stailq_entry(node, struct ast_property, link);
			if (resolve_column_property(ctx, res, property) != 0)
				return -1;
		}
	}
	if (!ast->is_autoinc)
		return 0;
	return resolve_autoinc(ctx, def->field_count - 1);
}

/**
 * Resolve the given table constraint of the space of the given CREATE TABLE
 * statement. As in the legacy code, the columns of a UNIQUE or PRIMARY KEY
 * constraint are converted first.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_table_constraint(struct sql_resolve_context *ctx,
			 struct rast_create_table *res,
			 const struct ast_property *ast)
{
	switch (ast->type) {
	case SQL_AST_PROPERTY_CHECK:
		resolve_new_check(ctx, res, &ast->name, ast->expr, false);
		return 0;
	case SQL_AST_PROPERTY_FOREIGN_KEY:
		return resolve_new_fk(ctx, res, &ast->name, &ast->foreign_key);
	case SQL_AST_PROPERTY_UNIQUE:
		if (convert_index_columns(ctx, ast->columns) != 0)
			return -1;
		return resolve_new_index(ctx, res, &ast->name, ast->columns,
					 SQL_INDEX_TYPE_CONSTRAINT_UNIQUE);
	default:
		assert(ast->type == SQL_AST_PROPERTY_PRIMARY_KEY);
		if (convert_index_columns(ctx, ast->columns) != 0)
			return -1;
		return resolve_new_primary_key(ctx, res, &ast->name,
					       ast->columns);
	}
}

/** Initialize the given resolved CREATE TABLE or ADD COLUMN statement. */
static void
rast_create_table_create(struct rast_create_table *res)
{
	memset(res, 0, sizeof(*res));
	rlist_create(&res->checks);
	rlist_create(&res->fkeys);
	stailq_create(&res->default_funcs);
	res->autoinc_fieldno = UINT32_MAX;
}

/**
 * Finish the resolving of the given CREATE TABLE or ADD COLUMN statement: the
 * fields starting with the given one, which have no NULL or NOT NULL, become
 * nullable, the self-referenced FOREIGN KEY constraints are resolved, and the
 * AUTOINCREMENT field is moved from the parser to the statement.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_table_finish(struct sql_resolve_context *ctx,
		     struct rast_create_table *res, uint32_t fieldno)
{
	struct space_def *def = res->space->def;
	for (uint32_t i = fieldno; i < def->field_count; ++i) {
		struct field_def *field = &def->fields[i];
		if (field->nullable_action == ON_CONFLICT_ACTION_DEFAULT) {
			field->nullable_action = ON_CONFLICT_ACTION_NONE;
			field->is_nullable = true;
		}
	}
	if (resolve_self_fkeys(ctx, res) != 0)
		return -1;
	if (ctx->parser->autoinc_fieldno != NULL)
		res->autoinc_fieldno = *ctx->parser->autoinc_fieldno;
	ctx->parser->autoinc_fieldno = NULL;
	return 0;
}

/**
 * Resolve the given AST CREATE TABLE statement into the given
 * rast_create_table. The space is built in the order of the checks of the
 * legacy code: the columns with their constraints, the table constraints, the
 * engine, and then the primary key and the self-referenced FOREIGN KEY
 * constraints are checked.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_create_table(struct sql_resolve_context *ctx,
		     const struct ast_create_table *ast,
		     struct rast_create_table *res)
{
	rast_create_table_create(res);
	res->if_not_exists = ast->if_not_exists;
	const char *name = sql_name_temp(ctx->region, ast->name.z, ast->name.n);
	if (sql_check_identifier_name(name, strlen(name)) != 0)
		return -1;
	struct space *space = sql_template_space_new(ctx->parser, name);
	strlcpy(space->def->engine_name,
		sql_storage_engine_strs[current_session()->sql_default_engine],
		ENGINE_NAME_MAX + 1);
	/* The parser frees the index definitions of the space. */
	ctx->parser->new_space = space;
	ctx->parser->space = space;
	res->space = space;

	struct ast_column *column;
	stailq_foreach_entry(column, &ast->properties->columns, link) {
		if (resolve_column(ctx, res, column) != 0)
			return -1;
	}
	struct ast_property *constraint;
	stailq_foreach_entry(constraint, &ast->properties->constraints, link) {
		if (resolve_table_constraint(ctx, res, constraint) != 0)
			return -1;
	}
	if (ast->engine.n > 0) {
		if (ast->engine.n > ENGINE_NAME_MAX) {
			diag_set(ClientError, ER_CREATE_SPACE, name,
				 "space engine name is too long");
			return -1;
		}
		const char *engine = sql_name_temp(ctx->region, ast->engine.z,
						   ast->engine.n);
		memcpy(space->def->engine_name, engine, strlen(engine) + 1);
	}
	if (space_primary_key(space) == NULL) {
		diag_set(ClientError, ER_CREATE_SPACE, name,
			 "PRIMARY KEY missing");
		return -1;
	}
	return resolve_table_finish(ctx, res, 0);
}

/**
 * Resolve the given AST ALTER TABLE ADD COLUMN statement into the given
 * rast_create_table. The column is added to a copy of the space.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_add_column(struct sql_resolve_context *ctx,
		   const struct ast_alter_add_column *ast,
		   struct rast_create_table *res)
{
	rast_create_table_create(res);
	struct space *origin = find_space(ctx, &ast->table, false);
	if (origin == NULL)
		return -1;
	res->space = space_shallow_copy(ctx, origin);
	/* The parser frees the index definitions of the new constraints. */
	ctx->parser->space = res->space;
	if (resolve_column(ctx, res, ast->col) != 0)
		return -1;
	return resolve_table_finish(ctx, res, res->space->def->field_count - 1);
}

/**
 * Look up the space of a DELETE or UPDATE statement with the given name and
 * INDEXED BY clause, and make it the only source of the given list, as
 * sql_lookup_space() does.
 *
 * Returns the space or NULL on error.
 */
static struct space *
resolve_target(struct sql_resolve_context *ctx, const struct Token *name,
	       const struct Token *indexed_by, struct rast_source_list *res)
{
	/*
	 * As targetSrcList() does, the space of a statement of a trigger is
	 * looked up by its exact name.
	 */
	struct space *space = lookup_space(ctx, name,
					   ctx->parser->triggered_space !=
					   NULL);
	if (space == NULL)
		return NULL;
	struct rast_source *target = xregion_alloc_object(ctx->region,
							  struct rast_source);
	memset(target, 0, sizeof(*target));
	target->space_id = space->def->id;
	target->index_id = UINT32_MAX;
	target->cursor = -1;
	res->sources = target;
	res->len = 1;
	if (resolve_indexed_by(ctx, space, indexed_by, target) != 0)
		return NULL;
	return space;
}

/**
 * Resolve SELECT * FROM the view with the given name and the given WHERE
 * clause into the given rast_select, realizing the view as
 * sql_materialize_view() does.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_view_select(struct sql_resolve_context *ctx, const struct Token *name,
		    struct ast_expr *where, struct rast_select *res)
{
	struct ast_source *src = ast_source_new(ctx->region);
	src->name = *name;
	struct ast_select *select = ast_select_new_asterisk(
		ctx->region, ast_source_list_append(ctx->region, NULL, src));
	select->where = where;
	return resolve_select(ctx, select, res);
}

/**
 * Resolve the given AST expression of a DELETE or UPDATE statement into the
 * given rast_expr in the scope of the given target, without allowing
 * aggregate functions.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_target_expr(struct sql_resolve_context *ctx,
		    struct rast_source_list *target, const struct ast_expr *ast,
		    struct rast_expr *res)
{
	struct sql_resolve_scope scope = {
		.sources = target,
	};
	ctx->scope = &scope;
	int rc = resolve_expr(ctx, ast, res);
	ctx->scope = NULL;
	return rc;
}

/**
 * Resolve the given AST DELETE statement, whose WITH clause is visible, into
 * the given rast_delete, see resolve_delete().
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_delete_in(struct sql_resolve_context *ctx, const struct ast_delete *ast,
		  struct rast_delete *res)
{
	struct space *space = resolve_target(ctx, &ast->table,
					     &ast->indexed_by, &res->target);
	if (space == NULL)
		return -1;
	if (space->def->opts.is_view) {
		if (sql_triggers_exist(space->def, TK_DELETE, NULL,
				       ctx->parser->sql_flags, NULL) == NULL) {
			diag_set(ClientError, ER_ALTER_SPACE, space->def->name,
				 "space is a view");
			return -1;
		}
		/*
		 * The WHERE clause is resolved in the SELECT realizing the
		 * view before it is resolved for the view.
		 */
		res->view_select = xregion_alloc_object(ctx->region,
							struct rast_select);
		if (resolve_view_select(ctx, &ast->table, ast->where,
					res->view_select) != 0)
			return -1;
	}
	if (ast->where == NULL)
		return 0;
	res->where = xregion_alloc_object(ctx->region, struct rast_expr);
	return resolve_target_expr(ctx, &res->target, ast->where, res->where);
}

/**
 * Resolve the given AST DELETE statement into the given rast_delete. As
 * sql_table_delete_from() does, the space is checked before the WHERE clause,
 * which is resolved in the scope of the space without allowing aggregate
 * functions.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_delete(struct sql_resolve_context *ctx, const struct ast_delete *ast,
	       struct rast_delete *res)
{
	memset(res, 0, sizeof(*res));
	struct sql_resolve_with *outer_with = ctx->with;
	struct sql_resolve_with with;
	if (ast->with != NULL && push_with(ctx, ast->with, &with) != 0)
		return -1;
	int rc = resolve_delete_in(ctx, ast, res);
	if (rc == 0 && ast->with != NULL)
		rc = check_unused_ctes(ctx, &with);
	ctx->with = outer_with;
	return rc;
}

/**
 * Check the names of the columns of the given AST SET clause of an UPDATE as
 * sql_code_update() does while converting it: the name of a column assigned a
 * value is checked to be a valid identifier, the number of the columns
 * assigned a vector to match its size, unless it is a SELECT, and the number
 * of the columns not to exceed the limit.
 *
 * Returns 0 on success and -1 on error.
 */
static int
check_set_list(struct sql_resolve_context *ctx, const struct ast_set_list *ast)
{
	uint32_t count = 0;
	for (const struct stailq_entry *node = ast->head.first.value;
	     node != NULL; node = node->next.value) {
		const struct ast_set_list_entry *entry =
			stailq_entry(node, struct ast_set_list_entry, link);
		if (entry->ids == NULL) {
			++count;
			const char *name = sql_name_temp(ctx->region,
							 entry->name.z,
							 entry->name.n);
			if (sql_check_identifier_name(name, strlen(name)) != 0)
				return -1;
			continue;
		}
		uint32_t len = entry->ids->len;
		uint32_t size = 1;
		if (entry->expr->op == TK_VECTOR)
			size = entry->expr->list != NULL ?
			       entry->expr->list->len : 0;
		if (entry->expr->op != TK_SELECT && len != size) {
			diag_set(ClientError, ER_SQL_PARSER_GENERIC,
				 tt_sprintf("%d columns assigned %d values",
					    len, size));
			return -1;
		}
		count += len;
	}
	if (count > SQL_MAX_COLUMN) {
		diag_set(ClientError, ER_SQL_PARSER_LIMIT,
			 "The number of columns in set list", count,
			 SQL_MAX_COLUMN);
		return -1;
	}
	return 0;
}

/**
 * Create a list of the names of the columns of the given AST SET clause, as
 * sql_code_update() names the entries of the list it creates from it.
 */
static struct ExprList *
set_list_names(struct Parse *parser, const struct ast_set_list *ast)
{
	struct ExprList *res = NULL;
	for (const struct stailq_entry *node = ast->head.first.value;
	     node != NULL; node = node->next.value) {
		const struct ast_set_list_entry *entry =
			stailq_entry(node, struct ast_set_list_entry, link);
		if (entry->ids == NULL) {
			res = sql_expr_list_append(res, NULL);
			sqlExprListSetName(parser, res,
					   (struct Token *)&entry->name, 1);
			continue;
		}
		struct ast_id_entry *id;
		stailq_foreach_entry(id, &entry->ids->head, link) {
			res = sql_expr_list_append(res, NULL);
			sqlExprListSetName(parser, res, &id->id, 1);
		}
	}
	return res;
}

/**
 * Resolve the given AST UPDATE statement, whose WITH clause is visible, into
 * the given rast_update, see resolve_update().
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_update_in(struct sql_resolve_context *ctx, const struct ast_update *ast,
		  struct rast_update *res)
{
	if (check_set_list(ctx, ast->set_list) != 0)
		return -1;
	struct space *space = resolve_target(ctx, &ast->table,
					     &ast->indexed_by, &res->target);
	if (space == NULL)
		return -1;
	struct space_def *def = space->def;
	if (def->opts.is_view) {
		struct ExprList *names = set_list_names(ctx->parser,
							ast->set_list);
		int tmask;
		sql_triggers_exist(def, TK_UPDATE, names,
				   ctx->parser->sql_flags, &tmask);
		sql_expr_list_delete(names);
		if (tmask == 0) {
			diag_set(ClientError, ER_ALTER_SPACE, def->name,
				 "space is a view");
			return -1;
		}
	}
	res->set_len = ast->set_list->len;
	res->set = xregion_alloc_array(ctx->region, struct rast_set_entry,
				       res->set_len);
	uint32_t *fieldnos = xregion_alloc_array(ctx->region, uint32_t,
						 SQL_MAX_COLUMN);
	uint32_t fieldno_count = 0;
	uint32_t i = 0;
	for (const struct stailq_entry *node = ast->set_list->head.first.value;
	     node != NULL; node = node->next.value, ++i) {
		const struct ast_set_list_entry *entry =
			stailq_entry(node, struct ast_set_list_entry, link);
		struct rast_set_entry *set = &res->set[i];
		set->name = entry->name;
		set->ids = entry->ids;
		if (resolve_target_expr(ctx, &res->target, entry->expr,
					&set->expr) != 0)
			return -1;
		struct ast_id_entry *id = NULL;
		if (entry->ids != NULL)
			id = stailq_first_entry(&entry->ids->head,
						struct ast_id_entry, link);
		do {
			const struct Token *tok = id != NULL ? &id->id :
						  &entry->name;
			const char *name = sql_name_temp(ctx->region, tok->z,
							 tok->n);
			uint32_t fieldno = space_def_fieldno(def, name);
			if (fieldno == UINT32_MAX && tok->z[0] != '"') {
				const char *legacy =
					sql_legacy_name_temp(ctx->region,
							     tok->z, tok->n);
				fieldno = space_def_fieldno(def, legacy);
			}
			if (fieldno == UINT32_MAX) {
				diag_set(ClientError,
					 ER_NO_SUCH_FIELD_NAME_IN_SPACE, name,
					 def->name);
				return -1;
			}
			for (uint32_t j = 0; j < fieldno_count; ++j) {
				if (fieldnos[j] != fieldno)
					continue;
				diag_set(ClientError, ER_SQL_PARSER_GENERIC,
					 tt_sprintf("set id list: duplicate "
						    "column name %s", name));
				return -1;
			}
			fieldnos[fieldno_count++] = fieldno;
			id = id != NULL ? stailq_next_entry(id, link) : NULL;
		} while (id != NULL);
	}
	if (def->opts.is_view) {
		res->view_select = xregion_alloc_object(ctx->region,
							struct rast_select);
		if (resolve_view_select(ctx, &ast->table, ast->where,
					res->view_select) != 0)
			return -1;
	}
	if (ast->where == NULL)
		return 0;
	res->where = xregion_alloc_object(ctx->region, struct rast_expr);
	return resolve_target_expr(ctx, &res->target, ast->where, res->where);
}

/**
 * Resolve the given AST UPDATE statement into the given rast_update. As
 * sqlUpdate() does, the space is checked before the SET clause, whose values
 * are resolved and whose columns are looked up in turn, and then the WHERE
 * clause is resolved, all in the scope of the space without allowing
 * aggregate functions.
 *
 * Returns 0 on success and -1 on error.
 */
static int
resolve_update(struct sql_resolve_context *ctx, const struct ast_update *ast,
	       struct rast_update *res)
{
	memset(res, 0, sizeof(*res));
	res->action = ast->action;
	struct sql_resolve_with *outer_with = ctx->with;
	struct sql_resolve_with with;
	if (ast->with != NULL && push_with(ctx, ast->with, &with) != 0)
		return -1;
	int rc = resolve_update_in(ctx, ast, res);
	if (rc == 0 && ast->with != NULL)
		rc = check_unused_ctes(ctx, &with);
	ctx->with = outer_with;
	return rc;
}

/** Build the resolved AST for the given parsed statement. */
struct sql_rast *
sql_resolve_ast(struct Parse *parser, const struct sql_ast *ast)
{
	struct sql_resolve_context ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.parser = parser;
	ctx.region = &parser->region;

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
	case SQL_AST_INSERT:
		if (resolve_insert(&ctx, ast->insert, &rast->insert) != 0) {
			parser->is_aborted = true;
			return NULL;
		}
		break;
	case SQL_AST_UPDATE:
		if (resolve_update(&ctx, ast->update, &rast->update) != 0) {
			parser->is_aborted = true;
			return NULL;
		}
		break;
	case SQL_AST_DELETE:
		if (resolve_delete(&ctx, ast->del, &rast->del) != 0) {
			parser->is_aborted = true;
			return NULL;
		}
		break;
	case SQL_AST_TRUNCATE:
		if (resolve_truncate(&ctx, &ast->truncate,
				     &rast->truncate) != 0) {
			parser->is_aborted = true;
			return NULL;
		}
		break;
	case SQL_AST_DROP_TABLE:
	case SQL_AST_DROP_VIEW:
		if (resolve_drop_table(&ast->drop_table,
				       ast->type == SQL_AST_DROP_VIEW,
				       &rast->drop_table) != 0) {
			parser->is_aborted = true;
			return NULL;
		}
		break;
	case SQL_AST_DROP_INDEX:
		if (resolve_drop_index(&ast->drop_index,
				       &rast->drop_index) != 0) {
			parser->is_aborted = true;
			return NULL;
		}
		break;
	case SQL_AST_SET_SESSION:
		rast->set_session.name = ast->set_session.name;
		if (resolve_expr(&ctx, ast->set_session.value,
				 &rast->set_session.value) != 0) {
			parser->is_aborted = true;
			return NULL;
		}
		break;
	case SQL_AST_PRAGMA:
		if (resolve_pragma(&ctx, &ast->pragma, &rast->pragma) != 0) {
			parser->is_aborted = true;
			return NULL;
		}
		break;
	case SQL_AST_SHOW_CREATE_TABLE: {
		const struct Token *name = &ast->show_create_table;
		if (name->n == 0)
			break;
		rast->show_create_table = sql_space_by_token(name);
		if (rast->show_create_table == NULL) {
			diag_set(ClientError, ER_NO_SUCH_SPACE,
				 sql_tt_name_from_token(name));
			parser->is_aborted = true;
			return NULL;
		}
		break;
	}
	case SQL_AST_TX_START:
	case SQL_AST_TX_COMMIT:
	case SQL_AST_TX_ROLLBACK:
		break;
	case SQL_AST_TX_SAVEPOINT_NEW: {
		const char *name = sql_name_temp(ctx.region, ast->savepoint.z,
						 ast->savepoint.n);
		if (sql_check_identifier_name(name, strlen(name)) != 0) {
			parser->is_aborted = true;
			return NULL;
		}
		rast->savepoint = ast->savepoint;
		break;
	}
	case SQL_AST_TX_SAVEPOINT_RELEASE:
	case SQL_AST_TX_SAVEPOINT_ROLLBACK:
		rast->savepoint = ast->savepoint;
		break;
	case SQL_AST_CREATE_TRIGGER:
		if (resolve_create_trigger(&ctx, &ast->create_trigger,
					   &rast->create_trigger) != 0) {
			parser->is_aborted = true;
			return NULL;
		}
		break;
	case SQL_AST_DROP_TRIGGER:
		rast->drop_trigger.name = ast->drop_trigger.name;
		rast->drop_trigger.if_exists = ast->drop_trigger.if_exists;
		break;
	case SQL_AST_ALTER_RENAME:
		if (resolve_alter_rename(&ctx, &ast->alter_rename,
					 &rast->alter_rename) != 0) {
			parser->is_aborted = true;
			return NULL;
		}
		break;
	case SQL_AST_ALTER_DROP_CONSTRAINT:
		if (resolve_drop_constraint(&ast->alter_drop_constraint,
					    &rast->drop_constraint) != 0) {
			parser->is_aborted = true;
			return NULL;
		}
		break;
	case SQL_AST_CREATE_INDEX:
		if (resolve_create_index(&ctx, &ast->create_index,
					 &rast->create_index) != 0) {
			parser->is_aborted = true;
			return NULL;
		}
		break;
	case SQL_AST_CREATE_TABLE:
		if (resolve_create_table(&ctx, &ast->create_table,
					 &rast->create_table) != 0) {
			parser->is_aborted = true;
			return NULL;
		}
		break;
	case SQL_AST_ALTER_ADD_COLUMN:
		if (resolve_add_column(&ctx, &ast->alter_add_column,
				       &rast->create_table) != 0) {
			parser->is_aborted = true;
			return NULL;
		}
		break;
	case SQL_AST_ALTER_ADD_CONSTRAINT:
		if (resolve_add_constraint(&ctx, &ast->alter_add_constraint,
					   &rast->add_constraint) != 0) {
			parser->is_aborted = true;
			return NULL;
		}
		break;
	case SQL_AST_CREATE_VIEW:
		if (resolve_create_view(&ctx, &ast->create_view,
					&rast->create_view) != 0) {
			parser->is_aborted = true;
			return NULL;
		}
		break;
	default:
		unreachable();
	}
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
	case TK_VAR_ANON:
	case TK_VAR_NUM:
	case TK_VAR_NAME: {
		struct Token name = {
			.z = expr->var.name,
			.n = expr->var.len,
			.isReserved = false,
		};
		res = sql_expr_new_dequoted(expr->op, &name);
		res->flags |= EP_Leaf;
		res->iColumn = expr->var.num;
		res->var_base = expr->var.base;
		break;
	}
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
	case TK_ID: {
		struct Token name = {
			.z = expr->z,
			.n = expr->n,
			.isReserved = false,
		};
		res = sql_expr_new(TK_ID, &name);
		break;
	}
	case TK_TRIGGER: {
		res = sql_expr_new_anon(TK_TRIGGER);
		res->iTable = expr->column.source;
		res->iColumn = expr->column.fieldno;
		res->space_def = parser->triggered_space->def;
		uint64_t *mask = res->iTable == 0 ? &parser->oldmask :
				 &parser->newmask;
		column_mask_set_fieldno(mask, res->iColumn);
		break;
	}
	case TK_COLUMN_REF: {
		const struct rast_build_scope *s = scope;
		for (uint32_t i = 0; i < expr->column.depth; ++i)
			s = s->parent;
		if (s->src != NULL) {
			res = sql_expr_new_column(s->src, expr->column.source,
						  expr->column.fieldno);
			struct SrcList_item *item =
				&s->src->a[expr->column.source];
			/* The right table of a LEFT JOIN can be all NULLs. */
			if ((item->fg.jointype & JT_LEFT) != 0)
				ExprSetProperty(res, EP_CanBeNull);
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
src_list_from_rast(struct Parse *parser, const struct rast_build_scope *parent,
		   struct rast_source_list *list)
{
	struct SrcList *res = NULL;
	for (uint32_t i = 0; i < list->len; ++i) {
		struct rast_source *src = &list->sources[i];
		res = sql_src_list_append(res, NULL);
		struct SrcList_item *item = &res->a[res->nSrc - 1];
		struct space *space;
		if (src->recursive != NULL) {
			/* The table is built before its SELECT. */
			space = src->recursive->space;
			assert(space != NULL);
			item->zName = sql_xstrdup(space->def->name);
			item->fg.isRecursive = true;
		} else if (src->def != NULL) {
			space = sql_template_space_new(sqlParseToplevel(parser),
						       src->def->name);
			uint32_t count = src->def->field_count;
			space->def->fields =
				xregion_alloc_array(&parser->region,
						    struct field_def, count);
			memcpy(space->def->fields, src->def->fields,
			       count * sizeof(struct field_def));
			space->def->field_count = count;
			if (src->is_cte) {
				item->zName = sql_xstrdup(src->def->name);
				src->space = space;
			}
		} else {
			space = space_by_id(src->space_id);
			assert(space != NULL);
			item->zName = sql_xstrdup(space->def->name);
		}
		if (src->alias != NULL)
			item->zAlias = sql_xstrdup(src->alias);
		item->space = space;
		item->iCursor = src->cursor;
		item->fg.jointype = src->join_type;
		item->fg.disallow_scan = src->disallow_scan;
		item->fg.notIndexed = src->is_not_indexed;
		if (src->index_id != UINT32_MAX) {
			item->pIBIndex = space_index(space, src->index_id)->def;
			item->indexed_by = sql_xstrdup(item->pIBIndex->name);
			item->fg.isIndexedBy = true;
		}
	}
	if (res == NULL)
		return NULL;
	for (uint32_t i = 0; i < list->len; ++i) {
		struct rast_select *select = list->sources[i].select;
		if (select == NULL)
			continue;
		struct SrcList_item *item = &res->a[i];
		item->pSelect = select_from_rast_in(parser, parent, select);
		if (parser->is_aborted) {
			sqlSrcListDelete(res);
			return NULL;
		}
		item->fg.isCorrelated = select->is_correlated;
		if (list->sources[i].def == NULL)
			continue;
		struct Select *leftmost = item->pSelect;
		while (leftmost->pPrior != NULL)
			leftmost = leftmost->pPrior;
		sqlSelectAddColumnTypeAndCollation(parser, item->space->def,
						   leftmost);
	}
	return res;
}

/**
 * Join the given term to the given WHERE expression with AND, without folding
 * it, since the terms are already resolved.
 */
static struct Expr *
where_append(struct Expr *where, struct Expr *term)
{
	if (where == NULL)
		return term;
	struct Expr *res = sql_expr_new_anon(TK_AND);
	sqlExprAttachSubtrees(res, where, term);
	ExprSetProperty(res, EP_Resolved);
	return res;
}

/**
 * Create the term `left = right` for the given pair of columns joined by
 * NATURAL or USING to the source with the given position in the given sources,
 * as addWhereTerm() does. The term of an outer join is a part of the join
 * restriction.
 */
static struct Expr *
join_column_from_rast(struct Parse *parser, struct SrcList *src,
		      uint32_t source, const struct rast_join_column *col,
		      bool is_outer)
{
	struct Expr *left = sql_expr_new_column(src, col->left_source,
						col->left_fieldno);
	struct Expr *right = sql_expr_new_column(src, source,
						 col->right_fieldno);
	struct Expr *res = sqlPExpr(parser, TK_EQ, left, right);
	ExprSetProperty(res, EP_Resolved);
	if (is_outer) {
		ExprSetProperty(res, EP_FromJoin);
		res->iRightJoinTable = right->iTable;
	}
	return res;
}

/**
 * Create the WHERE expression of the given resolved SELECT, with the given
 * scope of sources, from its WHERE clause, the columns joined by NATURAL or
 * USING and the ON clauses of its sources. As in sqlProcessJoin(), they are
 * joined to WHERE with AND in the order of the sources, and the terms of an
 * outer join are marked as a part of the join restriction.
 *
 * Returns NULL if there is no WHERE expression.
 */
static struct Expr *
where_from_rast(struct Parse *parser, const struct rast_build_scope *scope,
		const struct rast_select *select)
{
	struct Expr *res = NULL;
	if (select->where != NULL)
		res = expr_from_rast(parser, scope, select->where);
	for (uint32_t i = 0; i < select->sources.len; ++i) {
		const struct rast_source *src = &select->sources.sources[i];
		bool is_outer = (src->join_type & JT_OUTER) != 0;
		for (uint32_t j = 0; j < src->join_column_count; ++j) {
			const struct rast_join_column *col =
				&src->join_columns[j];
			struct Expr *eq =
				join_column_from_rast(parser, scope->src, i,
						      col, is_outer);
			res = where_append(res, eq);
		}
		if (src->on == NULL)
			continue;
		struct Expr *on = expr_from_rast(parser, scope, src->on);
		if (is_outer)
			sql_expr_set_join(on, scope->src->a[i].iCursor);
		res = where_append(res, on);
	}
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
		.src = src_list_from_rast(parser, parent, &select->sources),
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
	struct Expr *where = where_from_rast(parser, &scope, select);
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

/**
 * Assign cursors to the given resolved sources, as sqlSrcListAssignCursors()
 * does: every subquery source is followed by the sources of the rightmost part
 * of its SELECT. The SELECT of a table of a WITH clause is attached to the
 * source later, as the SELECT of a view.
 */
static void
rast_sources_assign_cursors(struct Parse *parser,
			    struct rast_source_list *sources)
{
	for (uint32_t i = 0; i < sources->len; ++i) {
		struct rast_source *src = &sources->sources[i];
		if (src->cursor >= 0)
			break;
		src->cursor = parser->nTab++;
		if (src->select != NULL && src->def != NULL && !src->is_cte)
			rast_sources_assign_cursors(parser,
						    &src->select->sources);
	}
}

/**
 * Assign cursors to the sources of the given resolved SELECT, which may be a
 * compound one, and of its subqueries, in the same order as the legacy code
 * does when expanding the SELECT: the parts of a compound SELECT from right to
 * left, and in every part its sources, the SELECTs of its views, subqueries
 * and tables of WITH clauses, and then the subqueries of its clauses. The ON
 * clauses are a part of WHERE. Even if the LIMIT of the SELECT is replaced,
 * its subqueries take their cursors. The rightmost part of a SELECT with
 * rast_select.is_rightmost_last set takes its cursors after the other parts.
 */
static void
rast_select_assign_cursors(struct Parse *parser, struct rast_select *select);

/**
 * Assign cursors to the sources of the subqueries of the given resolved
 * expression, visiting them in the same order as the legacy walker visits the
 * expression it creates. A reference to a result column is a copy of its
 * expression, which shares the cursors of the original.
 */
static void
rast_expr_assign_cursors(struct Parse *parser, const struct rast_expr *expr)
{
	switch (expr->op) {
	case TK_NOT:
	case TK_BITNOT:
	case TK_ISNULL:
	case TK_NOTNULL:
	case TK_UMINUS:
	case TK_UPLUS:
	case TK_CAST:
		rast_expr_assign_cursors(parser, expr->expr);
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
		rast_expr_assign_cursors(parser, expr->left);
		rast_expr_assign_cursors(parser, expr->right);
		break;
	case TK_BETWEEN:
		rast_expr_assign_cursors(parser, expr->between.value);
		rast_expr_assign_cursors(parser, expr->between.lower);
		rast_expr_assign_cursors(parser, expr->between.upper);
		break;
	case TK_IN:
		rast_expr_assign_cursors(parser, expr->in.value);
		if (expr->in.select != NULL)
			rast_select_assign_cursors(parser, expr->in.select);
		for (uint32_t i = 0; i < expr->in.len; ++i)
			rast_expr_assign_cursors(parser, &expr->in.exprs[i]);
		break;
	case TK_CASE:
		if (expr->cs.value != NULL)
			rast_expr_assign_cursors(parser, expr->cs.value);
		for (uint32_t i = 0; i < expr->cs.len; ++i)
			rast_expr_assign_cursors(parser, &expr->cs.exprs[i]);
		break;
	case TK_VECTOR:
	case TK_ARRAY:
	case TK_MAP:
		for (uint32_t i = 0; i < expr->list.len; ++i)
			rast_expr_assign_cursors(parser, &expr->list.exprs[i]);
		break;
	case TK_GETITEM:
		for (uint32_t i = 0; i < expr->getitem.len; ++i) {
			rast_expr_assign_cursors(parser,
						 &expr->getitem.exprs[i]);
		}
		rast_expr_assign_cursors(parser, expr->getitem.value);
		break;
	case TK_COLLATE:
		rast_expr_assign_cursors(parser, expr->coll.expr);
		break;
	case TK_SELECT:
	case TK_EXISTS:
		rast_select_assign_cursors(parser, expr->select);
		break;
	case TK_FUNCTION:
		for (uint32_t i = 0; i < expr->func.n_args; ++i)
			rast_expr_assign_cursors(parser, &expr->func.args[i]);
		break;
	default:
		break;
	}
}

/**
 * Assign cursors to the sources of the subqueries of the given resolved
 * expression list.
 */
static void
rast_expr_list_assign_cursors(struct Parse *parser,
			      const struct rast_expr_list *list)
{
	for (uint32_t i = 0; i < list->len; ++i)
		rast_expr_assign_cursors(parser, &list->exprs[i].expr);
}

/**
 * Assign cursors to the sources of the given part of a resolved SELECT and of
 * its subqueries, see rast_select_assign_cursors().
 */
static void
rast_part_assign_cursors(struct Parse *parser, struct rast_select *p)
{
	rast_sources_assign_cursors(parser, &p->sources);
	for (uint32_t i = 0; i < p->sources.len; ++i) {
		struct rast_select *sub = p->sources.sources[i].select;
		if (sub != NULL)
			rast_select_assign_cursors(parser, sub);
	}
	rast_expr_list_assign_cursors(parser, &p->columns);
	if (p->where != NULL)
		rast_expr_assign_cursors(parser, p->where);
	for (uint32_t i = 0; i < p->sources.len; ++i) {
		const struct rast_expr *on = p->sources.sources[i].on;
		if (on != NULL)
			rast_expr_assign_cursors(parser, on);
	}
	rast_expr_list_assign_cursors(parser, &p->group_by);
	if (p->having != NULL)
		rast_expr_assign_cursors(parser, p->having);
	rast_expr_list_assign_cursors(parser, &p->order_by);
	if (p->limit != NULL)
		rast_expr_assign_cursors(parser, p->limit);
	if (p->offset != NULL)
		rast_expr_assign_cursors(parser, p->offset);
}

static void
rast_select_assign_cursors(struct Parse *parser, struct rast_select *select)
{
	struct rast_select *p = select;
	if (select->is_rightmost_last)
		p = p->prior;
	for (; p != NULL; p = p->prior)
		rast_part_assign_cursors(parser, p);
	if (select->is_rightmost_last)
		rast_part_assign_cursors(parser, select);
}

struct Select *
select_from_rast(struct Parse *parser, struct rast_select *select)
{
	rast_select_assign_cursors(parser, select);
	return select_from_rast_in(parser, NULL, select);
}

struct Expr *
sql_func_expr_from_ast(struct Parse *parser, const struct ast_expr *ast)
{
	struct sql_resolve_context ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.parser = parser;
	ctx.region = &parser->region;
	ctx.is_row_expr = true;
	struct sql_resolve_scope scope;
	memset(&scope, 0, sizeof(scope));
	ctx.scope = &scope;
	struct rast_expr res;
	if (resolve_expr(&ctx, ast, &res) != 0) {
		parser->is_aborted = true;
		return NULL;
	}
	return expr_from_rast_in(parser, NULL, &res);
}

struct Expr *
sql_trigger_when_from_ast(struct Parse *parser, const struct ast_expr *when)
{
	struct sql_resolve_context ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.parser = parser;
	ctx.region = &parser->region;
	struct sql_resolve_scope scope;
	memset(&scope, 0, sizeof(scope));
	ctx.scope = &scope;
	struct rast_expr res;
	if (resolve_expr(&ctx, when, &res) != 0) {
		parser->is_aborted = true;
		return NULL;
	}
	return expr_from_rast_in(parser, NULL, &res);
}

struct SrcList *
src_list_from_rast_target(struct Parse *parser, struct rast_source_list *list)
{
	return src_list_from_rast(parser, NULL, list);
}

struct Expr *
expr_from_rast_in(struct Parse *parser, struct SrcList *src,
		  const struct rast_expr *expr)
{
	rast_expr_assign_cursors(parser, expr);
	struct rast_build_scope scope = {
		.src = src,
		.parent = NULL,
	};
	return expr_from_rast(parser, &scope, expr);
}
