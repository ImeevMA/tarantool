## bugfix/sql

* Fixed a bug where an integer argument of the `TRIM()` function was taken
  as the trimming side, and a negative integer caused an assertion failure
  in a debug build (gh-13353).
