/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2023, Tarantool AUTHORS, please see AUTHORS file.
 */
#include <ctype.h>
#include "sqlInt.h"
#include "schema.h"
#include "sequence.h"
#include "coll_id_cache.h"
#include "tuple_constraint_def.h"

enum {
	TYPE_STR_SIZE = 16,
};

/** An objected used to accumulate a statement. */
struct sql_desc {
	/** Accumulate the string representation of the statement. */
	struct StrAccum acc;
	/** Accumulate the string representation of the error. */
	struct StrAccum err;
	/** Space used in SHOW CREATE TABLE;. */
	const struct space *space;
	/** A type that specifies the action to take on an error. */
	enum sql_show_type type;
	/** True, if accumulations must be interrupted. */
	bool is_aborted;
};

/**
 * Return name that can be used in CREATE TABLE and the created object will have
 * the original name.
 */
static char *
sql_prettify_name_new(const char *name)
{
	char *escaped_name = sql_escaped_name_new(name);
	assert(escaped_name[0] == '"' &&
	       escaped_name[strlen(escaped_name) - 1] == '"');
	char *normalized_name = sql_normalized_name_new(name, strlen(name));
	if (isalpha(name[0]) && strlen(escaped_name) == strlen(name) + 2 &&
	    strcmp(normalized_name, name) == 0) {
		sql_xfree(escaped_name);
		return normalized_name;
	}
	sql_xfree(normalized_name);
	return escaped_name;
}

/** Initialize the object used to accumulate a statement. */
static void
sql_desc_initialize(struct sql_desc *desc, enum sql_show_type type,
		    const struct space *space)
{
	sqlStrAccumInit(&desc->acc, NULL, 0, SQL_MAX_LENGTH);
	sqlStrAccumInit(&desc->err, NULL, 0, SQL_MAX_LENGTH);
	desc->space = space;
	desc->type = type;
	desc->is_aborted = false;

	char *new_name = sql_prettify_name_new(space->def->name);
	sqlXPrintf(&desc->acc, "CREATE TABLE %s(", new_name);
	sql_xfree(new_name);
}

/** Append a new string to the object used to accumulate a statement. */
CFORMAT(printf, 2, 3) static void
sql_desc_append(struct sql_desc *desc, const char *fmt, ...)
{
	if (desc->is_aborted)
		return;
	va_list ap;
	va_start(ap, fmt);
	sqlVXPrintf(&desc->acc, fmt, ap);
	va_end(ap);
}

/** Append a name to the object used to accumulate a statement. */
static void
sql_desc_append_name(struct sql_desc *desc, const char *name)
{
	if (desc->is_aborted)
		return;
	char *new_name = sql_prettify_name_new(name);
	sqlXPrintf(&desc->acc, "%s", new_name);
	sql_xfree(new_name);
}

/** Append a new error to the object used to accumulate a statement. */
static void
sql_desc_error(struct sql_desc *desc, const char *type, const char *name,
	       const char *error)
{
	if (desc->is_aborted)
		return;
	if (desc->type == SQL_SHOW_INCLUDE) {
		sqlXPrintf(&desc->err, "\n/* Problem with %s '%s': %s. */",
			   type, name, error);
		return;
	}
	desc->is_aborted = true;
	if (desc->type == SQL_SHOW_IGNORE)
		return;
	assert(desc->type == SQL_SHOW_THROW);
	diag_set(ClientError, ER_SQL_DESCRIPTION, type, name, error);
}

/** Finalize a described statement. */
static char *
sql_desc_finalize(struct sql_desc *desc)
{
	if (desc->is_aborted) {
		sqlStrAccumReset(&desc->acc);
		sqlStrAccumReset(&desc->err);
		return NULL;
	}
	if (desc->err.nChar > 0) {
		char *err = sqlStrAccumFinish(&desc->err);
		sqlXPrintf(&desc->acc, "%s", err);
		sql_xfree(err);
	}

	if (space_is_memtx(desc->space))
		sqlXPrintf(&desc->acc, ")\nWITH ENGINE = 'memtx';");
	else if (space_is_vinyl(desc->space))
		sqlXPrintf(&desc->acc, ")\nWITH ENGINE = 'vinyl';");
	else
		sqlXPrintf(&desc->acc, ");");

	return sqlStrAccumFinish(&desc->acc);
}

/** Add a field foreign key constraint to the statement description. */
static void
sql_describe_field_foreign_key(struct sql_desc *desc,
			       const struct tuple_constraint_def *cdef)
{
	if (desc->is_aborted)
		return;
	assert(cdef->type == CONSTR_FKEY && cdef->fkey.field_mapping_size == 0);
	const struct space *foreign_space = space_by_id(cdef->fkey.space_id);
	assert(foreign_space != NULL);
	const struct tuple_constraint_field_id *field = &cdef->fkey.field;
	if (field->name_len == 0 &&
	    field->id >= foreign_space->def->field_count) {
		sql_desc_error(desc, "foreign key", cdef->name,
			       "foreign field is unnamed");
		return;
	}

	const char *field_name = field->name_len > 0 ? field->name :
				 foreign_space->def->fields[field->id].name;
	sql_desc_append(desc, " CONSTRAINT ");
	sql_desc_append_name(desc, cdef->name);
	sql_desc_append(desc, " REFERENCES ");
	sql_desc_append_name(desc, foreign_space->def->name);
	sql_desc_append(desc, "(");
	sql_desc_append_name(desc, field_name);
	sql_desc_append(desc, ")");
}

/** Add a tuple foreign key constraint to the statement description. */
static void
sql_describe_tuple_foreign_key(struct sql_desc *desc,
			       const struct space_def *def,
			       const struct tuple_constraint_def *cdef)
{
	if (desc->is_aborted)
		return;
	assert(cdef->type == CONSTR_FKEY && cdef->fkey.field_mapping_size > 0);
	const struct space *foreign_space = space_by_id(cdef->fkey.space_id);
	assert(foreign_space != NULL);
	bool is_error = false;
	for (uint32_t i = 0; i < cdef->fkey.field_mapping_size; ++i) {
		const struct tuple_constraint_field_id *field =
			&cdef->fkey.field_mapping[i].local_field;
		if (field->name_len == 0 &&
		    field->id >= foreign_space->def->field_count) {
			sql_desc_error(desc, "foreign key", cdef->name,
				       "local field is unnamed");
			is_error = true;
		}
		field = &cdef->fkey.field_mapping[i].foreign_field;
		if (field->name_len == 0 &&
		    field->id >= foreign_space->def->field_count) {
			sql_desc_error(desc, "foreign key", cdef->name,
				       "foreign field is unnamed");
			is_error = true;
		}
	}
	if (is_error)
		return;

	sql_desc_append(desc, ",\nCONSTRAINT ");
	sql_desc_append_name(desc, cdef->name);
	sql_desc_append(desc, " FOREIGN KEY(");
	for (uint32_t i = 0; i < cdef->fkey.field_mapping_size; ++i) {
		const struct tuple_constraint_field_id *field =
			&cdef->fkey.field_mapping[i].local_field;
		const char *field_name = field->name_len != 0 ? field->name :
					 def->fields[field->id].name;
		if (i > 0)
			sql_desc_append(desc, ", ");
		sql_desc_append_name(desc, field_name);
	}

	sql_desc_append(desc, ") REFERENCES ");
	sql_desc_append_name(desc, foreign_space->def->name);
	sql_desc_append(desc, "(");
	for (uint32_t i = 0; i < cdef->fkey.field_mapping_size; ++i) {
		const struct tuple_constraint_field_id *field =
			&cdef->fkey.field_mapping[i].foreign_field;
		assert(field->name_len != 0 || field->id < def->field_count);
		const char *field_name = field->name_len != 0 ? field->name :
					 def->fields[field->id].name;
		if (i > 0)
			sql_desc_append(desc, ", ");
		sql_desc_append_name(desc, field_name);
	}
	sql_desc_append(desc, ")");
}

/** Add a field check constraint to the statement description. */
static void
sql_describe_field_check(struct sql_desc *desc, const char *field_name,
			 const struct tuple_constraint_def *cdef)
{
	if (desc->is_aborted)
		return;
	assert(cdef->type == CONSTR_FUNC);
	bool is_error = false;
	const struct func *func = func_by_id(cdef->func.id);
	if (func->def->language != FUNC_LANGUAGE_SQL_EXPR) {
		sql_desc_error(desc, "check constraint", cdef->name,
			       "wrong constraint expression");
		is_error = true;
	} else if (!func_sql_expr_has_single_arg(func, field_name)) {
		sql_desc_error(desc, "check constraint", cdef->name,
			       "wrong field name in constraint expression");
		is_error = true;
	}
	if (is_error)
		return;

	sql_desc_append(desc, " CONSTRAINT ");
	sql_desc_append_name(desc, cdef->name);
	sql_desc_append(desc, " CHECK(%s)", func->def->body);
}

/** Add a tuple check constraint to the statement description. */
static void
sql_describe_tuple_check(struct sql_desc *desc,
			 const struct tuple_constraint_def *cdef)
{
	if (desc->is_aborted)
		return;
	assert(cdef->type == CONSTR_FUNC);
	const struct func *func = func_by_id(cdef->func.id);
	if (func->def->language != FUNC_LANGUAGE_SQL_EXPR) {
		sql_desc_error(desc, "check constraint", cdef->name,
			       "wrong constraint expression");
		return;
	}
	sql_desc_append(desc, ",\nCONSTRAINT ");
	sql_desc_append_name(desc, cdef->name);
	sql_desc_append(desc, " CHECK(%s)", func->def->body);
}

/** Add a field to the statement description. */
static void
sql_describe_field(struct sql_desc *desc, const struct field_def *field)
{
	if (desc->is_aborted)
		return;
	sql_desc_append_name(desc, field->name);
	char *field_type = strtoupperdup(field_type_strs[field->type]);
	sql_desc_append(desc, " %s", field_type);
	free(field_type);

	if (field->coll_id != 0) {
		struct coll_id *coll_id = coll_by_id(field->coll_id);
		if (coll_id == NULL) {
			sql_desc_error(desc, "collation",
				       tt_sprintf("%d", field->coll_id),
				       "collation does not exist");
		} else {
			sql_desc_append(desc, " COLLATE ");
			sql_desc_append_name(desc, coll_id->name);
		}
	}
	if (!field->is_nullable)
		sql_desc_append(desc, " NOT NULL");
	if (field->default_value != NULL)
		sql_desc_append(desc, " DEFAULT(%s)", field->default_value);
	for (uint32_t i = 0; i < field->constraint_count; ++i) {
		struct tuple_constraint_def *cdef = &field->constraint_def[i];
		assert(cdef->type == CONSTR_FKEY || cdef->type == CONSTR_FUNC);
		if (cdef->type == CONSTR_FKEY)
			sql_describe_field_foreign_key(desc, cdef);
		else
			sql_describe_field_check(desc, field->name, cdef);
	}
}

/** Add a primary key to the statement description. */
static void
sql_describe_primary_key(struct sql_desc *desc, const struct space *space)
{
	if (desc->is_aborted)
		return;
	if (space->index_count == 0) {
		sql_desc_error(desc, "space", space->def->name,
			       "primary key is not defined");
		return;
	}

	const struct index *pk = space->index[0];
	assert(pk->def->opts.is_unique);
	bool is_error = false;
	if (pk->def->type != TREE) {
		const char *err = "primary key has unsupported index type";
		sql_desc_error(desc, "space", space->def->name, err);
		is_error = true;
	}

	for (uint32_t i = 0; i < pk->def->key_def->part_count; ++i) {
		uint32_t fieldno = pk->def->key_def->parts[i].fieldno;
		if (fieldno >= space->def->field_count) {
			const char *err = tt_sprintf("field %u is unnamed",
						     fieldno + 1);
			sql_desc_error(desc, "primary key", pk->def->name, err);
			is_error = true;
			continue;
		}
		struct field_def *field = &space->def->fields[fieldno];
		if (pk->def->key_def->parts[i].type != field->type) {
			const char *err =
				tt_sprintf("field '%s' and related part are of "
					   "different types", field->name);
			sql_desc_error(desc, "primary key", pk->def->name, err);
			is_error = true;
		}
		if (pk->def->key_def->parts[i].coll_id != field->coll_id) {
			const char *err =
				tt_sprintf("field '%s' and related part have "
					   "different collations", field->name);
			sql_desc_error(desc, "primary key", pk->def->name, err);
			is_error = true;
		}
	}

	if (is_error)
		return;

	bool has_sequence = false;
	if (space->sequence != NULL) {
		struct sequence_def *sdef = space->sequence->def;
		if (sdef->step != 1 || sdef->min != 0 || sdef->start != 1 ||
		    sdef->max != INT64_MAX || sdef->cache != 0 || sdef->cycle ||
		    strcmp(sdef->name, space->def->name) != 0) {
			const char *err = "unsupported sequence definition";
			sql_desc_error(desc, "sequence", sdef->name, err);
		} else if (space->sequence_fieldno > space->def->field_count) {
			const char *err =
				"sequence is attached to unnamed field";
			sql_desc_error(desc, "sequence", sdef->name, err);
		} else {
			has_sequence = true;
		}
	}

	sql_desc_append(desc, ",\nCONSTRAINT ");
	sql_desc_append_name(desc, pk->def->name);
	sql_desc_append(desc, " PRIMARY KEY(");
	for (uint32_t i = 0; i < pk->def->key_def->part_count; ++i) {
		uint32_t fieldno = pk->def->key_def->parts[i].fieldno;
		if (i > 0)
			sql_desc_append(desc, ", ");
		sql_desc_append_name(desc, space->def->fields[fieldno].name);
		if (has_sequence && fieldno == space->sequence_fieldno)
			sql_desc_append(desc, " AUTOINCREMENT");
	}
	sql_desc_append(desc, ")");
}

/** Add a index to the statement description. */
static void
sql_describe_index(struct sql_desc *desc, const struct space *space,
		   const struct index *index)
{
	if (desc->is_aborted)
		return;
	assert(index != NULL);
	bool is_error = false;
	if (index->def->type != TREE) {
		const char *err = "unsupported index type";
		sql_desc_error(desc, "index", index->def->name, err);
		is_error = true;
	}
	if (!index->def->opts.is_unique) {
		const char *err = "non-unique index";
		sql_desc_error(desc, "index", index->def->name, err);
		is_error = true;
	}
	for (uint32_t i = 0; i < index->def->key_def->part_count; ++i) {
		uint32_t fieldno = index->def->key_def->parts[i].fieldno;
		if (fieldno >= space->def->field_count) {
			const char *err = tt_sprintf("field %u is unnamed",
						     fieldno + 1);
			sql_desc_error(desc, "index", index->def->name, err);
			is_error = true;
			continue;
		}
		struct field_def *field = &space->def->fields[fieldno];
		if (index->def->key_def->parts[i].type != field->type) {
			const char *err =
				tt_sprintf("field '%s' and related part are of "
					   "different types", field->name);
			sql_desc_error(desc, "index", index->def->name, err);
			is_error = true;
		}
		if (index->def->key_def->parts[i].coll_id != field->coll_id) {
			const char *err =
				tt_sprintf("field '%s' and related part have "
					   "different collations", field->name);
			sql_desc_error(desc, "index", index->def->name, err);
			is_error = true;
		}
	}

	if (is_error)
		return;

	sql_desc_append(desc, ",\nCONSTRAINT ");
	sql_desc_append_name(desc, index->def->name);
	sql_desc_append(desc, " UNIQUE(");
	for (uint32_t i = 0; i < index->def->key_def->part_count; ++i) {
		uint32_t fieldno = index->def->key_def->parts[i].fieldno;
		if (i > 0)
			sql_desc_append(desc, ", ");
		sql_desc_append_name(desc, space->def->fields[fieldno].name);
	}
	sql_desc_append(desc, ")");
}

/** Add the table to the statement description. */
static void
sql_describe_table(struct sql_desc *desc, const struct space *space)
{
	if (desc->is_aborted)
		return;
	if (space->def->field_count == 0) {
		const char *err = "format is missing";
		sql_desc_error(desc, "space", space->def->name, err);
	}
	for (uint32_t i = 0; i < space->def->field_count; ++i) {
		if (i > 0)
			sql_desc_append(desc, ",");
		sql_desc_append(desc, "\n");
		sql_describe_field(desc, &space->def->fields[i]);
	}

	sql_describe_primary_key(desc, space);
	for (uint32_t i = 1; i < space->index_count; ++i)
		sql_describe_index(desc, space, space->index[i]);

	for (uint32_t i = 0; i < space->def->opts.constraint_count; ++i) {
		struct tuple_constraint_def *cdef =
			&space->def->opts.constraint_def[i];
		assert(cdef->type == CONSTR_FKEY || cdef->type == CONSTR_FUNC);
		if (cdef->type == CONSTR_FKEY)
			sql_describe_tuple_foreign_key(desc, space->def, cdef);
		else
			sql_describe_tuple_check(desc, cdef);
	}

	if (!space_is_memtx(space) && !space_is_vinyl(space)) {
		const char *err = "wrong space engine";
		sql_desc_error(desc, "space", space->def->name, err);
	}
}

int
sql_show_create_table(uint32_t space_id, enum sql_show_type type, char **res)
{
	struct space *space = space_by_id(space_id);
	assert(space != NULL);

	struct sql_desc desc;
	sql_desc_initialize(&desc, type, space);
	sql_describe_table(&desc, space);
	*res = sql_desc_finalize(&desc);
	if (desc.is_aborted && type == SQL_SHOW_THROW)
		return -1;
	return 0;
}
