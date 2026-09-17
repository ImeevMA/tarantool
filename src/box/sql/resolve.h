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
		struct rast_expr *expr;
		struct rast_select *select;
		struct {
			struct rast_expr *left;
			struct rast_expr *right;
		};
		struct {
			struct rast_expr *expr;
			uint32_t id;
		} coll;
		struct {
			struct rast_expr *expr;
			enum field_type type;
		} cast;
		struct {
			struct rast_expr *exprs;
			uint32_t len;
		} list;
		struct {
			struct rast_expr *expr;
			struct rast_expr *first;
			struct rast_expr *last;
		} between;
		struct {
			struct rast_expr *expr;
			struct rast_select *select;
			struct rast_expr *list;
			uint32_t len;
		} in;
		struct {
			/**
			 * Message of the RAISE, a resolved TK_STRING expression,
			 * or NULL if there is none, that is, for RAISE(IGNORE).
			 */
			struct rast_expr *expr;
			/** Conflict resolution action of the RAISE. */
			enum on_conflict_action action;
		} raise;
		struct {
			/** Operand of the CASE, or NULL if there is none. */
			struct rast_expr *expr;
			/**
			 * WHEN and THEN expressions of the CASE, in pairs,
			 * followed by its ELSE expression, if there is one.
			 */
			struct rast_expr *list;
			/** Number of expressions in the list above. */
			uint32_t len;
		} cs;
	};
	/**
	 * Height of the tree headed by this node. Checked against
	 * SQL_MAX_EXPR_DEPTH during resolution so expr_from_rast() never has
	 * to fail on it.
	 */
	int height;
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
	/** Resolved list of columns. */
	struct rast_expr_list columns;
	uint32_t flags;
	/** Height of the tallest expression among the columns above. */
	int height;
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
	bool is_resolved;
};

struct sql_rast *
sql_resolve_ast(struct Parse *parser, struct sql_ast *ast);
