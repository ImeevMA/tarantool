/*
** Compile and run this standalone program in order to generate code that
** implements a function that will translate alphabetic identifiers into
** parser token codes.
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>

/*
** A header comment placed at the beginning of generated code.
*/
static const char zHdr[] =
  "/***** This file contains automatically generated code ******\n"
  "**\n"
  "** The code in this file has been automatically generated by\n"
  "**\n"
  "**   extra/mkkeywordhash.c\n"
  "**\n"
  "** The code in this file implements a function that determines whether\n"
  "** or not a given identifier is really an SQL keyword.  The same thing\n"
  "** might be implemented more directly using a hand-written hash table.\n"
  "** But by using this automatically generated code, the size of the code\n"
  "** is substantially reduced.  This is important for embedded applications\n"
  "** on platforms with limited memory.\n"
  "*/\n"
;

/*
** All the keywords of the SQL language are stored in a hash
** table composed of instances of the following structure.
*/
typedef struct Keyword Keyword;
struct Keyword {
  char *zName;         /* The keyword name */
  char *zTokenType;    /* Token value for this keyword */
  bool isReserved;     /* Is this word reserved by SQL standard */
  int id;              /* Unique ID for this record */
  int hash;            /* Hash on the keyword */
  int offset;          /* Offset to start of name string */
  int len;             /* Length of this keyword, not counting final \000 */
  int prefix;          /* Number of characters in prefix */
  int longestSuffix;   /* Longest suffix that is a prefix on another word */
  int iNext;           /* Index in aKeywordTable[] of next with same hash */
  int substrId;        /* Id to another keyword this keyword is embedded in */
  int substrOffset;    /* Offset into substrId for start of this keyword */
  char zOrigName[50];  /* Original keyword name before processing */
};

/*
** These are the keywords
*/
static Keyword aKeywordTable[] = {
  { "ABORT",                  "TK_ABORT",       false },
  { "ACTION",                 "TK_ACTION",      false },
  { "ADD",                    "TK_ADD",         false },
  { "AFTER",                  "TK_AFTER",       false },
  { "ALL",                    "TK_ALL",         true  },
  { "ALTER",                  "TK_ALTER",       true  },
  { "ANALYZE",                "TK_STANDARD",    true  },
  { "AND",                    "TK_AND",         true  },
  { "ARRAY",                  "TK_ARRAY",       true  },
  { "AS",                     "TK_AS",          true  },
  { "ASC",                    "TK_ASC",         true  },
  { "AUTOINCREMENT",          "TK_AUTOINCR",    false },
  { "BEFORE",                 "TK_BEFORE",      false },
  { "BEGIN",                  "TK_BEGIN",       true  },
  { "BETWEEN",                "TK_BETWEEN",     true  },
  { "BOOL",                   "TK_BOOLEAN",     true  },
  { "BOOLEAN",                "TK_BOOLEAN",     true  },
  { "BY",                     "TK_BY",          true  },
  { "CASCADE",                "TK_CASCADE",     false },
  { "CASE",                   "TK_CASE",        true  },
  { "CAST",                   "TK_CAST",        false },
  { "CHECK",                  "TK_CHECK",       true  },
  { "COLLATE",                "TK_COLLATE",     true  },
  { "COLUMN_REF",             "TK_COLUMN_REF",  true  },
  { "COLUMN",                 "TK_COLUMN",      true  },
  { "COMMIT",                 "TK_COMMIT",      true  },
  { "CONFLICT",               "TK_CONFLICT",    false },
  { "CONSTRAINT",             "TK_CONSTRAINT",  true  },
  { "CREATE",                 "TK_CREATE",      true  },
  { "CROSS",                  "TK_JOIN_KW",     true  },
  { "DEFAULT",                "TK_DEFAULT",     true  },
  { "DEFERRED",               "TK_DEFERRED",    false },
  { "DEFERRABLE",             "TK_STANDARD",    false },
  { "DELETE",                 "TK_DELETE",      true  },
  { "DISABLE",                "TK_DISABLE",     false },
  { "DESC",                   "TK_DESC",        true  },
  { "DISTINCT",               "TK_DISTINCT",    true  },
  { "DROP",                   "TK_DROP",        true  },
  { "END",                    "TK_END",         true  },
  { "ENGINE",                 "TK_ENGINE",      false },
  { "EACH",                   "TK_EACH",        true  },
  { "ELSE",                   "TK_ELSE",        true  },
  { "ESCAPE",                 "TK_ESCAPE",      true  },
  { "EXCEPT",                 "TK_EXCEPT",      true  },
  { "EXISTS",                 "TK_EXISTS",      true  },
  { "EXPLAIN",                "TK_EXPLAIN",     true  },
  { "FAIL",                   "TK_FAIL",        false },
  { "FALSE",                  "TK_FALSE",       true  },
  { "FOR",                    "TK_FOR",         true  },
  { "FOREIGN",                "TK_FOREIGN",     true  },
  { "FROM",                   "TK_FROM",        true  },
  { "FULL",                   "TK_STANDARD",    true  },
  { "GROUP",                  "TK_GROUP",       true  },
  { "HAVING",                 "TK_HAVING",      true  },
  { "IF",                     "TK_IF",          true  },
  { "IGNORE",                 "TK_IGNORE",      false },
  { "IMMEDIATE",              "TK_STANDARD",    true  },
  { "IN",                     "TK_IN",          true  },
  { "INDEX",                  "TK_INDEX",       true  },
  { "INDEXED",                "TK_INDEXED",     false },
  { "INITIALLY",              "TK_INITIALLY",   false },
  { "INNER",                  "TK_JOIN_KW",     true  },
  { "INSERT",                 "TK_INSERT",      true  },
  { "INSTEAD",                "TK_INSTEAD",     false },
  { "INTERSECT",              "TK_INTERSECT",   true  },
  { "INTO",                   "TK_INTO",        true  },
  { "IS",                     "TK_IS",          true  },
  { "JOIN",                   "TK_JOIN",        true  },
  { "KEY",                    "TK_KEY",         false },
  { "LEFT",                   "TK_JOIN_KW",     true  },
  { "LIKE",                   "TK_LIKE_KW",     true  },
  { "LIMIT",                  "TK_LIMIT",       false },
  { "MAP",                    "TK_MAP",         true  },
  { "MATCH",                  "TK_MATCH",       true  },
  { "NATURAL",                "TK_JOIN_KW",     true  },
  { "NO",                     "TK_NO",          false },
  { "NOT",                    "TK_NOT",         true  },
  { "NULL",                   "TK_NULL",        true  },
  { "NUMBER",                 "TK_NUMBER",      true  },
  { "OF",                     "TK_OF",          true  },
  { "OFFSET",                 "TK_OFFSET",      false },
  { "ON",                     "TK_ON",          true  },
  { "OR",                     "TK_OR",          true  },
  { "ORDER",                  "TK_ORDER",       true  },
  { "OUTER",                  "TK_JOIN_KW",     true  },
  { "PARTIAL",                "TK_STANDARD",    true  },
  { "PLAN",                   "TK_PLAN",        false },
  { "PRAGMA",                 "TK_PRAGMA",      true  },
  { "PRIMARY",                "TK_PRIMARY",     true  },
  { "QUERY",                  "TK_QUERY",       false },
  { "RAISE",                  "TK_RAISE",       false },
  { "RECURSIVE",              "TK_RECURSIVE",   true  },
  { "REFERENCES",             "TK_REFERENCES",  true  },
  { "REGEXP",                 "TK_LIKE_KW",     false },
  { "RELEASE",                "TK_RELEASE",     true  },
  { "RENAME",                 "TK_RENAME",      true  },
  { "REPLACE",                "TK_REPLACE",     false },
  { "RESTRICT",               "TK_RESTRICT",    false },
  { "RIGHT",                  "TK_JOIN_KW",     true  },
  { "ROLLBACK",               "TK_ROLLBACK",    true  },
  { "ROW",                    "TK_ROW",         true  },
  { "SAVEPOINT",              "TK_SAVEPOINT",   true  },
  { "SCALAR",                 "TK_SCALAR",      true  },
  { "SELECT",                 "TK_SELECT",      true  },
  { "SESSION",                "TK_SESSION",     false },
  { "SET",                    "TK_SET",         true  },
  { "SIMPLE",                 "TK_STANDARD",    true  },
  { "START",                  "TK_START",       true  },
  { "STRING",                 "TK_STRING_KW",   true  },
  { "TABLE",                  "TK_TABLE",       true  },
  { "THEN",                   "TK_THEN",        true  },
  { "TO",                     "TK_TO",          true  },
  { "TRANSACTION",            "TK_TRANSACTION", true  },
  { "TRIGGER",                "TK_TRIGGER",     true  },
  { "TRUE",                   "TK_TRUE",        true  },
  { "UNION",                  "TK_UNION",       true  },
  { "UNIQUE",                 "TK_UNIQUE",      true  },
  { "UNKNOWN",                "TK_NULL",        true  },
  { "UNSIGNED",               "TK_UNSIGNED",    true  },
  { "UPDATE",                 "TK_UPDATE",      true  },
  { "USING",                  "TK_USING",       true  },
  { "UUID" ,                  "TK_UUID" ,       false },
  { "VALUES",                 "TK_VALUES",      true  },
  { "VARBINARY",              "TK_VARBINARY",   true  },
  { "VIEW",                   "TK_VIEW",        true  },
  { "WILDCARD",               "TK_STANDARD",    true  },
  { "WITH",                   "TK_WITH",        true  },
  { "WHEN",                   "TK_WHEN",        true  },
  { "WHERE",                  "TK_WHERE",       true  },
  { "ANY",                    "TK_ANY",         true  },
  { "ASENSITIVE",             "TK_STANDARD",    true  },
  { "BLOB",                   "TK_STANDARD",    true  },
  { "CALL",                   "TK_STANDARD",    true  },
  { "CHAR",                   "TK_CHAR",        true  },
  { "CONDITION",              "TK_STANDARD",    true  },
  { "CONNECT",                "TK_STANDARD",    true  },
  { "CURRENT",                "TK_STANDARD",    true  },
  { "CURRENT_USER",           "TK_STANDARD",    true  },
  { "CURSOR",                 "TK_STANDARD",    true  },
  { "CURRENT_DATE",           "TK_STANDARD",    true  },
  { "CURRENT_TIME",           "TK_STANDARD",    true  },
  { "CURRENT_TIMESTAMP",      "TK_STANDARD",    true  },
  { "DATE",                   "TK_STANDARD",    true  },
  { "DATETIME",               "TK_DATETIME",    true  },
  { "DEC",                    "TK_DECIMAL",     true  },
  { "DECIMAL",                "TK_DECIMAL",     true  },
  { "DECLARE",                "TK_STANDARD",    true  },
  { "DENSE_RANK",             "TK_STANDARD",    true  },
  { "DESCRIBE",               "TK_STANDARD",    true  },
  { "DETERMINISTIC",          "TK_STANDARD",    true  },
  { "DOUBLE",                 "TK_DOUBLE",      true  },
  { "ELSEIF",                 "TK_STANDARD",    true  },
  { "ENABLE",                 "TK_ENABLE",      false },
  { "FETCH",                  "TK_STANDARD",    true  },
  { "FLOAT",                  "TK_STANDARD",    true  },
  { "FUNCTION",               "TK_FUNCTION_KW", true  },
  { "GET",                    "TK_STANDARD",    true  },
  { "GRANT",                  "TK_STANDARD",    true  },
  { "INT",                    "TK_INT",         true  },
  { "INTEGER",                "TK_INTEGER_KW",  true  },
  { "INOUT",                  "TK_STANDARD",    true  },
  { "INSENSITIVE",            "TK_STANDARD",    true  },
  { "ITERATE",                "TK_STANDARD",    true  },
  { "LEAVE",                  "TK_STANDARD",    true  },
  { "LOCALTIME",              "TK_STANDARD",    true  },
  { "LOCALTIMESTAMP",         "TK_STANDARD",    true  },
  { "LOOP",                   "TK_STANDARD",    true  },
  { "NUM",                    "TK_STANDARD",    true  },
  { "NUMERIC",                "TK_STANDARD",    true  },
  { "OUT",                    "TK_STANDARD",    true  },
  { "OVER",                   "TK_STANDARD",    true  },
  { "PARTITION",              "TK_STANDARD",    true  },
  { "PRECISION",              "TK_STANDARD",    true  },
  { "PROCEDURE",              "TK_STANDARD",    true  },
  { "RANGE",                  "TK_STANDARD",    true  },
  { "RANK",                   "TK_STANDARD",    true  },
  { "READS",                  "TK_STANDARD",    true  },
  { "REAL",                   "TK_STANDARD",    true  },
  { "REPEAT",                 "TK_STANDARD",    true  },
  { "RESIGNAL",               "TK_STANDARD",    true  },
  { "RETURN",                 "TK_STANDARD",    true  },
  { "REVOKE",                 "TK_STANDARD",    true  },
  { "ROWS",                   "TK_STANDARD",    true  },
  { "ROW_NUMBER",             "TK_STANDARD",    true  },
  { "SENSITIVE",              "TK_STANDARD",    true  },
  { "SIGNAL",                 "TK_STANDARD",    true  },
  { "SPECIFIC",               "TK_STANDARD",    true  },
  { "SYSTEM",                 "TK_STANDARD",    true  },
  { "SQL",                    "TK_STANDARD",    true  },
  { "USER",                   "TK_STANDARD",    true  },
  { "VARCHAR",                "TK_VARCHAR",     true  },
  { "WHENEVER",               "TK_STANDARD",    true  },
  { "WHILE",                  "TK_STANDARD",    true  },
  { "TEXT",                   "TK_TEXT",        true  },
  { "TRUNCATE",               "TK_TRUNCATE",    true  },
  { "TRIM",                   "TK_TRIM",        true  },
  { "LEADING",                "TK_LEADING",     true  },
  { "TRAILING",               "TK_TRAILING",    true  },
  { "BOTH",                   "TK_BOTH",        true  },
  { "INTERVAL",               "TK_INTERVAL",    true  },
  { "SEQSCAN",                "TK_SEQSCAN",     false },
  { "SHOW",                   "TK_SHOW",        false },
};

/* Number of keywords */
static int nKeyword = (sizeof(aKeywordTable)/sizeof(aKeywordTable[0]));

/* Map all alphabetic characters into lower-case for hashing.  This is
** only valid for alphabetics.  In particular it does not work for '_'
** and so the hash cannot be on a keyword position that might be an '_'.
*/
#define charMap(X)   (0x20|(X))

/*
** Comparision function for two Keyword records
*/
static int keywordCompare1(const void *a, const void *b){
  const Keyword *pA = (Keyword*)a;
  const Keyword *pB = (Keyword*)b;
  int n = pA->len - pB->len;
  if( n==0 ){
    n = strcmp(pA->zName, pB->zName);
  }
  assert( n!=0 );
  return n;
}
static int keywordCompare2(const void *a, const void *b){
  const Keyword *pA = (Keyword*)a;
  const Keyword *pB = (Keyword*)b;
  int n = pB->longestSuffix - pA->longestSuffix;
  if( n==0 ){
    n = strcmp(pA->zName, pB->zName);
  }
  assert( n!=0 );
  return n;
}
static int keywordCompare3(const void *a, const void *b){
  const Keyword *pA = (Keyword*)a;
  const Keyword *pB = (Keyword*)b;
  int n = pA->offset - pB->offset;
  if( n==0 ) n = pB->id - pA->id;
  assert( n!=0 );
  return n;
}

/*
** Return a KeywordTable entry with the given id
*/
static Keyword *findById(int id){
  int i;
  for(i=0; i<nKeyword; i++){
    if( aKeywordTable[i].id==id ) break;
  }
  return &aKeywordTable[i];
}

/*
** This routine does the work.  The generated code is printed on standard
** output.
*/
int main(int argc, char **argv){
  int i, j, k, h;
  int bestSize, bestCount;
  int count;
  int nChar;
  int totalLen = 0;
  int aHash[1000];  /* 1000 is much bigger than nKeyword */
  char zText[2000];

  /* Fill in the lengths of strings and hashes for all entries. */
  for(i=0; i<nKeyword; i++){
    Keyword *p = &aKeywordTable[i];
    p->len = (int)strlen(p->zName);
    assert( p->len<sizeof(p->zOrigName) );
    memcpy(p->zOrigName, p->zName, p->len+1);
    totalLen += p->len;
    p->hash = (charMap(p->zName[0])*4) ^
              (charMap(p->zName[p->len-1])*3) ^ (p->len*1);
    p->id = i+1;
  }

  /* Sort the table from shortest to longest keyword */
  qsort(aKeywordTable, nKeyword, sizeof(aKeywordTable[0]), keywordCompare1);

  /* Look for short keywords embedded in longer keywords */
  for(i=nKeyword-2; i>=0; i--){
    Keyword *p = &aKeywordTable[i];
    for(j=nKeyword-1; j>i && p->substrId==0; j--){
      Keyword *pOther = &aKeywordTable[j];
      if( pOther->substrId ) continue;
      if( pOther->len<=p->len ) continue;
      for(k=0; k<=pOther->len-p->len; k++){
        if( memcmp(p->zName, &pOther->zName[k], p->len)==0 ){
          p->substrId = pOther->id;
          p->substrOffset = k;
          break;
        }
      }
    }
  }

  /* Compute the longestSuffix value for every word */
  for(i=0; i<nKeyword; i++){
    Keyword *p = &aKeywordTable[i];
    if( p->substrId ) continue;
    for(j=0; j<nKeyword; j++){
      Keyword *pOther;
      if( j==i ) continue;
      pOther = &aKeywordTable[j];
      if( pOther->substrId ) continue;
      for(k=p->longestSuffix+1; k<p->len && k<pOther->len; k++){
        if( memcmp(&p->zName[p->len-k], pOther->zName, k)==0 ){
          p->longestSuffix = k;
        }
      }
    }
  }

  /* Sort the table into reverse order by length */
  qsort(aKeywordTable, nKeyword, sizeof(aKeywordTable[0]), keywordCompare2);

  /* Fill in the offset for all entries */
  nChar = 0;
  for(i=0; i<nKeyword; i++){
    Keyword *p = &aKeywordTable[i];
    if( p->offset>0 || p->substrId ) continue;
    p->offset = nChar;
    nChar += p->len;
    for(k=p->len-1; k>=1; k--){
      for(j=i+1; j<nKeyword; j++){
        Keyword *pOther = &aKeywordTable[j];
        if( pOther->offset>0 || pOther->substrId ) continue;
        if( pOther->len<=k ) continue;
        if( memcmp(&p->zName[p->len-k], pOther->zName, k)==0 ){
          p = pOther;
          p->offset = nChar - k;
          nChar = p->offset + p->len;
          p->zName += k;
          p->len -= k;
          p->prefix = k;
          j = i;
          k = p->len;
        }
      }
    }
  }
  for(i=0; i<nKeyword; i++){
    Keyword *p = &aKeywordTable[i];
    if( p->substrId ){
      p->offset = findById(p->substrId)->offset + p->substrOffset;
    }
  }

  /* Sort the table by offset */
  qsort(aKeywordTable, nKeyword, sizeof(aKeywordTable[0]), keywordCompare3);

  /* Figure out how big to make the hash table in order to minimize the
  ** number of collisions */
  bestSize = nKeyword;
  bestCount = nKeyword*nKeyword;
  for(i=nKeyword/2; i<=2*nKeyword; i++){
    for(j=0; j<i; j++) aHash[j] = 0;
    for(j=0; j<nKeyword; j++){
      h = aKeywordTable[j].hash % i;
      aHash[h] *= 2;
      aHash[h]++;
    }
    for(j=count=0; j<i; j++) count += aHash[j];
    if( count<bestCount ){
      bestCount = count;
      bestSize = i;
    }
  }

  /* Compute the hash */
  for(i=0; i<bestSize; i++) aHash[i] = 0;
  for(i=0; i<nKeyword; i++){
    h = aKeywordTable[i].hash % bestSize;
    aKeywordTable[i].iNext = aHash[h];
    aHash[h] = i+1;
  }

  /* Begin generating code */
  printf("%s", zHdr);
  printf("/* Hash score: %d */\n", bestCount);
  printf("static int keywordCode(const char *z, int n, int *pType, "
         "bool *pFlag){\n");
  printf("  /* zText[] encodes %d bytes of keywords in %d bytes */\n",
          totalLen + nKeyword, nChar+1 );
  for(i=j=k=0; i<nKeyword; i++){
    Keyword *p = &aKeywordTable[i];
    if( p->substrId ) continue;
    memcpy(&zText[k], p->zName, p->len);
    k += p->len;
    if( j+p->len>70 ){
      printf("%*s */\n", 74-j, "");
      j = 0;
    }
    if( j==0 ){
      printf("  /*   ");
      j = 8;
    }
    printf("%s", p->zName);
    j += p->len;
  }
  if( j>0 ){
    printf("%*s */\n", 74-j, "");
  }
  printf("  static const char zText[%d] = {\n", nChar);
  zText[nChar] = 0;
  for(i=j=0; i<k; i++){
    if( j==0 ){
      printf("    ");
    }
    if( zText[i]==0 ){
      printf("0");
    }else{
      printf("'%c',", zText[i]);
    }
    j += 4;
    if( j>68 ){
      printf("\n");
      j = 0;
    }
  }
  if( j>0 ) printf("\n");
  printf("  };\n");

  printf("  static const unsigned short aHash[%d] = {\n", bestSize);
  for(i=j=0; i<bestSize; i++){
    if( j==0 ) printf("    ");
    printf(" %3d,", aHash[i]);
    j++;
    if( j>12 ){
      printf("\n");
      j = 0;
    }
  }
  printf("%s  };\n", j==0 ? "" : "\n");

  printf("  static const unsigned short aNext[%d] = {\n", nKeyword);
  for(i=j=0; i<nKeyword; i++){
    if( j==0 ) printf("    ");
    printf(" %3d,", aKeywordTable[i].iNext);
    j++;
    if( j>12 ){
      printf("\n");
      j = 0;
    }
  }
  printf("%s  };\n", j==0 ? "" : "\n");

  printf("  static const unsigned char aLen[%d] = {\n", nKeyword);
  for(i=j=0; i<nKeyword; i++){
    if( j==0 ) printf("    ");
    printf(" %3d,", aKeywordTable[i].len+aKeywordTable[i].prefix);
    j++;
    if( j>12 ){
      printf("\n");
      j = 0;
    }
  }
  printf("%s  };\n", j==0 ? "" : "\n");

  printf("  static const unsigned short int aOffset[%d] = {\n", nKeyword);
  for(i=j=0; i<nKeyword; i++){
    if( j==0 ) printf("    ");
    printf(" %3d,", aKeywordTable[i].offset);
    j++;
    if( j>12 ){
      printf("\n");
      j = 0;
    }
  }
  printf("%s  };\n", j==0 ? "" : "\n");

  printf("  static const unsigned char aCode[%d] = {\n", nKeyword);
  for(i=j=0; i<nKeyword; i++){
    char *zToken = aKeywordTable[i].zTokenType;
    if( j==0 ) printf("    ");
    printf("%s,%*s", zToken, (int)(14-strlen(zToken)), "");
    j++;
    if( j>=5 ){
      printf("\n");
      j = 0;
    }
  }
  printf("%s  };\n", j==0 ? "" : "\n");

  printf("  static const bool aFlag[%d] = {\n", nKeyword);
  for(i=j=0; i<nKeyword; i++){
    bool isReserved = aKeywordTable[i].isReserved;
    const char *flag = (isReserved ? "true" : "false");
    if( j==0 ) printf("    ");
    printf("%s,%*s", flag, (int)(14-strlen(flag)), "");
    j++;
    if( j>=5 ){
      printf("\n");
      j = 0;
    }
  }
  printf("%s  };\n", j==0 ? "" : "\n");

  printf("  int i, j;\n");
  printf("  const char *zKW;\n");
  printf("  if( n>=2 ){\n");
  printf("    i = ((charMap(z[0])*4) ^ (charMap(z[n-1])*3) ^ n) %% %d;\n",
          bestSize);
  printf("    for(i=((int)aHash[i])-1; i>=0; i=((int)aNext[i])-1){\n");
  printf("      if( aLen[i]!=n ) continue;\n");
  printf("      j = 0;\n");
  printf("      zKW = &zText[aOffset[i]];\n");
  printf("      while( j<n && (z[j]&~0x20)==zKW[j] ){ j++; }\n");
  printf("      if( j<n ) continue;\n");
  printf("      *pType = aCode[i];\n");
  printf("      if (pFlag) {\n");
  printf("        *pFlag = aFlag[i];\n");
  printf("      }\n");
  printf("      break;\n");
  printf("    }\n");
  printf("  }\n");
  printf("  return n;\n");
  printf("}\n");
  printf("#define SQL_N_KEYWORD %d\n", nKeyword);
  return 0;
}
