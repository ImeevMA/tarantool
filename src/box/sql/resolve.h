/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 */
#pragma once

#include "ast.h"

struct sql_join_column;

struct rast_expr {
	struct ast_expr *ast;
	const char *name;
	const char *span;
	uint32_t span_len;
	enum sort_order order;
	bool autoinc;
};

enum rast_source_type {
	RAST_SOURCE_SPACE,
	RAST_SOURCE_SELECT,
	RAST_SOURCE_VIEW,
};

struct rast_source;

struct rast_select {
	struct ast_select *ast;

	struct rast_expr *columns;
	uint32_t column_count;

	struct rast_source *sources;
	uint32_t source_count;
};

struct rast_source {
	enum rast_source_type type;
	struct ast_source *ast;
	/** Resolved alias, if any. */
	const char *alias;
	/** Resolved ON-clause expression of a join, if any. */
	struct rast_expr join_on;
	/** Resolved type of join between this source and the previous one. */
	int join_type;
	/**
	 * True if the USING or NATURAL JOIN constraint of this source was
	 * resolved, so join_columns holds its resolved column pairs.
	 */
	bool join_resolved;
	/** Resolved join columns, if join_resolved is set. */
	struct sql_join_column *join_columns;
	/** Number of entries in join_columns. */
	uint32_t join_column_count;
	/** Resolved space id, or 0 if the source is a subquery. */
	uint32_t space_id;
	/** Resolved INDEXED BY index id, if is_indexed_by is set. */
	uint32_t index_id;
	/** True if the source has an INDEXED BY clause. */
	bool is_indexed_by;
	/** Resolved subquery, if type is SELECT or VIEW source. */
	struct rast_select select;
	/** True if scanning is not allowed for this source. */
	bool disallow_scan;
};

struct sql_rast {
	enum sql_ast_type type;
	union {
		struct sql_ast *ast;
		struct rast_select select;
	};
};

struct sql_rast *
sql_resolve_ast(struct region *region, struct sql_ast *ast);
