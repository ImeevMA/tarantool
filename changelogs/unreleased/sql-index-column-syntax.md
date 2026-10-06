## feature/sql

* Now only a column name with an optional `COLLATE` clause, sort order and
  `AUTOINCREMENT` is accepted as a part of an index or of a `UNIQUE` or
  `PRIMARY KEY` constraint. Other expressions are rejected with a syntax
  error instead of the "Expressions are prohibited in an index definition"
  error. A column name in parentheses and several `COLLATE` clauses are not
  accepted anymore.
