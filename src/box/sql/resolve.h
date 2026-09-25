/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 */
#pragma once

#include "ast.h"
#include "core/decimal.h"

struct Parse;

/** Expression resolved into the form the build stage consumes. */
struct rast_expr {
	union {
		/** Value for TK_TRUE, TK_FALSE and TK_UNKNOWN. */
		bool b;
		/** Value of a TK_INTEGER expression. */
		struct {
			/** Absolute value. */
			uint64_t u;
			/** Whether the value is negative. */
			bool is_neg;
		};
		/** Value of a TK_FLOAT expression. */
		double f;
		/** Value of a TK_DECIMAL expression. */
		decimal_t *dec;
		/** Value of a TK_STRING or TK_BLOB expression. */
		struct {
			/** Dequoted string or decoded binary data. */
			char *z;
			/** Length of the data. */
			uint32_t n;
		};
	};
	/** Data type of the expression. */
	enum sql_type type;
	/** Height of expression tree. */
	uint32_t height;
	/** Operation of the expression. */
	uint8_t op;
};

/** Resolved entry of an expression list. */
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
	 * only, NULL otherwise.
	 */
	const char *span;
	/** Length of the text above. */
	uint32_t span_len;
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
	/** Flags of the SELECT. */
	uint32_t flags;
	/** Link type between linked SELECTs. */
	uint8_t op;
};

/**
 * Resolved AST. During resolve separation it is a thin overlay over the parsed
 * AST: the resolve phase gradually moves work here from bytecode generation.
 */
struct sql_rast {
	/** Type of the resolved statement, mirrors struct sql_ast.type. */
	enum sql_ast_type type;
	union {
		/** Resolved SELECT statement. */
		struct rast_select select;
	};
	/** Whether the statement is resolved. */
	bool is_resolved;
};

/** Build the resolved AST for the given parsed statement. */
struct sql_rast *
sql_resolve_ast(struct Parse *parser, const struct sql_ast *ast);

/**
 * Build a SELECT statement from the given resolved SELECT. A statement that is
 * not resolved is built from the AST.
 *
 * Returns NULL on error.
 */
struct Select *
select_from_rast(struct Parse *parser, struct rast_select *select);
