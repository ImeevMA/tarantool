## feature/sql

* Introduced a `DROP CONSTRAINT` variation to drop field constraints (gh-9112).
* `DROP CONSTRAINT` is now prohibited if a given name matches more than one
  constraint (gh-9112).
