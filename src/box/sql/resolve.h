/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 */
#pragma once

#include "ast.h"

/** Expression resolved into the form the build stage consumes. */
struct rast_expr {
	union {
		/** AST of an expression that is not resolved yet. */
		struct ast_expr *ast;
		struct {
			/** Dequoted string value. */
			const char *s;
			/** Length of the dequoted string above. */
			uint32_t n;
		};
		uint64_t u;
		double f;
		decimal_t d;
		bool b;
	};
	/**
	 * Operation of the expression. The same as ast_expr::op and defines
	 * which of the union members above is used.
	 */
	uint8_t op;
};

/** Resolved expression of an expression list. */
struct rast_expr_list_entry {
	/** The expression itself. */
	struct rast_expr expr;
	/** Dequoted and checked alias of the expression, or NULL. */
	const char *name;
	/**
	 * Alias of the expression in the legacy form: an unquoted alias is
	 * converted to upper case, a quoted one is kept as is. NULL when the
	 * alias is not set.
	 */
	const char *legacy_name;
	/**
	 * Text of the expression in the original SQL. Set for a select list
	 * only.
	 */
	const char *span;
	/** Length of the text above. */
	uint32_t span_len;
	/** Sort order of the expression. */
	enum sort_order order;
	/** Whether the expression is an AUTOINCREMENT column. */
	bool autoinc;
};

/** Resolved expression list. */
struct rast_expr_list {
	/** Array of entries. */
	struct rast_expr_list_entry *exprs;
	/** Number of entries. */
	uint32_t len;
};

/** Resolved SELECT statement. */
struct rast_select {
	/** AST of the statement. */
	struct ast_select *ast;
	/**
	 * Whether the statement is resolved, that is, whether the fields
	 * below are filled in.
	 */
	bool is_resolved;

	/** Resolved list of columns. */
	struct rast_expr_list columns;
};

/** Statement resolved into the form the build stage consumes. */
struct sql_rast {
	/** Type of the statement above. */
	enum sql_ast_type type;
	union {
		/** AST of a statement that is not resolved yet. */
		struct sql_ast *ast;
		/** Resolved SELECT statement. */
		struct rast_select select;
	};
};

struct sql_rast *
sql_resolve_ast(struct Parse *parser, struct sql_ast *ast);
