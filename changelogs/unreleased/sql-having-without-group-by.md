## bugfix/sql

* Fixed a crash when a SELECT without GROUP BY had a HAVING clause and a result
  set containing a non-aggregate expression without column references, such as
  a subquery.
