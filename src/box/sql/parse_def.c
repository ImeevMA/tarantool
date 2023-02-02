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
	rlist_create(&stmt->columns);
	rlist_create(&stmt->constraints);
}

void
sql_parse_table_engine(struct Parse *parse, struct Token *name)
{
	assert(parse->create_table.engine_name.n == 0);
	parse->create_table.engine_name = *name;
}

void
sql_parse_table_pk(struct Parse *parse, struct Token *name,
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
	struct sql_parse_constraint *c = sql_xmalloc0(sizeof(*c));
	c->name = *name;
	c->type = SQL_CONSTRAINT_UNIQ;
	c->uniq.cols = cols;
	rlist_add_entry(&parse->create_table.constraints, c, link);
}

void
sql_parse_table_ck(struct Parse *parse, struct Token *name,
		   struct ExprSpan *expr)
{
	struct sql_parse_constraint *c = sql_xmalloc0(sizeof(*c));
	c->name = *name;
	c->type = SQL_CONSTRAINT_CK;
	c->ck.expr = *expr;
	rlist_add_entry(&parse->create_table.constraints, c, link);
}

void
sql_parse_table_fk(struct Parse *parse, struct Token *name,
		   struct ExprList *child_cols, struct Token *parent_name,
		   struct ExprList *parent_cols)
{
	struct sql_parse_constraint *c = sql_xmalloc0(sizeof(*c));
	c->name = *name;
	c->type = SQL_CONSTRAINT_FK;
	c->fk.child_cols = child_cols;
	c->fk.parent_cols = parent_cols;
	c->fk.parent_name = *parent_name;
	rlist_add_entry(&parse->create_table.constraints, c, link);
}

void
sql_parse_column_create(struct sql_parse_column *column, struct Token *name,
			enum field_type type)
{
	rlist_create(&column->constraints);
	column->name = *name;
	column->type = type;
}

void
sql_parse_column_add(struct Parse *parse, struct SrcList *table_name,
		     struct Token *name, enum field_type type)
{
	parse->type = PARSE_TYPE_ADD_COLUMN;
	parse->add_column.table_name = table_name;
	sql_parse_column_create(&parse->add_column.column, name, type);
}

void
sql_parse_column_new(struct Parse *parse, struct Token *name,
		     enum field_type type)
{
	struct sql_parse_column *column = sql_xmalloc0(sizeof(*column));
	sql_parse_column_create(column, name, type);
	rlist_add_entry(&parse->create_table.columns, column, link);
	++parse->create_table.column_count;
}

static inline struct sql_parse_column *
sql_parse_last_column(struct Parse *parse)
{
	if (parse->type == PARSE_TYPE_CREATE_TABLE) {
		struct rlist *list = &parse->create_table.columns;
		assert(!rlist_empty(list));
		return rlist_first_entry(list, struct sql_parse_column, link);
	}
	assert(parse->type == PARSE_TYPE_ADD_COLUMN);
	return &parse->add_column.column;
}

void
sql_parse_column_autoinc(struct Parse *parse)
{
	if (parse->create_table.autoinc_col_name.n == 0) {
		struct sql_parse_column *column = sql_parse_last_column(parse);
		parse->create_table.autoinc_col_name = column->name;
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
	/* TODO: Check for column->default_expr.n == 0. */
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
	/* TODO: Check for column->coll_name.n == 0. */
	column->coll_name = *coll_name;
}

void
sql_parse_column_fk(struct Parse *parse, struct Token *name,
		    struct Token *parent_name, struct ExprList *parent_cols)
{
	struct sql_parse_column *column = sql_parse_last_column(parse);
	struct sql_parse_constraint *c = sql_xmalloc0(sizeof(*c));
	c->name = *name;
	c->type = SQL_CONSTRAINT_FK;
	c->fk.parent_cols = parent_cols;
	c->fk.parent_name = *parent_name;
	assert(c->fk.child_cols == NULL);
	rlist_add_entry(&column->constraints, c, link);
}

void
sql_parse_column_ck(struct Parse *parse, struct Token *name,
		    struct ExprSpan *expr)
{
	struct sql_parse_column *column = sql_parse_last_column(parse);
	struct sql_parse_constraint *c = sql_xmalloc0(sizeof(*c));
	c->name = *name;
	c->type = SQL_CONSTRAINT_CK;
	c->ck.expr = *expr;
	rlist_add_entry(&column->constraints, c, link);
}

void
sql_parse_column_unique(struct Parse *parse, struct Token *name)
{
	struct sql_parse_column *column = sql_parse_last_column(parse);
	if (name->n == 0) {
		column->is_unique = true;
		return;
	}
	struct sql_parse_constraint *c = sql_xmalloc0(sizeof(*c));
	c->name = *name;
	c->type = SQL_CONSTRAINT_UNIQ;
	rlist_add_entry(&column->constraints, c, link);
}

void
sql_parse_column_pk(struct Parse *parse, struct Token *name)
{
	if (parse->type == PARSE_TYPE_ADD_COLUMN) {
		parse->add_column.pk_name = *name;
		parse->add_column.is_pk = true;
		return;
	}
	assert(parse->type == PARSE_TYPE_CREATE_TABLE);
	if (parse->create_table.pk_columns == NULL) {
		struct sql_parse_column *column = sql_parse_last_column(parse);
		struct Expr *expr = sql_expr_new(TK_ID, &column->name);
		struct ExprList *cols = sql_expr_list_append(NULL, expr);
		parse->create_table.pk_columns = cols;
		parse->create_table.pk_name = *name;
		return;
	}
	diag_set(ClientError, ER_SQL_SYNTAX_WITH_POS, parse->line_count,
		 parse->line_pos, "primary key has been already declared");
	parse->is_aborted = true;
}

void
sql_parse_column_null(struct Parse *parse, int action, int on_conflict)
{
	struct sql_parse_column *column = sql_parse_last_column(parse);
	if (column->is_nullable_set && (int)column->nullable_action != action)
		goto err;
	/*
	 * The following IF is needed to reproduce the strange behavior of the
	 * NULL constraint with ON CONFLICT ABORT.
	 */
	if (action == ON_CONFLICT_ACTION_NONE &&
	    on_conflict != ON_CONFLICT_ACTION_ABORT)
		goto err;
	column->is_nullable_set = true;
	column->nullable_action = action;
	return;
err:
	diag_set(ClientError, ER_SQL_SYNTAX_WITH_POS, parse->line_count,
		 parse->line_pos, "Another NULL declaration has already been "
		 "set.");
	parse->is_aborted = true;
}
