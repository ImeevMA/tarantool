/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/decimal.h"

struct space;
struct Parse;
struct ast_select;

/**
 * Resolved description of a source of a SELECT statement. All values
 * except the cursor are already resolved during the resolve phase.
 */
struct rast_source {
	/** Space of the source. */
	struct space *space;
	/** Name of the source. */
	const char *name;
	/** Alias of the source, NULL if not set. */
	const char *alias;
	/** VDBE cursor of the source. */
	int cursor;
	/** True if scanning is not allowed for this source. */
	bool disallow_scan;
};

/** Kind of a result column of a simple SELECT statement. */
enum rast_column_kind {
	/** The column references a field of the source. */
	RAST_COLUMN_FIELD,
	/** The column is a constant value. */
	RAST_COLUMN_VALUE,
};

/** Type of a constant value of a result column. */
enum rast_value_type {
	/** Unsigned integer literal, e.g. 1. */
	RAST_VALUE_UNSIGNED,
	/** Negative integer literal, e.g. -1. */
	RAST_VALUE_INTEGER,
	/** Floating point literal, e.g. 1.5e0. */
	RAST_VALUE_DOUBLE,
	/** Decimal literal, e.g. 1.5. */
	RAST_VALUE_DECIMAL,
	/** String literal, e.g. 'abc'. */
	RAST_VALUE_STRING,
	/** Varbinary literal, e.g. x'41'. */
	RAST_VALUE_BLOB,
	/** Boolean literal: true or false. */
	RAST_VALUE_BOOLEAN,
	/** NULL literal. */
	RAST_VALUE_NULL,
};

/** A constant value of a result column of a simple SELECT statement. */
struct rast_value {
	/** Type of the value. */
	enum rast_value_type type;
	/**
	 * The value itself: which member is used depends on the type.
	 * All strings and the decimal are allocated in the region of
	 * the parsing context.
	 */
	union {
		/** RAST_VALUE_UNSIGNED. */
		uint64_t u;
		/** RAST_VALUE_INTEGER. */
		int64_t i;
		/** RAST_VALUE_DOUBLE. */
		double f;
		/** RAST_VALUE_BOOLEAN. */
		bool b;
		/** RAST_VALUE_STRING: dequoted, NUL-terminated. */
		const char *s;
		/** RAST_VALUE_BLOB. */
		struct {
			const char *z;
			uint32_t n;
		} blob;
		/** RAST_VALUE_DECIMAL. */
		decimal_t *dec;
	} v;
};

/** Resolved description of a result column of a SELECT statement. */
struct rast_column {
	/** Kind of the column: a field reference or a constant value. */
	enum rast_column_kind kind;
	/** Index of the field in the source. Used if kind is FIELD. */
	uint32_t fieldno;
	/** Value of the constant. Used if kind is VALUE. */
	struct rast_value value;
	/** Alias of the result column, NULL if not set. */
	const char *alias;
	/**
	 * Original text of the expression, NULL if the column was
	 * expanded from an asterisk.
	 */
	const char *span;
};

/**
 * Resolved description of a simple SELECT statement: a single source
 * and result columns referencing only fields of that source or
 * constant values.
 */
struct rast_select {
	/** The only source of the query. */
	struct rast_source source;
	/** Array of result columns. */
	struct rast_column *columns;
	/** Number of result columns. */
	uint32_t column_count;
};

/**
 * Resolve a simple SELECT statement into struct rast_select: a single
 * plain table source and result columns referencing only fields of
 * that source or constant values.
 *
 * @param parser Parsing context.
 * @param select AST of the SELECT statement.
 *
 * @retval rast_select Resolved description of the SELECT statement.
 * @retval NULL        The statement is not simple - code it with the
 *                     legacy pipeline. If parsing was aborted, the
 *                     statement has an error and must not be coded.
 */
struct rast_select *rast_select_resolve(struct Parse *parser,
					struct ast_select *select);

/**
 * Generate VDBE program for a SELECT statement resolved into
 * struct rast_select: a full scan of the single source emitting
 * the requested fields and constant values.
 *
 * @param parser Parsing context.
 * @param select Resolved description of the SELECT statement.
 */
void
vdbe_emit_rast_select(struct Parse *parser, struct rast_select *select);
