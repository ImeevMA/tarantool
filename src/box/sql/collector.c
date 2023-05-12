/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2023, Tarantool AUTHORS, please see AUTHORS file.
 */

#include "sqlInt.h"

struct sql_avl_node {
	char *key;
	size_t n;
	struct sql_avl_node *left;
	struct sql_avl_node *right;
	int height;
};

static inline int
node_height(struct sql_avl_node *x)
{
	int l = x->left == NULL ? 0 : x->left->height;
	int r = x->right == NULL ? 0 : x->right->height;
	return MAX(r, l) + 1;
}

static struct sql_avl_node *
node_new(char *key, size_t n)
{
	struct sql_avl_node *node = xmalloc(sizeof(*node));
	node->key = key;
	node->n = n;
	node->left = NULL;
	node->right = NULL;
	node->height = 1;
	return node;
}

static struct sql_avl_node *
node_rotate_right(struct sql_avl_node *x)
{
	struct sql_avl_node *y = x->left;
	struct sql_avl_node *z = y->right;
	y->right = x;
	x->left = z;
	x->height = node_height(x);
	y->height = node_height(y);
	return y;
}

static struct sql_avl_node *
node_rotate_left(struct sql_avl_node *x)
{
	struct sql_avl_node *y = x->right;
	struct sql_avl_node *z = y->left;
	y->left = x;
	x->right = z;
	x->height = node_height(x);
	y->height = node_height(y);
	return y;
}

static inline int
node_balance(const struct sql_avl_node *x)
{
	if (x == NULL)
		return 0;
	int l = x->left == NULL ? 0 : x->left->height;
	int r = x->right == NULL ? 0 : x->right->height;
	return l - r;
}

static int
node_compare(const char *key_x, size_t n_x, const char *key_y, size_t n_y,
	     struct key_def *key_def)
{
	(void)key_def;
	return strncmp(key_x, key_y, MIN(n_x, n_y));
}

struct sql_avl_node *
node_insert(struct sql_avl_node *x, char *key, size_t n,
	    struct key_def *key_def)
{
	if (x == NULL)
		return node_new(key, n);
	int cmp = node_compare(key, n, x->key, x->n, key_def);
	if (cmp == 0) {
		sql_xfree(key);
		return x;
	}
	if (cmp < 0)
		x->left = node_insert(x->left, key, n, key_def);
	else
		x->right = node_insert(x->right, key, n, key_def);
	int l = x->left == NULL ? 0 : x->left->height;
	int r = x->right == NULL ? 0 : x->right->height;
	x->height = MAX(l, r) + 1;
	int balance = l - r;
	if (balance > 1) {
		int cmp_l = node_compare(key, n, x->left->key, x->left->n,
					 key_def);
		if (cmp_l < 0)
			return node_rotate_right(x);
		if (cmp_l > 0) {
			x->left = node_rotate_left(x->left);
			return node_rotate_right(x);
		}
	} else if (balance < -1) {
		int cmp_r = node_compare(key, n, x->right->key, x->right->n,
					 key_def);
		if (cmp_r > 0)
			return node_rotate_left(x);
		if (cmp_r < 0) {
			x->left = node_rotate_right(x->left);
			return node_rotate_left(x);
		}
	}
	return x;
}
