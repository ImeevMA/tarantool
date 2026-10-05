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
 * stops at the first erroneous expression.
 */
static void
ast_check_expr_list(struct Parse *parser, struct ast_expr_list *list,
		    struct ast_check_result *res)
{
	memset(res, 0, sizeof(*res));
	res->is_constant = true;
	if (list == NULL)
		return;
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

/**
 * Check if the given AST expression is always FALSE: it is FALSE, IN with an
 * empty list, or AND with an always FALSE operand.
 */
static bool
ast_is_false(struct ast_expr *expr)
{
	while (expr->op == TK_PARENTHESES)
		expr = expr->left;
	switch (expr->op) {
	case TK_FALSE:
		return true;
	case TK_IN:
		return expr->right->op == TK_VECTOR &&
		       (expr->right->list == NULL ||
			expr->right->list->len == 0);
	case TK_AND:
		return ast_is_false(expr->left) || ast_is_false(expr->right);
	default:
		return false;
	}
}

/**
 * Collect the names of the spaces the given AST SELECT refers to: for every
 * part of a compound SELECT, starting from the rightmost one, the names of its
 * sources, then the names from the subqueries of its expressions, then the
 * names from its subquery sources. The WITH clauses and the ON clauses are
 * skipped.
 */
static void
ast_view_refs_select(struct sql_view_refs **refs, struct ast_select *select);

/**
 * Collect the space names of the subqueries of the given AST expression. The
 * operands of an always FALSE expression are skipped.
 */
static void
ast_view_refs_expr(struct sql_view_refs **refs, struct ast_expr *expr);

/** Collect the space names of the given AST expression list. */
static void
ast_view_refs_list(struct sql_view_refs **refs, struct ast_expr_list *list)
{
	if (list == NULL)
		return;
	struct ast_expr_list_entry *entry;
	stailq_foreach_entry(entry, &list->head, link)
		ast_view_refs_expr(refs, entry->expr);
}

static void
ast_view_refs_expr(struct sql_view_refs **refs, struct ast_expr *expr)
{
	if (expr == NULL)
		return;
	while (expr->op == TK_PARENTHESES)
		expr = expr->left;
	switch (expr->op) {
	case TK_AND:
		if (ast_is_false(expr))
			return;
		FALLTHROUGH;
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
		ast_view_refs_expr(refs, expr->left);
		ast_view_refs_expr(refs, expr->right);
		break;
	case TK_COLLATE:
	case TK_CAST:
	case TK_NOT:
	case TK_BITNOT:
	case TK_NOTNULL:
	case TK_ISNULL:
	case TK_UPLUS:
	case TK_UMINUS:
		ast_view_refs_expr(refs, expr->left);
		break;
	case TK_ARRAY:
	case TK_MAP:
	case TK_VECTOR:
		ast_view_refs_list(refs, expr->list);
		break;
	case TK_GETITEM:
		ast_view_refs_list(refs, expr->list);
		ast_view_refs_expr(refs, expr->left);
		break;
	case TK_FUNCTION:
		if (expr->right != NULL)
			ast_view_refs_list(refs, expr->right->list);
		break;
	case TK_BETWEEN:
	case TK_CASE:
		ast_view_refs_expr(refs, expr->left);
		ast_view_refs_list(refs, expr->list);
		break;
	case TK_IN:
		if (ast_is_false(expr))
			return;
		ast_view_refs_expr(refs, expr->left);
		if (expr->right->op == TK_SELECT)
			ast_view_refs_select(refs, expr->right->select);
		else
			ast_view_refs_list(refs, expr->right->list);
		break;
	case TK_EXISTS:
	case TK_SELECT:
		ast_view_refs_select(refs, expr->select);
		break;
	default:
		break;
	}
}

/** Add the given space name to the given names if it is not there. */
static void
ast_view_refs_add(struct sql_view_refs **refs, const struct Token *name)
{
	char *str = sql_name_new(name->z, name->n);
	for (uint32_t i = 0; i < (*refs)->count; ++i) {
		if (strcmp((*refs)->names[i], str) == 0) {
			sql_xfree(str);
			return;
		}
	}
	uint32_t count = (*refs)->count + 1;
	*refs = xrealloc(*refs, sizeof(**refs) + count * sizeof(char *));
	(*refs)->names[(*refs)->count++] = str;
}

static void
ast_view_refs_select(struct sql_view_refs **refs, struct ast_select *select)
{
	if (select == NULL)
		return;
	struct ast_select *part = select;
	while (true) {
		struct ast_source *src;
		if (part->sources != NULL) {
			stailq_foreach_entry(src, &part->sources->head, link) {
				if (src->name.n > 0)
					ast_view_refs_add(refs, &src->name);
			}
		}
		ast_view_refs_list(refs, part->columns);
		ast_view_refs_expr(refs, part->where);
		ast_view_refs_list(refs, part->group_by);
		ast_view_refs_expr(refs, part->having);
		ast_view_refs_list(refs, part->order_by);
		ast_view_refs_expr(refs, part->limit);
		ast_view_refs_expr(refs, part->offset);
		if (part->sources != NULL) {
			stailq_foreach_entry(src, &part->sources->head, link)
				ast_view_refs_select(refs, src->select);
		}
		part = rlist_prev_entry(part, link);
		if (part == select)
			break;
	}
}

/**
 * Check if the given name is a prefix of the name of a table of the WITH
 * clause of the given AST SELECT or of the subquery sources of the SELECT,
 * recursively. The WITH clauses of the tables of WITH clauses are skipped.
 */
static bool
ast_has_cte(struct ast_select *select, const char *name)
{
	size_t len = strlen(name);
	if (select->with != NULL) {
		struct ast_with_entry *entry;
		stailq_foreach_entry(entry, &select->with->head, link) {
			char *cte = sql_name_new(entry->name.z, entry->name.n);
			bool is_match = strncmp(name, cte, len) == 0;
			sql_xfree(cte);
			if (is_match)
				return true;
		}
	}
	if (select->sources == NULL)
		return false;
	struct ast_source *src;
	stailq_foreach_entry(src, &select->sources->head, link) {
		if (src->select != NULL && ast_has_cte(src->select, name))
			return true;
	}
	return false;
}

struct sql_view_refs *
sql_ast_view_refs(struct ast_select *select)
{
	struct sql_view_refs *refs = xmalloc(sizeof(*refs));
	refs->count = 0;
	ast_view_refs_select(&refs, select);
	uint32_t count = 0;
	for (uint32_t i = 0; i < refs->count; ++i) {
		if (ast_has_cte(select, refs->names[i]))
			sql_xfree(refs->names[i]);
		else
			refs->names[count++] = refs->names[i];
	}
	refs->count = count;
	return refs;
}

uint32_t
sql_ast_source_count(struct ast_select *select)
{
	uint32_t count = 0;
	if (select->sources == NULL)
		return 0;
	struct ast_source *src;
	stailq_foreach_entry(src, &select->sources->head, link) {
		count++;
		if (src->select != NULL)
			count += sql_ast_source_count(src->select);
	}
	return count;
}
