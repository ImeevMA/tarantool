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
	/** Link to the next element of the list. */
	struct stailq_entry link;
	/** Name of the WITH clause. */
	char *name;
	/**
	 * Column names of the WITH clause, if specified explicitly.
	 * If not NULL, the number or columns is equal to number of
	 * the resulting columns of the corresponding SELECT.
	 */
	char **columns;
	/** Resolved SELECT statement of the WITH clause. */
	struct rast_select *select;
};

/** Element of the FROM clause after resolve. */
struct rast_source {
	/** Link to the next element of the list. */
	struct stailq_entry link;
	/** Type of the resolve source. */
	enum rast_source_type type;
	/** Original AST of the source. */
	struct ast_source *ast;
	union {
		/** Resolved WITH clause. */
		struct rast_with *with;
		/** Resolved subselect. */
		struct rast_select *select;
		/** The space and index. */
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
	struct stailq sources;
	/** Own WITH clauses of the SELECT. */
	struct stailq with;
};

struct sql_rast {
	enum sql_ast_type type;
	union {
		struct sql_ast *ast;
		struct rast_select *select;
	};
};

struct sql_rast *
sql_resolve_ast(struct region *region, struct sql_ast *ast);
