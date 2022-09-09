/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
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
#include "schema.h"
#include "sequence.h"
#include "assoc.h"
#include "alter.h"
#include "scoped_guard.h"
#include "user.h"
#include "vclock/vclock.h"
#include "fiber.h"
#include "memtx_tx.h"
#include "txn.h"
#include "box.h"
#include "tuple_constraint_def.h"

/**
 * @module Data Dictionary
 *
 * The data dictionary is responsible for storage and caching
 * of system metadata, such as information about existing
 * spaces, indexes, tuple formats. Space and index metadata
 * is called in dedicated spaces, _space and _index respectively.
 * The contents of these spaces is fully cached in a cache of
 * struct space objects.
 *
 * struct space is an in-memory instance representing a single
 * space with its metadata, space data, and methods to manage
 * it.
 */

static struct mh_i32ptr_t *sequences;
/** Public change counter. On its update clients need to fetch
 *  new space data from the instance. */
uint32_t schema_version = 0;

/** Persistent version of the schema, stored in _schema["version"]. */
uint32_t dd_version_id = 0;

struct rlist on_schema_init = RLIST_HEAD_INITIALIZER(on_schema_init);
struct rlist on_alter_space = RLIST_HEAD_INITIALIZER(on_alter_space);
struct rlist on_alter_sequence = RLIST_HEAD_INITIALIZER(on_alter_sequence);
struct rlist on_alter_func = RLIST_HEAD_INITIALIZER(on_alter_func);

struct entity_access entity_access;

/** Return current schema version */
uint32_t
box_schema_version(void)
{
	return schema_version;
}

static int
on_replace_dd_system_space(struct trigger *trigger, void *event)
{
	(void) trigger;
	struct txn *txn = (struct txn *) event;
	if (txn->space_on_replace_triggers_depth > 1) {
		diag_set(ClientError, ER_UNSUPPORTED,
			 "Space on_replace trigger", "DDL operations");
		return -1;
	}
	memtx_tx_acquire_ddl(txn);
	return 0;
}

/** A wrapper around space_new() for data dictionary spaces. */
static void
sc_space_new(uint32_t id, const char *name,
	     struct key_part_def *key_parts,
	     uint32_t key_part_count,
	     struct trigger *replace_trigger)
{
	struct key_def *key_def = key_def_new(key_parts, key_part_count, false);
	if (key_def == NULL)
		diag_raise();
	auto key_def_guard =
		make_scoped_guard([=] { key_def_delete(key_def); });
	struct index_def *index_def = index_def_new(id, /* space id */
						    0 /* index id */,
						    "primary", /* name */
						    strlen("primary"),
						    TREE /* index type */,
						    &index_opts_default,
						    key_def, NULL);
	if (index_def == NULL)
		diag_raise();
	auto index_def_guard =
		make_scoped_guard([=] { index_def_delete(index_def); });
	struct space_def *def =
		space_def_new_xc(id, ADMIN, 0, name, strlen(name), "memtx",
				 strlen("memtx"), &space_opts_default, NULL, 0);
	auto def_guard = make_scoped_guard([=] { space_def_delete(def); });
	struct rlist key_list;
	rlist_create(&key_list);
	rlist_add_entry(&key_list, index_def, link);
	struct space *space = space_new_xc(def, &key_list);
	space_cache_replace(NULL, space);
	if (replace_trigger)
		trigger_add(&space->on_replace, replace_trigger);
	struct trigger *t = (struct trigger *) malloc(sizeof(*t));
	trigger_create(t, on_replace_dd_system_space, NULL, (trigger_f0) free);
	trigger_add(&space->on_replace, t);
	/*
	 * Data dictionary spaces are fully built since:
	 * - they contain data right from the start
	 * - they are fully operable already during recovery
	 * - if there is a record in the snapshot which mandates
	 *   addition of a new index to a system space, this
	 *   index is built tuple-by-tuple, not in bulk, which
	 *   ensures validation of tuples when starting from
	 *   a snapshot of older version.
	 */
	init_system_space(space);
}

int
schema_find_id(uint32_t system_space_id, uint32_t index_id,
	       const char *name, uint32_t len, uint32_t *object_id)
{
	if (len > BOX_NAME_MAX) {
		*object_id = BOX_ID_NIL;
		return 0;
	}
	struct space *space = space_cache_find(system_space_id);
	if (space == NULL)
		return -1;
	if (!space_is_memtx(space)) {
		diag_set(ClientError, ER_UNSUPPORTED,
			 space->engine->name, "system data");
		return -1;
	}
	struct index *index = index_find(space, index_id);
	if (index == NULL)
		return -1;
	uint32_t size = mp_sizeof_str(len);
	struct region *region = &fiber()->gc;
	uint32_t used = region_used(region);
	char *key = (char *)region_alloc(region, size);
	if (key == NULL) {
		diag_set(OutOfMemory, size, "region", "key");
		return -1;
	}
	mp_encode_str(key, name, len);
	struct iterator *it = index_create_iterator(index, ITER_EQ, key, 1);
	if (it == NULL) {
		region_truncate(region, used);
		return -1;
	}
	struct tuple *tuple;
	int rc = iterator_next(it, &tuple);
	if (rc == 0) {
		/* id is always field #1 */
		if (tuple == NULL)
			*object_id = BOX_ID_NIL;
		else if (tuple_field_u32(tuple, 0, object_id) != 0)
			rc = -1;
	}
	iterator_delete(it);
	region_truncate(region, used);
	return rc;
}

/**
 * Initialize a prototype for the two mandatory data
 * dictionary spaces and create a cache entry for them.
 * When restoring data from the snapshot these spaces
 * will get altered automatically to their actual format.
 */
void
schema_init(void)
{
	struct key_part_def key_parts[3];
	for (uint32_t i = 0; i < lengthof(key_parts); i++)
		key_parts[i] = key_part_def_default;

	/* Initialize the space cache. */
	space_cache_init();
	func_cache_init();
	sequences = mh_i32ptr_new();
	/*
	 * Create surrogate space objects for the mandatory system
	 * spaces (the primal eggs from which we get all the
	 * chicken). Their definitions will be overwritten by the
	 * data in the snapshot, and they will thus be
	 * *re-created* during recovery.  Note, the index type
	 * must be TREE and space identifiers must be the smallest
	 * one to ensure that these spaces are always recovered
	 * (and re-created) first.
	 */
	/* _schema - key/value space with schema description */
	key_parts[0].fieldno = 0;
	key_parts[0].type = FIELD_TYPE_STRING;
	sc_space_new(BOX_SCHEMA_ID, "_schema", key_parts, 1,
		     &on_replace_schema);

	/* _collation - collation description. */
	key_parts[0].fieldno = 0;
	key_parts[0].type = FIELD_TYPE_UNSIGNED;
	sc_space_new(BOX_COLLATION_ID, "_collation", key_parts, 1,
		     &on_replace_collation);

	/* _space - home for all spaces. */
	sc_space_new(BOX_SPACE_ID, "_space", key_parts, 1,
		     &alter_space_on_replace_space);

	/* _truncate - auxiliary space for triggering space truncation. */
	sc_space_new(BOX_TRUNCATE_ID, "_truncate", key_parts, 1,
		     &on_replace_truncate);

	/* _sequence - definition of all sequence objects. */
	sc_space_new(BOX_SEQUENCE_ID, "_sequence", key_parts, 1,
		     &on_replace_sequence);

	/* _sequence_data - current sequence value. */
	sc_space_new(BOX_SEQUENCE_DATA_ID, "_sequence_data", key_parts, 1,
		     &on_replace_sequence_data);

	/* _space_seq - association space <-> sequence. */
	sc_space_new(BOX_SPACE_SEQUENCE_ID, "_space_sequence", key_parts, 1,
		     &on_replace_space_sequence);

	/* _user - all existing users */
	sc_space_new(BOX_USER_ID, "_user", key_parts, 1, &on_replace_user);

	/* _func - all executable objects on which one can have grants */
	sc_space_new(BOX_FUNC_ID, "_func", key_parts, 1, &on_replace_func);
	/*
	 * _priv - association user <-> object
	 * The real index is defined in the snapshot.
	 */
	sc_space_new(BOX_PRIV_ID, "_priv", key_parts, 1, &on_replace_priv);
	/*
	 * _cluster - association instance uuid <-> instance id
	 * The real index is defined in the snapshot.
	 */
	sc_space_new(BOX_CLUSTER_ID, "_cluster", key_parts, 1,
		     &on_replace_cluster);

	/* _trigger - all existing SQL triggers. */
	key_parts[0].fieldno = 0;
	key_parts[0].type = FIELD_TYPE_STRING;
	sc_space_new(BOX_TRIGGER_ID, "_trigger", key_parts, 1,
		     &on_replace_trigger);

	/* _index - definition of all space indexes. */
	key_parts[0].fieldno = 0; /* space id */
	key_parts[0].type = FIELD_TYPE_UNSIGNED;
	key_parts[1].fieldno = 1; /* index id */
	key_parts[1].type = FIELD_TYPE_UNSIGNED;
	sc_space_new(BOX_INDEX_ID, "_index", key_parts, 2,
		     &alter_space_on_replace_index);

	/* _fk_сonstraint - foreign keys constraints. */
	key_parts[0].fieldno = 0; /* constraint name */
	key_parts[0].type = FIELD_TYPE_STRING;
	key_parts[1].fieldno = 1; /* child space */
	key_parts[1].type = FIELD_TYPE_UNSIGNED;
	sc_space_new(BOX_FK_CONSTRAINT_ID, "_fk_constraint", key_parts, 2,
		     &on_replace_fk_constraint);

	/* _ck_сonstraint - check constraints. */
	key_parts[0].fieldno = 0; /* space id */
	key_parts[0].type = FIELD_TYPE_UNSIGNED;
	key_parts[1].fieldno = 1; /* constraint name */
	key_parts[1].type = FIELD_TYPE_STRING;
	sc_space_new(BOX_CK_CONSTRAINT_ID, "_ck_constraint", key_parts, 2,
		     &on_replace_ck_constraint);

	/* _func_index - check constraints. */
	key_parts[0].fieldno = 0; /* space id */
	key_parts[0].type = FIELD_TYPE_UNSIGNED;
	key_parts[1].fieldno = 1; /* index id */
	key_parts[1].type = FIELD_TYPE_UNSIGNED;
	sc_space_new(BOX_FUNC_INDEX_ID, "_func_index", key_parts, 2,
		     &on_replace_func_index);

	/*
	 * _vinyl_deferred_delete - blackhole that is needed
	 * for writing deferred DELETE statements generated by
	 * vinyl compaction tasks to WAL.
	 *
	 * There is an intricate ordering dependency between
	 * recovery of this system space and initialization of
	 * the vinyl engine, when we set an on_replace trigger
	 * on the space. To resolve this dependency, we create
	 * a space stub in schema_init(), then set a trigger in
	 * engine_begin_initial_recovery(), which is called next,
	 * then recover WAL rows, executing the trigger for each
	 * of them.
	 */
	{
		const char *engine = "blackhole";
		const char *name = "_vinyl_deferred_delete";
		struct space_opts opts = space_opts_default;
		opts.group_id = GROUP_LOCAL;
		struct space_def *def;
		def = space_def_new_xc(BOX_VINYL_DEFERRED_DELETE_ID, ADMIN, 0,
				       name, strlen(name), engine,
				       strlen(engine), &opts, NULL, 0);
		auto def_guard = make_scoped_guard([=] {
			space_def_delete(def);
		});
		RLIST_HEAD(key_list);
		struct space *space = space_new_xc(def, &key_list);
		space_cache_replace(NULL, space);
		init_system_space(space);
	}

	/*
	 * Run the triggers right after creating all the system
	 * space stubs.
	 */
	trigger_run(&on_schema_init, NULL);
}

void
schema_free(void)
{
	space_cache_destroy();
	func_cache_destroy();

	while (mh_size(sequences) > 0) {
		mh_int_t i = mh_first(sequences);

		struct sequence *seq = ((struct sequence *)
					mh_i32ptr_node(sequences, i)->val);
		sequence_cache_delete(seq->def->id);
	}
	mh_i32ptr_delete(sequences);
}

int
schema_find_grants(const char *type, uint32_t id, bool *out)
{
	struct space *priv = space_cache_find(BOX_PRIV_ID);
	if (priv == NULL)
		return -1;

	/** "object" index */
	if (!space_is_memtx(priv)) {
		diag_set(ClientError, ER_UNSUPPORTED,
			 priv->engine->name, "system data");
		return -1;
	}
	struct index *index = index_find(priv, 2);
	if (index == NULL)
		return -1;
	/*
	 * +10 = max(mp_sizeof_uint32) +
	 *       max(mp_sizeof_strl(uint32)).
	 */
	char key[GRANT_NAME_MAX + 10];
	assert(strlen(type) <= GRANT_NAME_MAX);
	mp_encode_uint(mp_encode_str(key, type, strlen(type)), id);
	struct iterator *it = index_create_iterator(index, ITER_EQ, key, 2);
	if (it == NULL)
		return -1;
	IteratorGuard iter_guard(it);
	struct tuple *tuple;
	if (iterator_next(it, &tuple) != 0)
		return -1;
	*out = (tuple != NULL);
	return 0;
}

struct sequence *
sequence_by_id(uint32_t id)
{
	mh_int_t k = mh_i32ptr_find(sequences, id, NULL);
	if (k == mh_end(sequences))
		return NULL;
	return (struct sequence *) mh_i32ptr_node(sequences, k)->val;
}

struct sequence *
sequence_cache_find(uint32_t id)
{
	struct sequence *seq = sequence_by_id(id);
	if (seq == NULL)
		diag_set(ClientError, ER_NO_SUCH_SEQUENCE, int2str(id));
	return seq;
}

void
sequence_cache_insert(struct sequence *seq)
{
	assert(sequence_by_id(seq->def->id) == NULL);

	struct mh_i32ptr_node_t node = { seq->def->id, seq };
	mh_i32ptr_put(sequences, &node, NULL, NULL);
}

void
sequence_cache_delete(uint32_t id)
{
	mh_int_t k = mh_i32ptr_find(sequences, id, NULL);
	if (k != mh_end(sequences))
		mh_i32ptr_del(sequences, k, NULL);
}

const char *
schema_find_name(enum schema_object_type type, uint32_t object_id)
{
	switch (type) {
	case SC_UNIVERSE:
	case SC_ENTITY_SPACE:
	case SC_ENTITY_FUNCTION:
	case SC_ENTITY_SEQUENCE:
	case SC_ENTITY_ROLE:
	case SC_ENTITY_USER:
		return "";
	case SC_SPACE:
		{
			struct space *space = space_by_id(object_id);
			if (space != NULL)
				return space->def->name;
			diag_set(ClientError, ER_NO_SUCH_SPACE,
				 tt_sprintf("%d", object_id));
			break;
		}
	case SC_FUNCTION:
		{
			struct func *func = func_by_id(object_id);
			if (func != NULL)
				return func->def->name;
			diag_set(ClientError, ER_NO_SUCH_FUNCTION,
				 tt_sprintf("%d", object_id));
			break;
		}
	case SC_SEQUENCE:
		{
			struct sequence *seq = sequence_by_id(object_id);
			if (seq != NULL)
				return seq->def->name;
			diag_set(ClientError, ER_NO_SUCH_SEQUENCE,
				 tt_sprintf("%d", object_id));
			break;
		}
	case SC_ROLE:
		{
			struct user *role = user_by_id(object_id);
			if (role != NULL)
				return role->def->name;
			diag_set(ClientError, ER_NO_SUCH_ROLE,
				 tt_sprintf("%d", object_id));
			break;
		}
	case SC_USER:
		{
			struct user *user = user_by_id(object_id);
			if (user != NULL)
				return user->def->name;
			diag_set(ClientError, ER_NO_SUCH_USER,
				 tt_sprintf("%d", object_id));
			break;
		}
	default:
		unreachable();
	}
	return NULL;
}

/**
 * Drop the constraint or foreign key. If fieldno is equal to UINT32_MAX then
 * the tuple constraint or tuple foreign key is dropped, otherwise the field
 * constraint or field foreign key is dropped.
 */
int
constraint_drop(uint32_t space_id, const char *name, uint32_t name_len,
		uint32_t fieldno, enum tuple_constraint_type type)
{
	char key[16];
	char *key_end = mp_encode_array(key, 1);
	key_end = mp_encode_uint(key_end, space_id);
	struct tuple *tuple;
	if (box_index_get(BOX_SPACE_ID, 0, key, key_end, &tuple) != 0)
		return -1;
	assert(tuple != NULL);
	const char *data = tuple_data(tuple);
	const char *data_end = data;
	mp_next(&data_end);

	const char *str;
	if (fieldno != UINT32_MAX) {
		str = tuple_field(tuple, BOX_SPACE_FIELD_FORMAT);
		assert(str != NULL);
		mp_decode_array(&str);
		for (uint32_t i = 0; i < fieldno; ++i)
			mp_next(&str);
	} else {
		str = tuple_field(tuple, BOX_SPACE_FIELD_OPTS);
		assert(str != NULL);
	}

	const char *prev = str;
	uint32_t count = mp_decode_map(&str);
	const char *mid = str;
	uint32_t id = 0;
	const char *constraints = str;
	const char *constraint_type =
		type == CONSTR_FKEY ? "foreign_key" : "constraint";
	for (; id < count; ++id) {
		uint32_t len;
		const char *tmp = mp_decode_str(&str, &len);
		if (len == strlen(constraint_type) &&
		    memcmp(tmp, constraint_type, len) == 0)
			break;
		mp_next(&str);
		constraints = str;
	}
	if (id == count) {
		int errcode = type == CONSTR_FKEY ? ER_NO_SUCH_FOREIGN_KEY :
			      ER_NO_SUCH_CONSTRAINT;
		diag_set(ClientError, errcode, name,
			 space_name(space_by_id(space_id)));
		return -1;
	}

	uint32_t constraint_count = mp_decode_map(&str);
	const char *constraint_begin = str;
	const char *constraint = str;
	uint32_t constraint_id = 0;
	for (; constraint_id < constraint_count; ++constraint_id) {
		uint32_t len;
		const char *tmp = mp_decode_str(&str, &len);
		if (name_len == len && memcmp(tmp, name, len) == 0)
			break;
		mp_next(&str);
		constraint = str;
	}
	if (constraint_id == constraint_count) {
		int errcode = type == CONSTR_FKEY ? ER_NO_SUCH_FOREIGN_KEY :
			      ER_NO_SUCH_CONSTRAINT;
		diag_set(ClientError, errcode, name,
			 space_name(space_by_id(space_id)));
		return -1;
	}
	const char *post = constraint;
	mp_next(&post);
	mp_next(&post);
	--constraint_count;
	if (constraint_count == 0)
		--count;

	uint32_t size = (data_end - data) - (post - constraint);
	char *new_data = (char *)xmalloc(size);
	char *new_data_end = new_data;
	uint32_t prev_size = prev - data;
	memcpy(new_data_end, data, prev_size);
	new_data_end += prev_size;
	new_data_end = mp_encode_map(new_data_end, count);
	uint32_t mid_size = constraints - mid;
	memcpy(new_data_end, mid, mid_size);
	new_data_end += mid_size;
	if (constraint_count > 0) {
		new_data_end = mp_encode_str0(new_data_end, constraint_type);
		new_data_end = mp_encode_map(new_data_end, constraint_count);
		uint32_t constraint_size = constraint - constraint_begin;
		memcpy(new_data_end, constraint_begin, constraint_size);
		new_data_end += constraint_size;
	}
	uint32_t post_size = data_end - post;
	memcpy(new_data_end, post, post_size);
	new_data_end += post_size;
	int rc = box_replace(BOX_SPACE_ID, new_data, new_data_end, NULL);
	free(new_data);
	return rc;
}

int
box_tuple_constraint_drop(uint32_t space_id, const char *name,
			  uint32_t name_len)
{
	return constraint_drop(space_id, name, name_len, UINT32_MAX,
			       CONSTR_FUNC);
}

int
box_field_constraint_drop(uint32_t space_id, const char *name,
			  uint32_t name_len, uint32_t fieldno)
{
	return constraint_drop(space_id, name, name_len, fieldno, CONSTR_FUNC);
}

int
box_tuple_foreign_key_drop(uint32_t space_id, const char *name,
			   uint32_t name_len)
{
	return constraint_drop(space_id, name, name_len, UINT32_MAX,
			       CONSTR_FKEY);
}

int
box_field_foreign_key_drop(uint32_t space_id, const char *name,
			   uint32_t name_len, uint32_t fieldno)
{
	return constraint_drop(space_id, name, name_len, fieldno, CONSTR_FKEY);
}

/* Value of new constraint or foreign key. */
union constraint_value {
	/* ID of a function for constraint. */
	uint32_t func_id;
	struct {
		/* Mapping for complex foreign key. */
		const char *mapping;
		/* Size of mapping for complex foreign key. */
		uint32_t mapping_size;
		/* Foreign space ID. */
		uint32_t space_id;
		/* Fieldno of foreign field for simple foreign key. */
		uint32_t fieldno;
	};
};

/**
 * Create constraint or foreign key. If fieldno is equal to UINT32_MAX then the
 * tuple constraint or tuple foreign key is created, otherwise the field
 * constraint or field foreign key is created.
 */
static int
constraint_create(uint32_t space_id, const char *name, uint32_t name_len,
		  uint32_t fieldno, const union constraint_value *value,
		  enum tuple_constraint_type type)
{
	char key[16];
	char *key_end = mp_encode_array(key, 1);
	key_end = mp_encode_uint(key_end, space_id);
	struct tuple *tuple;
	if (box_index_get(BOX_SPACE_ID, 0, key, key_end, &tuple) != 0)
		return -1;
	assert(tuple != NULL);
	const char *data = tuple_data(tuple);
	const char *data_end = data;
	mp_next(&data_end);

	const char *str;
	if (fieldno != UINT32_MAX) {
		str = tuple_field(tuple, BOX_SPACE_FIELD_FORMAT);
		assert(str != NULL);
		mp_decode_array(&str);
		for (uint32_t i = 0; i < fieldno; ++i)
			mp_next(&str);
	} else {
		str = tuple_field(tuple, BOX_SPACE_FIELD_OPTS);
		assert(str != NULL);
	}

	const char *prev = str;
	uint32_t count = mp_decode_map(&str);
	const char *post = str;
	uint32_t id = 0;
	const char *type_str = type == CONSTR_FUNC ? "constraint" :
			       "foreign_key";
	uint32_t type_len = strlen(type_str);
	for (; id < count; ++id) {
		uint32_t len;
		const char *tmp = mp_decode_str(&str, &len);
		if (len == type_len && memcmp(tmp, type_str, len) == 0)
			break;
		mp_next(&str);
	}
	uint32_t constraint_count;
	uint32_t size = (data_end - data) + mp_sizeof_str(name_len);
	if (id == count) {
		++count;
		constraint_count = 0;
		size += mp_sizeof_map(count) + mp_sizeof_str(type_len);
	} else {
		prev = str;
		constraint_count = mp_decode_map(&str);
		assert(constraint_count > 0);
		post = str;
	}
	++constraint_count;
	if (type == CONSTR_FUNC) {
		size += mp_sizeof_uint(value->func_id);
	} else if (fieldno != UINT32_MAX) {
		size += mp_sizeof_map(2) + mp_sizeof_str(strlen("space")) +
			mp_sizeof_str(strlen("field")) +
			mp_sizeof_uint(value->space_id) +
			mp_sizeof_uint(value->fieldno);
	} else {
		size += mp_sizeof_map(2) + mp_sizeof_str(strlen("space")) +
			mp_sizeof_str(strlen("field")) +
			mp_sizeof_uint(value->space_id) + value->mapping_size;
	}
	size += mp_sizeof_map(constraint_count);
	char *new_data = (char *)xmalloc(size);
	char *new_data_end = new_data;
	uint32_t prev_size = prev - data;
	memcpy(new_data_end, data, prev_size);
	new_data_end += prev_size;
	if (constraint_count == 1) {
		new_data_end = mp_encode_map(new_data_end, count);
		new_data_end = mp_encode_str0(new_data_end, type_str);
	}
	new_data_end = mp_encode_map(new_data_end, constraint_count);
	new_data_end = mp_encode_str(new_data_end, name, name_len);
	if (type == CONSTR_FUNC) {
		new_data_end = mp_encode_uint(new_data_end, value->func_id);
	} else if (fieldno != UINT32_MAX) {
		new_data_end = mp_encode_map(new_data_end, 2);
		new_data_end = mp_encode_str0(new_data_end, "space");
		new_data_end = mp_encode_uint(new_data_end, value->space_id);
		new_data_end = mp_encode_str0(new_data_end, "field");
		new_data_end = mp_encode_uint(new_data_end, value->fieldno);
	} else {
		new_data_end = mp_encode_map(new_data_end, 2);
		new_data_end = mp_encode_str0(new_data_end, "space");
		new_data_end = mp_encode_uint(new_data_end, value->space_id);
		new_data_end = mp_encode_str0(new_data_end, "field");
		memcpy(new_data_end, value->mapping, value->mapping_size);
		new_data_end += value->mapping_size;
	}
	uint32_t post_size = data_end - post;
	memcpy(new_data_end, post, post_size);
	new_data_end += post_size;
	int rc = box_replace(BOX_SPACE_ID, new_data, new_data_end, NULL);
	free(new_data);
	return rc;
}

int
box_tuple_constraint_create(uint32_t space_id, const char *name,
			    uint32_t name_len, uint32_t func_id)
{
	union constraint_value value = {.func_id = func_id};
	return constraint_create(space_id, name, name_len, UINT32_MAX, &value,
				 CONSTR_FUNC);
}

int
box_field_constraint_create(uint32_t space_id, const char *name,
			    uint32_t name_len, uint32_t fieldno,
			    uint32_t func_id)
{
	union constraint_value value = {.func_id = func_id};
	return constraint_create(space_id, name, name_len, fieldno, &value,
				 CONSTR_FUNC);
}

int
box_tuple_foreign_key_create(uint32_t space_id, const char *name,
			     uint32_t name_len, uint32_t parent_id,
			     const char *mapping, uint32_t mapping_size)
{
	union constraint_value value;
	value.mapping = mapping;
	value.mapping_size = mapping_size;
	value.space_id = parent_id;
	return constraint_create(space_id, name, name_len, UINT32_MAX, &value,
				 CONSTR_FKEY);
}

int
box_field_foreign_key_create(uint32_t space_id, const char *name,
			     uint32_t name_len, uint32_t fieldno,
			     uint32_t parent_id, uint32_t parent_fieldno)
{
	union constraint_value value;
	value.fieldno = parent_fieldno;
	value.space_id = parent_id;
	return constraint_create(space_id, name, name_len, fieldno, &value,
				 CONSTR_FKEY);
}
