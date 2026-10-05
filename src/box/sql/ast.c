/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 */
#include "sqlInt.h"

struct ast_id_list *
ast_id_list_append(struct region *region, struct ast_id_list *list,
		   const struct Token *id)
{
	if (list == NULL) {
		list = xregion_alloc_object(region, typeof(*list));
		stailq_create(&list->head);
		list->len = 0;
	}
	struct ast_id_entry *entry =
		xregion_alloc_object(region, typeof(*entry));
	entry->id = *id;
	stailq_add_tail(&list->head, &entry->link);
	list->len++;
	return list;
}

struct IdList *
id_list_from_ast(struct ast_id_list *list)
{
	if (list == NULL)
		return NULL;
	struct IdList *res = NULL;
	struct ast_id_entry *entry;
	stailq_foreach_entry(entry, &list->head, link) {
		res = sql_id_list_append(res, &entry->id);
	}
	return res;
}

struct ast_source *
ast_source_new(struct region *region)
{
	struct ast_source *src = xregion_alloc_object(region, typeof(*src));
	memset(src, 0, sizeof(*src));
	return src;
}

struct ast_source_list *
ast_source_list_append(struct region *region, struct ast_source_list *list,
		       struct ast_source *src)
{
	if (list == NULL) {
		list = xregion_alloc_object(region, typeof(*list));
		stailq_create(&list->head);
		list->len = 0;
	}
	stailq_add_tail(&list->head, &src->link);
	list->len++;
	return list;
}

struct SrcList *
src_list_from_ast(struct Parse *parser, struct ast_source_list *list)
{
	if (list == NULL)
		return NULL;
	struct SrcList *res = NULL;
	struct ast_source *src;
	stailq_foreach_entry(src, &list->head, link) {
		struct Select *select = select_from_ast(parser, src->select);
		struct Expr *join_on = expr_from_ast(parser, src->join_on);
		struct Token *name = src->name.n > 0 ? &src->name: NULL;
		res = sqlSrcListAppendFromTerm(res, name, &src->alias, select,
					       join_on, src->join_using,
					       src->disallow_scan);
		if (src->indexed_by.n != 0)
			sqlSrcListIndexedBy(res, &src->indexed_by);
		res->a[res->nSrc - 1].fg.jointype = src->join_type;
		if ((src->join_type & JT_INNER) != 0 &&
		    (src->join_type & JT_OUTER) != 0) {
			diag_set(ClientError, ER_SQL_PARSER_GENERIC,
				 "JOIN cannot be both OUTER and INNER");
			parser->is_aborted = true;
			break;
		}
		if ((src->join_type & JT_OUTER) != 0 &&
		    (src->join_type & (JT_LEFT | JT_RIGHT)) != JT_LEFT) {
			diag_set(ClientError, ER_UNSUPPORTED, "Tarantool",
				 "RIGHT and FULL OUTER JOINs");
			parser->is_aborted = true;
			break;
		}
	}
	if (parser->is_aborted) {
		sqlSrcListDelete(res);
		return NULL;
	}
	return res;
}

struct ast_select *
ast_select_new(struct region *region)
{
	struct ast_select *res = xregion_alloc_object(region, typeof(*res));
	memset(res, 0, sizeof(*res));
	rlist_create(&res->link);
	res->op = TK_SELECT;
	return res;
}

struct ast_select *
ast_select_new_asterisk(struct region *region, struct ast_source_list *sources)
{
	struct ast_select *res = ast_select_new(region);
	struct ast_expr *asterisk = ast_expr_new(region, "*", 1, TK_ASTERISK);
	res->columns = ast_expr_list_append(region, NULL, asterisk);
	res->columns->is_select_list = true;
	res->sources = sources;
	return res;
}

/**
 * Build single `struct Select` object from `struct ast_select` object. The WITH
 * clause is not converted.
 *
 * Return NULL on error.
 */
static struct Select *
select_from_ast_single(struct Parse *parser, struct ast_select *select)
{
	struct ExprList *columns = expr_list_from_ast(parser, select->columns);
	struct SrcList *list = src_list_from_ast(parser, select->sources);
	struct Expr *where = expr_from_ast(parser, select->where);
	struct ExprList *group_by = expr_list_from_ast(parser,
						       select->group_by);
	struct Expr *having = expr_from_ast(parser, select->having);
	struct ExprList *order_by = expr_list_from_ast(parser,
						       select->order_by);
	struct Expr *limit = expr_from_ast(parser, select->limit);
	struct Expr *offset = expr_from_ast(parser, select->offset);
	struct Select *res = sqlSelectNew(columns, list, where,
					  group_by, having, order_by,
					  select->flags, limit, offset);
	res->op = select->op;
	if (parser->is_aborted) {
		sql_select_delete(res);
		return NULL;
	}
	return res;
}

struct Select *
select_from_ast(struct Parse *parser, struct ast_select *select)
{
	if (select == NULL)
		return NULL;
	struct With *with = with_from_ast(parser, select->with);
	if (parser->is_aborted)
		return NULL;
	struct Select *prior = NULL;
	struct ast_select *part;
	int count = 1;
	rlist_foreach_entry(part, &select->link, link) {
		struct Select *cur = select_from_ast_single(parser, part);
		if (parser->is_aborted) {
			sqlWithDelete(with);
			sql_select_delete(prior);
			return NULL;
		}
		if (prior != NULL) {
			cur->pPrior = prior;
			prior->pNext = cur;
		}
		prior = cur;
		count++;
	}
	struct Select *res = select_from_ast_single(parser, select);
	if (parser->is_aborted) {
		sqlWithDelete(with);
		sql_select_delete(prior);
		return NULL;
	}
	res->pWith = with;
	if (prior != NULL) {
		res->pPrior = prior;
		prior->pNext = res;
	}
	if ((res->selFlags & SF_MultiValue) == 0 &&
	    count > SQL_MAX_COMPOUND_SELECT) {
		diag_set(ClientError, ER_SQL_PARSER_LIMIT, "The number of "
			 "UNION or EXCEPT or INTERSECT operations", count,
			 SQL_MAX_COMPOUND_SELECT);
		parser->is_aborted = true;
		sql_select_delete(res);
		return NULL;
	}
	return res;
}

struct ast_with_list *
ast_with_list_append(struct region *region, struct ast_with_list *list,
		     const struct Token *name, struct ast_id_list *columns,
		     struct ast_select *select)
{
	if (list == NULL) {
		list = xregion_alloc_object(region, typeof(*list));
		stailq_create(&list->head);
		list->len = 0;
	}
	struct ast_with_entry *entry =
		xregion_alloc_object(region, typeof(*entry));
	entry->name = *name;
	entry->columns = columns;
	entry->select = select;
	stailq_add_tail(&list->head, &entry->link);
	list->len++;
	return list;
}

struct ExprList *
expr_list_from_ids(struct Parse *parser, struct ast_id_list *list)
{
	if (list == NULL)
		return NULL;
	struct ExprList *res = NULL;
	struct ast_id_entry *entry;
	stailq_foreach_entry(entry, &list->head, link) {
		res = sql_expr_list_append(res, NULL);
		sqlExprListSetName(parser, res, &entry->id, 1);
	}
	if (parser->is_aborted) {
		sql_expr_list_delete(res);
		return NULL;
	}
	return res;
}

struct With *
with_from_ast(struct Parse *parser, struct ast_with_list *list)
{
	if (list == NULL)
		return NULL;
	struct With *res = NULL;
	struct ast_with_entry *entry;
	stailq_foreach_entry(entry, &list->head, link) {
		struct ExprList *cols =
			expr_list_from_ids(parser, entry->columns);
		struct Select *select = select_from_ast(parser, entry->select);
		res = sqlWithAdd(parser, res, &entry->name, cols, select);
	}
	if (parser->is_aborted) {
		sqlWithDelete(res);
		return NULL;
	}
	return res;
}

struct ast_expr *
ast_expr_new(struct region *region, const char *str, uint32_t len, uint8_t op)
{
	struct ast_expr *expr = xregion_alloc_object(region, typeof(*expr));
	expr->left = NULL;
	expr->right = NULL;
	expr->str = str;
	expr->len = len;
	expr->op = op;
	return expr;
}

struct ast_expr *
ast_var_new(struct sql_parser_context *ctx, const char *str, uint32_t len,
	    uint8_t op)
{
	struct ast_expr *expr = ast_expr_new(ctx->region, str, len, op);
	expr->var.num = 0;
	expr->var.base = 0;
	if (op == TK_VAR_ANON) {
		assert(len == 1 && str[0] == '?');
		expr->var.num = ctx->var_offset++;
		expr->var.base = ctx->var_base;
		if (ctx->var_base + expr->var.num > ctx->var_count)
			ctx->var_count = ctx->var_base + expr->var.num;
	} else if (op == TK_VAR_NUM) {
		assert(str[0] == '$');
		int64_t num;
		bool is_neg;
		bool is_ok = sql_atoi64(&str[1], &num, &is_neg, len - 1) == 0;
		if (is_neg || num < 1) {
			diag_set(ClientError, ER_SQL_PARSER_GENERIC,
				 "Index of binding slots must start from 1");
			ctx->is_aborted = true;
			return expr;
		}
		if (!is_ok || num > SQL_BIND_PARAMETER_MAX) {
			diag_set(ClientError, ER_SQL_BIND_PARAMETER_MAX,
				 SQL_BIND_PARAMETER_MAX);
			ctx->is_aborted = true;
			return expr;
		}
		expr->var.num = num;
		ctx->var_base = 0;
		ctx->var_offset = num + 1;
		if (num > ctx->var_count)
			ctx->var_count = num;
	} else {
		assert(op == TK_VAR_NAME);
		/*
		 * The other prefixes are followed by a name by the tokenizer,
		 * while a colon is a separate token.
		 */
		if (str[0] == ':' && IdChar(str[1]) == 0) {
			diag_set(ClientError, ER_SQL_PARSER_GENERIC,
				 tt_sprintf("Wrong bind variable name '%.*s'",
					    len, str));
			ctx->is_aborted = true;
			return expr;
		}
		int num = sqlVListNameToNum(ctx->var_list, str, len);
		if (num == 0) {
			num = ++ctx->var_count;
			ctx->var_list = sqlVListAdd(ctx->var_list, str, len,
						    num);
		}
		ctx->var_base = num;
		ctx->var_offset = 1;
	}
	if (expr->var.num > SQL_BIND_PARAMETER_MAX) {
		diag_set(ClientError, ER_SQL_BIND_PARAMETER_MAX,
			 SQL_BIND_PARAMETER_MAX);
		ctx->is_aborted = true;
	}
	return expr;
}

struct ast_expr_list *
ast_expr_list_append(struct region *region, struct ast_expr_list *list,
		     struct ast_expr *expr)
{
	struct ast_expr_list_entry *entry =
		xregion_alloc_object(region, typeof(*entry));
	entry->name = Token_nil;
	entry->expr = expr;
	entry->order = SORT_ORDER_ASC;
	entry->autoinc = false;
	if (list == NULL) {
		list = xregion_alloc_object(region, typeof(*list));
		stailq_create(&list->head);
		list->len = 0;
		list->is_select_list = false;
	}
	stailq_add_tail(&list->head, &entry->link);
	list->len++;
	return list;
}

void
ast_expr_list_set_name(struct ast_expr_list *list, struct Token *name)
{
	struct ast_expr_list_entry *entry =
		stailq_last_entry(&list->head, typeof(*entry), link);
	entry->name = *name;
}

void
ast_expr_list_set_order(struct ast_expr_list *list, enum sort_order order)
{
	struct ast_expr_list_entry *entry =
		stailq_last_entry(&list->head, typeof(*entry), link);
	entry->order = order;
}

void
ast_expr_list_set_autoinc(struct ast_expr_list *list, bool autoinc)
{
	struct ast_expr_list_entry *entry =
		stailq_last_entry(&list->head, typeof(*entry), link);
	entry->autoinc = autoinc;
}

struct ExprList *
expr_list_from_ast(struct Parse *parser, struct ast_expr_list *list)
{
	if (list == NULL)
		return NULL;
	struct ExprList *res = NULL;
	struct ast_expr_list_entry *entry;
	stailq_foreach_entry(entry, &list->head, link) {
		struct ast_expr *ast_expr = entry->expr;
		struct Expr *expr = expr_from_ast(parser, ast_expr);
		if (expr == NULL)
			break;
		res = sql_expr_list_append(res, expr);
		if (entry->name.n > 0)
			sqlExprListSetName(parser, res, &entry->name, 1);
		if (list->is_select_list)
			sqlExprListSetSpan(res, ast_expr->str, ast_expr->len);
		if (entry->order != SORT_ORDER_ASC)
			sqlExprListSetSortOrder(res, entry->order);
		if (entry->autoinc) {
			if (parser->autoinc_fieldno != NULL) {
				diag_set(ClientError, ER_SQL_PARSER_GENERIC,
					 "Table must feature at most one "
					 "AUTOINCREMENT field");
				parser->is_aborted = true;
				break;
			}
			if (expr != sqlExprSkipCollate(expr)) {
				diag_set(ClientError, ER_SQL_PARSER_GENERIC,
					 "AUTOINCREMENT cannot be used with "
					 "a non-integer column");
				parser->is_aborted = true;
				break;
			}
			parser->autoinc_fieldno = &expr->iColumn;
		}
	}
	if (parser->is_aborted) {
		sql_expr_list_delete(res);
		return NULL;
	}
	return res;
}

/** Build an identifier `struct Expr`, e.g. a function or collation name. */
static struct Expr *
expr_id(struct ast_expr *expr)
{
	struct Token t;
	t.z = expr->str;
	t.n = expr->len;
	t.isReserved = false;
	struct Expr *res = sql_expr_new_dequoted(expr->op, &t);
	res->type = FIELD_TYPE_SCALAR;
	if (expr->str[0] != '"')
		res->flags |= EP_Lookup2;
	return res;
}

/** Build a leaf `struct Expr` (a literal) of the given field type. */
static struct Expr *
expr_leaf(struct ast_expr *expr, enum field_type type)
{
	struct Token t;
	t.z = expr->str;
	t.n = expr->len;
	t.isReserved = false;
	struct Expr *res = sql_expr_new_dequoted(expr->op, &t);
	res->type = type;
	res->flags |= EP_Leaf;
	return res;
}

/** Build a `struct Expr` for a bound variable. */
static struct Expr *
expr_var(struct ast_expr *expr)
{
	struct Token t;
	t.z = expr->str;
	t.n = expr->len;
	t.isReserved = false;
	struct Expr *res = sql_expr_new_dequoted(expr->op, &t);
	res->type = FIELD_TYPE_BOOLEAN;
	res->flags |= EP_Leaf;
	res->iColumn = expr->var.num;
	res->var_base = expr->var.base;
	return res;
}

/**
 * Build a `struct Expr` for a unary operator applied to `expr->left`.
 *
 * Return NULL on error.
 */
static struct Expr *
expr_unary(struct Parse *parser, struct ast_expr *expr)
{
	struct Expr *left = expr_from_ast(parser, expr->left);
	if (parser->is_aborted)
		return NULL;
	struct Expr *res = sqlPExpr(parser, expr->op, left, NULL);
	if (parser->is_aborted) {
		sql_expr_delete(res);
		return NULL;
	}
	return res;
}

/**
 * Build a `struct Expr` for unary minus. A unary minus applied to a numeric
 * literal is folded into a single signed literal, so the sign is resolved once
 * here rather than lazily at opcode creation.
 *
 * Return NULL on error.
 */
static struct Expr *
expr_uminus(struct Parse *parser, struct ast_expr *expr)
{
	struct Expr *left = expr_from_ast(parser, expr->left);
	if (parser->is_aborted)
		return NULL;
	if (left->op == TK_DECIMAL) {
		decimal_minus(left->v.d, left->v.d);
		return left;
	}
	if (left->op == TK_FLOAT) {
		left->v.f = -left->v.f;
		return left;
	}
	if (left->op == TK_INTEGER) {
		if ((left->flags & EP_Negative) != 0) {
			/* Compute -u in unsigned to avoid signed overflow. */
			left->v.u = ~left->v.u + 1;
			left->flags &= ~EP_Negative;
			return left;
		}
		if (left->v.u == 0)
			return left;
		int64_t value;
		if (sql_neg_uint(&value, left->v.u) != 0) {
			parser->is_aborted = true;
			sql_expr_delete(left);
			return NULL;
		}
		left->v.u = (uint64_t)value;
		left->flags |= EP_Negative;
		return left;
	}
	struct Expr *res = sqlPExpr(parser, TK_UMINUS, left, NULL);
	if (parser->is_aborted) {
		sql_expr_delete(res);
		return NULL;
	}
	return res;
}

/**
 * Build a `struct Expr` for unary plus. A unary plus applied to a numeric
 * literal is a no-op, so it is folded away and the literal is returned as is.
 *
 * Return NULL on error.
 */
static struct Expr *
expr_uplus(struct Parse *parser, struct ast_expr *expr)
{
	struct Expr *left = expr_from_ast(parser, expr->left);
	if (parser->is_aborted)
		return NULL;
	if (left->op == TK_DECIMAL || left->op == TK_FLOAT)
		return left;
	if (left->op == TK_INTEGER)
		return left;
	struct Expr *res = sqlPExpr(parser, TK_UPLUS, left, NULL);
	if (parser->is_aborted) {
		sql_expr_delete(res);
		return NULL;
	}
	return res;
}

/**
 * Build a `struct Expr` for a binary operator applied to left and right.
 *
 * Return NULL on error.
 */
static struct Expr *
expr_binary(struct Parse *parser, struct ast_expr *expr)
{
	struct Expr *left = expr_from_ast(parser, expr->left);
	if (parser->is_aborted)
		return NULL;
	struct Expr *right = expr_from_ast(parser, expr->right);
	if (parser->is_aborted) {
		sql_expr_delete(left);
		return NULL;
	}
	struct Expr *res = sqlPExpr(parser, expr->op, left, right);
	if (parser->is_aborted) {
		sql_expr_delete(res);
		return NULL;
	}
	return res;
}

/**
 * Build a `struct Expr` of the given type whose operand is expr->list.
 *
 * Return NULL on error.
 */
static struct Expr *
expr_list(struct Parse *parser, struct ast_expr *expr, enum field_type type)
{
	struct Expr *res = sql_expr_new_anon(expr->op);
	res->x.pList = expr_list_from_ast(parser, expr->list);
	res->type = type;
	sqlExprSetHeightAndFlags(parser, res);
	if (parser->is_aborted) {
		sql_expr_delete(res);
		return NULL;
	}
	return res;
}

/**
 * Build a `struct Expr` with a left operand and an expression list operand.
 *
 * Return NULL on error.
 */
static struct Expr *
expr_left_and_list(struct Parse *parser, struct ast_expr *expr)
{
	struct Expr *left = expr_from_ast(parser, expr->left);
	if (parser->is_aborted)
		return NULL;
	struct Expr *res = sqlPExpr(parser, expr->op, left, NULL);
	res->x.pList = expr_list_from_ast(parser, expr->list);
	sqlExprSetHeightAndFlags(parser, res);
	if (parser->is_aborted) {
		sql_expr_delete(res);
		return NULL;
	}
	return res;
}

/**
 * Build a `struct Expr` for a function call expression.
 *
 * Return NULL on error.
 */
static struct Expr *
expr_function(struct Parse *parser, struct ast_expr *expr)
{
	struct Expr *res = expr_id(expr->left);
	res->op = TK_FUNCTION;
	if (expr->right == NULL)
		return res;
	if (expr->right->op == TK_DISTINCT)
		res->flags |= EP_Distinct;
	if (expr->right->list == NULL)
		return res;
	if (expr->right->list->len > SQL_MAX_FUNCTION_ARG) {
		const char *err = tt_sprintf("Number of arguments to "
					     "function %s", res->u.zToken);
		diag_set(ClientError, ER_SQL_PARSER_LIMIT, err,
			 expr->right->list->len, SQL_MAX_FUNCTION_ARG);
		parser->is_aborted = true;
		sql_expr_delete(res);
		return NULL;
	}
	res->x.pList = expr_list_from_ast(parser, expr->right->list);
	sqlExprSetHeightAndFlags(parser, res);
	if (parser->is_aborted) {
		sql_expr_delete(res);
		return NULL;
	}
	return res;
}

/**
 * Build a `struct Expr` for an IN expression (subquery or value list).
 *
 * Return NULL on error.
 */
static struct Expr *
expr_in(struct Parse *parser, struct ast_expr *expr)
{
	if (expr->right->op == TK_SELECT) {
		struct Expr *left = expr_from_ast(parser, expr->left);
		if (parser->is_aborted)
			return NULL;
		struct Select *select = select_from_ast(parser,
							expr->right->select);
		if (parser->is_aborted) {
			sql_expr_delete(left);
			return NULL;
		}
		struct Expr *res = sqlPExpr(parser, expr->op, left, NULL);
		sqlPExprAddSelect(parser, res, select);
		if (parser->is_aborted) {
			sql_expr_delete(res);
			return NULL;
		}
		return res;
	}
	assert(expr->right->op == TK_VECTOR);
	if (expr->right->list == NULL || expr->right->list->len == 0) {
		struct Expr *res = sql_expr_new_anon(TK_FALSE);
		res->type = FIELD_TYPE_BOOLEAN;
		return res;
	}
	if (expr->right->list->len == 1) {
		struct Expr *left = expr_from_ast(parser, expr->left);
		if (parser->is_aborted)
			return NULL;
		struct ast_expr_list_entry *entry =
			stailq_first_entry(&expr->right->list->head,
					   typeof(*entry), link);
		struct Expr *right = expr_from_ast(parser, entry->expr);
		if (parser->is_aborted) {
			sql_expr_delete(left);
			return NULL;
		}
		struct Expr *res = sqlPExpr(parser, TK_EQ, left, right);
		if (parser->is_aborted) {
			sql_expr_delete(res);
			return NULL;
		}
		return res;
	}
	struct Expr *left = expr_from_ast(parser, expr->left);
	if (parser->is_aborted)
		return NULL;
	struct Expr *res = sqlPExpr(parser, expr->op, left, NULL);
	res->x.pList = expr_list_from_ast(parser, expr->right->list);
	sqlExprSetHeightAndFlags(parser, res);
	if (parser->is_aborted) {
		sql_expr_delete(res);
		return NULL;
	}
	return res;
}

/**
 * Build a `struct Expr` for a subscripting operator expression.
 *
 * Return NULL on error.
 */
static struct Expr *
expr_getitem(struct Parse *parser, struct ast_expr *expr)
{
	struct Expr *left = expr_from_ast(parser, expr->left);
	if (parser->is_aborted)
		return NULL;
	struct ExprList *list = expr_list_from_ast(parser, expr->list);
	if (parser->is_aborted) {
		sql_expr_delete(left);
		return NULL;
	}
	struct Expr *res = sql_expr_new_anon(expr->op);
	res->x.pList = sql_expr_list_append(list, left);
	res->type = FIELD_TYPE_ANY;
	sqlExprSetHeightAndFlags(parser, res);
	if (parser->is_aborted) {
		sql_expr_delete(res);
		return NULL;
	}
	return res;
}

/**
 * Build a `struct Expr` for a VARBINARY value.
 *
 * Return NULL on error.
 */
static struct Expr *
expr_varbinary(struct ast_expr *expr)
{
	assert(expr->op == TK_BLOB);
	assert(expr->str[0] == 'x' || expr->str[0] == 'X');
	assert(expr->str[1] == '\'' && expr->str[expr->len - 1] == '\'');
	assert(expr->len > 2 && expr->len % 2 == 1);

	uint32_t len = (expr->len - 3) / 2;
	struct Expr *res = sql_expr_new_empty(expr->op, len);
	res->type = FIELD_TYPE_VARBINARY;
	res->flags |= EP_Leaf;
	res->v.n = len;
	res->v.z = (char *)&res[1];
	for (uint32_t i = 0; i < len; ++i) {
		res->v.z[i] = (sqlHexToInt(expr->str[2 + i * 2]) << 4 |
			       sqlHexToInt(expr->str[3 + i * 2]));
	}
	return res;
}

/**
 * Build a `struct Expr` for an INTEGER value.
 *
 * Return NULL on error.
 */
static struct Expr *
expr_integer(struct Parse *parser, struct ast_expr *expr)
{
	uint32_t used = region_used(&parser->region);
	char *str = xregion_alloc(&parser->region, expr->len + 1);
	memcpy(str, expr->str, expr->len);
	str[expr->len] = '\0';

	struct Expr *res = sql_expr_new_empty(expr->op, 0);
	res->type = FIELD_TYPE_INTEGER;
	res->flags |= EP_Leaf;
	if (sql_uint_from_str(&res->v.u, str) != 0) {
		parser->is_aborted = true;
		sql_expr_delete(res);
		return NULL;
	}

	region_truncate(&parser->region, used);
	return res;
}

/**
 * Build a `struct Expr` for a DECIMAL value.
 *
 * Return NULL on error.
 */
static struct Expr *
expr_decimal(struct Parse *parser, struct ast_expr *expr)
{
	uint32_t used = region_used(&parser->region);
	char *str = xregion_alloc(&parser->region, expr->len + 1);
	memcpy(str, expr->str, expr->len);
	str[expr->len] = '\0';

	struct Expr *res = sql_expr_new_empty(expr->op, sizeof(decimal_t));
	res->type = FIELD_TYPE_DECIMAL;
	res->flags |= EP_Leaf;
	res->v.d = (decimal_t *)&res[1];
	if (sql_dec_from_str(res->v.d, str) != 0) {
		parser->is_aborted = true;
		sql_expr_delete(res);
		return NULL;
	}

	region_truncate(&parser->region, used);
	return res;
}

/**
 * Build a `struct Expr` for a COLLATE.
 *
 * Return NULL on error.
 */
static struct Expr *
expr_collate(struct Parse *parser, struct ast_expr *expr)
{
	uint32_t id;
	if (sql_coll_id(&id, expr->right->str, expr->right->len) != 0) {
		parser->is_aborted = true;
		return NULL;
	}
	struct Expr *left = expr_from_ast(parser, expr->left);
	if (parser->is_aborted)
		return NULL;
	return sql_expr_new_collate(parser, left, id);
}

struct Expr *
expr_from_ast(struct Parse *parser, struct ast_expr *expr)
{
	if (expr == NULL)
		return NULL;
	struct Expr *res = NULL;
	switch (expr->op) {
	case TK_STRING:
		res = sql_expr_new_string(expr->str, expr->len);
		res->v.len = sql_dequote(res->v.s, expr->len);
		res->v.s[res->v.len] = '\0';
		break;
	case TK_BLOB:
		res = expr_varbinary(expr);
		break;
	case TK_INTEGER:
		res = expr_integer(parser, expr);
		break;
	case TK_FLOAT:
		res = sql_expr_new_empty(expr->op, 0);
		res->type = FIELD_TYPE_DOUBLE;
		res->flags |= EP_Leaf;
		sqlAtoF(expr->str, &res->v.f, expr->len);
		break;
	case TK_DECIMAL:
		res = expr_decimal(parser, expr);
		break;
	case TK_TRUE:
		res = sql_expr_new_empty(expr->op, 0);
		res->type = FIELD_TYPE_BOOLEAN;
		res->flags |= EP_Leaf;
		res->v.b = true;
		break;
	case TK_FALSE:
	case TK_UNKNOWN:
		res = sql_expr_new_empty(expr->op, 0);
		res->type = FIELD_TYPE_BOOLEAN;
		res->flags |= EP_Leaf;
		assert(!res->v.b);
		break;
	case TK_VAR_ANON:
	case TK_VAR_NUM:
	case TK_VAR_NAME:
		res = expr_var(expr);
		break;
	case TK_AND:
	case TK_OR:
	case TK_LT:
	case TK_LE:
	case TK_GT:
	case TK_GE:
	case TK_EQ:
	case TK_NE:
	case TK_BITAND:
	case TK_BITOR:
	case TK_LSHIFT:
	case TK_RSHIFT:
	case TK_PLUS:
	case TK_MINUS:
	case TK_STAR:
	case TK_SLASH:
	case TK_REM:
	case TK_CONCAT:
	case TK_DOT:
		res = expr_binary(parser, expr);
		break;
	case TK_PARENTHESES:
		while (expr->op == TK_PARENTHESES)
			expr = expr->left;
		res = expr_from_ast(parser, expr);
		break;
	case TK_COLLATE:
		res = expr_collate(parser, expr);
		break;
	case TK_CAST:
		res = expr_unary(parser, expr);
		if (res == NULL)
			break;
		res->type = sql_type_to_field_type(expr->type);
		break;
	case TK_NOT:
	case TK_BITNOT:
	case TK_NOTNULL:
	case TK_ISNULL:
		res = expr_unary(parser, expr);
		break;
	case TK_UPLUS:
		res = expr_uplus(parser, expr);
		break;
	case TK_UMINUS:
		res = expr_uminus(parser, expr);
		break;
	case TK_ARRAY:
		res = expr_list(parser, expr, FIELD_TYPE_ARRAY);
		break;
	case TK_MAP:
		res = expr_list(parser, expr, FIELD_TYPE_MAP);
		break;
	case TK_GETITEM:
		res = expr_getitem(parser, expr);
		break;
	case TK_FUNCTION:
		res = expr_function(parser, expr);
		break;
	case TK_BETWEEN:
		res = expr_left_and_list(parser, expr);
		break;
	case TK_VECTOR:
		res = expr_list(parser, expr, FIELD_TYPE_ANY);
		break;
	case TK_IN:
		res = expr_in(parser, expr);
		break;
	case TK_EXISTS:
	case TK_SELECT: {
		struct Select *select = select_from_ast(parser, expr->select);
		if (parser->is_aborted)
			return NULL;
		res = sql_expr_new_anon(expr->op);
		sqlPExprAddSelect(parser, res, select);
		break;
	}
	case TK_RAISE:
		if (expr->on_conflict_action != ON_CONFLICT_ACTION_IGNORE) {
			res = sql_expr_new_empty(expr->op, expr->left->len + 1);
			res->v.raise = (char *)&res[1];
			memcpy(res->v.raise, expr->left->str, expr->left->len);
			uint32_t n = sql_dequote(res->v.raise, expr->left->len);
			res->v.raise[n] = '\0';
		} else {
			res = sql_expr_new_anon(expr->op);
		}
		res->type = FIELD_TYPE_ANY;
		res->flags |= EP_Leaf;
		res->op = TK_RAISE;
		res->v.action = expr->on_conflict_action;
		break;
	case TK_CASE:
		if (expr->left != NULL)
			res = expr_left_and_list(parser, expr);
		else
			res = expr_list(parser, expr, FIELD_TYPE_ANY);
		break;
	default:
		res = expr_leaf(expr, FIELD_TYPE_SCALAR);
		break;
	}
	if (parser->is_aborted) {
		sql_expr_delete(res);
		return NULL;
	}
	return res;
}

struct ast_insert *
ast_insert_new(struct region *region)
{
	struct ast_insert *res = xregion_alloc_object(region, typeof(*res));
	memset(res, 0, sizeof(*res));
	return res;
}

struct ast_set_list *
ast_set_list_append_expr(struct region *region, struct ast_set_list *list,
			 struct Token *name, struct ast_expr *expr)
{
	if (list == NULL) {
		list = xregion_alloc_object(region, typeof(*list));
		stailq_create(&list->head);
		list->len = 0;
	}
	struct ast_set_list_entry *entry =
		xregion_alloc_object(region, typeof(*entry));
	memset(entry, 0, sizeof(*entry));
	entry->name = *name;
	entry->expr = expr;
	stailq_add_tail(&list->head, &entry->link);
	list->len++;
	return list;
}

struct ast_set_list *
ast_set_list_append_vector(struct region *region, struct ast_set_list *list,
			   struct ast_id_list *ids, struct ast_expr *expr)
{
	if (list == NULL) {
		list = xregion_alloc_object(region, typeof(*list));
		stailq_create(&list->head);
		list->len = 0;
	}
	struct ast_set_list_entry *entry =
		xregion_alloc_object(region, typeof(*entry));
	memset(entry, 0, sizeof(*entry));
	entry->ids = ids;
	entry->expr = expr;
	stailq_add_tail(&list->head, &entry->link);
	list->len++;
	return list;
}

struct ExprList *
expr_list_from_set_list(struct Parse *parser, struct ast_set_list *list)
{
	assert(list != NULL);
	struct ExprList *res = NULL;
	struct ast_set_list_entry *entry;
	stailq_foreach_entry(entry, &list->head, link) {
		struct Expr *expr = expr_from_ast(parser, entry->expr);
		if (expr == NULL)
			break;
		if (entry->ids != NULL) {
			struct IdList *ids = id_list_from_ast(entry->ids);
			res = sqlExprListAppendVector(parser, res, ids, expr);
		} else {
			res = sql_expr_list_append(res, expr);
			sqlExprListSetName(parser, res, &entry->name, 1);
		}
	}
	if (parser->is_aborted) {
		sql_expr_list_delete(res);
		return NULL;
	}
	return res;
}

struct ast_update *
ast_update_new(struct region *region)
{
	struct ast_update *res = xregion_alloc_object(region, typeof(*res));
	memset(res, 0, sizeof(*res));
	return res;
}

struct ast_trigger_action_list *
ast_trigger_action_list_append(struct region *region,
			       struct ast_trigger_action_list *list,
			       struct ast_trigger_action *action)
{
	if (list == NULL) {
		list = xregion_alloc_object(region, typeof(*list));
		stailq_create(&list->head);
		list->len = 0;
	}
	stailq_add_tail(&list->head, &action->link);
	list->len++;
	return list;
}

struct ast_delete *
ast_delete_new(struct region *region)
{
	struct ast_delete *res = xregion_alloc_object(region, typeof(*res));
	memset(res, 0, sizeof(*res));
	return res;
}

struct ast_trigger_action *
ast_trigger_action_new(struct region *region)
{
	struct ast_trigger_action *res =
		xregion_alloc_object(region, typeof(*res));
	memset(res, 0, sizeof(*res));
	return res;
}

struct ast_property *
ast_property_new(struct region *region)
{
	struct ast_property *res = xregion_alloc_object(region, typeof(*res));
	memset(res, 0, sizeof(*res));
	return res;
}

struct ast_property_list *
ast_property_list_append(struct region *region, struct ast_property_list *list,
			 struct ast_property *property)
{
	if (list == NULL) {
		list = xregion_alloc_object(region, typeof(*list));
		stailq_create(&list->head);
		list->len = 0;
	}
	stailq_add_tail(&list->head, &property->link);
	list->len++;
	return list;
}

struct ast_column *
ast_column_new(struct region *region)
{
	struct ast_column *res = xregion_alloc_object(region, typeof(*res));
	memset(res, 0, sizeof(*res));
	return res;
}

struct ast_table_properties *
ast_table_properties_new(struct region *region)
{
	struct ast_table_properties *res =
		xregion_alloc_object(region, typeof(*res));
	stailq_create(&res->columns);
	stailq_create(&res->constraints);
	return res;
}

struct ast_table_properties *
ast_table_properties_append_column(struct ast_table_properties *properties,
				   struct ast_column *column)
{
	stailq_add_tail(&properties->columns, &column->link);
	return properties;
}

struct ast_table_properties *
ast_table_properties_append_constraint(struct ast_table_properties *properties,
				       struct ast_property *constraint)
{
	stailq_add_tail(&properties->constraints, &constraint->link);
	return properties;
}

/**
 * Check the given AST expression and set the given result. The checks are the
 * ones not depending on the names: the literals, the collations, the number
 * of the arguments of the functions and of the parts of the compound SELECTs,
 * the types of the joins and the heights of the expressions.
 */
static void
ast_check_expr(struct Parse *parser, struct ast_expr *expr,
	       struct ast_check_result *res);

/** Check the given name, which is an identifier, dequoted. */
static void
ast_check_name(struct Parse *parser, const struct Token *name)
{
	size_t used = region_used(&parser->region);
	const char *str = sql_name_temp(&parser->region, name->z, name->n);
	if (sql_check_identifier_name(str, strlen(str)) != 0)
		parser->is_aborted = true;
	region_truncate(&parser->region, used);
}

/**
 * Check the given AST expression list and set the largest height of its
 * expressions and whether they are all constant in the given result. The check
 * stops at the first erroneous expression. There must be at most one
 * AUTOINCREMENT field, counting the one of the parser, and it must be a column
 * without COLLATE.
 */
static void
ast_check_expr_list(struct Parse *parser, struct ast_expr_list *list,
		    struct ast_check_result *res)
{
	memset(res, 0, sizeof(*res));
	res->is_constant = true;
	if (list == NULL)
		return;
	bool has_autoinc = parser->autoinc_fieldno != NULL;
	struct ast_expr_list_entry *entry;
	stailq_foreach_entry(entry, &list->head, link) {
		struct ast_check_result expr;
		ast_check_expr(parser, entry->expr, &expr);
		if (parser->is_aborted)
			break;
		res->height = MAX(res->height, expr.height);
		res->is_constant = res->is_constant && expr.is_constant;
		if (entry->name.n > 0)
			ast_check_name(parser, &entry->name);
		if (!entry->autoinc)
			continue;
		if (has_autoinc) {
			diag_set(ClientError, ER_SQL_PARSER_GENERIC,
				 "Table must feature at most one "
				 "AUTOINCREMENT field");
			parser->is_aborted = true;
			break;
		}
		if (expr.op == TK_COLLATE) {
			diag_set(ClientError, ER_SQL_PARSER_GENERIC,
				 "AUTOINCREMENT cannot be used with "
				 "a non-integer column");
			parser->is_aborted = true;
			break;
		}
		has_autoinc = true;
	}
}

/** Check the given AST SELECT and set the largest height of its expressions. */
static void
ast_check_select_height(struct Parse *parser, struct ast_select *select,
			int *height);

/** Check the given AST sources. */
static void
ast_check_sources(struct Parse *parser, struct ast_source_list *list)
{
	if (list == NULL)
		return;
	struct ast_source *src;
	stailq_foreach_entry(src, &list->head, link) {
		int height;
		ast_check_select_height(parser, src->select, &height);
		struct ast_check_result on;
		ast_check_expr(parser, src->join_on, &on);
		if ((src->join_type & JT_INNER) != 0 &&
		    (src->join_type & JT_OUTER) != 0) {
			diag_set(ClientError, ER_SQL_PARSER_GENERIC,
				 "JOIN cannot be both OUTER and INNER");
			parser->is_aborted = true;
			break;
		}
		if ((src->join_type & JT_OUTER) != 0 &&
		    (src->join_type & (JT_LEFT | JT_RIGHT)) != JT_LEFT) {
			diag_set(ClientError, ER_UNSUPPORTED, "Tarantool",
				 "RIGHT and FULL OUTER JOINs");
			parser->is_aborted = true;
			break;
		}
	}
}

/**
 * Check the given part of an AST SELECT, without its WITH clause, and set the
 * largest height of its expressions.
 */
static void
ast_check_select_part(struct Parse *parser, struct ast_select *select,
		      int *height)
{
	struct ast_check_result expr;
	ast_check_expr_list(parser, select->columns, &expr);
	*height = expr.height;
	ast_check_sources(parser, select->sources);
	ast_check_expr(parser, select->where, &expr);
	*height = MAX(*height, expr.height);
	ast_check_expr_list(parser, select->group_by, &expr);
	*height = MAX(*height, expr.height);
	ast_check_expr(parser, select->having, &expr);
	*height = MAX(*height, expr.height);
	ast_check_expr_list(parser, select->order_by, &expr);
	*height = MAX(*height, expr.height);
	ast_check_expr(parser, select->limit, &expr);
	*height = MAX(*height, expr.height);
	ast_check_expr(parser, select->offset, &expr);
	*height = MAX(*height, expr.height);
}

/** Check the given AST WITH clause. */
static void
ast_check_with(struct Parse *parser, struct ast_with_list *list)
{
	if (list == NULL)
		return;
	struct ast_with_entry *entry;
	stailq_foreach_entry(entry, &list->head, link) {
		if (entry->columns != NULL) {
			struct ast_id_entry *id;
			stailq_foreach_entry(id, &entry->columns->head, link)
				ast_check_name(parser, &id->id);
		}
		int height;
		ast_check_select_height(parser, entry->select, &height);
		size_t used = region_used(&parser->region);
		const char *name = sql_name_temp(&parser->region,
						 entry->name.z, entry->name.n);
		struct ast_with_entry *prev;
		stailq_foreach_entry(prev, &list->head, link) {
			if (prev == entry)
				break;
			const char *prev_name =
				sql_name_temp(&parser->region, prev->name.z,
					      prev->name.n);
			if (strcmp(name, prev_name) != 0)
				continue;
			const char *err = "Ambiguous table name in WITH "
					  "query: %s";
			diag_set(ClientError, ER_SQL_PARSER_GENERIC,
				 tt_sprintf(err, name));
			parser->is_aborted = true;
		}
		region_truncate(&parser->region, used);
	}
}

static void
ast_check_select_height(struct Parse *parser, struct ast_select *select,
			int *height)
{
	*height = 0;
	if (select == NULL)
		return;
	ast_check_with(parser, select->with);
	if (parser->is_aborted)
		return;
	int count = 1;
	struct ast_select *part;
	rlist_foreach_entry(part, &select->link, link) {
		int h;
		ast_check_select_part(parser, part, &h);
		if (parser->is_aborted)
			return;
		*height = MAX(*height, h);
		count++;
	}
	int h;
	ast_check_select_part(parser, select, &h);
	if (parser->is_aborted)
		return;
	*height = MAX(*height, h);
	if ((select->flags & SF_MultiValue) == 0 &&
	    count > SQL_MAX_COMPOUND_SELECT) {
		diag_set(ClientError, ER_SQL_PARSER_LIMIT, "The number of "
			 "UNION or EXCEPT or INTERSECT operations", count,
			 SQL_MAX_COMPOUND_SELECT);
		parser->is_aborted = true;
	}
}

/**
 * Set the given result of an operation with the given operands, whose
 * heights are the given ones, and check its height.
 */
static void
ast_check_node(struct Parse *parser, int op, int left, int right,
	       struct ast_check_result *res)
{
	res->op = op;
	res->literal = NULL;
	res->height = MAX(left, right) + 1;
	sqlExprCheckHeight(parser, res->height);
}

/** Check the given AST INTEGER literal. */
static void
ast_check_integer(struct Parse *parser, struct ast_expr *expr,
		  struct ast_check_result *res)
{
	size_t used = region_used(&parser->region);
	char *str = xregion_alloc(&parser->region, expr->len + 1);
	memcpy(str, expr->str, expr->len);
	str[expr->len] = '\0';
	if (sql_uint_from_str(&res->u, str) != 0)
		parser->is_aborted = true;
	region_truncate(&parser->region, used);
}

/** Check the given AST DECIMAL literal. */
static void
ast_check_decimal(struct Parse *parser, struct ast_expr *expr)
{
	size_t used = region_used(&parser->region);
	char *str = xregion_alloc(&parser->region, expr->len + 1);
	memcpy(str, expr->str, expr->len);
	str[expr->len] = '\0';
	decimal_t dec;
	if (sql_dec_from_str(&dec, str) != 0)
		parser->is_aborted = true;
	region_truncate(&parser->region, used);
}

/** Check the given AST unary minus. */
static void
ast_check_uminus(struct Parse *parser, struct ast_expr *expr,
		 struct ast_check_result *res)
{
	ast_check_expr(parser, expr->left, res);
	if (parser->is_aborted)
		return;
	if (res->op == TK_DECIMAL || res->op == TK_FLOAT) {
		res->is_neg = !res->is_neg;
		return;
	}
	if (res->op != TK_INTEGER) {
		ast_check_node(parser, TK_UMINUS, res->height, 0, res);
		return;
	}
	if (res->is_neg) {
		res->u = ~res->u + 1;
		res->is_neg = false;
		return;
	}
	if (res->u == 0)
		return;
	int64_t value;
	if (sql_neg_uint(&value, res->u) != 0) {
		parser->is_aborted = true;
		return;
	}
	res->u = (uint64_t)value;
	res->is_neg = true;
}

/** Check the given AST function call. */
static void
ast_check_function(struct Parse *parser, struct ast_expr *expr,
		   struct ast_check_result *res)
{
	res->op = TK_FUNCTION;
	res->height = 1;
	if (expr->right == NULL || expr->right->list == NULL)
		return;
	if (expr->right->list->len > SQL_MAX_FUNCTION_ARG) {
		size_t used = region_used(&parser->region);
		char *name = xregion_alloc(&parser->region,
					   expr->left->len + 1);
		memcpy(name, expr->left->str, expr->left->len);
		name[expr->left->len] = '\0';
		sqlDequote(name);
		const char *err = tt_sprintf("Number of arguments to "
					     "function %s", name);
		region_truncate(&parser->region, used);
		diag_set(ClientError, ER_SQL_PARSER_LIMIT, err,
			 expr->right->list->len, SQL_MAX_FUNCTION_ARG);
		parser->is_aborted = true;
		return;
	}
	ast_check_expr_list(parser, expr->right->list, res);
	if (!parser->is_aborted)
		ast_check_node(parser, TK_FUNCTION, res->height, 0, res);
}

/**
 * Check the given AST expression with a left operand and a list of operands
 * of the given operation.
 */
static void
ast_check_left_and_list(struct Parse *parser, int op, struct ast_expr *left,
			struct ast_expr_list *list,
			struct ast_check_result *res)
{
	struct ast_check_result l;
	ast_check_expr(parser, left, &l);
	if (parser->is_aborted)
		return;
	ast_check_node(parser, op, l.height, 0, res);
	ast_check_expr_list(parser, list, res);
	res->is_constant = res->is_constant && l.is_constant;
	if (!parser->is_aborted)
		ast_check_node(parser, op, l.height, res->height, res);
}

/** Check the given AST IN expression. */
static void
ast_check_in(struct Parse *parser, struct ast_expr *expr,
	     struct ast_check_result *res)
{
	struct ast_check_result left;
	if (expr->right->op == TK_SELECT) {
		ast_check_expr(parser, expr->left, &left);
		if (parser->is_aborted)
			return;
		int height;
		ast_check_select_height(parser, expr->right->select, &height);
		if (parser->is_aborted)
			return;
		ast_check_node(parser, expr->op, left.height, 0, res);
		if (!parser->is_aborted)
			ast_check_node(parser, expr->op, left.height, height,
				       res);
		res->is_constant = false;
		return;
	}
	assert(expr->right->op == TK_VECTOR);
	struct ast_expr_list *list = expr->right->list;
	if (list == NULL || list->len == 0) {
		res->op = TK_FALSE;
		res->height = 1;
		res->is_constant = true;
		return;
	}
	if (list->len > 1) {
		ast_check_left_and_list(parser, expr->op, expr->left, list,
					res);
		return;
	}
	ast_check_expr(parser, expr->left, &left);
	if (parser->is_aborted)
		return;
	struct ast_check_result right;
	struct ast_expr_list_entry *entry =
		stailq_first_entry(&list->head, typeof(*entry), link);
	ast_check_expr(parser, entry->expr, &right);
	if (!parser->is_aborted)
		ast_check_node(parser, TK_EQ, left.height, right.height, res);
	res->is_constant = left.is_constant && right.is_constant;
}

/** Check the given AST binary operation. */
static void
ast_check_binary(struct Parse *parser, struct ast_expr *expr,
		 struct ast_check_result *res)
{
	struct ast_check_result left;
	ast_check_expr(parser, expr->left, &left);
	if (parser->is_aborted)
		return;
	struct ast_check_result right;
	ast_check_expr(parser, expr->right, &right);
	if (parser->is_aborted)
		return;
	if (expr->op == TK_AND &&
	    (left.op == TK_FALSE || right.op == TK_FALSE)) {
		res->op = TK_FALSE;
		res->height = 1;
		res->is_constant = true;
		return;
	}
	ast_check_node(parser, expr->op, left.height, right.height, res);
	res->is_constant = left.is_constant && right.is_constant;
}

static void
ast_check_expr(struct Parse *parser, struct ast_expr *expr,
	       struct ast_check_result *res)
{
	memset(res, 0, sizeof(*res));
	res->is_constant = true;
	if (expr == NULL)
		return;
	while (expr->op == TK_PARENTHESES)
		expr = expr->left;
	res->op = expr->op;
	res->height = 1;
	struct ast_check_result list;
	switch (expr->op) {
	case TK_INTEGER:
		res->literal = expr;
		ast_check_integer(parser, expr, res);
		break;
	case TK_DECIMAL:
		res->literal = expr;
		ast_check_decimal(parser, expr);
		break;
	case TK_STRING:
	case TK_BLOB:
	case TK_FLOAT:
		res->literal = expr;
		break;
	case TK_ID:
	case TK_VAR_ANON:
	case TK_VAR_NUM:
	case TK_VAR_NAME:
		res->is_constant = false;
		break;
	case TK_AND:
	case TK_OR:
	case TK_LT:
	case TK_LE:
	case TK_GT:
	case TK_GE:
	case TK_EQ:
	case TK_NE:
	case TK_BITAND:
	case TK_BITOR:
	case TK_LSHIFT:
	case TK_RSHIFT:
	case TK_PLUS:
	case TK_MINUS:
	case TK_STAR:
	case TK_SLASH:
	case TK_REM:
	case TK_CONCAT:
	case TK_DOT:
		ast_check_binary(parser, expr, res);
		break;
	case TK_COLLATE: {
		uint32_t id;
		if (sql_coll_id(&id, expr->right->str, expr->right->len) != 0) {
			parser->is_aborted = true;
			break;
		}
		ast_check_expr(parser, expr->left, res);
		if (!parser->is_aborted)
			ast_check_node(parser, TK_COLLATE, res->height, 0, res);
		break;
	}
	case TK_CAST:
	case TK_NOT:
	case TK_BITNOT:
	case TK_NOTNULL:
	case TK_ISNULL:
		ast_check_expr(parser, expr->left, res);
		if (!parser->is_aborted)
			ast_check_node(parser, expr->op, res->height, 0, res);
		break;
	case TK_UPLUS:
		ast_check_expr(parser, expr->left, res);
		if (parser->is_aborted || res->op == TK_DECIMAL ||
		    res->op == TK_FLOAT || res->op == TK_INTEGER)
			break;
		ast_check_node(parser, TK_UPLUS, res->height, 0, res);
		break;
	case TK_UMINUS:
		ast_check_uminus(parser, expr, res);
		break;
	case TK_ARRAY:
	case TK_MAP:
	case TK_VECTOR:
		ast_check_expr_list(parser, expr->list, &list);
		res->is_constant = list.is_constant;
		if (!parser->is_aborted)
			ast_check_node(parser, expr->op, list.height, 0, res);
		res->len = expr->list != NULL ? expr->list->len : 0;
		break;
	case TK_GETITEM: {
		struct ast_check_result left;
		ast_check_expr(parser, expr->left, &left);
		if (parser->is_aborted)
			break;
		ast_check_expr_list(parser, expr->list, &list);
		res->is_constant = left.is_constant && list.is_constant;
		if (!parser->is_aborted)
			ast_check_node(parser, expr->op, left.height,
				       list.height, res);
		break;
	}
	case TK_FUNCTION:
		ast_check_function(parser, expr, res);
		break;
	case TK_BETWEEN:
		ast_check_left_and_list(parser, expr->op, expr->left,
					expr->list, res);
		break;
	case TK_IN:
		ast_check_in(parser, expr, res);
		break;
	case TK_EXISTS:
	case TK_SELECT: {
		int height;
		ast_check_select_height(parser, expr->select, &height);
		if (!parser->is_aborted)
			ast_check_node(parser, expr->op, height, 0, res);
		res->is_constant = false;
		break;
	}
	case TK_CASE:
		if (expr->left != NULL) {
			ast_check_left_and_list(parser, expr->op, expr->left,
						expr->list, res);
			break;
		}
		ast_check_expr_list(parser, expr->list, &list);
		res->is_constant = list.is_constant;
		if (!parser->is_aborted)
			ast_check_node(parser, expr->op, list.height, 0, res);
		break;
	default:
		break;
	}
}

int
sql_ast_check_expr(struct Parse *parser, struct ast_expr *expr,
		   struct ast_check_result *res)
{
	ast_check_expr(parser, expr, res);
	return parser->is_aborted ? -1 : 0;
}

int
sql_ast_check_index_columns(struct Parse *parser, struct ast_expr_list *list)
{
	struct ast_check_result res;
	ast_check_expr_list(parser, list, &res);
	return parser->is_aborted ? -1 : 0;
}

int
sql_ast_check_select(struct Parse *parser, struct ast_select *select)
{
	int height;
	ast_check_select_height(parser, select, &height);
	return parser->is_aborted ? -1 : 0;
}

/**
 * Check the given AST SET clause of UPDATE: the values, the names of the
 * columns and the number of the values assigned to a list of columns.
 */
static void
ast_check_set_list(struct Parse *parser, struct ast_set_list *list)
{
	struct ast_set_list_entry *entry;
	stailq_foreach_entry(entry, &list->head, link) {
		struct ast_check_result expr;
		ast_check_expr(parser, entry->expr, &expr);
		if (parser->is_aborted)
			break;
		if (entry->ids == NULL) {
			ast_check_name(parser, &entry->name);
			continue;
		}
		if (expr.op == TK_SELECT)
			continue;
		uint32_t len = expr.op == TK_VECTOR ? expr.len : 1;
		if (entry->ids->len == len)
			continue;
		const char *err = tt_sprintf("%d columns assigned %d values",
					     entry->ids->len, len);
		diag_set(ClientError, ER_SQL_PARSER_GENERIC, err);
		parser->is_aborted = true;
	}
}

/** Check the given action of a trigger when the trigger is created. */
static void
sql_trigger_action_check(struct Parse *parser,
			 const struct ast_trigger_action *action)
{
	struct ast_check_result res;
	switch (action->op) {
	case TK_UPDATE:
	case TK_DELETE: {
		const struct Token *indexed_by = action->op == TK_UPDATE ?
						 &action->update->indexed_by :
						 &action->del->indexed_by;
		if (indexed_by->n > 0) {
			diag_set(ClientError, ER_SQL_PARSER_GENERIC,
				 "The INDEXED BY clause is not allowed on "
				 "UPDATE or DELETE statements within triggers");
			parser->is_aborted = true;
			return;
		}
		if (action->op == TK_UPDATE) {
			struct ast_update *update = action->update;
			ast_check_set_list(parser, update->set_list);
			if (!parser->is_aborted)
				sql_ast_check_expr(parser, update->where,
						   &res);
		} else {
			sql_ast_check_expr(parser, action->del->where, &res);
		}
		break;
	}
	case TK_INSERT:
		sql_ast_check_select(parser, action->insert->select);
		break;
	case TK_SELECT:
		sql_ast_check_select(parser, action->select);
		break;
	default:
		unreachable();
	}
}

struct sql_trigger *
sql_trigger_from_ast(struct Parse *parser, struct ast_trigger *def)
{
	parser->disableLookaside++;
	sql_get()->lookaside.bDisable++;
	if (!def->is_for_each_row) {
		diag_set(ClientError, ER_UNSUPPORTED, "Tarantool SQL",
			 "FOR EACH STATEMENT triggers, please supply "
			 "FOR EACH ROW clause");
		parser->is_aborted = true;
		return NULL;
	}
	/*
	 * The WHEN clause and the actions are resolved and coded every time
	 * the trigger is coded, so here they are only checked.
	 */
	struct ast_check_result when;
	if (sql_ast_check_expr(parser, def->when, &when) != 0)
		return NULL;
	parser->initiateTTrans = true;
	struct ast_trigger_action *action;
	stailq_foreach_entry(action, &def->actions->head, link) {
		sql_trigger_action_check(parser, action);
		if (parser->is_aborted)
			return NULL;
	}
	struct IdList *columns = id_list_from_ast(def->columns);
	return sql_trigger_new(parser, &def->name, &def->table, def->time,
			       def->event, columns);
}

struct sql_ast *
sql_ast_new(struct region *region)
{
	struct sql_ast *res = xregion_alloc_object(region, typeof(*res));
	memset(res, 0, sizeof(*res));
	return res;
}
