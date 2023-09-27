## feature/sql

* Introduced `DROP CONSTRAINT` variation to drop field constraints (gh-9136).
* `DROP CONSTRAINT` is now prohibited if a given name matches more than one
  constraint.
