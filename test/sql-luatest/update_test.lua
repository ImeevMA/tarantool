local server = require('luatest.server')
local t = require('luatest')

local g = t.group("update", {{engine = 'memtx'}, {engine = 'vinyl'}})

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

--
-- gh-2251: Make sure that the UPDATE operation affects all eligible rows.
--
g.test_2251_multiple_update = function(cg)
    cg.server:exec(function()
        box.execute([[CREATE TABLE t1(a INTEGER PRIMARY KEY,
                      b INT UNIQUE, e INT);]])
        box.execute("INSERT INTO t1 VALUES(1, 4, 6);")
        box.execute("INSERT INTO t1 VALUES(2, 5, 7);")

        local res = box.execute([[UPDATE t1 SET e = e + 1
                                  WHERE b IN (SELECT b FROM t1);]])
        t.assert_equals(res, {row_count = 2})

        res = box.execute("SELECT e FROM t1")
        t.assert_equals(res.rows, {{7}, {8}})

        box.execute([[CREATE TABLE t2(a INTEGER PRIMARY KEY, b INT UNIQUE,
                      c NUMBER, d NUMBER, e INT, UNIQUE(c, d));]])
        box.execute("INSERT INTO t2 VALUES(1, 2, 3, 4, 5);")
        box.execute("INSERT INTO t2 VALUES(2, 3, 4, 4, 6);")

        res = box.execute([[UPDATE t2 SET e = e + 1 WHERE b
                            IN (SELECT b FROM t2);]])
        t.assert_equals(res, {row_count = 2})

        res = box.execute("SELECT e FROM t2")
        t.assert_equals(res.rows, {{6}, {7}})

        box.execute("DROP TABLE t1")
        box.execute("DROP TABLE t2")
    end)
end

--
-- Make sure that the nested SELECT statement within the UPDATE statement
-- executes correctly.
--
g.test_update_with_nested_select = function(cg)
    cg.server:exec(function ()
        -- Create space.
        box.execute([[CREATE TABLE t1(a INTEGER PRIMARY KEY,
                                      b INT UNIQUE, e INT);]])

        -- Seed entries.
        box.execute("INSERT INTO t1 VALUES(1, 4, 6);")
        box.execute("INSERT INTO t1 VALUES(2, 5, 7);")

        -- Both entries must be updated.
        local sql = "UPDATE t1 SET e = e + 1 WHERE b IN (SELECT b FROM t1);"
        t.assert_equals(box.execute(sql), {row_count = 2})

        -- Check.
        t.assert_equals(box.execute("SELECT e FROM t1;").rows, {{7}, {8}})

        -- Cleanup.
        box.execute("DROP TABLE t1;")
    end)
end

-- gh-3566: UPDATE OR IGNORE causes deletion of old entry.
g.test_3566_update_ignore_delection_old_entry = function(cg)
    cg.server:exec(function()
        local sql = "CREATE TABLE tj (s0 INT PRIMARY KEY, "..
                    "s1 INT UNIQUE, s2 INT);"
        box.execute(sql)
        box.execute("INSERT INTO tj VALUES (1, 1, 2), (2, 2, 3);")
        box.execute("CREATE UNIQUE INDEX i ON tj (s2);")
        box.execute("UPDATE OR IGNORE tj SET s1 = s1 + 1;")

        local exp = {
            {1, 2},
            {3, 3},
        }
        local res = box.execute("SELECT s1, s2 FROM tj;")
        t.assert_equals(res.rows, exp)

        box.execute("UPDATE OR IGNORE tj SET s2 = s2 + 1;")

        exp = {
            {1, 2},
            {3, 4},
        }
        res = box.execute("SELECT s1, s2 FROM tj;")
        t.assert_equals(res.rows, exp)
        box.execute("DROP TABLE tj;")
    end)
end

--
-- Make sure the SET clause of UPDATE is checked as it is converted, then the
-- space, and then the values of the SET clause and their columns in turn
-- before the WHERE clause, all resolved in the scope of the space without
-- allowing aggregate functions.
--
g.test_update_resolve = function(cg)
    cg.server:exec(function()
        box.execute([[CREATE TABLE t (a INT PRIMARY KEY, b INT);]])
        box.execute([[CREATE VIEW v AS SELECT * FROM t;]])
        local function check(sql, exp)
            local _, err = box.execute(sql)
            t.assert_equals(err.message, exp, sql)
        end
        check([[UPDATE nosuch SET (a, b) = (1, 2, 3), "" = 1;]],
              "2 columns assigned 3 values")
        check([[UPDATE nosuch SET "" = 1, (a, b) = (1, 2, 3);]],
              "Invalid identifier '' (expected printable symbols only " ..
              "or it is too long)")
        check([[UPDATE nosuch SET nosuch = nosuch;]],
              "Space 'nosuch' does not exist")
        check([[UPDATE v SET nosuch = nosuch;]],
              "Can't modify space 'v': space is a view")
        check([[UPDATE t SET b = 1, nosuch = nosuch2 WHERE nosuch3 = 1;]],
              "Can't resolve field 'nosuch2'")
        check([[UPDATE t SET nosuch = 1 WHERE nosuch2 = 1;]],
              "Field 'nosuch' was not found in space 't' format")
        check([[UPDATE t SET (b, b) = (1, 2) WHERE nosuch = 1;]],
              "set id list: duplicate column name b")
        check([[UPDATE t SET b = 1 WHERE nosuch = 1;]],
              "Can't resolve field 'nosuch'")
        check([[UPDATE t SET b = COUNT(*);]],
              "misuse of aggregate function COUNT()")
        box.execute([[DROP VIEW v;]])
        box.execute([[DROP TABLE t;]])
    end)
end

--
-- Make sure a vector is assigned to the columns of the SET clause of UPDATE,
-- and a view with an INSTEAD OF UPDATE OF trigger is updated only if one of
-- its columns is assigned.
--
g.test_update_vector_and_view = function(cg)
    cg.server:exec(function()
        box.execute([[CREATE TABLE t (a INT PRIMARY KEY, b INT, c INT);]])
        box.execute([[INSERT INTO t VALUES (1, 10, 100), (2, 20, 200);]])
        box.execute([[CREATE TABLE s (a INT PRIMARY KEY, x INT);]])
        box.execute([[INSERT INTO s VALUES (2, 7);]])
        box.execute([[UPDATE t SET (b, c) = (SELECT a, x FROM s
                      WHERE s.a = t.a) WHERE a = 2;]])
        t.assert_equals(box.execute([[SELECT * FROM t;]]).rows,
                        {{1, 10, 100}, {2, 2, 7}})

        box.execute([[CREATE TABLE lg (b INT PRIMARY KEY);]])
        box.execute([[CREATE VIEW v AS SELECT a, b FROM t;]])
        box.execute([[CREATE TRIGGER v1 INSTEAD OF UPDATE OF b ON v
                      FOR EACH ROW BEGIN
                      INSERT INTO lg VALUES (new.b); END;]])
        local _, err = box.execute([[UPDATE v SET a = 5;]])
        t.assert_equals(err.message, "Can't modify space 'v': space is a view")
        box.execute([[UPDATE v SET b = b + 1 WHERE a IN (SELECT a FROM s);]])
        t.assert_equals(box.execute([[SELECT * FROM lg;]]).rows, {{3}})

        box.execute([[DROP VIEW v;]])
        box.execute([[DROP TABLE lg;]])
        box.execute([[DROP TABLE s;]])
        box.execute([[DROP TABLE t;]])
    end)
end

--
-- Make sure an error building the WHERE clause of UPDATE is reported.
--
g.test_update_where_error = function(cg)
    cg.server:exec(function()
        box.execute([[CREATE TABLE t (a INT PRIMARY KEY, b INT);]])
        box.execute([[SET SESSION "sql_seq_scan" = false;]])
        local _, err = box.execute([[WITH c AS (SELECT 5 AS y)
                                     UPDATE t SET b = 1
                                     WHERE a IN (SELECT y - 4 FROM c);]])
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        t.assert_equals(err.message, "Scanning is not allowed for 'c'")
        box.execute([[DROP TABLE t;]])
    end)
end
