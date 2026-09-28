local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'gh-XXXX'})
    g.server:start()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
    end)
end)

g.after_all(function()
    g.server:stop()
end)

-- Make sure a CASE operand and a WHEN clause with an unequal number of columns
-- raise a proper error instead of triggering an assertion.
g.test_case_operand_vector_arity = function()
    g.server:exec(function()
        local msg = "Unequal number of entries in row expression: " ..
                    "left side has 2, but right side - 3"

        -- Resolved path: a FROM-less SELECT.
        local res, err = box.execute(
            [[SELECT CASE (1, 2) WHEN (1, 2, 3) THEN 'a' ELSE 'b' END]])
        t.assert_equals(res, nil)
        t.assert_equals(tostring(err), msg)

        -- The mismatch may be in a later WHEN clause.
        res, err = box.execute([[SELECT CASE (1, 2) WHEN (1, 2) THEN 'a' ]] ..
                               [[WHEN (1, 2, 3) THEN 'x' ELSE 'b' END]])
        t.assert_equals(res, nil)
        t.assert_equals(tostring(err), msg)

        -- Legacy path: the same expression over a table.
        box.execute([[CREATE TABLE t (i INT PRIMARY KEY);]])
        res, err = box.execute(
            [[SELECT CASE (1, 2) WHEN (1, 2, 3) THEN 'a' ELSE 'b' END FROM t]])
        t.assert_equals(res, nil)
        t.assert_equals(tostring(err), msg)
        box.execute([[DROP TABLE t;]])

        -- An equal number of columns still works.
        res = box.execute(
            [[SELECT CASE (1, 2) WHEN (1, 2) THEN 'a' ELSE 'b' END]])
        t.assert_equals(res.rows, {{'a'}})
    end)
end
