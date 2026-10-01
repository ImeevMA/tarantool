local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'parenthesized-sources'})
    g.server:start()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE t (a INT PRIMARY KEY, b INT);]])
        box.execute([[INSERT INTO t VALUES (1, 10), (2, 20);]])
        box.execute([[CREATE TABLE s (a INT PRIMARY KEY, x INT);]])
        box.execute([[INSERT INTO s VALUES (1, 100), (3, 300);]])
        box.execute([[CREATE TABLE r (y INT PRIMARY KEY);]])
        box.execute([[INSERT INTO r VALUES (7);]])
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

g.after_all(function()
    g.server:stop()
end)

--
-- The columns of a parenthesized list of sources are looked up by their names,
-- optionally qualified with the names of the sources in the list or with the
-- alias of the list.
--
g.test_column_lookup = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        check([[SELECT b, y FROM (t, r) AS q;]], {{10, 7}, {20, 7}})
        check([[SELECT t.b, r.y FROM (t, r) AS q;]], {{10, 7}, {20, 7}})
        check([[SELECT q.b, q.y FROM (t, r) AS q;]], {{10, 7}, {20, 7}})
        check([[SELECT b FROM (t, r) AS q WHERE t.a = 2;]], {{20}})
        check([[SELECT t.a, s.a FROM (t, s) AS q WHERE x = 300;]],
              {{1, 3}, {2, 3}})
        check([[SELECT a FROM (t, s) AS q;]], 'ambiguous column name: a')
        check([[SELECT q.a FROM (t, s) AS q;]], 'ambiguous column name: q.a')
        check([[SELECT "t.b" FROM (t, r) AS q;]],
              "Can't resolve field 't.b'")
        check([[SELECT u.b FROM (t AS u, r) AS q WHERE y = 7;]],
              {{10}, {20}})
        check([[SELECT t.b, x FROM ((t, r) AS q, s) AS z WHERE s.a = 1;]],
              {{10, 100}, {20, 100}})
        check([[SELECT b, y FROM s JOIN (t, r) ON s.a = t.a;]], {{10, 7}})
    end)
end

--
-- An asterisk is expanded into all columns of a parenthesized list of sources,
-- even if their names are the same, and `table.*` is expanded into the columns
-- of the source of the list with the given name or of the whole list.
--
g.test_asterisks = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        check([[SELECT * FROM (t, r) AS q;]], {{1, 10, 7}, {2, 20, 7}})
        check([[SELECT * FROM (t, s) AS q WHERE x = 100;]],
              {{1, 10, 1, 100}, {2, 20, 1, 100}})
        check([[SELECT q.* FROM (t, r) AS q;]], {{1, 10, 7}, {2, 20, 7}})
        check([[SELECT t.* FROM (t, r) AS q;]], {{1, 10}, {2, 20}})
        check([[SELECT * FROM (t JOIN s USING (a)) AS q;]], {{1, 10, 100}})
        check([[SELECT * FROM (t, r) AS q, (s, r) AS p WHERE t.a = s.a;]],
              {{1, 10, 7, 1, 100, 7}})
        -- The columns are not looked up in the enclosing SELECT.
        check([[SELECT x FROM s WHERE 7 IN (SELECT r.* FROM (t, r) AS q);]],
              {{100}, {300}})
    end)
end

--
-- With full column names, the columns of a parenthesized list of sources are
-- still looked up by the names of the sources in the list.
--
g.test_full_column_names = function()
    g.server:exec(function()
        local check = rawget(_G, 'check')
        box.execute([[SET SESSION "sql_full_column_names" = true;]])
        local res = box.execute([[SELECT * FROM (t, r) AS q WHERE t.a = 1;]])
        t.assert_equals(res.metadata, {
            {name = 'q.t.a', type = 'integer'},
            {name = 'q.t.b', type = 'integer'},
            {name = 'q.r.y', type = 'integer'},
        })
        t.assert_equals(res.rows, {{1, 10, 7}})
        check([[SELECT t.b FROM (t, r) AS q;]], {{10}, {20}})
        box.execute([[SET SESSION "sql_full_column_names" = false;]])
    end)
end
