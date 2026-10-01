## bugfix/sql

* Fixed a bug where a compound SELECT of a view or of a WITH clause table with
  a COLLATE clause in ORDER BY used the collation to compare the rows of the
  parts, so INTERSECT, EXCEPT, and UNION could return wrong rows.
