## bugfix/sql

* Fixed an assertion failure on a CASE expression whose operand and WHEN
  clause have an unequal number of columns, for example
  `CASE (1, 2) WHEN (1, 2, 3) THEN 'a' END` (gh-XXXX).
