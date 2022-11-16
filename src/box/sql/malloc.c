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

/*
 * Like realloc().  Resize an allocation previously obtained from
 * sqlMemMalloc().
 *
 * For this low-level interface, we know that pPrior!=0.  Cases where
 * pPrior==0 while have been intercepted by higher-level routine and
 * redirected to sql_sized_malloc.  Similarly, we know that nByte>0 because
 * cases where nByte<=0 will have been intercepted by higher-level
 * routines and redirected to sql_sized_free.
 */
static void *
sql_sized_realloc(void *pPrior, int nByte)
{
	sql_int64 *p = (sql_int64 *) pPrior;
	assert(pPrior != 0 && nByte > 0);
	assert(nByte == ROUND8(nByte));	/* EV: R-46199-30249 */
	p--;
	p = realloc(p, nByte + 8);
	if (p == NULL) {
		sql_get()->mallocFailed = 1;
		diag_set(OutOfMemory, nByte, "realloc", "p");
		return NULL;
	}
	p[0] = nByte;
	p++;
	return (void *)p;
}

static_assert(0x7fffff00 < SIZE_MAX);

void *
sqlMalloc(size_t n)
{
	if (n >= 0x7fffff00) {
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

/*
 * Change the size of an existing memory allocation
 */
void *
sqlRealloc(void *pOld, u64 nBytes)
{
	int nOld, nNew;
	void *pNew;
	if (pOld == 0) {
		return sqlMalloc(nBytes);	/* IMP: R-04300-56712 */
	}
	if (nBytes == 0) {
		sql_free(pOld);	/* IMP: R-26507-47431 */
		return 0;
	}
	if (nBytes >= 0x7fffff00) {
		/* The 0x7ffff00 limit term is explained in comments on sqlMalloc() */
		return 0;
	}
	nOld = sqlMallocSize(pOld);
	nNew = ROUND8((int)nBytes);
	if (nOld == nNew)
		pNew = pOld;
	else
		pNew = sql_sized_realloc(pOld, nNew);
	assert(EIGHT_BYTE_ALIGNMENT(pNew));	/* IMP: R-11148-40995 */
	return pNew;
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
	void *p = sqlDbMallocRawNN(db, n);
	if (p)
		memset(p, 0, (size_t) n);
	return p;
}

/*
 * Allocate memory, either lookaside (if possible) or heap.
 * If the allocation fails, set the mallocFailed flag in
 * the connection pointer.
 */
void *
sqlDbMallocRawNN(sql * db, u64 n)
{
	assert(db != NULL);
	LookasideSlot *pBuf;
	if (db->lookaside.bDisable == 0) {
		assert(db->mallocFailed == 0);
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
	} else if (db->mallocFailed) {
		return 0;
	}
	return sqlMalloc(n);
}

/* Forward declaration */
static SQL_NOINLINE void *dbReallocFinish(sql * db, void *p, u64 n);

/*
 * Resize the block of memory pointed to by p to n bytes. If the
 * resize fails, set the mallocFailed flag in the connection object.
 */
void *
sqlDbRealloc(sql * db, void *p, u64 n)
{
	assert(db != 0);
	if (p == 0)
		return sqlDbMallocRawNN(db, n);
	if (is_lookaside(p) && n <= db->lookaside.sz)
		return p;
	return dbReallocFinish(db, p, n);
}

static SQL_NOINLINE void *
dbReallocFinish(sql * db, void *p, u64 n)
{
	void *pNew = 0;
	assert(db != 0);
	assert(p != 0);
	if (db->mallocFailed == 0) {
		if (is_lookaside(p)) {
			pNew = sqlDbMallocRawNN(db, n);
			if (pNew) {
				memcpy(pNew, p, db->lookaside.sz);
				sqlDbFree(db, p);
			}
		} else {
			pNew = sqlRealloc(p, n);
			if (!pNew)
				sqlOomFault(db);
		}
	}
	return pNew;
}

/*
 * Attempt to reallocate p.  If the reallocation fails, then free p
 * and set the mallocFailed flag in the database connection.
 */
void *
sqlDbReallocOrFree(sql * db, void *p, u64 n)
{
	void *pNew;
	pNew = sqlDbRealloc(db, p, n);
	if (!pNew) {
		sqlDbFree(db, p);
	}
	return pNew;
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
	char *zNew;
	size_t n;
	if (z == 0) {
		return 0;
	}
	n = strlen(z) + 1;
	zNew = sqlDbMallocRawNN(db, n);
	if (zNew) {
		memcpy(zNew, z, n);
	}
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
	zNew = sqlDbMallocRawNN(db, n + 1);
	if (zNew) {
		memcpy(zNew, z, (size_t) n);
		zNew[n] = 0;
	}
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
