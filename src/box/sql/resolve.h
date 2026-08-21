/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 */
#pragma once

#include "ast.h"

struct rast_drop_index {
	uint32_t space_id;
	uint32_t index_id;
};

struct sql_rast {
	enum sql_ast_type type;
	union {
		struct sql_ast *ast;
		struct rast_drop_index drop_index;
		const char *trigger_name;
	};
};

struct sql_rast *
sql_resolve_ast(struct region *region, struct sql_ast *ast);
