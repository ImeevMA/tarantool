/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 */
#pragma once

#include "ast.h"
#include "box/field_def.h"

struct sql_resolver_context {
	struct region *region;
	struct Parse *parser;
};

struct rast_drop_index {
	/** Link to the next element of the list. */
	struct stailq_entry link;
	uint32_t space_id;
	uint32_t index_id;
};

/** Element of list of names. */
struct rast_name_list_entry {
	/** Link to the next element of the list. */
	struct stailq_entry link;
	/** NULL-terminated name. */
	char *name;
};

/** Element of list of privileges. */
struct rast_priv_list_entry {
	/** Link to the next element of the list. */
	struct stailq_entry link;
	/** NULL-terminated type of object. */
	char *type;
	/** User ID of grantee. */
	uint32_t grantee;
	/** ID of the object. */
	uint32_t id;
};

struct rast_drop_view {
	uint32_t space_id;
	struct stailq priv_list;
	struct stailq trigger_list;
};

struct rast_drop_sequence {
	uint32_t *id;
	bool has_data;
};

struct rast_drop_table {
	uint32_t space_id;
	struct stailq priv_list;
	struct stailq index_list;
	struct stailq trigger_list;
	uint32_t *sequence_id;
	bool has_sequence;
	bool has_sequence_data;
	bool has_truncate;
};

/** Resolved expression. */
struct rast_expr {
	/** Parser token code identifying the kind of expression. */
	uint8_t op;
	/** Resolved type of the expression. */
	enum field_type type;
	union {
		/**
		 * Original AST node, for opcodes not yet resolved
		 * directly by this function.
		 */
		struct ast_expr *ast;
		/** TK_INTEGER value, when type is FIELD_TYPE_INTEGER. */
		int64_t ival;
		/** TK_INTEGER value, when type is FIELD_TYPE_UNSIGNED. */
		uint64_t uval;
		/** Resolved collation id of the expression, or COLL_NONE. */
		uint32_t coll_id;
	};
};

/**
 * Resolves names and types in @a ast, using @a nc to look up columns
 * and tables. Returns NULL and sets diag on error.
 */
struct rast_expr *
sql_resolve_expr(struct sql_resolver_context *ctx, struct ast_expr *ast);

struct sql_rast {
	enum sql_ast_type type;
	union {
		struct sql_ast *ast;
		struct rast_drop_index drop_index;
		const char *trigger_name;
		struct rast_drop_view drop_view;
		struct rast_drop_table drop_table;
	};
};

struct sql_rast *
sql_resolve_ast(struct sql_resolver_context *ctx, struct sql_ast *ast);
