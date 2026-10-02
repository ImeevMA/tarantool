local server = require('luatest.server')
local t = require('luatest')

local g = t.group("insert", {{engine = 'memtx'}, {engine = 'vinyl'}})

g.before_all(function(cg)
    cg.server = server:new({alias = 'master'})
    cg.server:start()
    cg.server:exec(function(engine)
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        local sql = [[SET SESSION "sql_default_engine" = '%s';]]
        box.execute(sql:format(engine))
    end, {cg.params.engine})
end)

g.after_all(function(cg)
    cg.server:drop()
end)

-- gh-3565: INSERT OR REPLACE causes assertion fault.
g.test_3565_insert_replace_assertion_fault = function(cg)
    cg.server:exec(function()
        box.execute("CREATE TABLE tj (s1 INT PRIMARY KEY, s2 INT);")
        box.execute("INSERT INTO tj VALUES (1, 2), (2, 3);")
        box.execute("CREATE UNIQUE INDEX i ON tj (s2);")
        box.execute("REPLACE INTO tj VALUES (1, 3);")

        local res = box.execute("SELECT * FROM tj;")
        t.assert_equals(res.rows, {{1, 3}})
        box.execute("INSERT INTO tj VALUES (2, 4), (3, 5);")
        box.execute("UPDATE OR REPLACE tj SET s2 = s2 + 1;")

        local exp = {
            {1, 4},
            {3, 6},
        }
        res = box.execute("SELECT * FROM tj;")
        t.assert_equals(res.rows, exp)

        box.execute("DROP TABLE tj;")
    end)
end

-- Several NOT NULL REPLACE constraints work.
--
g.test_several_not_null_replace_constraints_work = function(cg)
    cg.server:exec(function()
        local sql = "CREATE TABLE a (id INT PRIMARY KEY, a INT NOT NULL "..
              "ON CONFLICT REPLACE DEFAULT 1, b INT NOT NULL ON "..
              "CONFLICT REPLACE DEFAULT 2);"
        box.execute(sql)
        box.execute("INSERT INTO a VALUES(1, NULL, NULL);")
        box.execute("INSERT INTO a VALUES(2, NULL, NULL);")

        local exp = {
            {1, 1, 2},
            {2, 1, 2},
        }
        local res = box.execute("SELECT * FROM a;")
        t.assert_equals(res.rows, exp)
        box.execute("DROP TABLE a;")
    end)
end

--
-- Make sure the space and the column list of INSERT are checked before its
-- rows, and the number of values after them.
--
g.test_insert_check_order = function(cg)
    cg.server:exec(function()
        box.execute([[CREATE TABLE t (a INT PRIMARY KEY, b INT);]])

        local sql = [[INSERT INTO nosuch SELECT nosuch FROM nosuch2;]]
        local _, err = box.execute(sql)
        t.assert_equals(err.message, "Space 'nosuch' does not exist")

        sql = [[INSERT INTO t(x) SELECT nosuch FROM t;]]
        _, err = box.execute(sql)
        t.assert_equals(err.message,
                        "Field 'x' was not found in space 't' format")

        sql = [[INSERT INTO t(a, a) SELECT nosuch FROM t;]]
        _, err = box.execute(sql)
        t.assert_equals(err.message,
                        "table id list: duplicate column name a")

        sql = [[INSERT INTO t SELECT nosuch FROM t;]]
        _, err = box.execute(sql)
        t.assert_equals(err.message, "Can't resolve field 'nosuch'")

        sql = [[INSERT INTO t SELECT a FROM t;]]
        _, err = box.execute(sql)
        t.assert_equals(err.message,
                        "table t has 2 columns but 1 values were supplied")

        sql = [[INSERT INTO t(a) VALUES (1, 2), (3, 4);]]
        _, err = box.execute(sql)
        t.assert_equals(err.message, "2 values for 1 columns")

        sql = [[INSERT INTO t(a) DEFAULT VALUES;]]
        _, err = box.execute(sql)
        t.assert_equals(err.message, "0 values for 1 columns")

        box.execute([[CREATE VIEW v AS SELECT * FROM t;]])
        _, err = box.execute([[INSERT INTO v(x) SELECT nosuch;]])
        t.assert_equals(err.message, "Can't modify space 'v': space is a view")

        box.execute([[DROP VIEW v;]])
        box.execute([[DROP TABLE t;]])
    end)
end

--
-- Make sure aggregate functions are not allowed in the single row of VALUES of
-- INSERT.
--
g.test_insert_values_aggregate = function(cg)
    cg.server:exec(function()
        box.execute([[CREATE TABLE t (a INT PRIMARY KEY, b INT);]])

        local _, err = box.execute([[INSERT INTO t VALUES (COUNT(*), 1);]])
        t.assert_equals(err.message, "misuse of aggregate function COUNT()")

        local sql = [[INSERT INTO t VALUES ((SELECT COUNT(*) FROM t), 1);]]
        local res = box.execute(sql)
        t.assert_equals(res.row_count, 1)
        t.assert_equals(box.execute([[SELECT * FROM t;]]).rows, {{0, 1}})

        box.execute([[DROP TABLE t;]])
    end)
end

--
-- Make sure the transfer optimization is used for INSERT INTO ... SELECT * FROM
-- a single space only without a column list.
--
g.test_insert_xfer = function(cg)
    cg.server:exec(function()
        box.execute([[CREATE TABLE t (a INT PRIMARY KEY, b INT);]])
        box.execute([[CREATE TABLE s (a INT PRIMARY KEY, b INT);]])
        box.execute([[INSERT INTO t VALUES (1, 10), (2, 20);]])

        local function xfer_count(sql)
            local before = box.stat.sql().sql_xfer_count
            box.execute(sql)
            box.execute([[DELETE FROM s;]])
            return box.stat.sql().sql_xfer_count - before
        end
        t.assert_equals(xfer_count([[INSERT INTO s SELECT * FROM t;]]), 1)
        t.assert_equals(xfer_count([[INSERT INTO s(a, b)
                                     SELECT * FROM t;]]), 0)
        t.assert_equals(xfer_count([[INSERT INTO s SELECT a, b FROM t;]]), 0)
        t.assert_equals(xfer_count([[INSERT INTO s SELECT * FROM t
                                     WHERE a > 1;]]), 0)
        t.assert_equals(xfer_count([[WITH c AS (SELECT 1)
                                     INSERT INTO s SELECT * FROM t;]]), 0)
        t.assert_equals(xfer_count([[INSERT INTO s WITH c AS (SELECT 1)
                                     SELECT * FROM t;]]), 0)

        box.execute([[DROP TABLE s;]])
        box.execute([[DROP TABLE t;]])
    end)
end
