local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'compound-order-by-collate'})
    g.server:start()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE t (id INT PRIMARY KEY, s STRING, n INT);]])
        box.execute([[INSERT INTO t VALUES (1, 'a', 1), (2, 'B', 1),
                                           (3, 'b', 2), (4, 'C', 3);]])
    end)
end)

g.after_all(function()
    g.server:stop()
end)

--
-- A compound SELECT with COLLATE in ORDER BY is executed as a SELECT from a
-- subquery. Only ORDER BY, LIMIT and OFFSET belong to the whole compound
-- SELECT, so GROUP BY, HAVING and DISTINCT of its rightmost part must stay
-- in the subquery.
--
g.test_rightmost_part_clauses = function()
    g.server:exec(function()
        local function check(sql, exp)
            local res, err = box.execute(sql)
            t.assert_equals(err, nil, sql)
            t.assert_equals(res.rows, exp, sql)
        end
        local coll = ' ORDER BY 1 COLLATE "unicode_ci"'
        check([[SELECT s FROM t UNION SELECT s FROM t GROUP BY s
                HAVING COUNT(*) > 1]] .. coll, {{'a'}, {'B'}, {'b'}, {'C'}})
        check([[SELECT s FROM t UNION SELECT CAST(n AS STRING) FROM t
                GROUP BY n]] .. coll .. ' LIMIT 4',
              {{'1'}, {'2'}, {'3'}, {'a'}})
        check([[SELECT s FROM t WHERE n = 3 UNION SELECT s FROM t
                WHERE n = 3 UNION ALL SELECT DISTINCT s FROM t]] .. coll,
              {{'a'}, {'B'}, {'b'}, {'C'}, {'C'}})
        check([[SELECT s FROM t UNION SELECT s FROM t WHERE n > 1]] ..
              coll .. ' LIMIT 2 OFFSET 1', {{'B'}, {'b'}})
    end)
end
