/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 */
#pragma once

#include "ast.h"
#include "core/decimal.h"

struct Parse;
struct space;
struct Expr;
struct Select;
struct SrcList;
struct field_link;
struct fk_constraint_def;

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
		/** Bound variable of a TK_VAR_* expression. */
		struct {
			/** Text of the variable in the query. */
			const char *name;
			/** Length of the text. */
			uint32_t len;
			/** Number of the variable, see struct ast_expr. */
			int num;
			/** Base of the variable, see struct ast_expr. */
			int base;
		} var;
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
		/**
		 * Column of a source referred to by a TK_COLUMN_REF, or of
		 * the OLD or NEW row of a trigger referred to by a
		 * TK_TRIGGER.
		 */
		struct {
			/**
			 * Number of subqueries between the reference and the
			 * SELECT the source belongs to.
			 */
			uint32_t depth;
			/**
			 * Position of the source in the FROM clause, or 0 for
			 * the OLD row and 1 for the NEW row of a trigger.
			 */
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
	/** Identifier of the space the source refers to, 0 for a subquery. */
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
	/**
	 * Resolved SELECT of a view, a subquery or a table of a WITH clause,
	 * NULL for a table.
	 */
	struct rast_select *select;
	/**
	 * Definition of a subquery or a table of a WITH clause, NULL for a
	 * table or a view.
	 */
	const struct space_def *def;
	/** Whether the source is a table of a WITH clause. */
	bool is_cte;
	/**
	 * Source of the table of a WITH clause the source refers to from the
	 * SELECT of the table, NULL if the source is not such a reference.
	 */
	const struct rast_source *recursive;
	/**
	 * Space of a table of a WITH clause, created when the SELECT using the
	 * table is built, and shared with the references to the table from its
	 * SELECT.
	 */
	struct space *space;
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
	/**
	 * Whether the parts of the compound SELECT to the left of the rightmost
	 * one are set up before it, as the legacy code does for the SELECT of
	 * a table of a WITH clause joined by UNION or UNION ALL.
	 */
	bool is_rightmost_last;
	/** Flags of the SELECT. */
	uint32_t flags;
	/** Link type between linked SELECTs. */
	uint8_t op;
};

/** Resolved INSERT statement. */
struct rast_insert {
	/** Space the rows are inserted into. */
	struct space *space;
	/**
	 * Numbers of the fields the values of a row are inserted into, in the
	 * order of the values, or NULL if no column list is given.
	 */
	uint32_t *fieldnos;
	/** Number of the fields above. */
	uint32_t fieldno_count;
	/** SELECT producing the rows, or NULL for DEFAULT VALUES. */
	struct rast_select *select;
	/**
	 * Space whose tuples are copied as they are, as xferOptimization()
	 * does, if the rows are produced by SELECT * from a single space
	 * without any other clauses, or NULL.
	 */
	struct space *xfer_source;
	/** Action on conflict. */
	enum on_conflict_action action;
};

/** Resolved entry of the SET clause of an UPDATE statement. */
struct rast_set_entry {
	/** Name of the column, if the value is assigned to one column. */
	struct Token name;
	/** Names of the columns the value is assigned to, or NULL. */
	struct ast_id_list *ids;
	/** The value. */
	struct rast_expr expr;
};

/** Resolved UPDATE statement. */
struct rast_update {
	/** The space the rows are updated in, as the only source. */
	struct rast_source_list target;
	/** Entries of the SET clause in the scope of the target. */
	struct rast_set_entry *set;
	/** Number of the entries above. */
	uint32_t set_len;
	/** WHERE clause in the scope of the target, or NULL. */
	struct rast_expr *where;
	/**
	 * SELECT * FROM the view with the WHERE clause realizing the view the
	 * rows are updated in, or NULL if the space is not a view.
	 */
	struct rast_select *view_select;
	/** Action on conflict. */
	enum on_conflict_action action;
};

/** Resolved DELETE statement. */
struct rast_delete {
	/** The space the rows are deleted from, as the only source. */
	struct rast_source_list target;
	/** WHERE clause in the scope of the target, or NULL. */
	struct rast_expr *where;
	/**
	 * SELECT * FROM the view with the WHERE clause realizing the view the
	 * rows are deleted from, or NULL if the space is not a view.
	 */
	struct rast_select *view_select;
};

/** Resolved TRUNCATE statement. */
struct rast_truncate {
	/** Space to truncate. */
	struct space *space;
};

/** Resolved DROP TABLE or DROP VIEW statement. */
struct rast_drop_table {
	/** Space to drop, or NULL if it does not exist. */
	const struct space *space;
	/** Whether it is DROP VIEW. */
	bool is_view;
};

/** Resolved DROP INDEX statement. */
struct rast_drop_index {
	/** Space of the index, or NULL if it does not exist. */
	const struct space *space;
	/** ID of the index, or UINT32_MAX if it does not exist. */
	uint32_t index_id;
};

/** Resolved DROP TRIGGER statement. */
struct rast_drop_trigger {
	/** Name of the trigger. */
	struct Token name;
	/** Whether it is not an error if the trigger does not exist. */
	bool if_exists;
};

/** Resolved ALTER TABLE RENAME statement. */
struct rast_alter_rename {
	/** Space to rename. */
	const struct space *space;
	/** New name of the space. */
	const char *new_name;
};

/** Resolved ALTER TABLE DROP CONSTRAINT statement. */
struct rast_drop_constraint {
	/** Space of the constraint. */
	const struct space *space;
	/** Field of a field constraint, or UINT32_MAX for a tuple one. */
	uint32_t fieldno;
	/** FOREIGN KEY constraint to drop, or NULL. */
	const struct tuple_constraint_def *fk;
	/** CHECK constraint to drop, or NULL. */
	const struct tuple_constraint_def *ck;
	/** ID of the index of the constraint to drop, or UINT32_MAX. */
	uint32_t index_id;
};

/** Resolved part of an index: a column and its collation. */
struct rast_index_part {
	/** Number of the field of the column. */
	uint32_t fieldno;
	/** ID of the collation of the part. */
	uint32_t coll_id;
};

/** Resolved CREATE INDEX statement. */
struct rast_create_index {
	/**
	 * Space the index is created for, or NULL if the statement does
	 * nothing: the space or the index does not exist with IF NOT EXISTS.
	 */
	struct space *space;
	/** Name of the index. */
	struct Token name;
	/** Parts of the index. */
	struct rast_index_part *parts;
	/** Number of the parts of the index. */
	uint32_t part_count;
	/** Whether the index is unique. */
	bool is_unique;
};

/** Resolved FOREIGN KEY constraint of a new table or column. */
struct rast_fk {
	/** Link in the list of the constraints. */
	struct rlist link;
	/**
	 * Definition of the constraint. The IDs of the spaces of a
	 * self-referenced constraint are set when the bytecode is emitted.
	 */
	struct fk_constraint_def *def;
	/**
	 * Referenced columns of a self-referenced constraint, resolved after
	 * all the columns of the space are resolved, or NULL.
	 */
	const struct ast_id_list *selfref_cols;
	/** Whether the constraint references the space itself. */
	bool is_self_referenced;
};

/** DEFAULT of a column which is computed by a function. */
struct rast_default_func {
	/** Link in the list of the functions. */
	struct stailq_entry link;
	/** Number of the field. */
	uint32_t fieldno;
	/** Text of the expression, the body of the function. */
	struct Token body;
};

/** Resolved CREATE TABLE or ALTER TABLE ADD COLUMN statement. */
struct rast_create_table {
	/**
	 * Space being created, or a copy of the space being altered, with all
	 * the new fields and indexes.
	 */
	struct space *space;
	/** CHECK constraints, struct ck_constraint_parse, in reverse order. */
	struct rlist checks;
	/** FOREIGN KEY constraints, struct rast_fk, in reverse order. */
	struct rlist fkeys;
	/** DEFAULTs computed by functions, struct rast_default_func. */
	struct stailq default_funcs;
	/** Number of the AUTOINCREMENT field, UINT32_MAX if there is none. */
	uint32_t autoinc_fieldno;
	/** If set, it is not an error if the space already exists. */
	bool if_not_exists;
};

/** Resolved ALTER TABLE ADD CONSTRAINT statement. */
struct rast_add_constraint {
	/** Space the constraint is added to. */
	struct space *space;
	/** Name of the constraint. */
	struct Token name;
	/** Type of the constraint. */
	enum ast_property_type type;
	union {
		/** Resolved UNIQUE or PRIMARY KEY constraint. */
		struct {
			/** Parts of the index of the constraint. */
			struct rast_index_part *parts;
			/** Number of the parts of the index. */
			uint32_t part_count;
			/**
			 * Number of the AUTOINCREMENT field, or UINT32_MAX if
			 * there is none.
			 */
			uint32_t autoinc_fieldno;
		} index;
		/** Resolved FOREIGN KEY constraint. */
		struct {
			/** Referenced space. */
			struct space *parent;
			/** Mapping of the child fields to the parent ones. */
			struct field_link *links;
			/** Number of the links. */
			uint32_t link_count;
		} fk;
		/** Text of the expression of CHECK constraint. */
		struct Token check;
	};
};

/** Resolved CREATE VIEW statement. */
struct rast_create_view {
	/** Name of the view. */
	struct Token name;
	/** Names of the columns of the view, or NULL. */
	struct ast_id_list *columns;
	/** SELECT of the view. */
	struct rast_select *select;
	/** Whether the view may already exist. */
	bool if_not_exists;
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
		/** Resolved INSERT statement. */
		struct rast_insert insert;
		/** Resolved CREATE VIEW statement. */
		struct rast_create_view create_view;
		/** Resolved TRUNCATE statement. */
		struct rast_truncate truncate;
		/** Resolved DROP TABLE or DROP VIEW statement. */
		struct rast_drop_table drop_table;
		/** Resolved DROP INDEX statement. */
		struct rast_drop_index drop_index;
		/** Name of the savepoint of a savepoint statement. */
		struct Token savepoint;
		/** Resolved DROP TRIGGER statement. */
		struct rast_drop_trigger drop_trigger;
		/** Resolved ALTER TABLE RENAME statement. */
		struct rast_alter_rename alter_rename;
		/** Resolved ALTER TABLE DROP CONSTRAINT statement. */
		struct rast_drop_constraint drop_constraint;
		/** Resolved CREATE INDEX statement. */
		struct rast_create_index create_index;
		/** Resolved ALTER TABLE ADD CONSTRAINT statement. */
		struct rast_add_constraint add_constraint;
		/** Resolved CREATE TABLE or ALTER TABLE ADD COLUMN. */
		struct rast_create_table create_table;
		/** Resolved DELETE statement. */
		struct rast_delete del;
		/** Resolved UPDATE statement. */
		struct rast_update update;
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

/**
 * Create the body of an SQL_EXPR function from the given AST, resolved in the
 * scope of a row whose fields are referred to by their names, looked up when
 * the function is called.
 *
 * Returns NULL on error.
 */
struct Expr *
sql_func_expr_from_ast(struct Parse *parser, const struct ast_expr *ast);

/**
 * Create the WHEN clause of a trigger from the given AST, resolved in the
 * scope of the trigger being coded by the given parser.
 *
 * Returns NULL on error.
 */
struct Expr *
sql_trigger_when_from_ast(struct Parse *parser, const struct ast_expr *when);

/**
 * Create a source list from the given resolved list of the only source of a
 * DELETE or UPDATE statement, whose cursor is assigned.
 */
struct SrcList *
src_list_from_rast_target(struct Parse *parser, struct rast_source_list *list);

/**
 * Create an expression of a DELETE or UPDATE statement from the given resolved
 * expression in the scope of the given source list, assigning cursors to the
 * sources of its subqueries.
 */
struct Expr *
expr_from_rast_in(struct Parse *parser, struct SrcList *src,
		  const struct rast_expr *expr);
