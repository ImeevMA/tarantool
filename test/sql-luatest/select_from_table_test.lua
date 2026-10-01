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
    end)
end

--
-- As in the legacy code, an aggregate function belongs to the nearest SELECT
-- whose sources its arguments refer to outside of their subqueries, directly
-- or through the aliases of the result columns, or to its own SELECT.
--
g.test_aggregate_owner = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        check([[SELECT (SELECT SUM(t.a)) FROM t;]], {{6}})
        check([[SELECT (SELECT (SELECT MAX(t.a))) FROM t;]], {{3}})
        check([[SELECT (SELECT COUNT(t.a) + COUNT(*)) FROM t;]], {{4}})
        check([[SELECT (SELECT SUM(x + t.a) FROM s) FROM t;]],
              {{302}, {304}, {306}})
        check([[SELECT (SELECT COUNT(t.a) + COUNT(s.x) FROM s) FROM t;]],
              {{5}})
        check([[SELECT (SELECT (SELECT SUM(s.x + t.a)) FROM s) FROM t;]],
              {{302}, {304}, {306}})
        check([[SELECT b, (SELECT SUM(t.a)) FROM t GROUP BY b
                ORDER BY b;]], {{10, 1}, {20, 2}, {30, 3}})
        check([[SELECT b FROM t GROUP BY b HAVING (SELECT SUM(t.a)) > 1;]],
              {{20}, {30}})
        check([[SELECT 1 FROM t HAVING (SELECT COUNT(t.a)) > 0;]], {{1}})
        check([[SELECT COUNT(*) FROM t ORDER BY (SELECT COUNT(t.a));]],
              {{3}})
        check([[SELECT a FROM t GROUP BY (SELECT COUNT(t.a));]],
              'aggregate functions are not allowed in the GROUP BY clause')
        check([[SELECT (SELECT 1 HAVING COUNT(t.a) > 0) FROM t;]],
              'Failed to execute SQL statement: HAVING argument must ' ..
              'appear in the GROUP BY clause or be used in an aggregate ' ..
              'function')
        check([[SELECT (SELECT 1 WHERE COUNT(t.a) > 0) FROM t;]],
              'misuse of aggregate function COUNT()')

        -- The legacy code reports these errors when generating the code.
        check([[SELECT a FROM t WHERE (SELECT COUNT(t.a)) > 0;]],
              'misuse of aggregate function COUNT()')
        check([[SELECT COUNT((SELECT COUNT(t.a))) FROM t;]],
              'misuse of aggregate function COUNT()')
        check([[SELECT a AS y FROM t WHERE (SELECT COUNT(y) FROM s) > 0;]],
              'misuse of aggregate function COUNT()')
        check([[SELECT (SELECT COUNT(t.a)) AS w FROM t WHERE w > 0;]],
              'misuse of aliased aggregate w')
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
-- With several sources, a name must match a column of one source only.
--
g.test_several_sources = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        check([[SELECT b, x FROM t, s WHERE t.a = s.a;]],
              {{10, 100}, {20, 200}})
        check([[SELECT t2.a, t.b FROM t, t AS t2 WHERE t.a + 1 = t2.a;]],
              {{2, 10}, {3, 20}})
        check([[SELECT COUNT(*) FROM t, s, lt;]], {{6}})
        check([[SELECT (SELECT COUNT(*) FROM s, lt WHERE s.a = t.a) FROM t;]],
              {{1}, {1}, {0}})
        check([[SELECT a FROM t, s;]], 'ambiguous column name: a')
        check([[SELECT t.a FROM t, t;]], 'ambiguous column name: t.a')
        check([[SELECT t.a FROM t, s ORDER BY a;]],
              'ambiguous column name: a')
        check([[SELECT 1 FROM t, s WHERE (SELECT a) = 1;]],
              'ambiguous column name: a')
        check([[SELECT nosuch.a FROM t, s;]],
              "Field 'a' was not found in space 'nosuch' format")
        check([[SELECT 1 FROM t, nosuch;]], "Space 'nosuch' does not exist")
    end)
end

--
-- As in the legacy code, the join types are checked, and the columns of the
-- right table of a LEFT JOIN can be NULL.
--
g.test_join_types = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        check([[SELECT t.a FROM t CROSS JOIN s WHERE t.a = s.a;]],
              {{1}, {2}})
        check([[SELECT t.a FROM t INNER JOIN s WHERE t.a = s.a;]],
              {{1}, {2}})
        check([[SELECT t.a, s.x FROM t LEFT OUTER JOIN s WHERE s.a = t.a;]],
              {{1, 100}, {2, 200}})
        check([[SELECT t.a, s.x FROM t LEFT JOIN s WHERE s.x IS NULL;]], {})
        local res = box.execute([[SELECT s.x FROM t LEFT JOIN s
                                  WHERE s.a = t.a;]])
        t.assert_equals(res.rows, {{100}, {200}})
        check([[SELECT 1 FROM t RIGHT JOIN s;]],
              'Tarantool does not support RIGHT and FULL OUTER JOINs')
        check([[SELECT 1 FROM t OUTER JOIN s;]],
              'Tarantool does not support RIGHT and FULL OUTER JOINs')
        check([[SELECT 1 FROM t INNER OUTER JOIN s;]],
              'JOIN cannot be both OUTER and INNER')
        check([[SELECT 1 FROM t RIGHT JOIN nosuch;]],
              'Tarantool does not support RIGHT and FULL OUTER JOINs')
    end)
end

--
-- With several sources, an asterisk is expanded into references qualified
-- with the exact name of the source, so they can be ambiguous.
--
g.test_asterisks_several_sources = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        local res = box.execute([[SELECT * FROM t, s WHERE t.a = s.a;]])
        t.assert_equals(res.metadata, {
            {name = 'a', type = 'integer'},
            {name = 'b', type = 'integer'},
            {name = 'a', type = 'integer'},
            {name = 'x', type = 'integer'},
        })
        t.assert_equals(res.rows, {{1, 10, 1, 100}, {2, 20, 2, 200}})
        check([[SELECT s.*, t.* FROM t, s WHERE t.a = s.a AND t.a = 1;]],
              {{1, 100, 1, 10}})
        check([[SELECT * FROM t, t AS t2 WHERE t.a = 1 AND t2.a = 2;]],
              {{1, 10, 2, 20}})
        check([[SELECT * FROM t, t;]], 'ambiguous column name: t.a')
        check([[SELECT * FROM t AS "UP", "UP";]],
              'ambiguous column name: UP.a')
        check([[SELECT x.* FROM t, s;]], "Space 'x' does not exist")
        -- Qualified references are not unqualified names.
        check([[SELECT * FROM t, s HAVING COUNT(*) > 0;]],
              {{3, 30, 2, 200}})
    end)
end

--
-- As in the legacy code, the ON clauses are a part of WHERE: they can refer to
-- all sources and to the aliases of the result columns, cannot use aggregate
-- functions, and the ON clause of a LEFT JOIN restricts the join.
--
g.test_on = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        check([[SELECT t.a, s.x FROM t JOIN s ON s.a = t.a;]],
              {{1, 100}, {2, 200}})
        check([[SELECT t.a, s.x FROM t LEFT JOIN s ON s.a = t.a;]],
              {{1, 100}, {2, 200}, {3, box.NULL}})
        check([[SELECT t.a, s.x FROM t LEFT JOIN s ON s.a = t.a
                WHERE s.x IS NULL;]], {{3, box.NULL}})
        check([[SELECT t.a, t.a < 2 AS c, s.x FROM t
                LEFT JOIN s ON s.a = t.a AND c;]],
              {{1, true, 100}, {2, false, box.NULL}, {3, false, box.NULL}})
        check([[SELECT t.a AS y FROM t JOIN s ON y = s.a;]], {{1}, {2}})
        check([[SELECT t.a FROM t JOIN s ON lt.a = 7 JOIN lt ON TRUE
                WHERE t.a = s.a;]], {{1}, {2}})
        check([[SELECT (SELECT x FROM s JOIN lt ON s.a = t.a) FROM t;]],
              {{100}, {200}, {box.NULL}})
        check([[SELECT t.a FROM t JOIN s ON a = 1;]],
              'ambiguous column name: a')
        check([[SELECT t.a FROM t JOIN s ON COUNT(*) > 0;]],
              'misuse of aggregate function COUNT()')
    end)
end

--
-- As in the legacy code, if WHERE or the ON clause of an inner join is FALSE,
-- the other clauses are dropped without being resolved, and the depth of the
-- expression tree is checked for WHERE joined with the ON clauses.
--
g.test_on_false_and_depth = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        check([[SELECT t.a FROM t JOIN s ON FALSE WHERE nosuch = 1;]], {})
        check([[SELECT t.a FROM t LEFT JOIN s ON nosuch = 1 WHERE FALSE;]],
              {})
        check([[SELECT t.a FROM t LEFT JOIN s ON FALSE WHERE t.a = 1;]],
              {{1}})
        check([[SELECT t.a FROM t LEFT JOIN s ON FALSE WHERE nosuch = 1;]],
              "Can't resolve field 'nosuch'")

        local on = '1' .. string.rep(' + 1', 198) .. ' > 0'
        check('SELECT t.a FROM t JOIN s ON ' .. on .. ' WHERE t.a = s.a;',
              'Number of nodes in expression tree 201 exceeds the limit ' ..
              '(200)')
        check('SELECT t.a FROM t JOIN s ON ' .. on .. ' JOIN lt ON TRUE;',
              'Number of nodes in expression tree 201 exceeds the limit ' ..
              '(200)')
        check('SELECT COUNT(*) FROM t JOIN s ON ' .. on .. ';', {{6}})
    end)
end

--
-- As in the legacy code, NATURAL and USING join the columns with the same
-- names, a name of a joined column refers to the left column, and `*` omits
-- the right columns.
--
g.test_natural_using = function()
    g.server:exec(function()
        box.execute([[CREATE TABLE nu (a INT PRIMARY KEY, b INT, n INT);]])
        box.execute([[INSERT INTO nu VALUES (1, 10, 5), (3, 31, 7);]])
        local check = rawget(_G, 'check')
        local res = box.execute([[SELECT * FROM t NATURAL JOIN nu;]])
        t.assert_equals(res.metadata, {
            {name = 'a', type = 'integer'},
            {name = 'b', type = 'integer'},
            {name = 'n', type = 'integer'},
        })
        t.assert_equals(res.rows, {{1, 10, 5}})
        check([[SELECT * FROM t NATURAL LEFT JOIN nu;]],
              {{1, 10, 5}, {2, 20, box.NULL}, {3, 30, box.NULL}})
        check([[SELECT a, x FROM t JOIN s USING (a);]],
              {{1, 100}, {2, 200}})
        check([[SELECT * FROM t JOIN nu USING (b);]], {{1, 10, 1, 5}})
        check([[SELECT * FROM t LEFT JOIN s USING (a) WHERE x IS NULL;]],
              {{3, 30, box.NULL}})
        check([[SELECT a FROM t JOIN s USING (a) JOIN nu USING (a);]],
              {{1}})
        check([[SELECT s.*, * FROM t NATURAL JOIN s;]],
              {{1, 100, 1, 10, 100}, {2, 200, 2, 20, 200}})
        check([[SELECT t.* FROM t JOIN s USING (a);]], {{1, 10}, {2, 20}})
        check([[SELECT a FROM t JOIN nu USING (b);]],
              'ambiguous column name: a')
        check([[SELECT * FROM t JOIN t USING (a);]],
              'ambiguous column name: t.b')
        check([[SELECT * FROM t JOIN s USING (x);]],
              'cannot join using column x - column not present in both ' ..
              'tables')
        check([[SELECT * FROM t NATURAL JOIN s ON TRUE;]],
              'a NATURAL join may not have an ON or USING clause')
        check([[SELECT * FROM t JOIN s ON TRUE USING (a);]],
              'cannot have both ON and USING clauses in the same join')
        -- The joined columns are a part of WHERE.
        check([[SELECT * FROM t NATURAL JOIN s WHERE nosuch AND FALSE;]], {})
        local where = '1' .. string.rep(' + 1', 198) .. ' > 0'
        check('SELECT 1 FROM t NATURAL JOIN s WHERE ' .. where .. ';',
              'Number of nodes in expression tree 201 exceeds the limit ' ..
              '(200)')
        box.execute([[DROP TABLE nu;]])
    end)
end

--
-- As in the legacy code, a view is compiled from its definition for every use.
-- Its columns are the fields of the view, and the result columns of the view
-- are named automatically before the result columns of the SELECT using it.
--
g.test_views = function()
    g.server:exec(function()
        box.execute([[CREATE VIEW vt AS SELECT a, b FROM t WHERE a > 1;]])
        box.execute([[CREATE VIEW v2 AS SELECT 1 + 1, a * 2 AS d FROM t;]])
        box.execute([[CREATE VIEW vagg AS SELECT b, COUNT(*) AS c FROM t
                      GROUP BY b;]])
        box.execute([[CREATE VIEW vc AS SELECT a FROM t UNION
                      SELECT x FROM s;]])
        local check = rawget(_G, 'check')
        local res = box.execute([[SELECT *, 1 + 1 FROM v2 WHERE d > 4;]])
        t.assert_equals(res.metadata, {
            {name = 'COLUMN_1', type = 'integer'},
            {name = 'd', type = 'integer'},
            {name = 'COLUMN_2', type = 'integer'},
        })
        t.assert_equals(res.rows, {{2, 6, 2}})
        check([[SELECT x.a FROM vt AS x;]], {{2}, {3}})
        check([[SELECT c FROM vagg WHERE b = 20;]], {{1}})
        check([[SELECT a FROM vc ORDER BY a DESC LIMIT 2;]], {{200}, {100}})
        check([[SELECT * FROM vt JOIN t USING (a);]],
              {{2, 20, 20}, {3, 30, 30}})
        check([[SELECT * FROM t NATURAL LEFT JOIN vt;]],
              {{1, 10}, {2, 20}, {3, 30}})
        check([[SELECT (SELECT b FROM vt WHERE vt.a = t.a) FROM t;]],
              {{box.NULL}, {20}, {30}})
        check([[SELECT COUNT(*) FROM vt AS p, vt AS q WHERE p.a = q.a;]],
              {{2}})
        check([[SELECT 3 IN vc, 4 IN vc;]], {{true, false}})
        check([[SELECT nosuch FROM vt;]], "Can't resolve field 'nosuch'")
        check([[SELECT * FROM vt, vt;]], 'ambiguous column name: vt.a')
        check([[SELECT a FROM vt INDEXED BY i;]],
              "No index 'i' is defined in space 'vt'")
        box.execute([[DROP VIEW vc;]])
        box.execute([[DROP VIEW vagg;]])
        box.execute([[DROP VIEW v2;]])
        box.execute([[DROP VIEW vt;]])
    end)
end

--
-- As in the legacy code, the fields of a subquery in FROM are named after the
-- result columns of the leftmost part of its SELECT: after their aliases or,
-- if there are none, after the column references as written. A name of
-- several fields refers to none of them. The result columns of the subquery
-- are named automatically before the result columns of the SELECT using it.
--
g.test_subqueries = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        local res = box.execute([[SELECT 1 + 1, * FROM (SELECT 2 + 2, 3);]])
        t.assert_equals(res.metadata, {
            {name = 'COLUMN_3', type = 'integer'},
            {name = 'COLUMN_1', type = 'integer'},
            {name = 'COLUMN_2', type = 'integer'},
        })
        t.assert_equals(res.rows, {{2, 4, 3}})
        check([[SELECT x.b FROM (SELECT b FROM t) AS x WHERE b > 10;]],
              {{20}, {30}})
        check([[SELECT * FROM (SELECT (t.a), b AS c FROM t) WHERE c = 20;]],
              {{2, 20}})
        check([[SELECT "a" FROM (SELECT a FROM t) WHERE a = 1;]], {{1}})
        check([[SELECT A FROM (SELECT a FROM t);]],
              "Can't resolve field 'A'")
        check([[SELECT a, A FROM (SELECT "a", "A" FROM q);]], {{2, 1}})
        check([[SELECT p FROM (SELECT 1 AS p UNION SELECT 2 AS r);]],
              {{1}, {2}})
        check([[SELECT * FROM (VALUES (1, 2), (3, 4)) WHERE COLUMN_4 = 4;]],
              {{3, 4}})
        check([[SELECT a FROM t INTERSECT VALUES (1), (5);]], {{1}})
        check([[SELECT * FROM (SELECT * FROM t) NATURAL JOIN s;]],
              {{1, 10, 100}, {2, 20, 200}})
        check([[SELECT * FROM s LEFT JOIN (SELECT a, b FROM t) AS x
                ON x.a = s.a + 1;]], {{1, 100, 2, 20}, {2, 200, 3, 30}})
        check([[SELECT * FROM (SELECT x FROM s), (SELECT b FROM t)
                WHERE x = 100 AND b = 30;]], {{100, 30}})
        check([[SELECT (SELECT y FROM (SELECT t.a + 1 AS y)) FROM t;]],
              {{2}, {3}, {4}})
        check([[SELECT * FROM (SELECT * FROM (SELECT b FROM t)) LIMIT 1;]],
              {{10}})
        check([[SELECT * FROM (SELECT 'a' COLLATE "unicode_ci" AS k)
                WHERE k = 'A';]], {{'a'}})
        check([[SELECT * FROM (SELECT a, a FROM t);]],
              'ambiguous column name: a')
        check([[SELECT x.a FROM (SELECT a, b AS a FROM t) AS x;]],
              'ambiguous column name: x.a')
        check([[SELECT b FROM (SELECT a, a FROM t), t WHERE b = 10;]],
              {{10}, {10}, {10}})
        check([[SELECT * FROM (SELECT nosuch FROM t);]],
              "Can't resolve field 'nosuch'")
    end)
end

--
-- As in the legacy code, a parenthesized list of sources is a subquery
-- selecting all columns of the sources, whose columns are looked up by their
-- spans `source.column` and, if no span matches a qualified name, by the fields
-- of the list. An asterisk refers to the columns of the list by position.
--
g.test_nested_sources = function()
    g.server:exec(function()
        box.execute([[CREATE TABLE r (y INT PRIMARY KEY);]])
        box.execute([[INSERT INTO r VALUES (7);]])
        local check = rawget(_G, 'check')
        check([[SELECT b, r.y FROM (t, r) AS x WHERE t.a = 1;]], {{10, 7}})
        check([[SELECT x.b FROM (t, r) AS x WHERE y = 7;]],
              {{10}, {20}, {30}})
        check([[SELECT a FROM (t, s) AS x;]], 'ambiguous column name: a')
        check([[SELECT x.a FROM (t, s) AS x;]],
              'ambiguous column name: x.a')
        check([[SELECT "t.b" FROM (t, r) AS x;]],
              "Can't resolve field 't.b'")
        check([[SELECT t.b, s.x FROM ((t, r) AS x, s) AS z
                WHERE s.a = t.a;]], {{10, 100}, {20, 200}})
        check([[SELECT * FROM (t, s) AS x WHERE s.a = 2 AND t.a = 2;]],
              {{2, 20, 2, 200}})
        check([[SELECT x.* FROM (t, r) AS x WHERE t.a = 3;]], {{3, 30, 7}})
        check([[SELECT r.* FROM (t, r) AS x WHERE t.a = 3;]], {{7}})
        check([[SELECT * FROM s NATURAL JOIN (t, r) AS x;]],
              {{1, 100, 10, 7}, {2, 200, 20, 7}})
        check([[SELECT a FROM s WHERE 7 IN (SELECT r.* FROM (t, r) AS x);]],
              {{1}, {2}})
        check([[SELECT * FROM (t, r) AS x HAVING COUNT(*) > 0;]],
              {{3, 30, 7}})
        box.execute([[SET SESSION "sql_full_column_names" = true;]])
        check([[SELECT t.b FROM (t, r) AS x WHERE t.a = 1;]], {{10}})
        box.execute([[SET SESSION "sql_full_column_names" = false;]])
        box.execute([[DROP TABLE r;]])
    end)
end

--
-- As in the legacy code, a compound SELECT, which is not UNION ALL, with
-- COLLATE in ORDER BY is a SELECT of all columns from the compound SELECT
-- without ORDER BY, LIMIT and OFFSET, which belong to the outer SELECT.
--
g.test_compound_order_by_collate = function()
    g.server:exec(function()
        box.execute([[CREATE TABLE cs (id INT PRIMARY KEY, s STRING);]])
        box.execute([[INSERT INTO cs VALUES (1, 'a'), (2, 'B'), (3, 'b');]])
        local check = rawget(_G, 'check')
        local coll = ' ORDER BY 1 COLLATE "unicode_ci"'
        check([[SELECT s FROM cs UNION SELECT 'C']] .. coll,
              {{'a'}, {'B'}, {'b'}, {'C'}})
        check([[SELECT s AS x FROM cs EXCEPT SELECT 'B' ORDER BY x
                COLLATE "unicode_ci" DESC LIMIT 1 OFFSET 0;]], {{'b'}})
        check([[SELECT s FROM cs UNION SELECT s FROM cs GROUP BY s
                HAVING COUNT(*) > 1]] .. coll, {{'a'}, {'B'}, {'b'}})
        check([[SELECT s FROM cs UNION SELECT MAX(s) FROM cs]] .. coll ..
              ' LIMIT 2', {{'a'}, {'B'}})
        check([[SELECT (SELECT s FROM cs UNION SELECT '0']] .. coll ..
              ' LIMIT 1);', {{'0'}})
        check([[SELECT s, s FROM cs UNION SELECT 'C', 'C']] .. coll,
              'ambiguous column name: s')
        box.execute([[DROP TABLE cs;]])
    end)
end

--
-- As in the legacy code, the SELECT of a table of a WITH clause is resolved
-- for every use, in the scope of the SELECT using it, so an unused table is
-- not resolved, and every use names its columns automatically. The tables are
-- looked up by their names as is, before the spaces, the innermost WITH clause
-- first. The SELECT of an unused table is still checked as the legacy code
-- converts it.
--
g.test_with = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        check([[WITH t AS (SELECT 5 AS z) SELECT * FROM t;]], {{5}})
        check([[WITH c AS (SELECT 5 AS z) SELECT * FROM C;]],
              "Space 'C' does not exist")
        check([[WITH c(x, y) AS (SELECT a, b FROM t) SELECT y FROM c
                WHERE x = 2;]], {{20}})
        check([[WITH c(x) AS (SELECT a, b FROM t) SELECT * FROM c;]],
              'table c has 2 values for 1 columns')
        local res = box.execute([[WITH c AS (SELECT 1 + 1, 2 + 2)
                                  SELECT 3 + 3, * FROM c, c AS d;]])
        t.assert_equals(res.metadata, {
            {name = 'COLUMN_5', type = 'integer'},
            {name = 'COLUMN_1', type = 'integer'},
            {name = 'COLUMN_2', type = 'integer'},
            {name = 'COLUMN_3', type = 'integer'},
            {name = 'COLUMN_4', type = 'integer'},
        })
        -- The parts of a UNION are named from left to right, unlike in a
        -- subquery.
        res = box.execute([[WITH c AS (SELECT 1 + 1 UNION SELECT 2 + 2)
                            SELECT * FROM c;]])
        t.assert_equals(res.metadata, {{name = 'COLUMN_1', type = 'integer'}})
        res = box.execute([[SELECT * FROM (SELECT 1 + 1 UNION
                            SELECT 2 + 2);]])
        t.assert_equals(res.metadata, {{name = 'COLUMN_2', type = 'integer'}})
        check([[WITH d AS (SELECT * FROM c), c AS (SELECT a FROM t
                WHERE a < 3) SELECT * FROM d;]], {{1}, {2}})
        check([[WITH c AS (SELECT * FROM d), d AS (SELECT * FROM c)
                SELECT * FROM c;]], 'circular reference: c')
        check([[WITH c AS (SELECT 5 AS z) SELECT * FROM (WITH c AS
                (SELECT 6 AS z) SELECT * FROM c), c;]], {{6, 5}})
        check([[WITH c AS (SELECT t.a + 1 AS z) SELECT (SELECT z FROM c)
                FROM t;]], {{2}, {3}, {4}})
        check([[WITH c AS (SELECT 1 AS z) SELECT a FROM t WHERE a IN c;]],
              {{1}})
        check([[WITH c AS (SELECT nosuch FROM t) SELECT 1;]], {{1}})
        check([[WITH c AS (SELECT 1 FROM t RIGHT JOIN t AS u ON TRUE)
                SELECT 1;]], 'Tarantool does not support RIGHT and FULL ' ..
              'OUTER JOINs')
        check([[WITH c AS (SELECT 1), c AS (SELECT 2) SELECT 1;]],
              'Ambiguous table name in WITH query: c')
        res = box.execute([[WITH c AS (SELECT ?) SELECT 1;]], {5})
        t.assert_equals(res.rows, {{1}})
        check([[WITH c AS (SELECT 1 AS z) SELECT * FROM c INDEXED BY i;]],
              "No index 'i' is defined in space 'c'")
    end)
end

--
-- As in the legacy code, a source of the rightmost part of the SELECT of a
-- table of a WITH clause joined by UNION or UNION ALL with the name of the
-- table, compared case-insensitively, refers to the table recursively and
-- shares its columns, named after the parts to the left. Any other reference
-- to the table from its SELECT is an error.
--
g.test_with_recursive = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        check([[WITH r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r
                WHERE n < 3) SELECT * FROM r;]], {{1}, {2}, {3}})
        check([[WITH r AS (SELECT 1 AS n UNION ALL SELECT q.n + 1 FROM R AS q
                WHERE q.n < 3) SELECT n FROM r;]], {{1}, {2}, {3}})
        check([[WITH r(n) AS (SELECT a FROM t UNION ALL SELECT n + 10 FROM r, s
                WHERE n < 15 AND s.a = 1) SELECT * FROM r;]],
              {{1}, {2}, {3}, {11}, {12}, {13}, {21}, {22}, {23}})
        check([[WITH r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r
                WHERE n < 5 ORDER BY 1 DESC LIMIT 2) SELECT * FROM r;]],
              {{1}, {2}})
        check([[SELECT (WITH r(n) AS (SELECT t.a UNION ALL SELECT n + 1 FROM r
                WHERE n < 3) SELECT SUM(n) FROM r) FROM t;]],
              {{6}, {5}, {3}})
        check([[WITH r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r, r AS q)
                SELECT * FROM r;]],
              'multiple references to recursive table: r')
        check([[WITH r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r
                WHERE n < (SELECT COUNT(*) FROM r)) SELECT * FROM r;]],
              'multiple recursive references: r')
        check([[WITH r(n) AS (SELECT n FROM r UNION ALL SELECT 1)
                SELECT * FROM r;]], 'circular reference: r')
        check([[WITH r(n, m) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r)
                SELECT * FROM r;]], 'table r has 1 values for 2 columns')
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
