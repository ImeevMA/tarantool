local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'select-without-from'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

g.test_limit_offset = function()
    g.server:exec(function()
        local res = box.execute([[SELECT 1 LIMIT 1;]])
        t.assert_equals(res.rows, {{1}})
        res = box.execute([[SELECT 1 LIMIT 1 OFFSET 1;]])
        t.assert_equals(res.rows, {})
        res = box.execute([[SELECT 1 LIMIT 0, 1;]])
        t.assert_equals(res.rows, {{1}})
        res = box.execute([[SELECT 1 LIMIT (SELECT 1) OFFSET (SELECT 0);]])
        t.assert_equals(res.rows, {{1}})
        res = box.execute([[SELECT (SELECT 1 LIMIT 1 OFFSET 0);]])
        t.assert_equals(res.rows, {{1}})
        res = box.execute([[SELECT (SELECT 1 LIMIT 1 OFFSET 1);]])
        t.assert_equals(res.rows, {{box.NULL}})

        local exp_err = "Failed to execute SQL statement: Only positive " ..
                        "integers are allowed in the LIMIT clause"
        local _, err = box.execute([[SELECT 1 LIMIT -1;]])
        t.assert_equals(err.message, exp_err)
    end)
end

--
-- LIMIT and OFFSET cannot refer to the result columns or use aggregate
-- functions, even in a subquery in the column list, where aggregate functions
-- are allowed.
--
g.test_limit_offset_names_and_aggregates = function()
    g.server:exec(function()
        local _, err = box.execute([[SELECT 1 AS a LIMIT a;]])
        t.assert_equals(err.message, "Can't resolve field 'a'")
        _, err = box.execute([[SELECT 1 AS a LIMIT 1 OFFSET a;]])
        t.assert_equals(err.message, "Can't resolve field 'a'")

        local exp_err = "misuse of aggregate function COUNT()"
        _, err = box.execute([[SELECT 1 LIMIT COUNT(*);]])
        t.assert_equals(err.message, exp_err)
        _, err = box.execute([[SELECT 1 LIMIT 1 OFFSET COUNT(*);]])
        t.assert_equals(err.message, exp_err)
        _, err = box.execute([[SELECT (SELECT 1 LIMIT COUNT(*));]])
        t.assert_equals(err.message, exp_err)
        _, err = box.execute([[SELECT ABS((SELECT 1 LIMIT COUNT(*)));]])
        t.assert_equals(err.message, exp_err)
    end)
end

--
-- LIMIT and OFFSET are resolved before the columns, and LIMIT before OFFSET,
-- whatever order they are written in.
--
g.test_limit_offset_error_order = function()
    g.server:exec(function()
        local _, err = box.execute([[SELECT a LIMIT b OFFSET c;]])
        t.assert_equals(err.message, "Can't resolve field 'b'")
        _, err = box.execute([[SELECT a LIMIT b, c;]])
        t.assert_equals(err.message, "Can't resolve field 'c'")
        _, err = box.execute([[SELECT * LIMIT b;]])
        t.assert_equals(err.message, "Failed to expand '*' in SELECT " ..
                                     "statement without FROM clause")
    end)
end
