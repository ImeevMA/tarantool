/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 */
#pragma once

#include "ast.h"

enum rast_source_type {
	SQL_RAST_TABLE = 0,
	SQL_RAST_SELECT,
	SQL_RAST_WITH,
};

/** Element of a WITH clause after resolve. */
struct rast_with {
	/** Link to other WITH entries visible in the current scope. */
	struct stailq_entry link;
	/** Name of the WITH clause. */
	char *name;
	/**
	 * Column names of the WITH clause, if specified explicitly. If not
	 * NULL, the number of columns is equal to select->ast->columns->len.
	 */
	char **columns;
	/** Resolved SELECT statement of the WITH clause. */
	struct rast_select *select;
};

/** Element of the FROM clause after resolve. */
struct rast_source {
	/** Type of the resolved source. */
	enum rast_source_type type;
	/** Original AST of the source. */
	struct ast_source *ast;
	union {
		/** Resolved WITH clause, when type is SQL_RAST_WITH. */
		struct rast_with *with;
		/** Resolved subselect, when type is SQL_RAST_SELECT. */
		struct rast_select *select;
		/** The space and index, when type is SQL_RAST_TABLE. */
		struct {
			/** Space defined by name. */
			struct space *space;
			/** Index defined by INDEXED BY clause. */
			struct index *index;
		};
	};
};

/** Structure that describes a resolved SELECT. */
struct rast_select {
	/** Link to other SELECTs of a compound SELECT. */
	struct rlist link;
	/** Original AST of the SELECT. */
	struct ast_select *ast;
	/** The FROM clause of the SELECT. */
	struct rast_source *sources;
	/** Number of elements in `sources`. */
	uint32_t source_count;
};

struct sql_rast {
	enum sql_ast_type type;
	/** Original AST the statement is resolved from. Always set. */
	struct sql_ast *ast;
	union {
		/** Resolved SELECT, set when type is SQL_AST_SELECT. */
		struct rast_select *select;
	};
};

/**
 * Resolve @a ast. Allocates on @a region. Returns NULL and sets diag on
 * error.
 */
struct sql_rast *
sql_resolve_ast(struct region *region, struct sql_ast *ast);
