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
        box.execute([[CREATE TABLE s (a INT PRIMARY KEY, x INT);]])
        box.execute([[INSERT INTO s VALUES (1, 100), (2, 200);]])
        box.execute([[CREATE TABLE q ("A" INT PRIMARY KEY, "a" INT);]])
        box.execute([[INSERT INTO q VALUES (1, 2);]])
        box.execute([[CREATE TABLE ty (a INT PRIMARY KEY, u UNSIGNED,
                                       n NUMBER, g ARRAY);]])
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
-- A column is looked up by its name as is and, if the name is not quoted, by
-- its name in upper case. A qualified column is looked up only in the source
-- with the given name, which is its alias, if any, or the name of its space.
--
g.test_column_lookup = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        check([[SELECT a, b FROM t WHERE a = 1;]], {{1, 10}})
        check([[SELECT t.a, "b" FROM t WHERE a = 1;]], {{1, 10}})
        check([[SELECT x.a FROM t AS x WHERE x.b > 10;]], {{2}, {3}})
        check([[SELECT a, "A" FROM q;]], {{2, 1}})
        check([[SELECT A FROM t;]], "Can't resolve field 'A'")
        check([[SELECT nosuch FROM t;]], "Can't resolve field 'nosuch'")
        check([[SELECT t.nosuch FROM t;]],
              "Field 'nosuch' was not found in space 't' format")
        check([[SELECT T.a FROM t;]],
              "Field 'a' was not found in space 'T' format")
        check([[SELECT t.a FROM t AS x;]],
              "Field 'a' was not found in space 't' format")
        check([[SELECT a FROM t LIMIT a;]], "Can't resolve field 'a'")
    end)
end

--
-- The type of a column reference is the exact type of the field.
--
g.test_column_types = function()
    g.server:exec(function()
        local res = box.execute([[SELECT a, u, n, g, u + 1,
                                  TYPEOF(n) FROM ty;]])
        t.assert_equals(res.metadata, {
            {name = 'a', type = 'integer'},
            {name = 'u', type = 'unsigned'},
            {name = 'n', type = 'number'},
            {name = 'g', type = 'array'},
            {name = 'COLUMN_1', type = 'integer'},
            {name = 'COLUMN_2', type = 'string'},
        })
        local check = rawget(_G, 'check')
        check([[SELECT a FROM ty ORDER BY g;]],
              'Type mismatch: can not convert array to comparable type')
    end)
end

--
-- In every scope, a name refers to a column of a source before an alias of a
-- result column. An ORDER BY term which is a name refers to an alias first.
--
g.test_columns_and_aliases = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        check([[SELECT a AS b FROM t WHERE b > 15;]], {{2}, {3}})
        check([[SELECT a AS z FROM t WHERE z > 1;]], {{2}, {3}})
        check([[SELECT b AS a FROM t GROUP BY a;]], {{10}, {20}, {30}})
        check([[SELECT b AS a FROM t ORDER BY a DESC;]], {{30}, {20}, {10}})
        check([[SELECT a AS y, b FROM t ORDER BY y + b DESC;]],
              {{3, 30}, {2, 20}, {1, 10}})
        check([[SELECT b AS a, (SELECT a) FROM t;]],
              {{10, 1}, {20, 2}, {30, 3}})
        check([[SELECT a + 1 AS y FROM t ORDER BY
                (SELECT y FROM s WHERE s.a = 1);]], {{2}, {3}, {4}})
        check([[SELECT a AS y, (SELECT y FROM s) FROM t;]],
              "Can't resolve field 'y'")
    end)
end

--
-- Subqueries can refer to the columns of the sources of the enclosing SELECTs.
--
g.test_correlated = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        check([[SELECT (SELECT x FROM s WHERE s.a = t.a) FROM t;]],
              {{100}, {200}, {box.NULL}})
        check([[SELECT (SELECT b FROM s WHERE a = t.a) FROM t;]],
              {{10}, {20}, {box.NULL}})
        check([[SELECT (SELECT t.a FROM s AS t WHERE t.a = 2) FROM t;]],
              {{2}, {2}, {2}})
        check([[SELECT a FROM t WHERE EXISTS
                (SELECT 1 FROM s WHERE s.a = t.a);]], {{1}, {2}})
        check([[SELECT a FROM t WHERE a IN (SELECT a FROM s);]], {{1}, {2}})
        check([[SELECT (SELECT COUNT(*) FROM s WHERE s.a <= t.a) FROM t;]],
              {{1}, {2}, {2}})
        -- An aggregate function of the enclosing SELECT.
        check([[SELECT (SELECT SUM(t.a)) FROM t;]], {{6}})
        check([[SELECT (SELECT SUM(x + t.a) FROM s) FROM t;]],
              {{302}, {304}, {306}})
    end)
end

g.test_aggregates = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        check([[SELECT SUM(a), MAX(b) FROM t;]], {{6, 30}})
        check([[SELECT b, COUNT(*) FROM t GROUP BY b HAVING b > 10;]],
              {{20, 1}, {30, 1}})
        check([[SELECT a FROM t GROUP BY a ORDER BY SUM(b) DESC;]],
              {{3}, {2}, {1}})
        check([[SELECT 1 FROM t HAVING SUM(a) > 0;]], {{1}})
        -- As in the legacy code, only unqualified names are checked.
        check([[SELECT 1 FROM t HAVING COUNT(*) > 0 AND t.a > 0;]], {{1}})
        local exp_err = 'Failed to execute SQL statement: HAVING argument ' ..
                        'must appear in the GROUP BY clause or be used in ' ..
                        'an aggregate function'
        check([[SELECT 1 FROM t HAVING COUNT(*) > 0 AND a > 0;]], exp_err)
        check([[SELECT a FROM t WHERE SUM(a) > 0;]],
              'misuse of aggregate function SUM()')
        check([[SELECT a FROM t GROUP BY SUM(a);]],
              'aggregate functions are not allowed in the GROUP BY clause')
    end)
end

--
-- An ORDER BY term of a compound SELECT is matched against the result columns
-- of its parts. As in the legacy code, once a term cannot be resolved in a
-- part, no term matches a column as an expression.
--
g.test_compound_order_by_columns = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        check([[SELECT x FROM s UNION SELECT a FROM t ORDER BY x DESC;]],
              {{200}, {100}, {3}, {2}, {1}})
        check([[SELECT a + 0 FROM t UNION SELECT x FROM s ORDER BY a + 0;]],
              {{1}, {2}, {3}, {100}, {200}})
        check([[SELECT a, b FROM t UNION SELECT a, x FROM s ORDER BY t.b
                LIMIT 2;]], {{1, 10}, {2, 20}})
        local exp_err = 'Error at ORDER BY in place 1: term does not ' ..
                        'match any column in the result set'
        check([[SELECT a FROM t UNION SELECT x FROM s ORDER BY x;]], exp_err)
        check([[SELECT a FROM t UNION SELECT x FROM s ORDER BY a + 0;]],
              exp_err)
        check([[SELECT a AS p FROM t UNION SELECT x AS q FROM s
                ORDER BY q;]], "Can't resolve field 'q'")
    end)
end

--
-- An asterisk is expanded into the columns of the sources it matches, named
-- after their fields. As in the legacy code, `table.*` matches only the source
-- with exactly the given name, and the last unmatched asterisk is reported.
--
g.test_asterisks = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        local res = box.execute([[SELECT * FROM t WHERE a = 1;]])
        t.assert_equals(res.metadata, {
            {name = 'a', type = 'integer'},
            {name = 'b', type = 'integer'},
        })
        t.assert_equals(res.rows, {{1, 10}})
        check([[SELECT t.*, a, * FROM t WHERE a = 2;]], {{2, 20, 2, 2, 20}})
        check([[SELECT x.* FROM t AS x WHERE x.a = 3;]], {{3, 30}})
        check([[SELECT "UP".* FROM up;]], {{8}})
        check([[SELECT * FROM t ORDER BY b DESC LIMIT 1;]], {{3, 30}})
        check([[SELECT * FROM t UNION SELECT * FROM s ORDER BY 2 LIMIT 2;]],
              {{1, 10}, {2, 20}})
        check([[SELECT (1, 100) IN (SELECT * FROM s);]], {{true}})
        check([[SELECT up.* FROM up;]], "Space 'up' does not exist")
        check([[SELECT t.* FROM t AS x;]], "Space 't' does not exist")
        check([[SELECT x.*, nosuch.* FROM t AS x;]],
              "Space 'nosuch' does not exist")
        check([[SELECT nosuch.*, nosuch2.* FROM t;]],
              "Space 'nosuch2' does not exist")
        check([[SELECT * FROM t UNION SELECT a FROM s;]],
              'SELECTs to the left and right of UNION do not have the same ' ..
              'number of result columns')

        -- An expanded column is an unqualified name, unless the column names
        -- are full.
        local exp_err = 'Failed to execute SQL statement: HAVING argument ' ..
                        'must appear in the GROUP BY clause or be used in ' ..
                        'an aggregate function'
        check([[SELECT * FROM t HAVING COUNT(*) > 0;]], exp_err)
        box.execute([[SET SESSION "sql_full_column_names" = true;]])
        res = box.execute([[SELECT * FROM t AS x WHERE a = 1;]])
        t.assert_equals(res.metadata, {
            {name = 'x.a', type = 'integer'},
            {name = 'x.b', type = 'integer'},
        })
        check([[SELECT * FROM t HAVING COUNT(*) > 0;]], {{3, 30}})
        box.execute([[SET SESSION "sql_full_column_names" = false;]])
    end)
end

--
-- The number of the result columns is checked after the asterisks are
-- expanded.
--
g.test_asterisks_limit = function()
    g.server:exec(function()
        local fields = {}
        for i = 1, 1999 do
            fields[i] = 'c' .. i .. ' INT'
        end
        box.execute('CREATE TABLE w (id INT PRIMARY KEY, ' ..
                    table.concat(fields, ', ') .. ');')
        local res, err = box.execute([[SELECT * FROM w;]])
        t.assert_equals(err, nil)
        t.assert_equals(#res.metadata, 2000)
        local check = rawget(_G, 'check')
        check([[SELECT *, 1 FROM w;]], 'The number of columns in result ' ..
              'set 2001 exceeds the limit (2000)')
        box.execute([[DROP TABLE w;]])
    end)
end

--
-- The index given by INDEXED BY is looked up by its name as is and, if the name
-- is not quoted, by its name in upper case.
--
g.test_indexed_by = function()
    g.server:exec(function()
        box.execute([[CREATE TABLE ib (a INT PRIMARY KEY, b INT, c INT);]])
        box.execute([[CREATE INDEX i1 ON ib (b);]])
        box.execute([[CREATE INDEX "I2" ON ib (c);]])
        box.execute([[INSERT INTO ib VALUES (1, 30, 5), (2, 20, 6);]])
        local check = rawget(_G, 'check')
        check([[SELECT a FROM ib INDEXED BY i1;]], {{2}, {1}})
        check([[SELECT a FROM ib INDEXED BY i2 WHERE c = 6;]], {{2}})
        check([[SELECT a FROM ib AS x INDEXED BY "i1" WHERE x.b < 25;]],
              {{2}})
        check([[SELECT a FROM ib NOT INDEXED WHERE b = 20;]], {{2}})
        check([[SELECT a FROM ib INDEXED BY I1;]],
              "No index 'I1' is defined in space 'ib'")
        check([[SELECT a FROM ib INDEXED BY "i2";]],
              "No index 'i2' is defined in space 'ib'")

        local res = box.execute([[EXPLAIN QUERY PLAN
                                  SELECT a FROM ib INDEXED BY i2 WHERE c > 5;]])
        t.assert_equals(res.rows, {{0, 0, 0, 'SEARCH TABLE ib USING ' ..
                                    'COVERING INDEX I2 (c>?) (~262144 rows)'}})
        -- As in the legacy code, NOT INDEXED disables only automatic
        -- indexes.
        res = box.execute([[EXPLAIN QUERY PLAN
                            SELECT a FROM ib NOT INDEXED WHERE b = 20;]])
        t.assert_equals(res.rows, {{0, 0, 0, 'SEARCH TABLE ib USING ' ..
                                    'COVERING INDEX i1 (b=?) (~10 rows)'}})
        box.execute([[DROP TABLE ib;]])
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
