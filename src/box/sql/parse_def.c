/*
 * Copyright 2010-2019, Tarantool AUTHORS, please see AUTHORS file.
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
#include <string.h>

#include "sqlInt.h"

const struct Token sqlIntTokens[] = {
	{"0", 1, false},
	{"1", 1, false},
	{"2", 1, false},
	{"3", 1, false},
};

void
sqlTokenInit(struct Token *p, char *z)
{
	p->z = z;
	p->n = z == NULL ? 0 : strlen(z);
}

void
sql_parse_table_create(struct Parse *parse, struct Token *name,
		       bool if_not_exist)
{
	parse->type = PARSE_TYPE_CREATE_TABLE;
	struct sql_parse_table *stmt = &parse->create_table;
	stmt->if_not_exist = if_not_exist;
	stmt->name = *name;
}

void
sql_parse_table_engine(struct Parse *parse, struct Token *name)
{
	assert(parse->create_table.engine_name.n == 0);
	parse->create_table.engine_name = *name;
}

void
sql_parse_table_primary_key(struct Parse *parse, struct Token *name,
		   struct ExprList *cols)
{
	assert(parse->type == PARSE_TYPE_CREATE_TABLE);
	if (parse->create_table.pk_columns == NULL) {
		parse->create_table.pk_columns = cols;
		parse->create_table.pk_name = *name;
		return;
	}
	diag_set(ClientError, ER_SQL_SYNTAX_WITH_POS, parse->line_count,
		 parse->line_pos, "primary key has been already declared");
	parse->is_aborted = true;
}

void
sql_parse_table_unique(struct Parse *parse, struct Token *name,
		       struct ExprList *cols)
{
	assert(parse->type == PARSE_TYPE_CREATE_TABLE);
	struct sql_parse_table *stmt = &parse->create_table;
	uint32_t id = stmt->unique_count;
	++stmt->unique_count;
	uint32_t size = stmt->unique_count * sizeof(*stmt->unique);
	stmt->unique = sql_xrealloc(stmt->unique, size);
	struct sql_parse_unique *c = &stmt->unique[id];
	c->name = *name;
	c->cols = cols;
}

void
sql_parse_table_check(struct Parse *parse, struct Token *name,
		      struct ExprSpan *expr)
{
	assert(parse->type == PARSE_TYPE_CREATE_TABLE);
	struct sql_parse_table *stmt = &parse->create_table;
	uint32_t id = stmt->check_count;
	++stmt->check_count;
	uint32_t size = stmt->check_count * sizeof(*stmt->check);
	stmt->check = sql_xrealloc(stmt->check, size);
	struct sql_parse_check *c = &stmt->check[id];
	c->name = *name;
	c->expr = expr;
	c->is_column_constraint = false;
}

void
sql_parse_table_foreign_key(struct Parse *parse, struct Token *name,
			    struct ExprList *child_cols,
			    struct Token *parent_name,
			    struct ExprList *parent_cols)
{
	assert(parse->type == PARSE_TYPE_CREATE_TABLE);
	struct sql_parse_table *stmt = &parse->create_table;
	uint32_t id = stmt->foreign_key_count;
	++stmt->foreign_key_count;
	uint32_t size = stmt->foreign_key_count * sizeof(*stmt->foreign_key);
	stmt->foreign_key = sql_xrealloc(stmt->foreign_key, size);
	struct sql_parse_foreign_key *c = &stmt->foreign_key[id];
	c->name = *name;
	c->child_cols = child_cols;
	c->parent_cols = parent_cols;
	c->parent_name = *parent_name;
	c->is_column_constraint = false;
}

void
sql_parse_table_autoinc(struct Parse *parse, struct Expr *expr)
{
	assert(parse->type == PARSE_TYPE_CREATE_TABLE);
	if (parse->create_table.autoinc_col_name == NULL) {
		parse->create_table.autoinc_col_name = expr;
		return;
	}
	diag_set(ClientError, ER_SQL_SYNTAX_WITH_POS, parse->line_count,
		 parse->line_pos,
		 "table must feature at most one AUTOINCREMENT field");
	parse->is_aborted = true;
}

void
sql_parse_column_add(struct Parse *parse, struct SrcList *table_name,
		     struct Token *name, enum field_type type)
{
	parse->type = PARSE_TYPE_ADD_COLUMN;
	parse->add_column.table_name = table_name;
	parse->add_column.column.name = *name;
	parse->add_column.column.type = type;
}

void
sql_parse_column_new(struct Parse *parse, struct Token *name,
		     enum field_type type)
{
	assert(parse->type == PARSE_TYPE_CREATE_TABLE);
	struct sql_parse_table *stmt = &parse->create_table;
	uint32_t id = stmt->column_count;
	++stmt->column_count;
	uint32_t size = stmt->column_count * sizeof(*stmt->columns);
	stmt->columns = sql_xrealloc(stmt->columns, size);
	struct sql_parse_column *column = &stmt->columns[id];
	memset(column, 0, sizeof(*column));
	column->name = *name;
	column->type = type;
}

static struct sql_parse_column *
sql_parse_last_column(struct Parse *parse)
{
	if (parse->type == PARSE_TYPE_CREATE_TABLE) {
		uint32_t id = parse->create_table.column_count - 1;
		return &parse->create_table.columns[id];
	}
	assert(parse->type == PARSE_TYPE_ADD_COLUMN);
	return &parse->add_column.column;
}

void
sql_parse_column_autoinc(struct Parse *parse)
{
	if (parse->type == PARSE_TYPE_ADD_COLUMN) {
		assert(parse->add_column.is_autoinc == false);
		parse->add_column.is_autoinc = true;
		return;
	}
	assert(parse->type == PARSE_TYPE_CREATE_TABLE);
	if (parse->create_table.autoinc_col_name == NULL) {
		struct sql_parse_column *column = sql_parse_last_column(parse);
		struct Expr *expr = sql_expr_new_dequoted(TK_ID, &column->name);
		parse->create_table.autoinc_col_name = expr;
		return;
	}
	diag_set(ClientError, ER_SQL_SYNTAX_WITH_POS, parse->line_count,
		 parse->line_pos,
		 "table must feature at most one AUTOINCREMENT field");
	parse->is_aborted = true;
}

void
sql_parse_column_default(struct Parse *parse, struct ExprSpan *expr)
{
	struct sql_parse_column *column = sql_parse_last_column(parse);
	if (sqlExprIsConstantOrFunction(expr->pExpr, sql_get()->init.busy)) {
		column->default_expr.z = expr->zStart;
		column->default_expr.n = expr->zEnd - expr->zStart;
		assert(!column->default_expr.isReserved);
		return;
	}
	diag_set(ClientError, ER_SQL_SYNTAX_WITH_POS, parse->line_count,
		 parse->line_pos, "default value is not constant");
	parse->is_aborted = true;
}

void
sql_parse_column_collation(struct Parse *parse, struct Token *coll_name)
{
	struct sql_parse_column *column = sql_parse_last_column(parse);
	column->coll_name = *coll_name;
}

void
sql_parse_column_foreign_key(struct Parse *parse, struct Token *name,
			     struct Token *parent_name,
			     struct ExprList *parent_cols)
{
	struct sql_parse_column *column = sql_parse_last_column(parse);
	struct Expr *expr = sql_expr_new_dequoted(TK_ID, &column->name);
	struct ExprList *child_cols = sql_expr_list_append(NULL, expr);
	if (parse->type == PARSE_TYPE_CREATE_TABLE) {
		return sql_parse_table_foreign_key(parse, name, child_cols,
						   parent_name, parent_cols);
	}

	assert(parse->type == PARSE_TYPE_ADD_COLUMN);
	struct sql_parse_add_column *stmt = &parse->add_column;
	uint32_t id = stmt->foreign_key_count;
	++stmt->foreign_key_count;
	uint32_t size = stmt->foreign_key_count * sizeof(*stmt->foreign_key);
	stmt->foreign_key = sql_xrealloc(stmt->foreign_key, size);
	struct sql_parse_foreign_key *c = &stmt->foreign_key[id];
	c->name = *name;
	c->child_cols = child_cols;
	c->parent_cols = parent_cols;
	c->parent_name = *parent_name;
	c->is_column_constraint = false;
}

void
sql_parse_column_check(struct Parse *parse, struct Token *name,
		       struct ExprSpan *expr)
{
	struct sql_parse_check *c;
	if (parse->type == PARSE_TYPE_CREATE_TABLE) {
		struct sql_parse_table *stmt = &parse->create_table;
		uint32_t id = stmt->check_count;
		++stmt->check_count;
		uint32_t size = stmt->check_count * sizeof(*stmt->check);
		stmt->check = sql_xrealloc(stmt->check, size);
		c = &stmt->check[id];
	} else {
		struct sql_parse_add_column *stmt = &parse->add_column;
		uint32_t id = stmt->check_count;
		++stmt->check_count;
		uint32_t size = stmt->check_count * sizeof(*stmt->check);
		stmt->check = sql_xrealloc(stmt->check, size);
		c = &stmt->check[id];
	}
	c->name = *name;
	c->expr = expr;
	c->is_column_constraint = true;
}

void
sql_parse_column_unique(struct Parse *parse, struct Token *name)
{
	struct sql_parse_unique *c;
	if (parse->type == PARSE_TYPE_CREATE_TABLE) {
		struct sql_parse_table *stmt = &parse->create_table;
		uint32_t id = stmt->unique_count;
		++stmt->unique_count;
		uint32_t size = stmt->unique_count * sizeof(*stmt->unique);
		stmt->unique = sql_xrealloc(stmt->unique, size);
		c = &stmt->unique[id];
	} else {
		struct sql_parse_add_column *stmt = &parse->add_column;
		uint32_t id = stmt->unique_count;
		++stmt->unique_count;
		uint32_t size = stmt->unique_count * sizeof(*stmt->unique);
		stmt->unique = sql_xrealloc(stmt->unique, size);
		c = &stmt->unique[id];
	}
	struct sql_parse_column *column = sql_parse_last_column(parse);
	struct Expr *expr = sql_expr_new_dequoted(TK_ID, &column->name);
	struct ExprList *cols = sql_expr_list_append(NULL, expr);
	c->name = *name;
	c->cols = cols;
}

void
sql_parse_column_primary_key(struct Parse *parse, struct Token *name,
			     int sort_order)
{
	if (parse->type == PARSE_TYPE_ADD_COLUMN) {
		parse->add_column.pk_name = *name;
		parse->add_column.is_pk = true;
		parse->add_column.pk_sort_order = sort_order;
		return;
	}
	assert(parse->type == PARSE_TYPE_CREATE_TABLE);
	if (parse->create_table.pk_columns == NULL) {
		struct sql_parse_column *column = sql_parse_last_column(parse);
		struct Expr *expr = sql_expr_new(TK_ID, &column->name);
		struct ExprList *cols = sql_expr_list_append(NULL, expr);
		sqlExprListSetSortOrder(cols, sort_order);
		parse->create_table.pk_columns = cols;
		parse->create_table.pk_name = *name;
		return;
	}
	diag_set(ClientError, ER_SQL_SYNTAX_WITH_POS, parse->line_count,
		 parse->line_pos, "primary key has been already declared");
	parse->is_aborted = true;
}

void
sql_parse_column_nullable_action(struct Parse *parse,
				 enum parse_nullable_action action,
				 enum parse_nullable_action on_conflict)
{
	struct sql_parse_column *column = sql_parse_last_column(parse);
	if (column->nullable_action != PARSE_NULLABLE_ACTION_UNKNOWN &&
	    column->nullable_action != action)
		goto err;
	if (action == PARSE_NULLABLE_ACTION_NONE &&
	    on_conflict != PARSE_NULLABLE_ACTION_ABORT)
		goto err;
	column->nullable_action = action;
	return;
err:
	diag_set(ClientError, ER_SQL_SYNTAX_WITH_POS, parse->line_count,
		 parse->line_pos, "Another NULL declaration has already been "
		 "set.");
	parse->is_aborted = true;
}
