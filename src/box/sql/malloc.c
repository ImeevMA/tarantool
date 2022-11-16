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

/*
 * Free memory that might be associated with a particular database
 * connection.
 */
void
sqlDbFree(sql * db, void *p)
{
	if (db != NULL) {
		if (is_lookaside(p)) {
			LookasideSlot *pBuf = (LookasideSlot *) p;
			pBuf->pNext = db->lookaside.pFree;
			db->lookaside.pFree = pBuf;
			db->lookaside.nOut--;
			return;
		}
	}
	sql_free(p);
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

/*
 * Allocate and zero memory.  If the allocation fails, make
 * the mallocFailed flag in the connection pointer.
 */
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
	if (db->mallocFailed != 0) {
		fprintf(stderr, "Can't allocate %zu bytes at %s:%d", n,
			__FILE__, __LINE__);
		exit(EXIT_FAILURE);
	}
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
	if (db->mallocFailed != 0) {
		fprintf(stderr, "Can't allocate %zu bytes at %s:%d", n,
			__FILE__, __LINE__);
		exit(EXIT_FAILURE);
	}
	if (buf == NULL)
		return sqlDbMallocRawNN(n);
	if (is_lookaside(buf)) {
		if (n <= (size_t)db->lookaside.sz)
			return buf;
		void *new_buf = sqlDbMallocRawNN(n);
		memcpy(new_buf, buf, db->lookaside.sz);
		sqlDbFree(db, buf);
		return new_buf;
	}
	return sqlRealloc(buf, n);
}

/*
 * Attempt to reallocate p.  If the reallocation fails, then free p
 * and set the mallocFailed flag in the database connection.
 */
void *
sqlDbReallocOrFree(sql * db, void *p, u64 n)
{
	(void)db;
	return sqlDbRealloc(p, n);
}

/*
 * Make a copy of a string in memory obtained from sqlMalloc(). These
 * functions call sqlMallocRaw() directly instead of sqlMalloc(). This
 * is because when memory debugging is turned on, these two functions are
 * called via macros that record the current file and line number in the
 * ThreadData structure.
 */
char *
sqlDbStrDup(sql * db, const char *z)
{
	(void)db;
	char *zNew;
	size_t n;
	if (z == 0) {
		return 0;
	}
	n = strlen(z) + 1;
	zNew = sqlDbMallocRawNN(n);
	memcpy(zNew, z, n);
	return zNew;
}

char *
sqlDbStrNDup(sql * db, const char *z, u64 n)
{
	char *zNew;
	assert(db != 0);
	if (z == 0) {
		return 0;
	}
	assert((n & 0x7fffffff) == n);
	zNew = sqlDbMallocRawNN(n + 1);
	memcpy(zNew, z, (size_t) n);
	zNew[n] = 0;
	return zNew;
}

/*
 * This routine reactivates the memory allocator and clears the
 * db->mallocFailed flag as necessary.
 *
 * The memory allocator is not restarted if there are running
 * VDBEs.
 */
void
sqlOomClear(sql * db)
{
	if (db->mallocFailed && db->nVdbeExec == 0) {
		db->mallocFailed = 0;
		assert(db->lookaside.bDisable > 0);
		db->lookaside.bDisable--;
	}
}
