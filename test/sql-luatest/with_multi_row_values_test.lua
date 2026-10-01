local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'with-multi-row-values'})
    g.server:start()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
    end)
end)

g.after_all(function()
    g.server:stop()
end)

--
-- The tables of WITH are visible in the rows of a VALUES with several rows,
-- as they are in the rows of a VALUES with one row.
--
g.test_with_multi_row_values = function()
    g.server:exec(function()
        local function check(sql, exp)
            local res, err = box.execute(sql)
            t.assert_equals(err, nil, sql)
            t.assert_equals(res.rows, exp, sql)
        end
        check([[WITH c AS (SELECT 1 AS z)
                VALUES ((SELECT z FROM c)), ((SELECT z + 1 FROM c));]],
              {{1}, {2}})
        check([[VALUES ((WITH d AS (SELECT 3 AS z) SELECT z FROM d)), (4);]],
              {{3}, {4}})
    end)
end
