local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'left-join-on-alias'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

--
-- An alias of a result column in the ON clause of a LEFT JOIN is a part of
-- the join restriction, the same as the expression it refers to.
--
g.test_left_join_on_alias = function()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE t (a INT PRIMARY KEY);]])
        box.execute([[INSERT INTO t VALUES (1), (2), (3);]])
        box.execute([[CREATE TABLE s (a INT PRIMARY KEY, x INT);]])
        box.execute([[INSERT INTO s VALUES (1, 100), (2, 200);]])

        local exp = {{1, true, 100}, {2, false, box.NULL},
                     {3, false, box.NULL}}
        local res = box.execute([[SELECT t.a, t.a < 2, s.x FROM t
                                  LEFT JOIN s ON s.a = t.a AND t.a < 2;]])
        t.assert_equals(res.rows, exp)
        res = box.execute([[SELECT t.a, t.a < 2 AS c, s.x FROM t
                            LEFT JOIN s ON s.a = t.a AND c;]])
        t.assert_equals(res.rows, exp)
        res = box.execute([[SELECT t.a, t.a < 2 AS c, s.x FROM t
                            LEFT JOIN s ON s.a = t.a AND COALESCE(c, FALSE);]])
        t.assert_equals(res.rows, exp)

        box.execute([[DROP TABLE s;]])
        box.execute([[DROP TABLE t;]])
    end)
end
