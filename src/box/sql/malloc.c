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
 *
 * Memory allocation functions used throughout sql.
 */
#include "sqlInt.h"
#include <stdarg.h>

enum {
	MALLOC_MAX = 0x7fffff00,
};
static_assert(MALLOC_MAX < SIZE_MAX);


/*
 * Report the allocated size of a prior return from sql_sized_malloc()
 * or sql_sized_realloc().
 */
static int
sql_sized_sizeof(void *pPrior)
{
	sql_int64 *p;
	assert(pPrior != 0);
	p = (sql_int64 *) pPrior;
	p--;
	return (int)p[0];
}

void *
sqlMalloc(size_t n)
{
	if (n >= MALLOC_MAX) {
		fprintf(stderr, "Can't allocate %zu bytes at %s:%d", n,
			__FILE__, __LINE__);
		exit(EXIT_FAILURE);
	}
	size_t size = ROUND8(n);
	int64_t *buf = xmalloc(size + 8);
	buf[0] = size;
	buf++;
	return buf;
}

/** Return TRUE if buf is a lookaside memory allocation. */
static inline bool
is_lookaside(void *buf)
{
	struct sql *db = sql_get();
	assert(db != NULL);
	return buf >= db->lookaside.pStart && buf < db->lookaside.pEnd;
}

/**
 * Return the size of a memory allocation previously obtained from
 * sqlMalloc().
 */
int
sqlMallocSize(void *p)
{
	return sql_sized_sizeof(p);
}

int
sqlDbMallocSize(sql * db, void *p)
{
	assert(p != 0);
	if (db == NULL || !is_lookaside(p))
		return sql_sized_sizeof(p);
	else
		return db->lookaside.sz;
}

/*
 * Free memory previously obtained from sqlMalloc().
 */
void
sql_free(void *p)
{
	if (p == NULL)
		return;
	sql_int64 *raw_p = (sql_int64 *) p;
	raw_p--;
	free(raw_p);
}

void
sqlDbFree(void *buf)
{
	struct sql *db = sql_get();
	assert(db != NULL);
	if (is_lookaside(buf)) {
		LookasideSlot *pBuf = (LookasideSlot *)buf;
		pBuf->pNext = db->lookaside.pFree;
		db->lookaside.pFree = pBuf;
		db->lookaside.nOut--;
		return;
	}
	sql_free(buf);
}

void *
sqlRealloc(void *buf, size_t n)
{
	if (buf == NULL)
		return sqlMalloc(n);
	if (n == 0) {
		sql_free(buf);
		return NULL;
	}
	if (n >= MALLOC_MAX) {
		fprintf(stderr, "Can't allocate %zu bytes at %s:%d", n,
			__FILE__, __LINE__);
		exit(EXIT_FAILURE);
	}
	size_t size = ROUND8(n);
	if (size == (size_t)sqlMallocSize(buf))
		return buf;
	int64_t *new_buf = buf;
	--new_buf;
	new_buf = xrealloc(new_buf, size + 8);
	new_buf[0] = size;
	new_buf++;
	return new_buf;
}

/*
 * Allocate and zero memory.
 */
void *
sqlMallocZero(u64 n)
{
	void *p = sqlMalloc(n);
	memset(p, 0, (size_t) n);
	return p;
}

/** Allocate and zero memory. */
void *
sqlDbMallocZero(sql * db, u64 n)
{
	(void)db;
	void *p = sqlDbMallocRawNN(n);
	memset(p, 0, (size_t) n);
	return p;
}

void *
sqlDbMallocRawNN(size_t n)
{
	struct sql *db = sql_get();
	assert(db != NULL);
	LookasideSlot *pBuf;
	if (db->lookaside.bDisable == 0) {
		if (n > db->lookaside.sz) {
			db->lookaside.anStat[1]++;
		} else if ((pBuf = db->lookaside.pFree) == 0) {
			db->lookaside.anStat[2]++;
		} else {
			db->lookaside.pFree = pBuf->pNext;
			db->lookaside.nOut++;
			db->lookaside.anStat[0]++;
			if (db->lookaside.nOut > db->lookaside.mxOut) {
				db->lookaside.mxOut = db->lookaside.nOut;
			}
			return (void *)pBuf;
		}
	}
	return sqlMalloc(n);
}

void *
sqlDbRealloc(void *buf, size_t n)
{
	struct sql *db = sql_get();
	assert(db != NULL);
	if (buf == NULL)
		return sqlDbMallocRawNN(n);
	if (is_lookaside(buf)) {
		if (n <= (size_t)db->lookaside.sz)
			return buf;
		void *new_buf = sqlDbMallocRawNN(n);
		memcpy(new_buf, buf, db->lookaside.sz);
		sqlDbFree(buf);
		return new_buf;
	}
	return sqlRealloc(buf, n);
}

char *
sqlDbStrDup(const char *str)
{
	if (str == NULL)
		return NULL;
	size_t size = strlen(str) + 1;
	char *new_str = sqlDbMallocRawNN(size);
	memcpy(new_str, str, size);
	return new_str;
}

char *
sqlDbStrNDup(const char *str, size_t len)
{
	if (str == NULL)
		return NULL;
	char *new_str = sqlDbMallocRawNN(len + 1);
	memcpy(new_str, str, len);
	new_str[len] = '\0';
	return new_str;
}
