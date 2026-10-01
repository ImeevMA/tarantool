## bugfix/sql

* Fixed a bug where GROUP BY, HAVING, and DISTINCT of the rightmost part of
  a compound SELECT with a COLLATE clause in ORDER BY were applied to the
  whole compound SELECT.
