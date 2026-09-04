/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 */
#pragma once

#include "box/field_def.h"

/** Element of a WITH clause after resolve. */
struct rast_with {
	/** Name of the CTE. */
	struct Token name;
	/** Column names of the CTE, if specified explicitly. */
	struct ast_id_list *columns;
	/** Resolved SELECT statement of the CTE. */
	struct rast_select *select;
};

/** Element of the FROM clause after resolve. */
struct rast_source {
	/** Source of the AST the element is built from. The fields that are
	 * not resolved yet are read from it.
	 */
	struct ast_source *ast;
	/** Resolved SELECT statement of a subquery, or of a CTE referenced
	 * by name. Not set for tables and views.
	 */
	struct rast_select *select;
};

/** Structure that describes a resolved SELECT. */
struct rast_select {
	/** Link to other SELECTs of a compound SELECT. */
	struct rlist link;
	/** SELECT of the AST this structure is built from. The fields that
	 * are not resolved yet are read from it.
	 */
	struct ast_select *ast;
	/** The FROM clause of the SELECT. */
	struct rast_source *sources;
	/** Number of sources in the FROM clause. */
	uint32_t source_count;
	/** Own WITH clauses of the SELECT. */
	struct rast_with *with;
	/** Number of own WITH clauses of the SELECT. */
	uint32_t with_count;
};

/**
 * Build a resolved SELECT from a parsed one. The WITH clauses of the outer
 * SELECTs, passed in with_list, are visible along with the own ones. A
 * reference to a CTE in the FROM clause is bound to the resolved SELECT
 * statement of the CTE.
 */
struct rast_select *
sql_resolve_ast_select(struct Parse *parser, struct ast_select *select,
		       struct rast_with *with_list, uint32_t with_count);

/** Resolved expression. */
struct rast_expr {
	/** Parser token code identifying the kind of expression. */
	uint8_t op;
	/** Resolved type of the expression. */
	enum field_type type;
	union {
		/**
		 * Original AST node, for opcodes not yet resolved directly
		 * by sql_resolve_ast_expr().
		 */
		struct ast_expr *ast;
		/** TK_INTEGER value, when type is FIELD_TYPE_INTEGER. */
		int64_t ival;
		/** TK_INTEGER value, when type is FIELD_TYPE_UNSIGNED. */
		uint64_t uval;
	};
};

/** Build a resolved expression from a parsed one. */
struct rast_expr *
sql_resolve_ast_expr(struct Parse *parser, struct ast_expr *ast);
