## feature/sql

* Now only `TRUE`, `FALSE` and a string literal are accepted as a value in
  the `SET SESSION` statement. Other values are rejected with a syntax error
  instead of an error during execution.
