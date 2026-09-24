/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 */
#pragma once

#include "box/field_def.h"

/**
 * The SQL value type. Note that the relative position is significant and must
 * be the same as in `enum field_type`, as it is used to compare scalar values
 * of different types.
 */
enum sql_type {
	/** The type is unknown until the value is evaluated. */
	SQL_TYPE_UNKNOWN = 0,
	/** A value of any type. */
	SQL_TYPE_ANY,
	/** An unsigned integer. */
	SQL_TYPE_UNSIGNED,
	/** A string. */
	SQL_TYPE_STRING,
	/** A number of any representation. */
	SQL_TYPE_NUMBER,
	/** A double. */
	SQL_TYPE_DOUBLE,
	/** A signed integer. */
	SQL_TYPE_INTEGER,
	/** A boolean. */
	SQL_TYPE_BOOLEAN,
	/** A binary string. */
	SQL_TYPE_VARBINARY,
	/** A scalar of any representation. */
	SQL_TYPE_SCALAR,
	/** A decimal. */
	SQL_TYPE_DECIMAL,
	/** A UUID. */
	SQL_TYPE_UUID,
	/** A date and time. */
	SQL_TYPE_DATETIME,
	/** A time interval. */
	SQL_TYPE_INTERVAL,
	/** An array. */
	SQL_TYPE_ARRAY,
	/** A map. */
	SQL_TYPE_MAP,
};

/**
 * Return the storage type of a value of the given SQL type. The type UNKNOWN is
 * reported as ANY, since a value of an unknown type can have any type at run
 * time.
 */
enum field_type
sql_type_to_field_type(enum sql_type type);
