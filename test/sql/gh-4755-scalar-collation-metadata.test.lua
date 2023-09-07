env = require('test_run')
test_run = env.new()

--
-- gh-4755: collation in metadata must be displayed for both
-- string and scalar field types.
--
test_run:cmd("setopt delimiter ';'");
box.execute([[SET SESSION "sql_full_metadata" = true;]]);
box.execute([[CREATE TABLE TEST (A SCALAR COLLATE "unicode_ci" PRIMARY KEY,
                                 B STRING COLLATE "unicode_ci");]]);
box.execute("SELECT * FROM SEQSCAN test;");

--
-- Cleanup.
--
box.execute([[SET SESSION "sql_full_metadata" = false;]]);
box.execute("DROP TABLE test;");
