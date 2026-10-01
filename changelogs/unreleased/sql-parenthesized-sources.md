## bugfix/sql

* Fixed the lookup of the columns of a parenthesized list of sources in the
  FROM clause, for example, `(t1, t2) AS x`: unqualified column names and
  asterisks over the list no longer fail or refer to the columns of the
  enclosing SELECT, `x.*` no longer fails, and the names qualified with the
  sources of the list are found when full column names are on.
