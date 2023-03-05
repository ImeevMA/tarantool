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

/** Return the name of the last created column. */
static struct Token *
last_column_name(struct Parse *parse)
{
	if (parse->ast.type == SQL_AST_TYPE_ADD_COLUMN)
		return &parse->ast.add_column.column.name;
	assert(parse->ast.type == SQL_AST_TYPE_CREATE_TABLE);
	assert(parse->ast.create_table.column_list.n > 0);
	uint32_t id = parse->ast.create_table.column_list.n - 1;
	return &parse->ast.create_table.column_list.a[id].name;
}

/** Returns the name of the table for which the column is being created. */
static const char *
column_table_name(struct Parse *parse)
{
	if (parse->ast.type == SQL_AST_TYPE_ADD_COLUMN)
		return parse->ast.add_column.src_list->a[0].zName;
	assert(parse->ast.type == SQL_AST_TYPE_CREATE_TABLE);
	return parse->create_table_def.new_space->def->name;
}

void
sql_ast_init_start_transaction(struct Parse *parse)
{
	assert(parse->ast.type == SQL_AST_TYPE_UNKNOWN);
	parse->ast.type = SQL_AST_TYPE_START_TRANSACTION;
}

void
sql_ast_init_commit(struct Parse *parse)
{
	assert(parse->ast.type == SQL_AST_TYPE_UNKNOWN);
	parse->ast.type = SQL_AST_TYPE_COMMIT;
}

void
sql_ast_init_rollback(struct Parse *parse)
{
	assert(parse->ast.type == SQL_AST_TYPE_UNKNOWN);
	parse->ast.type = SQL_AST_TYPE_ROLLBACK;
}

void
sql_ast_init_savepoint(struct Parse *parse, const struct Token *name)
{
	assert(parse->ast.type == SQL_AST_TYPE_UNKNOWN);
	parse->ast.type = SQL_AST_TYPE_SAVEPOINT;
	parse->ast.savepoint.name = *name;
}

void
sql_ast_init_release_savepoint(struct Parse *parse, const struct Token *name)
{
	assert(parse->ast.type == SQL_AST_TYPE_UNKNOWN);
	parse->ast.type = SQL_AST_TYPE_RELEASE_SAVEPOINT;
	parse->ast.savepoint.name = *name;
}

void
sql_ast_init_rollback_to_savepoint(struct Parse *parse,
				   const struct Token *name)
{
	assert(parse->ast.type == SQL_AST_TYPE_UNKNOWN);
	parse->ast.type = SQL_AST_TYPE_ROLLBACK_TO_SAVEPOINT;
	parse->ast.savepoint.name = *name;
}

void
sql_ast_init_table_rename(struct Parse *parse, const struct Token *old_name,
			  const struct Token *new_name)
{
	assert(parse->ast.type == SQL_AST_TYPE_UNKNOWN);
	parse->ast.type = SQL_AST_TYPE_RENAME;
	parse->ast.rename.old_name = *old_name;
	parse->ast.rename.new_name = *new_name;
}

void
sql_ast_init_constraint_drop(struct Parse *parse,
			     const struct Token *table_name,
			     const struct Token *name)
{
	assert(parse->ast.type == SQL_AST_TYPE_UNKNOWN);
	parse->ast.type = SQL_AST_TYPE_DROP_CONSTRAINT;
	parse->ast.drop_constraint.table_name = *table_name;
	parse->ast.drop_constraint.name = *name;
}

void
sql_ast_init_index_drop(struct Parse *parse, const struct Token *table_name,
			const struct Token *index_name, bool if_exists)
{
	assert(parse->ast.type == SQL_AST_TYPE_UNKNOWN);
	parse->ast.type = SQL_AST_TYPE_DROP_INDEX;
	parse->ast.drop_index.table_name = *table_name;
	parse->ast.drop_index.index_name = *index_name;
	parse->ast.drop_index.if_exists = if_exists;
}

void
sql_ast_init_trigger_drop(struct Parse *parse, const struct Token *name,
			  bool if_exists)
{
	assert(parse->ast.type == SQL_AST_TYPE_UNKNOWN);
	parse->ast.type = SQL_AST_TYPE_DROP_TRIGGER;
	parse->ast.drop_trigger.name = *name;
	parse->ast.drop_trigger.if_exists = if_exists;
}

void
sql_ast_init_view_drop(struct Parse *parse, const struct Token *name,
		       bool if_exists)
{
	assert(parse->ast.type == SQL_AST_TYPE_UNKNOWN);
	parse->ast.type = SQL_AST_TYPE_DROP_VIEW;
	parse->ast.drop_view.name = *name;
	parse->ast.drop_view.if_exists = if_exists;
}

void
sql_ast_init_table_drop(struct Parse *parse, const struct Token *name,
			bool if_exists)
{
	assert(parse->ast.type == SQL_AST_TYPE_UNKNOWN);
	parse->ast.type = SQL_AST_TYPE_DROP_TABLE;
	parse->ast.drop_table.name = *name;
	parse->ast.drop_table.if_exists = if_exists;
}

void
sql_ast_init_create_table(struct Parse *parse)
{
	parse->ast.type = SQL_AST_TYPE_CREATE_TABLE;
}

void
sql_ast_init_create_index(struct Parse *parse, struct Token *table_name,
			  const struct Token *index_name, struct ExprList *cols,
			  bool is_unique, bool if_not_exists)
{
	parse->ast.type = SQL_AST_TYPE_CREATE_INDEX;
	struct sql_ast_create_index *stmt = &parse->ast.create_index;
	stmt->src_list = sql_src_list_append(NULL, table_name);
	stmt->name = *index_name;
	stmt->cols = cols;
	stmt->is_unique = is_unique;
	stmt->if_not_exists = if_not_exists;
}

void
sql_ast_init_add_column(struct Parse *parse, struct SrcList *table_name,
			struct Token *name, enum field_type type)
{
	parse->ast.type = SQL_AST_TYPE_ADD_COLUMN;
	struct sql_ast_add_column *stmt = &parse->ast.add_column;
	stmt->src_list = table_name;
	stmt->column.name = *name;
	stmt->column.type = type;
}

void
sql_ast_init_add_foreign_key(struct Parse *parse, struct SrcList *table_name,
			     const struct Token *name,
			     struct ExprList *child_cols,
			     const struct Token *parent_name,
			     struct ExprList *parent_cols)
{
	parse->ast.type = SQL_AST_TYPE_ADD_FOREIGN_KEY;
	parse->ast.add_foreign_key.src_list = table_name;
	struct sql_ast_foreign_key *c = &parse->ast.add_foreign_key.foreign_key;
	c->name = *name;
	c->child_cols = child_cols;
	c->parent_cols = parent_cols;
	c->parent_name = *parent_name;
	c->is_column_constraint = false;
}

void
sql_ast_init_add_check(struct Parse *parse, struct SrcList *table_name,
		       const struct Token *name, struct ExprSpan *expr)
{
	parse->ast.type = SQL_AST_TYPE_ADD_CHECK;
	parse->ast.add_check.src_list = table_name;
	struct sql_ast_check *c = &parse->ast.add_check.check;
	c->name = *name;
	c->expr = *expr;
	c->column_name = Token_nil;
}

void
sql_ast_init_add_unique(struct Parse *parse, struct SrcList *table_name,
			const struct Token *name, struct ExprList *cols)
{
	parse->ast.type = SQL_AST_TYPE_ADD_UNIQUE;
	parse->ast.add_unique.src_list = table_name;
	struct sql_ast_unique *c = &parse->ast.add_unique.unique;
	c->name = *name;
	c->cols = cols;
}

void
sql_ast_init_add_primary_key(struct Parse *parse, struct SrcList *table_name,
			     const struct Token *name, struct ExprList *cols)
{
	parse->ast.type = SQL_AST_TYPE_ADD_PRIMARY_KEY;
	parse->ast.add_primary_key.src_list = table_name;
	parse->ast.add_primary_key.primary_key.cols = cols;
	parse->ast.add_primary_key.primary_key.name = *name;
}

/** Append a new FOREIGN KEY to FOREIGN KEY list. */
static void
foreign_key_list_append(struct sql_ast_foreign_key_list *list,
			const struct Token *name, struct ExprList *child_cols,
			const struct Token *parent_name,
			struct ExprList *parent_cols, bool is_column_constraint)
{
	uint32_t id = list->n;
	++list->n;
	uint32_t size = list->n * sizeof(*list->a);
	list->a = sql_xrealloc(list->a, size);
	struct sql_ast_foreign_key *c = &list->a[id];
	c->name = *name;
	c->child_cols = child_cols;
	c->parent_cols = parent_cols;
	c->parent_name = *parent_name;
	c->is_column_constraint = is_column_constraint;
}

void
sql_ast_save_column_foreign_key(struct Parse *parse, const struct Token *name,
				const struct Token *parent_name,
				struct ExprList *parent_cols)
{
	assert(parse->ast.type == SQL_AST_TYPE_CREATE_TABLE ||
	       parse->ast.type == SQL_AST_TYPE_ADD_COLUMN);
	struct sql_ast_foreign_key_list *list;
	if (parse->ast.type == SQL_AST_TYPE_CREATE_TABLE)
		list = &parse->ast.create_table.foreign_key_list;
	else
		list = &parse->ast.add_column.foreign_key_list;
	struct ExprList *child_cols = sql_expr_list_append(NULL, NULL);
	sqlExprListSetName(parse, child_cols, last_column_name(parse), 1);
	foreign_key_list_append(list, name, child_cols, parent_name,
				parent_cols, true);
}

void
sql_ast_save_table_foreign_key(struct Parse *parse, const struct Token *name,
			       struct ExprList *child_cols,
			       const struct Token *parent_name,
			       struct ExprList *parent_cols)
{
	assert(parse->ast.type == SQL_AST_TYPE_CREATE_TABLE);
	foreign_key_list_append(&parse->ast.create_table.foreign_key_list, name,
				child_cols, parent_name, parent_cols, false);
}

/** Append a new CHECK to CHECK list. */
static void
check_list_append(struct sql_ast_check_list *list, const struct Token *name,
		  struct ExprSpan *expr, const struct Token *column_name)
{
	uint32_t id = list->n;
	++list->n;
	uint32_t size = list->n * sizeof(*list->a);
	list->a = sql_xrealloc(list->a, size);
	struct sql_ast_check *c = &list->a[id];
	c->name = *name;
	c->expr = *expr;
	c->column_name = *column_name;
}

void
sql_ast_save_column_check(struct Parse *parse, const struct Token *name,
			  struct ExprSpan *expr)
{
	assert(parse->ast.type == SQL_AST_TYPE_CREATE_TABLE ||
	       parse->ast.type == SQL_AST_TYPE_ADD_COLUMN);
	struct sql_ast_check_list *list;
	if (parse->ast.type == SQL_AST_TYPE_CREATE_TABLE)
		list = &parse->ast.create_table.check_list;
	else
		list = &parse->ast.add_column.check_list;
	check_list_append(list, name, expr, last_column_name(parse));
}

void
sql_ast_save_table_check(struct Parse *parse, const struct Token *name,
			 struct ExprSpan *expr)
{
	assert(parse->ast.type == SQL_AST_TYPE_CREATE_TABLE);
	check_list_append(&parse->ast.create_table.check_list, name, expr,
			  &Token_nil);
}

/** Append a new UNIQUE to UNIQUE list. */
static void
unique_list_append(struct sql_ast_unique_list *list, const struct Token *name,
		   struct ExprList *cols)
{
	uint32_t id = list->n;
	++list->n;
	uint32_t size = list->n * sizeof(*list->a);
	list->a = sql_xrealloc(list->a, size);
	struct sql_ast_unique *c = &list->a[id];
	c->name = *name;
	c->cols = cols;
}

void
sql_ast_save_column_unique(struct Parse *parse, const struct Token *name)
{
	assert(parse->ast.type == SQL_AST_TYPE_CREATE_TABLE ||
	       parse->ast.type == SQL_AST_TYPE_ADD_COLUMN);
	struct sql_ast_unique_list *list;
	if (parse->ast.type == SQL_AST_TYPE_CREATE_TABLE)
		list = &parse->ast.create_table.unique_list;
	else
		list = &parse->ast.add_column.unique_list;
	struct Token *column_name = last_column_name(parse);
	struct Expr *expr = sql_expr_new_dequoted(TK_ID, column_name);
	struct ExprList *cols = sql_expr_list_append(NULL, expr);
	unique_list_append(list, name, cols);
}

void
sql_ast_save_table_unique(struct Parse *parse, const struct Token *name,
			  struct ExprList *cols)
{
	assert(parse->ast.type == SQL_AST_TYPE_CREATE_TABLE);
	unique_list_append(&parse->ast.create_table.unique_list, name, cols);
}

/** Fill in the description of the PRIMARY KEY. */
static void
primary_key_fill(struct Parse *parse, struct sql_ast_unique *pk,
		 const struct Token *name, struct ExprList *cols)
{
	assert(parse->ast.type == SQL_AST_TYPE_CREATE_TABLE ||
	       parse->ast.type == SQL_AST_TYPE_ADD_COLUMN);
	if (pk->cols != NULL) {
		diag_set(ClientError, ER_CREATE_SPACE, column_table_name(parse),
			 "primary key has been already declared");
		parse->is_aborted = true;
		sql_expr_list_delete(cols);
		sql_expr_list_delete(pk->cols);
		return;
	}
	pk->cols = cols;
	pk->name = *name;
}

void
sql_ast_save_column_primary_key(struct Parse *parse, const struct Token *name,
				enum sort_order sort_order)
{
	assert(parse->ast.type == SQL_AST_TYPE_CREATE_TABLE ||
	       parse->ast.type == SQL_AST_TYPE_ADD_COLUMN);
	struct sql_ast_unique *pk;
	if (parse->ast.type == SQL_AST_TYPE_CREATE_TABLE)
		pk = &parse->ast.create_table.primary_key;
	else
		pk = &parse->ast.add_column.primary_key;
	struct Token *column_name = last_column_name(parse);
	struct Expr *expr = sql_expr_new_dequoted(TK_ID, column_name);
	struct ExprList *cols = sql_expr_list_append(NULL, expr);
	sqlExprListSetSortOrder(cols, sort_order);
	primary_key_fill(parse, pk, name, cols);
}

void
sql_ast_save_table_primary_key(struct Parse *parse, const struct Token *name,
			       struct ExprList *cols)
{
	assert(parse->ast.type == SQL_AST_TYPE_CREATE_TABLE);
	struct sql_ast_unique *pk = &parse->ast.create_table.primary_key;
	primary_key_fill(parse, pk, name, cols);
}

void
sql_ast_save_table_column(struct Parse *parse, struct Token *name,
			  enum field_type type)
{
	struct sql_ast_column_list *list = &parse->ast.create_table.column_list;
	uint32_t id = list->n;
	++list->n;
	list->a = sql_xrealloc(list->a, list->n * sizeof(*list->a));
	struct sql_ast_column *c = &list->a[id];
	memset(c, 0, sizeof(*c));
	c->name = *name;
	c->type = type;
}

/** Set the AUTOINCREMENT column name. */
static void
autoincrement_add(struct Parse *parse, struct Expr *column_name)
{
	if (parse->ast.type == SQL_AST_TYPE_ADD_COLUMN) {
		parse->ast.add_column.autoinc_name = column_name;
		return;
	}
	if (parse->ast.create_table.autoinc_name != NULL) {
		diag_set(ClientError, ER_SQL_SYNTAX_WITH_POS, parse->line_count,
			 parse->line_pos,
			 "table must feature at most one AUTOINCREMENT field");
		parse->is_aborted = true;
		return;
	}
	parse->ast.create_table.autoinc_name = column_name;
}

void
sql_ast_save_column_autoincrement(struct Parse *parse)
{
	assert(parse->ast.type == SQL_AST_TYPE_CREATE_TABLE ||
	       parse->ast.type == SQL_AST_TYPE_ADD_COLUMN);
	struct Token *column_name = last_column_name(parse);
	autoincrement_add(parse, sql_expr_new_dequoted(TK_ID, column_name));
}

void
sql_ast_save_table_autoincrement(struct Parse *parse, struct Expr *column_name)
{
	assert(parse->ast.type == SQL_AST_TYPE_CREATE_TABLE);
	autoincrement_add(parse, column_name);
}

/**
 * Returns the created column for ALTER TABLE ADD COLUMN and the last created
 * column for CREATE TABLE.
 */
static struct sql_ast_column *
last_column(struct Parse *parse)
{
	if (parse->ast.type == SQL_AST_TYPE_ADD_COLUMN)
		return &parse->ast.add_column.column;
	assert(parse->ast.type == SQL_AST_TYPE_CREATE_TABLE);
	assert(parse->ast.create_table.column_list.n > 0);
	uint32_t id = parse->ast.create_table.column_list.n - 1;
	return &parse->ast.create_table.column_list.a[id];
}

void
sql_ast_save_column_collate(struct Parse *parse, struct Token *collate_name)
{
	last_column(parse)->collate_name = *collate_name;
}

void
sql_ast_save_column_default(struct Parse *parse, struct ExprSpan *expr)
{
	last_column(parse)->default_expr = *expr;
}

void
sql_ast_save_column_null_action(struct Parse *parse,
				enum on_conflict_action null_action,
				enum on_conflict_action on_conflict)
{
	struct sql_ast_column *c = last_column(parse);
	const char *action_str = NULL;
	if (c->is_null_action_set && c->null_action != null_action) {
		action_str = on_conflict_action_strs[c->null_action];
	} else if ((on_conflict != ON_CONFLICT_ACTION_ABORT ||
		    null_action != ON_CONFLICT_ACTION_NONE) &&
		   null_action != on_conflict) {
		action_str = on_conflict_action_strs[ON_CONFLICT_ACTION_NONE];
	} else {
		c->null_action = null_action;
		c->is_null_action_set = true;
		return;
	}
	const char *fmt = "NULL declaration for column '%s' of table '%s' has "
			  "been already set to '%s'";
	const char *table_name = column_table_name(parse);
	char *column_name = sql_name_from_token(&c->name);
	const char *err = tt_sprintf(fmt, column_name, table_name, action_str);
	diag_set(ClientError, ER_SQL_EXECUTE, err);
	parse->is_aborted = true;
	sql_xfree(column_name);
}
