/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * This file contains C code routines that are called by the sql parser
 * when syntax rules are reduced.  The routines in this file handle the
 * following kinds of SQL syntax:
 *
 *     CREATE TABLE
 *     DROP TABLE
 *     CREATE INDEX
 *     DROP INDEX
 *     creating ID lists
 *     BEGIN TRANSACTION
 *     COMMIT
 *     ROLLBACK
 */
#include <ctype.h>
#include "sqlInt.h"
#include "mem.h"
#include "resolve.h"
#include "vdbeInt.h"
#include "mp_decimal.h"
#include "tarantoolInt.h"
#include "box/sequence.h"
#include "box/session.h"
#include "box/identifier.h"
#include "box/schema.h"
#include "box/tuple_format.h"
#include "box/coll_id_cache.h"
#include "box/user.h"
#include "box/session_settings.h"
#include "box/tuple_constraint_def.h"

void
sql_finish_coding(struct Parse *parse_context)
{
	assert(parse_context->pToplevel == NULL);
	struct Vdbe *v = sqlGetVdbe(parse_context);
	sqlVdbeAddOp0(v, OP_Halt);

	if (parse_context->is_aborted)
		return;
	/*
	 * Begin by generating some termination code at the end
	 * of the vdbe program
	 */
	int last_instruction = v->nOp;
	if (parse_context->initiateTTrans)
		sqlVdbeAddOp0(v, OP_TTransaction);
	if (parse_context->pConstExpr != NULL) {
		assert(sqlVdbeGetOp(v, 0)->opcode == OP_Init);
		/*
		 * Code constant expressions that where
		 * factored out of inner loops.
		 */
		struct ExprList *exprs = parse_context->pConstExpr;
		parse_context->okConstFactor = 0;
		for (int i = 0; i < exprs->nExpr; ++i) {
			sqlExprCode(parse_context, exprs->a[i].pExpr,
					exprs->a[i].u. iConstExprReg);
		}
	}
	/*
	 * Finally, jump back to the beginning of
	 * the executable code. In fact, it is required
	 * only if some additional opcodes are generated.
	 * Otherwise, it would be useless jump:
	 *
	 * 0:        OP_Init 0 vdbe_end ...
	 * 1: ...
	 *    ...
	 * vdbe_end: OP_Goto 0 1 ...
	 */
	if (parse_context->initiateTTrans ||
	    parse_context->pConstExpr != NULL) {
		sqlVdbeChangeP2(v, 0, last_instruction);
		sqlVdbeGoto(v, 1);
	}
	/* Get the VDBE program ready for execution. */
	if (!parse_context->is_aborted) {
		assert(parse_context->iCacheLevel == 0);
		sqlVdbeMakeReady(v, parse_context);
	} else {
		parse_context->is_aborted = true;
	}
}

bool
sql_space_column_is_in_pk(const struct space *space, uint32_t column)
{
	if (space->def->opts.is_view)
		return false;
	const struct index *primary_idx = space->index[0];
	assert(primary_idx != NULL);
	const struct key_def *key_def = primary_idx->def->key_def;
	uint64_t pk_mask = key_def->column_mask;
	if (column < 63)
		return (pk_mask & (((uint64_t) 1) << column)) != 0;
	else if ((pk_mask & (((uint64_t) 1) << 63)) != 0)
		return key_def_find_by_fieldno(key_def, column) != NULL;
	return false;
}

/*
 * This routine is used to check if the UTF-8 string zName is a legal
 * unqualified name for an identifier.
 * Some objects may not be checked, because they are validated in Tarantool.
 * (e.g. table, index, column name of a real table)
 * All names are legal except those that cantain non-printable
 * characters or have length greater than BOX_NAME_MAX.
 *
 * @param pParse Parser context.
 * @param zName Identifier to check.
 *
 * @retval 0 on success.
 * @retval -1 on error.
 */
int
sql_check_identifier_name(const char *name, ssize_t len)
{
	if (len > BOX_NAME_MAX) {
		diag_set(ClientError, ER_IDENTIFIER,
			 tt_cstr(name, BOX_INVALID_NAME_MAX));
		return -1;
	}
	if (identifier_check(name, len) != 0)
		return -1;
	return 0;
}

int
sqlCheckIdentifierName(Parse *pParse, char *zName)
{
	if (sql_check_identifier_name(zName, strlen(zName)) != 0) {
		pParse->is_aborted = true;
		return -1;
	}
	return 0;
}

/*
 * Begin constructing a new table representation in memory.  This is
 * the first of several action routines that get called in response
 * to a CREATE TABLE statement.  In particular, this routine is called
 * after seeing tokens "CREATE" and "TABLE" and the table name. The isTemp
 * flag is true if the table should be stored in the auxiliary database
 * file instead of in the main database file.  This is normally the case
 * when the "TEMP" or "TEMPORARY" keyword occurs in between
 * CREATE and TABLE.
 *
 * The new table record is initialized and put in pParse->new_space.
 * As more of the CREATE TABLE statement is parsed, additional action
 * routines will be called to add more information to this record.
 * At the end of the CREATE TABLE statement, the sqlEndTable() routine
 * is called to complete the construction of the new table record.
 *
 * @param pParse Parser context.
 * @param pName1 First part of the name of the table or view.
 */
struct space *
sqlStartTable(Parse *pParse, Token *pName)
{
	char *zName = 0;	/* The name of the new table */
	struct space *new_space = NULL;
	struct Vdbe *v = sqlGetVdbe(pParse);
	sqlVdbeCountChanges(v);

	zName = sql_name_from_token(pName);
	if (sqlCheckIdentifierName(pParse, zName) != 0) {
		sql_xfree(zName);
		return NULL;
	}

	new_space = sql_template_space_new(pParse, zName);
	sql_xfree(zName);
	strlcpy(new_space->def->engine_name,
		sql_storage_engine_strs[current_session()->sql_default_engine],
		ENGINE_NAME_MAX + 1);

	assert(v == sqlGetVdbe(pParse));
	return new_space;
}

char *
sql_expr_term_encode(struct region *region, struct Expr *expr,
		     uint32_t *size)
{
	char *buf = NULL;
	*size = 0;
	switch (sql_expr_type(expr)) {
	case FIELD_TYPE_BOOLEAN: {
		assert(expr->op == TK_TRUE || expr->op == TK_FALSE);
		*size = mp_sizeof_bool(expr->v.b);
		buf = xregion_alloc(region, *size);
		mp_encode_bool(buf, expr->v.b);
		break;
	}
	case FIELD_TYPE_VARBINARY: {
		assert(expr->op == TK_BLOB);
		*size = mp_sizeof_bin(expr->v.n);
		buf = xregion_alloc(region, *size);
		mp_encode_bin(buf, expr->v.z, expr->v.n);
		break;
	}
	case FIELD_TYPE_STRING: {
		const char *val = expr->v.s;
		uint32_t len = expr->v.len;
		*size = mp_sizeof_str(len);
		buf = xregion_alloc(region, *size);
		mp_encode_str(buf, val, len);
		break;
	}
	case FIELD_TYPE_DOUBLE:
		assert(expr->op == TK_FLOAT && !sqlIsNaN(expr->v.f));
		*size = mp_sizeof_double(expr->v.f);
		buf = xregion_alloc(region, *size);
		mp_encode_double(buf, expr->v.f);
		break;
	case FIELD_TYPE_DECIMAL:
		assert(expr->op == TK_DECIMAL);
		*size = mp_sizeof_decimal(expr->v.d);
		buf = xregion_alloc(region, *size);
		mp_encode_decimal(buf, expr->v.d);
		break;
	case FIELD_TYPE_INTEGER: {
		assert(expr->op == TK_INTEGER);
		if ((expr->flags & EP_Negative) != 0) {
			int64_t val = (int64_t)expr->v.u;
			assert(val < 0);
			*size = mp_sizeof_int(val);
			buf = xregion_alloc(region, *size);
			mp_encode_int(buf, val);
			break;
		}

		uint64_t val = expr->v.u;
		*size = mp_sizeof_uint(val);
		buf = xregion_alloc(region, *size);
		mp_encode_uint(buf, val);
		break;
	}
	default:
		assert(sql_expr_type(expr) == FIELD_TYPE_SCALAR);
		assert(expr->op == TK_NULL || expr->op == TK_UNKNOWN);
		assert(buf == NULL);
		assert(*size == 0);
	}
	return buf;
}

/**
 * Prepare a 0-terminated string in the wptr memory buffer that
 * does not contain a sequence of more than one whatespace
 * character. Routine enforces ' ' (space) as whitespace
 * delimiter. When character ' or " was met, the string is copied
 * without any changes until the next ' or " sign.
 * The wptr buffer is expected to have str_len + 1 bytes
 * (this is the expected scenario where no extra whitespace
 * characters in the source string).
 * @param wptr The destination memory buffer of size
 *             @a str_len + 1.
 * @param str The source string to be copied.
 * @param str_len The source string @a str length.
 */
static void
trim_space_snprintf(char *wptr, const char *str, uint32_t str_len)
{
	const char *str_end = str + str_len;
	char quote_type = '\0';
	bool is_prev_chr_space = false;
	while (str < str_end) {
		if (quote_type == '\0') {
			if (*str == '\'' || *str == '\"') {
				quote_type = *str;
			} else if (isspace((unsigned char)*str)) {
				if (!is_prev_chr_space)
					*wptr++ = ' ';
				is_prev_chr_space = true;
				str++;
				continue;
			}
		} else if (*str == quote_type) {
			quote_type = '\0';
		}
		is_prev_chr_space = false;
		*wptr++ = *str++;
	}
	*wptr = '\0';
}

static void
vdbe_emit_ck_constraint_create(struct Parse *parser,
			       const struct ck_constraint_def *ck_def,
			       uint32_t reg_space_id, const char *space_name);

/**
 * Calculate check constraint definition memory size and fields
 * offsets for given arguments.
 *
 * Alongside with struct ck_constraint_def itself, we reserve
 * memory for string containing its name and expression string.
 *
 * Memory layout:
 * +-----------------------------+ <- Allocated memory starts here
 * |   struct ck_constraint_def  |
 * |-----------------------------|
 * |          name + \0          |
 * |-----------------------------|
 * |        expr_str + \0        |
 * +-----------------------------+
 *
 * @param name_len The length of the name.
 * @param expr_str_len The length of the expr_str.
 * @param[out] expr_str_offset The offset of the expr_str string.
 * @return The size of the ck constraint definition object for
 *         given parameters.
 */
static inline uint32_t
ck_constraint_def_sizeof(uint32_t name_len, uint32_t expr_str_len,
			 uint32_t *expr_str_offset)
{
	*expr_str_offset = sizeof(struct ck_constraint_def) + name_len + 1;
	return *expr_str_offset + expr_str_len + 1;
}

struct ck_constraint_parse *
ck_constraint_parse_new(struct Parse *parser, const char *name,
			const char *expr_str, uint32_t expr_str_len)
{
	size_t name_len = strlen(name);
	/*
	 * Allocate memory for ck constraint parse structure and
	 * ck constraint definition as a single memory chunk on
	 * region:
	 *
	 *    [ck_parse][ck_def[name][expr_str]]
	 *         |_____^  |_________^
	 */
	uint32_t expr_str_offset;
	uint32_t ck_def_sz = ck_constraint_def_sizeof(name_len, expr_str_len,
						      &expr_str_offset);
	struct region *region = &parser->region;
	struct ck_constraint_parse *ck_parse;
	size_t total = sizeof(*ck_parse) + ck_def_sz;
	ck_parse = xregion_aligned_alloc(region, total, alignof(*ck_parse));
	struct ck_constraint_def *ck_def =
		(struct ck_constraint_def *)((char *)ck_parse +
					     sizeof(*ck_parse));
	static_assert(alignof(*ck_def) == alignof(*ck_parse),
		      "allocated in one block and should have the same "
		      "alignment");
	ck_parse->ck_def = ck_def;
	rlist_create(&ck_parse->link);
	ck_def->is_field_ck = false;
	ck_def->fieldno = 0;
	ck_def->expr_str = (char *)ck_def + expr_str_offset;
	ck_def->space_id = BOX_ID_NIL;
	trim_space_snprintf(ck_def->expr_str, expr_str, expr_str_len);
	memcpy(ck_def->name, name, name_len);
	ck_def->name[name_len] = '\0';
	return ck_parse;
}

struct coll *
sql_column_collation(struct space_def *def, uint32_t column, uint32_t *coll_id)
{
	assert(def != NULL);
	assert(column < (uint32_t)def->field_count);
	*coll_id = def->fields[column].coll_id;
	struct coll_id *collation = coll_by_id(*coll_id);
	return collation != NULL ? collation->coll : NULL;
}

void
vdbe_emit_open_cursor(struct Parse *parse_context, int cursor,
		      uint32_t index_id, const struct space *space)
{
	assert(space != NULL);
	const struct index *idx = NULL;
	if (index_id <= space->index_id_max)
		idx = space->index_map[index_id];
	if (idx == NULL) {
		diag_set(ClientError, ER_NO_SUCH_INDEX_ID, index_id,
			 space->def->name);
		parse_context->is_aborted = true;
		return;
	}
	if (idx->def->type != TREE) {
		diag_set(ClientError, ER_UNSUPPORTED, "SQL",
			 "using non-TREE index type. Please, use " \
			 "INDEXED BY clause to force using proper index.");
		parse_context->is_aborted = true;
		return;
	}
	struct Vdbe *vdbe = parse_context->pVdbe;
	int reg = ++parse_context->nMem;
	sqlVdbeAddOp2(vdbe, OP_OpenSpace, reg, space->def->id);
	sqlVdbeAddOp3(vdbe, OP_IteratorOpen, cursor, index_id, reg);
}

/*
 * Generate code to determine the new space Id.
 * Fetch the max space id seen so far from _schema and increment it.
 * Return register storing the result.
 */
static int
getNewSpaceId(Parse * pParse)
{
	Vdbe *v = sqlGetVdbe(pParse);
	int iRes = ++pParse->nMem;

	sqlVdbeAddOp1(v, OP_GenSpaceid, iRes);
	return iRes;
}

/**
 * Generate VDBE code to create an Index. This is accomplished by
 * adding an entry to the _index table.
 *
 * @param parse Current parsing context.
 * @param def Definition of space which index belongs to.
 * @param idx_def Definition of index under construction.
 * @param pk_def Definition of primary key index.
 * @param space_id_reg Register containing generated space id.
 * @param index_id_reg Register containing generated index id.
 */
static void
vdbe_emit_create_index(struct Parse *parse, struct space_def *def,
		       const struct index_def *idx_def, int space_id_reg,
		       int index_id_reg)
{
	struct Vdbe *v = sqlGetVdbe(parse);
	int entry_reg = ++parse->nMem;
	/*
	 * Entry in _index space contains 6 fields.
	 * The last one contains encoded tuple.
	 */
	int tuple_reg = (parse->nMem += 6);
	/* Format "opts" and "parts" for _index entry. */
	struct region *region = &parse->region;
	uint32_t index_opts_sz = 0;
	char *index_opts = sql_encode_index_opts(region, &idx_def->opts,
						 &index_opts_sz);
	if (index_opts == NULL)
		goto error;
	uint32_t index_parts_sz = 0;
	char *index_parts = sql_encode_index_parts(region, def->fields, idx_def,
						   &index_parts_sz);
	if (index_parts == NULL)
		goto error;
	char *raw = sql_xmalloc(index_opts_sz + index_parts_sz);
	memcpy(raw, index_opts, index_opts_sz);
	index_opts = raw;
	raw += index_opts_sz;
	memcpy(raw, index_parts, index_parts_sz);
	index_parts = raw;

	if (parse->new_space != NULL || parse->space != NULL) {
		sqlVdbeAddOp2(v, OP_SCopy, space_id_reg, entry_reg);
		sqlVdbeAddOp2(v, OP_Integer, idx_def->iid, entry_reg + 1);
	} else {
		/*
		 * An existing table is being modified;
		 * space_id_reg is literal, but index_id_reg is
		 * register.
		 */
		sqlVdbeAddOp2(v, OP_Integer, space_id_reg, entry_reg);
		sqlVdbeAddOp2(v, OP_SCopy, index_id_reg, entry_reg + 1);
	}
	sqlVdbeAddOp4(v, OP_String8, 0, entry_reg + 2, 0,
		      sql_xstrdup(idx_def->name), P4_DYNAMIC);
	sqlVdbeAddOp4(v, OP_String8, 0, entry_reg + 3, 0, "tree",
			  P4_STATIC);
	sqlVdbeAddOp4(v, OP_Blob, index_opts_sz, entry_reg + 4,
			  SQL_SUBTYPE_MSGPACK, index_opts, P4_DYNAMIC);
	/* opts and parts are co-located, hence STATIC. */
	sqlVdbeAddOp4(v, OP_Blob, index_parts_sz, entry_reg + 5,
			  SQL_SUBTYPE_MSGPACK, index_parts, P4_STATIC);
	sqlVdbeAddOp3(v, OP_MakeRecord, entry_reg, 6, tuple_reg);
	sqlVdbeAddOp2(v, OP_SInsert, BOX_INDEX_ID, tuple_reg);
	return;
error:
	parse->is_aborted = true;

}

/**
 * Generate code to create a new space.
 *
 * @param space_id_reg is a register storing the id of the space.
 * @param table Table containing meta-information of space to be
 *              created.
 */
static void
vdbe_emit_space_create(struct Parse *pParse, int space_id_reg,
		       int space_name_reg, struct space *space)
{
	Vdbe *v = sqlGetVdbe(pParse);
	int iFirstCol = ++pParse->nMem;
	int tuple_reg = (pParse->nMem += 7);
	struct region *region = &pParse->region;
	uint32_t table_opts_stmt_sz = 0;
	char *table_opts_stmt = sql_encode_table_opts(region, space->def,
						      &table_opts_stmt_sz);
	if (table_opts_stmt == NULL)
		goto error;
	uint32_t table_stmt_sz = 0;
	char *table_stmt = sql_encode_table(region, space->def, &table_stmt_sz);
	if (table_stmt == NULL)
		goto error;
	char *raw = sql_xmalloc(table_stmt_sz + table_opts_stmt_sz);
	memcpy(raw, table_opts_stmt, table_opts_stmt_sz);
	table_opts_stmt = raw;
	raw += table_opts_stmt_sz;
	memcpy(raw, table_stmt, table_stmt_sz);
	table_stmt = raw;

	sqlVdbeAddOp2(v, OP_SCopy, space_id_reg, iFirstCol /* spaceId */ );
	sqlVdbeAddOp2(v, OP_Integer, effective_user()->uid,
			  iFirstCol + 1 /* owner */ );
	sqlVdbeAddOp2(v, OP_SCopy, space_name_reg, iFirstCol + 2);
	sqlVdbeAddOp4(v, OP_String8, 0, iFirstCol + 3 /* engine */ , 0,
		      sql_xstrdup(space->def->engine_name), P4_DYNAMIC);
	sqlVdbeAddOp2(v, OP_Integer, space->def->field_count,
			  iFirstCol + 4 /* field_count */ );
	sqlVdbeAddOp4(v, OP_Blob, table_opts_stmt_sz, iFirstCol + 5,
			  SQL_SUBTYPE_MSGPACK, table_opts_stmt, P4_DYNAMIC);
	/* zOpts and zFormat are co-located, hence STATIC */
	sqlVdbeAddOp4(v, OP_Blob, table_stmt_sz, iFirstCol + 6,
			  SQL_SUBTYPE_MSGPACK, table_stmt, P4_STATIC);
	sqlVdbeAddOp3(v, OP_MakeRecord, iFirstCol, 7, tuple_reg);
	sqlVdbeAddOp2(v, OP_SInsert, BOX_SPACE_ID, tuple_reg);
	sqlVdbeChangeP5(v, OPFLAG_NCHANGE);
	return;
error:
	pParse->is_aborted = true;
}

int
emitNewSysSequenceRecord(Parse *pParse, int reg_seq_id, const char *seq_name)
{
	Vdbe *v = sqlGetVdbe(pParse);
	int first_col = pParse->nMem + 1;
	pParse->nMem += 10; /* 9 fields + new record pointer  */

	const long long int min_usigned_long_long = 0;
	const long long int max_usigned_long_long = LLONG_MAX;

	/* 1. New sequence id  */
	sqlVdbeAddOp2(v, OP_SCopy, reg_seq_id, first_col + 1);
	/* 2. user is  */
	sqlVdbeAddOp2(v, OP_Integer, effective_user()->uid, first_col + 2);
	/* 3. New sequence name  */
	sqlVdbeAddOp4(v, OP_String8, 0, first_col + 3, 0, sql_xstrdup(seq_name),
		      P4_DYNAMIC);

	/* 4. Step  */
	sqlVdbeAddOp2(v, OP_Integer, 1, first_col + 4);

	/* 5. Minimum  */
	sql_vdbe_add_op4_uint64(v, 0, first_col + 5, 0, min_usigned_long_long);
	/* 6. Maximum  */
	sql_vdbe_add_op4_uint64(v, 0, first_col + 6, 0, max_usigned_long_long);
	/* 7. Start  */
	sqlVdbeAddOp2(v, OP_Integer, 1, first_col + 7);

	/* 8. Cache  */
	sqlVdbeAddOp2(v, OP_Integer, 0, first_col + 8);

	/* 9. Cycle  */
	sqlVdbeAddOp2(v, OP_Bool, false, first_col + 9);

	sqlVdbeAddOp3(v, OP_MakeRecord, first_col + 1, 9, first_col);

	return first_col;
}

static int
emitNewSysSpaceSequenceRecord(Parse *pParse, int reg_space_id, int reg_seq_id,
			      uint32_t fieldno)
{
	Vdbe *v = sqlGetVdbe(pParse);
	int first_col = pParse->nMem + 1;
	pParse->nMem += 6; /* 5 fields + new record pointer  */

	/* 1. Space id  */
	sqlVdbeAddOp2(v, OP_SCopy, reg_space_id, first_col + 1);
	
	/* 2. Sequence id  */
	sqlVdbeAddOp2(v, OP_SCopy, reg_seq_id, first_col + 2);

	/* 3. Autogenerated. */
	sqlVdbeAddOp2(v, OP_Bool, true, first_col + 3);

	/* 4. Field id. */
	sqlVdbeAddOp2(v, OP_Integer, fieldno, first_col + 4);

	/* 5. Field path. */
	sqlVdbeAddOp4(v, OP_String8, 0, first_col + 5, 0, "", P4_STATIC);

	sqlVdbeAddOp3(v, OP_MakeRecord, first_col + 1, 5, first_col);
	return first_col;
}

/**
 * Emit code to create a new function with the given name and body and copy the
 * new function ID into the given register.
 */
static void
vdbe_emit_create_function(struct Parse *parser, int reg_id, const char *name,
			  const char *body)
{
	/*
	 * Occupy registers for 21 fields: each member in _func space plus one
	 * for final msgpack tuple.
	 */
	const int reg_count = box_func_field_MAX + 1;
	int regs = sqlGetTempRange(parser, reg_count);
	int name_reg = regs + BOX_FUNC_FIELD_NAME;
	struct Vdbe *v = sqlGetVdbe(parser);
	sqlVdbeAddOp4(v, OP_String8, 0, name_reg, 0, sql_xstrdup(name),
		      P4_DYNAMIC);
	vdbe_emit_halt_with_presence_test(parser, BOX_FUNC_ID,
					  BOX_FUNC_FIELD_NAME, name_reg, 1,
					  ER_FUNCTION_EXISTS, name, false,
					  OP_NoConflict);
	sqlVdbeAddOp2(v, OP_GenFuncId, name_reg, regs + BOX_FUNC_FIELD_ID);
	sqlVdbeAddOp2(v, OP_Integer, effective_user()->uid,
		      regs + BOX_FUNC_FIELD_UID);
	sqlVdbeAddOp2(v, OP_Integer, 0, regs + BOX_FUNC_FIELD_SETUID);
	sqlVdbeAddOp4(v, OP_String8, 0, regs + BOX_FUNC_FIELD_LANGUAGE, 0,
		      "SQL_EXPR", P4_STATIC);
	sqlVdbeAddOp4(v, OP_String8, 0, regs + BOX_FUNC_FIELD_BODY, 0,
		      sql_xstrdup(body), P4_DYNAMIC);
	sqlVdbeAddOp4(v, OP_String8, 0, regs + BOX_FUNC_FIELD_ROUTINE_TYPE, 0,
		      "function", P4_STATIC);
	char *param_list = sql_xmalloc(32);
	char *buf = mp_encode_array(param_list, 0);
	sqlVdbeAddOp4(v, OP_Blob, buf - param_list,
		      regs + BOX_FUNC_FIELD_PARAM_LIST, SQL_SUBTYPE_MSGPACK,
		      param_list, P4_DYNAMIC);
	sqlVdbeAddOp4(v, OP_String8, 0, regs + BOX_FUNC_FIELD_RETURNS, 0, "any",
		      P4_STATIC);
	sqlVdbeAddOp4(v, OP_String8, 0, regs + BOX_FUNC_FIELD_AGGREGATE, 0,
		      "none", P4_STATIC);
	sqlVdbeAddOp4(v, OP_String8, 0, regs + BOX_FUNC_FIELD_SQL_DATA_ACCESS,
		      0, "none", P4_STATIC);
	sqlVdbeAddOp2(v, OP_Bool, true, regs + BOX_FUNC_FIELD_IS_DETERMINISTIC);
	sqlVdbeAddOp2(v, OP_Bool, true, regs + BOX_FUNC_FIELD_IS_SANDBOXED);
	sqlVdbeAddOp2(v, OP_Bool, true, regs + BOX_FUNC_FIELD_IS_NULL_CALL);
	char *exports = buf;
	buf = mp_encode_array(exports, 1);
	buf = mp_encode_str0(buf, "LUA");
	sqlVdbeAddOp4(v, OP_Blob, buf - exports, regs + BOX_FUNC_FIELD_EXPORTS,
		      SQL_SUBTYPE_MSGPACK, exports, P4_STATIC);
	char *opts = buf;
	buf = mp_encode_map(opts, 0);
	sqlVdbeAddOp4(v, OP_Blob, buf - opts, regs + BOX_FUNC_FIELD_OPTS,
		      SQL_SUBTYPE_MSGPACK, opts, P4_STATIC);
	sqlVdbeAddOp4(v, OP_String8, 0, regs + BOX_FUNC_FIELD_COMMENT, 0, "",
		      P4_STATIC);
	sqlVdbeAddOp4(v, OP_String8, 0, regs + BOX_FUNC_FIELD_CREATED, 0, "",
		      P4_STATIC);
	sqlVdbeAddOp4(v, OP_String8, 0, regs + BOX_FUNC_FIELD_LAST_ALTERED, 0,
		      "", P4_STATIC);
	char *trigger = buf;
	buf = mp_encode_array(trigger, 0);
	sqlVdbeAddOp4(v, OP_Blob, buf - trigger,
		      regs + BOX_FUNC_FIELD_TRIGGER, SQL_SUBTYPE_MSGPACK,
		      trigger, P4_STATIC);
	sqlVdbeAddOp3(v, OP_MakeRecord, regs, box_func_field_MAX,
		      regs + box_func_field_MAX);
	sqlVdbeAddOp2(v, OP_SInsert, BOX_FUNC_ID, regs + box_func_field_MAX);
	sqlVdbeAddOp2(v, OP_SCopy, regs + BOX_FUNC_FIELD_ID, reg_id);
	sqlReleaseTempRange(parser, regs, reg_count);
}

/**
 * Generate opcodes to serialize check constraint definition into
 * MsgPack and insert produced tuple into _ck_constraint space.
 * @param parser Parsing context.
 * @param ck_def Check constraint definition to be serialized.
 * @param reg_space_id The VDBE register containing space id.
 * @param space_name Name of the space owning the CHECK. For error
 *     message.
 */
static void
vdbe_emit_ck_constraint_create(struct Parse *parser,
			       const struct ck_constraint_def *ck_def,
			       uint32_t reg_space_id, const char *space_name)
{
	struct Vdbe *v = sqlGetVdbe(parser);
	assert(v != NULL);
	char *name = sqlMPrintf("check_%s_%s", space_name, ck_def->name);
	int reg_id = ++parser->nMem;
	vdbe_emit_create_function(parser, reg_id, name, ck_def->expr_str);
	sql_xfree(name);
	VdbeComment((v, "Create func constraint %s", ck_def->name));
	sqlVdbeAddOp4(v, OP_CreateCheck, reg_space_id, reg_id, ck_def->fieldno,
		      sql_xstrdup(ck_def->name), P4_DYNAMIC);
	sqlVdbeChangeP5(v, ck_def->is_field_ck);
	sqlVdbeCountChanges(v);
}

/**
 * Generate opcodes to create foreign key constraint.
 *
 * @param parse_context Parsing context.
 * @param fk Foreign key to be created.
 * @param is_alter_add_constr True if the constraint is added to an existing
 *        space by ALTER TABLE ADD CONSTRAINT, then the IDs of the spaces are
 *        literals, otherwise the IDs are in registers.
 */
static void
vdbe_emit_fk_constraint_create(struct Parse *parse_context,
			       const struct fk_constraint_def *fk,
			       bool is_alter_add_constr)
{
	assert(parse_context != NULL);
	assert(fk != NULL);
	struct Vdbe *vdbe = sqlGetVdbe(parse_context);
	assert(vdbe != NULL);
	/*
	 * Take 3 registers for the table constraint and 4 registers for the
	 * field constraint. The first register contains the child_id, the
	 * second register contains the parent_id. If the created constraint is
	 * a table constraint, the third register contains the encoded mapping.
	 * Otherwise, the third register contains the field number of the child
	 * column, and the fourth one contains the field number of the parent
	 * column.
	 */
	int regs = sqlGetTempRange(parse_context, fk->is_field_fk ? 4 : 3);
	/*
	 * In case we are adding FK constraints during execution
	 * of <CREATE TABLE ...> or <ALTER TABLE ADD COLUMN>
	 * statement, we don't have child id (we know it, but
	 * fk->child_id stores register because of code reuse in
	 * vdbe_emit_create_constraints()), but we know register
	 * where it will be stored.
	 */
	if (!is_alter_add_constr)
		sqlVdbeAddOp2(vdbe, OP_SCopy, fk->child_id, regs);
	else
		sqlVdbeAddOp2(vdbe, OP_Integer, fk->child_id, regs);
	if (!is_alter_add_constr && fk->child_id == fk->parent_id)
		sqlVdbeAddOp2(vdbe, OP_SCopy, fk->parent_id, regs + 1);
	else
		sqlVdbeAddOp2(vdbe, OP_Integer, fk->parent_id, regs + 1);
	if (fk->is_field_fk) {
		sqlVdbeAddOp2(vdbe, OP_Integer, fk->links->child_field,
			      regs + 2);
		sqlVdbeAddOp2(vdbe, OP_Integer, fk->links->parent_field,
			      regs + 3);
	} else {
		uint32_t size;
		char *raw = fk_constraint_encode_links(fk, &size);
		sqlVdbeAddOp4(vdbe, OP_Blob, size, regs + 2,
			      SQL_SUBTYPE_MSGPACK, raw, P4_DYNAMIC);
	}
	sqlVdbeAddOp4(vdbe, OP_CreateForeignKey, regs, 0, 0,
		      sql_xstrdup(fk->name), P4_DYNAMIC);
	sqlReleaseTempRange(parse_context, regs, fk->is_field_fk ? 4 : 3);
	sqlVdbeCountChanges(vdbe);
}

/**
 * Emit code to create the indexes, the sequence, and the FOREIGN KEY and CHECK
 * constraints of the given resolved CREATE TABLE or ALTER TABLE ADD COLUMN
 * statement. The ID of the space is in the register with the given number.
 * For ALTER TABLE ADD COLUMN, only the new indexes are created.
 */
static void
vdbe_emit_create_constraints(struct Parse *parse,
			     struct rast_create_table *stmt,
			     int reg_space_id, bool is_alter)
{
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
	return;
#endif /* FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION */
	assert(reg_space_id != 0);
	struct space *space = stmt->space;
	uint32_t i = 0;
	if (is_alter) {
		assert(space->def->id != 0);
		i = space_by_id(space->def->id)->index_count;
	}
	for (; i < space->index_count; ++i) {
		struct index *idx = space->index[i];
		vdbe_emit_create_index(parse, space->def, idx->def,
				       reg_space_id, idx->def->iid);
	}

	/*
	 * Check to see if we need to create an _sequence table
	 * for keeping track of autoincrement keys.
	 */
	if (stmt->autoinc_fieldno != UINT32_MAX) {
		/* Do an insertion into _sequence. */
		int reg_seq_id = ++parse->nMem;
		struct Vdbe *v = sqlGetVdbe(parse);
		assert(v != NULL);
		sqlVdbeAddOp2(v, OP_NextSequenceId, 0, reg_seq_id);
		int reg_seq_rec = emitNewSysSequenceRecord(parse, reg_seq_id,
							   space->def->name);
		if (is_alter) {
			int errcode = ER_SQL_CANT_ADD_AUTOINC;
			vdbe_emit_halt_with_presence_test(parse,
							  BOX_SEQUENCE_ID, 2,
							  reg_seq_rec + 3, 1,
							  errcode,
							  space->def->name,
							  false, OP_NoConflict);
		}
		sqlVdbeAddOp2(v, OP_SInsert, BOX_SEQUENCE_ID, reg_seq_rec);
		/* Do an insertion into _space_sequence. */
		int reg_space_seq_record =
			emitNewSysSpaceSequenceRecord(parse, reg_space_id,
						      reg_seq_id,
						      stmt->autoinc_fieldno);
		sqlVdbeAddOp2(v, OP_SInsert, BOX_SPACE_SEQUENCE_ID,
			      reg_space_seq_record);
	}

	struct rast_fk *fk;
	rlist_foreach_entry(fk, &stmt->fkeys, link) {
		if (fk->is_self_referenced)
			fk->def->parent_id = reg_space_id;
		fk->def->child_id = reg_space_id;
		vdbe_emit_fk_constraint_create(parse, fk->def, false);
	}

	struct ck_constraint_parse *ck_parse;
	rlist_foreach_entry(ck_parse, &stmt->checks, link) {
		vdbe_emit_ck_constraint_create(parse, ck_parse->ck_def,
					       reg_space_id, space->def->name);
	}
}

/**
 * Emit code to create the functions computing the DEFAULTs of the given
 * resolved CREATE TABLE or ALTER TABLE ADD COLUMN statement. Return the
 * numbers of the registers with the IDs of the functions.
 */
static int *
vdbe_emit_create_default_funcs(struct Parse *parser,
			       struct rast_create_table *stmt)
{
	uint32_t count = 0;
	struct rast_default_func *func;
	stailq_foreach_entry(func, &stmt->default_funcs, link)
		++count;
	if (count == 0)
		return NULL;
	int *regs = xregion_alloc_array(&parser->region, int, count);
	const struct space_def *def = stmt->space->def;
	uint32_t i = 0;
	stailq_foreach_entry(func, &stmt->default_funcs, link) {
		char *name = sqlMPrintf("default_%s_%s", def->name,
					def->fields[func->fieldno].name);
		char *body = sql_xstrndup(func->body.z, func->body.n);
		regs[i] = ++parser->nMem;
		vdbe_emit_create_function(parser, regs[i++], name, body);
		sql_xfree(name);
		sql_xfree(body);
	}
	return regs;
}

/**
 * Emit code to set the functions with the IDs in the given registers as the
 * DEFAULTs of the fields of the space with the ID in the register with the
 * given number.
 */
static void
vdbe_emit_set_default_funcs(struct Parse *parser,
			    struct rast_create_table *stmt,
			    const int *regs, int reg_space_id)
{
	struct Vdbe *v = sqlGetVdbe(parser);
	struct rast_default_func *func;
	uint32_t i = 0;
	stailq_foreach_entry(func, &stmt->default_funcs, link) {
		sqlVdbeAddOp3(v, OP_AddFuncDefault, reg_space_id, regs[i++],
			      func->fieldno);
	}
}

void
sql_create_view(struct Parse *parse_context, const char *sql,
		struct Token *name, struct ExprList *aliases,
		struct Select *view_select, bool if_not_exists)
{
	if (parse_context->nVar > 0) {
		diag_set(ClientError, ER_SQL_PARSER_GENERIC,
			 "Parameters are not allowed in views");
		parse_context->is_aborted = true;
		goto create_view_fail;
	}
	struct space *space = sqlStartTable(parse_context, name);
	if (space == NULL || parse_context->is_aborted)
		goto create_view_fail;

	/* Find result of SELECT. */
	struct Select *select = view_select;
	uint32_t saved_flags = parse_context->sql_flags;
	parse_context->sql_flags = 0;
	sqlSelectPrep(parse_context, select, 0);
	if (parse_context->is_aborted)
		goto create_view_fail;
	while (select->pPrior)
		select = select->pPrior;
	parse_context->sql_flags = saved_flags;

	struct ExprList *columns = aliases;
	if (columns != NULL) {
		if (select->pEList->nExpr != columns->nExpr) {
			diag_set(ClientError, ER_CREATE_SPACE, space->def->name,
				 "number of aliases doesn't match provided "\
				 "columns");
			parse_context->is_aborted = true;
			goto create_view_fail;
		}
		select = view_select;
	} else {
		columns = select->pEList;
	}
	sqlColumnsFromExprList(parse_context, columns, space->def);
	sqlSelectAddColumnTypeAndCollation(parse_context, space->def, select);

	space->def->opts.is_view = true;
	space->def->opts.sql = xstrdup(sql);
	const char *space_name = sql_name_from_token(name);
	int name_reg = ++parse_context->nMem;
	sqlVdbeAddOp4(parse_context->pVdbe, OP_String8, 0, name_reg, 0,
		      space_name, P4_DYNAMIC);
	vdbe_emit_halt_with_presence_test(parse_context, BOX_SPACE_ID, 2,
					  name_reg, 1, ER_SPACE_EXISTS,
					  space_name, if_not_exists,
					  OP_NoConflict);
	vdbe_emit_space_create(parse_context, getNewSpaceId(parse_context),
			       name_reg, space);
	free(space->def->opts.sql);

 create_view_fail:
	sql_expr_list_delete(aliases);
	sql_select_delete(view_select);
	return;
}

int
sql_view_assign_cursors(struct Parse *parse, const char *view_stmt)
{
	assert(view_stmt != NULL);
	struct Select *select = sql_view_compile(view_stmt);
	if (select == NULL)
		return -1;
	sqlSrcListAssignCursors(parse, select->pSrc);
	sql_select_delete(select);
	return 0;
}

/**
 * Generate VDBE program to revoke all
 * privileges associated with the given object.
 *
 * @param parser Parsing context.
 * @param object_type Object type.
 * @param object_id Object id.
 * @param access Access array associated with an object.
 */
static void
vdbe_emit_revoke_object(struct Parse *parser, const char *object_type,
			uint32_t object_id, const struct access *access)
{
	struct Vdbe *v = sqlGetVdbe(parser);
	assert(v != NULL);
	/*
	 * Get uid of users through access array
	 * and generate code to delete corresponding
	 * entries from _priv.
	 */
	int key_reg = sqlGetTempRange(parser, 4);
	bool had_grants = false;
	for (uint8_t token = 0; token < BOX_USER_MAX; ++token) {
		if (!access[token].granted)
			continue;
		had_grants = true;
		const struct user *user = user_find_by_token(token);
		sqlVdbeAddOp2(v, OP_Integer, user->def->uid, key_reg);
		sqlVdbeAddOp4(v, OP_String8, 0, key_reg + 1, 0,
			      object_type, P4_STATIC);
		sqlVdbeAddOp2(v, OP_Integer, object_id, key_reg + 2);
		sqlVdbeAddOp3(v, OP_MakeRecord, key_reg, 3, key_reg + 3);
		sqlVdbeAddOp2(v, OP_SDelete, BOX_PRIV_ID, key_reg + 3);
	}
	if (had_grants)
		VdbeComment((v, "Remove %s grants", object_type));
	sqlReleaseTempRange(parser, key_reg, 4);
}

/**
 * Generate code to drop a table.
 * This routine includes dropping triggers, sequences,
 * all indexes and entry from _space space.
 *
 * @param parse_context Current parsing context.
 * @param space Space to be dropped.
 * @param is_view True, if space is
 */
static void
sql_code_drop_table(struct Parse *parse_context, const struct space *space,
		    bool is_view)
{
	struct Vdbe *v = sqlGetVdbe(parse_context);
	assert(v != NULL);
	/*
	 * Remove all grants associated with
	 * the table being dropped.
	 */
	vdbe_emit_revoke_object(parse_context, "space", space->def->id,
				space->access);
	/*
	 * Drop all triggers associated with the table being
	 * dropped. Code is generated to remove entries from
	 * _trigger. on_replace_dd_trigger will remove it from
	 * internal SQL structures.
	 *
	 * Do not account triggers deletion - they will be
	 * accounted in DELETE from _space below.
	 */
	const struct sql_trigger *trigger = space->sql_triggers;
	while (trigger != NULL) {
		vdbe_code_drop_trigger(parse_context, trigger->zName, false);
		trigger = trigger->next;
	}
	/*
	 * Remove any entries from the _sequence_data, _sequence
	 * and _space_sequence spaces associated with the table
	 * being dropped. This is done before the table is dropped
	 * from internal schema.
	 */
	int idx_rec_reg = ++parse_context->nMem;
	int space_id_reg = ++parse_context->nMem;
	int index_id_reg = ++parse_context->nMem;
	int space_id = space->def->id;
	sqlVdbeAddOp2(v, OP_Integer, space_id, space_id_reg);
	sqlVdbeAddOp1(v, OP_CheckViewReferences, space_id_reg);
	if (space->sequence != NULL) {
		/* Delete entry from _space_sequence. */
		sqlVdbeAddOp3(v, OP_MakeRecord, space_id_reg, 1,
				  idx_rec_reg);
		sqlVdbeAddOp2(v, OP_SDelete, BOX_SPACE_SEQUENCE_ID,
				  idx_rec_reg);
		VdbeComment((v, "Delete entry from _space_sequence"));
		if (space->sequence->is_generated) {
			/* Delete entry from _sequence_data. */
			int sequence_id_reg = ++parse_context->nMem;
			sqlVdbeAddOp2(v, OP_Integer, space->sequence->def->id,
				      sequence_id_reg);
			sqlVdbeAddOp3(v, OP_MakeRecord, sequence_id_reg, 1,
				      idx_rec_reg);
			sqlVdbeAddOp2(v, OP_SDelete, BOX_SEQUENCE_DATA_ID,
				      idx_rec_reg);
			VdbeComment((v, "Delete entry from _sequence_data"));
			/* Delete entries from _priv */
		        vdbe_emit_revoke_object(parse_context, "sequence",
						space->sequence->def->id,
						space->sequence->access);
			/* Delete entry by id from _sequence. */
			sqlVdbeAddOp3(v, OP_MakeRecord, sequence_id_reg, 1,
				      idx_rec_reg);
			sqlVdbeAddOp2(v, OP_SDelete, BOX_SEQUENCE_ID,
				      idx_rec_reg);
			VdbeComment((v, "Delete entry from _sequence"));
		}
	}
	/*
	 * Drop all _space and _index entries that refer to the
	 * table.
	 */
	if (!is_view) {
		uint32_t index_count = space->index_count;
		if (index_count > 1) {
			/*
			 * Remove all indexes, except for primary.
			 * Tarantool won't allow remove primary when
			 * secondary exist.
			 */
			for (uint32_t i = 1; i < index_count; ++i) {
				sqlVdbeAddOp2(v, OP_Integer,
						  space->index[i]->def->iid,
						  index_id_reg);
				sqlVdbeAddOp3(v, OP_MakeRecord,
						  space_id_reg, 2, idx_rec_reg);
				sqlVdbeAddOp2(v, OP_SDelete, BOX_INDEX_ID,
						  idx_rec_reg);
				VdbeComment((v,
					     "Remove secondary index iid = %u",
					     space->index[i]->def->iid));
			}
		}
		sqlVdbeAddOp2(v, OP_Integer, 0, index_id_reg);
		sqlVdbeAddOp3(v, OP_MakeRecord, space_id_reg, 2,
				  idx_rec_reg);
		sqlVdbeAddOp2(v, OP_SDelete, BOX_INDEX_ID, idx_rec_reg);
		VdbeComment((v, "Remove primary index"));
	}
	/* Delete records about the space from the _truncate. */
	sqlVdbeAddOp3(v, OP_MakeRecord, space_id_reg, 1, idx_rec_reg);
	sqlVdbeAddOp2(v, OP_SDelete, BOX_TRUNCATE_ID, idx_rec_reg);
	VdbeComment((v, "Delete entry from _truncate"));
	/* Eventually delete entry from _space. */
	sqlVdbeAddOp3(v, OP_MakeRecord, space_id_reg, 1, idx_rec_reg);
	sqlVdbeAddOp2(v, OP_SDelete, BOX_SPACE_ID, idx_rec_reg);
	sqlVdbeChangeP5(v, OPFLAG_NCHANGE);
	VdbeComment((v, "Delete entry from _space"));
}

/** Emit VDBE bytecode for the given resolved DROP TABLE or DROP VIEW. */
static void
sql_emit_drop_table(struct Parse *parser, const struct rast_drop_table *stmt)
{
	struct Vdbe *v = sqlGetVdbe(parser);
	sqlVdbeCountChanges(v);
	if (stmt->space != NULL)
		sql_code_drop_table(parser, stmt->space, stmt->is_view);
}

/**
 * Alongside with struct fk_constraint_def itself, we reserve memory for
 * string containing its name and for array of links.
 * Memory layout:
 * +----------------------------------+ <- Allocated memory starts here
 * |     struct fk_constraint_def     |
 * |----------------------------------|
 * |             name + \0            |
 * |----------------------------------|
 * |       memory align padding       |
 * |----------------------------------|
 * |             links                |
 * +----------------------------------+
 */
static inline size_t
fk_constraint_def_sizeof(uint32_t link_count, uint32_t name_len,
			 uint32_t *links_offset)
{
	*links_offset = small_align(sizeof(struct fk_constraint_def) +
				    name_len + 1, alignof(struct field_link));
	return *links_offset + link_count * sizeof(struct field_link);
}

struct fk_constraint_def *
sql_fk_constraint_def_new(struct Parse *parser, const char *name,
			  uint32_t link_count)
{
	uint32_t name_len = strlen(name);
	uint32_t links_offset;
	size_t size = fk_constraint_def_sizeof(link_count, name_len,
					       &links_offset);
	struct fk_constraint_def *def =
		xregion_aligned_alloc(&parser->region, size, alignof(*def));
	def->field_count = link_count;
	def->links = (struct field_link *)((char *)def + links_offset);
	memcpy(def->name, name, name_len + 1);
	return def;
}

void
sql_drop_table_constraint(struct Parse *parser, const struct Token *table_name,
			  const struct Token *name)
{
	const struct space *space = sql_space_by_token(table_name);
	if (space == NULL) {
		const char *table_name_str = sql_tt_name_from_token(table_name);
		diag_set(ClientError, ER_NO_SUCH_SPACE, table_name_str);
		parser->is_aborted = true;
		return;
	}
	struct Vdbe *v = sqlGetVdbe(parser);

	const struct tuple_constraint_def *fk =
		sql_tuple_fk_by_token(space, name);
	if (fk != NULL) {
		sqlVdbeCountChanges(v);
		sqlVdbeAddOp4(v, OP_DropTupleForeignKey, space->def->id, 0, 0,
			      sql_xstrdup(fk->name), P4_DYNAMIC);
	}

	const struct tuple_constraint_def *ck =
		sql_tuple_ck_by_token(space, name);
	if (ck != NULL) {
		if (fk != NULL) {
			diag_set(ClientError, ER_SQL_EXECUTE,
				 tt_sprintf("ambiguous constraint name: '%s'",
					    fk->name));
			parser->is_aborted = true;
			return;
		}
		sqlVdbeCountChanges(v);
		sqlVdbeAddOp4(v, OP_DropTupleCheck, space->def->id, 0, 0,
			      sql_xstrdup(ck->name), P4_DYNAMIC);
	}

	uint32_t index_id = sql_index_id_by_token(space, name);
	if (index_id != UINT32_MAX) {
		if (fk != NULL || ck != NULL) {
			const char *str = fk != NULL ? fk->name : ck->name;
			diag_set(ClientError, ER_SQL_EXECUTE,
				 tt_sprintf("ambiguous constraint name: '%s'",
					    str));
			parser->is_aborted = true;
			return;
		}
		int regs = sqlGetTempRange(parser, 3);
		sqlVdbeCountChanges(v);
		sqlVdbeAddOp2(v, OP_Integer, space->def->id, regs);
		sqlVdbeAddOp2(v, OP_Integer, index_id, regs + 1);
		sqlVdbeAddOp3(v, OP_MakeRecord, regs, 2, regs + 2);
		sqlVdbeAddOp3(v, OP_SDelete, BOX_INDEX_ID, regs + 2, 0);
		sqlVdbeChangeP5(v, OPFLAG_NCHANGE);
		sqlReleaseTempRange(parser, regs, 3);
		return;
	}

	if (fk != NULL || ck != NULL)
		return;

	diag_set(ClientError, ER_NO_SUCH_CONSTRAINT,
		 sql_tt_name_from_token(name), space->def->name);
	parser->is_aborted = true;
}

void
sql_drop_tuple_foreign_key(struct Parse *parser, const struct Token *table_name,
			   const struct Token *name)
{
	const struct space *space = sql_space_by_token(table_name);
	if (space == NULL) {
		const char *table_name_str = sql_tt_name_from_token(table_name);
		diag_set(ClientError, ER_NO_SUCH_SPACE, table_name_str);
		parser->is_aborted = true;
		return;
	}

	const struct tuple_constraint_def *fk =
		sql_tuple_fk_by_token(space, name);
	if (fk != NULL) {
		struct Vdbe *v = sqlGetVdbe(parser);
		sqlVdbeCountChanges(v);
		sqlVdbeAddOp4(v, OP_DropTupleForeignKey, space->def->id, 0, 0,
			      sql_xstrdup(fk->name), P4_DYNAMIC);
		return;
	}

	diag_set(ClientError, ER_NO_SUCH_CONSTRAINT,
		 sql_tt_name_from_token(name), space->def->name);
	parser->is_aborted = true;
}

void
sql_drop_tuple_check(struct Parse *parser, const struct Token *table_name,
		     const struct Token *name)
{
	const struct space *space = sql_space_by_token(table_name);
	if (space == NULL) {
		const char *table_name_str = sql_tt_name_from_token(table_name);
		diag_set(ClientError, ER_NO_SUCH_SPACE, table_name_str);
		parser->is_aborted = true;
		return;
	}

	const struct tuple_constraint_def *ck =
		sql_tuple_ck_by_token(space, name);
	if (ck != NULL) {
		struct Vdbe *v = sqlGetVdbe(parser);
		sqlVdbeCountChanges(v);
		sqlVdbeAddOp4(v, OP_DropTupleCheck, space->def->id, 0, 0,
			      sql_xstrdup(ck->name), P4_DYNAMIC);
		return;
	}

	diag_set(ClientError, ER_NO_SUCH_CONSTRAINT,
		 sql_tt_name_from_token(name), space->def->name);
	parser->is_aborted = true;
}

void
sql_drop_primary_key(struct Parse *parser, const struct Token *table_name,
		     const struct Token *name)
{
	const struct space *space = sql_space_by_token(table_name);
	if (space == NULL) {
		const char *table_name_str = sql_tt_name_from_token(table_name);
		diag_set(ClientError, ER_NO_SUCH_SPACE, table_name_str);
		parser->is_aborted = true;
		return;
	}

	uint32_t index_id = sql_index_id_by_token(space, name);
	if (index_id == 0) {
		int regs = sqlGetTempRange(parser, 3);
		struct Vdbe *v = sqlGetVdbe(parser);
		sqlVdbeCountChanges(v);
		sqlVdbeAddOp2(v, OP_Integer, space->def->id, regs);
		sqlVdbeAddOp2(v, OP_Integer, 0, regs + 1);
		sqlVdbeAddOp3(v, OP_MakeRecord, regs, 2, regs + 2);
		sqlVdbeAddOp3(v, OP_SDelete, BOX_INDEX_ID, regs + 2, 0);
		sqlVdbeChangeP5(v, OPFLAG_NCHANGE);
		sqlReleaseTempRange(parser, regs, 3);
		return;
	}

	diag_set(ClientError, ER_NO_SUCH_CONSTRAINT,
		 sql_tt_name_from_token(name), space->def->name);
	parser->is_aborted = true;
}

void
sql_drop_unique(struct Parse *parser, const struct Token *table_name,
		const struct Token *name)
{
	const struct space *space = sql_space_by_token(table_name);
	if (space == NULL) {
		const char *table_name_str = sql_tt_name_from_token(table_name);
		diag_set(ClientError, ER_NO_SUCH_SPACE, table_name_str);
		parser->is_aborted = true;
		return;
	}

	uint32_t index_id = sql_index_id_by_token(space, name);
	if (index_id != 0 && index_id != UINT32_MAX) {
		int regs = sqlGetTempRange(parser, 3);
		struct Vdbe *v = sqlGetVdbe(parser);
		sqlVdbeCountChanges(v);
		sqlVdbeAddOp2(v, OP_Integer, space->def->id, regs);
		sqlVdbeAddOp2(v, OP_Integer, index_id, regs + 1);
		sqlVdbeAddOp3(v, OP_MakeRecord, regs, 2, regs + 2);
		sqlVdbeAddOp3(v, OP_SDelete, BOX_INDEX_ID, regs + 2, 0);
		sqlVdbeChangeP5(v, OPFLAG_NCHANGE);
		sqlReleaseTempRange(parser, regs, 3);
		return;
	}

	diag_set(ClientError, ER_NO_SUCH_CONSTRAINT,
		 sql_tt_name_from_token(name), space->def->name);
	parser->is_aborted = true;
}

void
sql_drop_field_constraint(struct Parse *parser, const struct Token *table_name,
			  const struct Token *column_name,
			  const struct Token *name)
{
	const struct space *space = sql_space_by_token(table_name);
	if (space == NULL) {
		const char *table_name_str = sql_tt_name_from_token(table_name);
		diag_set(ClientError, ER_NO_SUCH_SPACE, table_name_str);
		parser->is_aborted = true;
		return;
	}
	uint32_t fieldno = sql_fieldno_by_token(space, column_name);
	if (fieldno == UINT32_MAX) {
		const char *column_name_str =
			sql_tt_name_from_token(column_name);
		diag_set(ClientError, ER_NO_SUCH_FIELD_NAME_IN_SPACE,
			 space->def->name, column_name_str);
		parser->is_aborted = true;
		return;
	}
	struct Vdbe *v = sqlGetVdbe(parser);
	const char *field_name = space->def->fields[fieldno].name;

	const struct tuple_constraint_def *fk =
		sql_field_fk_by_token(space, fieldno, name);
	if (fk != NULL) {
		sqlVdbeCountChanges(v);
		sqlVdbeAddOp4(v, OP_DropFieldForeignKey, space->def->id, 0,
			      fieldno, sql_xstrdup(fk->name), P4_DYNAMIC);
	}

	const struct tuple_constraint_def *ck =
		sql_field_ck_by_token(space, fieldno, name);
	if (ck != NULL) {
		if (fk != NULL) {
			const char *err =
				tt_sprintf("ambiguous constraint name: '%s.%s'",
					   field_name, fk->name);
			diag_set(ClientError, ER_SQL_EXECUTE, err);
			parser->is_aborted = true;
			return;
		}
		sqlVdbeCountChanges(v);
		sqlVdbeAddOp4(v, OP_DropFieldCheck, space->def->id, 0, fieldno,
			      sql_xstrdup(ck->name), P4_DYNAMIC);
		return;
	}

	if (fk != NULL)
		return;

	char *name_str = sql_name_from_token(name);
	diag_set(ClientError, ER_NO_SUCH_CONSTRAINT,
		 tt_sprintf("%s.%s", field_name, name_str), space->def->name);
	sql_xfree(name_str);
	parser->is_aborted = true;
}

void
sql_drop_field_foreign_key(struct Parse *parser, const struct Token *table_name,
			   const struct Token *column_name,
			   const struct Token *name)
{
	const struct space *space = sql_space_by_token(table_name);
	if (space == NULL) {
		const char *table_name_str = sql_tt_name_from_token(table_name);
		diag_set(ClientError, ER_NO_SUCH_SPACE, table_name_str);
		parser->is_aborted = true;
		return;
	}

	uint32_t fieldno = sql_fieldno_by_token(space, column_name);
	if (fieldno == UINT32_MAX) {
		const char *column_name_str =
			sql_tt_name_from_token(column_name);
		diag_set(ClientError, ER_NO_SUCH_FIELD_NAME_IN_SPACE,
			 space->def->name, column_name_str);
		parser->is_aborted = true;
		return;
	}

	const struct tuple_constraint_def *fk =
		sql_field_fk_by_token(space, fieldno, name);
	if (fk != NULL) {
		struct Vdbe *v = sqlGetVdbe(parser);
		sqlVdbeCountChanges(v);
		sqlVdbeAddOp4(v, OP_DropFieldForeignKey, space->def->id, 0,
			      fieldno, sql_xstrdup(fk->name), P4_DYNAMIC);
		return;
	}

	char *name_str = sql_name_from_token(name);
	const char *field_name = space->def->fields[fieldno].name;
	diag_set(ClientError, ER_NO_SUCH_CONSTRAINT,
		 tt_sprintf("%s.%s", field_name, name_str), space->def->name);
	sql_xfree(name_str);
	parser->is_aborted = true;
}

void
sql_drop_field_check(struct Parse *parser, const struct Token *table_name,
		     const struct Token *column_name, const struct Token *name)
{
	const struct space *space = sql_space_by_token(table_name);
	if (space == NULL) {
		const char *table_name_str = sql_tt_name_from_token(table_name);
		diag_set(ClientError, ER_NO_SUCH_SPACE, table_name_str);
		parser->is_aborted = true;
		return;
	}

	uint32_t fieldno = sql_fieldno_by_token(space, column_name);
	if (fieldno == UINT32_MAX) {
		const char *column_name_str =
			sql_tt_name_from_token(column_name);
		diag_set(ClientError, ER_NO_SUCH_FIELD_NAME_IN_SPACE,
			 space->def->name, column_name_str);
		parser->is_aborted = true;
		return;
	}

	const struct tuple_constraint_def *ck =
		sql_field_ck_by_token(space, fieldno, name);
	if (ck != NULL) {
		struct Vdbe *v = sqlGetVdbe(parser);
		sqlVdbeCountChanges(v);
		sqlVdbeAddOp4(v, OP_DropFieldCheck, space->def->id, 0, fieldno,
			      sql_xstrdup(ck->name), P4_DYNAMIC);
		return;
	}

	char *name_str = sql_name_from_token(name);
	const char *field_name = space->def->fields[fieldno].name;
	diag_set(ClientError, ER_NO_SUCH_CONSTRAINT,
		 tt_sprintf("%s.%s", field_name, name_str), space->def->name);
	sql_xfree(name_str);
	parser->is_aborted = true;
}

/**
 * Position @a _index_cursor onto a last record in _index space
 * with a specified @a space_id. It corresponds to the latest
 * created index with the biggest id.
 * @param parser SQL parser.
 * @param space_id Space identifier to use as a key for _index.
 * @param _index_cursor A cursor, opened on _index system space.
 * @param[out] not_found_addr A VDBE address from which a jump
 *       happens when a record was not found.
 *
 * @return A VDBE address from which a jump happens when a record
 *         was found.
 */
static int
vdbe_emit_space_index_search(struct Parse *parser, uint32_t space_id,
			     int _index_cursor, int *not_found_addr)
{
	struct Vdbe *v = sqlGetVdbe(parser);
	int key_reg = ++parser->nMem;

	sqlVdbeAddOp2(v, OP_Integer, space_id, key_reg);
	int not_found1 = sqlVdbeAddOp4Int(v, OP_SeekLE, _index_cursor, 0,
					  key_reg, 1);
	int not_found2 = sqlVdbeAddOp4Int(v, OP_IdxLT, _index_cursor, 0,
					  key_reg, 1);
	int found_addr = sqlVdbeAddOp0(v, OP_Goto);
	sqlVdbeJumpHere(v, not_found1);
	sqlVdbeJumpHere(v, not_found2);
	*not_found_addr = sqlVdbeAddOp0(v, OP_Goto);
	return found_addr;
}

/**
 * Generate code to determine next free secondary index id in the
 * space identified by @a space_id. Overall VDBE program logic is
 * following:
 *
 * 1 Seek for space id in _index, goto l1 if seeks fails.
 * 2 Fetch index id from _index record.
 * 3 Goto l2
 * 4 l1: Generate iid == 1..
 * 6 l2: Continue index creation.
 *
 * Note that we generate iid == 1 in case of index search on
 * purpose: it allows on_replace_dd_index() raise correct
 * error - "can not add a secondary key before primary".
 *
 * @return Register holding a new index id.
 */
static int
vdbe_emit_new_sec_index_id(struct Parse *parse, uint32_t space_id,
			   int _index_cursor)
{
	struct Vdbe *v = sqlGetVdbe(parse);
	int not_found_addr, found_addr =
		vdbe_emit_space_index_search(parse, space_id, _index_cursor,
					     &not_found_addr);
	int iid_reg = ++parse->nMem;
	sqlVdbeJumpHere(v, found_addr);
	/* Fetch iid from the row and increment it. */
	sqlVdbeAddOp3(v, OP_Column, _index_cursor, BOX_INDEX_FIELD_ID, iid_reg);
	sqlVdbeAddOp2(v, OP_AddImm, iid_reg, 1);
	/* Jump over block assigning wrong index id. */
	int skip_bad_iid = sqlVdbeAddOp0(v, OP_Goto);
	sqlVdbeJumpHere(v, not_found_addr);
	/*
	 * Absence of any records in _index for that space is
	 * handled here: to indicate that secondary index can't
	 * be created before primary.
	 */
	sqlVdbeAddOp2(v, OP_Integer, 1, iid_reg);
	sqlVdbeJumpHere(v, skip_bad_iid);
	return iid_reg;
}

/**
 * Add new index to ephemeral @a space's "index" array. Reallocate
 * memory on @a parse's region if needed.
 *
 * We follow convention that PK comes first in list.
 *
 * @param parse Parsing structure.
 * @param space Space to which belongs given index.
 * @param index Index to be added to list.
 */
static void
sql_space_add_index(struct Parse *parse, struct space *space,
		    struct index *index)
{
	uint32_t idx_count = space->index_count;
	struct index **idx = NULL;
	struct region *region = &parse->region;
	if (idx_count == 0) {
		idx = xregion_alloc_object(region, typeof(struct index *));
	/*
	 * Reallocate each time the idx_count becomes equal to the
	 * power of two.
	 */
	} else if ((idx_count & (idx_count - 1)) == 0) {
		idx = xregion_alloc_array(region, typeof(struct index *),
					  idx_count * 2);
		memcpy(idx, space->index, idx_count * sizeof(struct index *));
	}
	if (idx != NULL)
		space->index = idx;
	/* Make sure that PK always comes as first member. */
	if (index->def->iid == 0 && idx_count != 0)
		SWAP(space->index[0], index);
	space->index[space->index_count++] = index;
	space->index_id_max =  MAX(space->index_id_max, index->def->iid);;
}

int
sql_space_def_check_format(const struct space_def *space_def)
{
	assert(space_def != NULL);
	if (space_def->field_count == 0) {
		diag_set(ClientError, ER_UNSUPPORTED, "SQL",
			 "space without format");
		return -1;
	}
	return 0;
}

/**
 * Create and set index_def in the given Index.
 *
 * @param parse Parse context.
 * @param index Index for which index_def should be created. It is
 *              used only to set index_def at the end of the
 *              function.
 * @param table Table which is indexed by 'index' param.
 * @param iid Index ID.
 * @param name Index name.
 * @param name_len Index name length.
 * @param parts Resolved parts of the index.
 * @param part_count Number of the parts.
 * @param idx_type Index type: non-unique index, unique index,
 *                 index implementing UNIQUE constraint or
 *                 index implementing PK constraint.
 * @retval 0 on success, -1 on error.
 */
static int
index_fill_def(struct Parse *parse, struct index *index,
	       struct space_def *space_def, uint32_t iid, const char *name,
	       uint32_t name_len, const struct rast_index_part *parts,
	       uint32_t part_count, enum sql_index_type idx_type)
{
	struct index_opts opts;
	index_opts_create(&opts);
	opts.is_unique = idx_type != SQL_INDEX_TYPE_NON_UNIQUE;
	index->def = NULL;
	int rc = -1;

	struct key_def *key_def = NULL;
	size_t region_svp = region_used(&fiber()->gc);
	struct key_part_def *key_parts =
		xregion_alloc_array(&fiber()->gc, typeof(key_parts[0]),
				    part_count);
	for (uint32_t i = 0; i < part_count; i++) {
		uint32_t fieldno = parts[i].fieldno;
		/*
		 * Tarantool: DESC indexes are not supported so
		 * far.
		 */
		struct key_part_def *part = &key_parts[i];
		part->fieldno = fieldno;
		part->type = space_def->fields[fieldno].type;
		part->nullable_action = space_def->fields[fieldno].nullable_action;
		part->is_nullable = part->nullable_action == ON_CONFLICT_ACTION_NONE;
		part->exclude_null = false;
		part->sort_order = SORT_ORDER_ASC;
		part->coll_id = parts[i].coll_id;
		part->path = NULL;
	}
	key_def = key_def_new(key_parts, part_count, 0);
	if (key_def == NULL)
		goto tnt_error;
	/*
	 * Index def of PK is set to be NULL since it matters
	 * only for comparison routine. Meanwhile on front-end
	 * side only definition is used.
	 */
	index->def = index_def_new(space_def->id, 0, name, name_len,
				   space_def->name, space_def->engine_name,
				   TREE, &opts, key_def, NULL);
	index->def->iid = iid;
	rc = 0;
cleanup:
	region_truncate(&fiber()->gc, region_svp);
	if (key_def != NULL)
		key_def_delete(key_def);
	return rc;
tnt_error:
	parse->is_aborted = true;
	goto cleanup;
}

/**
 * Simple attempt at figuring out whether constraint was created
 * with name or without.
 */
static bool
constraint_is_named(const char *name)
{
	return strncmp(name, "sql_autoindex_", strlen("sql_autoindex_")) &&
		strncmp(name, "pk_unnamed_", strlen("pk_unnamed_")) &&
		strncmp(name, "unique_unnamed_", strlen("unique_unnamed_"));
}

void
sql_index_create(struct Parse *parse, struct space *space,
		 const char *index_name, const struct rast_index_part *parts,
		 uint32_t part_count, enum sql_index_type idx_type,
		 bool is_create_table_or_add_col)
{
	struct space_def *def = space->def;
	struct index *index = xregion_alloc_object(&parse->region,
						   typeof(*index));
	memset(index, 0, sizeof(*index));

	/*
	 * TODO: Issue a warning if two or more columns of the
	 * index are identical.
	 * TODO: Issue a warning if the table primary key is used
	 * as part of the index key.
	 */
	uint32_t iid;
	if (idx_type != SQL_INDEX_TYPE_CONSTRAINT_PK)
		iid = space->index_id_max + 1;
	else
		iid = 0;
	if (index_fill_def(parse, index, def, iid, index_name,
			   strlen(index_name), parts, part_count,
			   idx_type) != 0)
		goto exit;
	/*
	 * Remove all redundant columns from the PRIMARY KEY.
	 * For example, change "PRIMARY KEY(a,b,a,b,c,b,c,d)" into
	 * just "PRIMARY KEY(a,b,c,d)". Later code assumes the
	 * PRIMARY KEY contains no repeated columns.
	 */
	struct key_part *key_parts = index->def->key_def->parts;
	uint32_t key_part_count = index->def->key_def->part_count;
	uint32_t new_part_count = 1;
	for (uint32_t i = 1; i < key_part_count; i++) {
		uint32_t j;
		for (j = 0; j < new_part_count; j++) {
			if (key_parts[i].fieldno == key_parts[j].fieldno)
				break;
		}

		if (j == new_part_count)
			key_parts[new_part_count++] = key_parts[i];
	}
	index->def->key_def->part_count = new_part_count;

	if (index_def_check(index->def, def->name) != 0) {
		parse->is_aborted = true;
		goto exit;
	}

	/*
	 * Here we handle cases, when in CREATE TABLE statement
	 * some UNIQUE constraints are putted exactly on the same
	 * columns with PRIMARY KEY constraint. Our general
	 * intention is to omit creating indexes for non-named
	 * UNIQUE constraints if these constraints are putted on
	 * the same columns as the PRIMARY KEY constraint. In
	 * different cases it is implemented in different ways.
	 *
	 * 1) CREATE TABLE t(a UNIQUE PRIMARY KEY)
	 *    CREATE TABLE t(a, UNIQUE(a), PRIMARY KEY(a))
	 *    In these cases we firstly proceed UNIQUE(a)
	 *    and create index for it, then proceed PRIMARY KEY,
	 *    but don't create index for it. Instead of it we
	 *    change UNIQUE constraint index name and index_type,
	 *    so it becomes PRIMARY KEY index.
	 *
	 * 2) CREATE TABLE t(a, PRIMARY KEY(a), UNIQUE(a))
	 *    In such cases we simply do not create index for
	 *    UNIQUE constraint.
	 *
	 * Note 1: We always create new index for named UNIQUE
	 * constraints.
	 *
	 * Note 2: If UNIQUE constraint (no matter named or
	 * non-named) is putted on the same columns as PRIMARY KEY
	 * constraint, but has different onError (behavior on
	 * constraint violation), then an error is raised.
	 */
	if (is_create_table_or_add_col) {
		for (uint32_t i = 0; i < space->index_count; ++i) {
			struct index *existing_idx = space->index[i];
			uint32_t iid = existing_idx->def->iid;
			struct key_def *key_def = index->def->key_def;
			struct key_def *exst_key_def =
				existing_idx->def->key_def;

			if (key_def->part_count != exst_key_def->part_count)
				continue;

			uint32_t k;
			for (k = 0; k < key_def->part_count; k++) {
				if (key_def->parts[k].fieldno !=
				    exst_key_def->parts[k].fieldno)
					break;
				if (key_def->parts[k].coll !=
				    exst_key_def->parts[k].coll)
					break;
			}

			if (k != key_def->part_count)
				continue;

			bool is_named =
				constraint_is_named(existing_idx->def->name);
			/* CREATE TABLE t(a, UNIQUE(a), PRIMARY KEY(a)). */
			if (idx_type == SQL_INDEX_TYPE_CONSTRAINT_PK &&
			    iid != 0 && !is_named) {
				existing_idx->def->iid = 0;
				goto exit;
			}

			/* CREATE TABLE t(a, PRIMARY KEY(a), UNIQUE(a)). */
			if (idx_type == SQL_INDEX_TYPE_CONSTRAINT_UNIQUE &&
			    !constraint_is_named(index->def->name))
				goto exit;
		}

	/*
	 * If this is the initial CREATE INDEX statement (or
	 * CREATE TABLE if the index is an implied index for a
	 * UNIQUE or PRIMARY KEY constraint) then emit code to
	 * insert new index into Tarantool. But, do not do this if
	 * we are simply parsing the schema, or if this index is
	 * the PRIMARY KEY index.
	 *
	 * If table == Token_nil it means this index is generated as
	 * an implied PRIMARY KEY or UNIQUE index in a CREATE
	 * TABLE statement.  Since the table has just been
	 * created, it contains no data and the index
	 * initialization step can be skipped.
	 */
	} else {
		Vdbe *vdbe;
		int cursor = parse->nTab++;

		vdbe = sqlGetVdbe(parse);
		int reg = ++parse->nMem;
		sqlVdbeAddOp2(vdbe, OP_OpenSpace, reg, BOX_INDEX_ID);
		sqlVdbeAddOp3(vdbe, OP_IteratorOpen, cursor, 0, reg);
		sqlVdbeChangeP5(vdbe, OPFLAG_SEEKEQ);
		int index_id;
		/*
		 * In case we are creating PRIMARY KEY constraint
		 * (via ALTER TABLE) we must ensure that table
		 * doesn't feature any indexes. Otherwise,
		 * we can immediately halt execution of VDBE.
		 */
		if (idx_type == SQL_INDEX_TYPE_CONSTRAINT_PK) {
			index_id = ++parse->nMem;
			sqlVdbeAddOp2(vdbe, OP_Integer, 0, index_id);
		} else {
			index_id = vdbe_emit_new_sec_index_id(parse, def->id,
							      cursor);
		}
		sqlVdbeAddOp1(vdbe, OP_Close, cursor);
		vdbe_emit_create_index(parse, def, index->def,
				       def->id, index_id);
		sqlVdbeChangeP5(vdbe, OPFLAG_NCHANGE);
		sqlVdbeAddOp0(vdbe, OP_Expire);
	}

	if (is_create_table_or_add_col) {
		sql_space_add_index(parse, space, index);
		index = NULL;
		goto exit;
	}

	if (parse->autoinc_fieldno != NULL) {
		struct Vdbe *v = sqlGetVdbe(parse);
		assert(v != NULL);
		int reg_space_id = ++parse->nMem;
		sqlVdbeAddOp2(v, OP_Integer, def->id, reg_space_id);
		int reg_seq_id = ++parse->nMem;
		sqlVdbeAddOp2(v, OP_NextSequenceId, 0, reg_seq_id);
		int reg_seq_rec = emitNewSysSequenceRecord(parse, reg_seq_id,
							   def->name);
		sqlVdbeAddOp2(v, OP_SInsert, BOX_SEQUENCE_ID, reg_seq_rec);
		int fieldno = *parse->autoinc_fieldno;
		int reg = emitNewSysSpaceSequenceRecord(parse, reg_space_id,
							reg_seq_id, fieldno);
		sqlVdbeAddOp2(v, OP_SInsert, BOX_SPACE_SEQUENCE_ID, reg);
	}

exit:
	if (index != NULL && index->def != NULL)
		index_def_delete(index->def);
}

/** Emit VDBE bytecode for the given resolved DROP INDEX statement. */
static void
sql_emit_drop_index(struct Parse *parser, const struct rast_drop_index *stmt)
{
	struct Vdbe *v = sqlGetVdbe(parser);
	sqlVdbeCountChanges(v);
	if (stmt->index_id == UINT32_MAX)
		return;
	int regs = sqlGetTempRange(parser, 3);
	sqlVdbeAddOp2(v, OP_Integer, stmt->space->def->id, regs);
	sqlVdbeAddOp2(v, OP_Integer, stmt->index_id, regs + 1);
	sqlVdbeAddOp3(v, OP_MakeRecord, regs, 2, regs + 2);
	sqlVdbeAddOp3(v, OP_SDelete, BOX_INDEX_ID, regs + 2, 0);
	sqlVdbeChangeP5(v, OPFLAG_NCHANGE);
	sqlReleaseTempRange(parser, regs, 3);
}

void *
sqlArrayAllocate(void *pArray, size_t szEntry, int *pnEntry, int *pIdx)
{
	char *z;
	int n = *pnEntry;
	if ((n & (n - 1)) == 0) {
		int sz = (n == 0) ? 1 : 2 * n;
		void *pNew = sql_xrealloc(pArray, sz * szEntry);
		pArray = pNew;
	}
	z = (char *)pArray;
	memset(&z[n * szEntry], 0, szEntry);
	*pIdx = n;
	++*pnEntry;
	return pArray;
}

struct IdList *
sql_id_list_append(struct IdList *list, struct Token *name_token)
{
	if (list == NULL)
		list = sql_xmalloc0(sizeof(*list));
	int i;
	assert(list->nId >= 0);
	list->a = sqlArrayAllocate(list->a, sizeof(list->a[0]), &list->nId, &i);
	assert(i >= 0);
	list->a[i].zName = sql_name_from_token(name_token);
	if (name_token->z[0] != '"') {
		list->a[i].legacy_name = sql_legacy_name_new(name_token->z,
							     name_token->n);
	}
	return list;
}

void
sqlIdListDelete(struct IdList *pList)
{
	int i;
	if (pList == 0)
		return;
	for (i = 0; i < pList->nId; i++) {
		sql_xfree(pList->a[i].zName);
		sql_xfree(pList->a[i].legacy_name);
	}
	sql_xfree(pList->a);
	sql_xfree(pList);
}

/*
 * Return the index in pList of the identifier named zId.  Return -1
 * if not found.
 */
int
sqlIdListIndex(IdList * pList, const char *zName)
{
	int i;
	if (pList == 0)
		return -1;
	for (i = 0; i < pList->nId; i++) {
		if (strcmp(pList->a[i].zName, zName) == 0)
			return i;
	}
	return -1;
}

struct SrcList *
sql_src_list_enlarge(struct SrcList *src_list, int new_slots, int start_idx)
{
	assert(start_idx >= 0);
	assert(new_slots >= 1);
	assert(src_list != NULL);
	assert(start_idx <= src_list->nSrc);

	/* Allocate additional space if needed. */
	if (src_list->nSrc + new_slots > (int)src_list->nAlloc) {
		int to_alloc = src_list->nSrc * 2 + new_slots;
		int size = sizeof(*src_list) +
			   (to_alloc - 1) * sizeof(src_list->a[0]);
		src_list = sql_xrealloc(src_list, size);
		src_list->nAlloc = to_alloc;
	}

	/*
	 * Move existing slots that come after the newly inserted
	 * slots out of the way.
	 */
	memmove(&src_list->a[start_idx + new_slots], &src_list->a[start_idx],
		(src_list->nSrc - start_idx) * sizeof(src_list->a[0]));
	src_list->nSrc += new_slots;

	/* Zero the newly allocated slots. */
	memset(&src_list->a[start_idx], 0, sizeof(src_list->a[0]) * new_slots);
	for (int i = start_idx; i < start_idx + new_slots; i++)
		src_list->a[i].iCursor = -1;

	/* Return a pointer to the enlarged SrcList. */
	return src_list;
}

struct SrcList *
sql_src_list_new(void)
{
	struct SrcList *src_list = sql_xmalloc(sizeof(struct SrcList));
	src_list->nAlloc = 1;
	src_list->nSrc = 1;
	memset(&src_list->a[0], 0, sizeof(src_list->a[0]));
	src_list->a[0].iCursor = -1;
	return src_list;
}

struct SrcList *
sql_src_list_append(struct SrcList *list, struct Token *name_token)
{
	if (list == NULL) {
		list = sql_src_list_new();
	} else {
		struct SrcList *new_list =
			sql_src_list_enlarge(list, 1, list->nSrc);
		list = new_list;
	}
	struct SrcList_item *item = &list->a[list->nSrc - 1];
	if (name_token != NULL) {
		item->zName = sql_name_from_token(name_token);
		if (name_token->z[0] != '"') {
			item->legacy_name = sql_legacy_name_new(name_token->z,
								name_token->n);
		}
	}
	return list;
}

/*
 * Assign VdbeCursor index numbers to all tables in a SrcList
 */
void
sqlSrcListAssignCursors(Parse * pParse, SrcList * pList)
{
	int i;
	struct SrcList_item *pItem;
	assert(pList != NULL);
	for (i = 0, pItem = pList->a; i < pList->nSrc; i++, pItem++) {
		if (pItem->iCursor >= 0)
			break;
		pItem->iCursor = pParse->nTab++;
		if (pItem->pSelect != NULL)
			sqlSrcListAssignCursors(pParse, pItem->pSelect->pSrc);
	}
}

void
sqlSrcListDelete(struct SrcList *pList)
{
	int i;
	struct SrcList_item *pItem;
	if (pList == 0)
		return;
	for (pItem = pList->a, i = 0; i < pList->nSrc; i++, pItem++) {
		sql_xfree(pItem->zName);
		sql_xfree(pItem->zAlias);
		sql_xfree(pItem->legacy_name);
		if (pItem->fg.isIndexedBy) {
			sql_xfree(pItem->indexed_by);
			sql_xfree(pItem->legacy_index_name);
		}
		/*
		* Space is either not ephemeral which means that
		* it came from space cache; or space is ephemeral
		* but has no indexes and check constraints.
		* The latter proves that it is not the space
		* which might come from CREATE TABLE routines.
		*/
		assert(pItem->space == NULL ||
			!pItem->space->def->opts.is_ephemeral ||
			pItem->space->index == NULL);
		sql_select_delete(pItem->pSelect);
		sql_expr_delete(pItem->pOn);
		sqlIdListDelete(pItem->pUsing);
	}
	sql_xfree(pList);
}

struct SrcList *
sqlSrcListAppendFromTerm(struct SrcList *p, struct Token *pTable,
			 struct Token *pAlias, struct Select *pSubquery,
			 struct Expr *pOn, struct ast_id_list *join_using,
			 int disallow_scan)
{
	struct SrcList_item *pItem;
	assert((pOn == NULL && join_using == NULL) || p != NULL);
	p = sql_src_list_append(p, pTable);
	assert(p->nSrc != 0);
	pItem = &p->a[p->nSrc - 1];
	assert(pAlias != 0);
	if (pAlias->n != 0) {
		pItem->zAlias = sql_name_from_token(pAlias);
	}
	pItem->pSelect = pSubquery;
	pItem->pOn = pOn;
	pItem->pUsing = id_list_from_ast(join_using);
	pItem->fg.disallow_scan = disallow_scan;
	return p;
}

/*
 * Add an INDEXED BY or NOT INDEXED clause to the most recently added
 * element of the source-list passed as the second argument.
 */
void
sqlSrcListIndexedBy(struct SrcList *p, struct Token *pIndexedBy)
{
	assert(pIndexedBy != 0);
	if (p && ALWAYS(p->nSrc > 0)) {
		struct SrcList_item *pItem = &p->a[p->nSrc - 1];
		assert(pItem->fg.notIndexed == 0);
		assert(pItem->fg.isIndexedBy == 0);
		if (pIndexedBy->n == 1 && !pIndexedBy->z) {
			/* A "NOT INDEXED" clause was supplied. See parse.y
			 * construct "indexed_opt" for details.
			 */
			pItem->fg.notIndexed = 1;
		} else if (pIndexedBy->z != NULL) {
			pItem->indexed_by = sql_name_from_token(pIndexedBy);
			pItem->fg.isIndexedBy = true;
			if (pIndexedBy->z[0] != '"') {
				pItem->legacy_index_name =
					sql_legacy_name_new(pIndexedBy->z,
							    pIndexedBy->n);
			}
		}
	}
}

void
sql_transaction_begin(struct Parse *parse_context)
{
	assert(parse_context != NULL);
	struct Vdbe *v = sqlGetVdbe(parse_context);
	sqlVdbeAddOp0(v, OP_TransactionBegin);
}

void
sql_transaction_commit(struct Parse *parse_context)
{
	assert(parse_context != NULL);
	struct Vdbe *v = sqlGetVdbe(parse_context);
	sqlVdbeAddOp0(v, OP_TransactionCommit);
}

void
sql_transaction_rollback(Parse *pParse)
{
	assert(pParse != 0);
	struct Vdbe *v = sqlGetVdbe(pParse);
	sqlVdbeAddOp0(v, OP_TransactionRollback);
}

/*
 * This function is called by the parser when it parses a command to create,
 * release or rollback an SQL savepoint.
 */
void
sqlSavepoint(Parse * pParse, int op, Token * pName)
{
	char *zName = sql_name_from_token(pName);
	Vdbe *v = sqlGetVdbe(pParse);
	if (op == SAVEPOINT_BEGIN &&
	    sqlCheckIdentifierName(pParse, zName) != 0) {
		sql_xfree(zName);
		return;
	}
	/*
	 * Keep the uppercase version of the unquoted name for backwards
	 * compatibility.
	 */
	int old_name_reg = 0;
	assert(pName->n > 0);
	if (op != SAVEPOINT_BEGIN && pName->z[0] != '"') {
		old_name_reg = ++pParse->nMem;
		char *old_name = sql_legacy_name_new(pName->z, pName->n);
		sqlVdbeAddOp4(v, OP_String8, 0, old_name_reg, 0, old_name,
			      P4_DYNAMIC);
	}
	sqlVdbeAddOp4(v, OP_Savepoint, op, 0, old_name_reg, zName, P4_DYNAMIC);
}

/*
 * This routine is invoked once per CTE by the parser while parsing a
 * WITH clause.
 */
With *
sqlWithAdd(Parse * pParse,	/* Parsing context */
	       With * pWith,	/* Existing WITH clause, or NULL */
	       Token * pName,	/* Name of the common-table */
	       ExprList * pArglist,	/* Optional column name list for the table */
	       Select * pQuery	/* Query used to initialize the table */
    )
{
	With *pNew;

	/*
	 * Check that the CTE name is unique within this WITH
	 * clause. If not, store an error in the Parse structure.
	 */
	char *name = sql_name_from_token(pName);
	if (pWith != NULL) {
		int i;
		const char *err = "Ambiguous table name in WITH query: %s";
		for (i = 0; i < pWith->nCte; i++) {
			if (strcmp(name, pWith->a[i].zName) == 0) {
				diag_set(ClientError, ER_SQL_PARSER_GENERIC,
					 tt_sprintf(err, name));
				pParse->is_aborted = true;
			}
		}
	}

	if (pWith) {
		int nByte =
		    sizeof(*pWith) + (sizeof(pWith->a[1]) * pWith->nCte);
		pNew = sql_xrealloc(pWith, nByte);
	} else {
		pNew = sql_xmalloc0(sizeof(*pWith));
	}

	pNew->a[pNew->nCte].pSelect = pQuery;
	pNew->a[pNew->nCte].pCols = pArglist;
	pNew->a[pNew->nCte].zName = name;
	pNew->a[pNew->nCte].zCteErr = 0;
	pNew->nCte++;
	return pNew;
}

/** Free the contents of the With object and remove the object. */
void
sqlWithDelete(struct With *pWith)
{
	if (pWith) {
		int i;
		for (i = 0; i < pWith->nCte; i++) {
			struct Cte *pCte = &pWith->a[i];
			sql_expr_list_delete(pCte->pCols);
			sql_select_delete(pCte->pSelect);
			sql_xfree(pCte->zName);
		}
		sql_xfree(pWith);
	}
}

void
vdbe_emit_halt_with_presence_test(struct Parse *parser, int space_id,
				  int index_id, int key_reg, uint32_t key_len,
				  int tarantool_error_code,
				  const char *error_src, bool no_error,
				  int cond_opcode)
{
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
	return;
#endif /* FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION */
	assert(cond_opcode == OP_NoConflict || cond_opcode == OP_Found);
	struct Vdbe *v = sqlGetVdbe(parser);
	assert(v != NULL);

	int cursor = parser->nTab++;
	vdbe_emit_open_cursor(parser, cursor, index_id, space_by_id(space_id));
	sqlVdbeChangeP5(v, OPFLAG_SYSTEMSP);
	int addr = sqlVdbeAddOp4Int(v, cond_opcode, cursor, 0, key_reg,
				    key_len);
	if (no_error) {
		sqlVdbeAddOp0(v, OP_Halt);
	} else {
		sqlVdbeAddOp4(v, OP_SetDiag, tarantool_error_code, 0, 0,
			      sql_xstrdup(error_src), P4_DYNAMIC);
		sqlVdbeAddOp2(v, OP_Halt, -1, ON_CONFLICT_ACTION_ABORT);
	}
	sqlVdbeJumpHere(v, addr);
	sqlVdbeAddOp1(v, OP_Close, cursor);
}

/**
 * A local structure that allows to establish a connection between
 * parameter and its field type and mask, if it has one.
 */
struct sql_option_metadata
{
	uint32_t field_type;
	uint32_t mask;
};

/**
 * Variable that contains SQL session option field types and masks
 * if they have one or 0 if don't have.
 *
 * It is IMPORTANT that these options sorted by name.
 */
static struct sql_option_metadata sql_session_opts[] = {
	/** SESSION_SETTING_SQL_DEFAULT_ENGINE */
	{FIELD_TYPE_STRING, 0},
	/** SESSION_SETTING_SQL_FULL_COLUMN_NAMES */
	{FIELD_TYPE_BOOLEAN, SQL_FullColNames},
	/** SESSION_SETTING_SQL_FULL_METADATA */
	{FIELD_TYPE_BOOLEAN, SQL_FullMetadata},
	/** SESSION_SETTING_SQL_PARSER_DEBUG */
	{FIELD_TYPE_BOOLEAN, SQL_SqlTrace | PARSER_TRACE_FLAG},
	/** SESSION_SETTING_SQL_RECURSIVE_TRIGGERS */
	{FIELD_TYPE_BOOLEAN, SQL_RecTriggers},
	/** SESSION_SETTING_SQL_REVERSE_UNORDERED_SELECTS */
	{FIELD_TYPE_BOOLEAN, SQL_ReverseOrder},
	/** SESSION_SETTING_SQL_SELECT_DEBUG */
	{FIELD_TYPE_BOOLEAN,
	 SQL_SqlTrace | SQL_SelectTrace | SQL_WhereTrace},
	/** SESSION_SETTING_SQL_SEQ_SCAN */
	{FIELD_TYPE_BOOLEAN, SQL_SeqScan},
	/** SESSION_SETTING_SQL_VDBE_DEBUG */
	{FIELD_TYPE_BOOLEAN,
	 SQL_SqlTrace | SQL_VdbeListing | SQL_VdbeTrace},
};

static void
sql_session_setting_get(int id, const char **mp_pair, const char **mp_pair_end)
{
	assert(id >= SESSION_SETTING_SQL_BEGIN && id < SESSION_SETTING_SQL_END);
	struct session *session = current_session();
	uint32_t flags = session->sql_flags;
	struct sql_option_metadata *opt =
		&sql_session_opts[id - SESSION_SETTING_SQL_BEGIN];
	uint32_t mask = opt->mask;
	const char *name = session_setting_strs[id];
	size_t name_len = strlen(name);
	size_t engine_len;
	const char *engine;
	size_t size = mp_sizeof_array(2) + mp_sizeof_str(name_len);
	/*
	 * Currently, SQL session settings are of a boolean or
	 * string type.
	 */
	bool is_bool = opt->field_type == FIELD_TYPE_BOOLEAN;
	if (is_bool) {
		size += mp_sizeof_bool(true);
	} else {
		assert(id == SESSION_SETTING_SQL_DEFAULT_ENGINE);
		engine = sql_storage_engine_strs[session->sql_default_engine];
		engine_len = strlen(engine);
		size += mp_sizeof_str(engine_len);
	}

	char *pos = static_alloc(size);
	assert(pos != NULL);
	char *pos_end = mp_encode_array(pos, 2);
	pos_end = mp_encode_str(pos_end, name, name_len);
	if (is_bool)
		pos_end = mp_encode_bool(pos_end, (flags & mask) == mask);
	else
		pos_end = mp_encode_str(pos_end, engine, engine_len);
	*mp_pair = pos;
	*mp_pair_end = pos_end;
}

static int
sql_set_boolean_option(int id, bool value)
{
	struct session *session = current_session();
	struct sql_option_metadata *option =
		&sql_session_opts[id - SESSION_SETTING_SQL_BEGIN];
	assert(option->field_type == FIELD_TYPE_BOOLEAN);
#ifdef NDEBUG
	if ((session->sql_flags & SQL_SqlTrace) == 0) {
		if (value)
			session->sql_flags |= option->mask;
		else
			session->sql_flags &= ~option->mask;
	}
#else
	if (value)
		session->sql_flags |= option->mask;
	else
		session->sql_flags &= ~option->mask;
	if (id == SESSION_SETTING_SQL_PARSER_DEBUG) {
		if (value)
			sqlParserTrace(stdout, "parser: ");
		else
			sqlParserTrace(NULL, NULL);
	}
#endif
	return 0;
}

static int
sql_set_string_option(int id, const char *value)
{
	assert(sql_session_opts[id - SESSION_SETTING_SQL_BEGIN].field_type ==
	       FIELD_TYPE_STRING);
	assert(id == SESSION_SETTING_SQL_DEFAULT_ENGINE);
	(void)id;
	enum sql_storage_engine engine = STR2ENUM(sql_storage_engine, value);
	if (engine == sql_storage_engine_MAX) {
		diag_set(ClientError, ER_NO_SUCH_ENGINE, value);
		return -1;
	}
	current_session()->sql_default_engine = engine;
	return 0;
}

static int
sql_session_setting_set(int id, const char *mp_value)
{
	assert(id >= SESSION_SETTING_SQL_BEGIN && id < SESSION_SETTING_SQL_END);
	enum mp_type mtype = mp_typeof(*mp_value);
	enum field_type stype =
		sql_session_opts[id - SESSION_SETTING_SQL_BEGIN].field_type;
	uint32_t len;
	const char *tmp;
	switch(stype) {
	case FIELD_TYPE_BOOLEAN:
		if (mtype != MP_BOOL)
			break;
		return sql_set_boolean_option(id, mp_decode_bool(&mp_value));
	case FIELD_TYPE_STRING:
		if (mtype != MP_STR)
			break;
		tmp = mp_decode_str(&mp_value, &len);
		tmp = tt_cstr(tmp, len);
		return sql_set_string_option(id, tmp);
	default:
		unreachable();
	}
	diag_set(ClientError, ER_SESSION_SETTING_INVALID_VALUE,
		 session_setting_strs[id], field_type_strs[stype]);
	return -1;
}

void
sql_session_settings_init(void)
{
	for (int i = 0, id = SESSION_SETTING_SQL_BEGIN;
	     id < SESSION_SETTING_SQL_END; ++id, ++i) {
		struct session_setting *setting = &session_settings[id];
		setting->field_type = sql_session_opts[i].field_type;
		setting->get = sql_session_setting_get;
		setting->set = sql_session_setting_set;
	}
}

void
sql_setting_set(struct Parse *parse_context, struct Token *name,
		struct Expr *expr)
{
	struct Vdbe *vdbe = sqlGetVdbe(parse_context);
	sqlVdbeCountChanges(vdbe);
	char *key = sql_name_from_token(name);
	int target = ++parse_context->nMem;
	sqlExprCode(parse_context, expr, target);
	sqlVdbeAddOp4(vdbe, OP_SetSession, target, 0, 0, key, P4_DYNAMIC);
	return;
}

/** Emit VDBE instructions for "SHOW CREATE TABLE table_name;" statement. */
void
sql_emit_show_create_table_one(struct Parse *parse, struct Token *name)
{
	const struct space *space = sql_space_by_token(name);
	if (space == NULL) {
		const char *name_str = sql_tt_name_from_token(name);
		diag_set(ClientError, ER_NO_SUCH_SPACE, name_str);
		parse->is_aborted = true;
		return;
	}

	struct Vdbe *v = sqlGetVdbe(parse);
	sqlVdbeSetNumCols(v, 2);
	vdbe_metadata_set_col_name(v, 0, "STATEMENTS");
	vdbe_metadata_set_col_type(v, 0, "array");
	vdbe_metadata_set_col_name(v, 1, "ERRORS");
	vdbe_metadata_set_col_type(v, 1, "array");
	int space_id_reg = ++parse->nMem;
	sqlVdbeAddOp2(v, OP_Integer, space->def->id, space_id_reg);
	int result_reg = ++parse->nMem;
	++parse->nMem;
	sqlVdbeAddOp2(v, OP_ShowCreateTable, space_id_reg, result_reg);
	sqlVdbeAddOp2(v, OP_ResultRow, result_reg, 2);
}

/** Emit VDBE instructions for "SHOW CREATE TABLE;" statement. */
void
sql_emit_show_create_table_all(struct Parse *parse)
{
	struct Vdbe *v = sqlGetVdbe(parse);
	sqlVdbeSetNumCols(v, 2);
	vdbe_metadata_set_col_name(v, 0, "STATEMENTS");
	vdbe_metadata_set_col_type(v, 0, "array");
	vdbe_metadata_set_col_name(v, 1, "ERRORS");
	vdbe_metadata_set_col_type(v, 1, "array");

	int cursor = parse->nTab++;
	int space_reg = ++parse->nMem;
	int key_reg = ++parse->nMem;
	sqlVdbeAddOp2(v, OP_OpenSpace, space_reg, BOX_VSPACE_ID);
	sqlVdbeAddOp3(v, OP_IteratorOpen, cursor, 0, space_reg);
	sqlVdbeAddOp2(v, OP_Integer, BOX_SYSTEM_ID_MAX, key_reg);
	int addr1 = sqlVdbeAddOp4Int(v, OP_SeekGT, cursor, 0, key_reg, 1);
	int space_id_reg = ++parse->nMem;
	int addr2 = sqlVdbeAddOp3(v, OP_Column, cursor, BOX_SPACE_FIELD_ID,
				  space_id_reg);
	int result_reg = ++parse->nMem;
	++parse->nMem;
	sqlVdbeAddOp2(v, OP_ShowCreateTable, space_id_reg, result_reg);
	sqlVdbeAddOp2(v, OP_ResultRow, result_reg, 2);
	sqlVdbeAddOp2(v, OP_Next, cursor, addr2);
	sqlVdbeJumpHere(v, addr1);
}

void
sql_emit_select(struct Parse *parser, struct rast_select *select)
{
	struct Select *res = select_from_rast(parser, select);
	if (parser->is_aborted)
		return;
	struct SelectDest dest = {SRT_Output, NULL, 0, 0, 0, 0, NULL};
	sqlSelect(parser, res, &dest);
	sql_select_delete(res);
}

/** Emit VDBE bytecode for the given resolved CREATE INDEX statement. */
static void
sql_emit_create_index(struct Parse *parser,
		      const struct rast_create_index *stmt)
{
	parser->disableLookaside++;
	sql_get()->lookaside.bDisable++;
	parser->initiateTTrans = true;
	sqlVdbeCountChanges(sqlGetVdbe(parser));
	if (stmt->space == NULL)
		return;
	char *name = sql_name_from_token(&stmt->name);
	enum sql_index_type type = stmt->is_unique ? SQL_INDEX_TYPE_UNIQUE :
						     SQL_INDEX_TYPE_NON_UNIQUE;
	sql_index_create(parser, stmt->space, name, stmt->parts,
			 stmt->part_count, type, false);
	sql_xfree(name);
}

/**
 * Emit VDBE bytecode for the given resolved FOREIGN KEY constraint added by
 * ALTER TABLE ADD CONSTRAINT.
 */
static void
sql_emit_add_foreign_key(struct Parse *parser,
			 const struct rast_add_constraint *stmt,
			 const char *name)
{
	struct fk_constraint_def *fk_def =
		sql_fk_constraint_def_new(parser, name, stmt->fk.link_count);
	fk_def->is_field_fk = false;
	fk_def->child_id = stmt->space->def->id;
	fk_def->parent_id = stmt->fk.parent->def->id;
	memcpy(fk_def->links, stmt->fk.links,
	       stmt->fk.link_count * sizeof(*fk_def->links));
	vdbe_emit_fk_constraint_create(parser, fk_def, true);
}

/**
 * Emit VDBE bytecode for the given resolved ALTER TABLE ADD CONSTRAINT
 * statement.
 */
static void
sql_emit_add_constraint(struct Parse *parser,
			const struct rast_add_constraint *stmt)
{
	parser->initiateTTrans = true;
	char *name = sql_name_from_token(&stmt->name);
	switch (stmt->type) {
	case SQL_AST_PROPERTY_CHECK: {
		struct ck_constraint_parse *ck =
			ck_constraint_parse_new(parser, name, stmt->check.z,
						stmt->check.n);
		int reg_space_id = ++parser->nMem;
		sqlVdbeAddOp2(sqlGetVdbe(parser), OP_Integer,
			      stmt->space->def->id, reg_space_id);
		vdbe_emit_ck_constraint_create(parser, ck->ck_def,
					       reg_space_id,
					       stmt->space->def->name);
		break;
	}
	case SQL_AST_PROPERTY_FOREIGN_KEY:
		sql_emit_add_foreign_key(parser, stmt, name);
		break;
	default: {
		assert(stmt->type == SQL_AST_PROPERTY_UNIQUE ||
		       stmt->type == SQL_AST_PROPERTY_PRIMARY_KEY);
		if (stmt->index.autoinc_fieldno != UINT32_MAX) {
			parser->autoinc_fieldno =
				xregion_alloc_object(&parser->region, int);
			*parser->autoinc_fieldno = stmt->index.autoinc_fieldno;
		}
		enum sql_index_type type =
			stmt->type == SQL_AST_PROPERTY_UNIQUE ?
			SQL_INDEX_TYPE_CONSTRAINT_UNIQUE :
			SQL_INDEX_TYPE_CONSTRAINT_PK;
		sql_index_create(parser, stmt->space, name, stmt->index.parts,
				 stmt->index.part_count, type, false);
		break;
	}
	}
	sql_xfree(name);
}

/** Emit VDBE bytecode for the given resolved CREATE TABLE statement. */
static void
sql_emit_create_table(struct Parse *parser,
		      struct rast_create_table *stmt)
{
	parser->disableLookaside++;
	sql_get()->lookaside.bDisable++;
	parser->initiateTTrans = true;
	struct Vdbe *v = sqlGetVdbe(parser);
	sqlVdbeCountChanges(v);
	int *func_regs = vdbe_emit_create_default_funcs(parser, stmt);
	/*
	 * Halt silently if the space exists and IF NOT EXISTS is specified,
	 * otherwise create the space.
	 */
	struct space *space = stmt->space;
	int name_reg = ++parser->nMem;
	sqlVdbeAddOp4(v, OP_String8, 0, name_reg, 0,
		      sql_xstrdup(space->def->name), P4_DYNAMIC);
	vdbe_emit_halt_with_presence_test(parser, BOX_SPACE_ID, 2, name_reg, 1,
					  ER_SPACE_EXISTS, space->def->name,
					  stmt->if_not_exists, OP_NoConflict);
	int reg_space_id = getNewSpaceId(parser);
	vdbe_emit_space_create(parser, reg_space_id, name_reg, space);
	vdbe_emit_create_constraints(parser, stmt, reg_space_id, false);
	vdbe_emit_set_default_funcs(parser, stmt, func_regs, reg_space_id);
}

/**
 * Emit VDBE bytecode for the given resolved ALTER TABLE ADD COLUMN statement:
 * the format of the space is replaced with the new one.
 */
static void
sql_emit_add_column(struct Parse *parser, struct rast_create_table *stmt)
{
	parser->initiateTTrans = true;
	int *func_regs = vdbe_emit_create_default_funcs(parser, stmt);
	struct space_def *def = stmt->space->def;
	uint32_t table_stmt_sz = 0;
	char *table_stmt = sql_encode_table(&parser->region, def,
					    &table_stmt_sz);
	if (table_stmt == NULL) {
		parser->is_aborted = true;
		return;
	}
	char *raw = sql_xmalloc(table_stmt_sz);
	memcpy(raw, table_stmt, table_stmt_sz);

	struct Vdbe *v = sqlGetVdbe(parser);
	assert(v != NULL);

	struct space *s_space = space_by_id(BOX_SPACE_ID);
	assert(s_space != NULL);
	int cursor = parser->nTab++;
	vdbe_emit_open_cursor(parser, cursor, 0, s_space);
	sqlVdbeChangeP5(v, OPFLAG_SYSTEMSP);

	int key_reg = ++parser->nMem;
	sqlVdbeAddOp2(v, OP_Integer, def->id, key_reg);
	int addr = sqlVdbeAddOp4Int(v, OP_Found, cursor, 0, key_reg, 1);
	sqlVdbeAddOp2(v, OP_Halt, -1, ON_CONFLICT_ACTION_ABORT);
	sqlVdbeJumpHere(v, addr);

	int tuple_reg = sqlGetTempRange(parser, box_space_field_MAX + 1);
	for (int i = 0; i < box_space_field_MAX - 1; ++i)
		sqlVdbeAddOp3(v, OP_Column, cursor, i, tuple_reg + i);
	sqlVdbeAddOp1(v, OP_Close, cursor);

	sqlVdbeAddOp2(v, OP_Integer, def->field_count,
		      tuple_reg + BOX_SPACE_FIELD_FIELD_COUNT);
	sqlVdbeAddOp4(v, OP_Blob, table_stmt_sz,
		      tuple_reg + BOX_SPACE_FIELD_FORMAT,
		      SQL_SUBTYPE_MSGPACK, raw, P4_DYNAMIC);
	sqlVdbeAddOp3(v, OP_MakeRecord, tuple_reg, box_space_field_MAX,
		      tuple_reg + box_space_field_MAX);
	int reg = ++parser->nMem;
	sqlVdbeAddOp2(v, OP_OpenSpace, reg, BOX_SPACE_ID);
	sqlVdbeAddOp2(v, OP_IdxReplace, tuple_reg + box_space_field_MAX, reg);
	sqlVdbeCountChanges(v);
	sqlVdbeChangeP5(v, OPFLAG_NCHANGE);
	sqlReleaseTempRange(parser, tuple_reg, box_space_field_MAX + 1);
	vdbe_emit_create_constraints(parser, stmt, key_reg, true);
	vdbe_emit_set_default_funcs(parser, stmt, func_regs, key_reg);
}

/** Emit VDBE bytecode for the given resolved CREATE VIEW statement. */
static void
sql_emit_create_view(struct Parse *parser, struct rast_create_view *stmt,
		     const char *sql)
{
	parser->disableLookaside++;
	sql_get()->lookaside.bDisable++;
	parser->initiateTTrans = true;
	struct Select *select = select_from_rast(parser, stmt->select);
	if (parser->is_aborted)
		return;
	struct ExprList *cols = expr_list_from_ids(parser, stmt->columns);
	sql_create_view(parser, sql, &stmt->name, cols, select,
			stmt->if_not_exists);
}

void
sql_emit_vdbe(struct Parse *parser, struct sql_rast *rast, const char *sql)
{
	switch (rast->type) {
	case SQL_AST_SELECT: {
		sql_emit_select(parser, &rast->select);
		break;
	}
	case SQL_AST_INSERT:
		sqlSubProgramsRemaining = SQL_MAX_COMPILING_TRIGGERS;
		parser->initiateTTrans = true;
		sql_emit_insert(parser, &rast->insert);
		break;
	case SQL_AST_CREATE_VIEW:
		sql_emit_create_view(parser, &rast->create_view, sql);
		break;
	case SQL_AST_CREATE_INDEX:
		sql_emit_create_index(parser, &rast->create_index);
		break;
	case SQL_AST_ALTER_ADD_CONSTRAINT:
		sql_emit_add_constraint(parser, &rast->add_constraint);
		break;
	case SQL_AST_CREATE_TABLE:
		sql_emit_create_table(parser, &rast->create_table);
		break;
	case SQL_AST_ALTER_ADD_COLUMN:
		sql_emit_add_column(parser, &rast->create_table);
		break;
	case SQL_AST_UPDATE:
		sqlSubProgramsRemaining = SQL_MAX_COMPILING_TRIGGERS;
		parser->initiateTTrans = true;
		sql_emit_update(parser, &rast->update);
		break;
	case SQL_AST_DELETE:
		sqlSubProgramsRemaining = SQL_MAX_COMPILING_TRIGGERS;
		parser->initiateTTrans = true;
		sql_emit_delete(parser, &rast->del);
		break;
	case SQL_AST_TRUNCATE:
		parser->initiateTTrans = true;
		sql_table_truncate(parser, rast->truncate.space);
		break;
	case SQL_AST_DROP_TABLE:
	case SQL_AST_DROP_VIEW:
		parser->initiateTTrans = true;
		sql_emit_drop_table(parser, &rast->drop_table);
		break;
	case SQL_AST_DROP_INDEX:
		parser->initiateTTrans = true;
		sql_emit_drop_index(parser, &rast->drop_index);
		break;
	case SQL_AST_ALTER_RENAME:
		parser->initiateTTrans = true;
		sqlVdbeAddOp4(sqlGetVdbe(parser), OP_RenameTable,
			      rast->alter_rename.space->def->id, 0, 0,
			      sql_xstrdup(rast->alter_rename.new_name),
			      P4_DYNAMIC);
		break;
	case SQL_AST_DROP_TRIGGER:
		parser->initiateTTrans = true;
		sql_drop_trigger(parser, &rast->drop_trigger.name,
				 rast->drop_trigger.if_exists);
		break;
	default:
		unreachable();
	}
	if (parser->is_aborted)
		return;
	sql_finish_coding(parser);
}
