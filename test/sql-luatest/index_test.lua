local server = require('luatest.server')
local t = require('luatest')

local g = t.group("index", {{engine = 'memtx'}, {engine = 'vinyl'}})

g.before_all(function(cg)
    cg.server = server:new({alias = 'master'})
    cg.server:start()
    cg.server:exec(function(engine)
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        local sql = [[SET SESSION "sql_default_engine" = '%s']]
        box.execute(sql:format(engine))
    end, {cg.params.engine})
end)

g.after_all(function(cg)
    cg.server:drop()
end)

g.test_insert_unique = function(cg)
    cg.server:exec(function()
        -- Create space.
        box.execute([[CREATE TABLE zoobar (c1 INT, c2 INT PRIMARY KEY,
                                           c3 TEXT, c4 INT);]])
        box.execute("CREATE UNIQUE INDEX zoobar2 ON zoobar(c1, c4);")

        -- Seed entry.
        box.execute("INSERT INTO zoobar VALUES (111, 222, 'c3', 444);")

        -- PK must be unique.
        local sql = "INSERT INTO zoobar VALUES (112, 222, 'c3', 444);"
        local _, err = box.execute(sql)
        local exp_err = 'Duplicate key exists in unique index ' ..
                        '"pk_unnamed_zoobar_1" in space "zoobar" ' ..
                        'with old tuple - [111, 222, "c3", 444] and ' ..
                        'new tuple - [112, 222, "c3", 444]'
        t.assert_equals(err.message, exp_err)

        -- Unique index must be respected.
        _, err = box.execute("INSERT INTO zoobar VALUES (111, 223, 'c3', 444);")
        exp_err = 'Duplicate key exists in unique index "zoobar2" in space ' ..
                  '"zoobar" with old tuple - [111, 222, "c3", 444] and new ' ..
                  'tuple - [111, 223, "c3", 444]'
        t.assert_equals(err.message, exp_err)

        -- Cleanup.
        box.execute("DROP INDEX zoobar2 ON zoobar;")
        box.execute("DROP TABLE zoobar;")
    end)
end

g.test_drop_index = function(cg)
    cg.server:exec(function()
        -- Create space.
        box.execute([[CREATE TABLE zzoobar (c1 NUMBER, c2 INT PRIMARY KEY,
                                            c3 TEXT, c4 NUMBER);]])

        box.execute("CREATE UNIQUE INDEX zoobar2 ON zzoobar(c1, c4);")
        box.execute("CREATE INDEX zoobar3 ON zzoobar(c3);")

        -- Dummy entry.
        box.execute("INSERT INTO zzoobar VALUES (111, 222, 'c3', 444);")

        box.execute("DROP INDEX zoobar2 ON zzoobar;")
        box.execute("DROP INDEX zoobar3 On zzoobar;")

        -- zoobar2 is dropped - should be OK.
        local sql = "INSERT INTO zzoobar VALUES (111, 223, 'c3', 444);"
        local res = box.execute(sql)
        t.assert_equals(res, {row_count = 1})

        -- zoobar2 was dropped. Re-creation should  be OK.
        res = box.execute("CREATE INDEX zoobar2 ON zzoobar(c3);")
        t.assert_equals(res, {row_count = 1})

        -- Cleanup.
        box.execute("DROP INDEX zoobar2 ON zzoobar;")
        box.execute("DROP TABLE zzoobar;")
    end)
end

g.test_message_func_indexes = function(cg)
    cg.server:exec(function()
        -- Creating tables.
        box.execute("CREATE TABLE t1(id INTEGER PRIMARY KEY, a INTEGER);")
        box.execute([[CREATE TABLE t2(object INTEGER PRIMARY KEY, price INTEGER,
                                      count INTEGER);]])

        -- Expressions that're supposed to create functional indexes
        -- should return certain message.
        local _, err = box.execute("CREATE INDEX i1 ON t1(a + 1);")
        local exp_err = "Expressions are prohibited in an index definition"
        t.assert_equals(err.message, exp_err)
        local res = box.execute("CREATE INDEX i2 ON t1(a);")
        t.assert_equals(res, {row_count = 1})
        _, err = box.execute("CREATE INDEX i3 ON t2(price + 100);")
        t.assert_equals(err.message, exp_err)
        res = box.execute("CREATE INDEX i4 ON t2(price);")
        t.assert_equals(res, {row_count = 1})
        _, err = box.execute("CREATE INDEX i5 ON t2(count + 1);")
        t.assert_equals(err.message, exp_err)
        _, err = box.execute("CREATE INDEX i6 ON t2(count * price);")
        t.assert_equals(err.message, exp_err)

        -- Cleaning up.
        box.execute("DROP TABLE t1;")
        box.execute("DROP TABLE t2;")
    end)
end

-- Check that original sql ON CONFLICT clause is really
-- disabled.
g.test_on_conflict = function(cg)
    cg.server:exec(function()
        local exp_err = "keyword 'ON' is reserved. "..
                        "Please use double quotes if 'ON' is an identifier."
        local sql = "CREATE TABLE t (id INTEGER PRIMARY KEY, "..
                    "v INTEGER UNIQUE ON CONFLICT ABORT);"
        local _, err = box.execute(sql)
        t.assert_str_contains(err.message, exp_err)

        sql = "CREATE TABLE q (id INTEGER PRIMARY KEY, "..
              "v INTEGER UNIQUE ON CONFLICT FAIL);"
        _, err = box.execute(sql)
        t.assert_str_contains(err.message, exp_err)

        sql = "CREATE TABLE p (id INTEGER PRIMARY KEY, "..
              "v INTEGER UNIQUE ON CONFLICT IGNORE);"
        _, err = box.execute(sql)
        t.assert_str_contains(err.message, exp_err)

        sql = "CREATE TABLE g (id INTEGER PRIMARY KEY, "..
              "v INTEGER UNIQUE ON CONFLICT REPLACE);"
        _, err = box.execute(sql)
        t.assert_str_contains(err.message, exp_err)

        sql = "CREATE TABLE e (id INTEGER PRIMARY KEY ON "..
              "CONFLICT REPLACE, v INTEGER);"
        _, err = box.execute(sql)
        t.assert_str_contains(err.message, exp_err)

        sql = "CREATE TABLE t1(a INT PRIMARY KEY ON CONFLICT REPLACE);"
        _, err = box.execute(sql)
        t.assert_str_contains(err.message, exp_err)

        sql = "CREATE TABLE t2(a INT PRIMARY KEY ON CONFLICT IGNORE);"
        _, err = box.execute(sql)
        t.assert_str_contains(err.message, exp_err)

        -- CHECK constraint is illegal with REPLACE option.
        sql = "CREATE TABLE t (id INTEGER PRIMARY KEY, "..
              "a INTEGER CHECK (a > 5) ON CONFLICT REPLACE);"
        _, err = box.execute(sql)
        t.assert_str_contains(err.message, exp_err)
    end)
end

--
-- Make sure the collations and the AUTOINCREMENT of the columns of CREATE
-- INDEX are checked before the space and the name of the index, and the
-- columns are resolved after them.
--
g.test_create_index_check_order = function(cg)
    cg.server:exec(function()
        local function check(sql, exp)
            local _, err = box.execute(sql)
            t.assert_equals(err.message, exp, sql)
        end
        box.execute([[CREATE TABLE t (a INT PRIMARY KEY, "B" STRING);]])
        box.execute([[CREATE VIEW v AS SELECT * FROM t;]])
        box.execute([[CREATE INDEX i ON t(a);]])
        box.schema.space.create('nofmt')

        check([[CREATE INDEX i ON nosuch(a COLLATE nosuch);]],
              "Collation 'nosuch' does not exist")
        check([[CREATE INDEX i ON nosuch(a COLLATE nosuch COLLATE "binary");]],
              "Collation 'nosuch' does not exist")
        check("CREATE INDEX i ON nosuch(a" ..
              string.rep(' COLLATE "binary"', 200) .. ");",
              "Number of nodes in expression tree 201 exceeds the limit (200)")
        -- Columns which are not names of fields are checked after the space.
        check([[CREATE INDEX i ON nosuch(a + 99999999999999999999);]],
              "Space 'nosuch' does not exist")
        check([[CREATE INDEX i1 ON t(a + 99999999999999999999);]],
              "Expressions are prohibited in an index definition")
        check([[CREATE INDEX i ON nosuch(nosuch);]],
              "Space 'nosuch' does not exist")
        check([[CREATE INDEX i ON v(nosuch);]], "Views can not be indexed")
        check([[CREATE INDEX i ON nofmt(nosuch);]],
              "SQL does not support space without format")
        check([[CREATE INDEX i ON t(nosuch);]],
              "Index 'i' already exists in space 't'")
        check([[CREATE INDEX "" ON t(nosuch);]],
              "Invalid identifier '' (expected printable symbols only " ..
              "or it is too long)")
        check([[CREATE INDEX i ON "_space"(nosuch);]],
              "Can't create or modify index 'i' in space '_space': " ..
              "can't create index on system space")
        check([[CREATE INDEX i1 ON t(a, nosuch, a + 1);]],
              "Can't resolve field 'nosuch'")
        check([[CREATE INDEX i1 ON t(a, t.a, nosuch);]],
              "Expressions are prohibited in an index definition")

        local res = box.execute([[CREATE INDEX IF NOT EXISTS i ON
                                  nosuch(nosuch);]])
        t.assert_equals(res, {row_count = 0})
        res = box.execute([[CREATE INDEX IF NOT EXISTS i ON
                            nosuch(a + 99999999999999999999);]])
        t.assert_equals(res, {row_count = 0})
        res = box.execute([[CREATE INDEX IF NOT EXISTS i ON t(nosuch);]])
        t.assert_equals(res, {row_count = 0})

        -- The number of the columns is checked after the first one.
        local cols = string.rep(', a', 2000)
        check("CREATE INDEX i1 ON t(nosuch" .. cols .. ");",
              "Can't resolve field 'nosuch'")
        check("CREATE INDEX i1 ON t(a" .. cols .. ");",
              "The number of columns in index 2001 exceeds the limit (2000)")

        -- The outermost collation is used, unquoted names have legacy form.
        box.execute([[CREATE INDEX i1 ON t((b) COLLATE "unicode_ci"
                                           COLLATE "unicode");]])
        local parts = box.space.t.index.i1.parts
        t.assert_equals(parts[1].fieldno, 2)
        t.assert_equals(parts[1].collation, 'unicode')

        box.space.nofmt:drop()
        box.execute([[DROP VIEW v;]])
        box.execute([[DROP TABLE t;]])
    end)
end
