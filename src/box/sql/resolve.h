/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 */
#pragma once

#include "ast.h"

struct rast_expr {
	struct ast_expr *ast;
	const char *name;
	const char *span;
	uint32_t span_len;
	enum sort_order order;
	bool autoinc;
};

struct rast_select {
	struct ast_select *ast;

	struct rast_expr *columns;
	uint32_t column_count;
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
