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

#include "parse_def.h"
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
sql_parse_field_new(struct Parse *parse, const struct Token *name,
		    enum field_type type)
{
	struct parse_create_table *stmt = &parse->parse_create_table;
	uint32_t id = stmt->field_count;
	++stmt->field_count;
	uint32_t size = sizeof(struct parse_table_field) * stmt->field_count;
	stmt->fields = sql_xrealloc(stmt->fields, size);
	struct parse_table_field *field = &stmt->fields[id];
	memset(field, 0, sizeof(*field));
	field->name = *name;
	field->type = type;
}

static struct parse_field_constraint *
sql_parse_field_constraint(struct Parse *parse)
{
	struct parse_table_field *field;
	if (parse->parse_type == 1) {
		struct parse_create_table *stmt = &parse->parse_create_table;
		field = &stmt->fields[stmt->field_count - 1];
	} else if (parse->parse_type == 2) {
		field = &parse->parse_add_column.field;
	}
	uint32_t id = field->constraint_count;
	++field->constraint_count;
	uint32_t size = sizeof(*field->constraints) * field->constraint_count;
	field->constraints = sql_xrealloc(field->constraints, size);
	struct parse_field_constraint *constraint = &field->constraints[id];
	memset(constraint, 0, sizeof(*constraint));
	return constraint;
}

void
sql_parse_field_null(struct Parse *parse, int onconf)
{
	struct parse_field_constraint *ct = sql_parse_field_constraint(parse);
	assert(ct->name.n == 0);
	ct->type = 0;
	ct->onconf = onconf;
}

void
sql_parse_field_not_null(struct Parse *parse, int onconf)
{
	struct parse_field_constraint *ct = sql_parse_field_constraint(parse);
	assert(ct->name.n == 0);
	ct->type = 1;
	ct->onconf = onconf;
}

void
sql_parse_field_pk(struct Parse *parse, const struct Token *name, int sortorder)
{
	struct parse_field_constraint *ct = sql_parse_field_constraint(parse);
	ct->name = *name;
	ct->type = 2;
	ct->sort_order = sortorder;
}

void
sql_parse_field_uq(struct Parse *parse, const struct Token *name)
{
	struct parse_field_constraint *ct = sql_parse_field_constraint(parse);
	ct->name = *name;
	ct->type = 3;
}

void
sql_parse_field_ck(struct Parse *parse, const struct Token *name,
		   const struct ExprSpan *expr)
{
	struct parse_field_constraint *ct = sql_parse_field_constraint(parse);
	ct->name = *name;
	ct->type = 4;
	ct->expr = *expr;
}

void
sql_parse_field_fk(struct Parse *parse, const struct Token *name,
		   struct ExprList *cols, const struct Token *parent)
{
	struct parse_field_constraint *ct = sql_parse_field_constraint(parse);
	ct->name = *name;
	ct->fk.foreign_fields = sql_expr_list_dup(cols, 0);
	ct->type = 5;
	ct->fk.table_name = *parent;
}

void
sql_parse_field_coll(struct Parse *parse, const struct Token *collate)
{
	struct parse_field_constraint *ct = sql_parse_field_constraint(parse);
	assert(ct->name.n == 0);
	ct->type = 6;
	ct->token = *collate;
}

void
sql_parse_field_default(struct Parse *parse, const struct ExprSpan *expr)
{
	struct parse_field_constraint *ct = sql_parse_field_constraint(parse);
	assert(ct->name.n == 0);
	ct->type = 7;
	ct->expr = *expr;
}

static struct parse_table_constraint *
sql_parse_space_constraint(struct Parse *parse)
{
	struct parse_create_table *stmt = &parse->parse_create_table;
	uint32_t id = stmt->constraint_count;
	++stmt->constraint_count;
	uint32_t size = sizeof(*stmt->constraints) * stmt->constraint_count;
	stmt->constraints = sql_xrealloc(stmt->constraints, size);
	struct parse_table_constraint *constraint = &stmt->constraints[id];
	memset(constraint, 0, sizeof(*constraint));
	return constraint;
}

void
sql_parse_table_pk(struct Parse *parse, const struct Token *name,
		   struct ExprList *cols)
{
	struct parse_table_constraint *ct = sql_parse_space_constraint(parse);
	ct->name = *name;
	ct->type = 2;
	ct->cols = sql_expr_list_dup(cols, 0);
}

void
sql_parse_table_uq(struct Parse *parse, const struct Token *name,
		   struct ExprList *cols)
{
	struct parse_table_constraint *ct = sql_parse_space_constraint(parse);
	ct->name = *name;
	ct->type = 3;
	ct->cols = sql_expr_list_dup(cols, 0);
}

void
sql_parse_table_ck(struct Parse *parse, const struct Token *name,
		   const struct ExprSpan *expr)
{
	struct parse_table_constraint *ct = sql_parse_space_constraint(parse);
	ct->name = *name;
	ct->type = 4;
	ct->expr = *expr;
}

void
sql_parse_table_fk(struct Parse *parse, const struct Token *name,
		   const struct Token *parent, struct ExprList *parent_cols,
		   struct ExprList *child_cols)
{
	struct parse_table_constraint *ct = sql_parse_space_constraint(parse);
	ct->name = *name;
	ct->fk.foreign_fields = sql_expr_list_dup(parent_cols, 0);
	ct->fk.local_fields = sql_expr_list_dup(child_cols, 0);
	ct->type = 5;
	ct->fk.table_name = *parent;
}
