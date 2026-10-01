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
		/** Operand of a unary expression. */
		struct rast_expr *expr;
		/** Value of a TK_STRING or TK_BLOB expression. */
		struct {
			/** Dequoted string or decoded binary data. */
			char *z;
			/** Length of the data. */
			uint32_t n;
		};
		/** Operands of a binary expression. */
		struct {
			/** Left operand of a binary expression. */
			struct rast_expr *left;
			/** Right operand of a binary expression. */
			struct rast_expr *right;
		};
		/** Value of a TK_RAISE expression. */
		struct {
			/** Error message, or NULL for the IGNORE action. */
			char *raise;
			/** Length of the error message. */
			uint32_t raise_len;
			/** Conflict resolution action. */
			enum on_conflict_action action;
		};
		/** Operands of a TK_BETWEEN expression. */
		struct {
			/** Expression being tested against the range. */
			struct rast_expr *value;
			/** Lower bound of the range. */
			struct rast_expr *lower;
			/** Upper bound of the range. */
			struct rast_expr *upper;
		} between;
		/** Operands of a TK_IN expression. */
		struct {
			/** Expression tested for membership. */
			struct rast_expr *value;
			/** Value expressions, or NULL for a subquery. */
			struct rast_expr *exprs;
			/** Number of value expressions. */
			uint32_t len;
			/** Subquery, or NULL for a value list. */
			struct rast_select *select;
		} in;
		/** Operands of a TK_CASE expression. */
		struct {
			/** Operand of the CASE, or NULL if there is none. */
			struct rast_expr *value;
			/**
			 * WHEN and THEN expressions in pairs, followed by the
			 * ELSE expression, if there is one.
			 */
			struct rast_expr *exprs;
			/** Number of expressions in the array above. */
			uint32_t len;
		} cs;
		/** Elements of a TK_ARRAY, TK_MAP or TK_VECTOR expression. */
		struct {
			/**
			 * Array of element expressions. A map holds its keys
			 * and values in pairs.
			 */
			struct rast_expr *exprs;
			/** Number of element expressions. */
			uint32_t len;
		} list;
		/** Operand and keys of a TK_GETITEM expression. */
		struct {
			/** Expression the subscript is applied to. */
			struct rast_expr *value;
			/** Key expressions. */
			struct rast_expr *exprs;
			/** Number of key expressions. */
			uint32_t len;
		} getitem;
		/** Operand and collation of a TK_COLLATE expression. */
		struct {
			/** Expression the collation is applied to. */
			struct rast_expr *expr;
			/** Identifier of the collation. */
			uint32_t id;
		} coll;
		/** Subquery of a TK_SELECT or TK_EXISTS expression. */
		struct rast_select *select;
		/**
		 * Result column referred to by a TK_AS expression, by its
		 * alias or by its position in an ORDER BY term.
		 */
		struct {
			/** Expression of the result column. */
			const struct rast_expr *expr;
			/**
			 * Number of subqueries between the reference and the
			 * SELECT the result column belongs to.
			 */
			uint32_t depth;
		} alias;
		/** Column of a source referred to by a TK_COLUMN_REF. */
		struct {
			/**
			 * Number of subqueries between the reference and the
			 * SELECT the source belongs to.
			 */
			uint32_t depth;
			/** Position of the source in the FROM clause. */
			uint32_t source;
			/** Number of the field in the space of the source. */
			uint32_t fieldno;
			/** Exact type of the field. */
			enum field_type field_type;
		} column;
		/** Name and arguments of a TK_FUNCTION expression. */
		struct {
			/** Name of the resolved function. */
			char *name;
			/** Argument expressions. */
			struct rast_expr *args;
			/** Number of arguments. */
			uint32_t n_args;
			/** Exact type of the function result. */
			enum field_type returns;
			/**
			 * Probability of the first argument to be true, scaled
			 * by 2^27. Set for LIKELY, UNLIKELY and LIKELIHOOD.
			 */
			int probability;
			/** Whether DISTINCT is applied to the arguments. */
			bool is_distinct;
			/** Whether the function is an aggregate. */
			bool is_agg;
			/**
			 * Number of subqueries between the aggregate function
			 * and the SELECT it belongs to.
			 */
			uint32_t agg_depth;
			/** Whether it is LIKELY, UNLIKELY or LIKELIHOOD. */
			bool is_unlikely;
			/** Whether the function is deterministic. */
			bool is_deterministic;
		} func;
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
	/** Sort order of the entry, used for ORDER BY lists. */
	enum sort_order order;
	/**
	 * Position of the result column, starting from 1, which an ORDER BY
	 * or GROUP BY term refers to, or 0 if there is none.
	 */
	uint32_t order_by_col;
	/**
	 * Whether the expression contains aggregate functions of the SELECT
	 * it belongs to, directly or through the aliases of the result columns.
	 */
	bool has_agg;
	/**
	 * The least number of subqueries between the expression and the
	 * SELECTs whose sources it refers to outside of its subqueries, or
	 * UINT32_MAX if it does not refer to any source. Set for a select
	 * list only.
	 */
	uint32_t ref_depth;
};

/** Resolved expression list. */
struct rast_expr_list {
	/** Array of entries. */
	struct rast_expr_list_entry *exprs;
	/** Number of entries. */
	uint32_t len;
};

/** Pair of columns joined by NATURAL or USING. */
struct rast_join_column {
	/** Position of the source of the left column in the FROM clause. */
	uint32_t left_source;
	/** Number of the field of the left column. */
	uint32_t left_fieldno;
	/** Number of the field of the right column in the joined source. */
	uint32_t right_fieldno;
};

/** Resolved source of a SELECT statement. */
struct rast_source {
	/** Identifier of the space the source refers to. */
	uint32_t space_id;
	/** Dequoted alias of the source, or NULL. */
	const char *alias;
	/** Identifier of the index given by INDEXED BY, or UINT32_MAX. */
	uint32_t index_id;
	/** Whether NOT INDEXED is given for this source. */
	bool is_not_indexed;
	/** Whether scanning is not allowed for this source. */
	bool disallow_scan;
	/** Type of the join with the previous source, JT_* flags. */
	uint8_t join_type;
	/** Resolved ON clause of the join, NULL if there is none. */
	struct rast_expr *on;
	/** Dequoted names of the USING clause of the join. */
	const char **using_names;
	/** Number of the names above. */
	uint32_t using_count;
	/** Columns joined by NATURAL or USING. */
	struct rast_join_column *join_columns;
	/** Number of the joined columns. */
	uint32_t join_column_count;
	/** Resolved SELECT of a view, NULL for a table. */
	struct rast_select *select;
	/**
	 * Cursor of the source, assigned when the SELECT is built, or -1.
	 */
	int cursor;
};

/** Resolved list of sources. */
struct rast_source_list {
	/** Array of sources. */
	struct rast_source *sources;
	/** Number of sources. */
	uint32_t len;
};

/** Resolved SELECT statement. */
struct rast_select {
	/** Resolved FROM clause. */
	struct rast_source_list sources;
	/** Resolved list of columns. */
	struct rast_expr_list columns;
	/** Resolved LIMIT expression, NULL if there is none. */
	struct rast_expr *limit;
	/** Resolved OFFSET expression, NULL if there is none. */
	struct rast_expr *offset;
	/** Resolved WHERE expression, NULL if there is none. */
	struct rast_expr *where;
	/** Resolved GROUP BY list. */
	struct rast_expr_list group_by;
	/** Resolved HAVING expression, NULL if there is none. */
	struct rast_expr *having;
	/** Resolved ORDER BY list. */
	struct rast_expr_list order_by;
	/**
	 * SELECT to the left of this one in a compound SELECT, or NULL. The
	 * rightmost SELECT holds the ORDER BY, LIMIT and OFFSET of the compound
	 * SELECT.
	 */
	struct rast_select *prior;
	/** Whether the SELECT refers to names of the enclosing SELECTs. */
	bool is_correlated;
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
