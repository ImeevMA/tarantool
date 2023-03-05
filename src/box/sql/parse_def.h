#ifndef TARANTOOL_BOX_SQL_PARSE_DEF_H_INCLUDED
#define TARANTOOL_BOX_SQL_PARSE_DEF_H_INCLUDED
/*
 * Copyright 2010-2019, Tarantool AUTHORS, please see AUTHORS file.
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
#include <assert.h>

#include "box/key_def.h"
#include "box/sql.h"

/**
 * This file contains auxiliary structures and functions which
 * are used only during parsing routine (see parse.y).
 * Their main purpose is to assemble common parts of altered
 * entities (such as name, or IF EXISTS clause) and pass them
 * as a one object to further functions.
 *
 * Hierarchy is following:
 *
 * Base structure is ALTER.
 * ALTER is omitted only for CREATE TABLE since table is filled
 * with meta-information just-in-time of parsing:
 * for instance, as soon as field's name and type are recognized
 * they are added to space definition.
 *
 * DROP is general for all existing objects and includes
 * name of object itself, name of parent object (table),
 * IF EXISTS clause and may contain on-drop behaviour
 * (CASCADE/RESTRICT, but now it is always RESTRICT).
 * Hence, it terms of grammar - it is a terminal symbol.
 *
 * RENAME can be applied only to table (at least now, since it is
 * ANSI extension), so it is also terminal symbol.
 *
 * CREATE in turn can be expanded to nonterminal symbol
 * CREATE CONSTRAINT or to terminal CREATE TABLE/INDEX/TRIGGER.
 * CREATE CONSTRAINT unfolds to FOREIGN KEY or UNIQUE/PRIMARY KEY.
 *
 * For instance:
 * ALTER TABLE t ADD CONSTRAINT c FOREIGN KEY REFERENCES t2(id);
 * ALTER *TABLE* -> CREATE ENTITY -> CREATE CONSTRAINT -> CREATE FK
 *
 * CREATE TRIGGER tr1 ...
 * ALTER *TABLE* -> CREATE ENTITY -> CREATE TRIGGER
 *
 * All terminal symbols are stored as a union within
 * parsing context (struct Parse).
 */

/** Type of parsed statement. */
enum sql_ast_type {
	/** Type of the statement is unknown. */
	SQL_AST_TYPE_UNKNOWN = 0,
	/** START TRANSACTION statement. */
	SQL_AST_TYPE_START_TRANSACTION,
	/** COMMIT statement. */
	SQL_AST_TYPE_COMMIT,
	/** ROLLBACK statement. */
	SQL_AST_TYPE_ROLLBACK,
	/** SAVEPOINT statement. */
	SQL_AST_TYPE_SAVEPOINT,
	/** RELEASE SAVEPOINT statement. */
	SQL_AST_TYPE_RELEASE_SAVEPOINT,
	/** ROLLBACK TO SAVEPOINT statement. */
	SQL_AST_TYPE_ROLLBACK_TO_SAVEPOINT,
	/** ALTER TABLE RENAME statement. */
	SQL_AST_TYPE_RENAME,
	/** ALTER TABLE DROP CONSTRAINT statement. */
	SQL_AST_TYPE_DROP_CONSTRAINT,
	/** DROP INDEX statement. */
	SQL_AST_TYPE_DROP_INDEX,
	/** DROP TRIGGER statement. */
	SQL_AST_TYPE_DROP_TRIGGER,
	/** DROP VIEW statement. */
	SQL_AST_TYPE_DROP_VIEW,
	/** DROP TABLE statement. */
	SQL_AST_TYPE_DROP_TABLE,
	/** CREATE TABLE statement. */
	SQL_AST_TYPE_CREATE_TABLE,
	/** CREATE INDEX statement. */
	SQL_AST_TYPE_CREATE_INDEX,
	/** ALTER TABLE ADD COLUMN statement. */
	SQL_AST_TYPE_ADD_COLUMN,
	/** ALTER TABLE ADD CONSTAINT FOREIGN KEY statement. */
	SQL_AST_TYPE_ADD_FOREIGN_KEY,
	/** ALTER TABLE ADD CONSTAINT CHECK statement. */
	SQL_AST_TYPE_ADD_CHECK,
	/** ALTER TABLE ADD CONSTAINT UNIQUE statement. */
	SQL_AST_TYPE_ADD_UNIQUE,
	/** ALTER TABLE ADD CONSTAINT PRIMARY KEY statement. */
	SQL_AST_TYPE_ADD_PRIMARY_KEY,
};

/**
 * Each token coming out of the lexer is an instance of
 * this structure. Tokens are also used as part of an expression.
 */
struct Token {
	/** Text of the token. Not NULL-terminated! */
	const char *z;
	/** Number of characters in this token. */
	unsigned int n;
	bool isReserved;
};

/**
 * An instance of this structure is used by the parser to record both the parse
 * tree for an expression and the span of input text for an expression.
 */
struct ExprSpan {
	/* The expression parse tree. */
	struct Expr *pExpr;
	/* First character of input text. */
	const char *zStart;
	/* One character past the end of input text. */
	const char *zEnd;
};

/** Description of a SAVEPOINT. */
struct sql_ast_savepoint {
	/** Name of the SAVEPOINT. */
	struct Token name;
};

/** Description of ALTER TABLE RENAME statement. */
struct sql_ast_rename {
	/** Name of the table to rename. */
	struct Token old_name;
	/** New name of the table. */
	struct Token new_name;
};

/** Description of ALTER TABLE DROP CONSTRAINT statement. */
struct sql_ast_drop_constraint {
	/** The name of the table which constraint will be dropped. */
	struct Token table_name;
	/** Name of the constraint to drop. */
	struct Token name;
};

/** Description of DROP INDEX statement. */
struct sql_ast_drop_index {
	/** The name of the table which index will be dropped. */
	struct Token table_name;
	/** Name of the index to drop. */
	struct Token index_name;
	/** IF EXISTS flag. */
	bool if_exists;
};

/** Description of DROP TRIGGER statement. */
struct sql_ast_drop_trigger {
	/** Name of the trigger to drop. */
	struct Token name;
	/** IF EXISTS flag. */
	bool if_exists;
};

/** Description of DROP VIEW statement. */
struct sql_ast_drop_view {
	/** Name of the VIEW to drop. */
	struct Token name;
	/** IF EXISTS flag. */
	bool if_exists;
};

/** Description of DROP TABLE statement. */
struct sql_ast_drop_table {
	/** Name of the TABLE to drop. */
	struct Token name;
	/** IF EXISTS flag. */
	bool if_exists;
};

/** Description of the FOREIGN KEY constraint being created. */
struct sql_ast_foreign_key {
	/** List child columns. */
	struct ExprList *child_cols;
	/** List parent columns. */
	struct ExprList *parent_cols;
	/** Name of the parent table. */
	struct Token parent_name;
	/** Constraint name. */
	struct Token name;
	/**
	 * Flag indicating whether the constraint is a column constraint or a
	 * table constraint.
	 */
	bool is_column_constraint;
};

/** FOREIGN KEY descriptions list. */
struct sql_ast_foreign_key_list {
	/** Array containing all FOREIGN KEY descriptions from the list. */
	struct sql_ast_foreign_key *a;
	/** Number of FOREIGN KEY descriptions in the list. */
	uint32_t n;
};

/** Description of the CHECK constraint being created. */
struct sql_ast_check {
	/** Expression. */
	struct ExprSpan expr;
	/** Constraint name. */
	struct Token name;
	/** Column name for column constraint, empty for table constraint. */
	struct Token column_name;
};

/** CHECK descriptions list. */
struct sql_ast_check_list {
	/** Array containing all CHECK descriptions from the list. */
	struct sql_ast_check *a;
	/** Number of CHECK descriptions in the list. */
	uint32_t n;
};

/** Description of the UNIQUE constraint being created. */
struct sql_ast_unique {
	/** Constraint name. */
	struct Token name;
	/** Unique columns. */
	struct ExprList *cols;
};

/** UNIQUE descriptions list. */
struct sql_ast_unique_list {
	/** Array containing all UNIQUE descriptions from the list. */
	struct sql_ast_unique *a;
	/** Number of UNIQUE descriptions in the list. */
	uint32_t n;
};

/** Description of the column being created. */
struct sql_ast_column {
	/** Column name. */
	struct Token name;
	/** Collation name. */
	struct Token collate_name;
	/** Expression for DEFAULT. */
	struct ExprSpan default_expr;
	/** Column data type. */
	enum field_type type;
	/** NULL and NOT NULL constraints. */
	enum on_conflict_action null_action;
	/** Flag to show if nullable action is set. */
	bool is_null_action_set;
};

/** Column descriptions list. */
struct sql_ast_column_list {
	/** Array containing all column descriptions from the list. */
	struct sql_ast_column *a;
	/** Number of column descriptions in the list. */
	uint32_t n;
};

/** Description of CREATE TABLE statement. */
struct sql_ast_create_table {
	/** Description of FOREIGN KEY constraints. */
	struct sql_ast_foreign_key_list foreign_key_list;
	/** Description of CHECK constraints. */
	struct sql_ast_check_list check_list;
	/** Description of UNIQUE constraints. */
	struct sql_ast_unique_list unique_list;
	/** Description of table columns. */
	struct sql_ast_column_list column_list;
	/** Description of created PRIMARY KEY constraint. */
	struct sql_ast_unique primary_key;
	/** Name of the column with AUTOINCREMENT. */
	struct Expr *autoinc_name;
};

/** Description of the CREATE INDEX statement. */
struct sql_ast_create_index {
	/** Index name. */
	struct Token name;
	/** Index columns. */
	struct ExprList *cols;
	/** Source list for the statement. */
	struct SrcList *src_list;
	/** Flag to show if the index is unique. */
	bool is_unique;
	/**
	 * Flag indicating whether to throw an error if an index with the same
	 * name exists.
	 */
	bool if_not_exists;
};

/** Description of ALTER TABLE ADD COLUMN statement. */
struct sql_ast_add_column {
	/** Description of FOREIGN KEY constraints. */
	struct sql_ast_foreign_key_list foreign_key_list;
	/** Description of CHECK constraints. */
	struct sql_ast_check_list check_list;
	/** Description of UNIQUE constraints. */
	struct sql_ast_unique_list unique_list;
	/** Description of created column. */
	struct sql_ast_column column;
	/** Description of created PRIMARY KEY constraint. */
	struct sql_ast_unique primary_key;
	/** Name of the column with AUTOINCREMENT. */
	struct Expr *autoinc_name;
	/** Source list for the statement. */
	struct SrcList *src_list;
};

/** Description of ALTER TABLE ADD CONSTRAINT FOREIGN KEY statement. */
struct sql_ast_add_foreign_key {
	/** Description of FOREIGN KEY constraint. */
	struct sql_ast_foreign_key foreign_key;
	/** Source list for the statement. */
	struct SrcList *src_list;
};

/** Description of ALTER TABLE ADD CONSTRAINT CHECK statement. */
struct sql_ast_add_check {
	/** Description of CHECK constraint. */
	struct sql_ast_check check;
	/** Source list for the statement. */
	struct SrcList *src_list;
};

/** Description of ALTER TABLE ADD CONSTRAINT CHECK statement. */
struct sql_ast_add_unique {
	/** Description of UNIQUE constraints. */
	struct sql_ast_unique unique;
	/** Source list for the statement. */
	struct SrcList *src_list;
};

/** Description of ALTER TABLE ADD CONSTRAINT CHECK statement. */
struct sql_ast_add_primary_key {
	/** Description of created PRIMARY KEY constraint. */
	struct sql_ast_unique primary_key;
	/** Source list for the statement. */
	struct SrcList *src_list;
};

/** A structure describing the AST of the parsed SQL statement. */
struct sql_ast {
	/** Parsed statement type. */
	enum sql_ast_type type;
	union {
		/** Savepoint description for savepoint-related statements. */
		struct sql_ast_savepoint savepoint;
		/** Description of ALTER TABLE RENAME statement. */
		struct sql_ast_rename rename;
		/** Description of ALTER TABLE DROP CONSTRAINT statement. */
		struct sql_ast_drop_constraint drop_constraint;
		/** Description of DROP INDEX statement. */
		struct sql_ast_drop_index drop_index;
		/** Description of DROP TRIGGER statement. */
		struct sql_ast_drop_trigger drop_trigger;
		/** Description of DROP VIEW statement. */
		struct sql_ast_drop_view drop_view;
		/** Description of DROP TABLE statement. */
		struct sql_ast_drop_table drop_table;
		/** Description of CREATE TABLE statement. */
		struct sql_ast_create_table create_table;
		/** Description of CREATE INDEX statement. */
		struct sql_ast_create_index create_index;
		/** Description of ALTER TABLE ADD COLUMN statement. */
		struct sql_ast_add_column add_column;
		/**
		 * Description of ALTER TABLE ADD CONSTRAINT FOREIGN KEY
		 * statement.
		 */
		struct sql_ast_add_foreign_key add_foreign_key;
		/**
		 * Description of ALTER TABLE ADD CONSTRAINT CHECK statement.
		 */
		struct sql_ast_add_check add_check;
		/**
		 * Description of ALTER TABLE ADD CONSTRAINT UNIQUE statement.
		 */
		struct sql_ast_add_unique add_unique;
		/**
		 * Description of ALTER TABLE ADD CONSTRAINT PRIMARY KEY
		 * statement.
		 */
		struct sql_ast_add_primary_key add_primary_key;
	};
};

/** Constant tokens for integer values. */
extern const struct Token sqlIntTokens[];

/** Generate a Token object from a string. */
void
sqlTokenInit(struct Token *p, char *z);

#define Token_nil ((struct Token) {NULL, 0, false})

/**
 * Structure representing foreign keys constraints appeared
 * within CREATE TABLE statement. Used only during parsing.
 */
struct fk_constraint_parse {
	/**
	 * Foreign keys constraint declared in <CREATE TABLE ...>
	 * statement. They must be coded after space creation.
	 */
	struct fk_constraint_def *fk_def;
	/**
	 * If inside <CREATE TABLE> or <ALTER TABLE ADD COLUMN>
	 * statement we want to declare self-referenced FK
	 * constraint, we must delay their resolution until the
	 * end of parsing of all columns.
	 * E.g.: CREATE TABLE t1(id REFERENCES t1(b), b);
	 */
	struct ExprList *selfref_cols;
	/**
	 * Still, self-referenced columns might be NULL, if
	 * we declare FK constraints referencing PK:
	 * CREATE TABLE t1(id REFERENCES t1) - it is a valid case.
	 */
	bool is_self_referenced;
	/** Organize these structs into linked list. */
	struct rlist link;
};

/**
 * Structure representing check constraint appeared within
 * CREATE TABLE statement. Used only during parsing.
 * All allocations are performed on region, so no cleanups are
 * required.
 */
struct ck_constraint_parse {
	/**
	 * Check constraint declared in <CREATE TABLE ...>
	 * statement. Must be coded after space creation.
	 */
	struct ck_constraint_def *ck_def;
	/** Organize these structs into linked list. */
	struct rlist link;
};

/**
 * Possible SQL index types. Note that PK and UNIQUE constraints
 * are implemented as indexes and have their own types:
 * _CONSTRAINT_PK and _CONSTRAINT_UNIQUE.
 */
enum sql_index_type {
	SQL_INDEX_TYPE_NON_UNIQUE = 0,
	SQL_INDEX_TYPE_UNIQUE,
	SQL_INDEX_TYPE_CONSTRAINT_UNIQUE,
	SQL_INDEX_TYPE_CONSTRAINT_PK,
	sql_index_type_MAX
};

enum entity_type {
	ENTITY_TYPE_TABLE = 0,
	ENTITY_TYPE_COLUMN,
	ENTITY_TYPE_VIEW,
	ENTITY_TYPE_INDEX,
	ENTITY_TYPE_TRIGGER,
	ENTITY_TYPE_CK,
	ENTITY_TYPE_FK,
	/**
	 * For assertion checks that constraint definition is
	 * created before initialization of a term constraint.
	 */
	ENTITY_TYPE_CONSTRAINT,
};

enum alter_action {
	ALTER_ACTION_CREATE = 0,
	ALTER_ACTION_DROP,
	ALTER_ACTION_RENAME,
	ALTER_ACTION_ENABLE,
};

struct alter_entity_def {
	/** Type of topmost entity. */
	enum entity_type entity_type;
	/** Action to be performed using current entity. */
	enum alter_action alter_action;
	/** As a rule it is a name of table to be altered. */
	struct SrcList *entity_name;
};

struct enable_entity_def {
	struct alter_entity_def base;
	/** Name of constraint to be enabled/disabled. */
	struct Token name;
	/** A new state to be set for entity found. */
	bool is_enabled;
};

struct create_entity_def {
	struct alter_entity_def base;
	struct Token name;
	/** Statement comes with IF NOT EXISTS clause. */
	bool if_not_exist;
};

struct create_table_def {
	struct create_entity_def base;
	struct space *new_space;
};

struct create_ck_constraint_parse_def {
	/** List of ck_constraint_parse_def objects. */
	struct rlist checks;
};

struct create_fk_constraint_parse_def {
	/** List of fk_constraint_parse_def objects. */
	struct rlist fkeys;
	/**
	 * True if a list of foreign keys is used and should be cleaned up
	 * properly.
	 */
	bool is_used;
};

struct create_view_def {
	struct create_entity_def base;
	/**
	 * Starting position of CREATE VIEW ... statement.
	 * It is used to fetch whole statement, which is
	 * saved as raw string to space options.
	 */
	struct Token *create_start;
	/** List of column aliases (SELECT x AS y ...). */
	struct ExprList *aliases;
	struct Select *select;
};

struct create_trigger_def {
	struct create_entity_def base;
	/** One of TK_BEFORE, TK_AFTER, TK_INSTEAD. */
	int tr_tm;
	/** One of TK_INSERT, TK_UPDATE, TK_DELETE. */
	int op;
	/** Column list if this is an UPDATE trigger. */
	struct IdList *cols;
	/** When clause. */
	struct Expr *when;
};

/** Basic initialisers of parse structures.*/
static inline void
alter_entity_def_init(struct alter_entity_def *alter_def,
		      struct SrcList *entity_name, enum entity_type type,
		      enum alter_action action)
{
	alter_def->entity_name = entity_name;
	alter_def->entity_type = type;
	alter_def->alter_action = action;
}

static inline void
enable_entity_def_init(struct enable_entity_def *enable_def,
		       enum entity_type type, struct SrcList *parent_name,
		       struct Token *name, bool is_enabled)
{
	alter_entity_def_init(&enable_def->base, parent_name, type,
			      ALTER_ACTION_ENABLE);
	enable_def->name = *name;
	enable_def->is_enabled = is_enabled;
}

static inline void
create_entity_def_init(struct create_entity_def *create_def,
		       enum entity_type type, struct SrcList *parent_name,
		       struct Token *name, bool if_not_exist)
{
	alter_entity_def_init(&create_def->base, parent_name, type,
			      ALTER_ACTION_CREATE);
	create_def->name = *name;
	create_def->if_not_exist = if_not_exist;
}

static inline void
create_trigger_def_init(struct create_trigger_def *trigger_def,
			struct SrcList *table_name, struct Token *name,
			int tr_tm, int op, struct IdList *cols,
			struct Expr *when, bool if_not_exists)
{
	create_entity_def_init(&trigger_def->base, ENTITY_TYPE_TRIGGER,
			       table_name, name, if_not_exists);
	trigger_def->tr_tm = tr_tm;
	trigger_def->op = op;
	trigger_def->cols = cols;
	trigger_def->when = when;
}

static inline void
create_table_def_init(struct create_table_def *table_def, struct Token *name,
		      bool if_not_exists)
{
	create_entity_def_init(&table_def->base, ENTITY_TYPE_TABLE, NULL, name,
			       if_not_exists);
}

static inline void
create_ck_constraint_parse_def_init(struct create_ck_constraint_parse_def *def)
{
	rlist_create(&def->checks);
}

static inline void
create_fk_constraint_parse_def_init(struct create_fk_constraint_parse_def *def)
{
	rlist_create(&def->fkeys);
	def->is_used = true;
}

static inline void
create_view_def_init(struct create_view_def *view_def, struct Token *name,
		     struct Token *create, struct ExprList *aliases,
		     struct Select *select, bool if_not_exists)
{
	create_entity_def_init(&view_def->base, ENTITY_TYPE_VIEW, NULL, name,
			       if_not_exists);
	view_def->create_start = create;
	view_def->select = select;
	view_def->aliases = aliases;
}

static inline void
create_fk_constraint_parse_def_destroy(struct create_fk_constraint_parse_def *d)
{
	if (!d->is_used)
		return;
	struct fk_constraint_parse *fk;
	rlist_foreach_entry(fk, &d->fkeys, link)
		sql_expr_list_delete(fk->selfref_cols);
}

/** Save parsed START TRANSACTION statement. */
void
sql_ast_init_start_transaction(struct Parse *parse);

/** Save parsed COMMIT statement. */
void
sql_ast_init_commit(struct Parse *parse);

/** Save parsed ROLLBACK statement. */
void
sql_ast_init_rollback(struct Parse *parse);

/** Save parsed SAVEPOINT statement. */
void
sql_ast_init_savepoint(struct Parse *parse, const struct Token *name);

/** Save parsed RELEASE SAVEPOINT statement. */
void
sql_ast_init_release_savepoint(struct Parse *parse, const struct Token *name);

/** Save parsed ROLLBACK TO SAVEPOINT statement. */
void
sql_ast_init_rollback_to_savepoint(struct Parse *parse,
				   const struct Token *name);

/** Save parsed ALTER TABLE RENAME statement. */
void
sql_ast_init_table_rename(struct Parse *parse, const struct Token *old_name,
			  const struct Token *new_name);

/** Save parsed ALTER TABLE DROP CONSTRAINT statement. */
void
sql_ast_init_constraint_drop(struct Parse *parse,
			     const struct Token *table_name,
			     const struct Token *name);

/** Save parsed DROP INDEX statement. */
void
sql_ast_init_index_drop(struct Parse *parse, const struct Token *table_name,
			const struct Token *index_name, bool if_exists);

/** Save parsed DROP TRIGGER statement. */
void
sql_ast_init_trigger_drop(struct Parse *parse, const struct Token *name,
			  bool if_exists);

/** Save parsed DROP VIEW statement. */
void
sql_ast_init_view_drop(struct Parse *parse, const struct Token *name,
		       bool if_exists);

/** Save parsed DROP TABLE statement. */
void
sql_ast_init_table_drop(struct Parse *parse, const struct Token *name,
			bool if_exists);

/** Save parsed CREATE TABLE statement. */
void
sql_ast_init_create_table(struct Parse *parse);

/** Save parsed CREATE INDEX statement. */
void
sql_ast_init_create_index(struct Parse *parse, struct Token *table_name,
			  const struct Token *index_name, struct ExprList *cols,
			  bool is_unique, bool if_not_exists);

/** Save parsed ADD COLUMN statement. */
void
sql_ast_init_add_column(struct Parse *parse, struct SrcList *table_name,
			struct Token *name, enum field_type type);

/** Save parsed table FOREIGN KEY from ALTER TABLE ADD CONSTRAINT statement. */
void
sql_ast_init_add_foreign_key(struct Parse *parse, struct SrcList *src_list,
			     const struct Token *name,
			     struct ExprList *child_cols,
			     const struct Token *parent_name,
			     struct ExprList *parent_cols);

/** Save parsed table CHECK from ALTER TABLE ADD CONSTRAINT statement. */
void
sql_ast_init_add_check(struct Parse *parse, struct SrcList *table_name,
		       const struct Token *name, struct ExprSpan *expr);

/** Save parsed table UNIQUE from ALTER TABLE ADD CONSTRAINT statement. */
void
sql_ast_init_add_unique(struct Parse *parse, struct SrcList *table_name,
			const struct Token *name, struct ExprList *cols);

/** Save parsed table PRIMARY KEY from ALTER TABLE ADD CONSTRAINT statement. */
void
sql_ast_init_add_primary_key(struct Parse *parse, struct SrcList *table_name,
			     const struct Token *name, struct ExprList *cols);

/** Save parsed column FOREIGN KEY. */
void
sql_ast_save_column_foreign_key(struct Parse *parse, const struct Token *name,
				const struct Token *parent_name,
				struct ExprList *parent_cols);

/** Save parsed table FOREIGN KEY from CREATE TABLE statement. */
void
sql_ast_save_table_foreign_key(struct Parse *parse, const struct Token *name,
			       struct ExprList *child_cols,
			       const struct Token *parent_name,
			       struct ExprList *parent_cols);

/** Save parsed column CHECK. */
void
sql_ast_save_column_check(struct Parse *parse, const struct Token *name,
			  struct ExprSpan *expr);

/** Save parsed table CHECK from CREATE TABLE statement. */
void
sql_ast_save_table_check(struct Parse *parse, const struct Token *name,
			 struct ExprSpan *expr);

/** Save parsed column UNIQUE. */
void
sql_ast_save_column_unique(struct Parse *parse, const struct Token *name);

/** Save parsed table UNIQUE from CREATE TABLE statement. */
void
sql_ast_save_table_unique(struct Parse *parse, const struct Token *name,
			  struct ExprList *cols);

/** Save parsed column PRIMARY KEY. */
void
sql_ast_save_column_primary_key(struct Parse *parse, const struct Token *name,
				enum sort_order sort_order);

/** Save parsed table PRIMARY KEY from CREATE TABLE statement. */
void
sql_ast_save_table_primary_key(struct Parse *parse, const struct Token *name,
			       struct ExprList *cols);

/** Save parsed column from CREATE TABLE statement. */
void
sql_ast_save_table_column(struct Parse *parse, struct Token *name,
			  enum field_type type);

/** Save parsed column AUTOINCREMENT clause. */
void
sql_ast_save_column_autoincrement(struct Parse *parse);

/** Save parsed AUTOINCREMENT clause from table PRIMARY KEY clause. */
void
sql_ast_save_table_autoincrement(struct Parse *parse, struct Expr *column_name);

/** Save parsed column COLLATE clause. */
void
sql_ast_save_column_collate(struct Parse *parse, struct Token *collate_name);

/** Save parsed column DEFAULT clause. */
void
sql_ast_save_column_default(struct Parse *parse, struct ExprSpan *expr);

/** Save parsed column NULL or NOT NULL constraint. */
void
sql_ast_save_column_null_action(struct Parse *parse,
				enum on_conflict_action null_action,
				enum on_conflict_action on_conflict);

#endif /* TARANTOOL_BOX_SQL_PARSE_DEF_H_INCLUDED */
