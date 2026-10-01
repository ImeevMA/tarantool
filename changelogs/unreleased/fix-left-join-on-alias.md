## bugfix/sql

* Fixed a bug when a condition in the `ON` clause of a `LEFT JOIN` given by
  an alias of a result column filtered the result rows instead of restricting
  the join.
