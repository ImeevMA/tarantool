## bugfix/sql

* Fixed a bug where the expressions of a subquery were counted twice towards
  the expression tree depth limit, so a statement with a subquery could fail
  with the "Number of nodes in expression tree" error while none of its
  expressions exceeded the limit (gh-XXXX).
* Fixed a bug where a result column alias used in an expression was not
  counted towards the expression tree depth limit with the height of the
  expression it refers to, so the limit could be exceeded (gh-XXXX).
