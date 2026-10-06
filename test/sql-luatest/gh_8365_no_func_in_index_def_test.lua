local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'gh-8365'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

--
-- Make sure there is no assert() when a function is given instead of a column
-- name. Also, aggregate functions should throw the same syntax error as
-- non-aggregate functions.
--
g.test_no_func_in_idx_def = function()
    g.server:exec(function()
        local sql = "CREATE TABLE t(i INT PRIMARY KEY, UNIQUE(abs(i)));"
        local _, err = box.execute(sql)
        local res = "Syntax error at line 1 near '('"
        t.assert_equals(err.message, res)

        sql = "CREATE TABLE t(i INT PRIMARY KEY, UNIQUE(max(i)));"
        _, err = box.execute(sql)
        t.assert_equals(err.message, res)
    end)
end
