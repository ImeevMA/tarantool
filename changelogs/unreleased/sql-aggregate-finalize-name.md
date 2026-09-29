## bugfix/sql

* Fixed a bug where the finalizer of a user-defined aggregate function was
  ignored when the function was called with an unquoted name not written in
  upper case.
