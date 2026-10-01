local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'select-from-table'})
    g.server:start()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE t (a INT PRIMARY KEY, b INT);]])
        box.execute([[INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);]])
        box.execute([[CREATE TABLE "lt" (a INT PRIMARY KEY);]])
        box.execute([[INSERT INTO "lt" VALUES (7);]])
        box.execute([[CREATE TABLE "UP" (a INT PRIMARY KEY);]])
        box.execute([[INSERT INTO "UP" VALUES (8);]])
        box.execute([[CREATE VIEW v AS SELECT 5;]])
        box.schema.space.create('NOPK', {format = {{'a', 'integer'}}})
        local s = box.schema.space.create('NOFMT')
        s:create_index('pk')
    end)
end)

g.after_all(function()
    g.server:stop()
end)

g.before_all(function()
    g.server:exec(function()
        rawset(_G, 'check', function(sql, exp)
            local res, err = box.execute(sql)
            if type(exp) == 'string' then
                t.assert_equals(res, nil, sql)
                t.assert_equals(err.message, exp, sql)
            else
                t.assert_equals(err, nil, sql)
                t.assert_equals(res.rows, exp, sql)
            end
        end)
    end)
end)

--
-- A table in FROM is looked up by its name as is and, if the name is not
-- quoted, by its name in upper case.
--
g.test_lookup = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        check([[SELECT 1 FROM t;]], {{1}, {1}, {1}})
        check([[SELECT 1 FROM "t";]], {{1}, {1}, {1}})
        check([[SELECT 1 FROM T;]], "Space 'T' does not exist")
        check([[SELECT 1 FROM "T";]], "Space 'T' does not exist")
        check([[SELECT 1 FROM up;]], {{1}})
        check([[SELECT 1 FROM UP;]], {{1}})
        check([[SELECT 1 FROM "up";]], "Space 'up' does not exist")
        check([[SELECT 1 FROM nosuch;]], "Space 'nosuch' does not exist")
        check([[SELECT 1 FROM nopk;]],
              'SQL does not support spaces without primary key')
        check([[SELECT 1 FROM nofmt;]],
              'SQL does not support space without format')
        check([[SELECT 1 FROM t UNION ALL SELECT 1 FROM nopk;]],
              'SQL does not support spaces without primary key')
        check([[SELECT 1 FROM v;]], {{1}})
    end)
end

--
-- The source is shown by the name of its space and by its alias.
--
g.test_name_and_alias = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        check([[EXPLAIN QUERY PLAN SELECT 1 FROM up AS x;]],
              {{0, 0, 0, 'SCAN TABLE UP AS x (~1048576 rows)'}})
        check([[EXPLAIN QUERY PLAN SELECT 1 FROM t y;]],
              {{0, 0, 0, 'SCAN TABLE t AS y (~1048576 rows)'}})
        box.execute([[SET SESSION "sql_seq_scan" = false;]])
        check([[SELECT 1 FROM lt;]], "Scanning is not allowed for 'lt'")
        check([[SELECT 1 FROM up;]], "Scanning is not allowed for 'UP'")
        check([[SELECT 1 FROM SEQSCAN lt;]], {{1}})
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
    end)
end

g.test_clauses = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        check([[SELECT 1 FROM t WHERE 1 > 2;]], {})
        check([[SELECT 1 AS x FROM t WHERE x = 1 LIMIT 2;]], {{1}, {1}})
        check([[SELECT 1 FROM t LIMIT 1 OFFSET 2;]], {{1}})
        check([[SELECT COUNT(*), SUM(2) FROM t;]], {{3, 6}})
        check([[SELECT 1 FROM t GROUP BY 1;]], {{1}})
        check([[SELECT 1 FROM t HAVING COUNT(*) > 2;]], {{1}})
        check([[SELECT 1 FROM t HAVING COUNT(*) > 3;]], {})
        check([[SELECT DISTINCT 1 AS x FROM t ORDER BY x;]], {{1}})
        check([[SELECT (SELECT COUNT(*) FROM t);]], {{3}})
        check([[SELECT 1 FROM t WHERE EXISTS (SELECT 1 FROM lt);]],
              {{1}, {1}, {1}})
        check([[SELECT 2 FROM lt WHERE 1 IN (SELECT 1 FROM t);]], {{2}})
        check([[SELECT 1 FROM t UNION SELECT 2 FROM lt ORDER BY 1 DESC;]],
              {{2}, {1}})
        check([[SELECT 1 FROM t INTERSECT SELECT 1;]], {{1}})
        check([[SELECT 1 FROM t UNION SELECT 1, 2 FROM t;]],
              'SELECTs to the left and right of UNION do not have the same ' ..
              'number of result columns')
    end)
end

--
-- A name which may refer to a column of a source is not confused with an alias
-- of a result column.
--
g.test_names = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        check([[SELECT 5 AS a FROM t WHERE a = 2;]], {{5}})
        check([[SELECT 5 AS a FROM t GROUP BY a;]], {{5}, {5}, {5}})
        check([[SELECT 5 AS a FROM t WHERE t.a = 2;]], {{5}})
        check([[SELECT 5 AS a FROM t WHERE (SELECT a) = 2;]], {{5}})
        check([[SELECT (SELECT b) FROM t WHERE a = 2;]], {{20}})
        check([[SELECT 1 FROM t WHERE nosuch = 1;]],
              "Can't resolve field 'nosuch'")
        check([[SELECT 1 FROM t UNION SELECT 2 FROM lt ORDER BY a;]],
              'Error at ORDER BY in place 1: term does not match any ' ..
              'column in the result set')
        check([[SELECT 7 IN lt, 8 NOT IN lt;]], {{true, true}})
        check([[SELECT * FROM lt;]], {{7}})
        check([[SELECT lt.* FROM lt;]], {{7}})
    end)
end

--
-- Sources are now resolved together with the expressions, so an unknown table
-- is reported before an error found later in the conversion of the statement.
--
g.test_error_order = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        check([[SELECT (SELECT 1 FROM nosuch), 99999999999999999999999;]],
              "Space 'nosuch' does not exist")
    end)
end
