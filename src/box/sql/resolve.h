/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 */
#pragma once

#include "ast.h"

struct rast_with_entry {
	const char *name;
	struct rast_with_list *with_list;
	struct ast_with_entry *ast;
	bool is_used;
};

struct rast_with_list {
	struct rast_with_entry *list;
	int len;
};

struct sql_rast {
	enum sql_ast_type type;
	union {
		struct sql_ast *ast;
	};
};

/**
 * Resolve @a ast. Allocates on @a region. Returns NULL and sets diag on
 * error.
 */
struct sql_rast *
sql_resolve_ast(struct region *region, struct sql_ast *ast);
