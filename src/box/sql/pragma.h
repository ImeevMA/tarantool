/** List of ID of pragmas. */
enum
{
	PRAGMA_COLLATION_LIST = 0,
	PRAGMA_FOREIGN_KEY_LIST,
	PRAGMA_INDEX_INFO,
	PRAGMA_INDEX_LIST,
	PRAGMA_STATS,
	PRAGMA_TABLE_INFO,
};

/**
 * Column names and types for pragmas. The type of the column is
 * the following value after its name.
 */
static const char *const pragCName[] = {
	/* Used by: table_info */
	/*   0 */ "cid",
	/*   1 */ "integer",
	/*   2 */ "name",
	/*   3 */ "text",
	/*   4 */ "type",
	/*   5 */ "text",
	/*   6 */ "notnull",
	/*   7 */ "integer",
	/*   8 */ "pk",
	/*   9 */ "integer",
	/* Used by: stats */
	/*  10 */ "table",
	/*  11 */ "text",
	/*  12 */ "index",
	/*  13 */ "text",
	/*  14 */ "width",
	/*  15 */ "integer",
	/*  16 */ "height",
	/*  17 */ "integer",
	/* Used by: index_info */
	/*  18 */ "seqno",
	/*  19 */ "integer",
	/*  20 */ "cid",
	/*  21 */ "integer",
	/*  22 */ "name",
	/*  23 */ "text",
	/*  24 */ "desc",
	/*  25 */ "integer",
	/*  26 */ "coll",
	/*  27 */ "text",
	/*  28 */ "type",
	/*  29 */ "text",
	/* Used by: index_list */
	/*  30 */ "seq",
	/*  31 */ "integer",
	/*  32 */ "name",
	/*  33 */ "text",
	/*  34 */ "unique",
	/*  35 */ "integer",
	/* Used by: collation_list */
	/*  36 */ "seq",
	/*  37 */ "integer",
	/*  38 */ "name",
	/*  39 */ "text",
	/* Used by: foreign_key_list */
	/*  40 */ "id",
	/*  41 */ "integer",
	/*  42 */ "seq",
	/*  43 */ "integer",
	/*  44 */ "table",
	/*  45 */ "text",
	/*  46 */ "from",
	/*  47 */ "text",
	/*  48 */ "to",
	/*  49 */ "text",
	/*  50 */ "on_update",
	/*  51 */ "text",
	/*  52 */ "on_delete",
	/*  53 */ "text",
	/*  54 */ "match",
	/*  55 */ "text",
};

/** Definitions of all built-in pragmas */
struct PragmaName {
	/** Name of pragma. */
	const char *const zName;
	/** Id of pragma. */
	u8 ePragTyp;
	/** Start of column names in pragCName[] */
	u8 iPragCName;
	/** Number of column names. */
	u8 nPragCName;
};

/**
 * The order of pragmas in this array is important: it has
 * to be sorted. For more info see pragma_locate function.
 */
static const struct PragmaName aPragmaName[] = {
	{"collation_list", PRAGMA_COLLATION_LIST, 36, 2},
	{"foreign_key_list", PRAGMA_FOREIGN_KEY_LIST, 40, 8},
	{"index_info", PRAGMA_INDEX_INFO, 18, 6},
	{"index_list", PRAGMA_INDEX_LIST, 30, 3},
	{"stats", PRAGMA_STATS, 10, 4},
	{"table_info", PRAGMA_TABLE_INFO, 0, 5},
};
