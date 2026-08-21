/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 */
#pragma once

#include "ast.h"

struct sql_rast {
	enum sql_ast_type type;
	union {
		struct sql_ast *ast;
	};
};

struct sql_rast *
sql_resolve_ast(struct region *region, struct sql_ast *ast);
